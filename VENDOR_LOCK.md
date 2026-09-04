# VENDOR_LOCK — Milestone 0 (Gate P0) and the P1/P2/P3 probes

The exact conditions under which P0 passed, and what the rig runs observed.
Human-owned and human-observed: nothing here can be derived from the repository.
If a later run disagrees with this file, this file describes the run that
happened.

**P0 closed:** 2026-09-02. **P1.0 probe first run:** 2026-09-03.
**P1.3 closed:** 2026-09-03. **P1.4 closed:** 2026-09-04.
**P1.5 closed:** 2026-09-04. **P2.0 / P2.1 closed:** 2026-09-04.
**P3.0 / P3.1 / P3.2 closed:** 2026-09-04.

---

## How to read the numbers in this file

Added at the close of P2, because the project spent a significant share of its
rig sessions producing figures that had to be re-taken and finally stopped.

Every quantity recorded here belongs to one of two classes, and the class
decides whether it is worth defending:

**Invariant** — survives a slot change, a link width, a resolution, a driver,
and the next milestone. Mechanisms, result codes, capability answers, API
requirements, byte-exactness verdicts.

**Perishable** — a snapshot of one configuration at one time. Every bandwidth,
every millisecond, every ratio.

The load-bearing claims of this project are *all* invariant. The perishable
column has consumed a large share of the runs and decided nothing. Both are
kept, but a perishable figure is never the reason to make an architectural
choice, and a section that quotes one says so in the same sentence.

**Corollary, added after the ×2 link turned out to be a cabling choice rather
than a ceiling:** every link-bound number below is a *lower bound on a rig that
can be reconfigured*, not a property of the design.

---

## Repository

| | |
|---|---|
| Commit that built the passing binary | `b307367a` — green, zero warnings at `/W3`, artifact `mgpu_bridge.addon64`. **This is the SHA to reproduce P0 from.** |
| ReShade headers | `crosire/reshade` @ `18deaa52de0c425a78b329e9cb3c497281cd00ec` |
| ReShade add-on API | 20 |
| NGX headers (P1.0) | `NVIDIA/DLSS` @ `a291cc7d2cc642a51566f3dfd5376f635cd1b284` — fetched by CI to `ext/ngx/`, never committed. `nvsdk_ngx_d3d12.h` **does not exist** in this tree; the D3D12 entry points are declared in `nvsdk_ngx.h`. Licence in `THIRD_PARTY.md`. |
| NGX header pin verified | build **#94**, green in 2 m 01 s — the fetch step ran in 45 s and the `NVSDK_NGX_Feature_Reserved18` check passed. Committed and validated before any NGX code existed. |
| CI runner | `windows-latest` → Visual Studio 18 2026, MSVC 19.51.36256.0 |
| Build command | `cmake -B build -A x64` — **no hardcoded generator**; the runner image has moved twice during P0 |
| Link libraries | `dxgi`, `d3d12` only — unchanged through P3. Every Win32 and NGX entry point outside those two is resolved by `LoadLibraryW` + `GetProcAddress`. |
| Deployed filename | **`nvngx.dll_mgpu_bridge.addon64`** since P1.0b. The rename is applied in `build.yml`'s staging step and nowhere else; `CMakeLists.txt` stays closed and CI still verifies the name CMake produces. |
| Containment guard | `build.yml` fails the build on out-of-scope symbols. A symbol moves out **in the same commit that first legitimately uses it, and not before.** Moved: `NVSDK_NGX`/`nvngx` (P1.0), `SHARED_CROSS_ADAPTER`/`HEAP_FLAG_SHARED`/`CreateSharedHandle`/`OpenSharedHandle` (P1.3), `reshade_finish_effects` (P1.5), `FENCE_FLAG_SHARED` (P2.0), `COMMAND_LIST_TYPE_COPY` (P2.1). **Still guarded: `GetClockCalibration` → P2.2.** |

## Rig A — the measurement rig (every number in this file)

| | |
|---|---|
| CPU | Ryzen 5 5600X |
| Topology | Second GPU on **chipset** PCIe lanes, not CPU lanes, negotiating **PCIe 3.0 ×2** (~1.6 GB/s theoretical, ~700–1260 MiB/s observed depending on path and discipline). Deliberate worst case. |
| GPU 0 (game) | NVIDIA RTX 5060 Ti 16GB — vendor `0x10DE`, device `0x2D04`, 16050 MB, `outputs=1` |
| GPU 1 (target) | NVIDIA RTX 5060 Ti 16GB — same vendor/device/memory, `outputs=0` |
| Driver | **616.56** through P2; **616.64** from 2026-09-04 (see below) |
| Display | ASUS VG27AQ5A, 2560×1440 @ **210 Hz**, 8-bit RGB, SDR, VRR not supported |
| ReBAR | **Disabled per-program** for the game executable |
| Inference server | llama-server **stopped** during every rig deploy — it otherwise occupies most of GPU 1's VRAM |

**The ×2 link is a cabling choice, not a ceiling.** Recorded at the close of P2:
this is the configuration that has been measured, not the limit of what the
hardware can do. Any transit figure in this file is therefore a floor.

**Topology note.** The game renders on the adapter that has the display
attached. The bridge target is the **headless** adapter. An earlier assumption
that game and display sat on different cards was wrong and is corrected here.

**LUIDs are never persisted.** Observed during P0 — game `0x18A8C7AD`, target
`0x0001382B`; after a restart on 2026-09-03 the same rig came back as game
`0x00011539`, target `0x000128A8`; later sessions show game `0x000114B7`,
target `0x00012827`. Recorded for log archaeology only. The add-on re-derives
the target every session, and P1.2 demonstrated the re-derivation working
across a reboot rather than asserting it.

**Windows enumerates four DXGI adapters** on this rig: the two RTX 5060 Ti and
**two** "Microsoft Basic Render Driver" software adapters — of which only one
sets `DXGI_ADAPTER_FLAG_SOFTWARE`. Both are excluded by vendor id `0x1414`.

## Rig B — exists, not yet used for any measurement

Recorded so that no figure in this file is mistaken for a hardware limit.

| | |
|---|---|
| CPU | Ryzen 7 **7800X3D** |
| Motherboard | **X870E** (exact model to be confirmed before it is quoted) |
| Lanes | **Bifurcation available** — CPU lanes, not chipset |
| Displays | A **second physical monitor** is available, which rig A did not have |

**Nothing has been run on rig B.** It is listed because it changes how rig A's
numbers must be read: the link, the CPU and the display topology are all
variables the project has been treating as constants. Rig B will be profiled
when there is a neural pipeline worth profiling — not before.

## Game and shaders

| | |
|---|---|
| Target game | Carnal Instinct v0.7.8 with HotFix — Unreal Engine 5, D3D12 |
| Game swapchain | 2560×1440, `R10G10B10A2_UNORM` (**DXGI format 24**), 4 buffers, flip, windowed |
| ReShade | **6.8.0.2155**, add-on-enabled build |
| LumeniteFX | **There is no single LumeniteFX version number.** Each shader carries its own date and they differ across the pack — `lumenite_QuantMotion.fx` reports **2026.06.16**, `lumenite_Kernel.fx` reports **2026.07.28**. What is pinned is the **pack release**; the per-file dates are how you confirm you have the right one. |
| Deploy path | `<game>\Carnal_Instinct_UE5\Binaries\Win64\` — add-on beside `dxgi.dll` |
| Shader path | `…\Binaries\Win64\reshade-shaders\Shaders\` |

**The game's runtime must have at least one effect enabled** for the
`reshade_finish_effects` event to fire on it. Established during P1.5: with no
`.fx` files found, the event fires only on the *bridge's* runtime, the LUID
filter correctly rejects it, and the capture never arms. The log names this
case explicitly.

**Do not treat a per-file date as the pack version.** The only shader P0's
result depends on is `lumenite_QuantMotion.fx` at **2026.06.16**, and section 09
of `P0_RECORD.md` documents that file's behaviour.

## Configuration that made P0 pass

`ReShade.ini` (game runtime, process-global add-on state):

```
[ADDON]
DisabledAddons=Effect Runtime Sync
```

**Drift observed 2026-09-02, after close:** a later rig run shows
`DisabledAddons=Generic Depth,Effect Runtime Sync`. `Generic Depth` was disabled
during DLSS-NR testing and is not part of what made P0 pass.

`ReShade2.ini` (GPU 1 runtime — **written by ReShade**, not by us):

```
PresetPath=.\gpu1.ini
```

`gpu1.ini` (GPU 1 preset — root scope, no `[GENERAL]` header):

```
Techniques=Lumenite_QuantMotion@lumenite_QuantMotion.fx
[lumenite_QuantMotion.fx]
PreprocessorDefinitions=DEBUG_FLOW=1
```

## Driver 616.64 — revalidated 2026-09-04

The first driver with **official NVIDIA DLSS 5 support**, installed mid-project.
Recorded because a vendor stack changing under a project is the classic way a
result quietly stops being reproducible.

| | |
|---|---|
| NGX core | `1.4.0.0` — **unchanged** |
| Feature version | `310.1.0` — **unchanged** |
| SDK | `1.5` — **unchanged** |
| Streamline | `2,14,0,0` — **unchanged** |
| Snippet | v310.8.0 — **unchanged** |
| Every probe P1.0 → P2.1 | **passes unchanged** |

**The officially shipped DLSS 5 NR stack is the same stack this project has been
driving since P1.0.** Nothing in the approach depended on a pre-release
artefact, and no probe needed a change.

## Reference DLSS-NR configuration (single-GPU, GPU 0)

Not part of P0. Recorded because it is the working NGX path on this exact rig
and is the contract this project imitates. From `reshade.ini` `[RENODX-DLSS]`:

| Key | Value | Why it matters |
|---|---|---|
| `DirectNeuralRenderingForceNgxCore` | `1` | **Load-bearing.** Forces the driver's own `_nvngx.dll` to be used as parameter provider. Without it the log reports `NVNGX parameter module not found: nvngx.dll` and NR does not run. |
| `DirectNeuralRenderingHookPoint` | `5` | |
| `DirectNeuralRenderingIntensity` | `0.54` | |
| `DirectNeuralRenderingStyle` | `2` | |

`nvngx_dlssnr.dll` sits beside `dxgi.dll` in the game's `Binaries\Win64\`. NGX
loads it itself; nothing calls into it directly. `nvngx_dlss.dll` and
`sl.interposer.dll` were **absent** in that run — DLSS super-resolution and
Streamline are not required for the NR path.

**Two NGX facts read out of the pinned header, before any NGX code had run.**
One held, one did not. Both are kept, with the outcome marked, because the value
of this section is showing what header-reading can and cannot settle:

- **`Shutdown1` is device-scoped.** The header states that passing a device
  shuts down only that device's instance. The probe always passes the GPU 1
  device, so the game's own NGX session on GPU 0 is not touched.
- ~~**`Init_Ext` is a Core↔Snippet entry point, not an application-facing
  one.**~~ **WRONG — disproved by the rig, 2026-09-03.** The header declares
  `Init_Ext` only under `NGX_SNIPPET_BUILD`, so the inference was that the core
  would not export it. The probe logged `=core` for all seven entry points.
  **A header's preprocessor gating describes what a compiler sees, not what a
  DLL exports.** Kept struck through rather than deleted because the dual-module
  resolver was designed around it and survived being wrong — that is the point
  of it.

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

## Reference DLSS-NR evaluation numbers (single-GPU, third-party tool)

Not ours. Measured by **NeuralOverlay** (`Merserk/dlss5-visual-enhancer`) on this
same rig, driver 616.56.

| Quantity | Value |
|---|---|
| `evaluateGPU` | **avg 14.2 ms**, p95 15.0, p99 15.5 — n=120 per window, stable over 1500 frames |
| `evaluateCPU` | 0.9 ms |
| Resolution | 2248×1264 (88% of 1440p), `passes=2/2` |
| Reliability | `success=3000 failures=0` |
| Path | `signed-snippet` — it reaches NR through the snippet, as we do |
| Motion source | **QuantMotion**, `source 2248x1264 motion 1976x1112 (88%)` |

**Two things this settles.** `path=signed-snippet` independently corroborates
that the snippet route P1.0c found is *the* route. And the QuantMotion→NR
substitution — flow instead of engine motion vectors — is **already demonstrated
working**, with zero failures over 3000 evaluates. It has simply never been done
across two adapters.

**Tuning values** used as P1.1's starting point, taken from that tool's working
configuration rather than invented: `Intensity 0.842`, `LocalToneStrength 1.142`,
`LocalStructureStrength 1.092`, `SkinStructureStrength 1.025`, `UseAutoMask 1`.

### Reaching the NGX core without SetupAPI or a third-party add-on

Two independent paths, both verified:

1. **It is already there.** `GetModuleHandleW(L"_nvngx.dll")` succeeds in the
   game process with only our add-on loaded.
2. **`HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore`** holds the path:

   | Value | Type | Data |
   |---|---|---|
   | `FullPath` | `REG_SZ` | `C:\WINDOWS\System32\DriverStore\FileRepository\nv_dispi.inf_amd64_a3944b54ff18b284` |
   | `Installed` | `REG_DWORD` | `0x1` |
   | `LogLevel` | `REG_DWORD` | `0x0` |

   Append `\_nvngx.dll`. **No new link library at all** — `RegOpenKeyExW` /
   `RegQueryValueExW` / `RegCloseKey` come from `LoadLibraryW(L"advapi32.dll")`
   + `GetProcAddress`.

**The `nv_dispi.inf_amd64_…` hash changes on driver update.** Recorded as an
observation of driver **616.56**: read at runtime, never persisted, never
hardcoded. Check `Installed == 1` before trusting `FullPath`.

## What passed, and the evidence

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

**1920×1080 is owed as a measured point.** The probe measures 1280×720 and
2560×1440; the resolution the viability argument rests on is 1080p, currently
reached by interpolation — exactly the kind of inference this project keeps
catching in itself. Add it once neural rendering runs end to end.

**No no-bridge baseline was taken, deliberately.** A baseline is worth taking
once there is a neural workload for it to be a baseline *of*. Owed before any
figure is quoted as a result, not before the next task.

**The game's preset was contaminated during testing.** Enabling QuantMotion with
the runtimes synced wrote it into `ReShadePreset.ini`. The *deltas* hold; the
absolute frame rate does not represent a clean game.

## Reproducing

1. Build via GitHub Actions. The artifact contains exactly
   `nvngx.dll_mgpu_bridge.addon64` and `gpu1.ini` — the `nvngx.dll_` prefix is
   what satisfies the snippet's caller gate.
2. Place both beside `dxgi.dll`, together with `nvngx_dlssnr.dll`.
3. Apply the `ReShade.ini` and `ReShade2.ini` settings above **with the game
   closed** — ReShade rewrites both on exit.
4. Ensure the **game's** runtime has at least one effect enabled, or the capture
   path will never arm.
5. Set `NGXCore\LogLevel` to a non-zero `REG_DWORD`.
6. Launch. Expect exactly **one** `bridge thread spawned` line.
7. Get into gameplay, then press **Ctrl+Alt+F10** for a manual probe cycle and
   frame capture. **One capture per process, by design.**

**Before reading any NGX result, check `nvngx.log`'s first line.** If it is
`NGXInitValidateSnippets: installed NGX API is older than the one used by client
application`, **open the NVIDIA app and relaunch the game** — that is the fix,
reproduced 2026-09-03, and every earlier clearing event (an app update, a
reboot, a settings change) was an instance of it. A memory corruption after the
game closes is the same fault; opening the NVIDIA app clears it.

**Void the NGX portion, not the launch.** The transit probe builds its own
devices and never touches NGX, so it produces valid results on a launch where
NGX init failed.

`ReShade.log` is overwritten on every launch. Copy it, `nvngx.log` and
`nvngx_dlssnr_*.log` aside **together** before relaunching — they are only
interpretable as a set.
