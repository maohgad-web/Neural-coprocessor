# Neural Coprocessor

**A second GPU running a game's neural post-processing, while the first one
renders.** Not SLI — nothing is split mid-frame. Neural rendering is a *terminal*
stage: it takes a finished frame and returns a finished frame, so it can be
picked up and executed somewhere else entirely.

**This is a ReShade add-on.** It is called **MGPU Bridge**, it is a `.addon64`
file that ReShade loads into a D3D12 game, and every log line it writes is
prefixed `[MGPU]` in `ReShade.log`. It is not a driver, not a patch, and not a
replacement for anything — it needs an **add-on-enabled** ReShade build to load
at all. There is no game modification of any kind: the add-on reads each
finished frame and does its work elsewhere.

This is research code with published measurements. It is not a product.

**See it running:** [The Blood of Dawnwalker, 1920 × 1080](https://youtu.be/yoEsuZyltFc)
— the game on one card, the neural output in its own window on the other, both
live in a single take.

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

Measured on one machine, one game, with the game rendering on the same card in
both runs — so the only variable is which GPU does the neural work. A second
title was run for compatibility, stability and power, but **not** for a
frame-rate comparison. Method and caveats in [RESULTS.md](RESULTS.md).

At 1920 × 1080, across the DLSS range:

| DLSS mode | NR on the render GPU | NR on the second GPU |
|---|---|---|
| DLAA | 43–44 | 48 |
| Quality | 44–45 | 58 |
| Performance | 47–48 | 69 |
| Ultra Performance | 56–57 | 79 |

The frame rates are not the finding. The **slope** is — what you gain by going
from DLAA down to Ultra Performance:

- neural work on the render GPU: **+30%**
- neural work on the second GPU: **+65%**

Neural post-processing saturates whatever device it runs on. On the render GPU,
lowering the render resolution frees capacity the neural stage immediately
consumes, so upscaling stops paying for itself. Move it to a second GPU and the
render GPU is genuinely freed.

**Neural post-processing on the render device largely defeats upscaling. Moving
it off restores it.**

The render GPU also runs 21 °C cooler, because the load sits across two coolers
instead of stacked on one.

All of it measured on the worst plausible configuration for the idea. The game
renders on a card in a **chipset-fed PCIe 3.0 x2 slot**, and the second GPU sits
on the CPU-fed slot — so every frame leaves the render card over that x2 chipset
link on its way to the neural stage. It is the narrowest path in the machine and
the whole payload crosses it. That is the point rather than a caveat — the
architecture wins where it should struggle most. Slot topology, verified against
the board specification, is in [RESULTS.md](RESULTS.md) §3.

### What this needs

**GeForce RTX 50-series cards.** DLSS Neural Rendering is documented by NVIDIA
as a 50-series feature; it is not available on 40-series or older hardware, and
nothing this add-on does changes that — it drives the NGX libraries in your own
driver installation and cannot enable a feature the driver will not run. Every
figure here was taken on two RTX 5060 Ti 16 GB. **The add-on does not check your
hardware**, so on an older card expect it to load, log, and produce nothing.

Strictly it is the **second** GPU that runs the neural stage, so that is the card
NVIDIA's requirement applies to — but a mixed pair was never tested, and the
cross-adapter shared-heap workaround this depends on is itself a 50-series one.
Two 50-series cards is the only configuration that has been run.

**Two GPUs and two monitors — one monitor on each card.** This is a requirement,
not a nicety, and it is the first thing to get right. The neural output is
displayed by the card that produced it, so nothing has to travel back across the
link. Earlier milestones ran the second GPU headless and it works, but moving the
cable onto the second card was worth **+33% throughput and roughly half the
latency** on this machine — one variable, five minutes apart. Run it headless and
you get a slower version of this with nothing to look at.

### What was measured

**Everything here was measured through ReShade**, both runs. The render-GPU arm
is RenoDX; the second-GPU arm is this bridge. Same route into DLSS-NR, same game,
same scene.

That is not a game's native DLSS 5 integration, and it is not **OptiScaler**,
which was not tested and is not described by any figure here.

**These numbers are not a prediction of what NVIDIA's own implementation does**,
and they are dated. If you are reading this well after September 2026, treat
every absolute figure as historical — driver versions, the DLSS-NR model inside
them, and the games themselves all move, and any of those changes the numbers
without anything in this repository changing.

The **shape** is what is expected to survive: neural work competes with render
work on the device it runs on, and moving it off a saturated device returns the
capacity upscaling was supposed to free. That is the claim worth checking against
your own machine rather than taking from this table.

---

## What it actually does

MGPU Bridge is a ReShade add-on — a `.addon64` that ReShade loads into the game
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
download — but two things decide whether it works at all:

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

## Reading the log

The instrumentation is deliberately loud, because this project repeatedly found
that a confident, correctly-formatted, wrong answer is harder to catch than an
obvious failure. Four lines answer most questions:

| line | what it settles |
|---|---|
| `Registered add-on "MGPU Bridge"` | ReShade has add-on support and found the file |
| `[MGPU][P1.6] ... N of 14 techniques ENABLED` | what is being drawn on top of what you are looking at, per runtime |
| `[MGPU][P7.2] mgpu.ini read from ...` | which settings file actually took effect |
| `[MGPU][P4.0] stream REQUESTED - ...` | every setting in force, in one line |

If a log line and this README disagree, believe the log and open an issue.

The instrumentation is loud on purpose, and the reasoning behind that — along
with the rest of the working rules — is in [METHOD.md](METHOD.md).

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

**The black-screen session — what is established and what is not.** It was a
**six-pass** run held for several minutes. That is the condition, and it is
solid. The *mechanism* is not: a later twenty-minute Cyberpunk 2077 run at two
passes sat at **167–174 W of a 180 W limit** and was completely stable, so
"pinned at the power limit" is survivable and cannot by itself be the
explanation. Six passes differ from two by more than the ceiling — per-frame GPU
time, live handle count, sustained thermals — and which of those mattered was
never isolated. **The build now allows at most two passes, a condition under
which the failure has never been seen and which has now been held for twenty
minutes.**

**Longest clean run: twenty minutes**, Cyberpunk 2077 at 1440p, two passes,
stable throughout. Beyond that is untested. Watch GPU load and temperature,
especially with `Frames=0`.

**The bridge window's frame rate falls as the pass count rises. The game's does
not.** That is the architecture working, not a fault.

**Interacting with the bridge window takes keyboard focus from the game.** A
controller sidesteps it entirely.

**Do not change resolution, DLSS mode or graphics presets while the stream is
armed.** The stream is armed once, against the game's swapchain exactly as it
stands at that instant — source size, format and row pitch are all fixed then.
Anything that makes the game rebuild its swapchain leaves the bridge consuming
against an arrangement that no longer exists. On the development machine that
produced a session-long run of dropped and reordered frames beginning one second
after the change, and the session could freeze. Set the game up first, then arm;
to change something afterwards, disarm, change it, and arm again. The seal
reports it when it happens, so the log will say plainly whether this is what you
hit.

**Frame generation is untested.** It was enabled once and that session ended
badly, but on a machine that was also failing on ordinary settings changes — so
nothing is established either way. It is not recommended and it has not been
characterised.

**Colour handling is not implemented, and on one of the two titles tested the
output comes back washed.** The frame handed to the model is correct and the
frame it returns is washed, established by same-frame split, so transport and
presentation are both exonerated. The cause is not established. **Taking `tone`
down in the panel fixed it on the development rig — `0.00` there — and yours may
differ.** The only recorded difference between the two titles is bit depth, 8
versus 10 bits per channel; both formats are plain UNORM, there is no sRGB
anywhere in this pipeline, and an earlier diagnosis that said there was is
retracted.

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

This is a limit of *this bridge*, not of the feature — NVIDIA's own DLSS-NR
programming guide is written against Vulkan, and the snippet ships a fuller
Vulkan surface than a D3D12 one. Everything here hooks ReShade's D3D12 path and
creates a D3D12 device on the second adapter, so Vulkan would be a separate
implementation rather than a flag.

**Image quality is not assessed anywhere in this repository.** That is a
separate question needing exposure-normalised comparison, and nothing here is a
claim about how the output looks.

---

## Please read the code before running it

Genuinely, not as a formality. This creates a second D3D12 device, allocates
cross-adapter shared heaps, and drives vendor libraries on your own hardware.
It has been exercised on **one machine**, with three titles — and only one of
those produced the measurements. A second machine exists and nothing has been run
on it yet.

See [ACKNOWLEDGEMENTS.md](ACKNOWLEDGEMENTS.md) for how it was built — including
which parts were written by AI, and the specific occasions where the AI was
confidently wrong and a human caught it. That belongs in the record.

No warranty. See [LICENSE](LICENSE). Not affiliated with, endorsed by, or
supported by NVIDIA.
