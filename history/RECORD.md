# RECORD — what happened, milestone by milestone

The chronological record, P0 → P7, assembled 2026-09-05 from the former
`P0_RECORD.md`, the milestone sections of `../VENDOR_LOCK.md`, and the former
`P5_P6_RECORD.md`. Those three source files no longer exist as separate
documents; every line of them is here, in `FACTS.md`, or in `../VENDOR_LOCK.md`.

**Assembled, not rewritten.** Every milestone below is the original text, moved.
Nothing was regenerated from memory, because the value of this file is that it
was written while the work was happening and carries its own corrections — P2.1's
withdrawn `speedup=` field and its withdrawn verdict line, P3.2's retraction of
the sentinel that had accused NVIDIA of not writing pixels it had written, P1.3's
Finding 1 written narrowly enough that it did not have to be retracted a day
later, and P7's defect H where the first explanation was wrong and the fix that
followed from it changed nothing. A record that has always been right is a record
nobody can calibrate their trust against.

Where the rest lives:

| | |
|---|---|
| `FACTS.md` | verified API facts (§09) and open observations (§10) — the standing reference |
| `../VENDOR_LOCK.md` | the rig, the driver, the vendor stack, the configuration each result was taken under |
| `P1_INSTRUMENT.md` | the instrument design, unedited, including the rules that were written before they were needed |
| `../RESULTS.md` | the published measurements |
| `../METHOD.md` | the working rules these milestones produced |

---

# P0 — a second effect runtime on a second adapter · CLOSED 2026-09-02

## Section numbering — for anyone following a source comment

Comments throughout `src/` cite this document by its former name
(`Agent_Task.md`) and by section numbers that no longer exist. Those references
are historical and the code they annotate is frozen; they were deliberately not
rewritten. Where they now point:

| Comment says | Now in |
|---|---|
| section 05 — layout / closed manifest | **01** · The shipped artifact |
| section 06 — task list, "four rules", "gotcha N" | **01** · invariants, and `FACTS.md` §09 for the underlying facts |
| section 07 — containment | `.github/workflows/build.yml`, which enforces it |
| section 08 — instrumentation | **01** · invariants; the log-line table lives in `../README.md` |
| section 00 "exception N" | Exceptions were per-task freeze carve-outs. They expired when P0 closed; **01** states the invariants they protected. |
| section 09, section 10 | **`FACTS.md`** — same section numbers, own file since 2026-09-05 |

## 00 · What P0 proved

A second, independent ReShade effect runtime can exist on a second physical
adapter inside a running game's process, compile a third-party shader, and
execute it — under stock, unmodified ReShade, while the game renders on the
first GPU and is measurably unaffected.

Verified in gameplay at 2560×1440: game frame rate 45.86 / 45.77 / 45.90 fps
across toggling a 15-pass effect on GPU 1, with no image change and no stutter.

| Task | Outcome |
|---|---|
| T1 — add-on loads under stock ReShade | passed |
| T2 — adapter enumeration and selection | passed |
| T3 — bridge thread and the GPU 1 device | passed |
| T4 — window and message pump | passed |
| T5 — swapchain and present loop | passed |
| T6 — second effect runtime | **satisfied without construction** |
| T7 — shader executes on GPU 1 | **satisfied by configuration** |
| T8 — teardown and run report | passed; the run report is `../VENDOR_LOCK.md` |

**T6 and T7 were never built, and that is the headline result.** ReShade
auto-created an effect runtime for our GPU 1 swapchain the moment T5 created it,
wrote it its own config (`ReShade2.ini`), and compiled the entire shader library
on the second adapter. Enabling `Lumenite_QuantMotion` in a GPU 1 preset with
`DEBUG_FLOW=1` then executed it there. No `create_effect_runtime` call, no
rendering code, no driving loop — a preset file.

The proof that it executed: the bridge window turns **black**. The clear colour
is `0.5 + 0.5·sin(φ)` on three channels at 120° offsets, so the three always sum
to 1.5 and it is never dark. Black cannot be the clear showing through; it is
`PS_Debug` returning `MotionToColor` of a zero flow field, which is correct for a
spatially uniform input. Reproduced three times.

**What P0 did not do.** Nothing crossed between the adapters. Zero bytes. The
synthetic clear colour exists precisely because no game content reaches GPU 1,
and `CrossAdapterRowMajorTextureSupported=0` on this hardware means the simplest
sharing path is unavailable. Transit is P1's subject and is entirely unproven.

## 01 · The shipped artifact

`mgpu_bridge.addon64` — a ReShade 6.8.0 add-on (API 20), built only by GitHub
Actions, deployed beside `dxgi.dll`.

```
src/dllmain.cpp        registration, ReShade events, HMODULE capture
src/diag.{hpp,cpp}     the single logging sink: [MGPU][Tn] prefixes
src/adapter.{hpp,cpp}  enumeration and LUID-exclusion selection
src/worker.{hpp,cpp}   the bridge thread: window, pump, present loop, teardown
src/gpu1_context.*     the GPU 1 device and present chain — raw D3D12 pointers
                       never leave this translation unit
assets/gpu1.ini        the GPU 1 preset
```

`runtime_probe.*` was reserved for T6 and never needed.

**Invariants the code holds, and any successor must preserve:**

- A selection exists only from a **swapchain-derived** game LUID. UE5 creates a
  probe device on every adapter before settling, so `init_device` is provisional
  and never authorises a selection.
- Software adapters are excluded by `DXGI_ADAPTER_FLAG_SOFTWARE` **or** vendor
  `0x1414`. The flag alone is unreliable — this rig has two WARP adapters and
  only one sets it.
- Binding is by LUID, never by enumeration index.
- If filtering does not leave exactly one candidate, **select nothing**. A
  missing device is diagnosable; a device on the wrong adapter succeeds, logs
  cleanly, and proves nothing.
- The selection is a one-shot latched by `S.decided`. That latch is load-bearing:
  after teardown the adapter table holds released pointers, and the latch is what
  stops a later event dereferencing them.
- One bridge thread per launch, started **only** from `init_swapchain`. Starting
  it from `init_device` spawned a thread per probe cycle that could be executing
  inside the module when ReShade unmapped it, and killed the game process.
- Every GPU 1 object is created, used and destroyed on the bridge thread. The
  game thread never blocks and never touches one.
- `worker::stop()` is signal-only and never joins — safe under the loader lock.
- The `ID3D12Device *` and every present-chain object stay inside
  `gpu1_context.cpp`, behind its mutex.
- Teardown order is `gpu1::shutdown()` → `DestroyWindow` → `UnregisterClass` →
  `adapter::shutdown()`, on the bridge thread, before `rearm()`. The swapchain
  holds a reference to its window and must not outlive it, and the GPU is drained
  before anything is released.

## 02 · Working rules that earned their place

These were adopted mid-milestone, each after a specific failure. They are the
transferable part of P0.

**A green log is not a passed task.** Verify each acceptance item against the
code that implements it, not against the log line that would appear if it did. A
poll that was never written and a poll that finds nothing produce identical logs
— and this project shipped exactly that, undetected until the function was
rewritten for another reason.

**A milestone does not flip on a log alone.** After each rig run, review the
source of the files the next task will modify, and of any file whose acceptance
is about to be marked passed, *before* the brief is updated. That review found a
thread-handle leak, a use-after-free ordering bug, and six compile errors that CI
would have cost a round trip each.

**Check requirements against interfaces before writing them.** Every requirement
of the form "call X on Y" presumes something exposes Y. This failed twice — a
device-removal poll specified with no way to reach the device, and D3D12
signatures written from their apparent shape rather than the headers. Read the
headers of every module a task will call into, and write any missing accessor
into the spec.

**When a choice is reversible and cheap, make it and move on.** An agent that
cannot ask questions has no way to end an unresolved decision except by deciding
it. Two options that both compile and both behave correctly are not a question.

**Marking a task passed and compressing it are two separate acts.** Section 00
will happily claim the first while the second never happens. Measure the document
at each milestone.

## 03 · Environment

Recorded fully in `../VENDOR_LOCK.md`. In brief: two RTX 5060 Ti on a 5600X with the
second card on chipset PCIe lanes (deliberate worst case), driver 616.56, ReShade
6.8.0.2155, ReBAR disabled per-program, Carnal Instinct v0.7.8 (UE5, D3D12) at
2560×1440 on a 210 Hz display.

The game renders on the adapter that **has the display attached**; the bridge
target is the **headless** one. UE5 loads and unloads the add-on **five times per
launch** while probing adapters.

## 04 · What P1 inherits

**P1 was renumbered on 2026-09-03, after P1.0 closed.** The original order put
both transit directions before evaluation:

```
old:  P1.0 NGX on GPU 1 -> P1.1 colour out -> P1.2 return -> P1.3 NR in loop
new:  P1.0 NGX on GPU 1 -> P1.1 LOCAL EVALUATE -> P1.2 parameter liveness
                        -> P1.3 TRANSIT -> P1.4 NR in loop -> P2 ring buffer
```

**Status as of 2026-09-03.** P1.0, P1.1 and P1.2 are closed and their facts are
below. **P1.3 is open**: no byte has crossed between the adapters and no transit
figure of any kind exists. `../VENDOR_LOCK.md` carries what it has and has not
established.

That ordering made sense while the pipe looked like the hard part. It stopped
making sense the moment `CreateFeature` returned a handle on GPU 1: evaluating
against GPU-1-local textures costs **no bus traffic at all**, and if the model
will not execute on a display-less adapter then every line of transit code is
dead. Same argument that put P1.0 first, applied again. **A reference to
"P1.3 = NR in loop" in an older document means what is now P1.4.**

---

Read sections 09 and 10 before designing anything. The load-bearing items, most
decisive first:

- **CLOSED 2026-09-03 — DLSS-NR creates a feature on a headless, non-game
  adapter.** This was the item that gated everything.
  `CreateFeature(Reserved18)` returned `Success` with a live handle in
  213 ms on the `outputs=0` adapter, and NVIDIA's own snippet log records
  the network built and 381.8 MB allocated there. Section 09 carries the
  seven-step sequence, the result-code ladder, the parameter keys and the
  allocation table. ~~**Nothing has been evaluated yet** — no pixels have
  passed through the feature. That is P1.3.~~ **Superseded 2026-09-03:** P1.1
  evaluated it and P1.2 established that its parameters are live per evaluate.
  Both are below. The renumbering moved transit to P1.3, which is still open.
- **The DLSS-NR path is public API and needs no effect runtime.**
  `Init_Ext` → private outputs → `CreateFeature(Reserved18)` → `EvaluateFeature`,
  with the driver's own `_nvngx.dll` as parameter provider. A working single-GPU
  run had no ReShade effects loaded at all. P0 already produces every object that
  path needs on GPU 1: device, queue, command list, fence, resources.
- **`CrossAdapterRowMajorTextureSupported = 0`** on this hardware. Transit must
  use a shared **buffer** with `GetCopyableFootprints` and
  `D3D12_PLACED_SUBRESOURCE_FOOTPRINT` at both ends. Rows pad to 256 bytes, so
  payload sizes come from the footprint, not from `width × height × bpp`.
- **Motion vectors need not cross the bus.** QuantMotion derives flow from
  colour, costs 0.13–0.17 ms, and already runs on GPU 1. Its flow buffer is
  320×180 `RG16F`, **0.220 MiB** at 1440p.
- **No no-bridge baseline exists, and that is deliberate.** Every frame-rate
  figure was taken with the bridge running. A baseline is owed before any figure
  is quoted as a result — not before the next task, because it is only meaningful
  once there is a neural workload for it to be a baseline of.

---


## P0 · What passed, and the evidence


| Task | Evidence |
|---|---|
| T1 — add-on loads under stock ReShade | `[MGPU][T1] … API version 20` |
| T2 — adapter selection | `SELECTED adapter[1] … rule="exclusion (luid != swapchain game luid) + software filter"` |
| T3 — device on GPU 1 | `D3D12CreateDevice hr=0x00000000`, both LUID checks pass |
| T4 — window + pump | `client=1280x720`; closing it left the game running a further 23 s |
| T5 — swapchain + present | `CreateSwapChainForHwnd hr=0x00000000`; vsync-locked at 210.0 fps |
| T6 — second effect runtime | Satisfied without code: ReShade auto-created it |
| T7 — shader executes on GPU 1 | QuantMotion with `DEBUG_FLOW=1`; bridge window turns black, reproduced three times |
| T8 — teardown | chain → device → window → class → thread, clean |

**Why black proves execution.** The clear colour is `0.5 + 0.5·sin(φ)` on three
channels at 120° offsets, so the three always sum to 1.5 — it is never dark.
Black cannot be the clear showing through. It is `PS_Debug` returning
`MotionToColor` of a zero flow field, the correct output for a spatially uniform
input.

## Measurements taken at P0 close

| Quantity | Value | Where from |
|---|---|---|
| Game frame rate, `DEBUG_FLOW` 1 / 0 / 1 | 45.86 / 45.77 / 45.90 fps | ReShade Statistics, in gameplay at 1440p |
| QuantMotion on the game's runtime | 0.183 ms CPU, 0.342 ms GPU, 15 passes | same |
| Motion-vector buffer `tFlow` | 320×180 `RG16F`, **0.220 MiB** | same |
| GPU 1 present loop | 210.0 fps, vsync-locked | 600 frames per 2.857 s, two runs |
| `CrossAdapterRowMajorTextureSupported` | **0** | shared **buffers** with placed footprints are mandatory |
| `D3D12_FEATURE_EXISTING_HEAPS` | **1 on both adapters** | path A′ is available |

~~**1920×1080 is owed as a measured point.**~~ **PAID, 2026-09-05.** 1920×1080 is
now measured directly across the full DLSS range in both arms; `../RESULTS.md`
carries the table. Nothing in the published result is reached by interpolation.

~~**No no-bridge baseline was taken, deliberately.**~~ **PAID, 2026-09-05.**
Single-GPU, RenoDX-local and offloaded arms were all taken on the same day, on
the same scene, at the same settings, with both arms rendering on the same card
so that the location of the neural work is the only variable. `../RESULTS.md` has
the method and the caveats.

**Measurement titles are not P0's title.** Everything above in this section was
taken on Carnal Instinct. The published measurements were taken on **The Blood of
Dawnwalker** (AppID `141207476`, DLSS `v310.2.1`, render preset **K** at every
mode and both resolutions). A second title, **Tainted Grail: The Fall of Avalon**
(AppID `140609876`, DLSSv3 `v3.8.10`), was used for arming and compatibility
checks only and reports presets E and F — **do not carry a preset observed in one
title into a statement about another**; that error was made once in this project
and caught by the operator.

**The game's preset was contaminated during testing.** Enabling QuantMotion with
the runtimes synced wrote it into `ReShadePreset.ini`. The *deltas* hold; the
absolute frame rate does not represent a clean game.


---

# P1 → P3 — NGX, transit, and the game's own frame

Moved here from `../VENDOR_LOCK.md` 2026-09-05. That file is the rig and the vendor
stack; these are results, and they were only in it because it was the file being
written when they happened.

## P1.0 → P1.2 — NGX on the headless adapter

### P1.0c — PASSED, 2026-09-03

`nvngx.dll_mgpu_bridge.addon64` only. **No RenoDX, no Streamline shim, no
third-party DLSS add-on, nothing patched, no NVIDIA binary modified.**

| | |
|---|---|
| `CreateFeature(Reserved18)` | **`0x00000001 Success`**, 1280×720, **213 ms** |
| Adapter | LUID `…0001382B`, `outputs=0` — the headless card |
| Snippet's own record | `Created feature 1 (output 1280x720, network 1280x720, preset=0 -> CC_Control_History_Blend_Quantize_With_Teacher_honest_tench_2026_07_04_22_30_weights)` |
| Teardown | `ReleaseFeature` Success, `DestroyParameters` Success, no crash |
| Allocated on GPU 1 | 381.8 MB — weights 140.9 + upload 140.9 + pool 93.0 + history 7.0 |
| Snippet build | v310.8.0 CL 38718415, `m_gpuArch = 0x1b0`, `Fast UAV clear: supported` |

**The sequence that works.** Order is load-bearing; each omission has its own
result code (`P0_RECORD.md` section 09 carries the ladder).

1. `NVSDK_NGX_D3D12_Init` — **core** — application form, with
   `NVSDK_NGX_FeatureCommonInfo`
2. `GetCapabilityParameters` — **core**
3. `NVSDK_NGX_D3D12_Init_Ext` — **snippet**, passing the core's block
4. `NVSDK_NGX_D3D12_PopulateParameters_Impl` — **snippet**
5. `Set("DLSSNR.Width")`, `Set("DLSSNR.Height")` — *not* the generic keys
6. `CreateFeature(Reserved18)` — **snippet**
7. Close → Execute → fence wait → `ReleaseFeature`

`Init_Ext2` **does not exist** in the D3D12 snippet. `Shutdown1` is never
called: the session is kept open for the process lifetime on purpose.

**The core/snippet split is the contract.** The core owns the parameter block;
the snippet owns the feature and **its own session**. Any future work resolving
NGX entry points must respect that division rather than searching one module
first and stopping at the first hit.

**The caller gate is satisfied by the filename.** The snippet resolves its
caller's return-address module and requires the path to contain `nvngx.dll`.
Deploying as `nvngx.dll_mgpu_bridge.addon64` satisfies it. Without the rename:
`0xBAD00002 FAIL_PlatformError`.

### P1.1 — PASSED — NR *executed* on the headless adapter

| | |
|---|---|
| `EvaluateFeature` | `0x00000001 Success`, depth **null**, first attempt |
| Readback | `differing_from_input=914752/921600 (99.26%)`, **`still_sentinel=0`** |
| Reproducibility | **bit-identical across two builds** |
| `CreateFeature` warm | ~220 ms; 1506 ms once on cold model load |
| GPU 1 working set | **388.8 MB** at 1280×720 |

**Depth may be null.** Not a fallback: the depth-free path was tried first and
accepted, and NVIDIA's own log records `Depth=0000000000000000` alongside
`result=0x1`.

### P1.2 — PASSED — parameters are live per evaluate

| Comparison | Result |
|---|---|
| Sentinel survivors | A=0 B=0 C=0 of 921,600 |
| `A(0.00)` vs `B(1.60)` | differing **903,460 (98.03%)** |
| `A(0.00)` vs `C(0.00)` **[CONTROL]** | **0 (0.00%)** — byte-identical |

**Why the control matters.** A and C ran at the same intensity with B between
them. Byte-identical output proves `DLSSNR.Reset` discards `dlssnr_prev_output`,
that evaluate order does not contaminate results, and therefore that the A-vs-B
difference is attributable to intensity alone. Two evaluates could not have
established any of that.

Reproduced across a reboot and a LUID change with byte-identical counts.

**`PollRuntimeParams - callback is NULL` does not mean parameters are frozen.**
Quality is adjustable at runtime with **no feature rebuild**, so the ~220 ms
`CreateFeature` cost is not on the tuning path.

## P1.3 — cross-adapter transit — PASSED, 2026-09-03

**A payload crosses between the two adapters intact, by both routes.**
`differing=0`, sentinel survivors 0, at 1280×720 and 2560×1440.

| Route | Mechanism |
|---|---|
| **A** | `CreateHeap(SHARED \| SHARED_CROSS_ADAPTER)` → `CreatePlacedResource` → `CreateSharedHandle` on the **HEAP** → `OpenSharedHandle` on GPU 1 → place a matching resource there |
| **A′** | one `VirtualAlloc` region, `OpenExistingHeapFromAddress` on both devices, a placed resource on each |

**Share the HEAP, not the placed resource.** A committed resource carries its
own implicit heap and can be shared directly; a placed resource cannot, because
the heap owns the memory. Handing `CreateSharedHandle` the resource returns
`E_INVALIDARG` and reads exactly like a capability refusal from the hardware.
It is not one.

### Finding 1 — committed creation refuses both cross-adapter tokens

| Heap flags | Resource flags | hr |
|---|---|---|
| `SHARED \| SHARED_CROSS_ADAPTER` | `ALLOW_CROSS_ADAPTER` | `0x80070057` |
| `SHARED \| SHARED_CROSS_ADAPTER` | none | `0x80070057` |
| `SHARED` | `ALLOW_CROSS_ADAPTER` | `0x80070057` |
| `SHARED` | none | **`S_OK`** |

**Scope, and why writing it narrowly mattered.** This is the **committed**
creation path only. A fifth row — `CreateHeap` + `CreatePlacedResource` with the
same flags — was added afterwards and **PASSED at both resolutions**. So
`SHARED_CROSS_ADAPTER` is not refused on this rig: it is refused on the
*committed* path and accepted on the *explicit heap* path. This entry first read
"untested; nothing here licenses the sentence 'cross-adapter sharing is
unsupported on this rig'", and that caution is the only reason the finding did
not have to be retracted a day later.

### Finding 2 — the runtime sets `SHARED_CROSS_ADAPTER` itself

`OpenExistingHeapFromAddress` over a single `VirtualAlloc` region succeeded on
**both** adapters. `ID3D12Heap::GetDesc()` on both, identical:

| Field | Value |
|---|---|
| `Properties.Type` | **4 — `CUSTOM`** |
| `CPUPageProperty` | **3 — `WRITE_BACK`** |
| `MemoryPoolPreference` | **1 — `L0`** (system memory) |
| `Flags` | **`0x421`** = `SHARED \| SHARED_CROSS_ADAPTER \| ALLOW_SHADER_ATOMICS` |

**We did not request `SHARED_CROSS_ADAPTER`. The runtime applied it.** So
Finding 1 is not a hardware verdict: the same flag the runtime refuses from us
on a committed resource, it sets unprompted on a heap built from host pages.

`CreatePlacedResource` into that heap needs **`ALLOW_CROSS_ADAPTER`** —
confirmed by a four-row matrix over {flags} × {initial state}; state was never
the variable.

### Finding 3 — the debug layer is unusable from here

`ID3D12Debug::EnableDebugLayer()` called from the bridge thread, after the
process already held live D3D12 devices, **reset every device in the process**:

```
:750  debug layer ENABLED
:751  D3D12CreateDevice (game adapter)  -> DXGI_ERROR_DEVICE_RESET
:752  D3D12CreateDevice (our adapter)   -> DXGI_ERROR_DEVICE_RESET
:759  already-running GPU 1 present-chain device removed, reason DEVICE_RESET
```

Nine milliseconds, one reason code, including a device created long before the
call. Removed permanently. **Recorded as a mid-process contract violation — the
layer is documented as something enabled before any device exists — and
explicitly NOT as a hardware-specific claim.**

Consequence for method: `ID3D12InfoQueue` is unavailable to this probe, so the
runtime cannot be asked to explain a rejection in words. The replacement is the
variant matrix — vary one field per row and let the difference between two rows
name the cause.

### Finding 4 — transit is link-bound *on this cabling*

| 2560×1440 | median |
|---|---|
| `rec0 + sub0 + rec1 + sub1` — all CPU | **0.25 ms** |
| `wait0` — GPU 0 executes | **~36 ms** |
| `wait1` — GPU 1 executes | **2.25 ms** |

CPU overhead is 0.25 ms of a ~37 ms trip. **All of these are perishable**, and
P2.1 later showed the conclusion drawn from them — that pipelining would not
help — was right for a reason this table does not contain (see P2.1).

**Caveat on the same-adapter control, recorded because it was written down
wrongly first.** The control's own first copy is `UPLOAD heap → texture`, and an
upload heap is system memory — so the control crosses PCIe once. It is a
one-crossing baseline, not a no-crossing one, and `wait0 − control` is therefore
**not** "the cost of crossing".

**A versus A′ is NOT decided, deliberately.** One session showed A′ ~11% faster
at 1440p; another showed them indistinguishable. Direction did not reproduce.
**Both paths are carried forward** and both still pass at every milestone since.

## P1.4 — PASSED, 2026-09-04 — NR on transited data

```
pattern on GPU 0  ->  shared cross-adapter heap  ->  GPU 1
                                                     EvaluateFeature(Reserved18)
GPU 0  <-  shared cross-adapter heap  <-  NR output on GPU 1
```

| Comparison | Result |
|---|---|
| vs the **local NR control** (same model, same input, same intensity, no bus) | **differing = 0 of 921,600** |
| vs the raw input pattern | differing = **13,728** |
| Sentinel survivors in the returned buffer | **0** |

**Why the control is the whole verdict.** `ref_local` is P1.2's output A —
captured from that evaluate, not a repeat of it — so the two runs differ in
exactly one variable: whether the input crossed an adapter.

**An unplanned cross-check makes it stronger.** `13,728` is the same count P1.2
recorded for `A vs input` at intensity 0.00, arrived at independently, on a
different code path, with two adapter crossings in between.

**Also settled: NR is deterministic across two evaluates within one session.**
P1.1's reproduction was across process launches; `differing = 0` here required
determinism *within* a session.

## P1.5 — PASSED, 2026-09-04 — the game's own frame crosses

**The application's finished colour buffer, not a pattern of ours.**

| | |
|---|---|
| Source | 2560×1440, **DXGI format 24** (`R10G10B10A2_UNORM`), rowPitch 10240, 14,745,600 bytes |
| Received vs reference | **differing = 0 of 3,686,400** |
| True sentinel survivors | **0** |
| Non-black source pixels | ~3,680,000 of 3,686,400 — real rendered content |
| Reproduced | 3+ distinct frames, across both drivers |

**Mechanism.** The `reshade_finish_effects` event fires on the game's own render
thread with the game's command list open. The heap is created on the **game's
device** — its command list can only reference resources from the device that
made it — and two copies are recorded into that list: one to the cross-adapter
buffer, one to a local readback that serves as the reference for what crossed.
One shot per process, self-disarming, LUID-filtered on both the arm and the
record path.

**Both filters were necessary and both were got wrong first.** The bridge's own
effect runtime raises the same event; without a filter the probe captures its
own window. And ReShade *wraps* D3D12 objects, so comparing device **pointers**
rejects everything — the comparison must be on **adapter LUID**. See
`P1_INSTRUMENT.md` for the failure rows.

## P2.0 — PASSED, 2026-09-04 — the cross-adapter shared fence

**The handoff is ordered, not guessed.**

P1.5 inferred "the copy has completed" from frames elapsed, because we do not
own the game's queue. P2.0 removes the inference.

| Step | Result |
|---|---|
| `CreateFence(SHARED \| SHARED_CROSS_ADAPTER)` on the **game's** device | `hr=0x00000000` |
| `CreateSharedHandle` | `hr=0x00000000` |
| `OpenSharedHandle` on the **NGX** device | `hr=0x00000000` |
| `Signal(1)` on the **game's own queue** | `hr=0x00000000` |
| Handoff confirmed | after **10 bridge presents** |

**A fence can be signalled on a queue this add-on does not own.**
`reshade::api::effect_runtime::get_command_queue()` gives the queue ReShade
itself submits on — which is the queue our copies execute on, and therefore the
only queue a signal placed behind them can be ordered against.

**Signal on the frame AFTER recording, not the same one.** ReShade executes the
list we recorded into *after* the event returns. Signalling in the same event
places the signal ahead of our own copies; the wait would clear before the data
existed — a race that produces a plausible frame most of the time and a torn one
occasionally, which is the worst available failure shape.

**The wait polls, it does not block.** `capture_poll` runs under the same mutex
the game-thread event handler takes. A blocking wait there would hold that lock
across work owned by another process's queue — the one place in this add-on
where a stall could reach the game's render thread. `GetCompletedValue` is a
read, once per present.

**P1.5's 240-frame bound was 24× larger than needed.** The real interval is
~35 ms, roughly one frame — exactly what queue ordering predicts. Perishable as
a duration; invariant as the statement *the bound was a guess and the guess was
wrong by an order of magnitude*.

## P2.1 — PASSED (CORRECTNESS ONLY), 2026-09-04 — copy queues and a banded ring

**What was established:** a 4-band ring, with dedicated `COPY` queues on both
adapters and GPU-side fence ordering between them (`cq1->Wait(prod, i+1)`),
transports the payload byte-exact. Eight runs, both paths, both resolutions,
**8/8 byte-exact in both arms**, every band boundary held.

**What was NOT established, and the claim that was withdrawn:** that the
pipelined discipline is faster. It is not, on this rig.

| 1440p, 14.06 MiB | serial | pipelined |
|---|---|---|
| settled mean over 4 runs | **11.48 ms** | **16.25 ms** |
| stability | ±2% | 11.90 – 18.90 ms |

The pipelined arm was ~1.4× **slower** and never won at that size. At 720p it
appeared to win, but the serial arm there swings 3.50–6.28 ms on identical work
and every apparent "2×" was a slow serial run rather than a fast ring.

**Why, and it is structural rather than a tuning problem.** Both halves of the
pipeline traverse **the same link**. Serial: GPU 0 writes across it, then GPU 1
reads across it. Pipelined: they do it simultaneously over one ×2 path. The
total bytes crossing are identical, so the best case was never a speedup — it
was breaking even — and the extra submissions plus cross-adapter waits made it
worse. **Pipelining pays when the overlapped stages use different resources.**
The overlap that could pay is transfer against *neural execution* on GPU 1, and
P2.1 does not test it.

**The `speedup=` field was removed from the probe.** Across eight runs it read
1.10 / 0.67 / 0.69 / 0.94 / 0.61 at 1440p and 1.08 / 0.50 / 1.15 / 2.24 / 2.00
at 720p — every one a ratio of two noisy single samples. A ratio invites a
claim; the raw pair does not. Both durations are still logged.

**A verdict line was also withdrawn.** It read *"GPU 1 consumed band 0 while
GPU 0 was still producing band 1"*. Nothing in the probe measures that; it was
the mechanism the code was written to produce, asserted in a PASSED line as
though observed. See `P1_INSTRUMENT.md` — this is the third instance of one
recurring error and is recorded there as a pattern.

**P2.1 is kept for its ordering primitive, not as an optimisation.** A working
cross-adapter GPU-side ordering mechanism between two devices, proven correct at
band granularity, is a building block for the overlap that can win.

## P3.0 — PASSED, 2026-09-04 — the game's frame into DLSS-NR

**The chain is closed end to end: game frame → adapter boundary → neural stage.
No stage of it is synthetic.**

| | |
|---|---|
| Input | the game's own transited frame, 2560×1440, **native resolution** |
| `Init` on the already-open session | `0x00000001 Success` |
| `CreateFeature(Reserved18)` at 2560×1440 | `0x00000001 Success`, **179 ms** |
| `EvaluateFeature` A / B / C | `0x00000001 Success` |
| `A(0.00)` vs `B(1.60)` | **3,671,974 pixels (99.61%)** |
| `A(0.00)` vs `C(0.00)` **[CONTROL]** | **0** |
| `A vs input` at intensity 0.00 | **13,492 (0.37%)** |

**Three vendor facts, all invariant:**

1. **The NGX session can be re-entered.** A full second `Init` →
   `GetCapabilityParameters` → snippet `Init_Ext` → `PopulateParameters_Impl`
   sequence on a session that was never shut down returns `Success` and yields a
   **new** parameter block and a **new** feature handle. The prior assumption —
   one feature per launch — was wrong. A production path can create the neural
   stage on demand, at an arbitrary resolution, mid-session.
2. **`CreateFeature`'s ~200 ms is one-time, not per-resolution.** 179 ms at
   2560×1440 against 212 ms at 1280×720 **in the same launch**. Four times the
   pixels, less time.
3. **NR's touch on real content at intensity 0 is ~0.37% of pixels**, stable
   across frames and formats. The first quality-shaped signal this project has
   produced.

**Implementation note.** `ngx_probe` takes an optional external frame. With it
null, every path is byte-identical to the P1 build — the four passing probes
cannot regress because P3 was added. P1.4's transit block is skipped on the
external path, because its control belongs to the synthetic one and comparing a
real frame against a pattern's control would report the difference as a finding.

## P3.1 — PASSED, 2026-09-04 — **native format accepted**

**DLSS-NR consumes the game's buffer in the format the game renders it. No
conversion stage belongs in this pipeline.**

| | |
|---|---|
| NR colour and output textures created as | **DXGI 24 (`R10G10B10A2_UNORM`)**, `hr=0x00000000` |
| Output textures | created with `ALLOW_UNORDERED_ACCESS` **in that format** — typed UAV support is present on this hardware |
| `CreateFeature` / `Evaluate` | Success, unconverted frame |
| `A(0.00)` vs `B(1.60)` | 3,672,299 pixels (99.62%) |
| Control | **0** |

**This deletes a full-resolution per-frame pass** from any production version of
this path. P3.0 paid for a CPU conversion (R10G10B10A2 → R8G8B8A8, two bits per
channel discarded); it is not needed.

**A prediction of mine that the rig disproved, kept because the reasoning was
plausible and still wrong:** I expected texture *creation* to refuse
`ALLOW_UNORDERED_ACCESS` on `R10G10B10A2_UNORM`, and said the failure would be
D3D12's rather than the model's. All six textures were created with
`hr=0x00000000` and NGX took them without complaint.

**Two statistics changed meaning and are not comparable across formats.** The
`A vs B` **mean** moved 8.009 → 71.624 and **max** 66 → 255 between the
converted and native runs. That is not NR behaving differently: mean and max
difference bit fields that straddle byte boundaries on a packed 10:10:10:2
format. **`differing` is the field that stays valid** — 99.61% vs 99.62%.

**Consequence for earlier numbers:** every quality figure taken through the
R8G8B8A8 path was measured two bits per channel short of what the model can see.

**Probe design worth reusing:** native first, converted second, both in one
launch. The capture is one shot per process, so a failed native attempt would
otherwise have cost a relaunch to learn one boolean. Running the fallback
afterwards makes the experiment free.

## P3.2 — PASSED, 2026-09-04 — a persistent feature, and a sound coverage test

**The feature is a pipeline object, not a one-shot.**

| | |
|---|---|
| Evaluates on **one** handle | **16** (two batches of 8), UAV barrier between each |
| Failures | **0** |
| Unwritten pixels (differential test) | **0 of 3,686,400** |

**The differential test, and why it replaces the sentinel.** Evaluate twice —
same input, same intensity — into an output pre-filled `0x00` the first time and
`0xFF` the second. A pixel NR wrote holds the model's value both times and
matches. A pixel NR did not write holds `0x00` once and `0xFF` once and **cannot**
match. The count of differing pixels is therefore the exact number of unwritten
pixels, with **no false positives possible, in any format**.

**It retracts the sentinel survivors.** The native-format runs reported
`A=1 B=0 C=1` survivors of 3,686,400 and the probe warned that NR "did not write
everything it claimed". It did. The absolute-colour sentinel test was matching
**real content that happened to equal the fill** — far likelier on a packed
format, where the sentinel bytes are compared against bit fields straddling byte
boundaries.

**The log's own pattern proves the coincidence.** `A=C=1, B=0` looks suspicious
for chance until you note that A and C are both intensity 0.00 and their control
is `differing=0` — they are *byte-identical outputs*. If a pixel in A equals the
sentinel, the same pixel in C must; B, at a different intensity, needn't. The
pattern is the arithmetic consequence of A≡C, not evidence of a fault.

**`DLSSNR.Output` rebinding is exonerated**, and so is the missing UAV barrier:
P1.2's three evaluates target three different textures, so there was no hazard
to guard.


---

# P4 — the continuous stream · CLOSED 2026-09-04/05

P0 → P3 were one-shot probes: each answered one question once. P4 is the first
milestone that is **continuous** — a persistent neural stage consuming a stream
of the game's frames rather than a single capture — and it is therefore the first
at which every quiet failure in `P1_INSTRUMENT.md` §00 becomes reachable. It is
the milestone that shipped the seal.

What closed in it:

- **P4.0** — one NGX feature handle serving a continuous stream: 600 consecutive
  game frames, 600 evaluates, 0 failures, at native resolution and native format.
- **P4.1** — `DLSSNR.Reset` becomes a first-frame-only key, so temporal history
  carries across frames instead of being discarded per evaluate. The first code
  in the project that lets NR accumulate.
- **P4.2** — the depth question, settled decisively and against expectation.

**The finding that set the architecture:** two evaluates on one captured frame,
identical in every respect except whether a depth texture was bound, produced
output byte-identical over all 3,686,400 pixels — with the depth bound as a
front-to-back gradient rather than a clear, which is what makes it decisive. The
per-frame payload is therefore **colour only**, and the ~2× payload every
bandwidth note in this project had been budgeting for does not exist.

The full entries, with the scope limits they must travel with, are in
`FACTS.md` under *Additions from P4.0 → P4.2*. Two of them are the load-bearing
claims of the whole project: **NGX works on a display-attached adapter**, and
**DLSS-NR running on every frame costs the render GPU nothing measurable.**

---

# P5 / P6 — the session of 2026-09-05

## 00 — Where this session started and ended

It started with the neural output visible in the bridge window for the first
time (P5.0) and no measurements. It ended with:

- the architecture's central claim **measured rather than argued**,
- the offload arm **ahead of the local arm** at the target resolution,
- a shipping display topology chosen on evidence,
- **five defects found**, four of them in our own code, all five of the same
  family: a wrong answer that was perfectly well-formed.

## 01 — Build chain

Tagged commits, oldest first. Each is a working tree, not a patch.

| tag | what it added |
|---|---|
| `p5.0` | neural output on screen; R10G10B10A2 swapchain, 1:1 centre crop |
| `p2.2` | GPU timestamps on GPU 1's consume list (5 marks). **Never run on the rig** |
| `p5.1` | present gate; `Profile=1` |
| `p5.2` | defect C fix; line-anchored ini reader; `Probes=` |
| `p5.3` | `Present=in` — the colour discriminator |
| `p5.4` | P1.6 preset guard (`dllmain.cpp`) |
| `p6.0` | `Passes=N`, one NGX feature handle per pass, ping-pong outputs |
| `p6.1` | defect D fix — ini truncation |
| `p6.2` | defect E fix — the mutex; per-pass `Intensity`; generic `Set.<key>` |
| `p6.3` | intensity on hotkeys (CTRL+ALT+F8 / F9 / F11) |
| `p6.4` | skip-to-newest; runtime pass count; ReShade overlay panel |
| `p6.4a` | scope fix — `run_nr` / `newest` hoisted to frame scope |

---

## 02 — The defect record

All five belong in `P1_INSTRUMENT.md` §00. The common shape: **the instrument
produced a confident, correctly-formatted, wrong answer**, and in four of five
cases every line in the log agreed with every other line.

### Incident 6 — the stale preset (not our code)

The bridge window's neural output came back with the colours destroyed:
psychedelic banding over black. Three code hypotheses were formed and two were
shipped against it. **None was the cause.**

`gpu1.ini` — the preset ReShade assigns to the *bridge* runtime — still carried
from an earlier session:

```
Techniques=Lumenite_QuantMotion@lumenite_QuantMotion.fx
[lumenite_QuantMotion.fx]
PreprocessorDefinitions=DEBUG_FLOW=1
```

A motion-flow debug view was being drawn over every neural frame and nothing in
any log said so. Marcelo found it by reading the file.

Two lessons, both now enforced in code:

1. **The run configuration is part of the measurement and must be in the log.**
   P1.6 makes both runtimes state, once each, their preset path and every
   enabled technique.
2. **A bisect under an uncontrolled variable proves nothing.** The p5.0 → p2.2
   bisect run to find this was contaminated by it; neither half was ever at
   fault.

`Present=in` (p5.3) would have located it in one launch — the *input* would have
been psychedelic too, which immediately clears the neural stage.

### Defect C — one NGX parameter block, two owners

`NVSDK_NGX_D3D12_GetCapabilityParameters` does **not** hand out a per-caller
block; it hands out the core's. `stream_release` had said so in a comment since
P4.1 and acted on it. The P1.0c probe teardown did not, and called
`DestroyParameters` on the block the live stream still held and still wrote
`DLSSNR.Color` / `DLSSNR.Output` into every frame.

Latent since P4.1; P5.1's reordering of `stream_poll` ahead of `present_frame`
exposed it. Fixed by suppressing the destroy while the stream holds the block,
and by making the one-shot probe chain opt-in (`Probes=1`) so two NGX consumers
can never share one block again.

**It was not the cause of the wrong colours** — that was incident 6 — but it was
real, and it was found while looking for them.

### Defect D — the ini reader took 1023 bytes and said nothing

`ini_slurp` read `sizeof buf - 1` into `char buf[1024]`. The shipped `mgpu.ini`,
whose comments this project's own documentation had grown, was **1419 bytes**.

| key | byte | read |
|---|---|---|
| `Frames=` | 82 | yes |
| `Neural=` | 145 | yes |
| `Profile=` | 395 | yes |
| `Probes=` | 625 | yes |
| `Present=` | 896 | yes |
| **`Passes=`** | **1410** | **no** |

Two rig launches executed `Passes=1` while the file said 2 and 4. Every log line
was self-consistent: `passes=1`, `pass 1/1`, `evaluates=3000 (=3000 frames x 1
passes)`. The operator's visual impression of "smoother at x4" was of an image
identical in construction to the x2 one.

**The size was the trigger; the defect was that truncation was silent.** A key
past the cutoff is indistinguishable from a key never written, and "absent" is a
legitimate value. Fixed: 8 KB buffer, and a file that does not fit is now
reported by name.

### Defect E — the stream mutex held across a GPU wait

`stream_poll` took a `lock_guard` for the whole function, including
`WaitForSingleObject` on the fence that completes when GPU 1 has finished the
unpack, every evaluate and the sample. `stream_on_finish_effects` takes that same
mutex **on the game's render thread, every frame**.

So the game's render thread waited on GPU 1's neural work. Invisible at one pass
(~8 ms inside a ~17 ms frame); at two passes (~16 ms) the game fell from ~60 to
**49.8 fps**.

The header had warned about exactly this since P4.0 — *"holding it across a wait
is the one hazard in this add-on that can reach the application"* — and P5.1
applied that discipline to `stream_present_gate` without checking that
`stream_poll` obeyed it.

**The architecture's claim that neural work is free to the game was true of the
design and false of the build.** Fixed with `unique_lock` released across the
wait; the next 2-pass run returned **59.05 fps**.

### Defect F — 83% of the neural work was computed and discarded

`stream_poll` consumes *every* arrived frame before returning; the loop presents
once afterwards. Behind by six frames, it evaluated six and showed one.

The 2-pass run: `evaluates=5302` (2651 frames × 2) against `polls=451`.
**2200 frames of finished neural work thrown away**, which is what turned "17.5 ms
of work against a 16.9 ms budget" into a 9 fps window and drove `overrun=349`.

Fixed in p6.4: every seal is still checked — identity and ordering are the point
and cost microseconds — but the *evaluates* run only on a frame nothing newer has
superseded. Counted as `nr skipped`, deliberately separate from `dropped`:
choosing not to denoise a stale frame is not the transport losing one.

---

## 03 — Measurement record

All figures 1920×1057 windowed unless stated. Producer rate is derived from seal
timestamps over ≥2900 frames. **Perishable: one rig, one link, one scene.**

### The link topology result — the largest single effect found

Same build, same payload to the byte, five minutes apart, one variable: which
card the second monitor's cable is in.

| | both monitors on GPU 0 | one monitor each |
|---|---|---|
| producer | 44.6 fps | **59.1 fps** |
| latency | ~60 ms | **~34 ms** |

**+33% throughput, latency roughly halved, from moving one cable.** With the
display on GPU 1 the neural output is scanned out by the card that produced it:
no cross-adapter DWM copy of the bridge window competing with the payload, and no
return leg. On a link where transport dominates, deleting traffic beats
everything else available.

**This is a shipping decision, not a preference: the neural output display hangs
off GPU 1.**

### Offload versus local, 1080p

| arm | frame rate |
|---|---|
| RenoDX — NR on GPU 0 | 43.17 fps |
| ours — NR on GPU 1 | **~48.4 fps** |

Ahead by ~12%. At 1600×900 it had been a wash (57.2 vs 58.29), so **the crossover
happens between 900p and 1080p** — at the resolution this card tier targets, on
the worst-case link.

Two caveats that must travel with this number:

- The two are not measured the same way — ours is 2928 frames of log timestamps,
  RenoDX's is one instantaneous overlay reading.
- **The arms are not matched on work.** RenoDX ran Model C at Overall Intensity
  0.54; we set `DLSSNR.Intensity 0.84` and no model. A 12% gap is inside what
  that could account for.

Separately and more strongly: **RenoDX's DLSS-NR does not run in dual-monitor at
all.** The offload path runs in a topology where the local path does not. That is
a categorical result and does not depend on matching parameters.

### Scaling

| stage | 1600×900 | 1080p | ratio | pixel ratio |
|---|---|---|---|---|
| unpack | 0.409 ms | 0.590 ms | **1.44×** | 1.44× |
| EVALUATE | 5.949 ms | 7.809 ms | 1.31× | 1.44× |
| latency (mean) | 36.05 ms | 54.33 ms | 1.51× | 1.44× |

Unpack tracks payload size to two decimals — bandwidth-bound and nothing else.

**Evaluate scales sub-linearly**: DLSS-NR has fixed per-invocation overhead, so it
gets *relatively cheaper* as resolution rises. That is part of why the crossover
happened, and it is a good property for this architecture.

Latency scales slightly super-linearly — still link-bound, still what rig B moves.

### Multi-pass

| passes | resolution | producer | GPU 1 / frame | per-pass evaluate (min) | counters |
|---|---|---|---|---|---|
| 1 | 1920×1057 | ~60 fps | 8.3 ms | 7.73 ms | clean |
| 2 | 1920×1057 | **59.05 fps** | 17.5 ms | 7.99 ms | dropped 331, overrun 349 |
| 3 | 2048×1152 | **54.15 fps** | 26.0 ms | 8.45 ms | **all zero** |

The x2 → x1 comparison at identical resolution is the clean one: **~2% for a
doubling of neural work**, inside noise. The x3 run changed resolution as well
(+16% pixels), so it does not isolate pass cost on its own — but 54.15 fps is
*better* than pure pixel scaling of the x2 figure would predict (50.9), which is
consistent with the passes costing the game nothing.

The x2 counters are pre-defect-F. The x3 run, with skip-to-newest in, is
**completely clean at 3 passes**: `produced=3000 consumed=3000 new=3000
dropped=0 reordered=0 overrun=0`, with 1161 frames' neural work deliberately
skipped and 1839 evaluated — a ~34 fps neural output against a 54 fps game.

### `Profile=0` contaminates its own GPU timestamps

`Profile=0` (on-screen output enabled) inflates the tail of **every** timestamp
bracket, because the present chain's submissions share GPU 1's queue:

| | `Profile=1` | `Profile=0` |
|---|---|---|
| EVALUATE min | 7.747 | 7.733 |
| EVALUATE mean | 7.809 | **12.246** |
| EVALUATE max | 8.863 | **75.751** |

The floor is unchanged — the model does the same work — and the same signature
appears in the unpack bracket independently. **Figures of record come from
`Profile=1`.**

One unresolved caveat: across three `Profile=0` runs the floor was 7.733, 7.725
and once **12.122**, with the whole distribution shifted rather than a tail.
Contention that happened to be constant, or GPU 1 clock/power state. Not settled.
Two back-to-back `Profile=1` runs would discriminate.

### The display path costs nothing in the shipping topology

| | `Profile=1` | `Profile=0` |
|---|---|---|
| producer | ~59.1 fps | **59.9 fps** |
| latency | ~34 ms | **33.50 ms** (n=3000) |

Equal. GPU 1 is about half busy and the producer is bound by GPU 0 and the link,
so the local display work is absorbed. `Profile=0` is now the *product*
configuration, not instrument overhead.

---


> **§04 of the original P5/P6 record is not missing.** It was vendor facts, and
> it moved to `FACTS.md` under *Additions from P5 / P6*. The numbering is left
> as it was so that anything citing §05 or §06 still resolves.

## 05 — Still owed

1. **Fault injection has never been run.** Every `STREAM PASSED` line carries its
   own caveat: *"A green run here is not evidence until the fault-injection runs
   in §04 have been seen to trip this same checker."* `Fault=` implements pitch,
   alias, magic, drop and stale.

   Partially discharged by accident: the 2-pass run drove `dropped=331
   reordered=10 overrun=349` from real conditions, so those three counters have
   now been seen to leave zero. `bad_magic`, `contract` and `alias` have not.

2. The **evaluate-floor instability** in `Profile=0` (7.73 vs 12.12) is
   unexplained.

3. The **`presents=` counter** in the P5.1 line includes pre-arm presents and
   reads as though the gate did nothing. Should count from arm.

4. **Model and intensity are not matched** between our arm and RenoDX's. Until
   they are, the 12% is directional.

5. The **overlay panel does not build** without `imgui.h` on the include path.
   The add-on degrades to hotkeys and says so at startup; the panel needs the
   ReShade deps' imgui headers added to the build.

---

## 06 — Method notes worth keeping

- **The visual check and the measured run became different launches** once
  `Profile=1` was adopted. P1.6 is what carries the guarantee across: read the
  preset line in the *measured* log, not just the visual one.
- **In `Profile=1` the seal does not check NR at all** — it checks transport.
  `evaluates=N failures=0` only means the API returned Success. The backstop is
  the evaluate timestamp: a stage doing no work cannot fake ~8 ms.
- **A tuning run is not a measurement run.** Editing intensity mid-stream makes
  the summary cover a moving target, and the summary now says so with the edit
  count.
- **Cross-run comparison is where this project loses time.** Every confounded
  result this session came from comparing launches that differed in more than one
  variable. The single-variable pairs (defect E before/after, cable in GPU 0
  versus GPU 1) produced the clearest results in the record.

### Status of the five, as of P7 close

Annotated 2026-09-05 rather than edited, so that what was owed and what was paid
are both legible.

1. **Fault injection — STILL OWED, and it is the largest open item in the
   project.** Unchanged. `Fault=` implements `pitch`, `alias`, `magic`, `drop`
   and `stale`; `drop`, `reorder` and `overrun` have since been seen to leave
   zero under real conditions, but `bad_magic`, `contract` and `alias` have never
   been observed to trip. Until they have, every `STREAM PASSED` line means "the
   checker did not object", not "the checker works". This is `../METHOD.md` §1 owed
   against this project's own instrument.
2. **Evaluate-floor instability in `Profile=0` (7.73 vs 12.12) — still
   unexplained.** Two back-to-back `Profile=1` runs would discriminate and have
   not been taken.
3. **The `presents=` counter — still counts from process start**, not from arm,
   and still reads as though the gate did nothing.
4. **Model and intensity matching against RenoDX — SUPERSEDED, not paid.** The
   published comparison in `../RESULTS.md` no longer rests on the 12% figure this
   item was about. It rests on the upscaling-shape result, which is a difference
   in *slope across the DLSS range* rather than a difference in a single frame
   rate, and a slope is not something an intensity mismatch produces. The
   parameter caveat is stated in `../RESULTS.md` regardless.
5. **The overlay panel — PAID, 2026-09-05.** It did not build because
   `ext/reshade/deps/imgui` did not exist and CMake silently ignores a missing
   include directory. Fixed by a configure-time fetch of Dear ImGui **1.92.5**
   (`IMGUI_VERSION_NUM` 19250, commit `3912b3d9`) with `#define ImTextureID ImU64`
   before the include. The version is exact, not a floor: ReShade's add-on ABI
   requires the same ImGui the runtime was built against.

---

# P7 — the shipping build · CLOSED 2026-09-05

P5/P6 made it work. P7 made it something a person other than its author can run,
and then measured it properly.

## What was built

| | |
|---|---|
| **P7.1** | `present_resize` places the bridge window on the *monitor's* rect, not the virtual-desktop origin. The old code passed `(0,0)` to `SetWindowPos`, which on a two-monitor rig lands the window on whichever display owns that origin — the game's. |
| **P7.2** | `mgpu.ini` is resolved **beside the add-on**, from `GetModuleHandleEx` + `GetModuleFileNameW`, with the old CWD path kept as a fallback. Which file took effect is logged. Defect G. |
| **P7.3** | Defect H, in two parts. |
| **P7.4** | `Present=` becomes a live toggle (output / input / split); intensity **preset shapes** (manual / front-loaded / back-loaded); `MAX_PASSES` raised to 6. |
| **P7.5** | The split seam is **draggable on hotkeys** — `CTRL+ALT+←/→`, `SHIFT` for a coarse step — clamped one pixel from each edge so the copy can never be zero-width. |
| **P7.6** | `Frames=0` means unbounded, and is the shipped default. The arm line prints a worded bound rather than `18446744073709551615`. |

## Defect G — the settings file was read from the current directory

`mgpu.ini` was opened by bare filename, so it resolved against the process's
working directory — which for a game launched from a launcher is not the game
folder. The file the operator edited and the file the add-on read were different
files, and nothing said so.

**The general form is the one worth keeping: a configuration file addressed by a
relative path is addressed by whoever launched the process, not by whoever
installed the software.** Resolve it against the module, and log which one won.

## Defect H — the present path refused the copy, and the clear colour showed

Two parts, and **the first explanation was wrong**, which is why both are
recorded.

The visible symptom was the bridge window cycling through colours instead of
showing the game — the T5 clear colour, which is `0.5 + 0.5·sin(φ)` on three
channels at 120° offsets and therefore never dark.

I attributed it to `ResizeBuffers` being called with a hardcoded
`R10G10B10A2_UNORM` instead of the format the game actually renders. That was a
real defect and it was fixed. **It was not the cause.** The cause was the guard
on the present path, which compared the new format against the same hardcoded
constant rather than against the swapchain's own format — so the copy was
refused and the clear showed through underneath.

Both are fixed: the chain's format is carried in state (`chain_fmt`) and follows
the game, and the guard compares against it.

**The lesson is `../METHOD.md` §4 in miniature.** A plausible mechanism was
available, it was genuinely a defect, and fixing it changed nothing. A fix that
does not change the symptom has not been shown to be the cause — and the
temptation to close the investigation at that point is exactly what makes this
class of error expensive.

## The seam, and why it is not an overlay feature

`Present=split` shows the frame handed *to* the model on one side of a seam and
what the model produced on the other — **the same frame**, in one window.

It exists because no two runs of a game contain the same frame, so any before/
after comparison assembled from two captures is confounded by everything that
changed in between. The seam moves on hotkeys rather than through the overlay
panel specifically so it can be dragged across a face with nothing on screen but
the game, and so that a video of it is a video of the result rather than a video
of a UI.

## Packaging

The CI artifact was shipping a `gpu1.ini` copied out of a working game folder,
carrying the QuantMotion + `DEBUG_FLOW=1` preset that caused incident 6 — so the
one-click install reproduced the project's own worst diagnostic failure on a new
user's first launch. Root cause was a single `cp assets/gpu1.ini _deploy/` over a
contaminated asset, compounded by ReShade's `AutoSavePreset=1` rewriting presets
at shutdown and re-contaminating them after each manual fix.

Both closed: the shipped `ReShade2.ini` sets `AutoSavePreset=0`, and `build.yml`
gained a step that **fails the build if a shipped preset is non-empty**. The
artifact is six files.

## What P7 did not settle

- Fault injection, still (see above).
- **Stability beyond about five and a half minutes.** One session ended with the
  game rendering black on both displays, with no fault in any log and no cause
  isolated. `Frames=0` makes this reachable by anyone who runs it, which is why
  it is stated in `../README.md` under limitations rather than left in a record.
- **Image quality.** Nothing in this project assesses how the output looks, and
  `Present=split` is a way to look at it, not a measurement of it.
