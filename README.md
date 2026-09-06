# MGPU Bridge

Cross-adapter DLSS Neural Rendering. The game renders on one GPU; the neural
post-processing runs on a second GPU that is not rendering the game.

This is research code with published measurements. It is not a product.

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
both runs — so the only variable is which GPU does the neural work. Method and
caveats in [RESULTS.md](RESULTS.md).

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

All of it measured on the worst plausible configuration for the idea: the second
GPU is on a **chipset-fed PCIe 3.0 x2 slot**. That is the point rather than a
caveat — the architecture wins where it should struggle most.

### What this needs

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

The bridge is a ReShade add-on. On a D3D12 game it:

1. identifies the adapter the game renders on, from the swapchain
2. creates its own D3D12 device on a *different* adapter
3. copies each finished frame across a cross-adapter shared heap, with a
   64-byte seal per ring slot so every frame's identity and ordering can be
   checked rather than assumed
4. runs DLSS Neural Rendering on the second adapter, up to six times per frame
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

---

## Limitations

**Stability beyond a few minutes is untested.** The longest clean run recorded
is about five and a half minutes. One session ended with the game rendering
black on both displays; the bridge logged no fault and the cause was never
isolated. Watch GPU load and temperature, especially with `Frames=0`.

**The bridge window's frame rate falls as the pass count rises. The game's does
not.** That is the architecture working, not a fault.

**Interacting with the bridge window takes keyboard focus from the game.** A
controller sidesteps it entirely.

**The bridge window opens on the game's display and has to be moved once per
launch.** Known defect, not a configuration mistake. The window is created before
the sizing code runs, and that code fits the window to *the monitor it is already
on* — so it sizes correctly to the wrong display. Drag it to the second monitor;
`Window=fit` then does the right thing. The proper fix is to derive the target
monitor from the bridge adapter's own DXGI output rather than from the window's
current position, which is also the placement that keeps scan-out on the card
that did the neural work.

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

**D3D11 games do nothing.** No game adapter is identified, the bridge stands
down, and the log says so. Some Unity titles can be forced with `-force-d3d12`.

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
