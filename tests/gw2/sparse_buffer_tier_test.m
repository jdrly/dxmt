// gw2-test: metal
// gw2-build: for arch in x86_64 arm64; do clang -x objective-c -std=gnu17 -fblocks -fno-objc-arc -O1 -Wall -Wextra -arch $arch -mmacosx-version-min=26.0 -Isrc/winemetal -Isrc/winemetal/unix -Isrc/airconv src/winemetal/unix/winemetal_gw2.c tests/gw2/sparse_buffer_tier_test.m -framework Metal -framework Foundation -o ${OUT}_$arch; done
// gw2-run: ${OUT}_x86_64 && ${OUT}_arm64
//
// gw2 (#341): the sparseBufferTier method the bridge adds to a buffer class that lacks it
// (src/winemetal/unix/winemetal_gw2.c, called from _MTLCopyAllDevices). An M2 Max on macOS 26.5.2
// ended GW2 at start on "-[AGXG14XFamilyBuffer sparseBufferTier]: unrecognized selector", on Metal 4
// and on Metal 3. No Wine, no game. Built for x86_64 (Rosetta, as 0.1.x's winemetal.so runs) and
// arm64 (0.2's).
//
// What it checks:
// - stand-ins: a class without the method gets it, answers respondsToSelector: afterwards, and
//   returns 0 (MTLBufferSparseTierNone); a second add does nothing; a subclass of a class that has it
//   and a class with its own method (returning 1) are left alone and keep their answer;
// - the device: after wmt_add_sparse_buffer_tiers on MTLCopyAllDevices(), a buffer of each kind the
//   bridge looks at answers sparseBufferTier, and one whose class didn't answer before answers 0.
//   Which classes answered before is printed as a RESULT line, not checked: on macOS 27 they all do,
//   and that line is the evidence for a 26.x Mac. The value of a class's own method is Apple's, so
//   it isn't checked.
//   Without a Metal device this part is skipped.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "winemetal_gw2_unix.h"

static int failures;
#define CHECK(cond, ...)                                                                                               \
  do {                                                                                                                 \
    if (!(cond)) {                                                                                                     \
      printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                                                           \
      printf(__VA_ARGS__);                                                                                             \
      printf("\n");                                                                                                    \
      failures++;                                                                                                      \
    }                                                                                                                  \
  } while (0)

@interface StandInBuffer : NSObject
@end
@implementation StandInBuffer
@end

@interface StandInSparseBuffer : NSObject
@end
@implementation StandInSparseBuffer
- (NSInteger)sparseBufferTier {
  return 1; // MTLBufferSparseTier1
}
@end

@interface StandInSparseBufferChild : StandInSparseBuffer
@end
@implementation StandInSparseBufferChild
@end

static NSInteger
tier(id object) {
  return ((NSInteger(*)(id, SEL))objc_msgSend)(object, @selector(sparseBufferTier));
}

static void
standIns(void) {
  SEL sel = @selector(sparseBufferTier);
  StandInBuffer *plain = [[StandInBuffer alloc] init];
  CHECK(![plain respondsToSelector:sel], "the stand-in answers before the add");
  CHECK(wmt_add_sparse_buffer_tier([StandInBuffer class]), "nothing added to a class without the method");
  CHECK([plain respondsToSelector:sel], "the stand-in doesn't answer after the add");
  CHECK(tier(plain) == 0, "the added method returns %ld, not MTLBufferSparseTierNone", (long)tier(plain));
  CHECK(!wmt_add_sparse_buffer_tier([StandInBuffer class]), "a second add replaced the method");

  StandInSparseBuffer *sparse = [[StandInSparseBuffer alloc] init];
  CHECK(!wmt_add_sparse_buffer_tier([StandInSparseBuffer class]), "added to a class that has the method");
  CHECK(tier(sparse) == 1, "a class with its own method answers %ld, not its own 1", (long)tier(sparse));
  StandInSparseBufferChild *child = [[StandInSparseBufferChild alloc] init];
  CHECK(!wmt_add_sparse_buffer_tier([StandInSparseBufferChild class]), "added to a class that inherits the method");
  CHECK(tier(child) == 1, "a subclass answers %ld, not its parent's 1", (long)tier(child));
  CHECK(!wmt_add_sparse_buffer_tier(Nil), "added to Nil");
  [plain release];
  [sparse release];
  [child release];
}

// what the class of `buffer` answered before the bridge's add, as "<class>:<yes|no>"; the buffer is
// kept for the check after it, and so is its class if it didn't answer
static void
before(id<MTLBuffer> buffer, const char *kind, NSMutableArray *kept, NSMutableSet *added, NSMutableString *line) {
  if (!buffer) {
    [line appendFormat:@" %s=none", kind];
    return;
  }
  BOOL answered = [buffer respondsToSelector:@selector(sparseBufferTier)];
  [line appendFormat:@" %s=%s:%s", kind, class_getName(object_getClass(buffer)), answered ? "yes" : "no"];
  if (!answered)
    [added addObject:object_getClass(buffer)];
  [kept addObject:buffer];
  [buffer release];
}

static void
devices(void) {
  NSArray *all = MTLCopyAllDevices();
  if (!all.count) {
    printf("RESULT no Metal device: the device part skipped\n");
    [all release];
    return;
  }
  NSOperatingSystemVersion os = [[NSProcessInfo processInfo] operatingSystemVersion];
  NSMutableArray *kept = [NSMutableArray array];
  NSMutableSet *added = [NSMutableSet set];
  for (id<MTLDevice> device in all) {
    NSMutableString *line = [NSMutableString string];
    before([device newBufferWithLength:16 options:MTLResourceStorageModeShared], "shared", kept, added, line);
    before([device newBufferWithLength:16 options:MTLResourceStorageModePrivate], "private", kept, added, line);
    before([device newBufferWithLength:16 options:MTLResourceStorageModeManaged], "managed", kept, added, line);
    size_t page = (size_t)getpagesize();
    void *memory = NULL;
    if (!posix_memalign(&memory, page, page))
      before(
          [device newBufferWithBytesNoCopy:memory length:page options:MTLResourceStorageModeShared deallocator:nil],
          "cpu", kept, added, line
      );
    MTLHeapDescriptor *desc = [[MTLHeapDescriptor alloc] init];
    desc.type = MTLHeapTypePlacement;
    desc.storageMode = MTLStorageModePrivate;
    desc.size = 1 << 16;
    id<MTLHeap> heap = [device newHeapWithDescriptor:desc];
    [desc release];
    if (heap)
      before([heap newBufferWithLength:16 options:MTLResourceStorageModePrivate offset:0], "heap", kept, added, line);
    [kept addObject:heap ? heap : [NSNull null]];
    [heap release];
    printf("RESULT macOS %ld.%ld.%ld, %s, answered before:%s\n", (long)os.majorVersion, (long)os.minorVersion,
           (long)os.patchVersion, device.name.UTF8String, line.UTF8String);
  }

  wmt_add_sparse_buffer_tiers(all);
  for (id object in kept) {
    if (![object conformsToProtocol:@protocol(MTLBuffer)])
      continue;
    const char *name = class_getName(object_getClass(object));
    CHECK([object respondsToSelector:@selector(sparseBufferTier)], "%s doesn't answer after the add", name);
    if ([added containsObject:object_getClass(object)] && [object respondsToSelector:@selector(sparseBufferTier)])
      CHECK(tier(object) == 0, "%s answers %ld, not MTLBufferSparseTierNone", name, (long)tier(object));
  }
  [all release];
}

int
main(void) {
  @autoreleasepool {
    standIns();
    devices();
  }
  if (failures) {
    printf("sparse_buffer_tier_test: %d failure(s)\n", failures);
    return 1;
  }
  printf("sparse_buffer_tier_test: ok\n");
  return 0;
}
