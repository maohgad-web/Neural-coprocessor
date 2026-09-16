# Neural Coprocessor

## Project Overview

**A second GPU running a game's neural post-processing, while the first one renders.** Not SLI: nothing is split mid-frame. Neural rendering is a *terminal* stage. It takes a finished frame and returns a finished frame, so it can be picked up and executed somewhere else entirely.

**This is a ReShade add-on.** It is called **MGPU Bridge**, it is a `.addon64` file that ReShade loads into a D3D12 game, and every log line it writes is prefixed `[MGPU]` in `ReShade.log`. It is not a driver, not a patch, and not a replacement for anything, and it needs an **add-on-enabled** ReShade build to load at all. There is no game modification of any kind: the add-on reads each finished frame and does its work elsewhere.

This is research code with published measurements. It is not a product. Run games with anti-cheat and online games at your own risk.

* * *

## What changed in 0.2.0

**`nvngx_dlssnr.dll` now goes in a folder called `mgpu`, next to the add-on, instead of beside the game executable.** Some titles load anything named `nvngx_*.dll` that sits next to their executable. Loading it from its own subfolder keeps it out of their way.

**If you are upgrading from 0.1.0, move `nvngx_dlssnr.dll` into the `mgpu` folder and delete the copy next to the executable.** The panel says INSTALL PROBLEM if it finds one there.

Also in 0.2.0:

- **The game's own depth and motion vectors now reach DLSS 5 on the second card.** It derives motion from colour on its own, which is what 0.1.0 ran on. Feeding it the engine's real depth and velocity gives it ground truth instead of an estimate, and the gain is image stability: less jitter and sizzle on faces and fine geometry while the camera moves. On a frame that cannot supply them, a menu or a load screen, they are unbound rather than left pointing at the last frame's, and the derived motion takes over for as long as that lasts.
- **Both cards now have their own DLSS Super Resolution pass.** The second card can do its neural work at a smaller resolution and let DLSS scale the result back up to full resolution, which makes that work a lot cheaper. This is its own setting and does not depend on the game's DLSS, which can be set to anything, or turned off entirely. That gives a weaker second card a way to keep up with a stronger render card, and it is what makes DLSS 5 affordable on older titles where the card doing the neural rendering was struggling. It ships off, so neural rendering runs at full resolution out of the box.
- **General optimization, and lower latency at higher resolutions.** The two cards now balance the load between them rather than each running at its own pace, in either direction, so a mismatch in speed no longer builds into a backlog. That is also what paid for depth and motion vectors: what they cost to move still fits inside the one frame window 0.1.0 ran on. **The latency improvement requires Reflex.**

**0.2.1** - `mgpu_depth_tap.fx` no longer needs ReShade's standard effects pack. It included `ReShade.fxh` from that pack, so skipping the pack in the installer made the tap fail to compile and the bridge never arm. Super Resolution on the second card also gained two named panel modes, `Native Upscaling` (the default, and the higher quality of the two) and `Experimental Upscaler`.

**0.2.2** - fixes a startup crash on titles that ship NVIDIA Streamline, reported on Battlefield 6 SP Campaign ([#14](https://github.com/maohgad-web/Neural-coprocessor/issues/14)), and that title now receives the game's real motion vectors. The same report turned up a limitation: the engine's motion vectors are only available while the game itself is running DLSS or DLAA - with TAA the model falls back to deriving motion from colour, which is what 0.1.0 ran on. See Limitations.

**0.2.3** - three fixes:

- **iGPU detection is improved.** If you have an integrated GPU enabled alongside your two graphics cards, it no longer causes problems - and you do not have to disable it.
- **Depth and motion vector support for 007 First Light** ([#16](https://github.com/maohgad-web/Neural-coprocessor/issues/16)). The same fix also covers Cyberpunk 2077, where depth would not bind if you had Ray Reconstruction enabled.
- **The bridge window reports a bad install instead of waiting.** `ERROR 204` when the game's ReShade runtime never compiled the depth tap - almost always `EffectSearchPaths` - and `ERROR 205` for a missing `mgpu.ini`. Read-only: the add-on never writes your `ReShade.ini`.

* * *

## Performance Results

The Blood of Dawnwalker at 1920 x 1080, across the whole DLSS range, on two RTX 5060 Ti 16 GB:

| DLSS mode | DLSS 5 off | DLSS 5 on render card | DLSS 5 on second card |
| --- | --- | --- | --- |
| DLAA | 67-70 | 44 | 67-70 |
| Quality | 98-99 | 54-55 | 91 |
| Performance | 127-131 | 59 | 106-107 |
| Ultra Performance | 172 | 69-71 | 157 |

### Cost Analysis

| DLSS mode | Cost on render card | Cost on second card |
| --- | --- | --- |
| DLAA | -36% | **0%** |
| Quality | -45% | -8% |
| Performance | -54% | -17% |
| Ultra Performance | -59% | -9% |

### Efficiency Gains (DLAA to Ultra Performance)

| Configuration | Gain | Share of ceiling |
| --- | --- | --- |
| DLSS 5 off | +151% | - |
| DLSS 5 on second card | **+129%** | **86%** |
| DLSS 5 on render card | +59% | 39% |

**Key Finding:** Moving neural post-processing to a second GPU restores 86% of available performance gains from upscaling, compared to 39% when running on the render card.

Measured on 0.1.0. Not re-measured on 0.2.0.

* * *

## Titles run on 0.2.0

Each completed a bounded run, arming and exiting cleanly. The logs are at [docs/0.2.0](https://github.com/maohgad-web/Neural-coprocessor/tree/main/docs/0.2.0).

| Title | Resolution |
| --- | --- |
| Resonance - A Plague Tale Legacy | 2560x1440 |
| CONTROL Ultimate Edition (DX12 executable) | 2560x1440 |
| DragonSword Awakening | 2560x1411 |
| Cyberpunk 2077 | 3840x2160 |
| The Blood of Dawnwalker | 2560x1440 |

A launch and transport check, not a benchmark. The counters in those logs are cumulative from frame one, so a title that changes resolution while loading shows a miss count that stops growing once the engine settles.

### Reported by users

I cannot test every game, so any report helps.

| Title | Resolution | Reported in | State |
| --- | --- | --- | --- |
| Battlefield 6 (SP executable) | 3840x2160 | [#14](https://github.com/maohgad-web/Neural-coprocessor/issues/14) | Runs clean on 0.2.2 |
| 007 First Light | 3840x2160 | [#16](https://github.com/maohgad-web/Neural-coprocessor/issues/16) | Runs clean on 0.2.3 |

**Battlefield 6** SP Campaign, anti-cheat: online games are at your own risk ([issue #14](https://github.com/maohgad-web/Neural-coprocessor/issues/14)). On 0.2.0 and 0.2.1 this title crashed inside its own `sl.common.dll` on some machines every launch, and never delivered a motion vector frame on any of them. **Both are fixed in 0.2.2**, verified by [@Zonnery](https://github.com/Zonnery) on the machine that crashed every time.

What that rig measures on 0.2.2, at 3840x2160:

| Game's upscaling setting | Motion vectors reaching the model | Vector lane per frame |
| --- | --- | --- |
| DLSS on | 98% of frames | 1280x720, 3.7 MB |
| DLAA on | 96% of frames | 3840x2160, 33.2 MB |
| Neither (TAA) | **none** | - |

The last row is a limitation rather than a fault - see Limitations. Neural rendering itself runs in all three states.

**Not the add-on.** [@Zonnery](https://github.com/Zonnery) also saw `bf6.exe` crash on its own with `KERNELBASE.dll / 0x80070057`, reproduced with an empty game folder and no add-ons installed.

**007 First Light** ([#16](https://github.com/maohgad-web/Neural-coprocessor/issues/16)), reported and tested by [@Zonnery](https://github.com/Zonnery). 3840x2160, path tracing with Ray Reconstruction - game on an RTX 5090, neural stage on an RTX 5070 Ti. On 0.2.2 it armed and then stopped, because the game's ReShade runtime was never compiling the depth tap. Fixed in 0.2.3.

Both hotfixes were tested on [@Zonnery](https://github.com/Zonnery)'s hardware before they were released, and most of 0.2.3's new diagnostics exist because of those two threads. Thank you - see `ACKNOWLEDGEMENTS.md`.

* * *

## System Requirements

- **GeForce RTX 50-series cards** (primary requirement for DLSS Neural Rendering)
- **GeForce RTX 40-series cards** (confirmed working with modded `nvngx_dlssnr.dll`)
- **Two GPUs and two monitors** (one monitor per card). One monitor is not supported, but there is an unsupported workaround, and it is the other way round - the display goes on the neural card. See Display setup
- **Add-on-enabled ReShade build, 6.8.0 or newer**
- **DirectX 12 games only** (D3D11 and Vulkan unsupported)
- **No shader packs required**
- **No other add-ons** (to avoid multiple NGX consumers)

* * *

## How It Works

MGPU Bridge operates by:

1. Identifying the adapter the game renders on from the swapchain
2. Creating its own D3D12 device on a *different* adapter
3. Copying each finished frame across a cross-adapter shared heap with a 192-byte seal per ring slot for frame identity and ordering verification
4. Running DLSS Neural Rendering on the second adapter once or twice per frame
5. Presenting the result in its own window on the second adapter

The game's rendering is never touched; the bridge reads the finished frame and executes work elsewhere.

* * *

## Installation

See `assets/README.txt` for complete installation instructions. Critical requirements:

- ReShade must support add-ons (effects-only build will not load `.addon64` files)
- File name must contain the literal substring `nvngx.dll`
- **`nvngx_dlssnr.dll` goes in a folder called `mgpu`, next to the add-on. NOT beside the game executable.** Beside the executable some titles load it themselves and the neural stage will crash. This changed in 0.2.0.
- **`mgpu_depth_tap.fx` goes in ReShade's `Shaders` folder.** The add-on switches it on itself, so nothing needs enabling in the effects list. Without it ReShade never binds a depth buffer and the bridge waits instead of arming. On 0.2.0 it also needs ReShade's standard effects pack installed - or upgrade to 0.2.1 or newer, which removes that requirement.
- **And check `EffectSearchPaths` in the game's `ReShade.ini`.** Putting the file in the right folder is not enough: ReShade only compiles effects it finds through that setting, so if it does not point at the folder you put the tap in, the file is in the right place and is still invisible. A ReShade install that skipped the effect packages can leave it pointing at the game folder instead of `.\reshade-shaders\Shaders\**`. Only the **game's** runtime matters here - the bridge window has its own config that ships correct, which is why every other line in the log can look healthy while nothing arms. 0.2.3 prints `[MGPU][R142]` with your path and ours side by side, and shows `ERROR 204` on the bridge window.

* * *

## Display setup

**Two displays, extended, one per card.** That is the supported configuration and the only one measured.

Anything else is structural rather than a missing feature. The bridge presents the second card's output through its own swapchain, in its own window, so it needs somewhere to put that window. Sharing one screen with the game means presenting into the game's swapchain instead, which is a different architecture.

**One display, only one card connected.** It can still be made to work, on one cable, one display, two GPUs \- with the card that renders the game the one that has nothing plugged into it. The display goes on the neural card, a third\-party tool handles input, and a controller is required, because with the bridge window on top the mouse cannot reach the game.  Write\-up in [workarounds/single\-display](https://github.com/maohgad-web/Neural-coprocessor/tree/main/workarounds/single-display).

Two cables from two cards into one monitor was explored and did not reach anything worth shipping. A pull request is welcome if you find an arrangement that does.


* * *

## Controls

| Hotkey | Function |
| --- | --- |
| `CTRL+ALT+F10` | Arm the stream (in gameplay only) |
| `CTRL+ALT+F7` | View mode: neural output, input, split |
| `CTRL+ALT+left` / `right` | Move split seam (`SHIFT` for coarse step) |
| `CTRL+ALT+F8` | Choose which pass the intensity keys affect |
| `CTRL+ALT+F9` / `F11` | Intensity down / up |
| `Home` | ReShade overlay with control panel |

* * *

## Configuration

**mgpu.ini** key settings:

| Setting | Description |
| --- | --- |
| `Monitor=auto` | Which of the second card's outputs the window opens on |
| `AutoArm=1` | Arms stream automatically; set to `0` to arm manually |
| `SRUpscale=0` | DLSS Super Resolution on the second card. Off by default, so neural rendering runs at full resolution |
| `SRQuality=2` | 2 quality, 1 balanced, 0 performance |
| `SRScale=0` | Which resolution neural rendering runs at. `0` is Native Upscaling - the game's own render extent. `67`/`58`/`50` is Experimental Upscaler, chosen by the panel's mode buttons |
| `SRMvLowRes=0` | Rides with `SRScale` and is written with it. Never set one without the other |
| `Depth=1` | Send the game's depth to the second card and bind it. Needs `mgpu_depth_tap.fx` |
| `MVec=3` | Send the game's motion vectors and bind them. `0` leaves the model to its own derivation |
| `MvecFromEval=2` | Where the vectors are taken from. `2` is automatic: the usual route, falling back to the DLSS pass on engines where it never fires. New in 0.2.2 |
| `SRPreset=0` | 0 title default, 11 K, 12 L, 13 M |
| `Frames=0` | Stop after this many frames. `0` runs until you quit |

Changes to `mgpu.ini` are read when the bridge arms, so **restart the game after editing it.** The same applies to changing resolution or DLSS mode in the game's own settings while the bridge is running.

**Troubleshooting Note:** If output appears washed out, reduce the `tone` parameter in the panel (default `0.00` on development machine).

* * *

## Limitations

- **Engine motion vectors can need the game to be running DLSS or DLAA.** Measured on Battlefield 6: 96-98% of frames with DLSS or DLAA, none with TAA.
- **DLAA sends more data than DLSS.** It renders at native, so depth and motion vectors cross the link at full resolution.
- **No resolution/DLSS changes while armed** (requires swapchain rebuild)
- **Frame generation:** Untested and not recommended
- **Colour handling:** Not fully implemented; tone adjustment may be needed. Motion vectors for UI and HUD elements are still missing
- **External overlays:** Tools like RivaTuner/MSI Afterburner misbehave; use ReShade's built-in FPS display instead
- **D3D12 only:** No D3D11 or Vulkan support
- **Keyboard focus:** Interacting with the bridge window removes focus from the game, so a controller is recommended. On a single display this stops being a recommendation - the bridge window covers the screen and the mouse cannot reach the game at all. See Display setup

* * *

## Key Technical Claims

- Neural rendering is a *terminal* stage and can be decoupled from the render device
- The bridge operates as post-processing after the game finishes rendering
- Motion vectors are derived from colour using optical flow on the second card
- Engine depth and motion vectors cross the link and are bound alongside the derived motion
- These measurements represent a *floor*, not a ceiling (crude implementation without engine integration)

* * *

## Related Documentation Files

| File | Purpose |
| --- | --- |
| `RESULTS.md` | Detailed measurements, conditions, and caveats |
| `ARCHITECTURE.md` | Frame transfer mechanism and NGX integration |
| `METHOD.md` | Working principles and measurement costs |
| `VENDOR_LOCK.md` | Hardware, driver, and configuration details |
| `ACKNOWLEDGEMENTS.md` | Prior work, AI use disclosure, and disclaimer |
| `CONTRIBUTORS.md` | Code contributed by others |
| `THIRD_PARTY.md` | Licenses and provenance |
| `docs/` | Run logs from the titles listed above |
| `reference/` | Sample `ReShade.ini` and complete `ReShade.log` |
| `history/` | Milestone record and instrument design |
| `workarounds/` | Unsupported arrangements that worked here. Not part of the add-on |

* * *

## Important Disclaimers

- Not affiliated with, endorsed by, or supported by NVIDIA
- No warranty provided (see MIT LICENSE)
- Code reviewed before execution recommended
- Only tested on two machines with limited game titles
- Photon-to-photon latency not measured
- Image quality assessment not performed
- Settings and driver versions may affect results over time

*Neural Coprocessor is research code. Measurements and their conditions are in `RESULTS.md`.*
