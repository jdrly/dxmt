# The GW2 fork of DXMT

The words this fork uses for its own concepts: the D3D11→Metal driver as GW2 on Mac changes it for
Guild Wars 2. Upstream's `docs/GLOSSARY.md` belongs to upstream; the fork's language lives here.
Each section comes from the architecture council of 2026-11-04 and arrives with the change that
uses it.

## Language

### Switches

**Switch**:
A `DXMT_*` setting, with a fallback and a grammar, read by the module that declares it.
_Avoid_: flag, option, env var

**Requested value**:
What the environment or the control file asks a switch for, as raw text.

**Effective value**:
What the process actually uses after parsing, fallbacks and capability checks; the startup line
shows it.

**Live switch**:
A switch the control file may change while the game runs.
_Avoid_: hot reload

**Control file**:
The file named by `DXMT_CONTROL` that carries live overrides.

### Compile service

**Compile job**:
The one build of a shader variant or pipeline for a key; queued, running, parked, building, complete
or failed. Its record (state, what it is parked on, and with `DXMT_STATS` its timeline) belongs to the
compile service.
_Avoid_: task, ThreadpoolWork, work item

**Demand**:
A thread blocking on a compile job because it needs the result now (`compile::demand`).
_Avoid_: stall, wait (alone)

**Wait cause**:
What a demanded job was doing when the demand began (`waitcause` in COMPILE).
_Avoid_: wait reason

**Warming**:
Preparing variants and pipelines before they are demanded; its jobs are **speculative**.
_Avoid_: prewarm, preload

**Pipeline recipe**:
The complete, explicit inputs a pipeline is built from (W1 records them).
_Avoid_: PSO desc, descriptor (that is Metal's object)

**Stable key**:
A key computed from content, the same in every process.
_Avoid_: hash (alone), persisted key

**Variant**:
One translation of a shader for one set of translation inputs.
