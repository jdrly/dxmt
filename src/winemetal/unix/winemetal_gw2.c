/*
 * gw2 (#291): my fork's bridge calls that need nothing from winemetal_unix.c, unix side: the calling
 * thread's QoS class (#9) and Mach base priority (#151). Their numbers are in winemetal_calls.h, the
 * PE side is winemetal_gw2_thunks.c, the declarations are in winemetal_gw2.h. A file of its own
 * because upstream appends every new handler at the end of winemetal_unix.c.
 */
#include <libproc.h>
#include <mach/thread_act.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <stdbool.h>
#include <sys/proc_info.h>
#include <unistd.h>
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
