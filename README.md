# Neural Coprocessor

**A second GPU running a game's neural post-processing, while the first one
renders.** Not SLI nothing is split mid-frame. Neural rendering is a *terminal*
stage: it takes a finished frame and returns a finished frame, so it can be
picked up and executed somewhere else entirely.

**This is a ReShade add-on.** It is called **MGPU Bridge**, it is a `.addon64`
file that ReShade loads into a D3D12 game, and every log line it writes is
prefixed `[MGPU]` in `ReShade.log`. It is not a driver, not a patch, and not a
replacement for anything it needs an **add-on-enabled** ReShade build to load
at all. There is no game modification of any kind: the add-on reads each
finished frame and does its work elsewhere.

This is research code with published measurements. It is not a product.

**See it running** the game on one card, the neural output in its own window on
the other, both live in a single take:

- [The Blood of Dawnwalker](https://youtu.be/yoEsuZyltFc) the title every
  measurement in [RESULTS.md](RESULTS.md) was taken on
- [Cyberpunk 2077](https://youtu.be/XWv5jw90yHc) gameplay, played normally.
  **No figure in this repository comes from Cyberpunk**; it was run for
  compatibility, stability and power draw only.

Both are demonstrations, not benchmarks screen-recording overhead is present in
each. The measured runs were separate and unrecorded.

| | |
|---|---|
| [RESULTS.md](RESULTS.md) | what was measured, on what, with what caveats |
| [ARCHITECTURE.md](ARCHITECTURE.md) | how a frame gets to the second GPU and back |
| [METHOD.md](METHOD.md) | the working rules, and what they cost to learn |
| [VENDOR_LOCK.md](VENDOR_LOCK.md) | the machine, driver and configuration every number was taken on |
| [ACKNOWLEDGEMENTS.md](ACKNOWLEDGEMENTS.md) | prior work, AI use, disclaimer |
| [THIRD_PARTY.md](THIRD_PARTY.md) | licences and provenance |
| [history/](history/) | the milestone record, the verified-facts ledger, the instrument design |

---

## The result

One game, at 1920 × 1080, across the whole DLSS range on dual 5060 ti 16GB's. **Three arms**, so the
neural stage can be priced rather than only compared against itself: the game
with no neural rendering at all, the neural stage on the render GPU, and the
neural stage on the second GPU. Method and caveats in [RESULTS.md](RESULTS.md).

| DLSS mode | no neural rendering | neural on the **render** GPU | neural on the **second** GPU |
|---|---|---|---|
| DLAA | 67–70 | 44 | 67–70 |
| Quality | 98–99 | 54–55 | 91 |
| Performance | 127–131 | 59 | 106–107 |
| Ultra Performance | 172 | 69–71 | 157 |

The first column is the ceiling what the machine does when nothing neural is
running. Against it, this is what the neural stage **costs the game**:

| DLSS mode | on the render GPU | on the second GPU |
|---|---|---|
| DLAA | −36% | **0%** |
| Quality | −45% | −8% |
| Performance | −54% | −17% |
| Ultra Performance | −59% | −9% |

The frame rates are not the finding. The **slope** is what you gain by going
from DLAA down to Ultra Performance, and how much of the available gain each arm
keeps:

| | gain, DLAA → Ultra Performance | share of the ceiling's gain |
|---|---|---|
| no neural rendering | +151% | — |
| neural on the second GPU | **+129%** | **86%** |
| neural on the render GPU | +59% | 39% |

Neural post-processing saturates whatever device it runs on, and it always runs
at *output* resolution so its cost barely falls as you drop the DLSS mode while
the render work collapses. On the render GPU it therefore eats a larger and
larger share of every frame, and upscaling stops paying for itself: about a third
of what the machine actually had to give. Move it to a second GPU and you keep
**86%** of it.

**At DLAA the offloaded neural stage costs the game nothing measurable** the
setting where the render GPU has no spare capacity to hand over.

The same shape was measured earlier on a second machine with a far worse link
smaller numbers, same conclusion. That table, and every condition both sets of
figures were taken under, is in [RESULTS.md](RESULTS.md).

**Neural post-processing on the render device largely defeats upscaling. Moving
it off restores it.**

**The render GPU also runs cooler.** On the first machine it was 21 °C cooler
with the neural stage moved off, load spread across two coolers instead of
stacked on one. On the second machine the neural stage costs the render card
about 10 °C when it runs there. Both are within-rig deltas, taken minutes apart
on one card; the two machines have different cooling and their absolute
temperatures are not comparable.

**And the first machine was the worst plausible configuration for the idea.**
The game rendered on a card in a **chipset-fed PCIe 3.0 x2 slot**, with the
second GPU on the CPU-fed slot so every frame left the render card over that
x2 chipset link on its way to the neural stage. Narrowest path in the machine,
and the whole payload crossed it. That is the point rather than a caveat: the
architecture won where it should have struggled most, and the second machine
both cards on CPU lanes at PCIe 5.0 x8 shows what the same code does when the
link is not the constraint. Slot topology, verified against the board
specifications, is in [RESULTS.md](RESULTS.md) §3.

HAT YOU NEED FIRST
-------------------
 
1. ReShade 6.8.0 or newer, installed WITH ADD-ON SUPPORT.
 
   This is the single most common reason nothing happens. The effects-only
   build of ReShade never loads .addon64 files at all, and it does not say so -
   no error, no log line, because the add-on was never loaded to write one. If
   the log has no "Registered add-on \"MGPU Bridge\"" line, this is why.
 
2. A second NVIDIA GPU in the machine, with a current driver. DLSS-NR comes
   from your driver installation.
 
   GEFORCE RTX 50-SERIES. NVIDIA documents DLSS Neural Rendering as a
   50-series feature - it is not available on 40-series or older cards, and
   this add-on cannot change that. It drives the NGX libraries already in your
   driver and cannot enable something the driver will not run. THIS ADD-ON
   DOES NOT CHECK YOUR HARDWARE: on an older card expect it to load, log
   normally, and produce nothing.
 
   It is the SECOND card that runs the neural stage, so that is the one the
   requirement applies to - but a mixed pair has never been tested, and the
   cross-adapter shared-heap workaround this depends on is itself specific to
   the 50-series. Two RTX 5060 Ti 16 GB is the only configuration this has
   been run on.
 
   You do NOT need a shader pack. Ticking effect packages in the ReShade
   installer is optional - this add-on needs add-on support and nothing else,
   and it was tested with no effects installed at all.
 
   YOU DO NOT NEED ANY OTHER ADD-ON. This one reaches DLSS Neural Rendering on
   its own: it creates its own D3D12 device on the second GPU and drives the
   NGX libraries in your driver directly. Nothing else has to be present.
 
   DO NOT RUN A SECOND NEURAL PATH ALONGSIDE IT. Untested, and this project has
   already seen two independent NGX consumers sharing one parameter block
   produce a visibly wrong image while every transport counter stayed clean.
   One neural path at a time.
 
3. TWO MONITORS - ONE ON EACH CARD. This is a requirement, not a nicety. The
   neural output is displayed by the card that produced it, so nothing has to
   travel back across the link. With both monitors on the render card, the same
   build measured 33% lower throughput and roughly double the latency on the
   development machine. A headless second card works and is slower, and there is
   nothing to look at.
 
4. A DIRECTX 12 GAME. D3D11 AND VULKAN TITLES DO NOTHING - the add-on loads,
   finds no D3D12 render device, stands down, and says so in the log and in
   the ReShade overlay panel. There is no bridge window on those titles: no
   D3D12 device means no window at all, which is why the message is in the
   panel. Some Unity titles can be forced with -force-d3d12.
 
   This is a limit of this add-on, not of DLSS Neural Rendering itself, which
   NVIDIA documents against Vulkan. Everything here is built on ReShade's
   D3D12 path.
 
NOTHING FROM NVIDIA IS INCLUDED HERE. _nvngx.dll, nvngx_dlssnr.dll and the
DLSS-NR weights come from your own driver install. Do not add them to this
folder to make it work for somebody else.

### What was measured

**Everything here was measured through ReShade**, both runs. The render-GPU arm
is RenoDX; the second-GPU arm is this bridge. Same route into DLSS-NR, same game,
same scene.

That is not a game's native DLSS 5 integration, and it is not **OptiScaler**,
which was not tested and is not described by any figure here.

**These numbers are not a prediction of what NVIDIA's own implementation does**,
and they are dated. If you are reading this well after September 2026, treat
every absolute figure as historical driver versions, the DLSS-NR model inside
them, and the games themselves all move, and any of those changes the numbers
without anything in this repository changing.

The **shape** is what is expected to survive: neural work competes with render
work on the device it runs on, and moving it off a saturated device returns the
capacity upscaling was supposed to free. That is the claim worth checking against
your own machine rather than taking from this table.

---

## What it actually does

MGPU Bridge is a ReShade add-on a `.addon64` that ReShade loads into the game
process. On a D3D12 game it:

1. identifies the adapter the game renders on, from the swapchain
2. creates its own D3D12 device on a *different* adapter
3. copies each finished frame across a cross-adapter shared heap, with a
   64-byte seal per ring slot so every frame's identity and ordering can be
   checked rather than assumed
4. runs DLSS Neural Rendering on the second adapter, once or twice per frame
5. presents the result in its own window on the second adapter

The game's own rendering is never touched. The bridge reads the finished frame
and does its work elsewhere.

[ARCHITECTURE.md](ARCHITECTURE.md) has the mechanism: adapter selection, the
shared heap and the seal, the NGX core/snippet split and the result-code ladder,
and the present path.

It uses NVIDIA's NGX libraries from your driver installation. Nothing from
NVIDIA is redistributed here.

---

## Install

See [assets/README.txt](assets/README.txt), which is also the `README.txt` in the
download but two things decide whether it works at all:

**ReShade must be installed with add-on support.** The effects-only build never
loads `.addon64` files and says nothing about it. No error, no log line, because
the add-on was never loaded to write one.

**No shader pack is needed.** Tested directly: no effect packages, no techniques
enabled, both titles fine. An earlier version of this documentation claimed
otherwise and was wrong.

**Do not rename the add-on.** Its filename must contain the literal substring
`nvngx.dll`. The DLSS-NR snippet resolves the module owning its caller's return
address, takes that module's file path, and requires it to contain that
substring. Rename the file and the bridge loads, logs normally, and produces
nothing.

---

## Controls

Live, no relaunch. The panel is registered on the game's overlay as well as the
bridge window's, so it can be driven without leaving the game.

| | |
|---|---|
| `CTRL+ALT+F10` | arm the stream (in gameplay, not a menu) |
| `CTRL+ALT+F7` | view: neural output → input → split |
| `CTRL+ALT+←` / `→` | move the split seam (`SHIFT` for a coarse step) |
| `CTRL+ALT+F8` | choose which pass the intensity keys act on |
| `CTRL+ALT+F9` / `F11` | intensity down / up |
| `Home` | ReShade overlay, containing the panel |

**Split** shows the frame handed *to* the model on the left of the seam and what
the model produced on the right — the **same frame**, in one window. It exists
because no two runs of a game contain the same frame, so any comparison made
across two captures is confounded by everything that changed in between. The
seam moves on hotkeys precisely so it can be dragged across a face with nothing
on screen but the game.

The panel in the overlay carries the same controls plus the model's own tuning
parameters — **Style A/B/C, tone, structure, skin, auto mask**. Those are off
until you touch one; the defaults are the feature's own, which is what every
figure above was measured under. Touching any of them marks the run as a tuning
run in the log.

**If the picture looks washed out or flat, take `tone` down.** On the machine
this was built on, `0.00` fixed it. That is one rig and one pair of titles, so
treat it as the first thing to try rather than the setting you should be on.

Two settings in `mgpu.ini` are worth knowing before the first launch:

| | |
|---|---|
| `Monitor=auto` | which of the **second card's own** outputs the window opens on |
| `AutoArm=0` | `1` arms the stream by itself once the game has settled |

`AutoArm` is off by default because every published measurement was armed by
hand, in gameplay, at a moment that was chosen. Turn it on if you just want to
see the thing work without learning a hotkey.

---

## Limitations

**Longest clean run: twenty minutes**, Cyberpunk 2077 at 1440p, two passes,
stable throughout. Beyond that is untested. Watch GPU load and temperature,
especially with `Frames=0`.

**Interacting with the bridge window takes keyboard focus from the game.** A
controller sidesteps it entirely.

**Do not change resolution, DLSS mode or graphics presets while the stream is
armed.** The stream is armed once, against the game's swapchain exactly as it
stands at that instant — source size, format and row pitch are all fixed then.
Anything that makes the game rebuild its swapchain leaves the bridge consuming
against an arrangement that no longer exists.

**Frame generation is untested.** It was enabled once and that session ended
badly, but on a machine that was also failing on ordinary settings changes — so
nothing is established either way. It is not recommended and it has not been
characterised.

**Colour handling is not implemented, and on one of the two titles tested the
output comes back washed.**  **Taking `tone down in the panel fixed it on the development rig — `0.00` there — and yours may
differ.** The only recorded difference between the two titles is bit depth, 8
versus 10 bits per channel; both formats are plain UNORM, there is no sRGB
anywhere in this pipeline. 

**External overlays that hook `Present` misbehave, and the reason is
structural.** This add-on creates a second swapchain inside the game's process,
and tools like RivaTuner / MSI Afterburner assume one swapchain per process — the
counter flickers between the two present streams. The NVIDIA overlay does not
recognise the bridge window at all. Use **ReShade's own FPS display** instead:
there are two ReShade runtimes in the process, so each draws its own counter for
its own swapchain, which is what you want anyway — the game's rate and the
bridge's rate are different numbers and the difference is the point. Set
`ShowFPS=1` under `[OVERLAY]` in both `ReShade.ini` and `ReShade2.ini`.

For screen recording, prefer display or Windows-Graphics-Capture sources over
"game capture", which adds a third `Present` hook to a process that already has
two.

**D3D12 only. D3D11 and Vulkan titles do nothing.** The add-on loads, finds no
D3D12 render device, stands down, and says so in the log and in its window
title. Some Unity titles can be forced with `-force-d3d12`.

This is a limit of *this bridge*, not of the feature. Everything here hooks ReShade's D3D12 path and
creates a D3D12 device on the second adapter, so Vulkan would be a separate
implementation rather than a flag.

**Image quality is not assessed anywhere in this repository.** That is a
separate question needing exposure-normalised comparison, and nothing here is a
claim about how the output looks.

---

## Please read the code before running it

Genuinely, not as a formality. This creates a second D3D12 device, allocates
cross-adapter shared heaps, and drives vendor libraries on your own hardware.
It has been exercised on **two machines**, with three titles — and only one of
those titles produced the frame-rate measurements. Sessions are measured in
minutes, not hours.

See [ACKNOWLEDGEMENTS.md](ACKNOWLEDGEMENTS.md) for how it was built — including
which parts were written by AI, and the specific occasions where the AI was
confidently wrong and a human caught it. That belongs in the record.

No warranty. See [LICENSE](LICENSE). Not affiliated with, endorsed by, or
supported by NVIDIA.
