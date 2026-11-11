# Neural Coprocessor

**A second GPU runs a game's DLSS Neural Rendering while the first one renders the game.**

### [Download the latest release](https://github.com/maohgad-web/Neural-coprocessor/releases)

Unpack into the folder containing the game's `.exe`. Full install steps and every known limitation are in `README.txt` inside the zip, and summarised below. The launcher is a separate download in the same release.

Research code with published measurements, not a product. Run games with anti-cheat and online games at your own risk.

* * *

## Project Overview

Not SLI: nothing is split mid-frame. Neural rendering is a *terminal* stage. It takes a finished frame and returns a finished frame, so it can be picked up and executed somewhere else entirely.

**This is a ReShade add-on.** It is called **MGPU Bridge**, it is a `.addon64` file that ReShade loads into a DirectX 12 or DirectX 11 game, and every log line it writes is prefixed `[MGPU]` in `ReShade.log`. It is not a driver, not a patch, and not a replacement for anything, and it needs an **add-on-enabled** ReShade build to load at all. There is no game modification of any kind: the add-on reads each finished frame and does its work elsewhere.

### Which card should run DLSS 5

Only the second card runs DLSS 5. That work uses its tensor cores, and above 1440p the cost is high.

Which card does which job is decided before the add-on loads. Windows renders the game on the card that drives its display, or on the card set under **Display settings > Graphics > Advanced graphics > Default high performance GPU**. The add-on takes the other card. It cannot choose, and no setting in `mgpu.ini` changes it.

**If your two cards are not evenly matched, experiment with inverting the setup.** Putting the stronger card on the DLSS 5 workload can give surprising results.

The Super Resolution options in the panel reduce that cost. They create a second DLSS Super Resolution feature on the neural card, separate from the one you set in the game, so it can do its work at a lower resolution. Both are off by default. `Native Upscaling` needs the game to be running DLSS. `Experimental Upscaler` chooses its own resolution, and is the one that works at DLAA.

Turn it on in the panel, or edit `mgpu.ini` yourself. You can find example `mgpu.ini` files [here](https://github.com/maohgad-web/Neural-coprocessor/tree/main/reference) - [DLSS Quality](https://github.com/maohgad-web/Neural-coprocessor/blob/main/reference/mgpuQUALITY.ini) and [DLSS Performance](https://github.com/maohgad-web/Neural-coprocessor/blob/main/reference/mgpuPERFORMANCE.ini).

**Residual NRS** is on by default since 0.5.0 and also reduces the cost. In the panel's Pass settings, each pass can run Residual Neural or Native, and Light-FP8 or Full-FP8. Light-FP8 is faster, and needs `nvngx_dlssnr.dll` 310.9.16 or newer.

* * *

## New in 0.5.0

**Choose NVIDIA's model.** With `nvngx_dlssnr.dll` 310.9.16 or newer, you can choose which of NVIDIA's two DLSS-NR models each pass uses: Light-FP8 is faster, Full-FP8 costs more but is much more stable.

**New default: Residual NRS.** The first pass now runs a residual neural pass at a reduced size (NRS), so the second GPU keeps up at higher frame rates. The big network (Full-FP8) runs on the first pass at x1 and on the last pass at x2, which removes the strobing when Structure is set high. For the 0.4.0 settings, set `NRS=0`, `NRPreset1=0` and `NRPreset2=0` in `mgpu.ini`, or choose Native and Light-FP8 in the panel.

**Live pass settings.** In the panel, each pass has its network (Light-FP8 or Full-FP8) and resolution (Residual Neural or Native). They change while you play, no restart. Best Quality-Performance puts the default back.

**Less impact on the game's FPS.** The bridge takes less from the GPU that renders the game.

**Frame generation on the second GPU (experimental).** The bridge can now generate frames on the second GPU, and it is much more stable than the game's own frame generation. Turn off the game's frame generation, then turn this on in the panel or set `FrameGen=4` in `mgpu.ini`. Please report your results.

**HDR displays (experimental).** The picture follows the game's HDR setting. Please report your results.

**3 and 4 passes (experimental)**, in `mgpu.ini`. Pass 1 scale (NRS) can now run on every pass.

**Fixes.** Games that use 3DMigoto now start.

**Launcher.** The Options page has the same pass settings. Optimization is Off or Performance; DLSS on GPU 1 has its own row.

Take the new `mgpu.ini`.

* * *

## New in 0.4.0

**Refactors and improvements.** Better performance and image stability in games that run DLSS. Less data goes to the second card, and the gain grows with resolution, so 4K should see the biggest gains. No more trailing after menus and loading screens.

**Reworked 2nd pass.** Better performance and better utilization of the second card. The 2nd pass now works on a smaller copy of the picture and adds its changes back at full size, so the fine detail it adds is a little softer. The old 2nd pass is still there as Full, under Experimental settings.

**Pass 1 scale (NRS)**, by N01950, is now a switch under Experimental settings. It reduces the cost of the first pass. It is an alternative to running an upscaling pass on the neural output. Test which one you like best against the upscaling options.

**A safe mode for DcompOverlay=1.** If you run into a DWM or driver issue with one display, such as a game that freezes or stops presenting, set `DcompSafeMode=3` in `mgpu.ini`. If that does not fix it, set `StartupTrace=1` and send me the log, so I can fix the problem quickly.

**Games without DLSS.** The first launch finds the right motion vectors faster and more reliably.

**Launcher.** A new Optimization row on the Options page: NRS, Native upscaling or Experimental upscaling, to compare against Off. Copy files replaces an old `mgpu.ini`, keeps your Dcomp settings, and can keep a backup.

Take the new `mgpu.ini`.

* * *

## New in 0.3.0

**DirectX 11 support.** DirectX 11 games now work, and it is on by default (`DX11=1`).

**Motion vectors for games without DLSS.** The add-on now finds the game's own motion vectors and depth when the game does not run DLSS. Tested on Resident Evil 4 (2023).

**Single display is now the default mode.** The add-on launches with `DcompOverlay=1`. One display, connected to the GPU that runs DLSS 5. No display connected to the GPU that renders the game. Use the launcher to verify that the GPU that renders the game is headless.

**Multi display support.** Set `DcompMultiDisplay=1` in `mgpu.ini`.

**Launcher (experimental).** A separate download. It should help you set up the add-on, check your setup, and manage your configurations. Comes in English, Português (Brasil), Español, 中文, 日本語 and 한국어.

See Display setup.

* * *

## What changed in 0.2.0

**`nvngx_dlssnr.dll` now goes in a folder called `mgpu`, next to the add-on, instead of beside the game executable.** Some titles load anything named `nvngx_*.dll` that sits next to their executable. Loading it from its own subfolder keeps it out of their way.

**If you are upgrading from 0.1.0, move `nvngx_dlssnr.dll` into the `mgpu` folder and delete the copy next to the executable.** The panel says INSTALL PROBLEM if it finds one there.

Also in 0.2.0:

- **The game's own depth and motion vectors now reach DLSS 5 on the second card.** It derives motion from colour on its own, which is what 0.1.0 ran on. Feeding it the engine's real depth and velocity gives it ground truth instead of an estimate, and the gain is image stability: less jitter and sizzle on faces and fine geometry while the camera moves. On a frame that cannot supply them, a menu or a load screen, they are unbound rather than left pointing at the last frame's, and the derived motion takes over for as long as that lasts.
- **Both cards now have their own DLSS Super Resolution pass.** The second card can do its neural work at a smaller resolution and let DLSS scale the result back up to full resolution, which makes that work a lot cheaper. This is its own setting and does not depend on the game's DLSS, which can be set to anything, or turned off entirely. That gives a weaker second card a way to keep up with a stronger render card, and it is what makes DLSS 5 affordable on older titles where the card doing the neural rendering was struggling. It ships off, so neural rendering runs at full resolution out of the box.
- **General optimization, and lower latency at higher resolutions.** The two cards now balance the load between them rather than each running at its own pace, in either direction, so a mismatch in speed no longer builds into a backlog. That is also what paid for depth and motion vectors: what they cost to move still fits inside the one frame window 0.1.0 ran on. **The latency improvement requires Reflex.**

* * *

## Release history

**0.2.1** - `mgpu_depth_tap.fx` no longer needs ReShade's standard effects pack. It included `ReShade.fxh` from that pack, so skipping the pack in the installer made the tap fail to compile and the bridge never arm. Super Resolution on the second card also gained two named panel modes, `Native Upscaling` (the default, and the higher quality of the two) and `Experimental Upscaler`.

**0.2.2** - fixes a startup crash on titles that ship NVIDIA Streamline, reported by [@Zonnery](https://github.com/Zonnery) on Battlefield 6 SP Campaign ([#14](https://github.com/maohgad-web/Neural-coprocessor/issues/14)), and that title now receives the game's real motion vectors. The same report turned up a limitation: the engine's motion vectors are only available while the game itself is running DLSS or DLAA - with TAA the model falls back to deriving motion from colour, which is what 0.1.0 ran on. See Limitations.

**0.2.3** - three fixes, all found from [@Zonnery](https://github.com/Zonnery)'s reports in [#14](https://github.com/maohgad-web/Neural-coprocessor/issues/14) and [#16](https://github.com/maohgad-web/Neural-coprocessor/issues/16):

- **iGPU detection is improved.** If you have an integrated GPU enabled alongside your two graphics cards, it no longer causes problems - and you do not have to disable it. Verified on a rig with the iGPU monitor attached.
- **Depth and motion vector support for 007 First Light** ([#16](https://github.com/maohgad-web/Neural-coprocessor/issues/16)). The same fix also covers Cyberpunk 2077, where depth would not bind if you had Ray Reconstruction enabled ([#14](https://github.com/maohgad-web/Neural-coprocessor/issues/14)).
- **The bridge window reports a bad install instead of waiting.** `ERROR 204` when the game's ReShade runtime never compiled the depth tap - almost always `EffectSearchPaths` - and `ERROR 205` for a missing `mgpu.ini`. Read-only: the add-on never writes your `ReShade.ini`.

**0.2.4** - one display is supported, and Native Upscaling is improved.

- **One display, one cable, no third-party tools.** `DcompOverlay=1` in `mgpu.ini` removes the bridge's own window, so mouse, keyboard and controller all work. See Display setup.
- **Native Upscaling is improved.** It now reads the render resolution the game declares to DLSS, which makes it available on more titles and improves stability in motion.
- **The panel reports your game's setting.** It says DLAA or DLSS, and names the setting to change when Native Upscaling cannot run.

**0.2.5** - the log now says which card is doing what, and refuses to run when that answer is wrong.

- **The binding is printed at arm**, read from the game's own device. Until now which card ran the neural stage had to be worked out from GPU utilisation, and more than one report was diagnosed backwards because of it.
- **`ERROR 207` when both stages land on one card.** Earlier versions ran that way in silence, where no measurement means anything.
- **Adapter selection is fixed for machines with three or more GPUs.** With an integrated GPU enabled and a single display, the bridge could pick the card the game renders on. Two-GPU machines are unaffected.
- **Display topology is reported** - how many display paths are active, which card owns them, and whether Windows' two APIs agree. They do not always.
- **Starfield support behind `SFPath`**, experimental. The game recreates its DLSS feature often and the add-on stopped following it. The detector runs on every title, and where it sees the fault the add-on sets the key itself - `SFPath=0` stops that.
- **Two log lines that read as faults no longer do.** `TAP = OFF` and the motion vector hand-off count were both stating true things that readers took as failures.

**0.3.0** - DirectX 11 support, motion vectors for games without DLSS, single display as the default mode, multi display support, and the launcher. See New in 0.3.0.

**0.3.1** - improves the input vector selector to support more setups and systems. Improves image stability and reduces flicker on those setups.

**0.4.0** - better performance and image stability in games that run DLSS, a reworked 2nd pass, NRS as a switch, a safe mode and a start-up log for one-display problems, and the launcher updated to match. See New in 0.4.0.

**0.5.0** - choose NVIDIA's model per pass, Residual NRS as the default, pass settings that change while you play, less impact on the game's FPS, and frame generation on the second GPU, HDR and 3 and 4 passes as experimental. See New in 0.5.0.

**0.5.1** - fixes washed-out HDR in games that turn HDR on after they start (`ChainColorSpace=auto`), and the version shown in the log.

* * *

## Performance Results

**Measured on 0.1.0.** The Blood of Dawnwalker at 1920 x 1080, across the whole DLSS range, on two RTX 5060 Ti 16 GB:

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

* * *

## Titles run on 0.5.0

| Title | API |
| --- | --- |
| Cyberpunk 2077 | DirectX 12 |
| Resident Evil 4 | DirectX 12 |
| Rise of the Tomb Raider | DirectX 12 |
| The Blood of Dawnwalker | DirectX 12 |
| DragonSword Awakening | DirectX 12 |
| Skyrim Special Edition | DirectX 11 |

## Titles run on 0.4.0

The logs are at [docs/0.4.0](https://github.com/maohgad-web/Neural-coprocessor/tree/main/docs/0.4.0).

| Title | API |
| --- | --- |
| Cyberpunk 2077 | DirectX 12 |
| Kingdom Come: Deliverance II | DirectX 12 |
| Rise of the Tomb Raider | DirectX 12 |
| The Blood of Dawnwalker | DirectX 12 |
| DragonSword Awakening | DirectX 12 |

## Titles run on 0.3.0

| Title | API |
| --- | --- |
| Resident Evil 4 (2023) | DirectX 12 |
| Rise of the Tomb Raider | DirectX 11 and DirectX 12 |
| Skyrim Special Edition | DirectX 11 |
| The Blood of Dawnwalker | DirectX 12 |
| Cyberpunk 2077 | DirectX 12 |
| Starfield | DirectX 12 |

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

I cannot test every game, so any report helps. The first two came from [@Zonnery](https://github.com/Zonnery), who found them and tested the fixes before either release shipped. Thank you.

| Title | Resolution | Reported in | State |
| --- | --- | --- | --- |
| Battlefield 6 (SP executable) | 3840x2160 | [#14](https://github.com/maohgad-web/Neural-coprocessor/issues/14) | Runs clean on 0.2.2 |
| 007 First Light | 3840x2160 | [#16](https://github.com/maohgad-web/Neural-coprocessor/issues/16) | Runs clean on 0.2.3 |
| RoboCop Rogue City - Unfinished Business | 2560x1440 | [#29](https://github.com/maohgad-web/Neural-coprocessor/issues/29) | Runs clean on 0.2.5 |
| Cyberpunk 2077, Kingdom Come: Deliverance II, Hogwarts Legacy | 3840x2160 | [#52](https://github.com/maohgad-web/Neural-coprocessor/issues/52) | Confirmed not working on one setup since 0.3.0 (below) |

**Battlefield 6** SP Campaign ([#14](https://github.com/maohgad-web/Neural-coprocessor/issues/14)). On 0.2.0 and 0.2.1 this title crashed inside its own `sl.common.dll` on some machines every launch, and never delivered a motion vector frame on any of them. **Both are fixed in 0.2.2**, verified on the machine that crashed every time. Anti-cheat: online games are at your own risk.

What that rig measures on 0.2.2, at 3840x2160:

| Game's upscaling setting | Motion vectors reaching the model | Vector lane per frame |
| --- | --- | --- |
| DLSS on | 98% of frames | 1280x720, 3.7 MB |
| DLAA on | 96% of frames | 3840x2160, 33.2 MB |
| Neither (TAA) | **none** | - |

The last row is a limitation rather than a fault - see Limitations. Neural rendering itself runs in all three states.

**Not the add-on.** The same rig also saw `bf6.exe` crash on its own with `KERNELBASE.dll / 0x80070057`, reproduced with an empty game folder and no add-ons installed.

**007 First Light** ([#16](https://github.com/maohgad-web/Neural-coprocessor/issues/16)), 3840x2160, path tracing with Ray Reconstruction - game on an RTX 5090, neural stage on an RTX 5070 Ti. On 0.2.2 it armed and then stopped, because the game's ReShade runtime was never compiling the depth tap. Fixed in 0.2.3.

**RoboCop Rogue City - Unfinished Business** ([#29](https://github.com/maohgad-web/Neural-coprocessor/issues/29)), 2560x1440, on an RTX 3080 Ti with an RTX 5060 Ti. Reported as depth and motion vectors going silent after a reboot, on a machine where display ownership kept moving between the two cards. It did not reproduce on 0.2.5, in either a single- or a dual-display arrangement, and that build is what settled which card was doing what.


* * *

## System Requirements

- **GeForce RTX 50-series cards** (primary requirement for DLSS Neural Rendering)
- **GeForce RTX 40-series cards** (confirmed working with modded `nvngx_dlssnr.dll`)
- **Two GPUs.** One display, connected to the GPU that runs DLSS 5, and no display connected to the GPU that renders the game. More than one display: `DcompMultiDisplay=1` - see Display setup
- **Add-on-enabled ReShade build, 6.8.0 or newer**
- **DirectX 12 or DirectX 11 games** (Vulkan unsupported). On some Unity titles DirectX 12 is still better - see Forcing D3D12 on Unity titles
- **No shader packs required**
- **No other add-ons** (to avoid multiple NGX consumers)

* * *

## How It Works

MGPU Bridge operates by:

1. Identifying the adapter the game renders on from the swapchain (DirectX 12) or the game's device (DirectX 11)
2. Creating its own D3D12 device on a *different* adapter
3. Copying each finished frame across a cross-adapter shared heap with a 192-byte seal per ring slot for frame identity and ordering verification
4. Running DLSS Neural Rendering on the second adapter once or twice per frame
5. Presenting the result on the display connected to the second adapter, drawn onto the game's own window (the default, `DcompOverlay=1`) or in its own window

The game's rendering is never touched; the bridge reads the finished frame and executes work elsewhere.

* * *

## Installation

**[Get the zip from Releases](https://github.com/maohgad-web/Neural-coprocessor/releases)**, then follow `README.txt` inside it. The launcher, a separate download in the same release, has the install guide and an install check for the steps below. Critical requirements:

- ReShade must support add-ons (effects-only build will not load `.addon64` files)
- File name must contain the literal substring `nvngx.dll`
- **`nvngx_dlssnr.dll` goes in a folder called `mgpu`, next to the add-on. NOT beside the game executable.** Beside the executable some titles load it themselves and the neural stage will crash. This changed in 0.2.0.
- **`mgpu_depth_tap.fx` goes in ReShade's `Shaders` folder.** The add-on switches it on itself, so nothing needs enabling in the effects list. Without it ReShade never binds a depth buffer and the bridge waits instead of arming. On 0.2.0 it also needs ReShade's standard effects pack installed - or upgrade to 0.2.1 or newer, which removes that requirement.
- **And check `EffectSearchPaths` in the game's `ReShade.ini`.** Putting the file in the right folder is not enough: ReShade only compiles effects it finds through that setting, so if it does not point at the folder you put the tap in, the file is in the right place and is still invisible. A ReShade install that skipped the effect packages can leave it pointing at the game folder instead of `.\reshade-shaders\Shaders\**`. Only the **game's** runtime matters here, and when it is wrong every other line in the log still looks healthy - so check that `[MGPU][P1.6] GAME runtime` reports a non-zero technique count. 0.2.3 prints `[MGPU][R142]` with your path and ours side by side, and shows `ERROR 204` on the bridge window.

* * *

## Display setup

**One display, one cable. The default since 0.3.0.** `DcompOverlay=1` is set in `mgpu.ini`. The display is connected to the GPU that runs DLSS 5, and no display is connected to the GPU that renders the game. Use the launcher to verify that the GPU that renders the game is headless. The bridge creates no window of its own and draws the neural output onto the game's window instead. The game keeps its input: mouse, keyboard and controller all work. No third-party tools.

In this mode the neural output is the topmost composition visual on the game's window, so it sits above the game's own ReShade overlay. You do not have to do anything about that: open the overlay with its normal key and the neural output steps aside by itself, then comes back when you close it.

`CTRL+ALT+F6` does that by hand - it unroots the visual and roots it again, and never touches the stream. It is registered only in this mode, and most people will never need it.

**More than one display.** Set `DcompMultiDisplay=1` in `mgpu.ini`.

**Two displays, extended, one per card, with the bridge's own window.** Set `DcompOverlay=0`. The bridge presents the second card's output in its own window on the second screen. Every published figure was measured this way, and it is what I develop on - a window of its own is what lets the bridge's swapchain be isolated from the game's when I am debugging.

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
| `CTRL+ALT+F6` | Unroot the neural output and root it again. `DcompOverlay=1` only, and rarely needed - see Display setup |
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
| `SRMvLowRes=2` | Rides with `SRScale` and is written with it. Never set one without the other. Changed from `0` in 0.2.4 |
| `Depth=1` | Send the game's depth to the second card and bind it. Needs `mgpu_depth_tap.fx` |
| `MVec=3` | Send the game's motion vectors and bind them. `0` leaves the model to its own derivation |
| `MvecFromEval=2` | Where the vectors are taken from. `2` is automatic: the usual route, falling back to the DLSS pass on engines where it never fires. New in 0.2.2 |
| `Calib=2` | How the add-on reads the game's own DLSS calls. `0` turns the whole tap off |
| `CalibRung=0` | Which install route the calibrator may use. `0` both, `1` the import table alone, `2` the cached-pointer scan alone. Leave at `0` unless a title faults at startup. New in 0.2.2 |
| `SRPreset=0` | 0 title default, 11 K, 12 L, 13 M |
| `DcompOverlay=1` | Draws the neural output onto the game's own window instead of opening one. The default since 0.3.0. `0` opens the bridge's own window - see Display setup |
| `DcompMultiDisplay` | Absent by default. `1` for more than one display - see Display setup. New in 0.3.0 |
| `DX11=1` | DirectX 11 support. On by default. New in 0.3.0 |
| `NgxProbeAfterScene` | Absent by default, which means `1`: the add-on starts its DLSS 5 after the game's own DLSS is running. `0` starts it at the game's first frame, as in 0.3.0. New in 0.3.1 |
| `DepthHoldMax` | Absent by default, which means 180 seconds: how long the bridge waits for the game's depth while the game's DLSS is running, before it starts without depth. `0` waits without a limit. New in 0.3.1 |
| `NRS=1` | Residual NRS on the first pass. On by default since 0.5.0. `0` runs Native, as in 0.4.0 |
| `NRPreset1=1` / `NRPreset2=1` | Which NVIDIA model each pass uses. `1` Full-FP8, `2` Light-FP8, `0` as in 0.4.0. Light-FP8 needs `nvngx_dlssnr.dll` 310.9.16 or newer. New in 0.5.0 |
| `FrameGen=0` | Frame generation on the second GPU. `4` turns it on. Experimental. New in 0.5.0 |
| `ChainColorSpace=auto` | The picture follows the game's HDR setting. Experimental. New in 0.5.0 |
| `Passes=1` | How many passes run. `3` and `4` are experimental. New in 0.5.0 |
| `NRFrameFilter=0` | The frame-generation vector filter, in the panel's Experimental settings. Off in the shipped file since 0.3.1 |
| `SFPath` | Absent by default, and absent is not off: the detector runs, the repair does not, and the add-on sets `1` itself if it sees the fault. `0` stops that. New in 0.2.5 |
| `Frames=0` | Stop after this many frames. `0` runs until you quit. A bounded run is what prints a summary |

Keys contributed by others (`NRScale`, `PanelOverPicture`, the R300 keys and more) are listed in [`docs/CONTRIBUTED_KEYS.md`](docs/CONTRIBUTED_KEYS.md). All are off until set.

Changes to `mgpu.ini` are read when the bridge arms, so **restart the game after editing it.** The pass settings in the panel are the exception: they change while you play. The same applies to changing resolution or DLSS mode in the game's own settings while the bridge is running.

**Troubleshooting Note:** If output appears washed out, reduce the `tone` parameter in the panel (default `0.00` on development machine).

**Reading latency.** `lat=` on a `[MGPU][SEAL] new` line in `ReShade.log` is one frame, logged every 61st frame. The panel's `submit-to-consume` is the mean of the same measure over the whole run. The panel's `ready-to-consume` is a different measure. The three numbers do not have to match.

* * *

## Forcing D3D12 on Unity titles

Optional since 0.3.0: DirectX 11 games run. On some games DirectX 12 is still better, because the game runs the full NGX suite and the add-on gets the correct motion vectors. Many Unity titles ship a D3D12 renderer and do not pick it by default. Adding `-force-d3d12` to the launch arguments switches them over.

**Steam.** Right-click the game, Properties, General, and put this in Launch Options:

```
-force-d3d12 %command%
```

**A desktop shortcut.** Right-click the shortcut, Properties, and add the flag at the end of the Target field, outside the quotes:

```
"C:\Games\<title>\<title>.exe" -force-d3d12
```

**Other launchers** take the same flag wherever they accept command-line arguments.

This only works if the title actually carries a D3D12 renderer - there is nothing to force if it does not. Without one, the game runs in DirectX 11, which is supported since 0.3.0.

* * *

## Choosing the render card on Unreal Engine titles

Some Unreal Engine games pick their own GPU and ignore Windows Graphics settings. If the game renders on the card that should run DLSS 5, the two cards swap roles. On some setups the bridge then cannot start DLSS 5, and the stream runs without neural rendering.

**Steam.** Right-click the game, Properties, General, and put this in Launch Options:

```
-graphicsadapter=N %command%
```

N is the number of the card that should render the game: the card with no monitor. The `adapter[N]` lines in `ReShade.log` list your cards with their numbers. If the game still renders on the wrong card, try the other number.

**A desktop shortcut** takes the same flag at the end of the Target field, outside the quotes.

* * *

## Limitations

- **Engine motion vectors can need the game to be running DLSS or DLAA.** Measured on Battlefield 6 (0.2.2): 96-98% of frames with DLSS or DLAA, none with TAA. Since 0.3.0 the add-on also finds the game's own motion vectors on games without DLSS, tested on Resident Evil 4 (2023).
- **DLAA sends more data than DLSS.** It renders at native, so depth and motion vectors cross the link at full resolution.
- **No resolution/DLSS changes while armed** (requires swapchain rebuild)
- **The game's frame generation:** Not recommended. It still flickers, as in 0.3.x; use the bridge's frame generation instead (experimental, new in 0.5.0). Some games with the game's frame generation on can crash at startup. Workaround: set `AutoArm=0` in `mgpu.ini`, get into gameplay, and press `CTRL+ALT+F10` to arm by hand.
- **Colour handling:** Not fully implemented; tone adjustment may be needed. Motion vectors for UI and HUD elements are still missing
- **External overlays:** Tools like RivaTuner/MSI Afterburner misbehave; use ReShade's built-in FPS display instead
- **Unreal Engine titles can ignore Windows Graphics settings** and render on the wrong card. See Choosing the render card on Unreal Engine titles
- **No Vulkan support.** DirectX 12 and DirectX 11 only. Some Unity titles can be forced to D3D12, optional since 0.3.0 - see Forcing D3D12 on Unity titles
- **Keyboard focus:** With `DcompOverlay=0`, interacting with the bridge window removes focus from the game, so a controller is recommended. With the default `DcompOverlay=1` there is no bridge window to take focus. See Display setup

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
| `docs/CONTRIBUTED_KEYS.md` | Every `mgpu.ini` key contributed by others: who, what, and whether it changes the picture |
| `THIRD_PARTY.md` | Licenses and provenance |
| `docs/` | Run logs from the titles listed above |
| `reference/` | Sample `ReShade.ini`, a complete `ReShade.log`, and two example `mgpu.ini` configurations with Super Resolution on |
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
