# The PSO archive is deleted

Decided 2026-11-16. Status: accepted.

`DXMT_PSO_ARCHIVE` kept built pipelines in `MTLBinaryArchive` files so a later session could skip
their compiles (#5, #52). It never paid. My performance review of 2026-11-10 found that Metal keeps
every compiled shader stage on disk itself, and a second process builds a known pipeline in 0.12 ms,
so the archive saves nothing that cache doesn't. Its hit rate across sessions was 15 to 25%. With it
on, the game crashed at exit: two crash reports of 2026-11-15 fault on the `dxmt-pso-archiv` thread
in `_dispatch_queue_cleanup` during `pthread_exit`, with `_WMTPSOArchiveFlush` and its
`dispatch_sync` on the stack. So the archive goes, with its tool, its tests and the crash. This is
ADR 0010's rule applied to the largest piece: 2,404 lines on the unix side, 859 in the tool, 1,347
in tests.

## Consequences

- `DXMT_PSO_ARCHIVE`, `DXMT_PSO_ARCHIVE_DIR`, `DXMT_PSO_FLUSH_EVERY` and `DXMT_PSO_GENERATIONS`
  leave the code, the startup line and the README. The COMPILE section loses its archive fields.
- Bridge calls 149 to 151 are `WMT_RESERVED` lines (ADR 0005).
- Upstream's two pipeline creation functions and `newFunctionWithName:constantValues:` are back to
  upstream's text, so seven upstream files shrink.
- Recording pipeline recipes (`DXMT_PSO_RECORD`, `recipes_1.db`) and warming (`DXMT_WARM`) are a
  different mechanism and stay (ADR 0001).
- Old archives stay on disk under `<cache>/dxmt/pso`. Removing them is a follow-up for the app's
  Repair.
- My check is three quits from the game's menu without a crash report that names `dxmt-pso-archiv`.

## Alternatives I rejected

- **Keeping the archive and fixing its exit crash.** The record doesn't argue this option. The
  reasons I gave for deleting are the two above: it saves nothing that Metal's own cache doesn't,
  and it crashes at exit.

## Evidence

- #275 (PR #276, `b8cdaa5`), #5, #52.
- GW2 on Mac workspace: `docs/research/perf-review-2026-11-10.html` ("Work I can stop"),
  `docs/research/pso-archive-reserialize-2026-11-04.md`,
  `advisor-plans/2026-11-16-forks/vetted-A-dxmt.md` (ARCH-01, BUG-01, BUG-07) and
  `plans/002-dxmt-delete-pso-archive.md`; `plans/night/2026-11-11-plan.md`, 2026-11-15 22:10 (the
  exit crash found) and 2026-11-16 15:28 (merged).
