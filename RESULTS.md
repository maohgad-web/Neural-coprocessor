# Results — rig A

All figures 2026-09-05, one machine, one game. Read the limitations section
before quoting anything here.

Every figure below is measured. An earlier draft carried **CONFIRM** markers on
values that were still owed from the run logs; all of them were filled, and none
remain. If you find one, it is an editing error and the value under it should not
be trusted.

---

## 1. Machine

| | |
|---|---|
| CPU | AMD Ryzen 5 5600, 6C/12T, AM4 |
| Board | Gigabyte B550 AORUS ELITE V2, BIOS FD |
| RAM | 2 × 16 GB DDR4-3600, running at rated speed |
| OS | Windows 11 Pro 25H2, build 26200.9168 |
| Driver | NVIDIA 616.64 (32.0.16.1664) |
| ReShade | 6.8.0.2155, add-on API 20 |
| GPUs | 2 × RTX 5060 Ti 16 GB, both at stock (180 W enforced) |

Both cards are the same model from different board partners — VBIOS
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
measuring ~200–220 MB/s sequential write on this machine's port. The same
physical drive moves to rig B, so storage is a controlled constant rather than
a variable.

Disk affects load times and streaming, not steady-state frame rate in a
resident scene. Figures below are taken stationary for that reason.

---

## 2. Method

- **Game**: The Blood of Dawnwalker (D3D12)
- **Scene**: the game's opening area — the forest at the start of *All Good
  Things* ("Find Lunka in the forest", 166 m marker). Stationary, same spot and
  same camera heading for every arm. `docs/scenes/Scene.png` is that scene as the game
  renders it, with no neural stage — the reference for where every figure below
  was taken. `docs/scenes/Split.png` is the same scene in the bridge's split view: left
  of the seam is the frame handed to the model, right of it is what the model
  produced, and both halves are the same frame.
- **Vsync**: off. Checked explicitly after the 1080p Quality figure looked like
  it might be capped; disabling vsync produced the same number, so the figures
  are real rather than clamped.
- **Clocks, memory, power**: all stock. No power limit applied.
- **ReShade effects**: zero techniques enabled on both runtimes — the game's
  and the bridge's — verified per run from the `[MGPU][P1.6]` log lines.
- **`AutoSavePreset=0`** on both ReShade configs. With it enabled, anything
  toggled in the overlay is written back to the preset at shutdown, so presets
  drift between runs with no file edited. This cost one invalid run before it
  was found.

### Parameter matching between the arms

The local arm is RenoDX **under ReShade**; the offload arm is this bridge.

**Which local path was measured matters, and only one of them was.** DLSS-NR can
be reached on the render device by more than one route, and they are not
interchangeable for a performance measurement — they differ in where they insert
into the frame and in how much of the frame they touch. Everything in this
document was measured through the **ReShade add-on path**. **OptiScaler, the
other route in common use, was not measured at all**, and no figure here should
be read as describing it. If someone reproduces this against OptiScaler and gets
a different local arm, that is a different measurement rather than a
contradiction of this one — and it is a measurement worth having.

Matched:

| | RenoDX | bridge |
|---|---|---|
| intensity | `DirectNeuralRenderingIntensity=1` | `Intensity=1.00` |
| applications per frame | 1 | `Passes=1` |
| encoding | `Encoding=0` | unencoded colour |
| white point | `DiffuseWhiteOverride=0` (not applied) | not set |
| tone/structure strengths | all `1` | not set — assumed NGX default of 1 |

Not matched, and unmatchable:

- RenoDX runs with `AutoMask=1` and masks regions; the bridge masks nothing, so
  it may perform *more* neural work per frame.
- RenoDX hooks at its own chosen point (`HookPoint=5`, order 2); the bridge
  hooks at ReShade's present. Different insertion points in the frame.

The bridge feeds colour and motion vectors only. Depth is null. Tone and
strength parameters are wired but unset.

---

## 3. Baselines — no neural rendering

| render card | link | fps |
|---|---|---|
| bus `04:00.0` | PCIe 3.0 x2 (chipset) | ~60 |
| bus `06:00.0` | PCIe 4.0 x8 (CPU) | ~80 |

Taken at 2560 × 1440, DLSS Quality, game graphics preset High — the same
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
fps — that is the compositor copying every frame across PCIe, not a cost of the
second GPU, and it is avoided by attaching the display to the render card.

---

## 4. Local versus offloaded neural rendering

Both arms render the game on the **same card** — bus `04:00.0`, the chipset
PCIe 3.0 x2 card — so the only variable is where the neural work runs. Verified
per run from GPU memory and utilisation, not assumed.

### 2560 × 1440

| | RenoDX local | offloaded |
|---|---|---|
| DLSS Quality | 33–34 | **40** |

Offload is roughly **18% ahead** on the same render card.

The offload arm across the DLSS range at 1440p: DLAA 32–33, Quality 40,
Performance 50, Ultra Performance 54.

RenoDX at 1440p was measured at DLSS Quality only, deliberately. 1440p exists
in this document to show how the two arms scale with resolution; **1920 × 1080
is the comparison resolution** and carries the full DLSS table below.

### 1920 × 1080

| DLSS mode | RenoDX local | offloaded | delta |
|---|---|---|---|
| DLAA | 43–44 | 48 | +10% |
| Quality | 44–45 | 58 | +30% |
| Performance | 47–48 | 69 | +45% |
| Ultra Performance | 56–57 | 79 | +39% |

### The result that matters

The frame-rate gap is not the finding. The **shape** is.

Across the full DLSS range, from DLAA down to Ultra Performance:

- RenoDX local gains **+30%** (43.5 → 56.5)
- Offloaded gains **+65%** (48 → 79)

With neural rendering local, the neural stage saturates the render GPU. Lowering
the render resolution frees capacity that the neural stage immediately consumes,
so upscaling buys very little — the classic reason for using DLSS stops working.
Offloaded, the render GPU is genuinely freed and DLSS behaves normally.

**Running neural post-processing on the render device largely defeats
upscaling. Moving it to a second device restores it.** That is a structural
claim about where the work runs, not a claim about this implementation's
efficiency, and it is the reason the architecture exists.

That it holds on the worst link in the machine — a chipset-fed PCIe 3.0 x2 slot
— is the point, not a caveat.

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

**Stability is untested beyond the durations recorded here** — sessions of
minutes, not hours. The longest clean run was 16,775 frames (~5.5 minutes) with
the transport reporting no gaps throughout.

~~One session ended with the game rendering black on both displays after several
minutes; the bridge logged no fault and no device removal, and the cause was not
isolated. Candidates included a power limit in force at the time, ReBAR
configuration, and the game engine.~~ **ISOLATED 2026-09-06.** That session was a
**six-pass** run held for several minutes. Of the three candidates listed, the
power one was right and the other two were not — ReBAR was not involved, and the
guess that it might be engine-specific was wrong and is withdrawn. Six passes
exceed the frame period, so the second GPU sits clamped at its power limit
indefinitely, and that is the state the session ended in. **The shipped build now
allows at most two passes**, which is a condition this failure has never been
observed under.

Worth recording as method rather than only as a fix: the entry above said the
cause was "not investigated further, deliberately — out of scope for the
architecture claim." It was in scope. It was the only observed failure that could
damage a user's hardware, and it went unexplained for two days because it was
filed as a curiosity rather than as the one open safety question. **Scope is a
judgement about what a result depends on, not a reason to leave the only
dangerous observation unexamined.**

**One game, one scene, one machine.** Rig B figures are pending.

**Quality is not assessed here.** Image quality under this architecture is a
separate question requiring exposure-normalised comparison and is deliberately
excluded. Nothing in this document is a claim about how the output looks.

**The comparison is against one implementation, reached by one route.** The local
arm is RenoDX under ReShade, at matched intensity and pass count, with two
parameters that could not be matched (masking, hook point). **OptiScaler — the
other way to reach DLSS-NR on the render device — was not tested**, and its
performance is not asserted here in either direction. This is not a general claim
about all possible local implementations, and the +30% / +65% shape is a
statement about the arms actually measured.

**Resizable BAR** is enabled in BIOS and left at default in the NVIDIA control
panel for both cards on this machine. It is a BIOS setting and does **not**
travel with the OS disk to rig B, so it must be checked there before any rig B
figure is compared with these.

**DLSS render preset, recorded rather than assumed.** The game's own overlay
reports the super-resolution render preset in use, and a different letter is a
different network. The Blood of Dawnwalker reports **preset K** throughout —
DLSS `v310.2.1`, AppID `141207476` — at every DLSS mode and both resolutions
measured here. It does not vary within this title, so every figure in this
document is preset K.

Recorded because it is not a constant across titles: Tainted Grail, used
earlier in this project for other purposes, reports presets E and F on
DLSS `v3.8.10`. None of its figures appear in this document. Any future
comparison against a different game must record its own preset rather than
inheriting this one.
