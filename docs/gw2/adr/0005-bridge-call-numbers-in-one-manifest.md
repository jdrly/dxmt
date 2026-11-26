# Bridge call numbers are explicit in one manifest

Every bridge call's number is written once, in `src/winemetal/winemetal_calls.h`, and the enum
(`WMTCall_<name>`), the 64-bit table and the wow64 table are generated from it. Numbers are append
only, and a hole is `WMT_RESERVED` with the issue that reserved it. PE and unix always ship from one
build, so compaction is allowed but never required. I chose this over a generator for params
structs and thunks, which would be too wide. Their layouts and wow64 conversions are hand-shaped,
and the failure was numbering and drift, not layouts. I also kept the holes for now, because
renumbering the calls of lanes still in flight is risk for nothing. Native tests use the production table, because
duplicate test handlers can't expose dispatch drift.

## Consequences

- The manifest has three line forms. `WMT_CALL(number, name, handler)` puts one handler in both
  tables; `WMT_CALL_WOW64(number, name, handler, wow64_handler)` gives the wow64 table its own (9
  airconv calls today), so a distinct wow64 handler is visible at a glance; `WMT_RESERVED(number,
  "who")` is NULL in both. (The council's draft had one four-field form; three forms keep the 181
  same-handler lines short and the exceptions explicit.)
- The build rejects a duplicate number or name, a gap and a line out of order (each line's position
  must equal its number), a table not `WMTCall_Count` long, and a move of the fixed anchors
  `NSObject_retain` = 0, `MetalView_visibility` = 147 and `MTLDevice_metal4Support` = 155, so the
  checks don't only compare the manifest with itself.
- A lane that needs a bridge call appends a line, or turns its own reserved line into a call; nobody
  hands out numbers any more. B2 kept every number and hole of the hand-written tables (0–192) and
  added the reservation 193–199 for lane C (#64).
- Code names a bridge call and never writes its number (B3, #76). The thunks in
  `winemetal_thunks.c` and `airconv_thunks.c` say `UNIX_CALL(NSObject_retain, …)`; the macro pastes
  the name onto `WMTCall_`, so a literal number, a misspelt name or a reserved number
  (`WMTReserved_<number>`) fails to build. airconv has no enum of its own any more. The enum lives in
  `winemetal_call_numbers.h`, which both thunk files include.
- Native tests dispatch through the production table: `src/nativemetal/wineunixlib.h`'s
  `WINE_UNIX_CALL` is `wmt_native_dispatch` (`winemetal_unix.c`, `DXMT_NATIVE` only), which returns
  an error for a reserved number instead of jumping to NULL. `mtl4_queue_test` links the whole unix
  side; only airconv is stubbed (`tests/gw2/shim/airconv_absent.c`). Hand-written handlers remain
  only in tests that call one handler directly or inject a failure on purpose.

Source: GW2 on Mac `council/2026-11-04-arch-grill/final.md` §5 (ADR draft 0005); implemented in
jdrly/dxmt #67 (B2) and #76 (B3).
