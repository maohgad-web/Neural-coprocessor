# P1 Instrument — how transit proves itself

**Status: design, not verified fact.** Nothing here has been read from a header or
run on the rig. It does not belong in `P0_RECORD.md` section 09, which is reserved
for things confirmed against a primary source. Every item marked **[VERIFY]** must
be checked against `microsoft/DirectX-Headers` · `main` ·
`include/directx/d3d12.h` before it is written as code. If a check contradicts
this document, the header wins and this document is wrong.

This exists because P0's verification methods do not transfer to P1, and that is
not obvious until it has already cost a milestone.

**It does not set P1's task order.** The first task is P1.0 — NGX on the GPU 1
device — which moves no bytes and to which nothing here applies. See section 06.

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
where P1 will actually fail, and **a human watching the bridge window will report
success on every one of them.**

Note the shape of the quiet set: with one exception (sRGB) they are not failures
of *content*. They are failures of **identity, ordering and time**. The pixels are
usually fine — they are just the wrong frame's pixels, or the right frame's pixels
too late.

That is the whole design constraint. The instrument does not need to judge whether
an image is correct. It needs to answer, for every frame that arrives on GPU 1:
**which source frame is this, and how old is it?**

---

## 01 · The seal

Write a fixed-size, machine-readable record into the transit payload itself, at a
known offset, produced on GPU 0 and read on GPU 1.

**In the same buffer as the pixels — not beside it.** A seal in a separate
allocation can desync from the payload, and then the instrument is measuring
itself rather than the transit. One allocation, one copy ordering, one fence: if
the seal and the pixels ever disagree about which frame they are, that disagreement
*is* the bug being hunted, and it is detectable rather than masked.

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

**Assert the size; do not trust a number in a document.** The fields above sum to
56 without the padding — an earlier draft of this section asserted 64 while
listing 56 bytes of fields, which is exactly the kind of arithmetic nobody
re-checks. The `static_assert` is the requirement; the comment is a courtesy.

**Why each field earns its place.** `magic` separates "nothing was written" from
"something old was written" — those have different causes and the same appearance.
`frame_index` is the identity that catches dropped, reordered and producer stall,
and it is what makes the reuse ratio measurable. `qpc_submit` yields end-to-end
latency for free (section 02). The geometry fields turn a pitch or
format disagreement into a checked mismatch at the header rather than a sheared
image someone has to notice. `slot_index` catches ring aliasing. `barcode` closes
the loop between the seal and the pixels (section 03).

### Placement

Put the seal at offset 0 of the shared buffer and start the placed footprint at
the first legal texture-placement boundary after it.

**[VERIFY]** `D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT` is believed to be 512 bytes
and `D3D12_TEXTURE_DATA_PITCH_ALIGNMENT` 256. If placement alignment is 512, the
seal occupies bytes 0–63 of space that would be padding regardless, and the
payload begins at 512. Confirm both constants and confirm that
`GetCopyableFootprints` accepts a non-zero `BaseOffset` and returns offsets
relative to it. Do not assume the arithmetic — read it.

### Writing it, without assuming CPU visibility

A cross-adapter shared heap is not guaranteed to be CPU-writable, and whether it
is on this hardware is unknown. **Do not build the design on that question.**

Write the seal with the GPU, in the same command list as the pixel copy:

1. CPU fills a small `UPLOAD` buffer on GPU 0 with the seal (`qpc_submit` taken
   at this moment).
2. `CopyBufferRegion` — upload buffer → shared buffer, offset 0, 64 bytes.
3. `CopyTextureRegion` — game texture → shared buffer at the placed footprint.
4. `ExecuteCommandLists`, then signal the shared fence.

Command-list order guarantees the seal is written before the pixels within the
same submission, and the single fence signal covers both. No CPU visibility of the
shared heap is required anywhere.

On GPU 1, after waiting the fence: `CopyBufferRegion` the 64 bytes into a
`READBACK` buffer, and map it. **[VERIFY]** `CopyBufferRegion` offset and size
alignment requirements — read the header rather than assuming 4-byte is enough.

The readback costs a map after a fence GPU 1 must wait on anyway. If it ever shows
up in a measurement, check the seal for frame N−1 while consuming frame N —
detection latency is irrelevant, only detection accuracy matters.

### What GPU 1 checks — and why "same frame twice" is normal

**The producer and the consumer run at different rates, and the checker must be
built around that.** `VENDOR_LOCK.md` records the game at **45.9 fps** and the
GPU 1 present loop at **210 fps** — the consumer is roughly **4.6× faster than the
producer**. GPU 1 will therefore read the same `frame_index` four or five times in
a row during entirely correct operation.

A rule of the form *`frame_index` must increase every consume, else STALE* fires
continuously on a healthy run. Repeat identity is the expected case, not the fault
— **staleness is a property of time, not of repetition.**

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
distribution by the reuse ratio and report a transit cost that is mostly consumer
idle time. This is the same mistake as the staleness rule, one layer down.

Staleness, correctly stated:

```
qpc_now - last_new_qpc > stall_threshold   → PRODUCER STALL
```

Derive the threshold from the observed producer period rather than hardcoding it —
some generous multiple, so it fires on a real stop and not on a slow frame. The
producer period is measurable from the seal stream itself.

**The reuse ratio is a measurement, not an artefact.** Consumes divided by
distinct frames is the producer/consumer rate ratio, and it should sit near the
ratio of the two frame rates. A reuse ratio that drifts is telling you one side
changed pace.

That block catches every quiet failure in section 00 except sRGB, which is a
content failure — see section 03 for what does and does not catch it.

---

## 02 · Latency comes free, and it is the deliverable

`qpc_submit` on GPU 0 and `QueryPerformanceCounter` at consumption on GPU 1 are
the same clock — same process, same machine — so the difference is directly
subtractable with no correlation machinery. **`GetClockCalibration` is not needed
and stays in the containment grep**; that API correlates GPU and CPU timelines,
which is a harder problem and not this one.

Be precise about what this measures: **submit-to-consume wall-clock age**, from
the moment GPU 0's copy was submitted to the moment GPU 1 had the bytes. It does
not include the game's render time before it, or GPU 1's present after it. It is
the transit cost, which is exactly the quantity the architecture is being judged
on.

This means the instrument is not overhead paid against P1's schedule. **It is the
first real measurement the project produces**, available on every frame from the
first task that moves a byte, months before M2. A per-frame distribution of
transit latency across an 18,000-frame run is a better number than anything a
later profiling task would construct, and it comes out of the correctness check at
no extra cost.

---

## 03 · The barcode, and what `pattern.fx` is for

The seal proves a *record* arrived intact and in order. It does not prove the
*pixels* beside it belong to that frame — a copy could deliver frame N's seal with
frame N−1's pixels.

Close it by making the pixels carry their own identity. Render into the source a
small machine-readable block — the low 16 bits of `frame_index` as 16 black/white
cells in a fixed corner region, with a fixed sentinel pattern beside it so the
decoder can confirm it is reading the right pixels and the right orientation. GPU 1
reads back that block (a few hundred bytes) and decodes it.

`barcode == frame_index` is then a genuine end-to-end integrity check: the pixels
are provably the ones the seal describes. It costs a trivial shader on GPU 0 and a
tiny readback on GPU 1, and it catches tearing, partial copies and stale pixels
behind a fresh seal.

**It does not catch sRGB double-conversion, and an earlier draft claimed it did.**
Pure black and pure white are the fixed points of that transform — 0 stays 0 and
1.0 stays 1.0 — so a black/white barcode is precisely the pattern that survives it
unchanged. The claim was backwards.

If colour-space integrity is worth checking, it needs mid-tones: a few reference
cells at known intermediate values beside the barcode, compared on GPU 1 against
what they were written as. Linear 0.5 and sRGB-encoded 0.5 differ by roughly 60
levels in 8-bit, which is unmistakable. That is cheap to add and is **not**
specified as a requirement here — the failure is cosmetic rather than structural,
and nothing else in the pipeline depends on it. Add it when there is a reason to
care, and until then treat colour space as unchecked rather than as covered.

**This is what `pattern.fx` is for, and it earns its place twice.** Section 09
records that P0 demonstrated QuantMotion *executing* on GPU 1 but not producing a
*correct* flow field — "a uniform input can only ever produce zero flow. Accuracy
needs `pattern.fx` and known motion." A pattern with a barcode and a known-velocity
element serves both: transit integrity now, flow ground truth later. One asset,
two milestones.

---

## 04 · The negative control — this is the part not to skip

**An instrument that has never failed is not known to work.** A green run from an
unproven checker means nothing, which is the same error as "a green log is not a
passed task", one level up.

Build deliberate fault injection and require that each fault is *observed to trip
the checker* before the instrument is trusted.

**Select it from a file, not an environment variable.** `VENDOR_LOCK.md` records
that the game is launched by double-clicking its executable; setting an
environment variable for that on Windows is awkward enough that the negative
control would get skipped, and skipping it is the one outcome this section exists
to prevent. Read a small `mgpu.ini` beside `dxgi.dll` at startup — **absent means
no fault**, so the shipped default is a clean run and a missing file is never an
error.

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

**Acceptance for P1's first task is not "a clean run." It is a clean run plus one
deliberately failed run per fault, each producing the named diagnosis.** Six rig
launches, and afterwards a green run is evidence instead of an absence of
evidence. It is also the only way to be sure a "clean" run is not a checker that
silently returns true.

Keep the fault switch in the shipped code. The cost is a branch; the benefit is
that the instrument can be re-proven on any future rig, driver or milestone
without rebuilding it.

---

## 05 · What the log must say

The P0 rule holds and gets stricter here: **log the inputs to the decision, not
the verdict.** `transit OK` is worthless. The agent cannot see the rig, so the log
is the entire channel between the run and the person reading it — and a verdict
cannot be re-examined after the fact while numbers can.

**Log on new frames, not on every consume.** At a 4.6× reuse ratio, per-consume
logging is four fifths noise about frames already reported. Sample every Nth *new*
frame, and log every anomaly unconditionally:

```
[MGPU][SEAL] new f=18432 slot=0 gap=1 reuse=5 lat=4.83ms pitch=5120 fmt=24 bytes=7372800 bc=18432 OK
[MGPU][SEAL] DROPPED f=18437 gap=3 (last_new=18434)
[MGPU][SEAL] PRODUCER STALL: 84.2ms since f=18434 (threshold 65.0ms, producer period ~21.8ms)
[MGPU][SEAL] CONTRACT MISMATCH row_pitch: seal=5120 expected=5100 (frame 18433)
```

`reuse=` is how many times the previous frame was consumed before this one
arrived. It should sit near the ratio of the two frame rates; printing it on every
sampled line means a pacing change is visible without waiting for the summary.

At teardown, unconditionally — **this is what catches quiet failures, because a
quiet failure is a rate, not an event:**

```
[MGPU][SEAL] summary: consumes=18600 new_frames=4043 reuse=4.60 dropped=0 reordered=0
[MGPU][SEAL] summary: stalls=0 bad_magic=0 contract_mismatch=0 ring_alias=0 barcode_mismatch=0
[MGPU][SEAL] latency ms: min=3.91 p50=4.77 p90=5.31 p99=6.20 max=11.40 n=4043
[MGPU][SEAL] producer period ms: p50=21.80 (implies 45.9 fps) — compare to the game
[MGPU][SEAL] fault=none seal_version=1
```

`reuse` near the ratio of the two frame rates says both sides are pacing as
expected; a drift in it says one changed. `dropped` against the game's own frame
count is the drop rate. A p99 far above p50 is the intermittent race that a p50
alone would hide. **`n` on the latency line is `new_frames`, not `consumes`** — if
those two are ever equal, latency is being sampled per consume and the
distribution is wrong.

The producer-period line is a free cross-check: it should agree with the frame
rate ReShade's own Statistics panel reports for the game. Disagreement means
transit is not being submitted once per game frame, which no other counter
reveals.

**Log the fault-injection setting in the summary.** A fault-injected run that is
later mistaken for a clean one is a self-inflicted false result, and this is a
cheap guard against it.

**Prefix collision.** P0 owns `[MGPU][T1]`…`[MGPU][T8]`. If P1 numbers its tasks
T1 upward the two milestones become indistinguishable in log archaeology, and the
P0 logs already archived stop being greppable. Use a functional prefix —
`[MGPU][SEAL]`, `[MGPU][XFER]` — or namespace the milestone as `[MGPU][P1T1]`.
Decide this in P1's brief, before the first log line is written.

---

## 06 · Where this fits in P1's task order

**This document does not set P1's order.** It is a requirement *of* the tasks that
move bytes, not a phase in front of them. An earlier draft of this section listed
four instrument tasks ahead of everything else; that was wrong, and the correction
is worth stating because the error is easy to repeat.

**P1.0 — NGX on GPU 1 — comes first, and nothing here changes that.** Initialise
`NVSDK_NGX_D3D12_Init_Ext` against the GPU 1 device that T3 already creates, then
`CreateFeature(Reserved18)`, and read the return codes. No transit, no game data,
no inference. Section 09 records that this is the load-bearing untested assumption
of the whole architecture: if NGX will not initialise a feature on a headless,
non-game adapter, every transit task is work on a pipeline with no consumer.

**The instrument is orthogonal to P1.0.** P1.0 moves no bytes, so there is nothing
for a seal to check. Nothing in this document applies until the first task that
crosses the bus — which is precisely why it must not be scheduled in front of the
experiment that decides whether that bus is worth building.

Where it does apply:

- **The first task that transits anything ships the seal with it**, in the same
  task — not before it and not after. Section 01 is a requirement on that task's
  design, not a task of its own. Building a 64-byte-payload dry run as a separate
  milestone step buys less than it costs; the plumbing it would prove is proven
  equally well by the real payload with a seal attached, and one rig session
  instead of two.
- **Fault injection is proven in the same rig session as that task's first clean
  run** (section 04). The negative control is not weakened by sharing a session —
  it only requires that the faults be run and observed to trip the checker before
  a clean run is treated as evidence. Order within the session matters; a separate
  task does not.
- **`pattern.fx` and the barcode** fold into whichever task first needs
  seal-to-pixel identity. They also land the flow ground truth section 09 says is
  still missing, so the asset earns its place twice — but neither is a reason to
  build it before there is content to put a barcode into.

The rule this section is enforcing on itself: **verification is a property of a
task that does real work, not a substitute for doing it.**

---

## 07 · What this design does not cover

Stated so it is not mistaken for complete:

- **Content correctness beyond the barcode.** The barcode proves the pixels belong
  to the frame the seal names. It does not prove every pixel arrived intact — a
  corrupt region away from the barcode passes. A sparse checksum over fixed sample
  positions would close that; it is deliberately not specified here because
  computing it on GPU 0 costs the game GPU time and contaminates the very
  measurement the project exists to take. Decide it when there is a reason to.
- **Photon-to-photon latency.** Section 02 measures submit-to-consume only.
- **Whether the bridge costs the game anything.** Still unmeasured, deliberately —
  that baseline is worth taking once there is a neural workload for it to be a
  baseline *of*.
- **Multi-slot ring behaviour under depth > 2.** `slot_index` catches aliasing at
  any depth, but nothing here says what depth is right.
- **Colour space.** Section 03 explains why the barcode cannot detect an sRGB
  double-conversion and what would. Until that is added, colour space is unchecked
  — not covered.
- **The consumer being slower than the producer.** Everything above assumes GPU 1
  consumes faster than GPU 0 produces, which is true at the rates in
  `VENDOR_LOCK.md`. If a neural workload later inverts that, the reuse ratio falls
  below 1 and frames are being produced that nobody consumes — a different failure
  needing a different counter. The reuse ratio is what makes the inversion
  visible; noticing it is the point.
