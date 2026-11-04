# One compile job per key; warming and demand share it

Shader variants and pipelines are built only through the compile service. It creates at most one job
per key (a stable content key, or a process-local key where no recipe schema exists), takes each job's
dependencies when the job is created, and alone decides the order. Warming and live draws share jobs
and results, because separate object-keyed caches would let an ordinary task rebuild a warmed pipeline.
We chose this over adding demand to the old scheduler, which would have left dedupe to three tables
kept by convention. The pointer-keyed front-end lookup stays as the fast path into it.

## Consequences

- C1 (this ADR's first step) moves the scheduler into `compile::Service` with its `legacy` policy and
  no behaviour change: the old run/park loop as is, job state owned by the service and kept with
  STATS off, one `demand()` instead of four wait copies. A parity fixture
  (`tests/gw2/compile_service_test.cpp`) holds the service to the old scheduler's traces.
- C2 adds the `declared` policy behind `DXMT_COMPILE_DEMAND` (off): dependencies taken at submit,
  the demand lane with closure promotion, and speculative admission (one at a time, none while a
  demand waits). Owning jobs moved on to C4, with the stable keys that need them.
- Stable keys and shared results come with C3/C4; warming (W2/W3) acquires speculative jobs through the
  service and keeps no queue or key table of its own.

Source: GW2 on Mac `council/2026-11-04-arch-grill/final.md` §1 (ADR draft 0001).
