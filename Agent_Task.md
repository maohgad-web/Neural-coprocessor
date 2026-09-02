# Cross-Adapter Bridge — Milestone 0, Gate P0

Multi-GPU DLSS-NR R&D. Private, local, non-redistributed.

**This document is authoritative.** It supersedes every earlier version of this
file. Where an instruction relayed in-session conflicts with this document, the
relayed instruction wins and this document should be updated.

---

## 00 · Current status

Read this first — it tells you where the work actually is.

**The build is green and T1–T3 have passed on hardware.** The add-on compiles in
CI and has been deployed to the rig. Adapter selection, the bridge thread and the
device on the second GPU are all done and verified. **T4 is the active task, and
it is where you start.**

- **T1 — PASSED.** The add-on loads under stock ReShade 6.8.0.2155 and logs
  `[MGPU][T1] ... API version 20`.
- **T2 — PASSED.** The swapchain-derived game LUID is established, software
  adapters are filtered by flag *or* Microsoft vendor ID, and exactly one
  non-game hardware adapter is selected by LUID. Both findings that made this
  work are recorded in section 09 — read them before touching adapter code.
- **T3 — PASSED.** `D3D12CreateDevice` runs on the bridge thread against the
  selected adapter and both LUID verifications pass. **One acceptance item was
  not built:** the device-removal poll. `bridge_main`'s post-device wait is a
  plain `WaitForSingleObject(stop_event, INFINITE)` and
  `GetDeviceRemovedReason()` is never called. T4 rewrites that wait, so the poll
  is folded into T4 rather than reopened as a separate task.
- **T4 — ACTIVE. This is your task.** Nothing beyond it is started.

You are joining an in-progress project. T1–T3 are working code that a human has
verified on the rig: **read them to understand the shape of the codebase, do not
revise them.** If something in T1–T3 looks wrong to you, say so in your report
and stop — do not fix it. A change there invalidates a hardware result that cost
a deploy cycle to obtain.

T4 names three exceptions to that, and they are the only ones: an `HMODULE`
capture added to `dllmain.cpp`, the `CloseHandle` fix in `worker::rearm()`, and
the rewrite of `bridge_main`'s post-device wait. Everything else in those files
stays as it is.

**One latent condition, recorded so you do not trip over it and do not try to
fix it.** The T2 selection is a one-shot latched by `S.decided`, and
`adapter::shutdown()` does not clear it. That latch is load-bearing: after a
teardown the adapter table holds released pointers, and the latch is what stops a
later event from dereferencing them. It works because on this rig the five probe
cycles produce no swapchain, so no selection is ever decided before the real one.
Do not add anything to T4 that re-enters `on_device` or `on_swapchain`, and do
not "improve" the latch.

Files in `src/`: `dllmain.cpp`, `diag.{hpp,cpp}`, `adapter.{hpp,cpp}`,
`gpu1_context.{hpp,cpp}`, `worker.{hpp,cpp}`. T4 extends `worker.*` — the bridge
thread already exists there. `runtime_probe.*` does not exist yet; it arrives
with T6, not now.

### The rig

Conditions are controlled and recorded by a human in `VENDOR_LOCK.md`. Summary,
so you can interpret logs:

- Target game: Carnal Instinct (Unreal Engine 5, D3D12).
- Two RTX 5060 Ti 16GB. The game renders on the adapter that **has the display
  attached** (`outputs=1`, and this process's VRAM usage sits on it). The
  selected target adapter is the **headless** one (`outputs=0`). Verified from
  the swapchain-derived LUID on the passing run, so treat this as the topology,
  not the earlier assumption that game and display were on different cards.
- Consequence for T5: the window we present to lives on a desktop driven by the
  game's adapter, so a GPU 1 present reaches the screen via a DWM cross-adapter
  copy. That is fine for P0 — it proves the runtime exists and executes — but it
  is not a free present, and M2 must not attribute its cost to the shader.
- Windows enumerates **four** DXGI adapters: the two RTX 5060 Ti and **two**
  "Microsoft Basic Render Driver" software adapters, only one of which sets the
  software flag.
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

With no transit, there is no game content on GPU 1 — so the Kernel's flow needs something
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

**A green log is not a passed task.** Before reporting a task complete, walk its
acceptance list item by item and point at the code that implements each one — not
at the log line that would appear if it did. The two are not the same evidence. A
poll that was never written and a poll that finds nothing healthy produce
identical logs, and this project has already shipped one task where the rig run
passed while an acceptance item did not exist. Where an acceptance item is of the
form "X stays healthy" or "Y never happens", the log is silent by construction
and the source is the only proof. Say so explicitly in your report: name each
acceptance item and the function that satisfies it.

**Three files are human-owned. Never write to them:**

- `.github/workflows/` — the containment check lives here, so write access would
  be write access to your own containment. If a workflow change is needed, output
  the complete file and hand it back.
- `VENDOR_LOCK.md` — it records rig conditions only a human can observe. When T8
  needs values recorded, output them in your report and a human commits them.
- `Agent_Task.md` — this document. It is maintained and re-issued by a human, so
  any edit you made would be overwritten by the next re-issue without warning.

**A milestone does not flip on a log alone.** After each rig run, a human reviews
the source of the files the next task will modify, and of any file whose
acceptance is about to be marked passed, before this document is updated. That
review is where the brief is reconciled with the code — it is not your job, but
it is why your report must name acceptance items against functions rather than
against log lines.

**Findings go in your report, not into this document.** When you discover
something that belongs in section 09 — a verified signature, an API that does not
behave as documented, an environment quirk — state it plainly in your report and
a human records it here. That keeps one writer per file and means section 09 is
never silently reverted.

**Verify what you can reach; do not hunt for what you cannot.** Section 09
records symbols already verified from primary sources — trust it. For anything
else, apply this in order:

1. **Reachable source → verify.** The pinned ReShade tree is always reachable via
   GitHub and is mandatory for every ReShade symbol. Read the declaration; do not
   recall it.
2. **No reachable source → stop looking after one or two tool calls.** You have
   GitHub read, and web search may be unavailable. The Windows SDK headers are
   not reachable from here. Hunting for a mirror costs more than the thing it
   prevents.
3. **Then split by what the compiler can catch.** Struct field names, function
   arity and type mismatches are all reported loudly by MSVC with a file and
   line. For that class: state your assumption, write it, mark it with an
   `// UNVERIFIED:` comment, flag it in your report, and let CI be the check.
4. **Never guess behaviour.** Whether an event exists and fires, whether a value
   is sticky, what a call actually does, whether an API means what its name
   suggests — the compiler cannot check any of it, and a wrong guess produces a
   green build that is silently wrong. If a behavioural fact is unverifiable,
   stop and report rather than assuming.

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
| **umar-afzaal/LumeniteFX** | `Lumenite_QuantMotion`, whose optical flow is the piece P0 needs. Run as shipped by the GPU 1 runtime. It includes only `ReShade.fxh`, so no additional shader headers are required on the rig. | **Never enters the repo.** ReShade compiles `.fx` at runtime, so it is a runtime asset — installed by hand on the rig. |
| **clshortfuse/renodx** | Read-only reference for private-device instantiation patterns. | Read on GitHub. Not a dependency. |

**Do not fork LumeniteFX on GitHub.** Per GitHub's docs: *"All forks of public
repositories are public. You cannot change the visibility of a fork."* A fork
would be permanently public, and pushing a modified shader to it is public
redistribution of a modified work — precisely what its AGNYA license withholds
without the author's permission. Should any LumeniteFX shader ever need
modifying, it happens in a private repo, never a fork. P0 needs no modification
— `DEBUG_FLOW` is a preprocessor definition set from the preset, not a source
edit.

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
│   └─ gpu1.ini                  preset + preprocessor defines (see T7)
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

### T2 — Adapter enumeration and selection ✅ PASSED

Implemented in `adapter.*` and verified on the rig. Recorded here because T4
onward depends on the selected adapter and on why it is selected the way it is.
**Do not revise this code.**

`IDXGIFactory4::EnumAdapters1`; log every adapter. Bind **by LUID, not index**.
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

**3. Filter software adapters out of the candidate set.** The test must be a
disjunction, because the flag alone is not reliable:

```cpp
const bool is_software =
    (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0 ||
    desc.VendorId == 0x1414;   // Microsoft — WARP / Basic Render Driver / virtual
```

A rig run proved this necessary: two "Microsoft Basic Render Driver" adapters
with identical vendor, device ID, memory and description reported **different**
`Flags` in the same session — `0x0` and `0x2`. The flag caught one and missed the
other, leaving an unflagged WARP in the candidate set, which made the tiebreak
ambiguous and correctly caused rule 1 to refuse. Vendor ID is the reliable
backstop, since Microsoft ships no hardware GPU.

**Do not gate on `DedicatedVideoMemory == 0`.** It separates both WARP entries on
this rig, but integrated GPUs legitimately report zero dedicated memory and use
shared system memory, so gating on it would exclude a real adapter elsewhere. Log
it; never filter on it.

Still enumerate and log software adapters — they simply cannot be selected. Log
`Flags` in hex, `VendorId`, `DeviceId` and `DedicatedVideoMemory` for every
adapter, and log which test rejected each candidate.

**4. Output count is never the primary discriminator.** This project's own
topology puts the display on the target card, and any rig with displays on both
breaks an output-count rule silently. Log output counts; use them only as a
tiebreaker when more than two hardware adapters are present.

**Diagnostic logging (must not influence selection).** For each adapter,
`QueryInterface` the `IDXGIAdapter1` for `IDXGIAdapter3` and call
`QueryVideoMemoryInfo` with `DXGI_MEMORY_SEGMENT_GROUP_LOCAL` (full details in
section 09), logging `CurrentUsage`. This is *this process's* usage on each
adapter, so expect multi-GB on the adapter the game renders on and near-zero on
every other one. That directly identifies which adapter the game is using,
independently of output counts.

**Acceptance:** the full adapter table, the game's adapter LUID and its source
(swapchain or provisional), and one line naming the selected LUID with the rule
that produced it. The selected adapter must be the non-game hardware adapter.

### T3 — Bridge thread, and the device on it ✅ PASSED

Implemented in `worker.*` and `gpu1_context.*`, verified on the rig. **This is
the thread T4 builds on — read it, do not revise it.**

The bridge thread is spawned here. `D3D12CreateDevice` against the selected adapter
**runs on that thread** — creating it inside a ReShade callback is the same
re-entrancy class T2 avoids.

After creation, verify both directions: the new device's `GetAdapterLuid()`
against the selected LUID (mismatch → release and stop) and against the game's
LUID (equal → log FATAL, release, stop).

`CrossAdapterRowMajorTextureSupported` is logged for the record. It reads `0` on
this hardware — a P1 constraint, recorded in section 09. It does not affect P0.

**One thing T3 has not demonstrated.** On the probe cycles the bridge thread
logged `game device released` and `bridge thread exiting`. On the final, real
cycle those lines are absent from the log — ReShade unregisters the add-on and
the process exits without them. That may only mean late logging was dropped, but
it has not been shown to be a clean join. T4 is where it stops being cosmetic: a
window whose owning thread never exits hangs the game on close.

**There is no device-removal event.** ReShade 6.8's `addon_event` enum has 80
values and contains no `device_removed` or `device_restored`. Poll
`ID3D12Device::GetDeviceRemovedReason()` from the bridge thread instead —
replace its infinite idle wait with a timed wait — and log only on transition,
since the value is sticky once removed.

**Acceptance:** device created on the bridge thread on the correct adapter, game
still renders normally, `GetDeviceRemovedReason()` stays `S_OK`, clean shutdown.

### T4 — Window and message pump ⬅ ACTIVE TASK

The bridge thread creates a visible **1280×720** window, runs a message loop, and
shuts down cleanly when the game exits or the add-on unloads. All of it lives in
`worker.*`, on the thread T3 already spawned. No new files.

**The size is fixed at 1280×720 and is not a placeholder.** LumeniteFX builds an
optical-flow pyramid whose coarsest level is `BUFFER_WIDTH/128 ×
BUFFER_HEIGHT/128`, and `PS_ComputeFlow128` runs a 7×7 search on it. At 640×360
that level is 5×2 pixels — degenerate, and it seeds every level below it.
1280×720 gives 10×5, which the hierarchy can work with. Do not make it
configurable, and do not shrink it to make a test cheaper.

**Thread ownership is the whole point of this task.** A window belongs to the
thread that called `CreateWindowExW`, and only that thread may pump its messages.
Everything here — register class, create window, `GetMessage`/`DispatchMessage`,
and eventually `DestroyWindow` — happens on the bridge thread. Nothing touches
the window from a ReShade callback, and nothing blocks the game thread waiting on
it. The game must keep rendering exactly as it does today.

Five requirements:

**1. Register the window class on the bridge thread**, with the add-on's
`HMODULE` as `hInstance`. `DllMain` has that handle but does not currently store
it, and `GetModuleHandle(nullptr)` returns the *game's* module, not ours. **You
may add exactly one thing to `dllmain.cpp`: a file-scope `HMODULE` captured at
`DLL_PROCESS_ATTACH` and an accessor for it.** That is the only permitted edit
to T1–T3 code.

**Build the class name so it cannot collide across module reloads** — embed the
`HMODULE` value in it, e.g. `MGPU_Bridge_Wnd_%p`. The reason is in section 09:
the add-on is loaded and unloaded five times per launch, and ReShade sometimes
re-attaches the module without a fresh `LoadLibrary`, so a class registered by a
previous cycle can still exist. A per-module name means a stale class from an
unmapped module can never be reused — which would mean creating a window whose
`lpfnWndProc` points into unmapped memory.

Even so, **treat `RegisterClassExW` failing with `ERROR_CLASS_ALREADY_EXISTS` as
success and proceed**: that case means this same still-mapped module registered
it on an earlier cycle, so the class is ours and is valid. Any other failure is a
stop-and-report.

**2. Create the window with `CreateWindowExW`**, `WS_OVERLAPPEDWINDOW |
WS_VISIBLE`, client area exactly 1280×720. `CreateWindowExW`'s width and height
are the *outer* dimensions, so compute them with `AdjustWindowRect` against the
same style — otherwise the client area comes out smaller than 720 lines by the
title bar and borders, and the pyramid argument above quietly stops holding. Log
the resulting client rect so a human can confirm it.

**3. Replace the post-device wait with a pumping loop, and add the removal poll
T3 never built.** `bridge_main`'s wait after `create_device` is currently
`WaitForSingleObject(st().stop_event, INFINITE)`. Replace it with a loop around

```cpp
MsgWaitForMultipleObjects(1, &st().stop_event, FALSE, 250, QS_ALLINPUT)
```

`WAIT_OBJECT_0` means shut down. `WAIT_OBJECT_0 + 1` means messages are waiting —
drain them with `PeekMessage` / `TranslateMessage` / `DispatchMessage` until the
queue is empty, then loop. `WAIT_TIMEOUT` is the poll tick: call
`GetDeviceRemovedReason()` on the GPU 1 device and log **only on the transition
away from `S_OK`**, since the value is sticky once removed. One loop, one thread.
Do not spawn a second thread for the pump.

The 250 ms timeout is the poll interval, not a pump interval — messages wake the
wait immediately regardless of it.

**Pumping is not optional once the window exists.** A thread that owns a
top-level window and blocks without pumping will hang any process that broadcasts
a message to all top-level windows — that includes the shell and, in the wrong
moment, the game. An unpumped window is a desktop-wide hazard, not a local one.
For the same reason the window must be created **after** `create_device` returns
true and never on a cycle with no device: a window whose thread is about to exit
is worse than no window.

**4. The window procedure stays minimal.** Handle `WM_CLOSE` and `WM_DESTROY` by
signalling the same shutdown path the add-on unload uses, and pass everything
else to `DefWindowProcW`. **The user closing this window must not close the
game** — signal our shutdown, tear down our side, and leave the game running.
Verify that specifically; it is the failure mode most likely to look like a
crash. No rendering, no D3D calls, no logging inside the window procedure beyond
those two messages.

**5. Teardown is ordered, and it happens before `rearm()`.** On shutdown the
bridge thread exits its loop and then, **on that same thread and in this order**:
`DestroyWindow`, `UnregisterClass`, the existing `gpu1::shutdown()` /
`adapter::shutdown()`, the final log lines, and only then `rearm()`.

The ordering is not stylistic. `rearm()` sets `started = false`, which lets the
game thread spawn a replacement bridge thread immediately — while the old thread
is still running. If `UnregisterClass` has not happened yet, the new thread's
`RegisterClassExW` races the old thread's cleanup. Everything the thread owns
must be released before it re-arms.

Nothing outside the bridge thread may destroy the window. `worker::stop()` is
called from the game thread and from `DllMain`; it must stay signal-only. Do not
add a `SendMessage` there — it would block the game thread on our pump, and from
`DllMain` it would block under the loader lock.

**Close the thread handle.** `rearm()` currently does `st().thread = nullptr`
without `CloseHandle`, so every one of the five load cycles leaks a thread
handle. Fix it in `rearm()` while you are there — it is the one leak the five-
cycle behaviour turns from theoretical into per-launch.

Section 06's T8 covers the full run report; here you only need this task's share
of teardown to be clean.

**Acceptance:** an empty 1280×720 window appears beside the game and can be moved
and focused while the game keeps rendering; the log shows the class name, the
window handle, the client rect and the owning thread id; closing the window
leaves the game running; unloading the add-on tears down without a hang and
without a `DestroyWindow` failure in the log.

**Two conditions specific to this rig, both from section 09 — read them before
you start.** First, the add-on is loaded and unloaded five times per launch while
UE5 probes adapters, and on those cycles the bridge thread starts and exits
before any selection completes. Your window must not be created on a cycle that
has no selected adapter, and the class must be unregistered on every one of those
teardowns or the sixth `RegisterClassExW` fails with
`ERROR_CLASS_ALREADY_EXISTS`. Treat `RegisterClassExW` failing that way as a
teardown bug, not as a condition to tolerate. Second, T3's teardown has never been
observed reaching its final log lines on the real cycle; log the class
unregistration and the thread exit explicitly so that this run answers the
question either way.

Note the diagnostic pair in section 08: a window that appears and then freezes is
a pump problem, not a rendering problem, and rendering does not arrive until T5.

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

The Kernel's flow is temporal — it compares the current luma against the previous
frame's. Present every frame rather than discovering this as motion-vector
garbage that looks like a shader bug.

**Use `LUMENITE: QuantMotion`, not Kernel.** Preset order is `pattern.fx`, then
`Lumenite_QuantMotion`. Nothing else.

**One preprocessor definition:**

```
DEBUG_FLOW=1
```

QuantMotion has **no depth dependency of any kind** — not a switch that disables
one, none to begin with. No `lumenite_Projections.fxh`, no `tDepth`/`tNormals`,
no `PS_ReconstructNormals`, no `GetDepth()`. Its `ATrousFilter` gates purely on
flow disagreement, and `PS_Confidence` uses luma and flow only. It includes
exactly one header, `ReShade.fxh`. **This is why `pattern.fx` does not need to
synthesise a depth buffer.**

`DEBUG_FLOW=1` adds a single `PS_Debug` pass returning
`MotionToColor(tex2D(sFlow, uv).xy)` — precisely the flow-field visualisation
this milestone needs, written by the shader's author against its own internal
target, with no extra includes. That is why there is no `mv_debug.fx` in the
manifest: do not write one.

QuantMotion is chosen over Kernel for three reasons: it is unconditionally
depth-free rather than conditionally so, it needs no additional shader headers on
the rig, and it is the motion source used in a known-working DLSS-NR pipeline —
so P0 validates the component M3 will actually use.

**The motion source is an input to this rig, not the subject of study.** It is
selected by preset, so swapping it is configuration, not code. Kernel is analysed
in section 09 not as a fallback but because comparing motion sources is a planned
*output* of the finished instrument — see the M3 note at the end of this
document. Do not substitute one for the other during P0.

**Acceptance:** the window shows the scrolling test pattern transformed into a
coherent motion-vector field, updating live, while the game runs on GPU 0.

**Read hue, not magnitude, for ground truth.** `MotionToColor` normalises
magnitude against 15 pixels, so 2 px/frame produces a dim colour — direction is
the reliable readout. Note also that the angle is computed as
`atan2(-motion.y, -motion.x)`, so hue encodes the inverse of the content's
movement. A hue that looks "backwards" is correct, not a bug.

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
- **The window cannot be created, or the pump will not drain.** Report the
  `GetLastError()` value and the owning thread id. Do not move window creation
  off the bridge thread to make it work — a window on the wrong thread passes T4
  and fails T5 in a way that is much harder to read.

---

## 08 · Instrumentation

Every line carries a fixed prefix and a task id — `[MGPU][T3] D3D12CreateDevice
hr=0x... luid=...` — so a whole run greps out of a long ReShade log in one pass.

| What you see | What it means |
|---|---|
| Coherent flow field tracking the pattern | P0 passes. The architecture holds. |
| Pattern visible, flow field black | QuantMotion ran but found nothing. Check preset order and that `DEBUG_FLOW=1` is set. Also check `FRAME_COUNT` — the flow passes return zero on frame 0 by design. |
| Flow hue inconsistent across a uniformly scrolling pattern | Genuine flow failure. The pyramid's coarse level may be too small — check the window is 1280×720, not smaller. |
| Flow dim but directionally consistent | Correct. Magnitude is normalised against 15 px; 2 px/frame is meant to look faint. |
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

P0 exits when T1–T8 pass and `VENDOR_LOCK.md` carries the SHA, driver version,
ReBAR state and LumeniteFX release that made them pass. Only then does the
cross-adapter heap enter the picture: P1 adds transit from GPU 0, P2 widens it to
the full ring buffer, M2 adds the resolution-scale knob and the sweep for
T_fixed, and M3 introduces DLSS-NR against an already-characterised transport.

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
