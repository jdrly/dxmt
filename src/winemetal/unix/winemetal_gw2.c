/*
 * gw2 (#291): my fork's bridge calls that need nothing from winemetal_unix.c, unix side: the calling
 * thread's QoS class (#9) and Mach base priority (#151). Also the sparseBufferTier method that
 * _MTLCopyAllDevices adds to a buffer class without it (#341). Their numbers are in winemetal_calls.h, the
 * PE side is winemetal_gw2_thunks.c, the declarations are in winemetal_gw2.h. A file of its own
 * because upstream appends every new handler at the end of winemetal_unix.c.
 */
#include <dispatch/dispatch.h>
#include <libproc.h>
#include <mach/thread_act.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/proc_info.h>
#include <unistd.h>
#include <objc/runtime.h>
#import <Metal/Metal.h>
#define WINEMETAL_API
#include "../winemetal_thunks.h"
#include "winemetal_gw2_unix.h"

#define STATUS_SUCCESS 0

/* gw2 (#9): runs on the calling thread, so "self" is the thread that made the unix call. */
NTSTATUS
_WMTSetThreadQoSClassSelf(void *obj) {
  struct unixcall_wmt_set_thread_qos *params = obj;
  params->ret_old_qos_class = qos_class_self();
  params->ret = pthread_set_qos_class_self_np((qos_class_t)params->qos_class, params->relative_priority);
  params->ret_new_qos_class = qos_class_self();
  return STATUS_SUCCESS;
}

/* gw2 (#151): the calling thread's base priority, -1 if unreadable. */
static int32_t
thread_base_priority_self(void) {
  uint64_t tid;
  struct proc_threadinfo info;
  if (pthread_threadid_np(NULL, &tid) ||
      proc_pidinfo(getpid(), PROC_PIDTHREADID64INFO, tid, &info, sizeof(info)) != sizeof(info))
    return -1;
  return info.pth_priority;
}

/* gw2 (#151): runs on the calling thread. A thread with Mach policies (every Wine thread) has the
   base priority task priority + precedence importance, so the importance is the target less the
   task's priority (31 for a console program, 26 for GW2). The heat ntdll derives the task's
   priority from the thread's base priority less its importance, which a QoS override (a waiter
   promoting the thread) would inflate; the task's own priority can't be. */
NTSTATUS
_WMTSetThreadBasePrioritySelf(void *obj) {
  struct unixcall_wmt_set_thread_base_priority *params = obj;
  struct proc_taskinfo task;
  params->ret_old_base = thread_base_priority_self();
  if (proc_pidinfo(getpid(), PROC_PIDTASKINFO, 0, &task, sizeof(task)) != sizeof(task)) {
    params->ret = -1;
  } else {
    struct thread_precedence_policy policy = {.importance = params->base_priority - task.pti_priority};
    params->ret = thread_policy_set(
        pthread_mach_thread_np(pthread_self()), THREAD_PRECEDENCE_POLICY, (thread_policy_t)&policy,
        THREAD_PRECEDENCE_POLICY_COUNT
    );
  }
  params->ret_new_base = thread_base_priority_self();
  return STATUS_SUCCESS;
}

/* gw2 (#341): sparseBufferTier is a required MTLBuffer property since macOS 26
   (MTLBufferSparseTier, an NSInteger; MTLBufferSparseTierNone = 0 is a buffer that isn't sparse).
   Apple's Metal there may send it to any buffer. On an M2 Max with macOS 26.5.2 the driver's buffer
   class, AGXG14XFamilyBuffer, doesn't implement it, and GW2 ended on the unrecognized selector at
   start, on Metal 4 and on Metal 3. DXMT makes no sparse buffers, so the answer is always None.

   Adds sparseBufferTier, returning MTLBufferSparseTierNone (0), to `cls` if its instances don't
   answer it. True if it added the method. A class that answers it, its own or inherited, keeps its
   own. */
bool
wmt_add_sparse_buffer_tier(Class cls) {
  SEL sel = @selector(sparseBufferTier);
  if (!cls || class_respondsToSelector(cls, sel))
    return false;
  char types[16];
  snprintf(types, sizeof(types), "%s@:", @encode(NSInteger));
  /* never freed: the class keeps it for the process */
  IMP imp = imp_implementationWithBlock(^NSInteger(id self) {
    (void)self;
    return 0; /* MTLBufferSparseTierNone */
  });
  if (class_addMethod(cls, sel, imp, types))
    return true;
  imp_removeBlock(imp);
  return false;
}

static void
answer_for(id<MTLBuffer> buffer, id<MTLDevice> device, const char *kind) {
  if (!buffer)
    return;
  Class cls = object_getClass(buffer);
  if (wmt_add_sparse_buffer_tier(cls))
    fprintf(
        stderr,
        "winemetal: %s (a %s buffer of %s) doesn't answer sparseBufferTier, which Apple's Metal on macOS 26 "
        "sends to buffers; added it, returning MTLBufferSparseTierNone (gw2 #341)\n",
        class_getName(cls), kind, device.name.UTF8String
    );
  [buffer release];
}

/* gw2 (#341): one buffer of each kind DXMT makes (shared, private, managed, on CPU memory, placed in a
   heap), each class given sparseBufferTier if it lacks it */
static void
add_for_device(id<MTLDevice> device) {
  answer_for([device newBufferWithLength:16 options:MTLResourceStorageModeShared], device, "shared");
  answer_for([device newBufferWithLength:16 options:MTLResourceStorageModePrivate], device, "private");
  answer_for([device newBufferWithLength:16 options:MTLResourceStorageModeManaged], device, "managed");
  /* never freed: the driver may hold the buffer, and so the page, past its release */
  size_t page = (size_t)getpagesize();
  void *memory = NULL;
  if (!posix_memalign(&memory, page, page))
    answer_for(
        [device newBufferWithBytesNoCopy:memory length:page options:MTLResourceStorageModeShared deallocator:nil],
        device, "CPU memory"
    );
  MTLHeapDescriptor *desc = [[MTLHeapDescriptor alloc] init];
  desc.type = MTLHeapTypePlacement;
  desc.storageMode = MTLStorageModePrivate;
  desc.size = 1 << 16;
  id<MTLHeap> heap = [device newHeapWithDescriptor:desc];
  [desc release];
  if (heap) {
    answer_for([heap newBufferWithLength:16 options:MTLResourceStorageModePrivate offset:0], device, "heap-placed");
    [heap release];
  }
}

/* gw2 (#341): sparseBufferTier on every buffer class of `devices` (an NSArray of MTLDevice) that lacks
   it, with a line on stderr (wine.log) per class added. Once per process, from the first
   _MTLCopyAllDevices, before DXMT has a device to make a buffer on. Nothing before macOS 26, which
   has no such property. */
void
wmt_add_sparse_buffer_tiers(id devices) {
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    if (@available(macOS 26.0, *)) {
      /* the calling thread may have no pool, and freeing a buffer autoreleases its device (#269) */
      @autoreleasepool {
        for (id<MTLDevice> device in (NSArray *)devices)
          add_for_device(device);
      }
    }
  });
}
