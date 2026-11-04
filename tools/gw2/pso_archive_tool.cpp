// gw2 (#111): the native PSO archive tool. Builds the pipelines W1 recorded (#57, recipes_1.db) and
// publishes them as a generation of the PSO archive store (#75's publisher), after the game exits, so
// the next session with DXMT_PSO_ARCHIVE=1 DXMT_PSO_GENERATIONS=1 finds them in its lookup archives.
// A macOS executable: no Wine, no game, no window.
//
// It shares the game's code instead of copying it (council/2026-11-04-perf-shaders/final.md, "Durable
// binary archives and the native tool"; council/2026-11-04-arch-grill/final.md, 6-9 table, row 8):
// - the recipes are read through the bridge's CacheEnumerate (the warm thread's read) and planned by
//   W2/W3's warmPlanAdd (src/dxmt/dxmt_gw2_warm.cpp): the same order, target filter and variants;
// - each variant's function comes from DXMT's AIR cache through the bridge's CacheReader (read-only),
//   named by warmFunctionName, as the warm variant job makes it (never a translation);
// - each pipeline is built by C3's Metal3PipelineBuilder (applyRecipe, then
//   MTLDevice_newRenderPipelineState), which reaches the bridge's real unix side, linked natively (B1):
//   its descriptor conversion (_MTLDevice_newRenderPipelineState) and the archive's lookup and capture
//   (pso_archive_new_render), byte for byte the game's;
// - the archive is the bridge's process instance, opened with WMTPSOArchiveInit (generations on) and
//   published with WMTPSOArchiveFlush: the exact-shape policy, the retries that exclude a shape Metal
//   names, the store's single-writer lock and its last-good-generation rule are all #75's.
//
// Commands (pso_archive_tool <command> [options]; --help lists the options):
//   run      builds every recipe in a child process and publishes one generation. A child that
//            crashes or can't publish (a failure naming no pipeline) is bisected in probe processes on
//            scratch stores no game reads; the recipes isolated that way are left out and the rest
//            published. Then a fresh process verifies the lookup (as `verify`).
//   verify   a fresh process builds the recipes against the published generation and reports lookup
//            hits and misses; publishes nothing.
//   status   the store's current generation (shards, keys, excluded shapes), whether a process holds
//            its writer lock, and the recipe and AIR files.
//   build    (internal) the child of `run`: --from/--to/--skip select recipes; --publish or not.
// Exit: 0 done, 1 usage or input error, 3 the store is busy (another process, a running game, holds
// its writer lock: nothing is written), 4 publishing failed, 5 a crash or failure the bisection
// couldn't isolate, 6 published, but the verifying process failed.

#include "Metal.hpp"
#include "airconv_public.h"
#include "dxmt_compile_recipe.hpp"
#include "dxmt_gw2_recipe.hpp"
#include "dxmt_gw2_warm.hpp"
#include "dxmt_gw2_warm_bridge.hpp"
extern "C" {
#include "pso_archive_publish.h"
}
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <glob.h>
#include <mach-o/dyld.h>
#include <set>
#include <spawn.h>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

extern char **environ;

using namespace dxmt;

namespace {

enum Exit { kOk = 0, kUsage = 1, kBusy = 3, kPublishFailed = 4, kUnisolated = 5, kVerifyFailed = 6 };

struct Options {
  std::string command;
  std::string cache_dir;   // where recipes_1.db and shaders_<target>.db are
  std::string recipes;     // overrides <cache_dir>/recipes_1.db
  std::string air;         // overrides <cache_dir>/shaders_<target>.db
  std::string archive_dir; // the store's base (DXMT_PSO_ARCHIVE_DIR's meaning); empty: the user cache dir
  std::string exe = "Gw2-64.exe";
  uint32_t keep = 4;       // the DLL's kKeepSessions
  uint32_t limit = 1u << 20;
  uint32_t metal_version = 0; // 0: as DXMT picks it for the device
  uint64_t from = 0, to = UINT64_MAX;
  std::string skip_file;
  bool publish = false;
  bool quiet = false;
  std::string self; // argv[0], for the children
};

void
usage(FILE *f) {
  fprintf(f, "usage: pso_archive_tool run|verify|status [options]\n"
             "  --cache-dir DIR     DXMT's per-exe cache (default: $(getconf DARWIN_USER_CACHE_DIR)dxmt/<exe>)\n"
             "  --recipes FILE      W1's recipes (default: <cache-dir>/recipes_1.db)\n"
             "  --air FILE          the AIR cache, read-only (default: <cache-dir>/shaders_<target>.db)\n"
             "  --archive-dir DIR   the archive store's base, as DXMT_PSO_ARCHIVE_DIR (default:\n"
             "                      $(getconf DARWIN_USER_CACHE_DIR)dxmt/pso); a separate one is a store no game reads\n"
             "  --exe NAME          the game's executable (default: Gw2-64.exe)\n"
             "  --keep N            sessions a generation keeps (default 4, as the DLL)\n"
             "  --limit N           recipes at most, in first-use order (default: all)\n"
             "  --metal-version N   the recipes' Metal target (default: as DXMT picks it, 320 on macOS 15+)\n"
             "  --quiet             no per-pipeline lines\n");
}

bool
parseU(const char *s, uint64_t &out) {
  char *end = nullptr;
  errno = 0;
  unsigned long long v = strtoull(s, &end, 10);
  if (errno || !end || *end || end == s)
    return false;
  out = v;
  return true;
}

// this executable's path, for the children (argv[0] may be a bare name found through PATH)
std::string
selfPath(const char *argv0) {
  char buf[PATH_MAX], real[PATH_MAX];
  uint32_t size = sizeof(buf);
  if (_NSGetExecutablePath(buf, &size) == 0 && realpath(buf, real))
    return real;
  return argv0;
}

bool
parseArgs(int argc, char **argv, Options &o) {
  o.self = selfPath(argv[0]);
  if (argc < 2)
    return false;
  o.command = argv[1];
  for (int i = 2; i < argc; i++) {
    std::string a = argv[i];
    auto value = [&](std::string &out) {
      if (i + 1 >= argc)
        return false;
      out = argv[++i];
      return true;
    };
    auto number = [&](uint64_t &out) { return i + 1 < argc && parseU(argv[++i], out); };
    uint64_t n = 0;
    if (a == "--cache-dir") {
      if (!value(o.cache_dir))
        return false;
    } else if (a == "--recipes") {
      if (!value(o.recipes))
        return false;
    } else if (a == "--air") {
      if (!value(o.air))
        return false;
    } else if (a == "--archive-dir") {
      if (!value(o.archive_dir))
        return false;
    } else if (a == "--exe") {
      if (!value(o.exe))
        return false;
    } else if (a == "--skip") {
      if (!value(o.skip_file))
        return false;
    } else if (a == "--keep") {
      if (!number(n) || !n || n > 64)
        return false;
      o.keep = uint32_t(n);
    } else if (a == "--limit") {
      if (!number(n) || !n || n > (1u << 20))
        return false;
      o.limit = uint32_t(n);
    } else if (a == "--metal-version") {
      if (!number(n) || (n != WMTMetal310 && n != WMTMetal320))
        return false;
      o.metal_version = uint32_t(n);
    } else if (a == "--from") {
      if (!number(o.from))
        return false;
    } else if (a == "--to") {
      if (!number(o.to))
        return false;
    } else if (a == "--publish") {
      o.publish = true;
    } else if (a == "--quiet") {
      o.quiet = true;
    } else {
      fprintf(stderr, "pso_archive_tool: unknown option %s\n", a.c_str());
      return false;
    }
  }
  if (!o.archive_dir.empty() && o.archive_dir[0] != '/') {
    fprintf(stderr, "pso_archive_tool: --archive-dir must be absolute\n");
    return false;
  }
  return true;
}

std::string
userCacheDir() {
  char buf[PATH_MAX];
  size_t n = confstr(_CS_DARWIN_USER_CACHE_DIR, buf, sizeof(buf));
  std::string s = n ? std::string(buf) : std::string();
  if (!s.empty() && s.back() != '/')
    s += '/';
  return s;
}

std::string
storeBase(const Options &o) {
  return o.archive_dir.empty() ? userCacheDir() + "dxmt/pso" : o.archive_dir;
}

// The device the game gets on this Mac (the first, as DXGI's first adapter) and DXMT's translation
// target for it (DeviceImpl in src/dxmt/dxmt_device.cpp).
WMT::Device
firstDevice(WMT::Reference<WMT::Array<WMT::Device>> &devices) {
  devices = WMT::Reference<WMT::Array<WMT::Device>>(WMTCopyAllDevices());
  if (devices == nullptr || devices.count() == 0)
    return WMT::Device{};
  return devices.object(0);
}

uint32_t
metalVersionFor(WMT::Device device) {
  uint64_t major = 0, minor = 0;
  WMTGetOSVersion(&major, &minor, nullptr);
  if (!MTLDevice_supportsFamily(device.handle, WMTGPUFamilyApple7))
    return WMTMetal310;
  return major >= 15 ? WMTMetal320 : WMTMetal310;
}

std::string
recipesPath(const Options &o) {
  if (!o.recipes.empty())
    return o.recipes;
  return (o.cache_dir.empty() ? userCacheDir() + "dxmt/" + o.exe : o.cache_dir) + "/recipes_" +
         std::to_string(gw2::kRecipeSchema) + ".db";
}

std::string
airPath(const Options &o, uint32_t metal_version) {
  if (!o.air.empty())
    return o.air;
  return (o.cache_dir.empty() ? userCacheDir() + "dxmt/" + o.exe : o.cache_dir) + "/shaders_" +
         std::to_string(metal_version) + ".db";
}

bool
fileExists(const std::string &path) {
  struct stat st;
  return !stat(path.c_str(), &st) && S_ISREG(st.st_mode);
}

// The recipes, planned as the warm thread plans them (W3, all pipelines), in first-use order: the warm
// thread's own read (#114: a row larger than the buffer grows it, here up to 256 MiB as before; a
// failed batch stops the read and is reported).
gw2::WarmPlan
readPlan(const std::string &path, uint32_t metal_version, uint32_t limit) {
  gw2::WarmPlan plan;
  std::atomic<bool> stop{false};
  gw2::warmRead(plan, gw2::warmEnumerateRecipes(path), metal_version, gw2::WarmMode::All, limit, stop, 256ull << 20);
  if (plan.read_error)
    fprintf(stderr, "pso_archive_tool: reading %s stopped at SQLite error %u (%llu busy retries); %llu recipes read\n",
            path.c_str(), plan.read_error, (unsigned long long)plan.busy, (unsigned long long)plan.recipes);
  if (plan.oversized)
    fprintf(stderr, "pso_archive_tool: %llu rows over 256 MiB skipped\n", (unsigned long long)plan.oversized);
  return plan;
}

std::set<uint64_t>
readSkip(const std::string &path) {
  std::set<uint64_t> skip;
  if (path.empty())
    return skip;
  FILE *f = fopen(path.c_str(), "r");
  if (!f)
    return skip;
  unsigned long long v;
  while (fscanf(f, "%llu", &v) == 1)
    skip.insert(v);
  fclose(f);
  return skip;
}

std::string
errorText(WMT::Reference<WMT::Error> &err) {
  if (err == nullptr)
    return "(no error)";
  auto text = err.description().getUTF8String();
  for (auto &c : text)
    if (c == '\n')
      c = ' ';
  return text.size() > 400 ? text.substr(0, 400) + "..." : text;
}

void
printReport(const std::string &report) {
  size_t start = 0;
  while (start < report.size() && report[start]) {
    size_t end = report.find('\n', start);
    if (end == std::string::npos)
      end = report.size();
    std::string line = report.substr(start, end - start);
    if (!line.empty() && line[0])
      fprintf(stderr, "  %s\n", line.c_str());
    start = end + 1;
  }
}

// --- build: one process builds recipes [from, to) minus the skipped ones through the game's code.
// With --publish it publishes a generation; without, it only builds (verify).
int
commandBuild(const Options &o, bool verify) {
  auto pool = WMT::MakeAutoreleasePool();
  WMT::Reference<WMT::Array<WMT::Device>> devices;
  WMT::Device device = firstDevice(devices);
  if (device == nullptr) {
    fprintf(stderr, "pso_archive_tool: no Metal device\n");
    return kUsage;
  }
  uint32_t metal_version = o.metal_version ? o.metal_version : metalVersionFor(device);
  std::string recipes = recipesPath(o), air = airPath(o, metal_version);
  if (!fileExists(recipes) || !fileExists(air)) {
    fprintf(stderr, "pso_archive_tool: missing %s\n", fileExists(recipes) ? air.c_str() : recipes.c_str());
    return kUsage;
  }
  auto plan = readPlan(recipes, metal_version, o.limit);
  auto skip = readSkip(o.skip_file);

  // the archive: the bridge's process instance, as the game opens it with DXMT_PSO_GENERATIONS=1
  std::string base = storeBase(o);
  WMTPSOArchiveConfig config = {};
  config.exe_name.set(o.exe.c_str());
  config.base_dir.set(base.c_str());
  config.keep_sessions = o.keep;
  // verify captures nothing (a cap of 0: no second compile for an archive it never publishes)
  config.max_pipelines = verify ? 0 : uint32_t(std::max<size_t>(plan.pipelines.size(), 1));
  config.generations = 1;
  std::string report(1 << 16, '\0');
  bool on = WMTPSOArchiveInit(device.handle, &config, report.data(), uint32_t(report.size()));
  printReport(report);
  if (!on) {
    fprintf(stderr, "pso_archive_tool: the archive store could not be opened\n");
    return kPublishFailed;
  }
  WMTPSOArchiveStats before = {};
  WMTPSOArchiveQuery(&before);
  if (o.publish && before.mode != 1) {
    fprintf(stderr, "pso_archive_tool: the store is busy: another process (a running game?) holds its writer lock; "
                    "nothing written\n");
    return kBusy;
  }

  // read-only (CacheReader opens it SQLITE_OPEN_READONLY); DXMT's kDXMTShaderCacheVersion
  auto reader = WMT::CacheReader::alloc_init(air.c_str(), AIRCONV_VERSION);
  if (reader == nullptr) {
    fprintf(stderr, "pso_archive_tool: cannot read %s\n", air.c_str());
    return kUsage;
  }
  std::vector<WMT::Reference<WMT::Function>> functions(plan.variants.size());
  std::vector<uint8_t> tried(plan.variants.size());
  uint64_t air_miss = 0, fn_failed = 0, built = 0, failed = 0, no_function = 0, selected = 0;
  const char *crash = getenv("PSO_ARCHIVE_TOOL_TEST_CRASH"); // tests only: abort at this recipe index
  auto function = [&](int index) -> WMT::Function {
    if (index < 0)
      return WMT::Function{};
    if (!tried[index]) {
      tried[index] = 1;
      auto inner = WMT::MakeAutoreleasePool();
      auto &v = plan.variants[index];
      auto data = reader.get(std::make_pair(v.shader, v.variant));
      if (data == nullptr) {
        air_miss++;
      } else {
        WMT::Reference<WMT::Error> err;
        auto library = device.newLibrary(data, err);
        if (library != nullptr && err == nullptr)
          functions[index] = library.newFunction(gw2::warmFunctionName(v).c_str());
        if (functions[index] == nullptr)
          fn_failed++;
      }
    }
    return functions[index];
  };
  compile::Metal3PipelineBuilder builder(device);
  uint64_t end = std::min<uint64_t>(o.to, plan.pipelines.size());
  for (uint64_t i = o.from; i < end; i++) {
    if (skip.count(i))
      continue;
    selected++;
    if (crash && std::to_string(i) == crash)
      abort();
    auto inner = WMT::MakeAutoreleasePool();
    auto [vs_index, ps_index] = plan.pipeline_variants[i];
    WMT::Function vs = function(vs_index), ps = function(ps_index);
    if (vs == nullptr || (ps_index >= 0 && ps == nullptr)) {
      no_function++;
      if (!o.quiet)
        printf("NOFUNCTION %llu\n", (unsigned long long)i);
      continue;
    }
    WMT::Reference<WMT::Error> err;
    auto state = builder.build(plan.pipelines[i], vs, ps, err);
    if (state == nullptr) {
      failed++;
      if (!o.quiet)
        printf("BUILDFAILED %llu %s %s\n", (unsigned long long)i, compile::recipeKey(plan.pipelines[i]).string().c_str(),
               errorText(err).c_str());
      continue;
    }
    built++;
  }

  int rc = kOk, flush = 0;
  if (o.publish) {
    // A flush tries 3 writes, each failure that names a pipeline excluding its shape (the archive's
    // `skipped` counter grows by the records it leaves out). Flush again while the last one named
    // something; one that named nothing would fail the same way (the bisection takes over). At most
    // kMaxFlushes flushes: 3 named shapes each.
    constexpr int kMaxFlushes = 64;
    WMTPSOArchiveStats s0 = {};
    WMTPSOArchiveQuery(&s0);
    uint64_t skipped_before = s0.skipped;
    for (int attempt = 0; attempt < kMaxFlushes; attempt++) {
      std::string r(1 << 16, '\0');
      flush = WMTPSOArchiveFlush(r.data(), uint32_t(r.size()));
      printReport(r);
      if (flush >= 0)
        break;
      WMTPSOArchiveStats s = {};
      WMTPSOArchiveQuery(&s);
      if (s.skipped == skipped_before)
        break;
      skipped_before = s.skipped;
    }
    if (flush < 0)
      rc = kPublishFailed;
  }
  WMTPSOArchiveStats s = {};
  WMTPSOArchiveQuery(&s);
  printf("RESULT recipes=%llu selected=%llu built=%llu failed=%llu nofunction=%llu airmiss=%llu fnfailed=%llu "
         "othertarget=%llu malformed=%llu hits=%llu misses=%llu known=%llu new=%llu missexcl=%llu excluded=%llu "
         "skipped=%llu generation=%llu published=%llu flush=%d mode=%s\n",
         (unsigned long long)plan.pipelines.size(), (unsigned long long)selected, (unsigned long long)built,
         (unsigned long long)failed, (unsigned long long)no_function, (unsigned long long)air_miss,
         (unsigned long long)fn_failed, (unsigned long long)plan.other_target, (unsigned long long)plan.malformed,
         (unsigned long long)s.lookup_hits, (unsigned long long)s.lookup_misses, (unsigned long long)s.miss_known,
         (unsigned long long)s.miss_new, (unsigned long long)s.miss_excluded, (unsigned long long)s.excluded,
         (unsigned long long)s.skipped, (unsigned long long)s.generation, (unsigned long long)s.published, flush,
         s.mode == 1 ? "writer" : s.mode == 2 ? "lookup" : "off");
  fflush(stdout);
  // the pipelines and functions go with the process; the archive's atexit report says what is unflushed
  return rc;
}

// --- run: children, bisection and verification

struct Child {
  int status = -1;  // the exit code, or 128 + the signal
  std::string out;  // its stdout
};

Child
spawnSelf(const Options &o, const std::vector<std::string> &extra) {
  std::vector<std::string> args = {o.self, "build"};
  auto add = [&](const char *flag, const std::string &v) {
    if (!v.empty()) {
      args.push_back(flag);
      args.push_back(v);
    }
  };
  add("--cache-dir", o.cache_dir);
  add("--recipes", o.recipes);
  add("--air", o.air);
  add("--exe", o.exe);
  args.push_back("--keep");
  args.push_back(std::to_string(o.keep));
  args.push_back("--limit");
  args.push_back(std::to_string(o.limit));
  if (o.metal_version) {
    args.push_back("--metal-version");
    args.push_back(std::to_string(o.metal_version));
  }
  args.insert(args.end(), extra.begin(), extra.end());
  std::vector<char *> argv;
  for (auto &a : args)
    argv.push_back(a.data());
  argv.push_back(nullptr);

  Child c;
  int fds[2];
  if (pipe(fds))
    return c;
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
  posix_spawn_file_actions_addclose(&fa, fds[0]);
  pid_t pid = 0;
  int err = posix_spawn(&pid, o.self.c_str(), &fa, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  close(fds[1]);
  if (err) {
    close(fds[0]);
    return c;
  }
  char buf[4096];
  ssize_t n;
  while ((n = read(fds[0], buf, sizeof(buf))) > 0)
    c.out.append(buf, size_t(n));
  close(fds[0]);
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  c.status = WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
  return c;
}

std::string
resultLine(const Child &c) {
  size_t at = c.out.rfind("RESULT ");
  if (at == std::string::npos)
    return "";
  size_t end = c.out.find('\n', at);
  return c.out.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

uint64_t
field(const std::string &line, const char *name) {
  std::string key = std::string(" ") + name + "=";
  size_t at = line.find(key);
  return at == std::string::npos ? 0 : strtoull(line.c_str() + at + key.size(), nullptr, 10);
}

bool
writeSkip(const std::string &path, const std::set<uint64_t> &skip) {
  FILE *f = fopen(path.c_str(), "w");
  if (!f)
    return false;
  for (auto v : skip)
    fprintf(f, "%llu\n", (unsigned long long)v);
  return fclose(f) == 0;
}

bool
removeTree(const std::string &path) {
  pid_t pid = 0;
  const char *argv[] = {"/bin/rm", "-rf", path.c_str(), nullptr};
  if (posix_spawn(&pid, "/bin/rm", nullptr, nullptr, const_cast<char **>(argv), environ))
    return false;
  int status = 0;
  waitpid(pid, &status, 0);
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

struct Bisect {
  const Options &o;
  std::string scratch_root;
  std::string skip_path;
  uint64_t probes = 0;
  std::vector<std::pair<uint64_t, int>> isolated; // recipe index, the probe's exit

  // a probe builds and publishes [from, to) into an empty scratch store of its own
  int probe(uint64_t from, uint64_t to) {
    std::string dir = scratch_root + "/probe-" + std::to_string(probes++);
    Child c = spawnSelf(o, {"--archive-dir", dir, "--skip", skip_path, "--from", std::to_string(from), "--to",
                            std::to_string(to), "--publish", "--quiet"});
    removeTree(dir);
    return c.status;
  }

  // the recipes of [from, to) whose probe fails alone; at most `budget` probes in all
  void isolate(uint64_t from, uint64_t to, uint64_t budget) {
    if (from >= to || probes >= budget)
      return;
    int rc = probe(from, to);
    if (rc == kOk)
      return;
    if (to - from == 1) {
      isolated.push_back({from, rc});
      fprintf(stderr, "pso_archive_tool: recipe %llu fails alone (exit %d): left out\n", (unsigned long long)from, rc);
      return;
    }
    uint64_t mid = from + (to - from) / 2;
    isolate(from, mid, budget);
    isolate(mid, to, budget);
  }
};

int
commandRun(const Options &o) {
  std::string scratch_root;
  {
    const char *tmp = getenv("TMPDIR");
    std::string t = tmp && *tmp ? tmp : "/tmp";
    while (t.size() > 1 && t.back() == '/')
      t.pop_back();
    std::string tmpl = t + "/pso-archive-tool.XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    if (!mkdtemp(buf.data())) {
      fprintf(stderr, "pso_archive_tool: cannot create a scratch directory\n");
      return kUsage;
    }
    scratch_root = buf.data();
  }
  std::set<uint64_t> skip = readSkip(o.skip_file);
  std::string skip_path = scratch_root + "/skip";
  int rc = kUnisolated;
  for (int round = 0; round < 4; round++) {
    writeSkip(skip_path, skip);
    std::vector<std::string> extra = {"--skip", skip_path, "--publish"};
    if (!o.archive_dir.empty()) {
      extra.push_back("--archive-dir");
      extra.push_back(o.archive_dir);
    }
    if (o.quiet)
      extra.push_back("--quiet");
    fprintf(stderr, "pso_archive_tool: building and publishing (%zu recipes left out)\n", skip.size());
    Child c = spawnSelf(o, extra);
    fputs(c.out.c_str(), stdout);
    fflush(stdout);
    if (c.status == kOk) {
      rc = kOk;
      break;
    }
    if (c.status == kBusy || c.status == kUsage) {
      rc = c.status;
      break;
    }
    // a crash (or a failure naming no pipeline): bisect in probes on scratch stores, no game reads them
    std::string line = resultLine(c);
    uint64_t total = line.empty() ? 0 : field(line, "recipes");
    if (!total) {
      // it died before its result: plan in this process to know how many recipes there are
      WMT::Reference<WMT::Array<WMT::Device>> devices;
      auto pool = WMT::MakeAutoreleasePool();
      WMT::Device device = firstDevice(devices);
      uint32_t mv = o.metal_version ? o.metal_version : device != nullptr ? metalVersionFor(device) : WMTMetal320;
      total = readPlan(recipesPath(o), mv, o.limit).pipelines.size();
    }
    fprintf(stderr, "pso_archive_tool: the build exited %d; bisecting %llu recipes in probe processes\n", c.status,
            (unsigned long long)total);
    Bisect b{o, scratch_root, skip_path, 0, {}};
    b.isolate(0, total, 4096);
    if (b.isolated.empty()) {
      fprintf(stderr, "pso_archive_tool: no recipe fails alone (%llu probes); nothing published\n",
              (unsigned long long)b.probes);
      rc = c.status == kPublishFailed ? kPublishFailed : kUnisolated;
      break;
    }
    for (auto &[index, why] : b.isolated) {
      printf("ISOLATED %llu exit=%d\n", (unsigned long long)index, why);
      skip.insert(index);
    }
    fprintf(stderr, "pso_archive_tool: %zu recipes isolated in %llu probes\n", b.isolated.size(),
            (unsigned long long)b.probes);
  }
  if (rc == kOk) {
    fprintf(stderr, "pso_archive_tool: verifying the lookup in a fresh process\n");
    std::vector<std::string> extra = {"--skip", skip_path, "--quiet"};
    if (!o.archive_dir.empty()) {
      extra.push_back("--archive-dir");
      extra.push_back(o.archive_dir);
    }
    Child v = spawnSelf(o, extra);
    std::string line = resultLine(v);
    if (v.status != kOk || line.empty()) {
      // the generation is published (it stays); the check of it failed
      printf("VERIFY failed exit=%d\n", v.status);
      rc = kVerifyFailed;
    } else {
      printf("VERIFY %s\n", line.c_str() + 7);
    }
    if (!skip.empty())
      printf("LEFTOUT %zu\n", skip.size());
  }
  removeTree(scratch_root);
  return rc;
}

// --- status: reads the store without opening it as a writer

int
commandStatus(const Options &o) {
  std::string base = storeBase(o);
  printf("store base: %s\n", base.c_str());
  std::string pattern = base + "/*/os-*/gen1";
  glob_t g = {};
  if (glob(pattern.c_str(), 0, nullptr, &g) != 0 || g.gl_pathc == 0)
    printf("no generation store under %s\n", base.c_str());
  for (size_t i = 0; i < g.gl_pathc; i++) {
    std::string dir = g.gl_pathv[i];
    printf("store %s\n", dir.c_str());
    // the writer lock: probed without blocking and released at once
    int fd = open((dir + "/LOCK").c_str(), O_RDONLY);
    if (fd >= 0) {
      bool held = flock(fd, LOCK_EX | LOCK_NB) != 0;
      if (!held)
        flock(fd, LOCK_UN);
      close(fd);
      printf("  writer lock: %s\n", held ? "held (a running game or tool writes this store)" : "free");
    }
    FILE *f = fopen((dir + "/CURRENT").c_str(), "r");
    char magic[16] = "";
    unsigned long long gen = 0;
    if (!f || fscanf(f, "%15s %16llx", magic, &gen) != 2 || strcmp(magic, "DXMTCUR1")) { // the publisher's index
      printf("  no current generation\n");
      if (f)
        fclose(f);
      continue;
    }
    fclose(f);
    char name[64];
    snprintf(name, sizeof(name), "/g%016llx.manifest", gen);
    std::vector<uint8_t> data;
    if (FILE *m = fopen((dir + name).c_str(), "rb")) {
      uint8_t buf[65536];
      size_t n;
      while ((n = fread(buf, 1, sizeof(buf), m)) > 0)
        data.insert(data.end(), buf, buf + n);
      fclose(m);
    }
    pso_gen_manifest man = {};
    if (!pso_gen_decode(data.data(), data.size(), &man)) {
      printf("  generation %llu: manifest missing or damaged (the game loads the last good one)\n", gen);
      continue;
    }
    uint64_t keys = 0, bytes = 0;
    for (uint64_t s = 0; s < man.shard_count; s++) {
      keys += man.shards[s].key_count;
      bytes += man.shards[s].size;
      printf("  shard: session %llu-%llu, %llu pipeline keys, %.1f MB\n", (unsigned long long)man.shards[s].started,
             (unsigned long long)man.shards[s].pid, (unsigned long long)man.shards[s].key_count,
             man.shards[s].size / 1048576.0);
    }
    printf("  generation %llu: %llu shards, %llu pipeline keys, %.1f MB, %llu excluded shapes\n", gen,
           (unsigned long long)man.shard_count, (unsigned long long)keys, bytes / 1048576.0,
           (unsigned long long)man.excluded_count);
    pso_gen_manifest_free(&man);
  }
  globfree(&g);
  std::string recipes = recipesPath(o);
  struct stat st;
  if (!stat(recipes.c_str(), &st))
    printf("recipes: %s (%.1f MB)\n", recipes.c_str(), st.st_size / 1048576.0);
  else
    printf("recipes: %s (missing)\n", recipes.c_str());
  return kOk;
}

} // namespace

int
main(int argc, char **argv) {
  Options o;
  if (!parseArgs(argc, argv, o)) {
    usage(stderr);
    return kUsage;
  }
  if (o.command == "run")
    return commandRun(o);
  if (o.command == "build")
    return commandBuild(o, !o.publish);
  if (o.command == "verify") {
    Options v = o;
    v.publish = false;
    return commandBuild(v, true);
  }
  if (o.command == "status")
    return commandStatus(o);
  if (o.command == "-h" || o.command == "--help" || o.command == "help") {
    usage(stdout);
    return kOk;
  }
  usage(stderr);
  return kUsage;
}
