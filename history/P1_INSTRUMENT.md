# P1 Instrument — how transit proves itself

**Status, 2026-09-04: the design has met the rig and survived, but it has not yet
been *built*.** P1.0 → P3.2 all closed using one-shot probes with sentinel fills
and control comparisons rather than the continuous seal specified below. That was
correct for probes: a probe answers one question once, and a per-frame identity
stream would have been machinery in front of the experiment. **The seal is the
instrument for continuous operation**, and continuous operation is exactly what
comes next. Nothing in this document is superseded; sections 01–05 are still
unbuilt and still required.

Every item marked **[VERIFY]** must be checked against `microsoft/DirectX-Headers`
· `main` · `include/directx/d3d12.h` before it is written as code. If a check
contradicts this document, the header wins and this document is wrong.

This exists because P0's verification methods do not transfer, and that is not
obvious until it has already cost a milestone.

**What the rig taught this document, 2026-09-03/04:**

- **Three new failure rows in section 00**, all of them instrument failures
  rather than transit failures, and all three instances of *one* recurring
  error. Section 00 now names the pattern rather than the three incidents.
- **A general replacement for the sentinel test** — section 04a, the differential
  test — arrived from P3.2 and is stronger than anything this document originally
  specified.
- **A new rule about which numbers are worth taking at all** — section 08.
- **Section 02's premise is confirmed and its scope narrowed.** Latency does come
  free from the clock arithmetic, and the sub-step decomposition it implies is
  what settled P1.3. But `QueryPerformanceCounter` boundaries can only see
  *submit* and *wait*; they cannot separate GPU execution from queue latency
  inside a wait. That needs timestamp queries. `GetClockCalibration` correctly
  remains the last symbol in the containment grep.

---

## 00 · The problem

Every P0 failure was loud. The game died, the build went red, the window was the
wrong colour, a log line was missing. Reading the log and looking at the window
was a sufficient instrument, and it worked.

Transit failures are quiet. The full set, sorted by how easily they are noticed:

| Failure | How it looks | Loud? |
|---|---|---|
| Nothing arrives | black or unchanged window | loud |
| Row pitch confused with `width × bpp` | sheared / skewed image | loud |
| Wrong placed-footprint offset | garbage or displaced image | loud |
| Partial copy into uninitialised memory | visible garbage band | loud |
| Format misread (`R10G10B10A2` as `R8G8B8A8`) | colour shift | semi — reads as a gamma bug |
| Torn frame — copy not fenced | half frame N, half N−1 | **quiet** — looks like ordinary tearing, only on motion |
| Stale — consistently presents frame N−k | **perfect on static content** | **quiet** |
| Intermittent stale — races under load | micro-judder | **quiet** |
| Dropped — transits every other frame | **smooth, if consistently every other** | **quiet** |
| Duplicated — same frame twice | smooth | **quiet** |
| Slot aliasing in the ring | intermittent corruption | **quiet** |
| sRGB double-conversion | slightly washed | **very quiet** |
| Correct content, N frames late | **indistinguishable in a screenshot** | **silent** |

The loud half needs no instrument; it is caught by looking. The quiet half is
where this project will actually fail, and **a human watching the bridge window
will report success on every one of them.**

Note the shape of the quiet set: with one exception (sRGB) they are not failures
of *content*. They are failures of **identity, ordering and time**. The pixels
are usually fine — they are just the wrong frame's pixels, or the right frame's
pixels too late.

### 00a · The instrument's own failures — one pattern, three incidents

Added 2026-09-03, extended twice since. These are not transit failures. They are
the **probe** failing, and together they have cost more than every row in the
table above.

| # | Incident | What it produced |
|---|---|---|
| 1 | **P1.3 `A.1` fallback.** The variant matrix took the first row returning `S_OK` — which carried *no cross-adapter tokens* — and fed that resource into a downstream call it could never satisfy. | A precise hexadecimal answer to a question nobody asked, reported as a bus verdict. |
| 2 | **P1.5e sentinel.** One real mid-grey pixel (`0xA5A5A5`) in a 3.69M-pixel frame matched the sentinel colour, and only the *received* buffer was counted. | `PROBE FAILED` on a run where `differing=0`. |
| 3 | **P1.2 sentinel on a packed format.** `A=1 B=0 C=1` survivors on `R10G10B10A2`, warned as "NR did not write everything it claimed". | A vendor accusation, from content that happened to equal the fill. |

**They are all the same error: an absolute test with no control.** Each asks
"does this value equal a value I chose?" rather than "does this differ from a
run that differs in exactly one variable?" An absolute test cannot distinguish
the condition it is looking for from a coincidence that resembles it, and the
coincidence rate rises with pixel count and with any format where the compared
bytes are not the compared quantity.

**Incident 3 also shows how such a test lies consistently enough to look
structural.** `A=C=1, B=0` reproduced across different frames and looked like a
mechanism. It was arithmetic: A and C are the same intensity and their control
comparison is `differing=0`, so they are byte-identical outputs — a sentinel
match in A *must* recur in C, and B at a different intensity needn't. A false
positive that reproduces is still a false positive.

### 00b · A fourth incident, different mechanism, same family

| # | Incident | What it produced |
|---|---|---|
| 4 | **P2.1's `speedup=` field and its verdict line.** A ratio of two single noisy wall-clock samples, printed per run; and a `PROBE PASSED` line asserting *"GPU 1 consumed band 0 while GPU 0 was still producing band 1"* — the mechanism the code was written to produce, stated as though observed. | Eight ratios between 0.50 and 2.24 that meant nothing, and a claim the probe never measured. |

Two rules come out of it:

- **A derived figure needs the sample count that justifies deriving it.** A ratio
  of two n=1 measurements is not a measurement. Log the raw pair; let the
  analysis divide, if there is enough of it to divide.
- **A verdict may state only what the probe compared.** Intent is not evidence.
  If the log says a thing happened, a comparison in the code must have shown it.

**A fallback is not a fallback if it changes what is being measured.** When a
probe substitutes one configuration for another, the substitution must be
carried into the verdict — which is why the P1.3 `A.1` verdict prints
`eligible=YES/NO` and the downstream result is defined as meaningless unless it
reads YES.

That is the whole design constraint. The instrument does not need to judge
whether an image is correct. It needs to answer, for every frame that arrives on
GPU 1: **which source frame is this, and how old is it?**

---

## 01 · The seal

Write a fixed-size, machine-readable record into the transit payload itself, at
a known offset, produced on GPU 0 and read on GPU 1.

**In the same buffer as the pixels — not beside it.** A seal in a separate
allocation can desync from the payload, and then the instrument is measuring
itself rather than the transit. One allocation, one copy ordering, one fence: if
the seal and the pixels ever disagree about which frame they are, that
disagreement *is* the bug being hunted, and it is detectable rather than masked.

```c
// Fixed layout at offset 0 of the shared buffer. Field order is chosen so that
// natural alignment introduces no padding; do not reorder without rechecking.
struct MgpuSeal
{
    uint32_t magic;         // 'MGPU' — distinguishes stale data from uninitialised
    uint32_t seal_version;  // bump when this struct changes; mismatch is a hard error
    uint64_t frame_index;   // monotonic, from GPU 0's present count. THE identity.
    uint64_t qpc_submit;    // QueryPerformanceCounter when the copy was submitted
    uint64_t payload_bytes; // from the footprint
    uint32_t width;
    uint32_t height;
    uint32_t dxgi_format;   // as an integer, compared not trusted
    uint32_t row_pitch;     // the FOOTPRINT pitch, not width × bpp
    uint32_t slot_index;    // which ring slot this claims to be
    uint32_t barcode;       // frame index as encoded into the pixels — see 03
    uint32_t reserved[2];   // pads to 64; room to grow without a version bump
};
static_assert(sizeof(MgpuSeal) == 64, "seal layout changed");
```

**Assert the size; do not trust a number in a document.** An earlier draft
asserted 64 while listing 56 bytes of fields — exactly the kind of arithmetic
nobody re-checks. The `static_assert` is the requirement; the comment is a
courtesy.

**Why each field earns its place.** `magic` separates "nothing was written" from
"something old was written" — different causes, same appearance. `frame_index`
is the identity that catches dropped, reordered and producer stall.
`qpc_submit` yields end-to-end latency for free (section 02). The geometry
fields turn a pitch or format disagreement into a checked mismatch at the header
rather than a sheared image someone has to notice. `slot_index` catches ring
aliasing. `barcode` closes the loop between the seal and the pixels (section 03).

**`dxgi_format` is no longer a formality.** P3.1 established that the pipeline
runs in the game's native `R10G10B10A2_UNORM` with no conversion. A format
disagreement between the two ends would now be a live possibility rather than a
theoretical one, and it is precisely the kind of failure that reads as a colour
bug.

### Placement

Put the seal at offset 0 of the shared buffer and start the placed footprint at
the first legal texture-placement boundary after it.

**[VERIFY]** `D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT` is believed to be 512
bytes and `D3D12_TEXTURE_DATA_PITCH_ALIGNMENT` 256. If placement alignment is
512, the seal occupies bytes 0–63 of space that would be padding regardless, and
the payload begins at 512. Confirm both constants and confirm that
`GetCopyableFootprints` accepts a non-zero `BaseOffset` and returns offsets
relative to it. Do not assume the arithmetic — read it.

### Writing it, without assuming CPU visibility

**Partially settled on the rig, for one heap kind.** A heap obtained via
`OpenExistingHeapFromAddress` over a `VirtualAlloc` region reports
`CUSTOM / WRITE_BACK / L0` and is CPU-writable by construction: it *is* host
memory. That says nothing about a heap created with `SHARED_CROSS_ADAPTER`.

**The design does not change.** Writing the seal with the GPU, in the same
command list as the pixel copy, requires no CPU visibility of any heap and works
under either transport:

1. CPU fills a small `UPLOAD` buffer on GPU 0 with the seal (`qpc_submit` taken
   at this moment).
2. `CopyBufferRegion` — upload buffer → shared buffer, offset 0, 64 bytes.
3. `CopyTextureRegion` — game texture → shared buffer at the placed footprint.
4. `ExecuteCommandLists`, then signal the shared fence.

Command-list order guarantees the seal is written before the pixels within the
same submission, and the single fence signal covers both.

**The fence is no longer hypothetical.** P2.0 established that a fence created
`SHARED | SHARED_CROSS_ADAPTER` on the *game's* device, opened on ours, and
signalled on the game's own queue via
`effect_runtime::get_command_queue()` orders the handoff exactly. Step 4 above
is built and proven.

**Signal on the frame after recording, not the same one.** ReShade executes the
list we record into *after* the event returns. A signal issued in the same event
sits ahead of our own copies, and the wait clears before the data exists — a
race producing a plausible frame most of the time and a torn one occasionally,
which is the worst available failure shape and sits in the **quiet** half of
section 00's table.

On GPU 1, after waiting the fence: `CopyBufferRegion` the 64 bytes into a
`READBACK` buffer, and map it. **[VERIFY]** `CopyBufferRegion` offset and size
alignment requirements — read the header rather than assuming 4-byte is enough.

### What GPU 1 checks — and why "same frame twice" is normal

**The producer and the consumer run at different rates, and the checker must be
built around that.** `../VENDOR_LOCK.md` records the game at **45.9 fps** and the
GPU 1 present loop at **210 fps** — the consumer is roughly **4.6× faster**.
GPU 1 will read the same `frame_index` four or five times in a row during
entirely correct operation.

A rule of the form *`frame_index` must increase every consume, else STALE* fires
continuously on a healthy run. Repeat identity is the expected case —
**staleness is a property of time, not of repetition.**

Per consume:

```
magic == 'MGPU'                      → else: nothing arrived, or wrong offset
seal_version == expected             → else: hard stop, the two ends disagree
slot_index == the slot we read       → else: RING ALIAS
width/height/format/row_pitch/bytes  → else: CONTRACT MISMATCH, name the field

frame_index == last_seen             → REUSE. Expected. Count it, do not warn.
frame_index >  last_seen             → NEW frame: see below.
frame_index <  last_seen             → REORDERED. Always an error.
```

On a **new** frame only:

```
gap = frame_index - last_seen
gap == 1                             → else: DROPPED, report the gap size
qpc_now - qpc_submit                 → the latency sample
barcode == frame_index               → else: PIXELS AND SEAL DISAGREE (section 03)
last_new_qpc = qpc_now
```

**Sample latency only on new frames.** Re-reading the same seal yields a larger
`qpc_now - qpc_submit` each time, so sampling every consume would smear the
distribution by the reuse ratio and report a transit cost that is mostly
consumer idle time.

Staleness, correctly stated:

```
qpc_now - last_new_qpc > stall_threshold   → PRODUCER STALL
```

Derive the threshold from the observed producer period rather than hardcoding
it. The producer period is measurable from the seal stream itself.

**The reuse ratio is a measurement, not an artefact.** Consumes divided by
distinct frames is the producer/consumer rate ratio. A reuse ratio that drifts
is telling you one side changed pace.

---

## 02 · Latency comes free, and it is the deliverable

`qpc_submit` on GPU 0 and `QueryPerformanceCounter` at consumption on GPU 1 are
the same clock — same process, same machine — so the difference is directly
subtractable with no correlation machinery.

Be precise about what this measures: **submit-to-consume wall-clock age**. It
does not include the game's render time before it, or GPU 1's present after it.

**What it cannot do, confirmed by P1.3 and P2.1.** QPC boundaries see *submit*
and *wait*. They cannot separate GPU execution from queue latency **inside** a
wait, and every duration this project has produced therefore contains driver
overhead and queue time as well as work. P2.1's 16.25 ms pipelined arm is the
sharpest example: it is measurably slower than serial, and the instrument cannot
say how much of that is submission cost and how much is link contention.

**That is what `GetClockCalibration` is for, and it stays guarded until P2.2.**
Section 02's original claim — that the *seal* does not need it — still holds.
Attributing a duration to a stage does.

---

## 03 · The barcode, and what `pattern.fx` is for

The seal proves a *record* arrived intact and in order. It does not prove the
*pixels* beside it belong to that frame — a copy could deliver frame N's seal
with frame N−1's pixels.

Close it by making the pixels carry their own identity: render into the source a
small machine-readable block — the low 16 bits of `frame_index` as 16
black/white cells in a fixed corner region, with a fixed sentinel pattern beside
it so the decoder can confirm orientation. GPU 1 reads back that block and
decodes it.

**It does not catch sRGB double-conversion, and an earlier draft claimed it
did.** Pure black and pure white are the fixed points of that transform, so a
black/white barcode is precisely the pattern that survives it unchanged. The
claim was backwards.

If colour-space integrity is worth checking it needs mid-tones: reference cells
at known intermediate values, compared on GPU 1 against what they were written
as. That is cheap to add and is **not** specified as a requirement — the failure
is cosmetic rather than structural. Until it is added, treat colour space as
unchecked rather than as covered.

**This is what `pattern.fx` is for, and it earns its place twice.** P0
demonstrated QuantMotion *executing* on GPU 1 but not producing a *correct* flow
field — a uniform input can only ever produce zero flow. A pattern with a
barcode and a known-velocity element serves both: transit integrity now, flow
ground truth later.

---

## 04 · The negative control — this is the part not to skip

**An instrument that has never failed is not known to work.** A green run from
an unproven checker means nothing, which is the same error as "a green log is
not a passed task", one level up.

**Select the fault from a file, not an environment variable.** The game is
launched by double-clicking its executable; setting an environment variable for
that is awkward enough that the negative control would get skipped. Read a small
`mgpu.ini` beside `dxgi.dll` — **absent means no fault**, so the shipped default
is a clean run and a missing file is never an error.

```
[MGPU]
Fault=stale
```

| `Fault=` | Injects | Must be reported as |
|---|---|---|
| `stale` | GPU 1 consumes the previous slot | `PRODUCER STALL`, then `REORDERED` when the real frame returns |
| `drop` | GPU 0 skips transit every 3rd frame | `DROPPED gap=2` |
| `tear` | GPU 1 skips the fence wait | `barcode != frame_index`, intermittently |
| `pitch` | GPU 0 writes `width × bpp` as `row_pitch` | `CONTRACT MISMATCH: row_pitch` |
| `alias` | GPU 0 writes the wrong `slot_index` | `RING ALIAS` |
| absent / `none` | nothing | clean run |

**Acceptance is not "a clean run." It is a clean run plus one deliberately
failed run per fault, each producing the named diagnosis.** Afterwards a green
run is evidence instead of an absence of evidence.

Keep the fault switch in the shipped code. The cost is a branch; the benefit is
that the instrument can be re-proven on any future rig, driver or milestone.

---

## 04a · The differential test — the general replacement for a sentinel

**New, 2026-09-04, from P3.2. This supersedes the absolute sentinel wherever a
control is affordable, and section 00a is why.**

A sentinel fill asks: *does this pixel still equal the value I chose?* That is an
absolute test, and section 00a records three occasions on which it answered yes
about a pixel the producer had legitimately written.

The differential form asks nothing absolute. **Run the operation twice, identical
in every respect except the pre-fill of the destination, and compare the two
results to each other:**

```
fill destination with A (e.g. 0x00)   → run → capture R1
fill destination with B (e.g. 0xFF)   → run → capture R2
unwritten pixels == count(R1 != R2)
```

A pixel the operation wrote holds the same produced value in both runs and
matches. A pixel it did not write holds A in one and B in the other and
**cannot** match. The differing count is therefore the *exact* number of
unwritten pixels.

Its properties are what make it worth adopting as the default:

- **No false positives are possible.** There is no value a correct result could
  take that would fake a miss.
- **It is format-independent.** It compares two outputs of the same pipeline, so
  packed, planar and float formats need no special handling — which is precisely
  where the absolute sentinel failed.
- **It costs one extra run**, and the pre-fill machinery already exists.

**When the absolute sentinel is still right:** when a second run is impossible —
a one-shot capture of a live frame, for instance. There, keep the sentinel *and
count it in a control buffer too*, subtracting, exactly as P1.5e was fixed to do.
An absolute test with a control is sound; an absolute test alone is not.

---

## 05 · What the log must say

The P0 rule holds and gets stricter: **log the inputs to the decision, not the
verdict.** `transit OK` is worthless. The agent cannot see the rig, so the log is
the entire channel between the run and the person reading it — and a verdict
cannot be re-examined after the fact while numbers can.

**Log on new frames, not on every consume.** At a 4.6× reuse ratio, per-consume
logging is four fifths noise. Sample every Nth *new* frame, and log every anomaly
unconditionally:

```
[MGPU][SEAL] new f=18432 slot=0 gap=1 reuse=5 lat=4.83ms pitch=10240 fmt=24 bytes=14745600 bc=18432 OK
[MGPU][SEAL] DROPPED f=18437 gap=3 (last_new=18434)
[MGPU][SEAL] PRODUCER STALL: 84.2ms since f=18434 (threshold 65.0ms, producer period ~21.8ms)
[MGPU][SEAL] CONTRACT MISMATCH row_pitch: seal=10240 expected=10200 (frame 18433)
```

At teardown, unconditionally — **this is what catches quiet failures, because a
quiet failure is a rate, not an event:**

```
[MGPU][SEAL] summary: consumes=18600 new_frames=4043 reuse=4.60 dropped=0 reordered=0
[MGPU][SEAL] summary: stalls=0 bad_magic=0 contract_mismatch=0 ring_alias=0 barcode_mismatch=0
[MGPU][SEAL] latency ms: min=3.91 p50=4.77 p90=5.31 p99=6.20 max=11.40 n=4043
[MGPU][SEAL] producer period ms: p50=21.80 (implies 45.9 fps) — compare to the game
[MGPU][SEAL] fault=none seal_version=1
```

A p99 far above p50 is the intermittent race a p50 alone would hide. **`n` on
the latency line is `new_frames`, not `consumes`** — if those two are ever equal,
latency is being sampled per consume and the distribution is wrong.

**Log the fault-injection setting in the summary.** A fault-injected run later
mistaken for a clean one is a self-inflicted false result.

**Two additions from the probe logs, both of which earned their place:**

- **State the discipline in the verdict, not only the numbers.** P2.0 prints
  whether completion was established by the shared fence or inferred from a
  frame count, before the verdict that depends on it. Without that line, a
  fallback result and a real one are indistinguishable.
- **Name the scope in the same sentence as the figure.** P2.1's verdict states
  that its durations cover a narrower path than P1.3's and that neither is a GPU
  timestamp. A number that travels without its scope acquires a wrong one.

**Prefix collision.** P0 owns `[MGPU][T1]`…`[MGPU][T8]`. Use functional or
milestone-namespaced prefixes — the probes settled on `[MGPU][P1.3]`,
`[MGPU][P2.0]`, `[MGPU][P3.1]` and so on, which has worked well in archaeology.
Continue it: `[MGPU][SEAL]`, `[MGPU][XFER]`.

---

## 06 · Where this fits in the task order

**This document does not set the order.** It is a requirement *of* the tasks
that move bytes, not a phase in front of them.

**That was proven correct.** P1.0 through P3.2 all closed without the seal,
because each was a one-shot probe answering one question. Building the seal
first would have delayed every one of them and checked nothing they needed.

**Where it applies now.** The next milestone is the first that is *continuous* —
a persistent neural stage consuming a stream of frames rather than one capture.
That is the point at which every quiet failure in section 00 becomes reachable,
and it is the task that ships the seal:

- **The first task that transits a stream ships the seal with it**, in the same
  task — not before and not after.
- **Fault injection is proven in the same rig session as that task's first clean
  run.** The negative control is not weakened by sharing a session; it only
  requires that the faults be observed to trip the checker before a clean run is
  treated as evidence.
- **`pattern.fx` and the barcode** fold into whichever task first needs
  seal-to-pixel identity.

The rule this section enforces on itself: **verification is a property of a task
that does real work, not a substitute for doing it.**

---

## 07 · What this design does not cover

Stated so it is not mistaken for complete:

- **Content correctness beyond the barcode.** The barcode proves the pixels
  belong to the frame the seal names. It does not prove every pixel arrived
  intact — a corrupt region away from the barcode passes. A sparse checksum over
  fixed sample positions would close that; deliberately not specified, because
  computing it on GPU 0 costs the game GPU time and contaminates the very
  measurement the project exists to take.
- **Photon-to-photon latency.** Section 02 measures submit-to-consume only.
- **Attribution of a duration to a stage.** Needs timestamp queries; P2.2.
- **Whether the bridge costs the game anything.** Still unmeasured,
  deliberately — that baseline is worth taking once there is a neural workload
  for it to be a baseline *of*.
- **Multi-slot ring behaviour under depth > 2.** `slot_index` catches aliasing
  at any depth, but nothing here says what depth is right. P2.1's 4-band ring is
  proven correct but is a *transfer* ring, not a frame ring, and its depth was
  chosen arbitrarily.
- **Colour space.** Section 03 explains why the barcode cannot detect an sRGB
  double-conversion and what would.
- **The consumer being slower than the producer.** Everything above assumes GPU 1
  consumes faster than GPU 0 produces, which is true at the rates in
  `../VENDOR_LOCK.md` **with no neural workload attached**. DLSS-NR is measured at
  ~14.2 ms per evaluate on a comparable single-GPU path, which at 1440p is of the
  same order as the game's whole frame — so this inversion is not hypothetical,
  it is expected. The reuse ratio falling below 1 is what makes it visible.

---

## 08 · Which numbers are worth taking at all

**New, 2026-09-04. This section exists because the project spent a significant
share of its rig sessions producing figures it later had to withdraw or ignore.**

Every quantity belongs to one of two classes:

**Invariant** — survives a slot change, a link width, a resolution, a driver, and
the next milestone. Mechanisms, result codes, capability answers, API
requirements, byte-exactness verdicts.

**Perishable** — a snapshot of one configuration at one time. Every bandwidth,
every millisecond, every ratio.

Reviewing what this project holds:

| Invariant | Perishable |
|---|---|
| NGX core/snippet split; the caller gate | 396 / 700–850 / 1207–1262 MiB/s |
| `Reserved18`, API `0x15`, the `DLSSNR.*` namespace | the 35.53 ms round trip |
| Cross-adapter needs a shared **heap**, not a committed resource | 6.6 ms fixed + 2.29 ms/MiB |
| `OpenExistingHeapFromAddress` works on both adapters | the `wait0`/`wait1` 15× asymmetry |
| NR runs on transited data, byte-identical to a local control | serial 11.48 vs pipelined 16.25 ms |
| The game's own frame crosses byte-exact | 10 presents to fence landing |
| A fence can be signalled on the game's queue from an add-on | `CreateFeature` 179 / 212 / 226 ms |
| GPU-side band ordering between adapters is correct | |
| **NR accepts the game's native format, unconverted** | |
| **The NGX session can be re-entered; features are re-creatable mid-session** | |
| **One feature survives 16 evaluates and writes every pixel** | |

**Every load-bearing claim is in the left column.** The right column has decided
nothing.

Three rules follow:

1. **Do not take a perishable number to make an architectural decision.** Two
   rigs exist and the link width on the measured one is a cabling choice, so a
   bandwidth figure cannot settle anything structural.
2. **Prefer the question whose answer is binary.** *Does NR accept this format?*
   deleted a full-resolution pass from the pipeline. *How fast is the ring?*
   produced eight contradictory ratios.
3. **Profile when there is something worth profiling.** Numbers taken before the
   neural pipeline runs continuously will have to be taken again afterwards,
   against a different workload, on possibly different hardware. The
   architecture-deciding work comes first; the profiling comes when the thing
   being profiled is the thing that will ship.

**A note on contamination, refined.** The probe labels startup-phase runs as
contaminated. An n=36 comparison put startup timings *inside* the manual-run
distribution, which retracted the blanket claim; a later run then showed startup
roughly 2× slower. The honest position is **real but intermittent** — a startup
sample is not automatically void, and it is not automatically comparable either.
Under section 08's rules this matters less than it once did: the numbers it
affects are all perishable.
