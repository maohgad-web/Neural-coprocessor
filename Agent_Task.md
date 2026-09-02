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
- **T4 — PASSED.** The bridge thread creates a 1280×720 window on itself, pumps
  messages in the same loop that polls `GetDeviceRemovedReason()`, and tears down
  in order. Verified on the rig: client rect exactly 1280×720, no
  `ERROR_CLASS_ALREADY_EXISTS` across all probe cycles, and closing the window
  left the game running for a further 23 seconds until a human quit it. Fixing
  T4's crash also closed a **pre-existing** process-killing race — the bridge
  thread used to be spawned on every probe cycle and could be executing inside
  the module when ReShade unmapped it. It is now started only from
  `init_swapchain`. Section 09 has the detail.
- **T5 — ACTIVE. This is your task.** Nothing beyond it is started.

You are joining an in-progress project. T1–T3 are working code that a human has
verified on the rig: **read them to understand the shape of the codebase, do not
revise them.** If something in T1–T3 looks wrong to you, say so in your report
and stop — do not fix it. A change there invalidates a hardware result that cost
a deploy cycle to obtain.

**The freeze is repo-wide over T1–T4 code, not per-file.** T5's work is confined
to three files, and within them to the changes listed here. These are the only
edits permitted anywhere in the repository:

1. **`gpu1_context.hpp`** — declare the three new functions T5 specifies, and
   add `<dxgi1_4.h>` if the declarations need it.
2. **`gpu1_context.cpp`** — implement them; add the DXGI include; extend
   `shutdown()` to release the present chain before the device. Everything
   already in this file, including `device_removed_reason()` and the LUID
   verification, stays as it is.
3. **`worker.cpp`** — the window style change, `ShowWindow`, the call into
   `create_present_chain`, the render loop, and the teardown reorder. All five
   are specified in T5. Nothing else in the file changes: `rearm()`, `stop()`,
   `ensure_started()`, `bridge_wndproc` and the class-registration block are
   finished code.

**Untouched, entirely:** `dllmain.cpp`, `adapter.hpp`, `adapter.cpp`, `diag.hpp`,
`diag.cpp`, `worker.hpp`, `CMakeLists.txt`, `README.md`, `.github/`.

If T5 appears to require a fourth file or a change not listed above, that is a
defect in this brief: report it and stop rather than deciding for yourself.

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

**When a choice is reversible and cheap, make it and move on.** You cannot ask
questions mid-task, so an unresolved decision has no way to end except by your
deciding it. Two outcomes that both compile, both behave correctly and differ
only in style are not a question worth resolving — pick one, note the choice and
the alternative in one line of your report, and continue. Stop-and-report is for
decisions that are *irreversible* or that would take you outside this brief: a
missing interface, a needed exception, a contradiction in these instructions.
Re-deriving a decision you have already implemented is not progress, and the step
budget it spends comes out of the task.

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

**Before a task is written, its requirements are checked against the interfaces
they assume.** Every requirement that says "call X on Y" presumes something
already exposes Y. Twice now a requirement has been written against an interface
that did not exist — the device-removal poll was specified with no way to reach
the device from the bridge thread, and before that the same poll was recorded as
implemented when nothing implemented it. Both are the same defect: a requirement
validated against intent rather than against the headers. Whoever writes the next
task reads the headers of every module the task will call into, and writes the
missing accessor into the spec as a numbered exception rather than leaving the
agent to discover it and decide alone. T5 creates a swapchain on the GPU 1
device, so `gpu1_context`'s surface is the first thing to settle before T5 is
written.

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

Implemented in `adapter.*`, verified on the rig. **Do not revise.** The rules and
their reasoning are documented at length in `adapter.hpp`'s header comment — read
that rather than a restatement here.

What it guarantees to everything downstream:

- A selection exists **only** if a swapchain-derived game LUID was established.
  `init_device` is provisional and never authorises a selection, because UE5
  creates a probe device on every adapter before settling.
- The selected adapter is hardware (not `DXGI_ADAPTER_FLAG_SOFTWARE`, not vendor
  `0x1414`), is not the game's, and is unique. Anything else refuses and selects
  nothing.
- Binding is by LUID, never by enumeration index.
- `selection_result::selected_adapter` is an AddRef'd `IDXGIAdapter1` owned by
  `adapter::shutdown()`.
- The decision is a one-shot latched by `S.decided`. That latch is load-bearing:
  after teardown the adapter table holds released pointers, and the latch is what
  stops a later event dereferencing them. Do not add anything that re-enters
  `on_device` or `on_swapchain`.

### T3 — Bridge thread and the GPU 1 device ✅ PASSED

Implemented in `worker.*` and `gpu1_context.*`, verified on the rig. **This is
the thread T5 builds on — read `worker.hpp` and `worker.cpp`'s header comment, do
not revise them.**

What it guarantees:

- One bridge thread per launch, started **only** from `init_swapchain`. Starting
  it from `init_device` spawned a thread per UE5 probe cycle that could be
  executing inside the module when ReShade unmapped it, and killed the game
  process. Section 09 has the detail. Do not move `ensure_started()`.
- Every GPU 1 object is created, used and destroyed on that thread. The game
  thread never blocks and never touches a GPU 1 object.
- `worker::stop()` is signal-only and never joins — safe from `DllMain` under the
  loader lock and from ReShade callbacks.
- `D3D12CreateDevice` runs against the T2 selection and verifies the resulting
  LUID in both directions. There is no fallback to the game's adapter.
- The `ID3D12Device *` never leaves `gpu1_context.cpp`. `has_device()` and
  `device_removed_reason()` are the guarded accessors, and they are callable from
  other threads — which is why the mutex exists.
- At process exit the teardown does not run and does not need to: Windows has
  already terminated the thread and reclaims everything.

### T4 — Window and message pump ✅ PASSED

Implemented in `worker.cpp`, verified on the rig. **T5 extends this loop, and
section 00 lists exactly which parts it may change. Everything not on that list
is finished code — read it, do not revise it.**

What it guarantees:

- A visible **1280×720 client area** window, created on the bridge thread only
  after `create_device` returned true. The size is fixed, not a placeholder: it
  keeps an optical-flow pyramid's coarsest level (`BUFFER_WIDTH/128`) at 10 px
  rather than degenerate. `AdjustWindowRect` converts it to outer dimensions.
- The window class name embeds the add-on's `HMODULE`, so a stale class from an
  unmapped module can never be reused. `ERROR_CLASS_ALREADY_EXISTS` is treated as
  success and logged at error level — it can only mean this same still-mapped
  module registered it and a prior teardown missed `UnregisterClass`.
- Class registration, window creation, message pumping, `DestroyWindow` and
  `UnregisterClass` all happen on the bridge thread. Nothing touches the window
  from a ReShade callback; `worker::stop()` stays signal-only.
- `WM_CLOSE` and `WM_DESTROY` signal the stop event and return 0. **Closing the
  bridge window does not close the game** — verified on the rig, the game ran a
  further 23 seconds until a human quit it.
- `WM_QUIT` is never the exit signal. It is drained and discarded; the stop event
  is the only shutdown signal for this thread.
- Teardown is ordered — `DestroyWindow`, `UnregisterClass`, `gpu1::shutdown()`,
  `adapter::shutdown()`, final logs — and completes **before** `rearm()`, so a
  replacement thread cannot race the cleanup. A permanently failed cycle closes
  the thread handle inline and does not re-arm.
- The device-removal poll T3 never built lives in this loop, via
  `gpu1::device_removed_reason()`, logged once on the transition away from `S_OK`.

Two known gaps, both fixed in T5, both recorded in section 09: the window takes
foreground activation when it appears, and it is resizable.

### T5 — Swapchain and a present loop ⬅ ACTIVE TASK

Create a swapchain on the GPU 1 device against T4's window and present a clear
colour every frame. This isolates "presentation works on GPU 1" from "the runtime
was created", so that when T6 fails you know which half broke.

**Section 09 subsections you need:** D3D12 / DXGI / Win32, and Environment. The
LumeniteFX subsections are not relevant to this task.

#### Where the code goes, and why

**All D3D12 objects live in `gpu1_context.*`. The window and the loop stay in
`worker.cpp`.** That split already exists and T5 extends it rather than moving
the line.

The reason is not abstraction for its own sake. `gpu1_context`'s mutex exists
because *other threads* call into it — `has_device()` and
`device_removed_reason()` are reachable from the game thread — and `shutdown()`
can release the device between another thread's read and its use. Handing the raw
`ID3D12Device *` out to `worker.cpp` would put every future caller outside that
guard. Everything T5 creates has the same property, so it goes behind the same
door.

Four functions, all **bridge thread only**:

```cpp
// Creates: command queue, swapchain (against hwnd), RTV heap, command
// allocator, command list, fence + event. Queries the window's client
// rect itself. Returns false with everything released on any failure.
bool create_present_chain(HWND hwnd);

// One frame: barrier to RENDER_TARGET, ClearRenderTargetView, barrier
// back to PRESENT, execute, Present, wait on the fence.
bool present_frame(float r, float g, float b);

bool has_present_chain();

// shutdown() is EXTENDED, not replaced: release the present chain in
// reverse creation order, then the device as it does today.
```

Nothing else changes in `gpu1_context`'s surface. **T6 will need the native
device, queue and swapchain pointers** to pass to `create_effect_runtime` — do
not add that accessor now, but do not structure the code so it becomes awkward
later.

#### The eight things that will otherwise cost a rig cycle

**1. `CreateSwapChainForHwnd`'s first object is the COMMAND QUEUE, not the
device.** This is the single most common D3D12 swapchain error. The parameter is
named `pDevice` and typed `IUnknown *`, and passing the device compiles, runs, and
fails at runtime with an unhelpful `E_INVALIDARG`. Create the command queue
first (`D3D12_COMMAND_LIST_TYPE_DIRECT`), pass **that**.

**2. The swapchain lands on the queue's adapter.** That is the entire mechanism by
which this swapchain is a *GPU 1* swapchain — there is no adapter parameter. The
queue comes from the T3 device, so the binding is already correct; do not look
for a way to specify the adapter.

**3. D3D12 requires the flip model.** `DXGI_SWAP_EFFECT_FLIP_DISCARD`,
`BufferCount = 2` or more, `SampleDesc.Count = 1`. The older `DISCARD` and
`SEQUENTIAL` effects and any MSAA count fail outright. Use
`DXGI_FORMAT_R8G8B8A8_UNORM`; `_SRGB` swapchain formats are not valid for the
flip model.

**4. `DXGI_SWAP_CHAIN_DESC1` is flat.** There is no `BufferDesc` member and no
`OutputWindow` — the HWND is a parameter of `CreateSwapChainForHwnd`, and
`Windowed` does not exist on this struct either (pass `nullptr` for
`pFullscreenDesc` to get a windowed swapchain).

**5. Call `factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER)` after
creating the swapchain.** Otherwise DXGI installs its own message hook on our
window and Alt+Enter toggles it to fullscreen — on a window we do not want
fullscreen, on the display the *game* is using.

**6. `CreateSwapChainForHwnd` yields `IDXGISwapChain1`.** `QueryInterface` for
`IDXGISwapChain3` to get `GetCurrentBackBufferIndex()`. Without it you are
tracking the index by hand and will drift.

**7. Barriers are mandatory.** `PRESENT → RENDER_TARGET` before the clear,
`RENDER_TARGET → PRESENT` after it. Omitting them is a debug-layer error and
undefined behaviour in release.

**8. Make the window non-resizable, and use the same style in both places.**
`WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX)` removes resize and
maximise, which removes `ResizeBuffers` from P0 entirely. **The identical style
value must be passed to `AdjustWindowRect`** — T4's client rect is exactly
1280×720 today and a style change in one place only would silently break it.
That number is load-bearing (see T4).

#### The loop changes shape

T4's loop waits with a 250 ms timeout because it had nothing to do. A present
loop cannot: vsync becomes the pacing mechanism.

```
for (;;) {
    if (WaitForSingleObject(stop_event, 0) == WAIT_OBJECT_0) break;
    drain messages with PeekMessageW / PM_REMOVE   (WM_QUIT discarded, as T4)
    present_frame(...)        // Present(1, 0) blocks on vblank, ~16 ms
    every 60th frame: device_removed_reason(), log once on the transition
}
```

`Present(1, 0)`, not `Present(0, 0)`: an uncapped loop would spin GPU 1 at full
rate for a clear, burn power, and add a variable the M2 measurements would
inherit. The device-removal poll moves from a 250 ms timer to a frame counter —
same intent, one loop.

**Keep the T4 no-window path exactly as it is.** When `pump` is false there is
nothing to present, and its `MsgWaitForMultipleObjects` / 250 ms structure stays.

#### Teardown order changes, and the GPU must be idle first

T4's teardown runs `DestroyWindow` → `UnregisterClass` → `gpu1::shutdown()` →
`adapter::shutdown()`. **That order is wrong once a swapchain exists**, because
the swapchain holds a reference to the window it was created against and would
outlive it. Reorder to:

```
gpu1::shutdown()      // present chain first, then the device
DestroyWindow
UnregisterClass
adapter::shutdown()
```

`gpu1::shutdown()` remains callable when no present chain was created, so the
no-window path is unaffected.

**Before releasing anything in the present chain, wait for the GPU to finish.**
`Present` is asynchronous: returning from it does not mean the queue has drained.
Releasing the swapchain, queue or command list while work is in flight is
undefined behaviour and presents as a crash or a hang at shutdown — the failure
mode T4 spent a rig cycle eliminating. Signal the fence one final time, wait on
it, and only then release, in reverse creation order.

#### Two fixes folded in from T4's rig run

**Show the window without stealing activation.** Drop `WS_VISIBLE` from
`CreateWindowExW` and call `ShowWindow(hwnd, SW_SHOWNOACTIVATE)` after creation.
As shipped, the window takes foreground when it appears, the game loses focus and
drops out of borderless-fullscreen to a bordered window with the taskbar showing.
Cosmetic now; contaminating at M2, where a game that lost foreground may present
differently. Section 09 has the detail.

**Colour must animate.** A static clear cannot distinguish "presenting" from
"presented once and hung". Cycle slowly — a few seconds per revolution, driven by
a frame counter in `worker.cpp`, not by a clock.

#### Logging

One line at creation with the swapchain format, buffer count, client size, and
the `HRESULT` of `CreateSwapChainForHwnd`. Then one line at the **first**
successful present, and one every 600 frames after it carrying the frame count —
enough to prove the loop is alive in a log a human reads afterwards, few enough
not to drown the file. Log the first `Present` failure and stop presenting; do
not log every frame's failure.

**Acceptance:** the window shows a smoothly animating colour while the game runs
normally on the other card; the log carries the creation line with `hr=0x00000000`
and a rising frame count; closing the window still tears down in order; and the
game does not lose borderless-fullscreen when the window appears.

**No new link libraries.** `dxgi` and `d3d12` are already in `CMakeLists.txt` and
cover everything here. If something appears to need a third, re-read section 09
before asking.

### T6 — Create the second effect runtime · not yet specified

**Intent.** Call `reshade::create_effect_runtime(device_api::d3d12, device,
queue, swapchain, "gpu1.ini", &runtime)` on the bridge thread and get a runtime
whose device carries the selected LUID. This is the milestone P0 exists to test —
everything before it is plumbing that only matters if this holds.

Not written out yet, deliberately. It will be specified against `gpu1_context`'s
actual surface once T5 has settled it, and against the pinned ReShade headers for
the exact signature. Section 09 records what is already verified about
`create_effect_runtime`.

**Free observation to collect while there.** Register `init_effect_runtime` and
log every runtime ReShade initialises, with its adapter LUID. That answers, at no
extra cost, whether ReShade's auto-hook also adopts our swapchain — and whether a
*third-party* add-on installed alongside us sees our second runtime and tries to
act on it. Whether other people's add-ons can ride this architecture is a real
question for what the project becomes; T6 is where the evidence is free.

### T7 — Drive a shader on GPU 1 · not yet specified

**Intent.** Execute an effect chain inside the T6 runtime, on GPU 1, driven by a
synthetic scrolling pattern, and see a coherent motion field in the window.

The motion source is an **input to this rig, not the subject of study**, and it is
selected by preset, so swapping it is configuration rather than code. LumeniteFX
`Lumenite_QuantMotion` with `DEBUG_FLOW=1` is the current candidate: already
installed, compiles on the rig every run, unconditionally depth-free, and ships
its own flow visualisation. Section 09 has the full analysis. It is not a
dependency. Writing our own pattern and flow visualiser is a live alternative
with two advantages — exact ground truth, because we generate the motion, and a
clean licence for anything later released. That choice is made when T7 is
written.

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
- **`README.md` is stale and is not maintained by any task.** It still describes a
  ~640×360 window (T4 fixes 1280×720) and a T7 pipeline of
  "pattern → Kernel → LumaFlow → mv_debug" with an `assets\mv_debug.fx` that the
  closed manifest does not contain — the real assets are `pattern.fx` and
  `gpu1.ini`, with `Lumenite_QuantMotion` and `DEBUG_FLOW=1`. Do not trust it over
  this document, and do not edit it.
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
  pinning the module with `GET_MODULE_HANDLE_EX_FLAG_PIN`: the module would stay
  mapped, but `DllMain` would stop running on subsequent loads, `register_addon`
  would never be called again, and ReShade would drop the add-on after the first
  cycle — a crash traded for silent non-function.
- **The leak that remains after that fix is accepted.** Because
  `stop()` is signal-only and the bridge thread performs its own teardown, a
  dynamic unload that unmaps the module before the thread wakes leaks the GPU 1
  device. T4 extends the same best-effort model to the window and class; it does
  not introduce the race and must not try to close it. On the rig,
  `destroy_device` fires before ReShade unregisters the add-on, so the thread
  exits first.
- **A visible window steals foreground activation from the game.** Creating the
  bridge window with `WS_VISIBLE` pulls focus off the game the moment it appears,
  and the game's borderless-fullscreen presentation drops to
  windowed-with-borders with the taskbar showing; clicking back on the game
  restores it. Cosmetic at P0. **Not cosmetic at M2** — a game that loses
  foreground can change presentation mode, throttle, or take a different present
  path, and every frame-timing number would inherit that. Create the window
  without `WS_VISIBLE` and show it with
  `ShowWindow(hwnd, SW_SHOWNOACTIVATE)`.
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
- **A fifth `D3D12CreateDevice` with `riid = {77ACCE80-...}` and
  `MinimumFeatureLevel = 1000`** fires after the game is up, on the game thread,
  and produces no `[MGPU]` line. Harmless and consistent across every run.
  Probably an NVIDIA or Streamline component. Recorded only so it is not mistaken
  for one of ours.

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
