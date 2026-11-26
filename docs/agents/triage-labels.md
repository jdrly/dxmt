# Triage labels

The skills speak in terms of five canonical triage roles. This file maps them to the labels of
`jdrly/dxmt`, and lists the type and area labels next to them.

## Triage state (exactly one on an open issue)

| Label in mattpocock/skills | Label in our tracker | Meaning                                                        |
| -------------------------- | -------------------- | -------------------------------------------------------------- |
| `needs-triage`             | `needs-triage`       | Jan needs to evaluate this issue                               |
| `needs-info`               | `needs-info`         | Waiting for information: a measurement, a trace, Jan's answer  |
| `ready-for-agent`          | `ready-for-agent`    | Fully specified: an agent can implement it                     |
| `ready-for-human`          | `ready-for-human`    | Needs Jan first: a decision, or an experiment in the game      |
| `wontfix`                  | `wontfix`            | Will not be actioned                                           |

A change to a GPU driver can't be checked without GW2 running. So here, unlike in SWTOR on Mac,
`ready-for-agent` means an agent can implement the issue, and Jan plays its UAT afterwards.
`ready-for-human` is for an issue that needs Jan's decision or his run in the game *before* the
code can be written.

## Type (exactly one)

| Label           | Meaning                                                      |
| --------------- | ------------------------------------------------------------ |
| `bug`           | Something runs or looks wrong, or a fork change misbehaves   |
| `enhancement`   | New or better behaviour that isn't about speed: logging, diagnostics |
| `documentation` | Only the docs change                                         |
| `performance`   | Frame rate, stutter, pacing, CPU or GPU cost, heat           |

## Area (one or more)

| Label           | Meaning                                                                                  |
| --------------- | ---------------------------------------------------------------------------------------- |
| `area:front-end` | The D3D11 front end on the game's render thread: contexts, pipeline cache, resources (`src/d3d11`) |
| `area:encoder`  | The command queue, chunks, the encode and finish threads, statistics (`src/dxmt`)         |
| `area:present`  | Swapchain, presenter, pacing, the background throttle (`src/d3d11/d3d11_swapchain.cpp`, `src/dxmt/dxmt_presenter.*`) |
| `area:bridge`   | The Wine bridge, PE and unix sides (`src/winemetal`)                                      |
| `area:shaders`  | The shader compiler, shader and pipeline caches, compile waits (`src/airconv`, `src/dxmt/dxmt_shader_cache.*`, `src/d3d11/d3d11_pipeline*`) |
| `area:logging`  | Log lines, STATS fields, switches and their documentation (`docs/gw2/README.md`)          |
