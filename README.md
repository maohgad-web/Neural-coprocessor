# MGPU Bridge — Cross-Adapter Bridge, Milestone 0 (Gate P0)

A ReShade add-on that stands up a **private D3D12 device on the second GPU** of
the game's process, and runs a ReShade effect runtime there — under stock,
unmodified ReShade, while the game renders on the first GPU.

P0 proves one thing: that a second, independent ReShade effect runtime can exist
on a second physical adapter inside a running game's process. Nothing crosses
between the cards yet — no transfer, no capture from GPU 0, no DLSS-NR/NGX, no
timing.

**`Agent_Task.md` at the repo root is the authoritative document.** It carries the
scope rationale, the task specifications, the verified API facts and the
stop-and-report conditions. Where this README and the brief disagree, the brief
is right and this file is stale.

## Status

| Task | State |
|-|-|
| T1 — add-on registers under stock ReShade | passed on hardware |
| T2 — adapter enumeration and selection | passed on hardware |
| T3 — bridge thread and the GPU 1 device | passed on hardware |
| T4 — window and message pump | passed on hardware |
| T5 — swapchain and present loop | in progress |
| T6 — the second effect runtime | not started |
| T7 — drive a shader on GPU 1 | not started |
| T8 — teardown and run report | not started |

## What the add-on does

1. Registers with stock ReShade (add-on API 20).

2. Enumerates every DXGI adapter — LUID, description, vendor, VRAM, active
   outputs — and selects the one the game is **not** using, by LUID exclusion.
   Never by index: enumeration order is not stable, and the LUIDs themselves are
   reassigned across driver restarts.

   The game's own LUID is taken from the **swapchain**, not from the first
   device. UE5 creates a probe device on every adapter before settling, so an
   `init_device`-derived LUID is whichever probe happened to fire first — one run
   captured a software adapter as the "game" LUID. Software adapters are
   excluded by `DXGI_ADAPTER_FLAG_SOFTWARE` **or** Microsoft's vendor ID
   `0x1414`, because the flag alone is not reliable: this rig enumerates two
   "Microsoft Basic Render Driver" adapters and only one of them sets it.

   If filtering does not leave exactly one candidate, the add-on **selects
   nothing**. A missing device is diagnosable; a device on the wrong adapter
   succeeds, logs cleanly, and proves nothing.

3. Spawns the bridge thread — every GPU 1 object is created, used and destroyed
   on it — and creates the private D3D12 device on the selected adapter,
   re-verifying the device's LUID in both directions. There is no fallback to the
   game's adapter, by design.

   The thread is started **only** from `init_swapchain`. Starting it on device
   events spawned one thread per UE5 probe cycle, each of which could still be
   executing inside the module when ReShade unmapped it — which killed the game
   process intermittently.

4. Creates a visible **1280×720** window on that thread and pumps its messages
   there. A window belongs to the thread that created it, and only that thread
   may pump it or destroy it. The size is not arbitrary: it keeps the coarsest
   level of an optical-flow pyramid large enough to be meaningful.

5. Creates a swapchain on the private device against that window and presents
   every frame. *(T5, in progress.)*

6. Builds the second effect runtime explicitly via
   `reshade::create_effect_runtime`, with `gpu1.ini` as its config. *(T6, not
   started.)*

7. Renders an effect chain — a synthetic scrolling pattern, then a motion-vector
   pass — into the back buffer each frame, so the window shows a live flow field
   while the game renders on the other card. *(T7, not started.)*

The game's own ReShade runtime on GPU 0 is never touched. All of our C++ is
device, window, swapchain and present; every pixel is rendered by ReShade's own
runtime on GPU 1, including the synthetic pattern that stands in for game content
until transit exists.

## Build

GitHub Actions only — `.github/workflows/build.yml`. The P0 containment check
runs first, then ReShade's `include/` is fetched at the pinned SHA, then the
add-on compiles. There is no local build.

Download the artifact from the workflow run. It is exactly
**`mgpu_bridge.addon64`** — the extension *replaces* `.dll`; there is no
`.addon64.dll`, and CI fails the build if one appears.

## Deploy (on the rig, by hand)

1. An **add-on-enabled ReShade build** for the target game, at the release pinned
   in `VENDOR_LOCK.md`. Confirm a trivial add-on loads first — this add-on will
   not load under an add-on-disabled build, and that failure looks identical to a
   broken add-on.

2. **LumeniteFX**, stock and unmodified, in the game's shader path
   (`<game dir>\Binaries\Win64\reshade-shaders\Shaders\`), at the release
   recorded in `VENDOR_LOCK.md`. Needed from T7 onward.

3. `mgpu_bridge.addon64` → beside `dxgi.dll` in
   `<game dir>\Binaries\Win64\`. ReShade searches its own directory.

4. `assets\pattern.fx` and `assets\gpu1.ini` → the shader path above. Needed from
   T7 onward. There is no `mv_debug.fx` — the flow visualisation comes from
   LumeniteFX's own `DEBUG_FLOW=1` debug pass.

5. Launch the game. The bridge window appears beside it; read the log.

**`ReShade.log` is overwritten on every launch.** Copy it aside before relaunching
if a run is worth keeping.

## Reading the log

Every line this add-on writes carries a `[MGPU][Tn]` prefix (task id), so a whole
run is one grep of `<game dir>\Binaries\Win64\ReShade.log`:

```
grep "\[MGPU\]" ReShade.log
```

The add-on logs the *inputs* to each decision, not just the outcome: the full
adapter table, both LUIDs at the comparison, the HRESULT of every D3D call. The
log is the only view anyone has of runtime behaviour.

If the `[MGPU][T1]` line is missing, the add-on never registered. Check DebugView
(capturing the game process) for `[MGPU][T1] register_addon failed`: present
means ReShade refused the registration — an add-on-disabled build or an API
mismatch; absent means the file was never loaded.

### Expected shape of a healthy run

UE5 probes each adapter in turn, and ReShade loads and unloads the add-on once
per probe — **five load/unload cycles per launch on this rig**. Those cycles log
`[MGPU][T1]` and the adapter table and nothing else. Exactly **one**
`bridge thread spawned` line should appear in the whole log, at the real
swapchain, immediately before the T2 selection. More than one means the thread is
being started on device events again.

| What you see | What it means |
|-|-|
| `T2 SELECTED adapter[n] ... rule="exclusion ..."` | Selection worked. The named LUID is the target card. |
| `T2 REFUSING: ...` | No unique non-game hardware adapter. The line says which rule refused; the adapter table above it says why. Not a crash — the add-on declines to guess. |
| `T3 D3D12CreateDevice hr=0x00000000` | The GPU 1 device is live. |
| `T3 MISMATCH` or `T3 FATAL` | The device bound to an adapter other than the one selected, or to the game's. Both release and stop by design. |
| `T4 window created: ... client=1280x720` | The window is up on the bridge thread. A client rect other than 1280×720 means DPI scaling interfered. |
| `T4 ERROR_CLASS_ALREADY_EXISTS` | A previous cycle's teardown missed `UnregisterClass`. The run continues — the class is ours — but it is a teardown defect worth reporting. |
| `T4 WM_CLOSE` then the ordered teardown | You closed the bridge window. The game keeps running; this only shuts down our side. |
| No teardown lines at all, clean `Finished exiting` | Normal. At process exit Windows terminates the bridge thread before our stop signal reaches it, and reclaims everything. Quitting the game does **not** exercise the teardown — only closing the bridge window does. |
| Game exits during startup with the log ending mid-cycle | A crash inside an add-on unload. This was the `init_device` thread-spawn bug; if it returns, the fix regressed. |
| Game crashes or hitches while running | Something ran on the game thread that should not have. P0 touches no game resource. |

## Files

The file manifest is closed — see the brief, section 05. `ext/reshade/` is
created by CI at build time, is gitignored, and is never committed.
