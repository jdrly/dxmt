# Bridge call numbers are explicit in one manifest

Every bridge call's number is written once, in `src/winemetal/winemetal_calls.h`, and the enum
(`WMTCall_<name>`), the 64-bit table and the wow64 table are generated from it. Numbers are append
only, and a hole is `WMT_RESERVED` with the issue that reserved it. PE and unix always ship from one
build, so compaction is allowed but never required. We chose this over a generator for params
structs and thunks (too wide: the params layouts and wow64 conversions are hand-shaped, and the
failure was numbering and drift, not layouts) and over removing the holes now (renumbering the
calls of lanes in flight is risk for nothing). Native tests use the production table, because
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
  checks don't only compare the manifest with itself. airconv's own enum must agree with it.
- A lane that needs a bridge call appends a line, or turns its own reserved line into a call; nobody
  hands out numbers any more. B2 kept every number and hole of the hand-written tables (0–192) and
  added the reservation 193–199 for lane C (#64).
- Until B3 the thunks still pass numeric literals to `UNIX_CALL`; B3 gives them the manifest's
  names and moves the native tests onto the real table.

Source: GW2 on Mac `council/2026-11-04-arch-grill/final.md` §5 (ADR draft 0005); implemented in
jdrly/dxmt #67.
