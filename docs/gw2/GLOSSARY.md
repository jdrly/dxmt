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

**Lane**:
Where a compile job queues under the declared policy: demand (taken first), normal, or speculative
(warming: one at a time, none while a demand waits).

**Promotion**:
A demand moving its job and the job's unfinished dependencies to the demand lane, once each.

### Chunks and retirement

**Chunk**:
One sequenced batch of commands that is committed and retired together (`CommandChunk`).
_Avoid_: frame

**Chunk sequence**:
A chunk's identity within one command queue (`seq`, from 1), distinct from its **ring slot**.

**Ring slot**:
A chunk's reused place in the queue's ring (`seq % DXMT_CHUNKS`). Always "ring slot": a bridge call's
position is its **call number**, so neither is called a bare "slot".
_Avoid_: slot (alone)

**Hold**:
Ownership that keeps an object alive until its chunk retires (`CommandQueue::Hold`, the retirement
ledger).
_Avoid_: keep, retain (Metal's word)

**Retire**:
The CPU release of everything a chunk used, after all its commits completed and every completion
consumer is done (`CommandQueue::RetireChunk`, on the finish thread).
_Avoid_: finish, complete (the GPU's event)

**Retirement ledger**:
The queue's typed record of what each chunk holds, per ring slot and stamped with the chunk
sequence; it releases a chunk's holds only when that chunk retires, in a fixed order.

**Hold reason**:
Why a chunk holds something (`HoldReason`): a present split's command buffer, the drawable, the
presenter, an event, a MetalFX scaler. Every hold names one.

**Retention policy**:
Which hold reasons a queue's chunks must hold (`RetentionPolicy`): everything, unless Metal 3's
retained command buffers hold it themselves (`DXMT_UNRETAINED=0` without the lifetime check); the
present split's buffers always. The ledger decides it, not the call site.

### Bridge

**Bridge call**:
One named PE→unix call through Wine's unix-call interface: a thunk in `winemetal_thunks.c` on the
PE side, a handler in `src/winemetal/unix/` on the unix side.
_Avoid_: thunk (that is the PE half), syscall

**Call number**:
A bridge call's explicit number, its line in the call manifest (`src/winemetal/winemetal_calls.h`)
and its index in both unix-call tables (`WMTCall_<name>`). Code names it, never writes it: a thunk
says `UNIX_CALL(NSObject_retain, …)`, and a literal number doesn't build. A number nobody uses yet
is **reserved** (`WMTReserved_<number>`, which no thunk can name), with the issue or lane that
reserved it.
_Avoid_: slot (that word belongs to the chunk ring: a **ring slot**), index, code

**Call manifest**:
`src/winemetal/winemetal_calls.h`, the one list of call numbers: the enum and both tables are
generated from it.
_Avoid_: call table (the generated `__wine_unix_call_funcs` and `__wine_unix_call_wow64_funcs`)

**Command packet**:
An encoded graphics command that the unix side interprets (the Metal 3 and Metal 4 encoders'
`encodeCommands`). Many packets travel in one bridge call; a packet is not a bridge call.

### Submission

**Commit**:
One command buffer of a chunk handed to the GPU. A chunk has one, or several when it splits (Metal 4's
queue events commit as well).

**Split**:
Committing a chunk's open command buffer early and continuing in a new one (the present split, which
commits the frame's work before `nextDrawable`). Its command buffers are the chunk's **split buffers**;
the one committed last is the chunk's **last buffer**.
_Avoid_: flush

**Commit record**:
What is known about one commit once it completed (`CommitRecord`, `src/dxmt/dxmt_gw2_commit_record.hpp`):
its ordinal in the chunk's commit order, its GPU start and end if Metal gave them (unknown is not 0),
its error, and on Metal 3 its logs. Metal 3 builds it from the command buffer, Metal 4 from the
commit's feedback; the finish thread reports a chunk from its records alone.
_Avoid_: feedback (that is Metal 4's source of it)

**Completed chunk**:
A chunk whose commits have all completed and whose commit records are final; it is reported, then
retired.

**Chunk submission**:
How a command queue's chunks are committed and completed (`ChunkSubmission`): begin, event waits and
signals, encode, commit on the encode thread; wait (a completed chunk) and retire on the finish thread;
drain at teardown. One adapter per backend (`Metal3Submission`, `Metal4Submission`), chosen once per
command queue; the encoding inside stays each backend's own.
_Avoid_: backend (alone), submit path
