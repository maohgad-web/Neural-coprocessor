# Results

Two machines, one game. Read the limitations section before quoting anything
here.

## Where to look first

**Rig B, section 4b, is the complete measurement.** Three arms at one
resolution: DLSS 5 neural rendering off, neural rendering on the render card,
and neural rendering on the second card. It is the only table in this document
that can say what the neural stage *costs*, because it is the only one with an
off arm at the same resolution as the on arms.

| DLSS mode | DLSS 5 off | DLSS 5 on the render card | DLSS 5 on the second card |
|---|---|---|---|
| DLAA | 67-70 | 44 | 67-70 |
| Quality | 98-99 | 54-55 | 91 |
| Performance | 127-131 | 59 | 106-107 |
| Ultra Performance | 172 | 69-71 | 157 |

The Blood of Dawnwalker, 1920 x 1080, two RTX 5060 Ti 16 GB. **DLSS
super-resolution is running in every column** - the rows are its modes. What
changes between columns is whether DLSS 5's *neural rendering* stage runs, and
which card runs it.

**Sections 1 to 4 are rig A**, the machine everything was developed on. Its
render card sits on a chipset-fed PCIe 3.0 x2 link, which is the worst plausible
configuration for this idea, and it has no DLSS-5-off arm at 1080p - so it can
compare the two neural arms to each other but cannot price the neural stage. The
often-quoted **18%** figure in section 4 is one DLSS mode, at 1440p, on that
worst-case link. It is not the headline and never was.

Every figure below is measured. An earlier draft carried **CONFIRM** markers on
values that were still owed from the run logs; all of them were filled, and none
remain. If you find one, it is an editing error and the value under it should not
be trusted.

---

## 1. Machine (rig A)

| | |
|---|---|
| CPU | AMD Ryzen 5 5600, 6C/12T, AM4 |
| Board | Gigabyte B550 AORUS ELITE V2, BIOS FD |
| RAM | 2 x 16 GB DDR4-3600, running at rated speed |
| OS | Windows 11 Pro 25H2, build 26200.9168 |
| Driver | NVIDIA 616.64 (32.0.16.1664) |
| ReShade | 6.8.0.2155, add-on API 20 |
| GPUs | 2 x RTX 5060 Ti 16 GB, both at stock (180 W enforced) |

Both cards are the same model from different board partners, VBIOS
`98.06.39.40.b2` and `98.06.1f.40.46`. Not identical silicon-to-silicon.

### Slot topology

Verified against Gigabyte's published specification for the board, not
inferred from lane counts:

| card | slot | fed by | link, measured under load |
|---|---|---|---|
| bus `04:00.0`, UUID `GPU-0b06288a` | PCIEX2 | **chipset** | PCIe 3.0 **x2** |
| bus `06:00.0`, UUID `GPU-ea80cdaf` | PCIEX16 | **CPU** | PCIe 4.0 **x8** |

The board has exactly one PCIe 3.0 x2 slot and it is chipset-integrated, so the
card measuring gen3 x2 can only be in it. The CPU slot is specified as x16 but
negotiates x8 on this machine; measured with the second card disabled and the
first at 100% load, so it is not caused by the second card and it is not an
idle downshift.

Cards are identified by bus ID and UUID throughout. **The Windows adapter index
and the nvidia-smi index both changed between runs on this machine** and must
not be used as identifiers.

### Storage

The game runs from a HIKSEMI 234 GB external USB SSD (serial `201503310007F`),
measuring ~200-220 MB/s sequential write on this machine's port. The same
physical drive moves to rig B, so storage is a controlled constant rather than
a variable.

Disk affects load times and streaming, not steady-state frame rate in a
resident scene. Figures below are taken stationary for that reason.

---

## 2. Method

- **Game**: The Blood of Dawnwalker (D3D12)
- **Scene**: the game's opening area, the forest at the start of *All Good
  Things* ("Find Lunka in the forest", 166 m marker). Stationary, same spot and
  same camera heading for every arm. `docs/scenes/Scene.png` is that scene as the game
  renders it, with no neural stage, the reference for where every figure below
  was taken. `docs/scenes/Split.png` is the same scene in the bridge's split view: left
  of the seam is the frame handed to the model, right of it is what the model
  produced, and both halves are the same frame.
- **Vsync**: off. Checked explicitly after the 1080p Quality figure looked like
  it might be capped; disabling vsync produced the same number, so the figures
  are real rather than clamped.
- **Clocks, memory, power**: all stock. No power limit applied.
- **ReShade effects**: zero techniques enabled on both runtimes, the game's
  and the bridge's, verified per run from the `[MGPU][P1.6]` log lines.
- **`AutoSavePreset=0`** on both ReShade configs. With it enabled, anything
  toggled in the overlay is written back to the preset at shutdown, so presets
  drift between runs with no file edited. This cost one invalid run before it
  was found.

### Parameter matching between the arms

The local arm is RenoDX **under ReShade**; the offload arm is this bridge.

**Which local path was measured matters, and only one of them was.** DLSS-NR can
be reached on the render device by more than one route, and they are not
interchangeable for a performance measurement, they differ in where they insert
into the frame and in how much of the frame they touch. Everything in this
document was measured through the **ReShade add-on path**. **OptiScaler, the
other route in common use, was not measured at all**, and no figure here should
be read as describing it. If someone reproduces this against OptiScaler and gets
a different local arm, that is a different measurement rather than a
contradiction of this one, and it is a measurement worth having.

Matched:

| | RenoDX | bridge |
|---|---|---|
| intensity | `DirectNeuralRenderingIntensity=1` | `Intensity=1.00` |
| applications per frame | 1 | `Passes=1` |
| encoding | `Encoding=0` | unencoded colour |
| white point | `DiffuseWhiteOverride=0` (not applied) | not set |
| tone/structure strengths | all `1` | not set, assumed NGX default of 1 |

Not matched, and unmatchable:

- RenoDX runs with `AutoMask=1` and masks regions; the bridge masks nothing, so
  it may perform *more* neural work per frame.
- RenoDX hooks at its own chosen point (`HookPoint=5`, order 2); the bridge
  hooks at ReShade's present. Different insertion points in the frame.

The bridge feeds colour and motion vectors only. Depth is null. Tone and
strength parameters are wired but unset.

---

## 3. Baselines, no neural rendering

| render card | link | fps |
|---|---|---|
| bus `04:00.0` | PCIe 3.0 x2 (chipset) | ~60 |
| bus `06:00.0` | PCIe 4.0 x8 (CPU) | ~80 |

Taken at 2560 x 1440, DLSS Quality, game graphics preset High, the same
settings as the neural arms below. No log was retained for these two runs; the
figures come from the session record rather than from a log file, and are
stated that way deliberately.

### Enabling the second GPU costs nothing

With both cards enabled and the game rendering on the chipset card, frame rate
was unchanged from the single-card figure. The measurement shows why:

```
0, bus 04:00.0, 5869 MiB, 86%   <- game
1, bus 06:00.0,    0 MiB,  0%   <- second card, enabled, idle
```

The second card holds nothing and does nothing. This holds **provided the game
renders on the card driving its own display**. A run with the game rendering on
one card while its window lived on a panel attached to the other fell to ~40
fps, that is the compositor copying every frame across PCIe, not a cost of the
second GPU, and it is avoided by attaching the display to the render card.

---

## 4. Local versus offloaded neural rendering

Both arms render the game on the **same card**, bus `04:00.0`, the chipset
PCIe 3.0 x2 card, so the only variable is where the neural work runs. Verified
per run from GPU memory and utilisation, not assumed.

### 2560 x 1440

| | RenoDX local | offloaded |
|---|---|---|
| DLSS Quality | 33-34 | **40** |

Offload is roughly **18% ahead** on the same render card.

**This single figure has been quoted as though it were the result. It is not.**
It is one DLSS mode, at one resolution, on the machine whose render card sits on
a chipset-fed PCIe 3.0 x2 link - and it compares two *neural* arms against each
other, so it says nothing about what the neural stage costs in the first place.
The measurement that answers that is section 4b.

The offload arm across the DLSS range at 1440p: DLAA 32-33, Quality 40,
Performance 50, Ultra Performance 54.

RenoDX at 1440p was measured at DLSS Quality only, deliberately. 1440p exists
in this document to show how the two arms scale with resolution; **1920 x 1080
is the comparison resolution** and carries the full DLSS table below.

### 1920 x 1080

| DLSS mode | RenoDX local | offloaded | delta |
|---|---|---|---|
| DLAA | 43-44 | 48 | +10% |
| Quality | 44-45 | 58 | +30% |
| Performance | 47-48 | 69 | +45% |
| Ultra Performance | 56-57 | 79 | +39% |

### The result that matters

The frame-rate gap is not the finding. The **shape** is.

Across the full DLSS range, from DLAA down to Ultra Performance:

- RenoDX local gains **+30%** (43.5 → 56.5)
- Offloaded gains **+65%** (48 → 79)

With neural rendering local, the neural stage saturates the render GPU. Lowering
the render resolution frees capacity that the neural stage immediately consumes,
so upscaling buys very little, the classic reason for using DLSS stops working.
Offloaded, the render GPU is genuinely freed and DLSS behaves normally.

**Running neural post-processing on the render device largely defeats
upscaling. Moving it to a second device restores it.** That is a structural
claim about where the work runs, not a claim about this implementation's
efficiency, and it is the reason the architecture exists.

That it holds on the worst link in the machine, a chipset-fed PCIe 3.0 x2 slot,
is the point rather than a caveat.

---

## 4b. Rig B, with all three arms

Everything above compares the two neural arms to each other. It cannot say what
the neural stage *costs*, because rig A has no DLSS-5-off arm at 1920 x 1080 -
its baselines (section 3) were taken at 1440p, and a figure from a different
resolution is not a column in a 1080p table.

Rig B has all three, at one resolution, in one session.

**Rig B**: Ryzen 7 7800X3D, Gigabyte X870E AERO, 32 GB DDR5 as a single stick at
4800 MT/s with EXPO off, two RTX 5060 Ti 16 GB both stock at 180 W, **both cards
on CPU lanes negotiating PCIe 5.0 x8 under load**, one monitor on each card,
850 W PSU, open bench. Same driver (616.64), same ReShade (6.8.0.2155), same game
on the same physical USB drive as rig A. Windows 11 Pro 25H2 build 26200.9168.

The Blood of Dawnwalker, 1920 x 1080, same stationary scene:

| DLSS mode | DLSS 5 off | DLSS 5 on the render card | DLSS 5 on the second card |
|---|---|---|---|
| DLAA | 67-70 | 44 | 67-70 |
| Quality | 98-99 | 54-55 | 91 |
| Performance | 127-131 | 59 | 106-107 |
| Ultra Performance | 172 | 69-71 | 157 |

**"DLSS 5 off" does not mean DLSS off.** DLSS super-resolution is running in
every column - the rows are its modes, and the game renders at the same internal
resolution in all three. What the first column switches off is DLSS 5's *neural
rendering* stage. The other two run it once per frame; the only thing that
differs between them is which card executes it.

### What the neural stage costs the game

Against the off column at each mode:

| DLSS mode | on the render card | on the second card |
|---|---|---|
| DLAA | -36% | **0%** |
| Quality | -45% | -8% |
| Performance | -54% | -17% |
| Ultra Performance | -59% | -9% |

The local column grows monotonically and the offloaded one does not. That is the
mechanism stated as a measurement rather than an inference: DLSS-NR runs at
*output* resolution, so its cost is roughly constant across the ladder while the
render work collapses. On the render card it therefore consumes an ever larger
share of each frame.

**At DLAA the offloaded arm is indistinguishable from having the neural stage
switched off.** DLAA is where the render card is most loaded by rendering and has
the least spare capacity to give up, which is exactly where offloading should buy
the least - and it is also where the neural stage's cost, moved off, disappears.

### Slope, and how much of the available gain each arm keeps

| | DLAA to Ultra Performance | share of the off column's gain |
|---|---|---|
| DLSS 5 off | +151% | - |
| on the second card | +129% | **86%** |
| on the render card | +59% | 39% |

Rig A could only say that offloading gained more than local. Rig B says how much
of what the machine actually had to give each arm keeps.

### Conditions, stated rather than smoothed

- The **off** and **render-card** arms were run single-card, with the second GPU
  disabled in Device Manager. The offloaded arm needs both. Section 3 establishes
  on rig A that an enabled-but-idle second card costs nothing when the game
  renders on the card driving its own display, which is the configuration here -
  but the two single-card arms and the two-card arm are not literally the same
  machine state, and that is recorded rather than argued away.
- **Memory is at 4800 MT/s with EXPO off, on a single dual-rank stick**, so
  single-channel. It bounds anything CPU- or bandwidth-limited on this rig and
  every figure in this section was taken that way.
- **DXGI reported `outputs=0` for the bridge adapter** on this machine even with
  a monitor attached to it, so the add-on's own output-count reading is not
  evidence about display topology here in either direction. The topology is
  recorded from the machine: one monitor on each card.
- Temperatures: the render card ran **61 C with the neural stage off and 71 C
  with it on**, same card, same session, open bench - a within-rig delta of about
  10 C. It is **not** comparable to rig A's 21 C figure, which is a different
  comparison (local versus offloaded) in a closed case with worse airflow.
- Cyberpunk 2077 was run on this machine and **produced no figure in this
  document.** Its frame rates were not reproducible across runs, and the add-on's
  own seal reported dropped and reordered frames through those sessions after the
  game rebuilt its swapchain on a settings change. A number taken while the
  instrument is reporting faults is not a measurement.

---

## 5. The neural work is genuinely on the second card

Not an inference. Both cards under load, offload arm, 1440p:

```
0, bus 04:00.0, 5563 MiB, 99%, 63C   <- game, chipset x2
1, bus 06:00.0, 3376 MiB, 94%, 69C   <- neural, CPU slot
```

Against the same card at 0 MiB / 0% in the baseline above.

### Thermal distribution

| | render card | second card |
|---|---|---|
| RenoDX local | **84 °C** @ 98% | 40 °C @ 0% |
| offloaded | **63 °C** @ 99% | 69 °C @ 94% |

The render card runs **21 °C cooler** when the neural work is moved off it. On
a card that throttles, this compounds into the frame-rate difference rather than
being independent of it.

---

## 6. Transport is not the bottleneck at either resolution

If the bridge were the ceiling, the cheaper DLSS modes would flatten out: the
per-frame transport cost does not fall when the game renders at a lower
internal resolution, because the frame transported is always the output frame.

It does not flatten. Frame rate climbs across the whole range at both
resolutions, and the 1440p → 1080p gain is roughly constant at ~45% in every
DLSS mode. On this rig, at these resolutions, the link is not the limit.

---

## 7. Limitations

**Stability is untested beyond the durations recorded here**, sessions of
minutes, not hours. The longest clean run was 16,775 frames (~5.5 minutes) with
the transport reporting no gaps throughout.

~~One session ended with the game rendering black on both displays after several
minutes; the bridge logged no fault and no device removal, and the cause was not
isolated. Candidates included a power limit in force at the time, ReBAR
configuration, and the game engine.~~ **ISOLATED 2026-09-06.** That session was a
**six-pass** run held for several minutes. Of the three candidates listed, the
power one was right and the other two were not, ReBAR was not involved, and the
guess that it might be engine-specific was wrong and is withdrawn. Six passes
exceed the frame period, so the second GPU sits clamped at its power limit
indefinitely, and that is the state the session ended in. **The shipped build now
allows at most two passes**, which is a condition this failure has never been
observed under.

Worth recording as method rather than only as a fix: the entry above said the
cause was "not investigated further, deliberately, out of scope for the
architecture claim." It was in scope. It was the only observed failure that could
damage a user's hardware, and it went unexplained for two days because it was
filed as a curiosity rather than as the one open safety question. **Scope is a
judgement about what a result depends on, not a reason to leave the only
dangerous observation unexamined.**

**One game, one scene, two machines.** Both are dual RTX 5060 Ti 16 GB. Nothing
here says how this scales to higher-tier cards, and no claim is made that it
does - a bigger card changes both the render cost and the neural cost, and which
one moves further is not something this project has measured.

**Quality is not assessed here.** Image quality under this architecture is a
separate question requiring exposure-normalised comparison and is deliberately
excluded. Nothing in this document is a claim about how the output looks.

**The comparison is against one implementation, reached by one route.** The local
arm is RenoDX under ReShade, at matched intensity and pass count, with two
parameters that could not be matched (masking, hook point). **OptiScaler, the
other way to reach DLSS-NR on the render device, was not tested**, and its
performance is not asserted here in either direction. This is not a general claim
about all possible local implementations, and the +30% / +65% shape is a
statement about the arms actually measured.

**Resizable BAR** is enabled in BIOS and left at default in the NVIDIA control
panel for both cards on this machine. It is a BIOS setting and does **not**
travel with the OS disk to rig B, so it must be checked there before any rig B
figure is compared with these.

**DLSS render preset, recorded rather than assumed.** The game's own overlay
reports the super-resolution render preset in use, and a different letter is a
different network. The Blood of Dawnwalker reports **preset K** throughout -
DLSS `v310.2.1`, AppID `141207476`, at every DLSS mode and both resolutions
measured here. It does not vary within this title, so every figure in this
document is preset K.

Recorded because it is not a constant across titles: Tainted Grail, used
earlier in this project for other purposes, reports presets E and F on
DLSS `v3.8.10`. None of its figures appear in this document. Any future
comparison against a different game must record its own preset rather than
inheriting this one.
