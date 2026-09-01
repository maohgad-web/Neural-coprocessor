# MGPU Bridge — Cross-Adapter Bridge, Milestone 0 (Gate P0)

A ReShade add-on that stands up a **private D3D12 device on the second
GPU** of the game's process and runs LumeniteFX there — under stock,
unmodified ReShade, while the game renders on the first GPU.

P0 proves one thing: that a second, independent ReShade effect runtime
can exist on a second physical adapter inside a running game's process.
Nothing crosses between the cards yet — no transfer, no capture from
GPU 0, no DLSS-NR/NGX, no timing. The full brief, with scope rationale
and stop-and-report conditions, is in `Agent_Task.md` at the repo root.

## What the add-on does

1. Registers with stock ReShade (add-on API 20).
2. Enumerates DXGI adapters and selects GPU 1 **by LUID, not index** —
   index ordering is not stable across driver restarts, and a silent
   bind back to adapter 0 produces a "working" result that proves
   nothing.
3. Creates a private D3D12 device on that adapter.
4. Spawns a bridge thread that creates a small visible window (~640×360)
   and pumps messages — the window and its swapchain must be created and
   pumped on the same thread, or presentation hangs.
5. Creates a swapchain on the private device and builds the second
   effect runtime for it explicitly via `reshade::create_effect_runtime`
   (with `gpu1.ini` as its config). Every runtime ReShade auto-creates
   is additionally logged with its adapter LUID, as instrumentation —
   whether ReShade's hook adopts our swapchain is a recorded finding,
   not a gate.
6. Each frame: render the runtime's effect chain —
   `pattern → Kernel → LumaFlow → mv_debug` — into the back buffer and
   present. The window shows the scrolling test pattern transformed into
   a live motion-vector field, updating in real time.

The game's ReShade runtime (GPU 0) is never touched. All of our C++ is
device/window/swapchain creation and present; every pixel is rendered by
ReShade's own runtime on GPU 1, including the synthetic test pattern
that stands in for game content until transit exists.

## Build

Built by GitHub Actions only (`.github/workflows/build.yml`): the P0
containment check runs first, then ReShade's `include/` is fetched at
the pinned SHA, then the add-on compiles.

Download the artifact from the workflow run. It is exactly
**`mgpu_bridge.addon64`** — the extension replaces `.dll`; there is no
`.addon64.dll`, and CI fails the build if one appears.

## Deploy (on the rig, by hand)

1. An **add-on-enabled ReShade build** for the target game, at the
   release pinned in `VENDOR_LOCK.md`. Confirm a trivial add-on loads
   first — this add-on will not load under an add-on-disabled build,
   and that failure looks identical to a broken add-on.
2. **LumeniteFX**, stock and unmodified, in the game's ReShade shader
   path (`<game dir>\shaders\`), at the release recorded in
   `VENDOR_LOCK.md`.
3. `mgpu_bridge.addon64` → `<game dir>\reshade-addons64\`.
4. `assets\pattern.fx`, `assets\mv_debug.fx`, `assets\gpu1.ini` →
   `<game dir>\shaders\`.
5. Launch the game. A second window appears beside the game window;
   read the log.

## Reading the log

Every line this add-on writes carries a `[MGPU][Tn]` prefix (task id),
so a whole run is one grep of `<game dir>\ReShade.log`:

```
grep "\[MGPU\]" ReShade.log
```

Log the inputs to each decision, not just the outcome: the adapter
table, both LUIDs at the comparison, the HRESULT of every D3D call.

| What you see | What it means |
|-|-|
| Coherent flow field tracking the scrolling pattern | **P0 passes.** The architecture holds. |
| Pattern visible, flow field black | LumaFlow ran but found nothing, or the Kernel isn't feeding it — check preset order in `gpu1.ini` |
| Flow magnitude wrong against known motion (2 px/frame pattern) | LumaFlow is working but mis-scaled — a real finding only the synthetic pattern can surface |
| Window black, runtime logged as matched | Preset didn't load, or technique handles are stale — the log says what the lookup returned |
| Window frozen after N frames | Message pump, not rendering — check the window's owning thread first |
| Only one runtime init in the log | ReShade did not adopt our swapchain — stop and report, do not patch ReShade |
| Game crashes or hitches | Threading bug — something ran on the game thread that shouldn't; P0 touches no game resource |

## Files

The file manifest is closed — see the brief, section 05. `ext/reshade/`
is created by CI at build time, is gitignored, and is never committed.
