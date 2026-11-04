// gw2 (#56): host-side test of the Subsample detector's DXBC scan (gw2::scanDxbc). Not part of the
// DLL build; run it on the Mac:
//
//   clang++ -std=c++20 -Itests/gw2/shim -Isrc/dxmt tests/gw2/subsample_dxbc_test.cpp \
//     src/dxmt/dxmt_gw2_subsample.cpp src/dxmt/dxmt_gw2_stats.cpp -o /tmp/subsample_dxbc_test \
//     && /tmp/subsample_dxbc_test [<dir of .dxbc files>]
//
// 1. Hand-built containers: a copy shader (one sample), a texel-load shader (ld), a customdata block
//    (its length in the next dword), an SHDR chunk among other chunks, and broken input (no magic,
//    truncated, a zero-length instruction) that must come back unparsed without reading past the end.
// 2. With a directory (GW2's shaders extracted from Gw2-64.exe, named by DXMT's SHA-1): every file
//    must parse; the pixel shaders are summarised, and the small sample-only ones (the kind of shader
//    a Subsample stretch would be) are listed.

#include "dxmt_gw2_subsample.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <vector>

using namespace dxmt::gw2;

static int failures = 0;
#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                  \
      failures++;                                                                                  \
    }                                                                                              \
  } while (0)

static void
put32(std::vector<uint8_t> &v, uint32_t x) {
  for (int i = 0; i < 4; i++)
    v.push_back(uint8_t(x >> (8 * i)));
}

static uint32_t
tok(uint32_t opcode, uint32_t len) {
  return opcode | (len << 24);
}

// a container of chunks: {fourcc, dwords}
static std::vector<uint8_t>
container(const std::vector<std::pair<const char *, std::vector<uint32_t>>> &chunks) {
  std::vector<uint8_t> v = {'D', 'X', 'B', 'C'};
  v.resize(20, 0); // checksum
  put32(v, 1);     // version
  size_t size_at = v.size();
  put32(v, 0);     // total size, below
  put32(v, uint32_t(chunks.size()));
  size_t offsets_at = v.size();
  for (size_t i = 0; i < chunks.size(); i++)
    put32(v, 0);
  for (size_t i = 0; i < chunks.size(); i++) {
    uint32_t off = uint32_t(v.size());
    std::memcpy(&v[offsets_at + i * 4], &off, 4);
    v.insert(v.end(), chunks[i].first, chunks[i].first + 4);
    put32(v, uint32_t(chunks[i].second.size() * 4));
    for (auto d : chunks[i].second)
      put32(v, d);
  }
  uint32_t total = uint32_t(v.size());
  std::memcpy(&v[size_at], &total, 4);
  return v;
}

// a program: version token (ps_5_0), length, then the instructions
static std::vector<uint32_t>
program(std::vector<uint32_t> body) {
  std::vector<uint32_t> p = {0x00000050u, uint32_t(body.size() + 2)};
  p.insert(p.end(), body.begin(), body.end());
  return p;
}

static std::vector<uint32_t>
filler(uint32_t opcode, uint32_t len) {
  std::vector<uint32_t> v = {tok(opcode, len)};
  v.resize(len, 0x00100000u); // operand tokens (contents don't matter to the scan)
  return v;
}

static std::vector<uint32_t>
cat(std::initializer_list<std::vector<uint32_t>> parts) {
  std::vector<uint32_t> v;
  for (auto &p : parts)
    v.insert(v.end(), p.begin(), p.end());
  return v;
}

static void
synthetic() {
  // dcl_globalFlags, dcl_sampler, dcl_resource_texture2d, dcl_input_ps, dcl_output, sample, ret
  auto copy = program(cat({filler(106, 1), filler(90, 3), filler(88, 4), filler(98, 3), filler(101, 3),
                           filler(69, 9), filler(62, 1)}));
  auto c = scanDxbc(container({{"RDEF", {1, 2, 3}}, {"ISGN", {4}}, {"SHEX", copy}}).data(),
                    container({{"RDEF", {1, 2, 3}}, {"ISGN", {4}}, {"SHEX", copy}}).size());
  CHECK(c.parsed && c.sample == 1 && c.load == 0 && c.resinfo == 0 && c.total == 7);

  auto load = program(cat({filler(88, 4), filler(45, 8), filler(61, 6), filler(46, 9), filler(62, 1)}));
  auto blob = container({{"SHDR", load}});
  c = scanDxbc(blob.data(), blob.size());
  CHECK(c.parsed && c.sample == 0 && c.load == 2 && c.resinfo == 1 && c.total == 5);

  // customdata (opcode 53): its length is the next dword (here 5: token, length, 3 data dwords)
  auto custom = program(cat({{tok(53, 0), 5, 0xdeadbeef, 0x2d00002d, 0x45000045}, filler(72, 10),
                             filler(109, 8), filler(62, 1)}));
  blob = container({{"SHEX", custom}});
  c = scanDxbc(blob.data(), blob.size());
  CHECK(c.parsed && c.sample == 2 && c.load == 0 && c.total == 4);

  // broken input
  c = scanDxbc(nullptr, 0);
  CHECK(!c.parsed);
  std::vector<uint8_t> junk(64, 0x41);
  c = scanDxbc(junk.data(), junk.size());
  CHECK(!c.parsed);
  blob = container({{"SHEX", copy}});
  c = scanDxbc(blob.data(), blob.size() - 12); // the chunk runs past the end
  CHECK(!c.parsed);
  auto zero = program({tok(54, 0), 0, 0});
  blob = container({{"SHEX", zero}});
  c = scanDxbc(blob.data(), blob.size());
  CHECK(!c.parsed);
  blob = container({{"RDEF", {1}}});
  c = scanDxbc(blob.data(), blob.size()); // no program chunk
  CHECK(!c.parsed);
}

static void
directory(const char *dir) {
  DIR *d = opendir(dir);
  if (!d) {
    std::printf("can't open %s\n", dir);
    failures++;
    return;
  }
  unsigned files = 0, unparsed = 0, ps = 0, ps_load = 0, ps_resinfo = 0;
  struct Small {
    std::string name;
    DxbcCounts c;
  };
  std::vector<Small> small;
  while (auto *e = readdir(d)) {
    std::string name = e->d_name;
    if (name.size() < 6 || name.substr(name.size() - 5) != ".dxbc")
      continue;
    std::string path = std::string(dir) + "/" + name;
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f)
      continue;
    std::vector<uint8_t> data;
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
      data.insert(data.end(), buf, buf + n);
    std::fclose(f);
    files++;
    auto c = scanDxbc(data.data(), data.size());
    if (!c.parsed) {
      unparsed++;
      std::printf("unparsed: %s\n", name.c_str());
      continue;
    }
    // the program type: the version token's high word (0 = pixel shader), found as the scan does
    uint32_t type = 0xffff;
    uint32_t chunks;
    std::memcpy(&chunks, &data[28], 4);
    for (uint32_t i = 0; i < chunks; i++) {
      uint32_t off;
      std::memcpy(&off, &data[32 + i * 4], 4);
      if (!std::memcmp(&data[off], "SHDR", 4) || !std::memcmp(&data[off], "SHEX", 4)) {
        uint32_t version;
        std::memcpy(&version, &data[off + 8], 4);
        type = version >> 16;
        break;
      }
    }
    if (type != 0)
      continue;
    ps++;
    ps_load += c.load ? 1 : 0;
    ps_resinfo += c.resinfo ? 1 : 0;
    if (c.sample >= 1 && c.sample <= 2 && !c.load && c.total <= 16)
      small.push_back({name.substr(0, name.size() - 5), c});
  }
  closedir(d);
  std::printf("%s: %u files, %u unparsed; %u pixel shaders, %u with ld/ld_ms, %u with resinfo\n", dir, files,
              unparsed, ps, ps_load, ps_resinfo);
  std::printf("small sample-only pixel shaders (1-2 samples, no ld, <= 16 instructions): %zu\n", small.size());
  for (auto &s : small)
    std::printf("  %s sample=%u resinfo=%u instr=%u\n", s.name.c_str(), s.c.sample, s.c.resinfo, s.c.total);
  CHECK(unparsed == 0);
}

int
main(int argc, char **argv) {
  synthetic();
  if (argc > 1)
    directory(argv[1]);
  std::printf(failures ? "FAILED: %d\n" : "OK\n", failures);
  return failures ? 1 : 0;
}
