# MGPU Bridge — Cross-Adapter Bridge, Milestone 0 (Gate P0)

A ReShade add-on that stands up a **private D3D12 device on the second GPU** of
the game's process, and runs a ReShade effect runtime there — under stock,
unmodified ReShade, while the game renders on the first GPU.

**P0 is closed. It passed on 2026-09-02.**

A second, independent ReShade effect runtime exists on a second physical adapter
inside a running game's process, compiles a third-party shader, and executes it —
with the game's frame rate unaffected. Nothing crosses between the cards yet: no
transfer, no capture from GPU 0, no DLSS-NR/NGX.

`P0_RECORD.md` is the P0 record and the technical reference.
`VENDOR_LOCK.md` is the exact configuration that made it pass.

## Status

| Task | State |
|-|-|
| T1 — add-on registers under stock ReShade | passed |
| T2 — adapter enumeration and selection | passed |
| T3 — bridge thread and the GPU 1 device | passed |
| T4 — window and message pump | passed |
| T5 — swapchain and present loop | passed |
| T6 — second effect runtime | satisfied without construction |
| T7 — shader executes on GPU 1 | satisfied by configuration |
| T8 — teardown and run report | passed |

**T6 and T7 were never built.** ReShade auto-created an effect runtime for our
GPU 1 swapchain the moment T5 created it, gave it its own config (`ReShade2.ini`),
and compiled the whole shader library on the second adapter. Enabling
`Lumenite_QuantMotion` in a GPU 1 preset with `DEBUG_FLOW=1` executed it there —
no `create_effect_runtime` call, no rendering code, a preset file.

## What the add-on does

1. Registers with stock ReShade (add-on API 20).

2. Enumerates every DXGI adapter — LUID, description, vendor, VRAM, active
   outputs — and selects the one the game is **not** using, by LUID exclusion.
   Never by index: enumeration order is not stable, and LUIDs are reassigned
   across driver restarts.

   The game's LUID comes from the **swapchain**, not the first device: UE5 probes
   every adapter before settling, and one run captured a software adapter as the
   "game" LUID. Software adapters are excluded by `DXGI_ADAPTER_FLAG_SOFTWARE`
   **or** Microsoft's vendor ID `0x1414`, because the flag alone is unreliable —
   this rig enumerates two "Microsoft Basic Render Driver" adapters and only one
   sets it.

   If filtering does not leave exactly one candidate, the add-on **selects
   nothing**. A missing device is diagnosable; a device on the wrong adapter
   succeeds, logs cleanly, and proves nothing.

3. Spawns the bridge thread — every GPU 1 object is created, used and destroyed
   on it — and creates the private D3D12 device on the selected adapter,
   re-verifying the LUID in both directions. There is no fallback to the game's
   adapter, by design.

   The thread starts **only** from `init_swapchain`. Starting it on device events
   spawned one thread per UE5 probe cycle, each able to be executing inside the
   module when ReShade unmapped it — which killed the game process intermittently.

4. Creates a **1280×720** window on that thread and pumps its messages there. The
   size keeps an optical-flow pyramid's coarsest level non-degenerate.

5. Creates a swapchain on the private device and presents a slowly cycling colour
   every frame, vsync-paced.

6. ReShade then attaches its own effect runtime to that swapchain, and a GPU 1
   preset decides what runs there.

The game's ReShade runtime on GPU 0 is never touched.

## Build

GitHub Actions only — `.github/workflows/build.yml`. The P0 containment check
runs first, then ReShade's `include/` is fetched at the pinned SHA, then the
add-on compiles. There is no local build.

The artifact is exactly **`mgpu_bridge.addon64`** — the extension *replaces*
`.dll`; there is no `.addon64.dll`, and CI fails the build if one appears.

## Deploy

Everything goes in `<game>\Carnal_Instinct_UE5\Binaries\Win64\` — the directory
containing `dxgi.dll`.

1. An **add-on-enabled ReShade build** at the release pinned in
   `VENDOR_LOCK.md`. Confirm a trivial add-on loads first; this one will not load
   under an add-on-disabled build, and that failure looks identical to a broken
   add-on.
2. `mgpu_bridge.addon64` beside `dxgi.dll`.
3. `assets\gpu1.ini` into the same directory — the GPU 1 preset.
4. **LumeniteFX** in `…\Binaries\Win64\reshade-shaders\Shaders\`.
5. Apply the `ReShade.ini` and `ReShade2.ini` settings from `VENDOR_LOCK.md`
   **with the game closed** — ReShade rewrites both on exit, so edits made while
   it is running are lost.
6. Launch. The bridge window appears; press **Home** with it focused for the
   GPU 1 overlay.

**`ReShade.log` is overwritten on every launch.** Copy it aside before relaunching
if a run is worth keeping.

## Reading the log

Every line carries a `[MGPU][Tn]` prefix, so a run is one grep:

```
grep "\[MGPU\]" ReShade.log
```

The add-on logs the *inputs* to each decision, not just the outcome — the full
adapter table, both LUIDs at the comparison, the HRESULT of every D3D call. The
log is the only view anyone has of runtime behaviour.

If `[MGPU][T1]` is missing the add-on never registered. Check DebugView for
`[MGPU][T1] register_addon failed`: present means ReShade refused the
registration; absent means the file was never loaded.

### Expected shape of a healthy run

UE5 probes each adapter in turn and ReShade loads and unloads the add-on once per
probe — **five cycles per launch on this rig**. Those cycles log `[MGPU][T1]` and
the adapter table and nothing else. Exactly **one** `bridge thread spawned` line
should appear in the whole log, at the real swapchain.

| What you see | What it means |
|-|-|
| `T2 SELECTED adapter[n] … rule="exclusion …"` | Selection worked; that LUID is the target card. |
| `T2 REFUSING: …` | No unique non-game hardware adapter. The line names the rule; the table above it says why. Not a crash — the add-on declines to guess. |
| `T3 D3D12CreateDevice hr=0x00000000` | The GPU 1 device is live. |
| `T3 MISMATCH` / `T3 FATAL` | The device bound to the wrong adapter, or to the game's. Both release and stop by design. |
| `T4 window created: … client=1280x720` | Window up on the bridge thread. Any other client rect means DPI scaling interfered. |
| `T4 ERROR_CLASS_ALREADY_EXISTS` | A prior cycle's teardown missed `UnregisterClass`. The run continues — the class is ours — but it is a teardown defect worth reporting. |
| `T5 present chain created: … hr=0x00000000` | Swapchain live on GPU 1. |
| `T5 present loop alive: frame N` | Every 600 frames. A gap in the sequence is more interesting than its absence. |
| `Recreated runtime environment on … ReShade2.ini` | ReShade attached its own effect runtime to our GPU 1 swapchain. |
| Bridge window **black** with QuantMotion + `DEBUG_FLOW=1` | The shader executed. The clear colour can never be black — see `P0_RECORD.md` section 00. |
| No teardown lines, clean `Finished exiting` | Normal at process exit: Windows terminates the bridge thread before our stop signal reaches it. **Quitting the game does not exercise teardown — only closing the bridge window does.** |
| Log ends mid-cycle, game gone | A crash inside an add-on unload. This was the `init_device` thread-spawn bug; if it returns, the fix regressed. |

## Files

The manifest is closed — see `P0_RECORD.md` section 01. `ext/reshade/` is
created by CI at build time, is gitignored, and is never committed.
