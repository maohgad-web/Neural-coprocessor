# VENDOR_LOCK — Milestone 0, Gate P0

The exact conditions under which P0 passed. Human-owned and human-observed:
nothing here can be derived from the repository. If a later run disagrees with
this file, this file describes the run that worked.

**P0 closed:** 2026-09-02.

---

## Repository

| | |
|---|---|
| Commit at close | `________` *(fill: final P0 commit SHA)* |
| Last CI-verified build | `b307367a` — green, zero warnings at `/W3`, artifact `mgpu_bridge.addon64` |
| ReShade headers | `crosire/reshade` @ `18deaa52de0c425a78b329e9cb3c497281cd00ec` |
| ReShade add-on API | 20 |
| CI runner | `windows-latest` → Visual Studio 18 2026, MSVC 19.51.36256.0 |
| Build command | `cmake -B build -A x64` — **no hardcoded generator**; the runner image has moved twice during P0 |
| Link libraries | `dxgi`, `d3d12` only |

## Rig

| | |
|---|---|
| CPU | Ryzen 5 5600X |
| Topology | Second GPU on **chipset** PCIe lanes, not CPU lanes. Deliberate worst case. An AM5 8×8 bifurcated CPU-lane system is the intended comparison rig. |
| GPU 0 (game) | NVIDIA RTX 5060 Ti 16GB — vendor `0x10DE`, device `0x2D04`, 16050 MB, `outputs=1` |
| GPU 1 (target) | NVIDIA RTX 5060 Ti 16GB — same vendor/device/memory, `outputs=0` |
| Driver | **616.56** |
| Display | ASUS VG27AQ5A, 2560×1440 @ **210 Hz**, 8-bit RGB, SDR, VRR not supported |
| ReBAR | **Disabled per-program** for the game executable |
| Inference server | llama-server **stopped** during every rig deploy — it otherwise occupies most of GPU 1's VRAM |

**Topology note.** The game renders on the adapter that has the display attached.
The bridge target is the **headless** adapter. An earlier assumption that game and
display sat on different cards was wrong and is corrected here.

**LUIDs observed during P0** — game `0x00000000-0x18A8C7AD`, target
`0x00000000-0x0001382B`. **Recorded for log archaeology only.** LUIDs are
reassigned across driver restarts and must never be persisted as configuration;
the add-on re-derives the target every session.

**Windows enumerates four DXGI adapters** on this rig: the two RTX 5060 Ti and
**two** "Microsoft Basic Render Driver" software adapters — of which only one
sets `DXGI_ADAPTER_FLAG_SOFTWARE`. Both are excluded by vendor id `0x1414`.

## Game and shaders

| | |
|---|---|
| Target game | Carnal Instinct v0.7.8 with HotFix — Unreal Engine 5, D3D12 |
| Game swapchain | 2560×1440, `R10G10B10A2_UNORM`, 4 buffers, flip, windowed |
| ReShade | **6.8.0.2155**, add-on-enabled build |
| LumeniteFX | `________` *(fill: release/version as installed)* — `lumenite_QuantMotion.fx` reports version 2026.06.16 |
| Deploy path | `<game>\Carnal_Instinct_UE5\Binaries\Win64\` — add-on beside `dxgi.dll` |
| Shader path | `…\Binaries\Win64\reshade-shaders\Shaders\` |

## Configuration that made P0 pass

`ReShade.ini` (game runtime, process-global add-on state):

```
[ADDON]
DisabledAddons=Effect Runtime Sync
```

`ReShade2.ini` (GPU 1 runtime — **written by ReShade**, not by us):

```
PresetPath=.\gpu1.ini
```

`gpu1.ini` (GPU 1 preset — root scope, no `[GENERAL]` header):

```
Techniques=Lumenite_QuantMotion@lumenite_QuantMotion.fx
[lumenite_QuantMotion.fx]
PreprocessorDefinitions=DEBUG_FLOW=1
```

## What passed, and the evidence

| Task | Evidence |
|---|---|
| T1 — add-on loads under stock ReShade | `[MGPU][T1] … API version 20` |
| T2 — adapter selection | `SELECTED adapter[1] luid=…0001382B rule="exclusion (luid != swapchain game luid) + software filter"` |
| T3 — device on GPU 1 | `D3D12CreateDevice hr=0x00000000`, both LUID checks pass |
| T4 — window + pump | `window created: … client=1280x720 (0,0,1280,720)`; closing it left the game running a further 23 s |
| T5 — swapchain + present | `present chain created: … CreateSwapChainForHwnd hr=0x00000000`; `frame 18600` reached; vsync-locked at 210.0 fps |
| T6 — second effect runtime | Satisfied without code: ReShade auto-created it — `Recreated runtime environment on runtime <ptr> ('ReShade2.ini')` on the bridge thread |
| T7 — shader executes on GPU 1 | Satisfied by configuration: QuantMotion enabled in `gpu1.ini` with `DEBUG_FLOW=1`; bridge window turns black, reproduced three times |
| T8 — teardown | `present chain released` → `device released` → `window destroyed` → `class unregistered` → `bridge thread exiting cleanly` |

**Why black proves execution.** The clear colour is `0.5 + 0.5·sin(φ)` on three
channels at 120° offsets, so the three always sum to 1.5 — it is never dark.
Black therefore cannot be the clear showing through. It is `PS_Debug` returning
`MotionToColor` of a zero flow field, which is the correct output for a
spatially uniform input.

## Measurements taken at P0 close

| Quantity | Value | Where from |
|---|---|---|
| Game frame rate, `DEBUG_FLOW` 1 / 0 / 1 | 45.86 / 45.77 / 45.90 fps | ReShade Statistics, in gameplay at 1440p |
| QuantMotion on the game's runtime | 0.183 ms CPU, 0.342 ms GPU, 15 passes | same |
| Motion-vector buffer `tFlow` | 320×180 `RG16F`, **0.220 MiB** | same |
| GPU 1 present loop | 210.0 fps, vsync-locked | 600 frames per 2.857 s, two runs |
| `CrossAdapterRowMajorTextureSupported` | **0** | read on the real adapter — P1 must use shared buffers with placed footprints |

**No no-bridge baseline was taken.** Every figure above was measured with the
bridge running. The cost of the add-on's mere presence, versus it being absent,
is unmeasured and is the first thing P1 should establish.

**The game's preset was contaminated during testing.** With the runtimes synced,
enabling QuantMotion wrote it into `ReShadePreset.ini`, so the game carried the
effect through the measurements above. It is constant across all three readings,
so the *deltas* hold; the absolute frame rate does not represent a clean game.

## Reproducing

1. Build via GitHub Actions at the commit above. Artifact is exactly
   `mgpu_bridge.addon64` — the extension replaces `.dll`.
2. Place it beside `dxgi.dll`. Place `gpu1.ini` in the same directory.
3. Apply the `ReShade.ini` and `ReShade2.ini` settings above **with the game
   closed** — ReShade rewrites both on exit.
4. Launch. Expect exactly **one** `bridge thread spawned` line; UE5's five
   add-on probe cycles must produce none.
5. Focus the bridge window, press **Home** for the GPU 1 overlay.

`ReShade.log` is overwritten on every launch. Copy it aside before relaunching.
