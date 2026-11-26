// gw2-test: pure
// gw2-build: clang++ -std=c++20 -O1 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address -DGW2_TEST_REAL_ENV -DDXMT_PAGE_SIZE=4096 -include optional -Itests/gw2/shim -Isrc/dxmt -Isrc/util -Isrc/winemetal -Iinclude tests/gw2/rename_trim_test.cpp src/dxmt/dxmt_dynamic.cpp src/dxmt/dxmt_allocation.cpp src/dxmt/dxmt_gw2_map.cpp src/dxmt/dxmt_gw2_stats.cpp src/dxmt/dxmt_gw2_switches.cpp -o $OUT
// gw2-run: env DXMT_RENAME_TRIM=600 DXMT_STATS=0 "$OUT" on
// gw2-run: env -u DXMT_RENAME_TRIM DXMT_STATS=1 "$OUT" off
// gw2-run: env -u DXMT_RENAME_TRIM DXMT_STATS=0 "$OUT" none
//
// gw2 (#265): DXMT_RENAME_TRIM's sweep of the rename queues (src/dxmt/dxmt_dynamic.cpp), built
// natively with the real DynamicBuffer and DynamicLinearTexture. Their Buffer, Texture and allocations
// are fakes defined below (no Metal): an allocation records its birth and its free, so a free of one
// that isn't alive fails the test, and AddressSanitizer catches a use after free. A command queue is a
// gw2::RenameClock the test moves by hand. The process reads its switches once, so each mode is a run:
//
// - on (DXMT_RENAME_TRIM=600, DXMT_STATS=0: the sweep alone lists resources): a burst of renames on a
//   buffer and on a linear texture, the clock moved 700 ids past them, one walk: both queues are
//   empty, swept counts their allocations and bytes, and the current names stay. An allocation given
//   back exactly 600 ids before the clock stays and one 601 before goes; one whose will_free_at is
//   ahead of the clock stays. A resource last mapped through another queue is swept only when that
//   queue's clock passes its ids. Then a thread destroys 2,000 resources while walks run, and every
//   allocation they made is freed exactly once. First of all (#295), page growth of a buffer over 1 GiB
//   returns without growing, and a 1 KiB buffer's still grows.
// - off (DXMT_RENAME_TRIM unset, DXMT_STATS=1): the same burst and clock: the queue stays at its peak,
//   swept is 0, nothing is freed.
// - none (both off): nothing is listed, so a walk counts nothing.
//
// Run it from the checkout's root (or through tests/gw2/run.zsh).

#include "dxmt_dynamic.hpp"
#include "dxmt_gw2_map.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
int g_failures = 0;
#define CHECK(cond, ...)                                                                                              \
  do {                                                                                                                \
    if (!(cond)) {                                                                                                    \
      std::fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                                            \
      std::fprintf(stderr, __VA_ARGS__);                                                                              \
      std::fprintf(stderr, "\n");                                                                                     \
      g_failures++;                                                                                                   \
    }                                                                                                                 \
  } while (0)

// every fake allocation alive, and how many were made and freed
std::mutex g_mutex;
std::set<const void *> g_live;
uint64_t g_made = 0, g_freed = 0;

void
born(const void *allocation) {
  std::lock_guard lock(g_mutex);
  g_live.insert(allocation);
  g_made++;
}
void
died(const void *allocation) {
  std::lock_guard lock(g_mutex);
  if (!g_live.erase(allocation)) {
    std::fprintf(stderr, "FAIL: allocation %p freed twice, or never made\n", allocation);
    std::abort();
  }
  g_freed++;
}
bool
alive(const void *allocation) {
  std::lock_guard lock(g_mutex);
  return g_live.count(allocation);
}
uint64_t
freed() {
  std::lock_guard lock(g_mutex);
  return g_freed;
}
} // namespace

// --- the fakes: what dxmt_dynamic.cpp calls on a Buffer and a Texture, without Metal

// A fake allocation has no Metal object or view; the destructors that would release one only link here.
extern "C" void
NSObject_release(obj_handle_t) {
  std::fprintf(stderr, "FAIL: a fake allocation released a Metal object\n");
  std::abort();
}

namespace dxmt {
void
TextureView::decRef() {
  std::fprintf(stderr, "FAIL: a fake texture allocation had a view\n");
  std::abort();
}

BufferAllocation::BufferAllocation(
    WMT::Device, const WMTBufferInfo &info, Flags<BufferAllocationFlag> flags, uint32_t page_size
) :
    info_(info),
    flags_(flags) {
  info_.length = page_size;
  suballocation_size_ = page_size;
  mappedMemory_ = nullptr;
  gpuAddress_ = 0;
  born(this);
}
void
BufferAllocation::free() {
  died(this);
  delete this;
}
Rc<BufferAllocation>
Buffer::allocate(Flags<BufferAllocationFlag> flags, uint32_t page_size) {
  WMTBufferInfo info{};
  info.length = length_;
  return new BufferAllocation(device_, info, flags, page_size);
}
Rc<BufferAllocation>
Buffer::rename(Rc<BufferAllocation> &&allocation) {
  Rc<BufferAllocation> old = std::move(current_);
  current_ = std::move(allocation);
  return old;
}

TextureAllocation::TextureAllocation(
    Texture *descriptor, WMT::Reference<WMT::Texture> &&, const WMTTextureInfo &, Flags<TextureAllocationFlag> flags
) :
    descriptor(descriptor),
    mappedMemory(nullptr),
    gpuResourceID(0),
    machPort(0),
    flags_(flags) {
  born(this);
}
void
TextureAllocation::free() {
  died(this);
  delete this;
}
Texture::Texture(unsigned bytes_per_image, unsigned bytes_per_row, const WMTTextureInfo &info, WMT::Device device) :
    info_(info),
    bytes_per_image_(bytes_per_image),
    bytes_per_row_(bytes_per_row),
    device_(device) {}
Rc<TextureAllocation>
Texture::allocate(Flags<TextureAllocationFlag> flags) {
  return new TextureAllocation(this, WMT::Reference<WMT::Texture>(), info_, flags);
}
Rc<TextureAllocation>
Texture::rename(Rc<TextureAllocation> &&allocation) {
  Rc<TextureAllocation> old = std::move(current_);
  current_ = std::move(allocation);
  return old;
}
} // namespace dxmt

using namespace dxmt;

namespace {
constexpr uint32_t kPage = 4096;  // a buffer allocation's bytes (the fake's page)
constexpr uint32_t kImage = 1000; // a linear texture allocation's bytes

// a resource with its fake Buffer or Texture, which outlives it
struct TestBuffer {
  Buffer buffer{kPage, WMT::Device{}};
  Rc<DynamicBuffer> dynamic;
  TestBuffer() {
    buffer.rename(buffer.allocate({}));
    dynamic = new DynamicBuffer(&buffer, {});
  }
  // a rename in chunk `seq` through the queue of `clock`: the old name is queued with will_free_at = seq,
  // and the Buffer takes the new one, as the encode thread does (Buffer::rename)
  void
  rename(uint64_t seq, gw2::RenameClock *clock) {
    dynamic->updateImmediateName(seq, dynamic->allocate(0, clock), 0, false);
    buffer.rename(dynamic->immediateName());
  }
  uint64_t
  queued() {
    gw2::RenameQueues q;
    std::vector<Rc<BufferAllocation>> none;
    dynamic->gw2Walk(q, 0, none);
    return q.pages;
  }
};

struct TestTexture {
  Texture texture{kImage, 100, WMTTextureInfo{}, WMT::Device{}};
  Rc<DynamicLinearTexture> dynamic;
  TestTexture() {
    texture.rename(texture.allocate({}));
    dynamic = new DynamicLinearTexture(&texture, {});
  }
  void
  rename(uint64_t seq, gw2::RenameClock *clock) {
    dynamic->updateImmediateName(seq, dynamic->allocate(0, clock), false);
    texture.rename(dynamic->immediateName());
  }
  uint64_t
  queued() {
    gw2::RenameQueues q;
    std::vector<Rc<TextureAllocation>> none;
    dynamic->gw2Walk(q, 0, none);
    return q.pages;
  }
};

// One walk, then what it swept. The background thread may walk too; walks run one at a time and sweep
// by the same rule, so the totals are the same.
gw2::RenameQueues
walk() {
  gw2::walkRenameQueues();
  return gw2::renameQueues();
}

template <typename R>
void
burstSwept(const char *what, uint32_t bytes) {
  gw2::RenameClock clock;
  R r;
  for (uint64_t seq = 1; seq <= 20; seq++)
    r.rename(seq, &clock);
  const void *name = r.dynamic->immediateName().ptr();
  uint64_t freed_before = freed();
  gw2::renameQueues(); // zero the totals
  auto first = walk();
  CHECK(first.swept == 0 && r.queued() == 20, "%s: clock at 0: swept %llu, queued %llu", what,
        (unsigned long long)first.swept, (unsigned long long)r.queued());

  clock.coherent = 700; // 680 to 699 ids past the burst
  auto q = walk();
  CHECK(r.queued() == 0, "%s: the queue keeps %llu", what, (unsigned long long)r.queued());
  CHECK(q.swept == 20 && q.swept_bytes == 20ull * bytes, "%s: swept %llu (%llu bytes), not 20", what,
        (unsigned long long)q.swept, (unsigned long long)q.swept_bytes);
  CHECK(freed() - freed_before == 20, "%s: %llu freed, not 20", what, (unsigned long long)(freed() - freed_before));
  CHECK(r.dynamic->immediateName().ptr() == name && alive(name), "%s: the current name went", what);
}

template <typename R>
void
ageBoundaries(const char *what) {
  gw2::RenameClock clock;
  R kept;  // given back 601 and 600 ids before the clock: the first goes, the second stays
  R ahead; // given back after the clock's id: stays
  kept.rename(99, &clock);
  kept.rename(100, &clock);
  ahead.rename(800, &clock);
  gw2::renameQueues();
  clock.coherent = 700;
  auto q = walk();
  CHECK(q.swept == 1, "%s: swept %llu, not 1", what, (unsigned long long)q.swept);
  CHECK(kept.queued() == 1, "%s: 600 ids old: %llu queued, not 1", what, (unsigned long long)kept.queued());
  CHECK(ahead.queued() == 1, "%s: ahead of the clock: %llu queued, not 1", what, (unsigned long long)ahead.queued());
}

template <typename R>
void
otherQueue(const char *what) {
  gw2::RenameClock mine, other;
  R r;
  for (uint64_t seq = 1; seq <= 5; seq++)
    r.rename(seq, &mine);
  gw2::renameQueues();
  other.coherent = 10000; // another queue's ids mean nothing for this resource
  walk();
  CHECK(r.queued() == 5, "%s: swept by another queue's clock: %llu queued", what, (unsigned long long)r.queued());
  mine.coherent = 700;
  auto q = walk();
  CHECK(r.queued() == 0 && q.swept == 5, "%s: its own queue's clock: %llu queued, swept %llu", what,
        (unsigned long long)r.queued(), (unsigned long long)q.swept);
}

// A thread destroys resources while the main thread walks: every allocation is freed exactly once.
void
destroyDuringWalks() {
  gw2::RenameClock clock;
  clock.coherent = 1000; // everything queued below is sweepable
  uint64_t made_before, freed_before;
  {
    std::lock_guard lock(g_mutex);
    made_before = g_made;
    freed_before = g_freed;
  }
  std::vector<TestBuffer *> buffers;
  std::vector<TestTexture *> textures;
  for (int i = 0; i < 1000; i++) {
    buffers.push_back(new TestBuffer());
    textures.push_back(new TestTexture());
    for (uint64_t seq = 1; seq <= 10; seq++) {
      buffers.back()->rename(seq, &clock);
      textures.back()->rename(seq, &clock);
    }
  }
  std::atomic<bool> done = false;
  std::atomic<int> walks = 0;
  std::thread destroyer([&] {
    while (walks < 1) // start inside the walks
      std::this_thread::yield();
    std::mt19937 rng(265);
    std::shuffle(buffers.begin(), buffers.end(), rng);
    std::shuffle(textures.begin(), textures.end(), rng);
    for (size_t i = 0; i < buffers.size(); i++) {
      delete buffers[i]; // the DynamicBuffer goes, then its Buffer
      delete textures[i];
      if (i % 8 == 0)
        std::this_thread::yield();
    }
    done = true;
  });
  while (!done) { // a walk holds the list's lock most of the time; a pause lets the destroyer in
    gw2::walkRenameQueues();
    walks++;
    std::this_thread::sleep_for(std::chrono::microseconds(50));
  }
  destroyer.join();
  gw2::walkRenameQueues();
  gw2::renameQueues();
  std::lock_guard lock(g_mutex);
  uint64_t made = g_made - made_before, gone = g_freed - freed_before;
  // per resource: the Buffer's or Texture's first allocation, then 10 renames
  CHECK(made == 2000 * 11 && gone == made, "destroy during walks: %llu made, %llu freed", (unsigned long long)made,
        (unsigned long long)gone);
  std::printf("RESULT destroy during walks: %d walks, %llu allocations, each freed once\n", walks.load(),
              (unsigned long long)made);
}

// gw2 (#295): a second renameImmediate in one chunk enters page growth. For a buffer over 1 GiB the
// 32-bit doubling wrapped to 0 and never ended (alarm: a hang kills the test); it must return, unGrown.
// A 1 KiB buffer still grows.
void
growthBounded() {
  gw2::RenameClock clock;
  for (uint32_t length : {0x40000010u, 1024u}) {
    TestBuffer r;
    r.dynamic->enableGrowth(length);
    alarm(20);
    r.dynamic->renameImmediate(5, 0, &clock);
    uint32_t second = r.dynamic->renameImmediate(5, 0, &clock);
    alarm(0);
    bool grown = second & DynamicBuffer::RenameGrown;
    CHECK(grown == (length == 1024u), "growth of a %u-byte buffer: result %u", length, second);
  }
}

template <typename R>
void
peakKept(const char *what) {
  gw2::RenameClock clock;
  R r;
  for (uint64_t seq = 1; seq <= 20; seq++)
    r.rename(seq, &clock);
  uint64_t freed_before = freed();
  gw2::renameQueues();
  clock.coherent = 700;
  auto q = walk();
  CHECK(q.swept == 0 && r.queued() == 20 && freed() == freed_before, "%s: swept %llu, queued %llu", what,
        (unsigned long long)q.swept, (unsigned long long)r.queued());
  CHECK(q.pages == 20, "%s: MAP's queued is %llu, not 20", what, (unsigned long long)q.pages);
}

template <typename R>
void
notListed(const char *what) {
  gw2::RenameClock clock;
  R r;
  for (uint64_t seq = 1; seq <= 20; seq++)
    r.rename(seq, &clock);
  clock.coherent = 700;
  auto q = walk();
  CHECK(q.pages == 0 && q.swept == 0 && r.queued() == 20, "%s: listed: walk counted %llu, swept %llu", what,
        (unsigned long long)q.pages, (unsigned long long)q.swept);
}
} // namespace

int
main(int argc, char **argv) {
  std::string mode = argc > 1 ? argv[1] : "";
  if (mode == "on") {
    growthBounded();
    burstSwept<TestBuffer>("buffer", kPage);
    burstSwept<TestTexture>("linear texture", kImage);
    ageBoundaries<TestBuffer>("buffer");
    ageBoundaries<TestTexture>("linear texture");
    otherQueue<TestBuffer>("buffer");
    otherQueue<TestTexture>("linear texture");
    destroyDuringWalks();
  } else if (mode == "off") {
    peakKept<TestBuffer>("buffer");
    peakKept<TestTexture>("linear texture");
  } else if (mode == "none") {
    notListed<TestBuffer>("buffer");
    notListed<TestTexture>("linear texture");
  } else {
    std::fprintf(stderr, "usage: %s on|off|none\n", argv[0]);
    return 2;
  }
  if (g_failures) {
    std::fprintf(stderr, "%s: %d failures\n", mode.c_str(), g_failures);
    return 1;
  }
  std::printf("RESULT %s: ok\n", mode.c_str());
  return 0;
}
