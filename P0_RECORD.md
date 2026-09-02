# Cross-Adapter Bridge — Milestone 0, Gate P0 · **CLOSED**

Multi-GPU DLSS-NR R&D. Private, local, non-redistributed.

> **If you are a coding agent: this is not your task list.**
> This document is a finished record of completed work. Every task in it passed
> and every file it describes is already committed. **Do not implement anything
> from it, do not edit it, and do not treat an item here as work to do.** Your
> assignment is in `Agent_Task.md` — and if that file says no task is assigned,
> then none is, and you should stop and say so rather than inferring one from
> here. Read sections 09 and 10 when you need a verified fact — that is what
> this document is for.

**This document is the P0 record. It is finished and frozen.** P0 passed on
2026-09-02. The conditions that made it pass are in `VENDOR_LOCK.md`; the
accumulated technical findings are in sections 09 and 10, which remain the
reference for every later milestone.

**The next milestone gets its own `Agent_Task.md`.** Do not extend this one. P1
should inherit only what it needs — the NGX contract, the cross-adapter format
constraints, the rig conditions by reference — not the adapter-selection rules or
the Win32 window facts, which describe finished work.

---

## Section numbering — for anyone following a source comment

Comments throughout `src/` cite this document by its former name
(`Agent_Task.md`) and by section numbers that no longer exist. Those references
are historical and the code they annotate is frozen; they were deliberately not
rewritten. Where they now point:

| Comment says | Now in |
|---|---|
| section 05 — layout / closed manifest | **01** · The shipped artifact |
| section 06 — task list, "four rules", "gotcha N" | **01** · invariants, and **09** for the underlying facts |
| section 07 — containment | `.github/workflows/build.yml`, which enforces it |
| section 08 — instrumentation | **01** · invariants; the log-line table lives in `README.md` |
| section 00 "exception N" | Exceptions were per-task freeze carve-outs. They expired when P0 closed; **01** states the invariants they protected. |
| section 09, section 10 | unchanged — same numbers |

## 00 · What P0 proved

A second, independent ReShade effect runtime can exist on a second physical
adapter inside a running game's process, compile a third-party shader, and
execute it — under stock, unmodified ReShade, while the game renders on the
first GPU and is measurably unaffected.

Verified in gameplay at 2560×1440: game frame rate 45.86 / 45.77 / 45.90 fps
across toggling a 15-pass effect on GPU 1, with no image change and no stutter.

| Task | Outcome |
|---|---|
| T1 — add-on loads under stock ReShade | passed |
| T2 — adapter enumeration and selection | passed |
| T3 — bridge thread and the GPU 1 device | passed |
| T4 — window and message pump | passed |
| T5 — swapchain and present loop | passed |
| T6 — second effect runtime | **satisfied without construction** |
| T7 — shader executes on GPU 1 | **satisfied by configuration** |
| T8 — teardown and run report | passed; the run report is `VENDOR_LOCK.md` |

**T6 and T7 were never built, and that is the headline result.** ReShade
auto-created an effect runtime for our GPU 1 swapchain the moment T5 created it,
wrote it its own config (`ReShade2.ini`), and compiled the entire shader library
on the second adapter. Enabling `Lumenite_QuantMotion` in a GPU 1 preset with
`DEBUG_FLOW=1` then executed it there. No `create_effect_runtime` call, no
rendering code, no driving loop — a preset file.

The proof that it executed: the bridge window turns **black**. The clear colour
is `0.5 + 0.5·sin(φ)` on three channels at 120° offsets, so the three always sum
to 1.5 and it is never dark. Black cannot be the clear showing through; it is
`PS_Debug` returning `MotionToColor` of a zero flow field, which is correct for a
spatially uniform input. Reproduced three times.

**What P0 did not do.** Nothing crossed between the adapters. Zero bytes. The
synthetic clear colour exists precisely because no game content reaches GPU 1,
and `CrossAdapterRowMajorTextureSupported=0` on this hardware means the simplest
sharing path is unavailable. Transit is P1's subject and is entirely unproven.

## 01 · The shipped artifact

`mgpu_bridge.addon64` — a ReShade 6.8.0 add-on (API 20), built only by GitHub
Actions, deployed beside `dxgi.dll`.

```
src/dllmain.cpp        registration, ReShade events, HMODULE capture
src/diag.{hpp,cpp}     the single logging sink: [MGPU][Tn] prefixes
src/adapter.{hpp,cpp}  enumeration and LUID-exclusion selection
src/worker.{hpp,cpp}   the bridge thread: window, pump, present loop, teardown
src/gpu1_context.*     the GPU 1 device and present chain — raw D3D12 pointers
                       never leave this translation unit
assets/gpu1.ini        the GPU 1 preset
```

`runtime_probe.*` was reserved for T6 and never needed.

**Invariants the code holds, and any successor must preserve:**

- A selection exists only from a **swapchain-derived** game LUID. UE5 creates a
  probe device on every adapter before settling, so `init_device` is provisional
  and never authorises a selection.
- Software adapters are excluded by `DXGI_ADAPTER_FLAG_SOFTWARE` **or** vendor
  `0x1414`. The flag alone is unreliable — this rig has two WARP adapters and
  only one sets it.
- Binding is by LUID, never by enumeration index.
- If filtering does not leave exactly one candidate, **select nothing**. A
  missing device is diagnosable; a device on the wrong adapter succeeds, logs
  cleanly, and proves nothing.
- The selection is a one-shot latched by `S.decided`. That latch is load-bearing:
  after teardown the adapter table holds released pointers, and the latch is what
  stops a later event dereferencing them.
- One bridge thread per launch, started **only** from `init_swapchain`. Starting
  it from `init_device` spawned a thread per probe cycle that could be executing
  inside the module when ReShade unmapped it, and killed the game process.
- Every GPU 1 object is created, used and destroyed on the bridge thread. The
  game thread never blocks and never touches one.
- `worker::stop()` is signal-only and never joins — safe under the loader lock.
- The `ID3D12Device *` and every present-chain object stay inside
  `gpu1_context.cpp`, behind its mutex.
- Teardown order is `gpu1::shutdown()` → `DestroyWindow` → `UnregisterClass` →
  `adapter::shutdown()`, on the bridge thread, before `rearm()`. The swapchain
  holds a reference to its window and must not outlive it, and the GPU is drained
  before anything is released.

## 02 · Working rules that earned their place

These were adopted mid-milestone, each after a specific failure. They are the
transferable part of P0.

**A green log is not a passed task.** Verify each acceptance item against the
code that implements it, not against the log line that would appear if it did. A
poll that was never written and a poll that finds nothing produce identical logs
— and this project shipped exactly that, undetected until the function was
rewritten for another reason.

**A milestone does not flip on a log alone.** After each rig run, review the
source of the files the next task will modify, and of any file whose acceptance
is about to be marked passed, *before* the brief is updated. That review found a
thread-handle leak, a use-after-free ordering bug, and six compile errors that CI
would have cost a round trip each.

**Check requirements against interfaces before writing them.** Every requirement
of the form "call X on Y" presumes something exposes Y. This failed twice — a
device-removal poll specified with no way to reach the device, and D3D12
signatures written from their apparent shape rather than the headers. Read the
headers of every module a task will call into, and write any missing accessor
into the spec.

**When a choice is reversible and cheap, make it and move on.** An agent that
cannot ask questions has no way to end an unresolved decision except by deciding
it. Two options that both compile and both behave correctly are not a question.

**Marking a task passed and compressing it are two separate acts.** Section 00
will happily claim the first while the second never happens. Measure the document
at each milestone.

## 03 · Environment

Recorded fully in `VENDOR_LOCK.md`. In brief: two RTX 5060 Ti on a 5600X with the
second card on chipset PCIe lanes (deliberate worst case), driver 616.56, ReShade
6.8.0.2155, ReBAR disabled per-program, Carnal Instinct v0.7.8 (UE5, D3D12) at
2560×1440 on a 210 Hz display.

The game renders on the adapter that **has the display attached**; the bridge
target is the **headless** one. UE5 loads and unloads the add-on **five times per
launch** while probing adapters.

## 04 · What P1 inherits

Read sections 09 and 10 before designing anything. The load-bearing items:

- **`CrossAdapterRowMajorTextureSupported = 0`** on this hardware. Transit must
  use a shared **buffer** with `GetCopyableFootprints` and
  `D3D12_PLACED_SUBRESOURCE_FOOTPRINT` at both ends. Rows pad to 256 bytes, so
  payload sizes come from the footprint, not from `width × height × bpp`.
- **The DLSS-NR path is public API and needs no effect runtime.**
  `Init_Ext` → private outputs → `CreateFeature(Reserved18)` → `EvaluateFeature`,
  with the driver's own `_nvngx.dll` as parameter provider. A working single-GPU
  run had no ReShade effects loaded at all.
- **Untested and gating everything:** whether NGX will initialise and create a
  feature on a headless, non-game adapter. That experiment costs almost nothing
  and should precede any transit work.
- **Motion vectors need not cross the bus.** QuantMotion derives flow from
  colour, costs 0.13–0.17 ms, and already runs on GPU 1. Its flow buffer is
  320×180 `RG16F`, **0.220 MiB** at 1440p.
- **No no-bridge baseline exists.** Every frame-rate figure was taken with the
  bridge running. The cost of the add-on's mere presence is unmeasured.

---

## 09 · Verified API facts

Each entry was read from a primary source — the pinned ReShade tree, the Windows
SDK, or Microsoft's documentation — after a guess proved wrong. Trust this
section. Verify anything not in it, and add what you verify.

**Where to verify, when you cannot search the web.** Every signature below was
confirmed against a primary source, and the same sources are readable as public
repositories. Use them rather than writing a call from its apparent shape — that
mistake cost six compile errors in one task, in code that read as correct:

**Note the branch names — they differ, and a wrong branch is a 404.**

| Need | Repo · branch | Path |
|---|---|---|
| Any D3D12 / DXGI signature, struct or enum | `microsoft/DirectX-Headers` · `main` | `include/directx/d3d12.h` — the authoritative declarations, not prose about them. Helper wrappers are `include/directx/d3dx12.h` and `include/directx/d3dx12_resource_helpers.h`. |
| ReShade add-on API | `crosire/reshade` · SHA in `VENDOR_LOCK.md` | `include/` — CI fetches exactly this tree, so what you read is what compiles. Do not read `main`; it drifts from the pin. |
| Cross-adapter transit, end to end | `microsoft/DirectX-Graphics-Samples` · `master` | `Samples/Desktop/D3D12HeterogeneousMultiadapter/src/` — a working two-adapter pipeline: shared heap, cross-adapter resource, shared fence, per-frame hand-off. The closest published thing to what the next milestone builds. |

**`Samples/Desktop/D3D12LinkedGpus` is not applicable and will mislead you.** It
is the *linked-node* case — one device with two nodes, addressed by node mask.
This rig has two separate adapters and two separate devices. Node masks are not
the mechanism here.

Read the header before writing the call. A signature you recall is not a
signature you verified.

**ReShade, v6.8.0 / API 20**

- Add-ons export `NAME` and `DESCRIPTION` as `extern "C" __declspec(dllexport)
  const char *`. `DllMain` calls `reshade::register_addon(hModule)`.
- `api_object::get_native()` returns **`uint64_t`**, not a pointer. Converting it
  to `ID3D12Device *` requires `reinterpret_cast`; `static_cast` will not compile.
- `addon_event` has 80 values, including `init_device`, `create_device`,
  `destroy_device`, `init_swapchain`, `create_swapchain`, `destroy_swapchain`,
  `init_effect_runtime`, `destroy_effect_runtime`. It contains **no**
  `device_removed`, `device_restored`, `reshade_init`, or `reshade_unload`.
- `init_device` and `destroy_device`: `void (api::device *)`.
  `init_effect_runtime`: `void (api::effect_runtime *)`. `init_swapchain`:
  `void (api::swapchain *, bool resize)`.
- `create_effect_runtime`, `update_and_present_effect_runtime` and
  `destroy_effect_runtime` exist. The latter two must **not** be called on
  runtimes ReShade created automatically.
- `render_effects(command_list *, resource_view rtv, resource_view rtv_srgb)` and
  `render_technique(effect_technique, command_list *, resource_view,
  resource_view)` both exist on `effect_runtime`.
- Headers are licensed `BSD-3-Clause OR MIT`.

**D3D12 / DXGI / Win32**

- `ID3D12Device::GetAdapterLuid()` takes **no arguments** and returns `LUID` by
  value in the C++ binding. It is not an HRESULT — do not wrap it in `SUCCEEDED`.
- `CrossAdapterRowMajorTextureSupported` is a `BOOL` member of
  `D3D12_FEATURE_DATA_D3D12_OPTIONS`, queried with `D3D12_FEATURE_D3D12_OPTIONS`.
  There is no `D3D12_FEATURE_CROSS_ADAPTER_ROW_MAJOR_TEXTURE_SUPPORT` enum, no
  matching `_DESC` struct, and no node mask on that struct.
- **On this rig it reads `0`.** RTX 5060 Ti, driver 616.56, `hr=S_OK`,
  `supported=0` — read from the real adapter after T2 was fixed. An earlier
  `supported=1` in the logs came from a software rasteriser and was void. This is
  a **P1 design constraint, not a P0 problem**: the transit milestone cannot
  share a row-major *texture* across adapters on this hardware. The path that
  remains is a cross-adapter shared **buffer** plus
  `GetCopyableFootprints` / `CopyTextureRegion` with a
  `D3D12_PLACED_SUBRESOURCE_FOOTPRINT` at each end. Budget for it when P1 is
  specified: footprint rows are padded to
  `D3D12_TEXTURE_DATA_PITCH_ALIGNMENT` (256 bytes), so the wire payload is
  slightly larger than `width × height × bpp` and the M2 bandwidth numbers must
  be computed from the footprint, not from the texture dimensions.
- `HRESULT ID3D12Device::GetDeviceRemovedReason()` — no parameters. Returns
  `S_OK` when healthy, otherwise the removal reason. **Sticky**: once removed it
  keeps returning the reason, so polling needs a one-shot transition guard.
  Microsoft also notes that device removal signals *all* fences to `UINT64_MAX`,
  making `ID3D12Fence::SetEventOnCompletion` the asynchronous alternative — not
  for P0, but the transit milestone will want it.
- `DXGI_ADAPTER_FLAG_SOFTWARE = 2`, tested against `DXGI_ADAPTER_DESC1::Flags`.
  The enum is `{ NONE = 0, REMOTE = 1, SOFTWARE = 2, FORCE_DWORD = 0xffffffff }`.
- **The software flag is not reliably set.** Observed on the rig, commit
  `de5d5c13`: two Microsoft adapters enumerated in the same pass, one with
  `flags=0x2` and one with `flags=0x0`, both `vendor=0x1414` and both
  `dedicated_vram=0MB`. Filtering on the flag alone left a phantom candidate and
  made the tiebreak ambiguous, so T2 refused. Treat `VendorId == 0x1414`
  (Microsoft — WARP, Basic Render Driver, virtual adapters) as software
  regardless of the flag. Do **not** substitute `DedicatedVideoMemory == 0` for
  either test: integrated GPUs legitimately report zero.
- `HRESULT IDXGIAdapter3::QueryVideoMemoryInfo(UINT NodeIndex,
  DXGI_MEMORY_SEGMENT_GROUP MemorySegmentGroup, DXGI_QUERY_VIDEO_MEMORY_INFO
  *pVideoMemoryInfo)`. Obtain `IDXGIAdapter3` by `QueryInterface` from
  `IDXGIAdapter1`. The struct has four `UINT64` members: `Budget`,
  `CurrentUsage`, `AvailableForReservation`, `CurrentReservation`.
  **`CurrentUsage` is the calling application's usage on that adapter, not the
  card's total** — queried from inside the game process it reads multi-GB on the
  adapter the game renders on and near-zero on the other. It will not match
  `nvidia-smi`'s system-wide figures, and near-zero is the signal that the game
  is not using that adapter.
- `DWORD MsgWaitForMultipleObjects(DWORD nCount, const HANDLE *pHandles, BOOL
  bWaitAll, DWORD dwMilliseconds, DWORD dwWakeMask)`. Returns
  `WAIT_OBJECT_0 + nCount` when input is available — that is *one past* the last
  handle index, not `WAIT_OBJECT_0`. Off-by-one here produces a loop that drains
  messages when the shutdown event fires and ignores input. Use `QS_ALLINPUT` as
  the wake mask. Requires `#include <windows.h>`; it is not in the DXGI headers.
- `BOOL AdjustWindowRect(LPRECT lpRect, DWORD dwStyle, BOOL bMenu)` converts a
  desired **client** rect to the outer window rect in place. Pass the same style
  bits given to `CreateWindowExW`, and `FALSE` for `bMenu`. `WS_VISIBLE` in the
  style is harmless here. Without this, a window created at 1280×720 has a client
  area roughly 1264×681 on default Windows metrics.
- **`strsafe.h` is inline by default — but do not use it here.** The `StringCch*`
  functions are declared `__inline` unless `STRSAFE_LIB` is defined before the
  include, so they need no extra link library. That question does not need
  answering, though: this codebase formats with `snprintf` throughout, and the
  wide equivalent is `swprintf_s(buf, ARRAYSIZE(buf), L"...", ...)` from the CRT.
  No new header, no new library, and consistent with every other log line in the
  tree. `wsprintfW` is the one to avoid — it lives in `user32`/`gdi32` and would
  add a link dependency.
- **No task may add a link library.** `CMakeLists.txt` is T1 code and closed. If
  a chosen API would require one, that is the signal to choose a different API,
  not to request an exception — the CRT and the already-linked Windows libraries
  cover everything P0 needs.
- `AdjustWindowRect` computes frame metrics at **system** DPI, not per-monitor
  DPI. In a per-monitor-DPI-aware process on a scaled display the resulting
  client area can be a few pixels off the requested size;
  `AdjustWindowRectExForDpi` is the exact form. T4 logs the real `GetClientRect`,
  so the rig log is the check.
- **`README.md` was stale for most of P0 and was regenerated at close.**
  ~~It still describes a ~640×360 window and a T7 pipeline of
  "pattern → Kernel → LumaFlow → mv_debug" with an `assets\mv_debug.fx` that the
  closed manifest does not contain.~~ **Superseded:** the README was rewritten
  when P0 closed and now matches the shipped code — 1280×720, `gpu1.ini`,
  `Lumenite_QuantMotion` with `DEBUG_FLOW=1`. Entry kept because the failure mode
  it names is general: no task owned the README, so it drifted for eight tasks
  without anyone noticing. Give the next milestone's documentation an owner.
- **The `FreeLibrary`-versus-teardown race kills the process. It is not a leak.**
  Observed on the rig: with the add-on present the game died silently during the
  third of UE5's add-on load/unload probe cycles, the log ending at
  `Unregistered add-on` with nothing after it; with the add-on renamed away the
  same launch succeeded. ReShade unmaps the module while the bridge thread is
  still executing inside it, and the thread faults on unmapped code — no log line
  is possible, because the code that would write it is gone. It is a race, not a
  determinism: the identical binary launched cleanly fifteen minutes earlier. The
  giveaway is the log ordering — in the surviving cycles ReShade's
  `was not unregistered` warning precedes `bridge thread exiting`; in the fatal
  one the bridge thread got ahead of it.
  **The cause is that a bridge thread exists at all during a probe cycle.**
  `on_init_device` calls `ensure_started()`, so every probe spawns a thread that
  waits on a ready event which cannot be set — by rule 2 no selection is possible
  before a swapchain — and is then torn down inside the unload window. Only
  `on_init_swapchain` should start the thread. Do **not** try to fix this by
  pinning the module with `GET_MODULE_HANDLE_EX_FLAG_PIN` — not because pinning
  fails (**correction:** RenoDX pins itself, logs
  `Pinned addon module to avoid unload` / `ReShade logical unload will be
  ignored`, and ReShade re-adopts it each cycle as an
  `externally registered add-on`, so pinning is demonstrably viable) but because
  starting the thread only from `init_swapchain` removes the thread from the
  unload window entirely rather than keeping code mapped around it.
- **The leak that remains after that fix is accepted.** Because
  `stop()` is signal-only and the bridge thread performs its own teardown, a
  dynamic unload that unmaps the module before the thread wakes leaks the GPU 1
  device. T4 extends the same best-effort model to the window and class; it does
  not introduce the race and must not try to close it. On the rig,
  `destroy_device` fires before ReShade unregisters the add-on, so the thread
  exits first.
- **ReShade auto-creates an effect runtime for our GPU 1 swapchain.** Observed on
  the rig the moment T5's swapchain existed: `Recreated runtime environment on
  runtime <ptr> ('ReShade2.ini')`, on the bridge thread, with a different runtime
  pointer and a different config file from the game's, immediately after our
  `CreateSwapChainForHwnd`. The full shader set — LumeniteFX included — then
  compiles **twice**, once per runtime, on disjoint thread sets about 34 ms
  apart. Teardown is symmetric: releasing our swapchain destroys that runtime.
  ReShade picks it up because its own `CreateSwapChainForHwnd` hook wraps our
  call, exactly as its `D3D12CreateDevice` hook has been wrapping our device
  since T3. **P0's central question is therefore answered before T6 begins**: a
  second effect runtime does exist on the second adapter, inside the game's
  process, under stock ReShade. What is *not* yet shown is that its effects
  execute — no technique is enabled yet, which is why the window shows a flat
  clear colour.
- **`ReShade2.ini` is a copy of the game's config, not a default.** That is why
  LumeniteFX compiled on GPU 1 with nothing configured:
  `EffectSearchPaths=.\reshade-shaders\Shaders\**` came across with it.
  **`PresetPath=.\ReShadePreset.ini` points at the same preset the game's
  runtime uses**, so enabling a technique there would enable it on *both*
  runtimes. T7 needs a preset of its own — which is what `gpu1.ini` in the closed
  manifest should be. Its role therefore changes: not the runtime's config file
  (ReShade already writes one), but the preset that config points at.
- **The GPU 1 runtime processes input and has an overlay.** `ReShade2.ini`
  carries `InputProcessing=2` and `KeyOverlay=36` (Home). Focusing the bridge
  window and pressing Home should open ReShade's own overlay *inside it*, giving
  interactive control of the second runtime — the technique list, toggles and
  preprocessor definitions — with no code at all. That is the cheapest possible
  test of whether effects execute on GPU 1.
- **The DLSS-NR path is public API and does not use ReShade's effect runtime.**
  From a working single-GPU run of `renodx-dlss.addon64` + `dlss5-feed.addon64`
  on this same rig: RenoDX vtable-hooks `NVSDK_NGX_D3D12_CreateFeature` and
  `NVSDK_NGX_D3D12_EvaluateFeature`, force-loads the driver's own
  `_nvngx.dll` parameter provider from the DriverStore, and NGX loads
  `nvngx_dlssnr.dll` itself. The sequence that succeeds is
  `Init_Ext` → private outputs (`2560×1440`, `format=24` =
  `R10G10B10A2_UNORM`, `flags=0x5` = RT|UAV) → `CreateFeature(Reserved18)` →
  `EvaluateFeature`. **`Reserved18` is the published enum name**, so the feature
  id needs no reverse engineering.
  **That run had no ReShade effects at all** — the shader search path failed to
  resolve, `DLSS5_Feed.fx` never loaded, `ReShadePreset.ini` was empty — and
  DLSS-NR still ran. So NR needs a device, queue, command list, fence and
  resources; it does **not** need an effect runtime. P0 already produces all of
  those on GPU 1.
  **The open question, and it gates everything:** whether
  `NVSDK_NGX_D3D12_Init_Ext` and `CreateFeature` succeed on a headless, non-game
  adapter. Untested. It should be tested before any transit work, because
  transit is pointless if the answer is no.
- **A LumeniteFX technique has executed on GPU 1. P0's hypothesis is
  demonstrated.** With `gpu1.ini` as the GPU 1 runtime's own preset,
  `LUMENITE: QuantMotion` enabled, and `DEBUG_FLOW` set to `1` in the overlay,
  the bridge window turned **black** — and stayed black across three independent
  repetitions, returning to the cycling colour whenever `DEBUG_FLOW` was reset to
  `0`. The log records the recompile on the bridge thread:
  `Successfully compiled '...lumenite_QuantMotion.fx' in 1.136000 s with
  warnings`, thread `17080`, the same thread that owns the GPU 1 objects.
  **Black cannot be the clear colour.** The three channels are
  `0.5 + 0.5·sin(φ)` at 120° offsets and always sum to 1.5, so the clear is never
  dark; black means something overwrote it. And black is the correct output of
  `PS_Debug` — `MotionToColor` maps magnitude to value, and a spatially uniform
  input yields zero flow everywhere. The brief already predicted a black first
  frame from `FRAME_COUNT == 0`, which also explains the momentary colour flash
  before it settles.
  **What this proves:** a second, independent ReShade effect runtime, on a second
  physical adapter, inside the running game's process, compiling and executing a
  third-party shader whose output reaches that adapter's swapchain — under stock
  ReShade, with its own preset, driven entirely by configuration.
  **What it does not prove:** that the flow field is *correct*. A uniform input
  can only ever produce zero flow. Accuracy needs `pattern.fx` and known motion.
- **Run T5RUNC: the GPU 1 effect costs the game nothing measurable.** Tested in
  actual gameplay at 2560×1440, not a menu. Game frame rate read **45.86 fps**
  with `DEBUG_FLOW=1`, **45.77** with it at `0`, **45.90** back at `1` — under a
  tenth of a frame across a 15-pass effect being toggled on the second adapter.
  The game's image did not change. The only frametime spike observed came at
  *reload*, not at the `0 → 1` swap, and a comparable spike occurs on alt-tab —
  consistent with a one-time compile cost rather than a per-frame tax. Provisional
  on this rig: a 5600X with the second card on chipset PCIe 3.0 ×16, deliberately
  the worst-case topology; an AM5 8×8 bifurcated CPU-lane system is the intended
  comparison point.
- **Measured: QuantMotion at 1440p costs 0.183 ms CPU / 0.342 ms GPU over 15
  passes** — about 1.6% of a 21.7 ms frame. Read from ReShade's own Statistics
  panel on the game's runtime.
- **Measured: the motion-vector buffer is small.** `V__QuantMotion__tFlow` is
  `320×180 RG16F`, **0.220 MiB**, pooled and referenced by 6 passes across 2
  effects. Against the M3 note's ~15 MB colour and ~14.7 MB depth estimates at
  1440p, flow is roughly **seventy times cheaper to move**. A pipeline that
  transits motion vectors rather than frames is a fundamentally different
  bandwidth problem, and this is the first hard number on it.
- **The canonical GPU 1 preset, written by ReShade itself:**
  ```
  Techniques=Lumenite_QuantMotion@lumenite_QuantMotion.fx
  TechniqueSorting=...
  [lumenite_QuantMotion.fx]
  PreprocessorDefinitions=DEBUG_FLOW=1
  ```
  Root-scope keys, then a per-effect section for preprocessor definitions. This is
  what `assets/gpu1.ini` should contain.
- **A ReShade preset is NOT shaped like a ReShade config.** `Techniques`,
  `TechniqueSorting` and `PreprocessorDefinitions` live at **root scope, with no
  section header**; the only sections in a preset are per-effect uniform blocks
  such as `[lumenite_QuantMotion.fx]`. Writing them under `[GENERAL]` — the shape
  `ReShade.ini` and `ReShade2.ini` use — produces
  `The selected file is not a valid preset!` in the overlay. **And an invalid
  `PresetPath` makes ReShade silently fall back to the default preset and rewrite
  `PresetPath` to match**, which reads exactly like an override by something else.
  Two rig cycles were spent blaming the wrong thing.
- **ReShade ships a built-in `Effect Runtime Sync` add-on**, enabled by default on
  this rig (`ReShade.ini` `[ADDON] DisabledAddons=` empty; `OverlayCollapsed`
  only folds its UI). It synchronises settings across effect runtimes in a
  process. It was *suspected* of forcing the GPU 1 runtime onto the game's
  preset, but the invalid-preset fallback above is a sufficient explanation on
  its own and has direct evidence, so this add-on's actual effect here is **not
  established**. Add-on enable state is process-global — `ReShade2.ini` has no
  `[ADDON]` section — so it cannot be disabled for one runtime only.
  Independently of that, it is worth noting a stock ReShade add-on is loaded in a
  process that now contains a second runtime on another adapter; whether it acts
  on that runtime is a question for the add-on-compatibility work, not a
  conclusion.
- **The GPU 1 overlay is fully interactive.** Focusing the bridge window and
  pressing Home opens ReShade's own overlay inside it — Home, Add-ons, Settings,
  Statistics, Log and the shader Editor — listing all fourteen compiled effects
  including `LUMENITE: QuantMotion`. Enabling a technique there triggers a
  recompile on that runtime. ReShade persists per-runtime ImGui layout into
  `ReShade2.ini` independently of the game's.
- **A technique enabled on GPU 1 with `DEBUG_FLOW=0` produces no visible change,
  and that is not a failure.** QuantMotion's default build outputs no flow
  visualisation, and a flat clear colour contains no motion to find. Only
  `DEBUG_FLOW=1` plus a moving input makes execution observable. Do not read an
  unchanged window as "the effect did not run".
- **`ShowWindow` does not report success or failure.** It returns nonzero if the
  window was **previously visible** and zero if it was **previously hidden**. A
  window created without `WS_VISIBLE` therefore returns `FALSE` from a perfectly
  successful `ShowWindow`, with `GetLastError() == 0`. Never treat its return as
  an error.
- **The bridge window covers the game. The game does not leave
  borderless-fullscreen.** A new top-level window enters at the top of the
  Z-order, so it sits over the game and the taskbar becomes visible — which reads
  as the game having gone windowed. It has not. Clicking the game raises it again
  and nothing was ever broken. `SW_SHOWNOACTIVATE` prevents *activation*, not
  Z-order placement, so it does not and cannot change this; it is still the right
  call, because taking foreground focus from the game is a separate and real
  problem that it does avoid.
  **Unverified, and flagged for M2 rather than asserted:** an occluding window
  can in principle push a borderless-fullscreen game from independent flip into
  DWM composition, which would change its present cost. Nothing here measured
  that. If M2 needs the game's present path undisturbed, the cheap answer is to
  put the bridge window on the other monitor or off-screen — but the effect
  should be measured before it is designed around.
- **D3D12 signatures that are not what they look like.** Every one of these was
  written wrong once, in code that read as correct:
  - `UINT IDXGISwapChain3::GetCurrentBackBufferIndex()` — **no parameters**, and
    it returns the index directly. It is not an `HRESULT` call.
  - `void ID3D12Device::CreateRenderTargetView(...)` — returns **void**. There is
    no `HRESULT` to check; a bad argument surfaces on the debug layer.
  - `HRESULT ID3D12Device::CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE, REFIID,
    void **)` — takes the **type directly**. There is no
    `D3D12_COMMAND_ALLOCATION_DESC` struct and no allocator flags enum.
  - `void ID3D12CommandQueue::ExecuteCommandLists(UINT, ID3D12CommandList *const *)`
    — the array is `ID3D12CommandList *const lists[]`. A
    `const ID3D12CommandList *lists[]` does not convert.
  - `DXGI_SWAP_CHAIN_DESC1::Scaling` is the enum `DXGI_SCALING`. Assigning `0`
    does not compile in C++; use `DXGI_SCALING_STRETCH`.
- **`CreateCommandList` returns the list in the RECORDING state.** `Reset` on a
  recording list is invalid, so a list used in a per-frame `Reset` loop must be
  `Close()`d once immediately after creation.
- **`ID3D12CommandAllocator::Reset()` must be called every frame.** Resetting the
  *command list* does not reclaim the allocator's memory. With a single allocator
  reused per frame, omitting it grows memory without bound for as long as the
  loop runs — invisible in a short run, fatal in a long one, and exactly the
  shape of thing an M2 measurement session would hit.
- **`Present(1, 0)` paces correctly across the adapter boundary.** The GPU 1
  present loop measured 600 frames per 2.857 s in two independent runs — 210.0
  fps against a 210 Hz display, exact to three digits. Vsync blocking works even
  though the swapchain lives on a headless adapter and reaches the screen through
  a DWM cross-adapter copy. M2 can treat the present as display-paced rather than
  free-running.
- **At process exit the bridge thread's teardown never runs, and that is
  correct.** `DllMain` receives `lpReserved != NULL`, meaning Windows has already
  terminated every other thread, so the stop signal reaches nothing and the OS
  reclaims the window and the device. Rig logs confirm a clean
  `Finished exiting` with no teardown lines and no hang. Consequence for testing:
  quitting the game does **not** exercise the ordered teardown. Only closing the
  bridge window does. Do not read a clean exit log as evidence that teardown
  works.
- **A thread that owns a top-level window must pump messages.** Broadcasts such
  as `WM_SETTINGCHANGE` and `WM_DISPLAYCHANGE` are delivered with `SendMessage`
  semantics to every top-level window; a window whose thread is blocked in a
  non-pumping wait stalls the broadcasting process until its timeout. The visible
  symptom is not our window freezing — it is the shell or the game hitching.
- `DestroyWindow` **must be called from the thread that created the window**; it
  returns `FALSE` with `ERROR_ACCESS_DENIED` from any other thread. `PostMessage`
  is the cross-thread-safe way to ask that thread to do it; `SendMessage` from the
  unloading thread blocks on the pump and deadlocks if the pump has already
  exited.
- **`CreateThreadW` does not exist.** The API is `CreateThread` — it takes no
  strings, so there is no ANSI/Wide pair. `CreateEventW` does exist.
- `_beginthreadex` takes **six** parameters: `(void *security, unsigned
  stack_size, unsigned (__stdcall *start)(void *), void *arglist, unsigned
  initflag, unsigned *thrdaddr)`. Returns `uintptr_t` — converting to `HANDLE`
  requires `reinterpret_cast`. Its thread-id out-param is `unsigned *`, not
  `DWORD *`. Prefer it over `CreateThread` for threads using CRT facilities.

**LumeniteFX — `lumenite_QuantMotion.fx`, version 2026.06.16 (the one P0 uses)**

- Technique `Lumenite_QuantMotion`, UI label "LUMENITE: QuantMotion". Author's
  description: *"Superfast motion vectors for low-end hardware."*
- **No depth dependency exists in this shader.** Not a switch — there is no depth
  path. No `lumenite_Projections.fxh`, no `tDepth`/`tNormals`, no
  `PS_ReconstructNormals`, no `GetDepth()`. `ATrousFilter` gates on flow
  disagreement only; `PS_Confidence` uses luma and flow only.
- **It includes exactly one header: `ReShade.fxh`.** No rig dependency beyond
  LumeniteFX itself.
- `DEBUG_FLOW` (default 0) set to **1** adds one `PS_Debug` pass returning
  `MotionToColor(tex2D(sFlow, uv).xy)`.
- Declares `uniform uint FRAME_COUNT < source = "framecount"; >` itself.
- The flow pyramid runs 128→64→32→16→8, so the coarsest target is
  `BUFFER_WIDTH/128 × BUFFER_HEIGHT/128`, and `PS_ComputeFlow128` does a 7×7
  search (`SEARCH_RADIUS = 3`) on it. Small buffers make that level degenerate —
  see T4's window size.
- Flow passes return `float2(0,0)` when `FRAME_COUNT == 0`. A black first frame
  is expected, not a fault.
- `MotionToColor` maps direction to hue and magnitude to value, normalising
  against 15 px, and computes the angle as `atan2(-motion.y, -motion.x)` — hue is
  the inverse of the content's direction of travel. Identical implementation in
  both shaders.

**LumeniteFX — `lumenite_Kernel.fx`, version 2026.07.28 (an M3 comparison subject)**

Not used at P0, and not a fallback. Recorded because comparing motion sources is
a planned output of the finished rig, and the analysis is cheaper to keep than to
redo.

- Technique `Lumenite_Kernel`, UI label "LUMENITE: Kernel 2.0". The optical flow
  lives inside it; there is no separate "LumaFlow" technique.
- `IMAGE_SPACE` (default 0) set to **1** removes every depth dependency: the
  `PS_ReconstructNormals` pass is compiled out and `ATrousFilter`'s gate switches
  from depth difference to flow disagreement.
- `DEBUG_KERNEL` (default 0) set to **1** adds a `DEBUG_VIEW` combo — Split View,
  Normals/Depth, Optical Flow, Motion Vectors (arrow overlay), Motion Confidence.
  Richer than QuantMotion's single view, but it `#include`s `DrawText.fxh`
  unconditionally, which must then exist in `reshade-shaders/Shaders`.
- Heavier chain than QuantMotion: 10 tournament candidates in `UpscaleFlow`
  versus 6, two bilateral median passes versus one, plus the normals pass.

**Environment**

- **Adapter LUIDs are reassigned across driver restarts.** Two runs on unchanged
  hardware showed the two RTX 5060 Ti LUIDs swap which one carried `outputs=1`.
  Never persist a LUID as configuration, and never compare against a remembered
  one across sessions. Re-derive the target every session.
- **UE5 creates a device on every adapter in turn before settling on one.** Five
  load cycles on the rig produced provisional LUIDs `0x18A8C7AD`, `0x0001382B`,
  `0x00014BA3`, `0x00014C08` in sequence — including the software adapters. Any
  game-LUID capture taken from `init_device` therefore records whichever probe
  happened to fire, and is worthless. `init_swapchain` is the only authoritative
  source: the swapchain is created once, on the adapter the game actually renders
  on. This is why T2 rule 1 is written the way it is.
- GitHub's `windows-latest` runner ships **Visual Studio 2026 / MSVC 19.51**. Do
  not hardcode a generator name — `cmake -B build -A x64` selects the newest
  installed Visual Studio and survives image bumps.
- **The add-on is loaded and unloaded once per UE5 probe cycle.** The passing run
  showed five full `Loading add-on` → `register_addon` → bridge-thread spawn →
  `destroy_device` → `Unregistered add-on` sequences before the real device
  arrived, all inside three seconds. Every static and every thread the add-on
  owns is therefore constructed and torn down five times per launch. Anything
  that leaks, or that assumes it runs once, fails here — and it fails as a slow
  resource creep across launches rather than as a visible error.
- UE5 picks its render adapter from the Windows per-app graphics preference. The
  NVIDIA app's "GPU App Assignment" is scoped to CUDA and AI-accelerated apps and
  is not the lever that moves it.

---

## 10 · Open observations

Things seen on the rig and not explained. **None of them block a task.** They are
here so they are not rediscovered, and so that when something breaks later this
is the first place to check for a pre-existing cause. Human-owned, like section
09 — report anything new, do not edit this.

- **`Reference count for ID3D12CommandQueue0 object ... is inconsistent (7)`.**
  ReShade logs this once at every game shutdown, immediately after
  `SetFullscreenState(FALSE)` destroys the runtime environment. It appears in
  runs with the add-on **renamed away**, so it is not ours. Worth settling before
  T6, because once a second effect runtime exists this warning becomes a
  candidate explanation for anything odd and we will waste a cycle on it. Would
  be settled by: checking whether stock ReShade logs it on this game with no
  add-ons present at all.
- **The GPU 1 runtime's `Generic Depth` reports `No depth buffers found.`**
  Expected — our swapchain has no depth buffer and P0 creates none. Recorded so
  it is not mistaken for a fault, and because it is the reason QuantMotion's
  depth-free design matters here.
- **QuantMotion emits `warning X4000: use of potentially uninitialized variable`**
  for `UpscaleFlow` and `BilateralMedian9` on compile. Pre-existing in the shader,
  identical on both runtimes, not introduced by this project. Noted so it is not
  read as a GPU 1 anomaly.
- **No no-bridge baseline exists yet.** Every frame-rate figure so far was taken
  with the bridge window present and presenting. We know that *toggling the GPU 1
  effect* costs the game nothing; we do not know what the bridge itself costs
  versus the add-on being absent. Would be settled by: the same gameplay scene
  measured with `mgpu_bridge.addon64` renamed away. Needed before any M2 number
  is quoted.
- **The game's runtime was contaminated during Test B.** With the runtimes synced,
  enabling QuantMotion wrote it into `ReShadePreset.ini`, so the game has been
  running the effect invisibly (its preset has `DEBUG_FLOW=0`) ever since. Clear
  `Techniques=` in `ReShadePreset.ini` to restore a known state. It is also the
  reason the measurements above were available at all.
- **The mouse cursor misbehaves while a ReShade overlay is open** — first observed
  as disappearing, later as freezing with buttons unresponsive. Seen on both
  the game's overlay and the GPU 1 overlay. Deferred deliberately: the next step
  installs third-party add-ons alongside ours, and if the same thing happens with
  them it is ReShade's input handling rather than anything this project
  introduced. Would be settled by: reproducing it with our add-on renamed away.
- **`Reference count for IDXGIFactory2 object ... is inconsistent (4)`.** New at
  T5, logged during our teardown right after ReShade destroys the GPU 1 runtime.
  It is ReShade's proxy around the factory our `create_present_chain` created and
  released; the swapchain and ReShade's own runtime both hold references to it.
  Probably benign. Would be settled by: checking whether it still appears once T6
  settles who owns the GPU 1 runtime.
- **A fifth `D3D12CreateDevice` with `riid = {77ACCE80-...}` and
  `MinimumFeatureLevel = 1000`** fires after the game is up, on the game thread,
  and produces no `[MGPU]` line. Harmless and consistent across every run.
  Probably an NVIDIA or Streamline component. Recorded only so it is not mistaken
  for one of ours.

---

**P0 exited on 2026-09-02.** `VENDOR_LOCK.md` carries the commit, driver
version, ReBAR state and shader provenance that made it pass — note that
LumeniteFX has no single pack version number and that file explains how to
identify the right one. The roadmap from
here — P1 transit, P2 ring buffer, M2 the resolution sweep, M3 DLSS-NR — is
inherited by P1's own document, and the notes below are written for it rather
than for this milestone.

**Named M3 experiment — motion source versus transit bandwidth.** Once the rig
measures, compare QuantMotion (no depth) against Kernel with `IMAGE_SPACE=0` (real
depth via the transited depth buffer). The question is not "which flow is better"
but *is depth-guided flow better by enough to justify roughly doubling the
payload* — `D32_FLOAT` at 1440p is ~14.7 MB against ~15 MB for colour at 72%
worker resolution. That is a quality-per-megabyte result the rig exists to
produce, and it also quantifies what an in-pipeline capture buys over a
desktop-duplication one, which cannot supply depth at all. Carrying depth is
additive to P1's design — a second resource in an existing heap — so nothing
about it needs deciding before the instrument works.

**Named M2 axis — neural pass count.** DLSS-NR supports a recursive multi-pass
cascade with independently tuned parameters per pass; a known-working
implementation runs 1, 2 and 3 passes with per-pass intensity, local tone, local
structure and skin structure values. This is the regime where offload matters
most, because **passes are GPU 1-local while transit is paid once per frame**.
The overhead ratio improves from `T/(T+N)` at one pass to `T/(T+3N)` at three —
the more expensive the neural workload, the better the architecture looks. On a
mid-tier card, three serialised passes on the rendering GPU is likely unplayable
while three parallel passes on an otherwise-idle second GPU may be near-free.

Pass count is orthogonal to payload size, so M2 sweeps both and the result is a
break-even contour rather than a single number.

**Nothing in this section changes P0.** P0 has no ring buffer — no heaps, no
slots, and `COMMAND_LIST_TYPE_COPY` is in the containment grep — so there is no
depth to parameterise. P0 also has no timing instrumentation of any kind; section
01 excludes it outright. Multi-pass is GPU 1-local work added to a frame loop
that does not exist until T5, and adding work to a loop is additive.

**Two accommodations belong in P1's design, and only these two. Do not implement
multi-pass before M3.**

1. **Ring depth is a named constant, never a hardcoded 2.** GPU 1 has one frame
   period per pipeline slot to do unpack, motion, all neural passes and present.
   If a pass count does not fit, the pipeline must deepen — and depth is
   entangled with the fence and ownership logic, the most delicate part of P1.
   Parameterising it when it is first written costs nothing; changing it later
   means reopening the synchronisation design.
2. **Timing is per-stage, not aggregate.** A single "neural work" figure cannot
   decompose into per-pass cost, and the sweep above needs it to. One extra
   timestamp pair now avoids reworking the instrumentation later.

Everything else multi-pass — the per-pass parameter surface, intermediate
ping-pong targets, cascade wiring — is additive and belongs at M3.

A consequence worth stating: ring depth is therefore a *dependent variable*, not
a constant. Each additional slot buys throughput at the cost of a frame of
latency, so M2's honest output is "at what pass count must the pipeline deepen,
and what does that cost" — not a break-even number that quietly assumed a depth.
