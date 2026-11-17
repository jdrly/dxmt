# The compile service is tested on its own, without the old scheduler

`compile_service_test.cpp` no longer runs its scripted job graphs through a copy of upstream's
`dxmt_tasks.hpp` scheduler, and no longer requires the two traces to match. The comparison guarded
the move from that scheduler to the compile service (C1 to C4, ADR 0001). The service has run in my
everyday build since 2026-11-04, so the comparison has done its job. What it still cost was #198: it
failed now and then on a busy machine, 2 of 445 runs, because a worker on either side could reach
the gate a few milliseconds late. The copy also hung at teardown twice in about 475 runs, because it
still has the lost wakeup that #72 fixed in the service. I chose deleting the copy over #198's own
repair, which stores the copy's stop under its lock and waits on job state instead of 8 ms of quiet.
That repair keeps 156 lines of a scheduler nobody runs, for a comparison whose question has been
answered.

## Consequences

- The eleven scenarios still run, through the service with its default options, which is the
  `legacy` policy my build runs unless `DXMT_COMPILE_DEMAND=1` or `DXMT_WARM` asks for the
  declared one. Each must not get stuck, must complete every job exactly once and must bring the
  compile queue's count back to zero. None of these checks compares the order of jobs.
- The rest of what guards the service is unchanged. In `compile_service_test.cpp`: the job records
  and demands with STATS off, the `declared` policy's cases (no park, the demand lane, speculative
  admission and `DXMT_WARM_JOBS`, the stress run), and each worker's priority (#152).
  `compile_service_stop_test.cpp` holds the stop to #72's fix, and `warm_test.cpp` runs warm jobs
  through the declared service.
- The eleven scenarios no longer pin the scheduling order or the worker growth. A change that hands
  out their jobs in another order, or grows workers at another time, passes as long as every job
  still completes once. The declared cases still check exact orders, and `workerPriority` still
  needs growth to four workers. If the scenarios' order needs a regression test again, the way is to
  record the service's own trace as a fixed expectation. That needs #198's wait on job state first,
  or the recorded trace flakes the same way the comparison did.
- The 8 ms quiet period still decides when a step is settled. A worker later than that, with no
  other job at the gate and nothing left to submit, makes a scenario report itself stuck. That job
  then reaches the gate with nobody left to release it, and the service's destructor waits for it
  forever, so the run hangs instead of failing. I haven't seen that happen. If it does, #198's wait
  on job state is the fix. The declared cases' own 8 ms wait does fail now and then under load, a
  separate bug.
- This replaces ADR 0001's sentence that a parity fixture holds the service to the old scheduler's
  traces. I leave 0001 as it was written. Upstream's file stays readable at the commit the copy came
  from, `git show 02532d0:src/dxmt/dxmt_tasks.hpp`.

Source: jdrly/dxmt #198 and my plan 031 (GW2 on Mac `advisor-plans/2026-11-16-forks`).
