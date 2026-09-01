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
2. Captures the game's own adapter LUID from the game's D3D12 device,
   enumerates every DXGI adapter (LUID, description, VRAM, active
   outputs), and selects the adapter whose LUID differs from the
   game's — **by exclusion, never by index**. Index order is not stable
   across driver restarts, and a silent bind back to the game's own
   card produces a "working" result that proves nothing. Output counts
   are used only as a tie-break when more than two adapters are
   present; a degenerate selection is logged loudly, and a single-
   adapter topology refuses to select at all.
3. Spawns the bridge thread — every GPU 1 object is created, used and
   destroyed on it — and creates the private D3D12 device on the
   selected adapter, re-verifying the device's LUID against the
   selection. There is no fallback to the game's adapter, by design.
4. Creates a small visible window (~640×360) on that thread and pumps
   messages — the window and its swapchain must be created and pumped
   on the same thread, or presentation hangs.
5. Creates a swapchain on the private device and builds the second
   effect runtime explicitly via `reshade::create_effect_runtime`
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

If the `[MGPU][T1]` line is missing, the add-on never registered.
Check DebugView (capturing the game process) for
`[MGPU][T1] register_addon failed`: present means ReShade refused the
registration (add-on-disabled build or API mismatch); absent means the
add-on file was never loaded.

| What you see | What it means |
|-|-|
| Coherent flow field tracking the scrolling pattern | **P0 passes.** The architecture holds. |
| Pattern visible, flow field black | LumaFlow ran but found nothing, or the Kernel isn't feeding it — check preset order in `gpu1.ini` |
| Flow magnitude wrong against known motion (2 px/frame pattern) | LumaFlow is working but mis-scaled — a real finding only the synthetic pattern can surface |
| Window black, runtime creation logged as OK | Preset didn't load, or technique handles are stale — the log says what the lookup returned |
| No explicit runtime line, or it carries a failure HRESULT | `gpu1.ini` missing or `create_effect_runtime` refused — the line carries the HRESULT and the config path |
| An auto-hooked runtime with our LUID in the log | Informational only — the runtime is created explicitly; whether ReShade's hook adopts our swapchain is a recorded finding, not a gate |
| `[MGPU][T2] selection DEGENERATE` | The topology gave no unique non-game adapter — confirm the selection against the adapter table before trusting anything downstream |
| `D3D12CreateDevice hr=0x...` non-zero | Adapter 1 device creation failed — report the HRESULT and the adapter table; there is no fallback to the game's adapter by design |
| `device_removed` with the game's LUID | The game's device was reset (driver TDR, etc.) — P0 creates nothing on it; investigate the game/driver, not the add-on |
| Window frozen after N frames | Message pump, not rendering — check the window's owning thread first |
| Game crashes or hitches | Threading bug — something ran on the game thread that shouldn't; P0 touches no game resource |

## Files

The file manifest is closed — see the brief, section 05. `ext/reshade/`
is created by CI at build time, is gitignored, and is never committed.
