# Neural Coprocessor

**A second GPU running a game's neural post-processing, while the first one
renders.** Not SLI: nothing is split mid-frame. Neural rendering is a *terminal*
stage. It takes a finished frame and returns a finished frame, so it can be
picked up and executed somewhere else entirely.

**This is a ReShade add-on.** It is called **MGPU Bridge**, it is a `.addon64`
file that ReShade loads into a D3D12 game, and every log line it writes is
prefixed `[MGPU]` in `ReShade.log`. It is not a driver, not a patch, and not a
replacement for anything, and it needs an **add-on-enabled** ReShade build to
load at all. There is no game modification of any kind: the add-on reads each
finished frame and does its work elsewhere.

This is research code with published measurements. It is not a product.

**See it running.** The game on one card, the neural output in its own window on
the other, both live in a single take:

- [The Blood of Dawnwalker](https://youtu.be/yoEsuZyltFc), the title every
  frame-rate measurement in [RESULTS.md](RESULTS.md) was taken on
- [Cyberpunk 2077](https://youtu.be/XWv5jw90yHc), gameplay, played normally.
  **No figure in this repository comes from Cyberpunk**; it was run for
  compatibility, stability and power draw only.

Both are demonstrations, not benchmarks. Screen-recording overhead is present in
each, and the measured runs were separate and unrecorded.

| | |
|---|---|
| [RESULTS.md](RESULTS.md) | what was measured, on what, with what caveats |
| [ARCHITECTURE.md](ARCHITECTURE.md) | how a frame gets to the second GPU and back |
| [METHOD.md](METHOD.md) | the working rules, and what they cost to learn |
| [VENDOR_LOCK.md](VENDOR_LOCK.md) | the machine, driver and configuration every number was taken on |
| [ACKNOWLEDGEMENTS.md](ACKNOWLEDGEMENTS.md) | prior work, AI use, disclaimer |
| [THIRD_PARTY.md](THIRD_PARTY.md) | licences and provenance |
| [reference/](reference/) | a working `ReShade.ini` and a complete `ReShade.log`, to diff yours against |
| [history/](history/) | the milestone record, the verified-facts ledger, the instrument design |

---

## The result

The Blood of Dawnwalker at 1920 x 1080, across the whole DLSS range, on two
RTX 5060 Ti 16 GB. **Three arms**, so the neural stage can be priced rather than
only compared against itself. Method and caveats in [RESULTS.md](RESULTS.md).

| DLSS mode | DLSS 5 off | DLSS 5 on the render card | DLSS 5 on the second card |
|---|---|---|---|
| DLAA | 67-70 | 44 | 67-70 |
| Quality | 98-99 | 54-55 | 91 |
| Performance | 127-131 | 59 | 106-107 |
| Ultra Performance | 172 | 69-71 | 157 |

**Read the columns before the numbers, because two things are easy to get
backwards here.**

**"DLSS 5 off" does not mean DLSS off.** DLSS super-resolution is running in
every column - the rows are its modes, and the game renders at the same internal
resolution in all three. What the first column switches off is DLSS 5's *neural
rendering* stage.

**This is not one GPU versus two GPUs.** The game renders on the same card in
all three columns. The neural stage runs exactly once per frame in both columns
that have it. Nothing is split, nothing is duplicated, and no rendering work is
added or moved. The only thing that changes is **which card executes that one
neural pass**.

So the first column is the ceiling: what this machine and this game do with the
neural stage switched off. Against it, this is what the neural stage **costs the
game**:

| DLSS mode | cost, on the render card | cost, on the second card |
|---|---|---|
| DLAA | -36% | **0%** |
| Quality | -45% | -8% |
| Performance | -54% | -17% |
| Ultra Performance | -59% | -9% |

Same work, same amount of it, same frame. Only the card differs, and the price
the game pays goes from more than half its frame rate to almost nothing.

The frame rates are not the finding. The **slope** is: what you gain by going
from DLAA down to Ultra Performance, and how much of that available gain each arm
keeps.

| | gain, DLAA to Ultra Performance | share of the ceiling's gain |
|---|---|---|
| DLSS 5 off | +151% | - |
| DLSS 5 on the second card | **+129%** | **86%** |
| DLSS 5 on the render card | +59% | 39% |

Neural post-processing saturates whatever device it runs on, and it always runs
at *output* resolution. Its cost barely falls as you drop the DLSS mode, while
the render work collapses. On the render card it therefore eats a larger and
larger share of every frame, and upscaling stops paying for itself: you keep
about a third of what the machine actually had to give. Move it to the second
card and you keep **86%** of it.

**At DLAA the offloaded neural stage costs the game nothing measurable.** That is
the setting where the render card has no spare capacity to hand over.

The same shape was measured earlier on a second machine with a far worse link:
smaller numbers, same conclusion. That table, and every condition both sets of
figures were taken under, is in [RESULTS.md](RESULTS.md).

**Neural post-processing on the render device largely defeats upscaling. Moving
it off restores it.**

**The render card also runs cooler.** On the first machine it was 21 C cooler
with the neural stage moved off, load spread across two coolers instead of
stacked on one. On the second machine the neural stage costs the render card
about 10 C when it runs there. Both are within-rig deltas taken minutes apart on
one card. The two machines have different cooling and their absolute
temperatures are not comparable.

**And the first machine was the worst plausible configuration for the idea.**
The game rendered on a card in a **chipset-fed PCIe 3.0 x2 slot**, with the
second GPU on the CPU-fed slot, so every frame left the render card over that x2
chipset link on its way to the neural stage. Narrowest path in the machine, and
the whole payload crossed it. That is the point rather than a caveat: the
architecture won where it should have struggled most, and the second machine
(both cards on CPU lanes at PCIe 5.0 x8) shows what the same code does when the
link is not the constraint. Slot topology, verified against the board
specifications, is in [RESULTS.md](RESULTS.md).

**Nothing here says how this scales to a higher-tier card.** Both machines are
dual RTX 5060 Ti 16 GB. A bigger card changes the render cost and the neural cost
at the same time, and which one moves further is not something this project has
measured. No claim is made in either direction, and this is not a reason to go
and buy a second GPU.

### What this needs

**GeForce RTX 50-series cards.** NVIDIA documents DLSS Neural Rendering as a
50-series feature. It is not available on 40-series or older hardware, and this
add-on cannot change that: it drives the `nvngx_dlssnr.dll` already on your
machine and cannot enable something that file will not run. **It does not check
your hardware**, so on an older card expect it to load, log normally, and
produce nothing.

**GeForce RTX 40-series cards.** This is now confirmed working, not just
theorised. [salient-cyanocitta](https://github.com/salient-cyanocitta) ran the
bridge on an RTX 4080 SUPER (rendering) paired with an RTX 5060 Ti (second
card), on Horizon Forbidden West and Cyberpunk 2077, using a modded
`nvngx_dlssnr.dll` sourced independently rather than the OptiScaler bundle
they'd tried first, which did not work for them. That also answers the PCIe
question this setup raises: both of their cards run at PCIe 4.0 x8,
. I still don't own Ada hardware myself, so I still can't troubleshoot a
40-series setup directly - but it's no longer an untested guess, 
it's a result, from a second machine (see the 
[discussion](https://github.com/maohgad-web/Neural-coprocessor/discussions/3)).

**Two GPUs and two monitors, one on each card.** A requirement, not a nicety. The
neural output is displayed by the card that produced it, so nothing has to travel
back across the link. Running the second card headless works and is meaningfully
slower: on the development machine it cost 33% throughput and roughly doubled
latency, one variable, five minutes apart.

**An add-on-enabled ReShade build, 6.8.0 or newer.** This is the single most
common reason nothing happens. The effects-only build never loads `.addon64`
files and says nothing about it, because the add-on was never loaded to write a
log line. If the log has no `Registered add-on "MGPU Bridge"` line, this is why.
[reference/](reference/) has a complete log from a working run, and the five
lines worth checking first, so you can diff yours against it.

**A DirectX 12 game.** D3D11 and Vulkan titles do nothing: the add-on loads,
finds no D3D12 render device, stands down, and says so in the log and in the
ReShade overlay panel. There is no bridge window on those titles, because no
D3D12 device means no window at all, which is why the message is in the panel.
Some Unity titles can be forced with `-force-d3d12`.

**No shader pack, and no other add-on.** Ticking effect packages in the ReShade
installer is optional, and this was tested with none installed. This add-on
reaches DLSS-NR on its own: it creates its own D3D12 device on the second card
and drives the NGX libraries directly. Do not run a second neural
path alongside it - untested, and this project has already seen two independent
NGX consumers sharing one parameter block produce a visibly wrong image while
every transport counter stayed clean.

**Nothing from NVIDIA is included here, and none of it may be.** The two NGX
modules do not arrive the same way, and this documentation previously said they
did:

- **`_nvngx.dll`** is the driver core. It is already resident in the game process
  on a current driver, with no DLSS add-on loaded - that is logged rather than
  assumed.
- **`nvngx_dlssnr.dll`** is the DLSS-NR snippet, and it is loaded **from the
  game's own folder**, not from the driver store. On the machines this was built
  on it was already present there. **This project does not ship it, cannot ship
  it, and does not document how to obtain it.** If the neural stage never starts
  and the log shows the snippet failing to load, that file is what is missing.

Do not add either of them to a release package or send them to anybody else.

### What this is, in plain terms

**This is not NVIDIA's DLSS 5 implementation, and it is not trying to be.** A
game with a native DLSS 5 integration hands the model everything the engine
knows: motion vectors, depth, and masks that say which parts of the frame are
worth the neural work at all. Nothing here has access to any of that. This is a
ReShade add-on. It sees the frame after the engine has finished with it, the
same way any post-processing effect does.

**What the model is actually fed here is colour.** One finished frame, copied to
the second card. Motion vectors are derived from that colour on the second card
by optical flow. **Depth is null** - the model never receives it, and on this
path it does not need it. No engine data of any kind crosses the link, because
none of it is available at the point the add-on hooks.

That has a consequence worth stating plainly: **these figures are a floor, not a
ceiling.** A version fed real engine motion vectors and depth, masking the
regions that matter instead of processing the whole frame, would be doing less
work and doing it more accurately. This project does not measure that version and
makes no claim about it. It measures the crudest possible way of getting the
neural stage onto another device, which is exactly why the result is interesting:
it wins anyway.

**Both arms in the tables are injected paths.** The render-card arm is RenoDX
under ReShade; the second-card arm is this bridge. Same route into DLSS-NR, same
game, same scene. So the comparison is injected-local against injected-offloaded,
not against a native integration. **OptiScaler**, the other common injection
route, was not tested and is not described by any figure here.

**Why the game stays fast.** The frame rates in the tables are the *game's* frame
rate, on the game's own monitor. The game's rendering is never touched: the
add-on reads the finished frame, copies it, and everything after that happens on
the other card. The neural picture appears in a second window on the second
card's own monitor, which is why that monitor is a requirement rather than a
nicety - nothing has to come back across the link to be displayed. When the pass
count rises, the bridge window's frame rate falls and the game's does not. That
is the architecture working.

**On latency, what is and is not known.** Each neural pass costs about 8.3 ms of
second-card time at 1080p, measured. What has *not* been measured anywhere in
this project is photon-to-photon latency - the real number, from your input to
light leaving the panel. The timings recorded here are submit-to-consume across
the link, which is a smaller and easier quantity. The neural window is playable
in practice, in the sense that it has been played on for sessions of minutes, but
"it felt fine" is not a measurement and is not offered as one. If you are
deciding whether this is usable for you, that is the gap to be aware of.

**What the project set out to prove**, and the only thing it claims: that neural
rendering is a *terminal* stage, and a terminal stage can be decoupled from the
render device entirely. Not split like SLI, not duplicated - lifted off one card
and executed on another, with the frame's identity and ordering checked rather
than assumed. Everything else in this repository is the evidence for that one
claim, and the measurements exist to show it is not free but is worth it.

**These numbers are not a prediction of what NVIDIA's own implementation does**,
and they are dated. If you are reading this well after September 2026, treat
every absolute figure as historical: driver versions, the DLSS-NR model inside
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

It uses NVIDIA's NGX libraries: the core resolved from the driver, the DLSS-NR
snippet loaded from the game folder. Nothing from NVIDIA is redistributed here,
and the snippet is a prerequisite this project does not supply. See
[THIRD_PARTY.md](THIRD_PARTY.md).

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
the model produced on the right, the **same frame**, in one window. It exists
because no two runs of a game contain the same frame, so any comparison made
across two captures is confounded by everything that changed in between. The
seam moves on hotkeys precisely so it can be dragged across a face with nothing
on screen but the game.

The panel in the overlay carries the same controls plus the model's own tuning
parameters, **Style A/B/C, tone, structure, skin, auto mask**. Those are off
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
| `AutoArm=1` | arms the stream by itself once the game has settled; set `0` to arm by hand |

`AutoArm` ships on, so the add-on shows you something without you having to find
a hotkey first. Set it to `0` before measuring anything: every published figure
was armed by hand, in gameplay, at a moment that was chosen.

### Configuration file boundary

The reader accepts the existing line-oriented `key=value` syntax, leading
spaces/tabs, and whole-line `;` or `#` comments. It accepts UTF-8 files with an
optional UTF-8 BOM; UTF-16, malformed UTF-8, embedded NULs, and unsupported
control bytes are rejected. The complete file is bounded at 64 KiB (65535
payload bytes, with one byte reserved for the terminator). This is intentionally
large enough for the shipped comment-heavy `mgpu.ini`. A file beyond the bound
is rejected as a whole rather than being clipped, so a setting after a long
comment cannot silently fall back to a default. The diagnostic identifies the
rejection and the documented built-in defaults remain in force.

---

## Limitations

**Longest clean run: twenty minutes**, Cyberpunk 2077 at 1440p, two passes,
stable throughout. Beyond that is untested. Watch GPU load and temperature,
especially with `Frames=0`.

**Interacting with the bridge window takes keyboard focus from the game.** A
controller sidesteps it entirely.

**Do not change resolution, DLSS mode or graphics presets while the stream is
armed.** The stream is armed once, against the game's swapchain exactly as it
stands at that instant, source size, format and row pitch are all fixed then.
Anything that makes the game rebuild its swapchain leaves the bridge consuming
against an arrangement that no longer exists.

**Frame generation is untested.** It was enabled once and that session ended
badly, but on a machine that was also failing on ordinary settings changes, so
nothing is established either way. It is not recommended and it has not been
characterised.

**Colour handling is not implemented, and on one of the two titles tested the
output comes back washed.** Taking `tone` down in the panel fixed it on the
development rig, at `0.00` there, and yours may differ. The only recorded difference between the two titles is bit depth, 8
versus 10 bits per channel; both formats are plain UNORM, there is no sRGB
anywhere in this pipeline. 

**External overlays that hook `Present` misbehave, and the reason is
structural.** This add-on creates a second swapchain inside the game's process,
and tools like RivaTuner / MSI Afterburner assume one swapchain per process, the
counter flickers between the two present streams. The NVIDIA overlay does not
recognise the bridge window at all. Use **ReShade's own FPS display** instead:
there are two ReShade runtimes in the process, so each draws its own counter for
its own swapchain, which is what you want anyway, the game's rate and the
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
It has been exercised on **two machines**, with three titles, and only one of
those titles produced the frame-rate measurements. Sessions are measured in
minutes, not hours.

See [ACKNOWLEDGEMENTS.md](ACKNOWLEDGEMENTS.md) for how it was built, including
which parts were written by AI, and the specific occasions where the AI was
confidently wrong and a human caught it. That belongs in the record.

No warranty. See [LICENSE](LICENSE). Not affiliated with, endorsed by, or
supported by NVIDIA.
