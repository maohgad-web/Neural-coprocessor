This document is the original P0 brief and is partially superseded. Where it conflicts with instructions relayed in-session, the relayed instruction wins. Section 04 (T6/T7 mechanism) and T2's selection rule are known stale — do not build from them without confirming.

Multi-GPU DLSS-NR R&D · Milestone 0 · Gate P0

**Cross-Adapter Bridge M0**

Get a ReShade add-on to stand up a private D3D12 device on GPU 1 and run
LumeniteFX there — under stock ReShade, while the game renders on GPU 0.
Nothing crosses between the cards yet.

**P0 proves one thing:** that a second, independent ReShade effect
runtime can exist on a second physical adapter inside a running game's
process. If that holds, every later milestone is plumbing. If it
doesn't, the architecture is wrong and we've spent days, not weeks.

01 · Scope

**What P0 is, exactly**

The add-on loads under stock ReShade. It enumerates adapters, creates a
D3D12 device on adapter 1, opens a window and swapchain on that device,
and ReShade builds a second effect runtime for it. LumeniteFX compiles
and executes inside that runtime — on GPU 1 — while the game renders on
GPU 0.

| **P0 proves**                                                        | **Observable as**                                          |
|----------------------------------------------------------------------|------------------------------------------------------------|
| Add-on loads under a stock, unmodified ReShade                       | One log line at init                                       |
| Adapter 1 is enumerable and bindable by LUID inside the game process | Adapter table in the log                                   |
| A private D3D12 device coexists with the game's on another card      | Device-created log line, game unaffected                   |
| **ReShade builds a runtime for a swapchain we created**              | init_effect_runtime fires twice, one with adapter 1's LUID |
| LumeniteFX compiles and runs on GPU 1                                | A second window showing a live flow field                  |

**What P0 explicitly does not do**

Each of these is excluded for a reason, not just deferred. The reasons
matter — an agent that understands them won't drift toward them.

- **No cross-adapter transfer.** "Can a second runtime exist on another
  GPU" and "can we move frames between GPUs" are two independent hard
  problems. Bundled together, a failure is ambiguous and you debug both
  at once. Separated, each failure names itself.

- **No capture from GPU 0.** reshade_finish_effects belongs to the
  transit milestone. P0 never touches the game's buffers, which is also
  why P0 cannot break the game's rendering.

- **No DLSS-NR, no NGX, no nvngx.** The neural layer consumes wrong
  input silently. It goes nowhere near a bridge that isn't yet visually
  validated.

- **No timing, no timestamp queries, no numbers.** Cross-adapter
  timestamps need GetClockCalibration to correlate two unrelated clock
  domains. Any millisecond figure produced at P0 would be wrong and
  would get quoted later.

- **No modification to ReShade or LumeniteFX.** Stock ReShade is the
  whole point of the approach. LumeniteFX is used as shipped.

**The synthetic input follows from the scope**

With no transit, there is no game content on GPU 1 — so LumaFlow needs
something to find motion in. P0 generates an animated test pattern *as
the first technique in the GPU 1 preset*. This is not a synthetic rig
standing in for the pipeline: the pipeline is real, in-process, under
stock ReShade, on the real second card. Only the input is synthetic, and
only until transit exists. It buys something the game can't: **known
ground truth**. Scroll the pattern 2 px/frame and the flow field must
read 2 px. That validates LumaFlow itself, which no game frame can do.

02 · Prerequisite

**Gate Zero — the agent's envelope**

Settle this before the first commit. It costs nothing to grant and it is
what determines whether P0 takes days or a week.

| **Capability**                           | **Verdict**          | **Why**                                                                                                                                                                                                                 |
|------------------------------------------|----------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| **Read public GitHub repos**             | Required             | Verify ReShade's headers directly before the first commit; read LumeniteFX technique names for the preset.                                                                                                              |
| **Create files** in the one private repo | Required             | The repo starts empty. Modify-only means a human hand-scaffolds every path first, which defeats the setup.                                                                                                              |
| **Read Actions run logs**                | Strongly recommended | Highest-leverage permission available. Without it every missing semicolon costs a human round trip; with it, compile errors become a loop the agent closes alone. Grants no new write surface.                          |
| **Write access to ReShade**              | Withhold             | Not a permissions question — an architecture tripwire. P0's premise is that stock ReShade suffices. An agent wanting to patch ReShade means P0 failed, and that is news you want, not something routed around silently. |
| **Write to any third-party repo**        | Not needed           | Nothing third-party is forked or vendored. CI fetches ReShade's headers; LumeniteFX never enters the repo.                                                                                                              |

03 · Dependencies

**What gets cloned, forked, and installed**

Nothing is forked. One private repo, created fresh, holding only our own
code.

| **Repository**             | **Role at P0**                                                                         | **Treatment**                                                                               |
|----------------------------|----------------------------------------------------------------------------------------|---------------------------------------------------------------------------------------------|
| **crosire/reshade**        | Add-on headers — reshade.hpp, reshade_events.hpp, reshade_api\_\*.hpp. API version 20. | **Fetched by CI at a pinned SHA.** Not forked, not vendored, not a submodule.               |
| **umar-afzaal/LumeniteFX** | Kernel + LumaFlow, run as shipped by the GPU 1 runtime.                                | **Never enters the repo.** Runtime asset, not a build input — installed by hand on the rig. |
| **clshortfuse/renodx**     | Read-only reference for private-device instantiation patterns.                         | Read on GitHub. Not a dependency.                                                           |

**Do not fork LumeniteFX on GitHub**

Per GitHub's docs: *"All forks of public repositories are public. You
cannot change the visibility of a fork."* A fork of LumeniteFX is
therefore permanently public, and pushing a modified shader to it is
public redistribution of a modified work — precisely what AGNYA
withholds without the author's permission. Nothing here is forked; the
working repo is created fresh and private. Should LumaFlow ever need
modifying, it happens in a private repo, never a fork.

**ReShade compiles .fx at runtime**, so LumeniteFX is never a compile
input and CI has no reason to see it. It is installed into the game's
shader directory exactly as any ReShade user installs it: unmodified,
private, never redistributed — squarely inside AGNYA's private-use
grant, with no copy in our tree.

**On the rig, by hand**

- An **add-on-enabled ReShade build** for the target game. The stub will
  not load under an add-on-disabled build, and that failure looks
  identical to a broken add-on — confirm a trivial add-on loads first.

- LumeniteFX, unmodified, in the shader path, at a recorded release.

- Our pattern.fx, mv_debug.fx, and gpu1.ini beside it.

- ReBAR in the state recorded in VENDOR_LOCK.md.

**Licensing files in the repo**

LICENSE — proprietary, all rights reserved, not
published. THIRD_PARTY.md — BSD 3-Clause attribution for the ReShade
headers CI pulls, plus a statement that LumeniteFX is used unmodified at
runtime and is neither contained in nor distributed by this
repository. VENDOR_LOCK.md — pinned ReShade SHA, LumeniteFX release on
the rig, driver version, ReBAR state. Keep this repo's remotes disjoint
from the community project's.

04 · Mechanism

**How the second runtime appears**

ReShade hooks DXGI process-wide and associates an independent effect
runtime with most swapchains. We never hand ReShade a foreign device —
we create a swapchain it will hook on its own, and the runtime it builds
is already on GPU 1.

```
GPU 0 · ADAPTER 0                    GPU 1 · ADAPTER 1
DX12 game render                     our device + window + swapchain
ReShade runtime #1 (game)            ReShade runtime #2 (ours)
untouched by P0                      pattern → Kernel → LumaFlow → mv_debug
  no capture, no hooks,              Present → debug window
  nothing crosses over
                    same process · same ReShade · no data path
```

Because every technique in the chain — including the pattern generator —
runs inside ReShade's own runtime on GPU 1, **our C++ renders nothing**.
It creates a device, a window, and a swapchain; then each frame it asks
the runtime to render its chain into the backbuffer and presents. That
is the entire frame loop, and it is why P0 is small.

05 · Layout

**Code structure**

Exactly these files. Nothing for transit, nothing for heaps, nothing for
fences — those files do not exist yet, and their absence is part of the
containment.

**mgpu-bridge/** — created fresh, PRIVATE, never a fork

├─ **.github/workflows/**

│ └─ build.yml → containment check, fetch ReShade @SHA, build, upload

├─ .gitignore → ext/, build/

├─ CMakeLists.txt → x64, C++17, output mgpu_bridge.addon64

├─ README.md → P0 scope, deploy steps, how to read the log

├─ LICENSE → proprietary, all rights reserved

├─ THIRD_PARTY.md → ReShade BSD-3; LumeniteFX not-contained note

├─ VENDOR_LOCK.md → pinned SHA, LumeniteFX release, driver, ReBAR

├─ **assets/** — deployed by hand, never compiled

│ ├─ pattern.fx → ours: animated known-motion test pattern

│ ├─ mv_debug.fx → ours: flow field → visible color

│ └─ gpu1.ini → preset, in order: pattern, Kernel, LumaFlow, mv_debug

└─ **src/**

├─ dllmain.cpp → register_addon, event wiring, teardown

├─ diag.hpp/.cpp → structured log lines, one per decision

├─ adapter.hpp/.cpp → EnumAdapters1, LUID select, caps logging

├─ gpu1_context.hpp/.cpp → device, window, swapchain on adapter 1

├─ runtime_probe.hpp/.cpp → init_effect_runtime, LUID match, preset
wiring

└─ worker.hpp/.cpp → bridge thread, message pump, frame loop, present

ext/reshade/ — created by CI at build time, gitignored, never committed

**Threading**

Two threads, and the separation is structural — it is the shape the
transit milestone will need, established now while it is trivial.

- **GAME THREAD** Runs ReShade's callbacks. At P0 it does almost
  nothing: register events, spawn the worker, log. It must never block,
  and it never touches GPU 1 objects.

- **BRIDGE THREAD** Owns the adapter-1 device, the window, and the
  swapchain, and runs the frame loop. Everything GPU 1 is created, used,
  and destroyed here.

**Gotcha — message pump**

The window's HWND must be created *and pumped* on the bridge thread. A
DXGI swapchain whose window has no message loop on its owning thread
will hang presentation, and it presents as a sync deadlock. Create
window → start pumping → create swapchain, all on the worker.

**Gotcha — re-entrancy**

Create the adapter-1 swapchain from the worker at add-on init, never
from inside a ReShade frame callback. Creating a swapchain through the
DXGI factory ReShade has hooked, from inside a ReShade callback,
recurses into its own hooks.

06 · Execution

**Agent task list**

Ordered. Each task ends in something observable — a log line or a
visible window — so a failure localises to one task instead of to "P0".

**T1Repo scaffold and a loading add-on**toolchain derisk

Every root file, plus a dllmain.cpp that registers the add-on, writes
one log line, and does nothing else. No D3D12 yet.

**Verify first:** read crosire/reshade on GitHub and confirm the
exported add-on name/description symbol convention against the pinned
SHA before writing dllmain.cpp.

**Acceptance**CI green, mgpu_bridge.addon64 downloadable as a workflow
artifact, and the game's ReShade log shows the init line.

**T2Adapter enumeration and selection**

IDXGIFactory4::EnumAdapters1; log every adapter with LUID, description,
and VRAM. Select adapter 1 **by LUID, not index** — index ordering is
not stable across driver restarts, and a silent bind back to adapter 0
produces a "working" result that proves nothing.

**Acceptance**Log contains the full adapter table and one line naming
the selected LUID.

**T3Private D3D12 device on adapter 1**

D3D12CreateDevice against the selected adapter. Log the HRESULT and, for
the record, CrossAdapterRowMajorTextureSupported — read and logged only,
used by nothing at P0.

**Acceptance**Device created, game still renders normally, no
device-removal event in the log.

**T4Bridge thread, window, message pump**

Spawn the worker at add-on init. It creates a visible ~640×360 window,
runs a message loop, and shuts down cleanly when the game exits or the
add-on unloads.

**Acceptance**An empty window appears beside the game, stays responsive,
and closes without hanging the game on exit.

**T5Swapchain and a present loop**

Create the swapchain on the adapter-1 device against that window,
through the hooked DXGI factory. Each frame: clear to a slowly cycling
colour and Present. This is the last task before the real unknown, and
it isolates "presentation works on GPU 1" from "ReShade hooked us".

**Acceptance**The window shows a smoothly animating colour while the
game runs on the other card.

**T6Catch the second effect runtime**the real gate

Register reshade::addon_event::init_effect_runtime,
signature void(reshade::api::effect_runtime \*). In the callback
take runtime-\>get_device()-\>get_native(), cast to ID3D12Device\*,
compare GetAdapterLuid() against the selected LUID, and store the match.
Log *every* runtime init with its LUID, matched or not.

**Acceptance**The callback fires twice per session and exactly one
carries adapter 1's LUID.

If it fires only once, ReShade filtered our swapchain. Before concluding
anything, vary the window — visible vs. hidden, size, style, and
creation order relative to the game's own swapchain init. A filter, if
it exists, is far more likely a window or lifecycle condition than a
device one. Report findings; do not start patching ReShade.

**T7Drive LumeniteFX on GPU 1**

Point the matched runtime at gpu1.ini. Replace the clear-and-present
loop with: render the runtime's effect chain into the backbuffer, then
present.

**Verify at the header first** — these three are read off the pinned
tree, not from this document:

- How to obtain a command_list for the runtime —
  likely runtime-\>get_command_queue()-\>get_immediate_command_list().

- How to obtain the backbuffer RTV — whether effect_runtime exposes it
  directly, or whether it comes from the swapchain object carried
  by init_swapchain.

- Whether render_effects(cmd_list, rtv, rtv_srgb) for the whole chain is
  right here, or whether per-technique render_technique calls in preset
  order are cleaner. Prefer the chain call; fall back to per-technique
  with find_technique.

**Acceptance**The window shows the scrolling test pattern transformed
into a coherent motion-vector field, updating live, while the game runs
on GPU 0. Ground truth holds: a pattern scrolling 2 px/frame reads as
2 px of flow.

**T8Teardown and the run report**

Clean shutdown on add-on unload: stop the worker, destroy the swapchain,
window, and device in order, unregister events. Then write the P0 run
report into VENDOR_LOCK.md — driver version, ReBAR state, LumeniteFX
release, ReShade SHA, and each task's pass/fail.

**Acceptance**Game exits cleanly with no hang and no crash on repeated
load/unload.

07 · Containment

**Keeping P0 to P0**

Instructions alone don't contain an agent that understands the roadmap.
These are mechanical.

**Forbidden symbols — enforced in CI**

Every one of these belongs to a later milestone. Their presence
in src/ or assets/ means scope drift, and the build fails rather than
quietly succeeding:

\- name: P0 containment check

shell: bash

run: \|

if grep -rnE
'SHARED_CROSS_ADAPTER\|HEAP_FLAG_SHARED\|CreateSharedHandle\|OpenSharedHandle\|FENCE_FLAG_SHARED\|reshade_finish_effects\|COMMAND_LIST_TYPE_COPY\|NVSDK_NGX\|nvngx\|GetClockCalibration'
src/ assets/; then

echo "::error::Out-of-scope symbol for P0 — see brief section 01"

exit 1

fi

Put this step *before* the build so it fails fast and cheaply. It is
also self-documenting: an agent reading the workflow learns the boundary
without being told twice.

**File manifest is closed**

The tree in section 05 is exhaustive.
Creating transit.cpp, xadapter.cpp, or any file not listed is out of
scope regardless of how good the code is. New files need a human
decision.

**Stop-and-report conditions**

Four situations end the agent's turn with a written report rather than a
workaround:

- **T6 fires only once.** Report the window variations tried and their
  results. Do not modify ReShade.

- **Any header detail contradicts this brief.** Report the actual
  signature; do not silently redesign around it.

- **Device creation on adapter 1 fails.** Report the HRESULT and the
  adapter table. Do not fall back to adapter 0 — a silent fallback is
  the single most expensive failure available here, because everything
  downstream then "works" and proves nothing.

- **Something needs a file outside the manifest.** Report what and why.

08 · Instrumentation

**Reading the result**

No debugger, no local build. The ReShade log and the debug window are
the only two instruments, so both are designed to make failures name
themselves.

Every line carries a fixed prefix and a task id — \[MGPU\]\[T3\]
D3D12CreateDevice hr=0x... luid=... — so the whole run greps out of a
long ReShade log in one pass. Log the *inputs* to each decision, not
just the outcome: the adapter table, not just the choice; both LUIDs at
the comparison, not just whether they matched.

| **What you see**                              | **What it means**                                                                                                             |
|-----------------------------------------------|-------------------------------------------------------------------------------------------------------------------------------|
| **Coherent flow field tracking the pattern**  | P0 passes. The architecture holds.                                                                                            |
| **Pattern visible, flow field black**         | LumaFlow ran but found nothing, or Kernel isn't feeding it. Check preset order in gpu1.ini.                                   |
| **Flow magnitude wrong against known motion** | LumaFlow is working but mis-scaled — a real finding, and one only the synthetic pattern can surface.                          |
| **Window black, runtime matched in log**      | Preset didn't load, or the technique handles are stale. Log what the lookup returned.                                         |
| **Window frozen after N frames**              | Message pump, not rendering. Check the window's owning thread before anything else.                                           |
| **Only one runtime init in the log**          | ReShade filtered our swapchain. T6's stop-and-report condition.                                                               |
| **Game crashes or hitches**                   | Something ran on the game thread that shouldn't. P0 touches no game resource, so this is a threading bug, not a graphics one. |

**Design decision — does the runtime tick without present?**

render_effects is documented as preventing "the usual rendering of
effects before swap chain presentation," which implies present normally
drives the chain. LumaFlow is *temporal* — it needs coherent
frame-to-frame history and timing uniforms. Present the debug swapchain
every frame rather than discovering this as motion-vector garbage that
looks like a shader bug.

**Writing for a CI-only loop**

- **Never crash the game.** A crash yields no log, which costs a full
  round trip and teaches nothing. Every D3D call HRESULT-checked; every
  task independently skippable; a failed T3 must still let T4's
  diagnostics report.

- **One binary, all tasks.** A single launch should exercise everything
  built so far and log it. Splitting into separate builds doubles the
  slowest step in the loop.

- **Optimise for compile-first-try.** C++17, Windows SDK, no exotic
  dependencies, warnings-as-errors off at P0. A compile error costs the
  same round trip as a logic error and teaches less.

- **Whole files, not patches.** Each source file independently
  replaceable — the layout already gives this.

P0 exits when T1–T8 pass and VENDOR_LOCK.md carries the SHA, driver
version, ReBAR state, and LumeniteFX release that made them pass. Only
then does the cross-adapter heap enter the picture: P1 adds transit from
GPU 0, P2 widens it to the full ring buffer, M2 adds the
resolution-scale knob and the sweep for T_fixed, and M3 introduces
DLSS-NR against an already-characterised transport.
