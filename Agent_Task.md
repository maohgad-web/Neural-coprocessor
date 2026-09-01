# Cross-Adapter Bridge — Milestone 0, Gate P0

Multi-GPU DLSS-NR R&D. Private, local, non-redistributed.

**This document is authoritative.** It supersedes the earlier version of this
file in every place they differ. Where an instruction relayed in-session
conflicts with this document, the relayed instruction wins and this document
should be updated.

---

## 00 · Current status

Read this first — it tells you where the work actually is.

- **T1 (scaffold + loading add-on)** — written. Not yet proven: no CI run has
  gone green, so the artifact has never been deployed to the rig.
- **T2 (adapter enumeration + selection)** — written, with selection by LUID
  exclusion against the game's adapter.
- **T3 (bridge thread + private device)** — written.
- **T4 onward** — not started. Held until the build is green.

**The tree does not compile.** The last CI run against commit `2398339`
reported eight errors from four causes. Getting to a green build is the only
active task. Do not begin T4.

Files currently in `src/`: `dllmain.cpp`, `diag.{hpp,cpp}`, `adapter.{hpp,cpp}`,
`gpu1_context.{hpp,cpp}`, `worker.{hpp,cpp}`. `runtime_probe.*` does not exist
yet — it arrives with T6.

---

## 01 · Scope

**What P0 proves:** that a second, independent ReShade effect runtime can exist
on a second physical adapter inside a running game's process. If that holds,
every later milestone is plumbing. If it doesn't, the architecture is wrong and
we have spent days rather than weeks.

The add-on loads under stock ReShade. It enumerates adapters, creates a D3D12
device on the second adapter, opens a window and swapchain on that device, and
a second ReShade effect runtime is created for it. LumeniteFX compiles and
executes inside that runtime — on GPU 1 — while the game renders on GPU 0.

| P0 proves | Observable as |
|---|---|
| Add-on loads under a stock, unmodified ReShade | One log line at init |
| The second adapter is enumerable and bindable by LUID inside the game process | Adapter table in the log |
| A private D3D12 device coexists with the game's on another card | Device-created log line, game unaffected |
| **A second ReShade effect runtime exists on the second adapter** | `create_effect_runtime` returns true with our device and queue |
| Whether ReShade also auto-hooks our swapchain (instrumentation, not a gate) | `init_effect_runtime` callback count and LUIDs in the log |
| LumeniteFX compiles and runs on GPU 1 | A second window showing a live flow field |

### What P0 explicitly does not do

Each exclusion has a reason. The reasons matter — understanding them is what
prevents drift toward them.

- **No cross-adapter transfer.** "Can a second runtime exist on another GPU" and
  "can we move frames between GPUs" are two independent hard problems. Bundled,
  a failure is ambiguous. Separated, each failure names itself.
- **No capture from GPU 0.** `reshade_finish_effects` belongs to the transit
  milestone. P0 never touches the game's buffers, which is also why P0 cannot
  break the game's rendering.
- **No DLSS-NR, no NGX, no nvngx.** The neural layer consumes wrong input
  silently. It goes nowhere near a bridge that is not yet visually validated.
- **No timing, no timestamp queries, no numbers.** Cross-adapter timestamps need
  `GetClockCalibration` to correlate two unrelated clock domains. Any
  millisecond figure produced at P0 would be wrong and would get quoted later.
- **No modification to ReShade or LumeniteFX.** Stock ReShade is the whole point
  of the approach. LumeniteFX is used as shipped.

### The synthetic input follows from the scope

With no transit, there is no game content on GPU 1 — so LumaFlow needs something
to find motion in. P0 generates an animated test pattern **as the first technique
in the GPU 1 preset**. This is not a synthetic rig standing in for the pipeline:
the pipeline is real, in-process, under stock ReShade, on the real second card.
Only the input is synthetic, and only until transit exists.

It buys something a game frame cannot: **known ground truth.** Scroll the pattern
2 px/frame and the flow field must read 2 px. That validates LumaFlow itself.

---

## 02 · Working rules

**You cannot see CI.** Compilation happens in GitHub Actions on `windows-latest`
with MSVC. A human relays compiler output to you. Optimise for compiling on the
first try: C++17, Windows SDK, no exotic dependencies, warnings not fatal.

**`.github/workflows/` is human-owned.** Never write there. The containment check
lives in that file, so write access to it would be write access to your own
containment. If a workflow change is needed, output the complete file and hand it
back for a human to commit.

**Read declarations before using symbols.** Before using any Win32, D3D12, or
ReShade symbol you have not personally read this session, read its declaration
from `d3d12.h` in the Windows SDK or from the pinned ReShade tree. Several round
trips have been lost to symbols recalled from memory that do not exist. Section
09 records the ones already verified — trust that section, verify anything else.

**Never crash the game.** A crash yields no log, which costs a full round trip
and teaches nothing. HRESULT-check every D3D call. Every task must be
independently skippable: a failed T3 must still let T4's diagnostics report.

**One binary, all tasks.** A single launch should exercise everything built so
far and log it. Splitting into separate builds doubles the slowest step.

**Whole files, not patches.** Each source file is independently replaceable.

**Report what you observe, not what you intended.** After committing, read the
files back and report what is actually in the tree, distinguishing "written" from
"verified present." A silent write failure must surface immediately rather than
in the next round's archaeology. Sessions have been cut off mid-task before —
when resuming, always check what actually landed before writing.

---

## 03 · Dependencies

Nothing is forked. One private repo, created fresh, holding only our own code.

| Repository | Role at P0 | Treatment |
|---|---|---|
| **crosire/reshade** | Add-on headers. Pinned at **v6.8.0**, SHA `18deaa52de0c425a78b329e9cb3c497281cd00ec`, `RESHADE_API_VERSION 20`. Licensed `BSD-3-Clause OR MIT`; we elect BSD-3. | Fetched by CI at the pinned SHA. Not forked, not vendored, not a submodule. |
| **umar-afzaal/LumeniteFX** | Kernel + LumaFlow, run as shipped by the GPU 1 runtime. | **Never enters the repo.** ReShade compiles `.fx` at runtime, so it is a runtime asset, not a build input — installed by hand on the rig. |
| **clshortfuse/renodx** | Read-only reference for private-device instantiation patterns. | Read on GitHub. Not a dependency. |

**Do not fork LumeniteFX on GitHub.** Per GitHub's docs: *"All forks of public
repositories are public. You cannot change the visibility of a fork."* A fork
would be permanently public, and pushing a modified shader to it is public
redistribution of a modified work — precisely what its AGNYA license withholds
without the author's permission. Should LumaFlow ever need modifying, it happens
in a private repo, never a fork.

### On the rig, by hand

- An **add-on-enabled ReShade build** for the target game. The add-on will not
  load under an add-on-disabled build, and that failure looks identical to a
  broken add-on — confirm a trivial add-on loads first.
- LumeniteFX, unmodified, in the shader path, at a recorded release.
- Our `pattern.fx`, `mv_debug.fx`, and `gpu1.ini` beside it.
- ReBAR in the state recorded in `VENDOR_LOCK.md`.

---

## 04 · Mechanism

### Two ways to get the second runtime — we use the explicit one

ReShade 6.8's public header exposes a first-class entry point for exactly this
case:

```cpp
inline bool create_effect_runtime(reshade::api::device_api api, void *device,
    void *command_queue, void *swapchain, const char *config_path,
    reshade::api::effect_runtime **out_runtime);
```

**This is the primary mechanism.** It is deterministic — no dependence on whether
ReShade's DXGI hook chooses to adopt our swapchain — and `config_path` takes
`gpu1.ini` directly, which removes the whole class of "the preset didn't load"
failures. Drive it with `update_and_present_effect_runtime`, which is legal
precisely because we created the runtime; the header forbids that call only on
runtimes ReShade auto-created.

The **auto-hook path** — ReShade noticing our swapchain and building a runtime
unprompted — stays in P0 as **instrumentation only**. Registering
`init_effect_runtime` and logging every runtime with its LUID costs one callback
and zero risk, and tells us whether ReShade adopts our swapchain. Worth knowing
for the roadmap; not worth gating P0 on.

Explicit creation is also the better foundation for transit: it hands us the
frame cadence rather than leaving the chain to fire on ReShade's own present
path.

```
GPU 0 · ADAPTER 0                    GPU 1 · ADAPTER 1
DX12 game render                     our device + window + swapchain
ReShade runtime #1 (game)            ReShade runtime #2 (ours, explicit)
untouched by P0                      pattern -> Kernel -> LumaFlow -> mv_debug
  no capture, no hooks,              Present -> debug window
  nothing crosses over
                    same process · same ReShade · no data path
```

Because every technique in the chain — including the pattern generator — runs
inside ReShade's own runtime on GPU 1, **our C++ renders nothing.** It creates a
device, a window, and a swapchain; then each frame it asks the runtime to render
its chain into the backbuffer and presents. That is the entire frame loop, and it
is why P0 is small.

---

## 05 · Layout

Repo `maohgad-web/MAGUMBOS`, private. Files live at the **repo root** — there is
no `mgpu-bridge/` subdirectory.

```
MAGUMBOS/
├─ .github/workflows/
│   └─ build.yml           HUMAN-OWNED — never write here
├─ .gitignore              ext/, build/, _dl/
├─ CMakeLists.txt          x64, C++17, output mgpu_bridge.addon64
├─ Agent_Task.md           this document
├─ README.md               P0 scope, deploy steps, how to read the log
├─ LICENSE                 proprietary, all rights reserved
├─ THIRD_PARTY.md          ReShade BSD-3; LumeniteFX not-contained note
├─ VENDOR_LOCK.md          pinned SHA, LumeniteFX release, driver, ReBAR
├─ assets/                 deployed by hand, never compiled
│   ├─ pattern.fx          ours: animated known-motion test pattern
│   ├─ mv_debug.fx         ours: flow field -> visible colour
│   └─ gpu1.ini            preset order: pattern, Kernel, LumaFlow, mv_debug
└─ src/
    ├─ dllmain.cpp         register_addon, event wiring, teardown
    ├─ diag.{hpp,cpp}      structured log lines, one per decision
    ├─ adapter.{hpp,cpp}   EnumAdapters1, LUID selection, caps logging
    ├─ gpu1_context.{hpp,cpp}  private D3D12 device on the selected adapter
    ├─ runtime_probe.{hpp,cpp} T6 — not yet created
    └─ worker.{hpp,cpp}    bridge thread, message pump, frame loop, present

ext/reshade/   created by CI at build time, gitignored, never committed
```

The output filename is `mgpu_bridge.addon64`. **The extension replaces `.dll`,
it does not append** — a file named `mgpu_bridge.addon64.dll` goes green in CI
and silently never loads on the rig. CMake achieves this with `PREFIX ""`,
`SUFFIX ".addon64"`, and `RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}>"`
(the generator-expression wrapper suppresses the per-config subdirectory that
multi-config generators would otherwise append).

### Threading

Two threads. The separation is structural — it is the shape the transit
milestone will need, established now while it is trivial.

- **Game thread.** Runs ReShade's callbacks. At P0 it does almost nothing:
  register events, enumerate adapters, spawn the worker, log. It must never
  block, and it never touches GPU 1 objects.
- **Bridge thread.** Owns the adapter-1 device, the window, and the swapchain,
  and runs the frame loop. Everything GPU 1 is created, used, and destroyed here.

**Gotcha — message pump.** The window's HWND must be created *and pumped* on the
bridge thread. A DXGI swapchain whose window has no message loop on its owning
thread will hang presentation, and it presents as a sync deadlock. Create window,
start pumping, then create the swapchain — all on the worker.

**Gotcha — re-entrancy.** Never create a DXGI factory, device, or swapchain from
inside `DllMain`: add-ons load from within the game's `CreateDXGIFactory1` call,
so DXGI work there recurses into ReShade's own factory hook. Do it from the first
`init_device` / `init_swapchain` callback, or from the bridge thread.

**Gotcha — never join from `DllMain`.** `DLL_PROCESS_DETACH` runs under the
loader lock, and a thread cannot finish exiting without that lock (its exit
dispatches `DLL_THREAD_DETACH` to every loaded module). A join deadlocks; a
timed-out join leaves a live thread pointing into a DLL that is about to unmap.
Signal and return. Branch on a named `lpReserved`: non-NULL means process exit
(all other threads are already terminated, nothing to do); NULL means dynamic
`FreeLibrary` unload. Leaking the GPU 1 device on dynamic unload is explicitly in
scope at P0. Deadlocking is not.

---

## 06 · Task list

Ordered. Each task ends in something observable — a log line or a visible window
— so a failure localises to one task rather than to "P0".

### T1 — Repo scaffold and a loading add-on

Every root file, plus a `dllmain.cpp` that registers the add-on, writes one log
line, and does nothing else. No D3D12.

`reshade::log::message` only works once `register_addon` has succeeded, so a
failed registration has no ReShade log channel. Returning `TRUE` quietly
collapses three distinct failures into one symptom — add-on not found, add-on
loaded but registration refused, add-on registered but not logging all look
identical. On failure, emit one `OutputDebugStringA` line and return `TRUE`. It
is DllMain-safe and readable in DebugView. Never `return FALSE` — that fails the
DLL load itself.

**Acceptance:** CI green, the workflow artifact is named exactly
`mgpu_bridge.addon64`, and the game's ReShade log shows the init line.

### T2 — Adapter enumeration and selection

`IDXGIFactory4::EnumAdapters1`; log every adapter with LUID, description, VRAM
and active output count. Bind **by LUID, not index** — index ordering is not
stable across driver restarts.

**The selection rule is exclusion, not a heuristic.** The game's own adapter LUID
is available directly: `init_device` hands us the game's `reshade::api::device`,
and its LUID comes from `get_native()` cast to `ID3D12Device *` then
`GetAdapterLuid()`, or from `device_properties::adapter_luid`. Select the adapter
whose LUID differs from the game device's. With two adapters that is
deterministic and correct regardless of display topology.

**Do not select by output count.** "The headless adapter is the target" looks
robust and is not: this project's own target topology puts the monitor on GPU 1,
and any rig with displays on both cards breaks it silently — selecting the game's
own adapter, the one failure that makes everything downstream appear to work
while proving nothing. Log output counts; use them only as a tiebreaker when
more than two adapters are present.

Zero candidates means a single-adapter rig: refuse to select, log loudly, and do
not bind the game's card. The one-shot guard must be atomic — `init_device` fires
for every device created process-wide with no thread guarantee.

**Acceptance:** the full adapter table, the game's adapter LUID, and one line
naming the selected LUID with the exclusion that produced it.

### T3 — Bridge thread, and the device on it

Spawn the bridge thread here. `D3D12CreateDevice` against the selected adapter
**runs on that thread** — creating it inside the `init_device` callback is the
same re-entrancy class T2 avoids. T2's callback hands the selected LUID to the
thread; the thread does the creating.

After creation, verify both directions: compare the new device's
`GetAdapterLuid()` against the selected LUID (mismatch means release and stop)
and against the game's LUID (equal means log FATAL, release, stop). Both
directions of a silent re-bind must be dead.

Log the HRESULT and, for the record, `CrossAdapterRowMajorTextureSupported` —
read and logged only, used by nothing at P0.

**There is no device-removal event.** ReShade 6.8's `addon_event` enum has 80
values and contains no `device_removed` or `device_restored`. Device loss is a
D3D12 concern: replace the thread's infinite idle wait with a timed wait and poll
`ID3D12Device::GetDeviceRemovedReason()`, logging only when it is not `S_OK`.

The window and message pump stay in T4 — T3's thread does nothing else yet.

**Acceptance:** device created on the bridge thread, game still renders normally,
`GetDeviceRemovedReason()` stays `S_OK`, clean thread shutdown on unload.

### T4 — Window and message pump

The bridge thread creates a visible ~640×360 window, runs a message loop, and
shuts down cleanly when the game exits or the add-on unloads.

**Acceptance:** an empty window appears beside the game, stays responsive, and
closes without hanging the game on exit.

### T5 — Swapchain and a present loop

Create the swapchain on the adapter-1 device against that window. Each frame:
clear to a slowly cycling colour and `Present`. This isolates "presentation works
on GPU 1" from "the runtime was created".

**Acceptance:** the window shows a smoothly animating colour while the game runs
on the other card.

### T6 — Create the second effect runtime

**Primary — explicit creation.** Call `reshade::create_effect_runtime(device_api::d3d12,
our_device, our_queue, our_swapchain, "gpu1.ini", &runtime)` from the bridge
thread once the swapchain exists. Log the boolean result and the pointer.

**Alongside — auto-hook instrumentation.** Register `init_effect_runtime`,
signature `void (reshade::api::effect_runtime *)`, and log every runtime init
with its adapter LUID via both `get_native()` and the `device_properties::adapter_luid`
cross-check. Add `init_swapchain` and `init_device` logging too — `init_device`
fires on every `D3D12CreateDevice` process-wide, so it independently confirms
T3's binding.

These do not conflict. The runtime we create is the one we drive; the callback is
a read-only observation of what ReShade did on its own.

**Acceptance:** `create_effect_runtime` returns true and yields a runtime whose
device carries the selected LUID. The auto-hook log is recorded either way and
gates nothing.

If explicit creation fails, report the arguments and the result. Do not fall back
to an auto-hooked runtime silently, and do not patch ReShade. If it succeeds but
the auto-hook callback never fires for our swapchain, that is a roadmap finding,
not a failure.

### T7 — Drive LumeniteFX on GPU 1

Replace the clear-and-present loop with the runtime drive. The preset arrived
through `create_effect_runtime`'s `config_path`, so there is no separate
preset-loading step.

- Command list: `runtime->get_command_queue()->get_immediate_command_list()`.
  `flush_immediate_command_list()` and `wait_idle()` are also available.
- Back buffer: `effect_runtime::get_current_back_buffer()` returns an
  `api::resource`; make the RTV with `device->create_resource_view(resource,
  resource_usage::render_target, desc, &out)`.
- Drive: `update_and_present_effect_runtime(runtime)` is the intended driver for
  a runtime we created, and it handles the present. Use
  `render_effects(cmd_list, rtv, rtv_srgb)` only if explicit control over the
  target is needed; `render_technique` + `find_technique` remain the
  per-technique fallback.

Technique handles from `find_technique` go stale on the `reshade_reloaded_effects`
event. If the per-technique fallback is used, register that event and re-query
rather than caching across reloads — otherwise a shader edit on the rig produces
a black window that looks like a bridge failure.

LumaFlow is temporal — it needs coherent frame-to-frame history and timing
uniforms. Present the debug swapchain every frame rather than discovering this as
motion-vector garbage that looks like a shader bug.

**Acceptance:** the window shows the scrolling test pattern transformed into a
coherent motion-vector field, updating live, while the game runs on GPU 0. Ground
truth holds: a pattern scrolling 2 px/frame reads as 2 px of flow.

### T8 — Teardown and the run report

Clean shutdown on add-on unload: stop the worker, destroy the swapchain, window
and device in order, unregister events — observing the never-join rule in section
05. Then write the P0 run report into `VENDOR_LOCK.md`: driver version, ReBAR
state, LumeniteFX release, ReShade SHA, and each task's pass/fail.

**Acceptance:** game exits cleanly with no hang and no crash on repeated
load/unload.

---

## 07 · Containment

Instructions alone do not contain an agent that understands the roadmap. These
are mechanical.

### Forbidden symbols — enforced in CI

Every one belongs to a later milestone. Their presence in `src/` or `assets/`
means scope drift, and the build fails rather than quietly succeeding. This step
runs before the build, in the human-owned workflow file:

```bash
PATTERN='SHARED_CROSS_ADAPTER|HEAP_FLAG_SHARED|CreateSharedHandle|OpenSharedHandle|FENCE_FLAG_SHARED|reshade_finish_effects|COMMAND_LIST_TYPE_COPY|NVSDK_NGX|nvngx|GetClockCalibration'
FOUND=0
for DIR in src assets; do
  [ -d "$DIR" ] || continue
  if grep -rnE "$PATTERN" "$DIR"; then FOUND=1; fi
done
if [ "$FOUND" -ne 0 ]; then
  echo "::error::Out-of-scope symbol for P0"
  exit 1
fi
```

(`grep` exits 2 on a missing directory and 1 on no-match, and Actions runs bash
with `set -eo pipefail` — hence the explicit flag and the `continue` guard.)

### The file manifest is closed

The tree in section 05 is exhaustive. Creating `transit.cpp`, `xadapter.cpp`, or
any file not listed is out of scope regardless of how good the code is. New files
need a human decision.

### Stop-and-report conditions

Four situations end your turn with a written report rather than a workaround:

- **Explicit runtime creation fails at T6.** Report the arguments and result. Do
  not modify ReShade.
- **Any header detail contradicts this document.** Report the actual signature;
  do not silently redesign around it.
- **Device creation on the selected adapter fails.** Report the HRESULT and the
  adapter table. Do not fall back to the game's adapter — a silent fallback is
  the single most expensive failure available here, because everything downstream
  then "works" and proves nothing.
- **Something needs a file outside the manifest.** Report what and why.

---

## 08 · Instrumentation

No debugger, no local build. The ReShade log and the debug window are the only
two instruments, so both are designed to make failures name themselves.

Every line carries a fixed prefix and a task id — `[MGPU][T3] D3D12CreateDevice
hr=0x... luid=...` — so a whole run greps out of a long ReShade log in one pass.
Log the **inputs** to each decision, not just the outcome: the adapter table, not
just the choice; both LUIDs at the comparison, not just whether they matched.

| What you see | What it means |
|---|---|
| Coherent flow field tracking the pattern | P0 passes. The architecture holds. |
| Pattern visible, flow field black | LumaFlow ran but found nothing, or Kernel is not feeding it. Check preset order in `gpu1.ini`. |
| Flow magnitude wrong against known motion | LumaFlow works but is mis-scaled — a real finding, and one only the synthetic pattern can surface. |
| Window black, runtime created in log | Preset path wrong, or shaders failed to compile on GPU 1. Check `config_path` resolved, then ReShade's own effect-compile errors. |
| Window frozen after N frames | Message pump, not rendering. Check the window's owning thread first. |
| Add-on never loads, CI green | Almost always the output filename. The extension replaces `.dll`. |
| No auto-hook runtime init for our swapchain | ReShade did not adopt it. A roadmap finding; it does not block P0. |
| Game crashes or hitches | Something ran on the game thread that should not have. P0 touches no game resource, so this is a threading bug, not a graphics one. |

---

## 09 · Verified API facts

Each of these was read from a primary source — the pinned ReShade tree, the
Windows SDK, or Microsoft's documentation — after a guess proved wrong. Trust
this section. Verify anything not in it.

**ReShade, v6.8.0 / API 20**

- Add-ons export `NAME` and `DESCRIPTION` as `extern "C" __declspec(dllexport)
  const char *`. `DllMain` calls `reshade::register_addon(hModule)`.
- `api_object::get_native()` returns **`uint64_t`**, not a pointer. Converting it
  to `ID3D12Device *` requires `reinterpret_cast`; `static_cast` will not compile.
- `addon_event` has 80 values. It contains `init_device`, `create_device`,
  `destroy_device`, `init_swapchain`, `create_swapchain`, `destroy_swapchain`,
  `init_effect_runtime`, `destroy_effect_runtime`, and the command-list surface.
  It contains **no** `device_removed`, `device_restored`, `reshade_init`, or
  `reshade_unload`.
- `init_device` and `destroy_device` both have signature `void (api::device *)`.
  `init_effect_runtime` is `void (api::effect_runtime *)`. `init_swapchain` is
  `void (api::swapchain *, bool resize)`.
- `create_effect_runtime`, `update_and_present_effect_runtime` and
  `destroy_effect_runtime` exist. The latter two must **not** be called on
  runtimes ReShade created automatically.
- `render_effects(command_list *, resource_view rtv, resource_view rtv_srgb)` and
  `render_technique(effect_technique, command_list *, resource_view, resource_view)`
  both exist on `effect_runtime`.
- Headers are licensed `BSD-3-Clause OR MIT`.

**D3D12 / Win32**

- `ID3D12Device::GetAdapterLuid()` takes **no arguments** and returns `LUID` by
  value in the C++ binding.
- `CrossAdapterRowMajorTextureSupported` is a `BOOL` member of
  `D3D12_FEATURE_DATA_D3D12_OPTIONS`, queried with the `D3D12_FEATURE_D3D12_OPTIONS`
  enum. There is no `D3D12_FEATURE_CROSS_ADAPTER_ROW_MAJOR_TEXTURE_SUPPORT` enum,
  no matching `_DESC` struct, and no node mask on that struct.
- **`CreateThreadW` does not exist.** The API is `CreateThread` — it takes no
  strings, so there is no ANSI/Wide pair. `CreateEventW` does exist.
- `_beginthreadex` takes **six** parameters: `(void *security, unsigned stack_size,
  unsigned (__stdcall *start)(void *), void *arglist, unsigned initflag,
  unsigned *thrdaddr)`. It returns `uintptr_t` — converting to `HANDLE` requires
  `reinterpret_cast`. Its thread-id out-param is `unsigned *`, not `DWORD *`.
- Prefer `_beginthreadex` over `CreateThread` for threads using CRT facilities.

**Environment**

- GitHub's `windows-latest` runner ships **Visual Studio 2026 / MSVC 19.51**. Do
  not hardcode a generator name in CMake or scripts — `cmake -B build -A x64`
  selects the newest installed Visual Studio and survives image bumps.

---

P0 exits when T1–T8 pass and `VENDOR_LOCK.md` carries the SHA, driver version,
ReBAR state and LumeniteFX release that made them pass. Only then does the
cross-adapter heap enter the picture: P1 adds transit from GPU 0, P2 widens it to
the full ring buffer, M2 adds the resolution-scale knob and the sweep for
T_fixed, and M3 introduces DLSS-NR against an already-characterised transport.
