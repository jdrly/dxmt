# Switches are declared by the module that reads them

There is no central settings struct. Each module declares its switches with their existing grammar,
they register deterministically, and the startup line is generated with today's names. A central
struct was rejected because the fork develops in parallel lanes, and a shared per-switch file was the
cause of nine swapchain commits that only added a startup field (architecture review §4). Parser
unification is deliberately not part of this. It would change accepted values.

## Consequences

- The startup line keeps today's fields in today's order. A switch declared later and not named by
  `LogStartup` follows them, sorted by name, before the process facts `cpus` and `refresh`. (The
  council's draft sorted the whole line; Jan's rule for R1 was an identical line, so the existing
  order stays.)
- Each switch keeps its own grammar (`!= "0"`, `== "1"`, `strtoul` without a full-consumption
  check, …). A grammar change is a behaviour change of its own, with its own PR.
- A read parses the environment when it happens; the switch keeps no cache. Callers keep the cache
  they always had (a `static const`, a per-object member) or read each time, so R1 changed no read's
  timing. One shared parse-once cache, as the council sketched, would change when values are sampled
  and is a separate, measured change.
- Live switches (the control file) are a separate scope and arrive with their own change.

Source: GW2 on Mac `council/2026-11-04-arch-grill/final.md` §4 (ADR draft 0004); implemented in
jdrly/dxmt #60.
