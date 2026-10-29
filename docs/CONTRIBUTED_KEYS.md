# Contributed keys

Every `mgpu.ini` key that came from someone other than the primary author, in one place: who it came from,
what it does, and whether it changes the picture. The credit and the story behind each contribution are in
[`CONTRIBUTORS.md`](../CONTRIBUTORS.md).

The rule for all of them: **a contributed path stays off until its key is set.** With the key absent (or 0) the
add-on runs exactly as it would without the contribution. The one exception is a fix that repairs code that was
already running (`DrainGpu0` below); it says so.

All keys go in `mgpu.ini`, next to the add-on. Changes are read when the bridge arms: restart the game after
editing.

## lordzaxxon555-art - R300 (T1.19)

Full description, log tags and how each part was checked: [`docs/R300_T119.md`](R300_T119.md). With at least one
of these keys set, `ReShade.log` has one `[R300]` line at start listing which are on.

| Key | Values | What it does | Changes the picture |
|---|---|---|---|
| `SceneHandleSet` | `1` | The DLSS evaluate filter takes every latched scene feature, not only the newest. Fixes lost vectors when a game (Starfield) evaluates an older DLSS feature | Yes: more frames carry vectors |
| `FeatureDiag` | `1` | Logs each DLSS create / release, and says when the game stops evaluating its scene feature | No |
| `TemporalDiag` | `1` | Per-evaluate record: does the vector belong to the colour frame, ages, frames elapsed. `TemporalDiagLines` sets how many lines | No |
| `TemporalMV` | `multiply`, `single` | `single`: vectors at one game frame instead of the frames elapsed | Yes |
| `TemporalResetK` | `0`, `3`-`5` | Resets NR history when this many game frames passed | Yes |
| `TemporalRequireTuple` | `1` | Does not bind a vector that belongs to another frame | Yes |
| `JitterSlot` | `1` | Slot jitter delta instead of R104's (needs `JitterComp=1`). `JitterFallback`, `JitterSlotStamp`, `JitterDiag` tune it | Yes |
| `Perf11` | `1` | GPU 1 job timing and the copy-queue split. `PerfLines` sets how many lines | No |
| `PerfPrefetch` | `1` (needs `Perf11=1`) | Evaluates the frame the copy queue already armed instead of the newest. `PerfPrefetchMax` `0`-`4`. Experimental | Yes: which frame is evaluated |
| `ProducerDiag` | `1` | The producer's CPU cost on GPU 0, bytes sealed vs evaluated, backlog every `ProducerReport` frames | No |
| `Gpu0Timers` | `1` | GPU timestamps around the producer's copies. Writes queries into the game's command lists: compare fps on and off | No (may cost fps) |
| `SLTagsMvec` | `1` (needs `SLTags=1` or `2`) | The motion vectors the game declared to Streamline replace the ranked source | Yes |
| `SLTagsCapture` | `1`, `2` (needs `SLTags=1` or `2`) | Also copies the vectors at the Streamline tag call. `1`: eOnlyValidNow tags only. `2`: every lifecycle | Yes |
| `BetterConsoleGuard` | `1` | With BetterConsole.dll loaded, uses the composition mode instead of the bridge window (BetterConsole aborts the game in the window's swapchain call) | No |
| `DcompNoFallback` | `1` | When composition fails, no bridge window at all | No |
| `PanelAutoHeight` | `1` | The panel's two boxes use the overlay's height | Panel only |
| `PanelStats` | `1` | Game present cadence, derived interval, payload, vector age in the panel | Panel only |
| `PanelPersist` | `1` | Panel and hotkey changes are written back to `mgpu.ini` | Panel only |
| `FrameCap` | `0`, `20`-`500` | Caps the game's present. The panel section shows only when the line exists | Caps fps |
| `NRScaleResolveDepth` | `0` (absent or `1`: unchanged) | With `NRScale`, the residual composite runs without its depth guard while DLSS-NR keeps depth. Fixes stripes and a dark veil on shadowed faces (Starfield, `Depth=1`) | Yes: removes those artefacts |

With `PerfPrefetch=1`, the present gate waits for the frame the copy queue armed instead of `consumed+1`. Before,
the bridge thread spun on a frame that had already arrived. No new key: it is part of `PerfPrefetch`.

## NO1950

| Key | Values | What it does | Changes the picture |
|---|---|---|---|
| `PanelOverPicture` | `1` | Opens the panel on the GPU 1 runtime, over the live neural picture, instead of on the game's runtime. Composition mode (`DcompOverlay=1`) | Panel only |
| `NRScale` | `0.30` to `NRScaleCap` | Residual neural rendering at a reduced scale: DLSS-NR runs at this fraction of native and its difference is added back onto the native frame. `1` is on, at 1:1. Changed live from the panel (R299) | Yes |
| `NRScaleCap` | default `1.00` | How far the panel's scale slider may go | With `NRScale` |
| `NRSPass` | `1` (default), `2` | `2`: every pass the frame runs takes the NRS path at the same global `NRScale`. `1`: pass 1 only, the published behaviour. Requires `NRScale` | Yes, via the NRS stage |
| `NRScaleX`, `NRScaleY`, `NRSharpness`, `NRDetailGain`, `NRMode`, `NRColorStrength` | | Tuning of the `NRScale` path. Read only when the `NRScale` line is present | With `NRScale` |
| `NRProbe` | `1` | Measures the neural effect in the log (R299c). Works with or without `NRScale` | No |
| `NRSetAtCreate` | `1` | Applies the `Set.*` lines of `mgpu.ini` to DLSS-NR before its CreateFeature as well as at each evaluate (P6.2c), for keys that size the network. Does nothing without `Set.*` lines. Separate from `NRScale` | Only through the `Set.*` keys |
| `PanelSimple` | `1` | Shows the simple panel (Chinese labels, layout in `panel_simple.txt` beside the add-on) instead of the full panel. CTRL+ALT+F4 flips this key in game. Without `panel_simple.txt` the simple panel shows nothing and the full panel stays (R330) | Panel only |

The simple panel is R330 (`src/panel_simple.cpp`, `src/panel_simple.hpp`, `assets/panel_simple.txt`, the hotkey in
`src/worker.cpp`). Every later commit on it takes the next letter, one letter per squashed commit: R330a, R330b, ...
Labels inside the controls are `ui.<name> | label` lines in `panel_simple.txt`; a missing line shows the English text.

## MoHasan9505

| Key | Values | What it does | Changes the picture |
|---|---|---|---|
| `DrainGpu0` | absent or `1`: on. `0`: off | At teardown, waits (at most 2 s) for the game's queue before the shared transfer buffer is freed (R275). **On by default**: it repairs a drain that never ran. The add-on writes `0` here itself, with the reason in `DrainGpu0Off`, if the wait ever reaches 2 s | No |

His other fix, the four-argument NGX `Init` (R274), has no key: it is tried only after the five-argument call has
failed with `FAIL_OutOfDate`.

## grtninja

No keys. Both contributions (adapter selection, `mgpu.ini` parsing) are fixes to code that always runs.
