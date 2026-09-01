# Cross-Adapter Bridge — Milestone 0, Gate P0

Multi-GPU DLSS-NR R&D. Private, local, non-redistributed.

**This document is authoritative.** It supersedes every earlier version of this
file. Where an instruction relayed in-session conflicts with this document, the
relayed instruction wins and this document should be updated.

---

## 00 · Current status

Read this first — it tells you where the work actually is.

**The build is green.** The add-on compiles in CI and has been deployed to the
rig three times.

- **T1 — PASSED on hardware.** The add-on loads under stock ReShade 6.8.0.2155
  and logs `[MGPU][T1] ... API version 20`. Nothing further needed.
- **T2 — FAILING.** It selects the **Microsoft Basic Render Driver** (a software
  adapter) instead of the second GPU. This is the active task; the fix is
  specified in section 06 under T2.
- **T3 — mechanism works, result void.** `D3D12CreateDevice` succeeds and both
  LUID verifications pass, but on the adapter T2 wrongly selected. A correct
  verification of a wrong selection. `CrossAdapterRowMajorTextureSupported=1` was
  read off a software rasteriser and must be re-read once T2 is fixed.
- **T4 onward** — not started. Held until T2 selects correctly on hardware.

Files in `src/`: `dllmain.cpp`, `diag.{hpp,cpp}`, `adapter.{hpp,cpp}`,
`gpu1_context.{hpp,cpp}`, `worker.{hpp,cpp}`. `runtime_probe.*` does not exist
yet — it arrives with T6.

### The rig

Conditions are controlled and recorded by a human in `VENDOR_LOCK.md`. Summary,
so you can interpret logs:

- Target game: Carnal Instinct (Unreal Engine 5, D3D12).
- Two RTX 5060 Ti 16GB. The game is **pinned** to one card via the Windows
  per-app graphics preference; the display is on the other. Game and display are
  therefore on *different* GPUs — the target adapter is the display adapter.
- Windows enumerates **four** DXGI adapters: the two RTX 5060 Ti and **two**
  "Microsoft Basic Render Driver" software adapters. This is what breaks T2.
- Driver 616.56, ReShade 6.8.0.2155, ReBAR disabled per-program for the game.
- The inference server is stopped during rig deploys, so VRAM is uncontended.

---

## 01 · Scope

**What P0 proves:** that a second, independent ReShade effect runtime can exist
on a second physical adapter inside a running game's process. If that holds,
every later milestone is plumbing. If it doesn't, the architecture is wrong and
we have spent days rather than weeks.

The add-on loads under stock ReShade. It enumerates adapters, creates a D3D12
device on the adapter the game is *not* using, opens a window and swapchain on
that device, and a second ReShade effect runtime is created for it. LumeniteFX
compiles and executes inside that runtime — on GPU 1 — while the game renders on
GPU 0.

| P0 proves | Observable as |
|---|---|
| Add-on loads under a stock, unmodified ReShade | One log line at init ✅ |
| The second adapter is enumerable and bindable by LUID inside the game process | Adapter table in the log ✅ |
| **The correct adapter is selected** | Selected LUID is the non-game hardware adapter ❌ |
| A private D3D12 device coexists with the game's on another card | Device-created log line, game unaffected |
| A second ReShade effect runtime exists on that adapter | `create_effect_runtime` returns true |
| Whether ReShade also auto-hooks our swapchain *(instrumentation, not a gate)* | `init_effect_runtime` count and LUIDs |
| LumeniteFX compiles and runs on GPU 1 | A second window showing a live flow field |

### What P0 explicitly does not do

Each exclusion has a reason. Understanding them is what prevents drift.

- **No cross-adapter transfer.** "Can a second runtime exist on another GPU" and
  "can we move frames between GPUs" are two independent hard problems. Bundled, a
  failure is ambiguous. Separated, each failure names itself.
- **No capture from GPU 0.** `reshade_finish_effects` belongs to the transit
  milestone. P0 never touches the game's buffers, which is also why P0 cannot
  break the game's rendering.
- **No DLSS-NR, no NGX, no nvngx.** The neural layer consumes wrong input
  silently. It goes nowhere near a bridge that is not yet visually validated.
- **No timing, no timestamp queries, no numbers.** Cross-adapter timestamps need
  `GetClockCalibration` to correlate two unrelated clock domains. Any figure
  produced at P0 would be wrong and would get quoted later.
- **No modification to ReShade or LumeniteFX.** Stock ReShade is the whole point.
  LumeniteFX is used as shipped.

### The synthetic input follows from the scope

With no transit, there is no game content on GPU 1 — so LumaFlow needs something
to find motion in. P0 generates an animated test pattern **as the first technique
in the GPU 1 preset**. The pipeline is real, in-process, under stock ReShade, on
the real second card. Only the input is synthetic, and only until transit exists.

It buys something a game frame cannot: **known ground truth.** Scroll the pattern
2 px/frame and the flow field must read 2 px.

---

## 02 · Working rules

**You cannot see CI.** Compilation happens in GitHub Actions on `windows-latest`
with MSVC. A human relays compiler output to you. Optimise for compiling on the
first try: C++17, Windows SDK, no exotic dependencies, warnings not fatal.

**You cannot see the rig.** A human deploys the binary and relays the ReShade
log. The log is your only view of runtime behaviour, so log the *inputs* to every
decision, not just the outcome.

**Two files are human-owned. Never write to them:**

- `.github/workflows/` — the containment check lives here, so write access would
  be write access to your own containment. If a workflow change is needed, output
  the complete file and hand it back.
- `VENDOR_LOCK.md` — it records rig conditions only a human can observe. When T8
  needs values recorded, output them in your report and a human commits them.

**Read declarations before using symbols.** Before using any Win32, D3D12, or
ReShade symbol you have not personally read this session, read its declaration
from `d3d12.h` / `dxgi.h` in the Windows SDK or from the pinned ReShade tree.
Several round trips have been lost to symbols recalled from memory that do not
exist. Section 09 records the ones already verified — trust that section, verify
anything else.

**A compiler error list is not a census.** MSVC reports the first thing it
tripped on. A failed cast invalidates a variable's type, after which every later
use of it emits a cascade and no further checking happens on that expression. Fix
the *class* of error across the whole tree, not the lines CI named. This has
already caught one bug that would otherwise have cost a full round trip.

**Never crash the game.** A crash yields no log, which costs a full round trip
and teaches nothing. HRESULT-check every D3D call. Every task must be
independently skippable: a failed T3 must still let T4's diagnostics report.

**Report what you observe, not what you intended.** After committing, read the
files back and report what is actually in the tree, distinguishing "written" from
"verified present." Sessions have been cut off mid-task before — when resuming,
always check what actually landed before writing.

---

## 03 · Dependencies

Nothing is forked. One private repo, `maohgad-web/MAGUMBOS`, holding only our own
code.

| Repository | Role at P0 | Treatment |
|---|---|---|
| **crosire/reshade** | Add-on headers. Pinned at **v6.8.0**, SHA `18deaa52de0c425a78b329e9cb3c497281cd00ec`, `RESHADE_API_VERSION 20`. Licensed `BSD-3-Clause OR MIT`; we elect BSD-3. | Fetched by CI at the pinned SHA. Not forked, not vendored, not a submodule. |
| **umar-afzaal/LumeniteFX** | Kernel + LumaFlow, run as shipped by the GPU 1 runtime. | **Never enters the repo.** ReShade compiles `.fx` at runtime, so it is a runtime asset — installed by hand on the rig. |
| **clshortfuse/renodx** | Read-only reference for private-device instantiation patterns. | Read on GitHub. Not a dependency. |

**Do not fork LumeniteFX on GitHub.** Per GitHub's docs: *"All forks of public
repositories are public. You cannot change the visibility of a fork."* A fork
would be permanently public, and pushing a modified shader to it is public
redistribution of a modified work — precisely what its AGNYA license withholds
without the author's permission. Should LumaFlow ever need modifying, it happens
in a private repo, never a fork.

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
ReShade's DXGI hook adopts our swapchain — and `config_path` takes `gpu1.ini`
directly, removing the whole class of "the preset didn't load" failures. Drive it
with `update_and_present_effect_runtime`, which is legal precisely because we
created the runtime; the header forbids that call only on runtimes ReShade
auto-created.

The **auto-hook path** stays as **instrumentation only**. Registering
`init_effect_runtime` and logging every runtime with its LUID costs one callback
and zero risk, and tells us whether ReShade adopts our swapchain. Worth knowing;
not worth gating P0 on.

Because every technique in the chain — including the pattern generator — runs
inside ReShade's own runtime on GPU 1, **our C++ renders nothing.** It creates a
device, a window, and a swapchain; then each frame it asks the runtime to render
its chain into the backbuffer and presents.

---

## 05 · Layout

Files live at the **repo root** — there is no `mgpu-bridge/` subdirectory.

```
MAGUMBOS/
├─ .github/workflows/build.yml   HUMAN-OWNED — never write here
├─ .gitignore                    ext/, build/, _dl/
├─ CMakeLists.txt                x64, C++17, output mgpu_bridge.addon64
├─ Agent_Task.md                 this document
├─ README.md                     P0 scope, deploy steps, how to read the log
├─ LICENSE                       proprietary, all rights reserved
├─ THIRD_PARTY.md                ReShade BSD-3; LumeniteFX not-contained note
├─ VENDOR_LOCK.md                HUMAN-OWNED — rig conditions, pinned SHA
├─ assets/                       deployed by hand, never compiled
│   ├─ pattern.fx                ours: animated known-motion test pattern
│   ├─ mv_debug.fx               ours: flow field -> visible colour
│   └─ gpu1.ini                  preset: pattern, Kernel, LumaFlow, mv_debug
└─ src/
    ├─ dllmain.cpp               register_addon, event wiring, teardown
    ├─ diag.{hpp,cpp}            structured log lines, one per decision
    ├─ adapter.{hpp,cpp}         enumeration, selection, caps logging
    ├─ gpu1_context.{hpp,cpp}    private D3D12 device on the selected adapter
    ├─ runtime_probe.{hpp,cpp}   T6 — not yet created
    └─ worker.{hpp,cpp}          bridge thread, message pump, frame loop
```

The output filename is `mgpu_bridge.addon64`. **The extension replaces `.dll`, it
does not append** — `mgpu_bridge.addon64.dll` goes green in CI and silently never
loads. CMake achieves this with `PREFIX ""`, `SUFFIX ".addon64"`, and
`RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}>"` (the generator-expression
wrapper suppresses the per-config subdirectory).

### Threading

- **Game thread.** Runs ReShade's callbacks. It must never block, and it never
  touches GPU 1 objects.
- **Bridge thread.** Owns the adapter-1 device, the window, and the swapchain.
  Everything GPU 1 is created, used, and destroyed here.

**Gotcha — message pump.** The window's HWND must be created *and pumped* on the
bridge thread. A DXGI swapchain whose window has no message loop on its owning
thread will hang presentation, and it presents as a sync deadlock.

**Gotcha — re-entrancy.** Never create a DXGI factory, device, or swapchain from
inside `DllMain`: add-ons load from within the game's `CreateDXGIFactory1` call,
so DXGI work there recurses into ReShade's own factory hook. Do it from a ReShade
callback or from the bridge thread.

**Gotcha — never join from `DllMain`.** `DLL_PROCESS_DETACH` runs under the
loader lock, and a thread cannot finish exiting without that lock. A join
deadlocks; a timed-out join leaves a live thread pointing into a DLL that is
about to unmap. Signal and return. Leaking the GPU 1 device on dynamic unload is
explicitly in scope at P0. Deadlocking is not.

**Known lifecycle quirk.** UE5 creates throwaway probe devices at startup, so the
add-on loads and tears down two or three times before the real render device
appears. ReShade sometimes re-attaches the module without a fresh `LoadLibrary`
("Loading externally registered add-on"), in which case static state such as a
`started` atomic survives and one-shot guards will not re-arm. Harmless at T3;
becomes a real hazard at T4 when there is a window and pump to recreate.

---

## 06 · Task list

### T1 — Repo scaffold and a loading add-on ✅ PASSED

Complete and verified on hardware. `reshade::log::message` only works once
`register_addon` has succeeded, so on failure the add-on emits one
`OutputDebugStringA` line and returns `TRUE` — never `FALSE`, which would fail the
DLL load.

### T2 — Adapter enumeration and selection ❌ ACTIVE TASK

`IDXGIFactory4::EnumAdapters1`; log every adapter. Bind **by LUID, not index**.

**Why it currently fails:** the rig enumerates four adapters — two RTX 5060 Ti and
two "Microsoft Basic Render Driver". Exclusion of the game's adapter leaves three
candidates, the headless tie-break finds three with `outputs=0`, and
last-in-enum-order picks a software rasteriser. T3 then creates a device on it
successfully, with both LUID checks passing: a correct verification of a wrong
selection.

Four rules, applied in this order so a partial application is always safe:

**1. Refuse rather than guess.** If after filtering there is not *exactly one*
candidate, select nothing. Log loudly and let T3 skip device creation. A missing
device is diagnosable; a device on the wrong adapter is not — it succeeds, logs
cleanly, and proves nothing. This replaces the old last-in-enum-order fallback.

**2. The game LUID must come from the swapchain.** UE5's probe devices mean the
first `init_device` is not reliably the game's renderer — one run captured a
*software adapter* as the game LUID. The authoritative game render device is the
one passed to `CreateSwapChainForHwnd`. Capture from `init_swapchain` via the
swapchain's device; keep `init_device` only as a provisional value that a later
swapchain-derived LUID overrides. **If no swapchain-derived game LUID has been
established, refuse to select.**

**3. Filter software adapters out of the candidate set.**
`DXGI_ADAPTER_FLAG_SOFTWARE` tested against `DXGI_ADAPTER_DESC1::Flags`. Still
enumerate and log them; they simply cannot be selected. Log `Flags` in hex,
`VendorId` (Microsoft is `0x1414`) and `DedicatedVideoMemory` alongside, so the
rig can confirm which discriminator actually separates them.

**4. Output count is never the primary discriminator.** This project's own
topology puts the display on the target card, and any rig with displays on both
breaks an output-count rule silently. Log output counts; use them only as a
tiebreaker when more than two hardware adapters are present.

**Diagnostic logging (must not influence selection).** For each adapter,
`QueryInterface` the `IDXGIAdapter1` for `IDXGIAdapter3` and call
`QueryVideoMemoryInfo` (signature in section 09), logging current local-memory
usage. With the game running, one card sits near 5 GB and the other near 2 GB,
which maps DXGI indices to physical cards unambiguously against `nvidia-smi`.

**Acceptance:** the full adapter table, the game's adapter LUID and its source
(swapchain or provisional), and one line naming the selected LUID with the rule
that produced it. The selected adapter must be the non-game hardware adapter.

### T3 — Bridge thread, and the device on it

Spawn the bridge thread here. `D3D12CreateDevice` against the selected adapter
**runs on that thread** — creating it inside a ReShade callback is the same
re-entrancy class T2 avoids.

After creation, verify both directions: the new device's `GetAdapterLuid()`
against the selected LUID (mismatch → release and stop) and against the game's
LUID (equal → log FATAL, release, stop).

Log `CrossAdapterRowMajorTextureSupported` for the record. **The current
`supported=1` in the log is void** — it was read from a software rasteriser. It
must be re-read once T2 selects correctly.

**There is no device-removal event.** ReShade 6.8's `addon_event` enum has 80
values and contains no `device_removed` or `device_restored`. Poll
`ID3D12Device::GetDeviceRemovedReason()` from the bridge thread instead —
replace its infinite idle wait with a timed wait — and log only on transition,
since the value is sticky once removed.

**Acceptance:** device created on the bridge thread on the correct adapter, game
still renders normally, `GetDeviceRemovedReason()` stays `S_OK`, clean shutdown.

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

**Primary:** `reshade::create_effect_runtime(device_api::d3d12, our_device,
our_queue, our_swapchain, "gpu1.ini", &runtime)` from the bridge thread.

**Alongside, as instrumentation:** register `init_effect_runtime` and log every
runtime init with its adapter LUID. Add `init_swapchain` and `init_device`
logging too. These do not conflict — the runtime we create is the one we drive.

**Acceptance:** `create_effect_runtime` returns true and yields a runtime whose
device carries the selected LUID. The auto-hook log is recorded either way and
gates nothing.

If explicit creation fails, report the arguments and result. Do not fall back to
an auto-hooked runtime silently, and do not patch ReShade.

### T7 — Drive LumeniteFX on GPU 1

The preset arrives through `create_effect_runtime`'s `config_path`, so there is no
separate preset-loading step.

- Command list: `runtime->get_command_queue()->get_immediate_command_list()`.
- Back buffer: `effect_runtime::get_current_back_buffer()` returns an
  `api::resource`; make the RTV with `device->create_resource_view(...)`.
- Drive: `update_and_present_effect_runtime(runtime)` is the intended driver for a
  runtime we created, and it handles the present. `render_effects(cmd_list, rtv,
  rtv_srgb)` only if explicit target control is needed; `render_technique` +
  `find_technique` are the per-technique fallback.

Technique handles from `find_technique` go stale on `reshade_reloaded_effects`.
If the fallback is used, register that event and re-query rather than caching.

LumaFlow is temporal — present every frame rather than discovering this as
motion-vector garbage that looks like a shader bug.

**Acceptance:** the window shows the scrolling test pattern transformed into a
coherent motion-vector field, updating live, while the game runs on GPU 0. A
pattern scrolling 2 px/frame reads as 2 px of flow.

### T8 — Teardown and the run report

Clean shutdown on add-on unload, observing the never-join rule. Then **output**
the P0 run report — driver version, ReBAR state, LumeniteFX release, ReShade SHA,
each task's pass/fail — for a human to commit to `VENDOR_LOCK.md`.

**Acceptance:** game exits cleanly with no hang and no crash on repeated
load/unload.

---

## 07 · Containment

### Forbidden symbols — enforced in CI

Every one belongs to a later milestone. Their presence in `src/` or `assets/`
fails the build. This step runs before the build, in the human-owned workflow:

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

### The file manifest is closed

The tree in section 05 is exhaustive. Creating `transit.cpp`, `xadapter.cpp`, or
any file not listed is out of scope regardless of how good the code is.

### Stop-and-report conditions

- **Explicit runtime creation fails at T6.** Report arguments and result. Do not
  modify ReShade.
- **Any header detail contradicts this document.** Report the actual signature;
  do not silently redesign around it.
- **Device creation on the selected adapter fails.** Report the HRESULT and the
  adapter table. Do not fall back to the game's adapter.
- **Something needs a file outside the manifest.** Report what and why.

---

## 08 · Instrumentation

Every line carries a fixed prefix and a task id — `[MGPU][T3] D3D12CreateDevice
hr=0x... luid=...` — so a whole run greps out of a long ReShade log in one pass.

| What you see | What it means |
|---|---|
| Coherent flow field tracking the pattern | P0 passes. The architecture holds. |
| Pattern visible, flow field black | LumaFlow ran but found nothing, or Kernel is not feeding it. Check preset order. |
| Flow magnitude wrong against known motion | LumaFlow works but is mis-scaled — only the synthetic pattern can surface this. |
| Window black, runtime created in log | Preset path wrong, or shaders failed to compile on GPU 1. |
| Window frozen after N frames | Message pump, not rendering. Check the window's owning thread first. |
| Add-on never loads, CI green | Almost always the output filename. The extension replaces `.dll`. |
| No auto-hook runtime init for our swapchain | ReShade did not adopt it. A roadmap finding; does not block P0. |
| Game crashes or hitches | Something ran on the game thread that should not have. P0 touches no game resource. |

---

## 09 · Verified API facts

Each entry was read from a primary source — the pinned ReShade tree, the Windows
SDK, or Microsoft's documentation — after a guess proved wrong. Trust this
section. Verify anything not in it, and add what you verify.

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
- `HRESULT ID3D12Device::GetDeviceRemovedReason()` — no parameters. Returns
  `S_OK` when healthy, otherwise the removal reason. **Sticky**: once removed it
  keeps returning the reason, so polling needs a one-shot transition guard.
  Microsoft also notes that device removal signals *all* fences to `UINT64_MAX`,
  making `ID3D12Fence::SetEventOnCompletion` the asynchronous alternative — not
  for P0, but the transit milestone will want it.
- `DXGI_ADAPTER_FLAG_SOFTWARE = 2`, tested against `DXGI_ADAPTER_DESC1::Flags`.
  The enum is `{ NONE = 0, REMOTE = 1, SOFTWARE = 2, FORCE_DWORD = 0xffffffff }`.
- `HRESULT IDXGIAdapter3::QueryVideoMemoryInfo(UINT NodeIndex,
  DXGI_MEMORY_SEGMENT_GROUP MemorySegmentGroup, DXGI_QUERY_VIDEO_MEMORY_INFO
  *pVideoMemoryInfo)`. Obtain `IDXGIAdapter3` by `QueryInterface` from
  `IDXGIAdapter1`. Read the struct's members from `dxgi1_4.h` — the method
  signature is verified, the field names are not.
- **`CreateThreadW` does not exist.** The API is `CreateThread` — it takes no
  strings, so there is no ANSI/Wide pair. `CreateEventW` does exist.
- `_beginthreadex` takes **six** parameters: `(void *security, unsigned
  stack_size, unsigned (__stdcall *start)(void *), void *arglist, unsigned
  initflag, unsigned *thrdaddr)`. Returns `uintptr_t` — converting to `HANDLE`
  requires `reinterpret_cast`. Its thread-id out-param is `unsigned *`, not
  `DWORD *`. Prefer it over `CreateThread` for threads using CRT facilities.

**Environment**

- **Adapter LUIDs are reassigned across driver restarts.** Two runs on unchanged
  hardware showed the two RTX 5060 Ti LUIDs swap which one carried `outputs=1`.
  Never persist a LUID as configuration, and never compare against a remembered
  one across sessions. Re-derive the target every session.
- GitHub's `windows-latest` runner ships **Visual Studio 2026 / MSVC 19.51**. Do
  not hardcode a generator name — `cmake -B build -A x64` selects the newest
  installed Visual Studio and survives image bumps.
- UE5 picks its render adapter from the Windows per-app graphics preference. The
  NVIDIA app's "GPU App Assignment" is scoped to CUDA and AI-accelerated apps and
  is not the lever that moves it.

---

P0 exits when T1–T8 pass and `VENDOR_LOCK.md` carries the SHA, driver version,
ReBAR state and LumeniteFX release that made them pass. Only then does the
cross-adapter heap enter the picture: P1 adds transit from GPU 0, P2 widens it to
the full ring buffer, M2 adds the resolution-scale knob and the sweep for
T_fixed, and M3 introduces DLSS-NR against an already-characterised transport.
