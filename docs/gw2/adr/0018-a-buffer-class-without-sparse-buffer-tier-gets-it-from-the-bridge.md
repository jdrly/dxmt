# A buffer class without sparseBufferTier gets it from the bridge

Decided 2026-11-19. Status: accepted.

Metal 4, Metal 3 and the residency set all depend on Apple's Metal getting an answer from every
buffer it asks. Where a driver's buffer class lacks a method that the macOS SDK makes required, my
bridge adds that method, returning the value the SDK documents for DXMT's buffers. My fork doesn't
fall back to another path for it, and no setting is involved.

## Context

A player's 0.2.0 ended at start on an M2 Max (Mac14,6) with macOS 26.5.2:

```
*** Terminating app due to uncaught exception 'NSInvalidArgumentException', reason: '-[AGXG14XFamilyBuffer sparseBufferTier]: unrecognized selector sent to instance 0xbefe36f40'
```

It happened on Metal 4 (the default) and on Metal 3 (`DXMT_METAL4=0`). My fork never sends
`sparseBufferTier`. Every 0.2 test before this ran on macOS 27, where the driver's buffer classes
answer it.

What the macOS 26 SDK says (`MTLBuffer.h`, `MTLResource.h`, Apple's documentation):

- `@property (readonly) MTLBufferSparseTier sparseBufferTier`, `API_AVAILABLE(macos(26.0))`, is a
  required property of the `MTLBuffer` protocol. So Metal built against that SDK may send it to any
  buffer.
- `typedef NS_ENUM(NSInteger, MTLBufferSparseTier)`: `MTLBufferSparseTierNone = 0` ("the buffer is
  not sparse") and `MTLBufferSparseTier1 = 1`.
- A sparse buffer is made with `newBufferWithLength:options:placementSparsePageSize:`, and its
  mappings change only through `MTL4CommandQueue`'s `updateBufferMappings` and
  `copyBufferMappingsFromBuffer`. Metal 3's `MTLCommandQueue` has no such call. DXMT makes no sparse
  buffer, so `None` is the true answer for every buffer it has.

## Decision

At the process's first `MTLCopyAllDevices` (`_MTLCopyAllDevices`, the only way DXMT gets a device),
the bridge's unix side makes one buffer of each kind DXMT makes: shared, private, managed, on CPU
memory, and placed in a heap. If a buffer's class doesn't respond to `sparseBufferTier`,
`class_addMethod` gives it one that returns `MTLBufferSparseTierNone`, and a line goes to stderr
(`wine.log`). A class that answers already, its own or inherited, is left alone. Before macOS 26
nothing is done, since the property doesn't exist there.

Metal 4 and the residency set stay as they are. On a device whose classes answer, as on my M5 Max
and the rented M1 on macOS 27, nothing changes and nothing is logged.

There is no switch. The change can't make a frame look different: it adds a method or it doesn't,
and the method only answers a question with the value that is true for DXMT's buffers.

This is an exception to ADR 0007, which has a bridge change merge with its switch off, and to the
Definition of Done's off-switch, decided in #341. A switch would leave the crash in place for every
player on a 26.x Mac whose driver lacks the method, and the release it is for (0.2.1) exists to end
that crash. Without the method there, the game ends at start. With it, Metal gets the answer that is
true. So the exception is narrow: on by default and without a switch, but it still merges only
after `tests/gw2/run.zsh pure metal` ran on a Mac, as ADR 0007 asks of a default that is on. Its
rollback is the previous build and a revert.

## Which call sends it

No stack names the sender. The game's `wine.log` has an empty "First throw call stack", and the
crash reports in the player's bundle are the app's own (#310). What the evidence allows:

- **The residency set (likely).** The Metal 3 crash rules out a Metal-4-only path. By default both
  paths add DXMT's buffers to one `MTLResidencySet` (`addAllocations`, `commit`, the queue's
  `addResidencySet`). Heap-placed and CPU-placed buffers stay out and keep `useResource`. That code
  is Apple's code shared by every GPU family, so it is the kind of caller that assumes every buffer
  class implements a new required property. It is a suspect, not a finding: no call is pinned.
- **Something only 0.2 runs (the other candidate).** 0.1.4 ships DXMT `e02a4d6`, which already had
  the residency set and Metal 4 on by default. The same Mac played it on 26.5.2 earlier the same
  day. If those sessions really ran with the set on, then something in 0.2 makes the difference: a
  native arm64 process instead of x86_64 under Rosetta, another Wine host binary and SDK, or one of
  136 DXMT commits.

I can't tell the two apart from here. The player's `DXMT_RESIDENCY_SET=0` run would. The added
method doesn't depend on which one it is: whoever sends the selector gets an answer.

## Consequences

- `sparseTextureTier` is the same kind of property on `MTLTexture` (macOS 26). Nothing shows a
  texture class without it, so the bridge doesn't add it. If a 26.x Mac ever stops on
  `-[AGX…Texture sparseTextureTier]`, the same fix applies to textures.
- A driver class that grows the method later (as macOS 27's have) keeps its own, because the bridge
  adds nothing to a class that answers.
- The fork's runner is macOS 27, so CI can't show a class without the method.
  `tests/gw2/sparse_buffer_tier_test.m` checks the add on stand-in classes, and prints the real
  device's classes and their answers as a `RESULT` line. Run on a 26.x Mac, that line is the
  evidence.

## Evidence

- #341: the player's logs of 2026-11-19 (0.2.0, DXMT `da80816`), and the Metal 3 result in its
  comments.
- The SDK's `MTLBuffer.h` and `MTLResource.h` (via metal-cpp's `MTLBuffer.hpp`, `MTLResource.hpp`
  and `MTL4CommandQueue.hpp`), and Apple's documentation pages for `sparseBufferTier` and
  `MTLBufferSparseTier`.
