# VENDOR_LOCK — Milestone 0 (Gate P0) and the P1 probes

The exact conditions under which P0 passed, and what the P1 rig runs observed.
Human-owned and human-observed: nothing here can be derived from the repository.
If a later run disagrees with this file, this file describes the run that
happened.

**P0 closed:** 2026-09-02. **P1.0 probe first run:** 2026-09-03.
**P1.3 closed:** 2026-09-03. **P1.4 closed:** 2026-09-04.

---

## Repository

| | |
|---|---|
| Commit that built the passing binary | `b307367a` — green, zero warnings at `/W3`, artifact `mgpu_bridge.addon64`. **This is the SHA to reproduce from.** |
| Commits after it | Documentation and CI packaging only. `src/` has not changed since `b307367a`; a later SHA builds the identical add-on. |
| ReShade headers | `crosire/reshade` @ `18deaa52de0c425a78b329e9cb3c497281cd00ec` |
| ReShade add-on API | 20 |
| NGX headers (P1.0) | `NVIDIA/DLSS` @ `a291cc7d2cc642a51566f3dfd5376f635cd1b284` — fetched by CI to `ext/ngx/`, never committed. `nvsdk_ngx_d3d12.h` **does not exist** in this tree; the D3D12 entry points are declared in `nvsdk_ngx.h`. Licence in `THIRD_PARTY.md`. |
| NGX header pin verified | build **#94**, green in 2 m 01 s — the fetch step ran in 45 s and the `NVSDK_NGX_Feature_Reserved18` check passed. Committed and validated before any NGX code existed. |
| CI runner | `windows-latest` → Visual Studio 18 2026, MSVC 19.51.36256.0 |
| Build command | `cmake -B build -A x64` — **no hardcoded generator**; the runner image has moved twice during P0 |
| Link libraries | `dxgi`, `d3d12` only |
| Deployed filename | **`nvngx.dll_mgpu_bridge.addon64`** since P1.0b. The rename is applied in `build.yml`'s staging step and nowhere else; `CMakeLists.txt` stays closed and CI still verifies the name CMake produces. |

## Rig

| | |
|---|---|
| CPU | Ryzen 5 5600X |
| Topology | Second GPU on **chipset** PCIe lanes, not CPU lanes, negotiating **PCIe 3.0 ×2** (~1.6 GB/s theoretical, ~700–850 MiB/s observed). Deliberate worst case, and the binding constraint on every transit figure in this file. An AM5 8×8 bifurcated CPU-lane system is the intended comparison rig. |
| GPU 0 (game) | NVIDIA RTX 5060 Ti 16GB — vendor `0x10DE`, device `0x2D04`, 16050 MB, `outputs=1` |
| GPU 1 (target) | NVIDIA RTX 5060 Ti 16GB — same vendor/device/memory, `outputs=0` |
| Driver | **616.56** |
| Display | ASUS VG27AQ5A, 2560×1440 @ **210 Hz**, 8-bit RGB, SDR, VRR not supported |
| ReBAR | **Disabled per-program** for the game executable |
| Inference server | llama-server **stopped** during every rig deploy — it otherwise occupies most of GPU 1's VRAM |

**Topology note.** The game renders on the adapter that has the display attached.
The bridge target is the **headless** adapter. An earlier assumption that game and
display sat on different cards was wrong and is corrected here.

**LUIDs observed during P0** — game `0x00000000-0x18A8C7AD`, target
`0x00000000-0x0001382B`. **Recorded for log archaeology only.** LUIDs are
reassigned across driver restarts and reboots and must never be persisted as
configuration; the add-on re-derives the target every session. **Confirmed by a
restart on 2026-09-03:** the same rig came back as game `0x00011539`, target
`0x000128A8`, and selection worked unchanged.

**Windows enumerates four DXGI adapters** on this rig: the two RTX 5060 Ti and
**two** "Microsoft Basic Render Driver" software adapters — of which only one
sets `DXGI_ADAPTER_FLAG_SOFTWARE`. Both are excluded by vendor id `0x1414`.

## Game and shaders

| | |
|---|---|
| Target game | Carnal Instinct v0.7.8 with HotFix — Unreal Engine 5, D3D12 |
| Game swapchain | 2560×1440, `R10G10B10A2_UNORM`, 4 buffers, flip, windowed |
| ReShade | **6.8.0.2155**, add-on-enabled build |
| LumeniteFX | **There is no single LumeniteFX version number.** Each shader carries its own date and they differ across the pack — `lumenite_QuantMotion.fx` reports **2026.06.16**, `lumenite_Kernel.fx` reports **2026.07.28**. What is pinned is therefore the **pack release**, identified by its repository release date; the per-file dates below are how you confirm you have the right one. The installed pack necessarily postdates 2026.07.28, the latest file date observed in it. |
| Deploy path | `<game>\Carnal_Instinct_UE5\Binaries\Win64\` — add-on beside `dxgi.dll` |
| Shader path | `…\Binaries\Win64\reshade-shaders\Shaders\` |

**Do not treat a per-file date as the pack version, and do not expect the two to
agree.** LumeniteFX versions each shader independently. An agent that reads
`2026.06.16` out of `lumenite_QuantMotion.fx` and goes looking for a
"LumeniteFX 2026.06.16" release will not find one. The only shader P0's result
depends on is `lumenite_QuantMotion.fx` at **2026.06.16**, and section 09 of
`P0_RECORD.md` documents that exact file's behaviour — technique name, the
`DEBUG_FLOW` pass, the pyramid levels, the `FRAME_COUNT == 0` black frame. If a
later pack ships a different QuantMotion date, section 09 is the thing to
re-verify against, not this table.

## Configuration that made P0 pass

`ReShade.ini` (game runtime, process-global add-on state):

```
[ADDON]
DisabledAddons=Effect Runtime Sync
```

**Drift observed 2026-09-02, after close:** a later rig run shows
`DisabledAddons=Generic Depth,Effect Runtime Sync`. `Generic Depth` was disabled
at some point during DLSS-NR testing and is not part of what made P0 pass. It is
recorded here so a future run that differs is not mistaken for a regression.

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

## Reference DLSS-NR configuration (single-GPU, GPU 0)

Not part of P0. Recorded because it is the working NGX path on this exact rig and
is the contract the next milestone imitates. From `reshade.ini` `[RENODX-DLSS]`:

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

Observed timings on this rig: `Init_Ext` → `CreateFeature(Reserved18)` took
**1.16 s**; private outputs are `2560×1440`, `format=24` (`R10G10B10A2_UNORM`),
`flags=0x5` (RT|UAV), created lazily — one at init, three more about six seconds
later, matching the game swapchain's `BufferCount=4`.

**Two NGX facts read out of the pinned header, before any NGX code had run.**
One held, one did not. Both are kept, with the outcome marked, because the
value of this section is showing what header-reading can and cannot settle:

- **`Shutdown1` is device-scoped.** The header states that passing a device
  shuts down only that device's instance and that passing `nullptr` shuts down
  all of them. The probe always passes the GPU 1 device, so the game's own NGX
  session on GPU 0 is not touched. This closes what was an open risk.
- ~~**`Init_Ext` is a Core↔Snippet entry point, not an application-facing
  one.**~~ **WRONG — disproved by the rig, 2026-09-03.** The reasoning was that
  the header declares `Init_Ext` only under `NGX_SNIPPET_BUILD`, so the core
  would not export it and the snippet would have to answer. The probe logged
  `=core` for all seven entry points, `Init_Ext` among them: `_nvngx.dll`
  exports it regardless of how the header gates the declaration. **A header's
  preprocessor gating describes what a compiler sees, not what a DLL exports.**
  The inference is kept struck through rather than deleted because the
  dual-module resolver was designed around it, and the design survived being
  wrong — that is the point of it.

## P1.0 results — observed on the rig, 2026-09-03

The milestone's primary question, answered. Binary from commit B; log
`ReShade.log` of the `20:24:22:051` run.

| Observation | Value |
|---|---|
| `NVSDK_NGX_D3D12_Init_Ext` | `result=0x00000001 (Success)` on the **headless** adapter, LUID `0x00000000-0x0001382B`, `outputs=0` |
| `GetCapabilityParameters` | Success on GPU 1 |
| Export provenance | all seven entry points `=core` (`_nvngx.dll`) |
| `_nvngx.dll` | `0x00007FFE82D60000`, **already resident** — no DLSS add-on in the process, RenoDX absent from the log |
| `nvngx_dlssnr.dll` | loads with plain `LoadLibraryW` from `Binaries\Win64\` |
| `CreateFeature(Reserved18)` | `0xBAD0000B FAIL_UnableToInitializeFeature`, ~0 ms, with only `Width`/`Height` set |
| Teardown | crashes; still being localised. Reproduces with RenoDX absent, so it is not our `Shutdown1` colliding with another NGX session. |

**NGX runs on the non-render device.** That is the load-bearing result of the
project so far, and it is now an observation rather than a hypothesis.

## P1.0b results — observed on the rig, 2026-09-03

Deployed as `nvngx.dll_mgpu_bridge.addon64`, **no RenoDX, no other DLSS add-on
in the process.** Log `ReShade.log` of the `21:36:47:364` launch, plus NGX's own
`nvngx.log` from the same run.

| Observation | Value |
|---|---|
| Caller gate | **satisfied by the rename** — no `0xBAD00002 FAIL_PlatformError` |
| `CreateFeature(Reserved18)` via snippet | `0xBAD00007 FAIL_NotInitialized`, 1 ms |
| Meaning | the snippet has its **own** NGX session; initialising the core is not enough |
| Snippet exports `Init` / `Init_Ext` | **yes** — both modules carry them |
| Snippet exports `GetCapabilityParameters` / `DestroyParameters` | **no** — core only |
| Core `Init` (application form, logging callback) | `Success` on LUID `0001382B` |
| Teardown crash | **`NVSDK_NGX_D3D12_Shutdown1`**, localised by announce-before-call |
| Logging callback | installed at VERBOSE, produced **zero** lines — driver used its file sink |
| NGX driver log | `NGXCheckArchitectureSupport` matched NVAPI GPU handle `0x600`, LUID `{0x0, 0x1382b}` — GPU 1 |
| NGX versions | core `1.4.0.0`, feature `310.1.0`, SDK `1.5`, driver `616.56` |

**The core/snippet split is the contract.** The core owns the parameter block;
the snippet owns the feature and its own session. Any future work that resolves
NGX entry points must respect that division rather than searching one module
first and stopping at the first hit.

**`Shutdown1` is not called any more, on purpose.** P1.1 keeps NGX initialised
for the process lifetime, so one session per launch reclaimed at process exit is
the end state, not a workaround for the crash.

### Enabling NGX's own log

Set `HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore\LogLevel` to a non-zero
`REG_DWORD`; the log lands under `%PROGRAMDATA%\NVIDIA\NGX\Logs`. It is a
driver-side witness independent of anything this project writes, and it carried
the adapter confirmation above. It is *not* reachable through an
`NVSDK_NGX_FeatureCommonInfo` logging callback — that was installed at VERBOSE
in the same run and produced nothing.

## P1.0c — PASSED, 2026-09-03

`nvngx.dll_mgpu_bridge.addon64` only. **No RenoDX, no Streamline shim, no
third-party DLSS add-on, nothing patched.** Logs: `ReShade.log`, `nvngx.log`
and `nvngx_dlssnr_310_8_0.log` of the `00:25:40` launch.

| | |
|---|---|
| `CreateFeature(Reserved18)` | **`0x00000001 Success`**, `handle=0x000001CC85E9AAC0`, 1280×720, **213 ms** |
| Adapter | LUID `00000000-0001382B`, `outputs=0` — the headless card |
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
called.

**What this does and does not establish.** A DLSS-NR feature — the real
network, the real weights, the real kernels — exists and tears down cleanly on
a GPU that drives no display and renders no frame of the game. It has **not**
been evaluated: no image has passed through it, and no performance claim of any
kind is supported by this run.

## P1.1 — PASSED, 2026-09-03

**DLSS-NR executed on the headless adapter.** `nvngx.dll_mgpu_bridge.addon64`
only; no RenoDX, no shim DLL, nothing patched. Logs of the `14:17:33` launch.

| | |
|---|---|
| `EvaluateFeature` | `0x00000001 Success`, depth **null**, first attempt |
| Readback | `differing_from_input=914752/921600 (99.26%)`, **`still_sentinel=0`**, `mean_abs_delta=5.324`, `max_abs_delta=252` |
| Reproducibility | **bit-identical across two builds** — all four statistics |
| `CreateFeature` warm | ~220 ms (213 / 236); 1506 ms once on cold model load |
| GPU 1 working set | **388.8 MB** at 1280×720 |
| Snippet's own record | `EvaluateFeature Color=… MVec=… Depth=0000000000000000 Output=… intensity=0.84 reset=1` / `color (0,0 1280x720) mvec (0,0 1280x720) scale (1.00,1.00)` |

**The sentinel is why this counts.** The output texture is pre-filled with a
constant that cannot occur in the test pattern. `still_sentinel=0` means NR
overwrote every pixel — without it, an uninitialised texture would also have
"differed from the input" and read as a pass.

**Depth may be null.** Not a fallback: the depth-free path was tried first and
accepted, and NVIDIA's log records `Depth=0000000000000000` alongside
`result=0x1`.

**Open: `PollRuntimeParams - callback is NULL (core did not set it)`.** The
echoed `intensity=0.84` proves the parameter was read, not that it was applied.
Whether tuning is per-evaluate or baked at create is unsettled, and it decides
whether a quality change costs a ~220 ms rebuild. **Settled by P1.2 — see
below.**

**Record the telemetry versions with any quoted result.** `StreamlineVersion`
moved from `2,12,129,0` to `2,14,0,0` between runs on this same rig and driver,
with the game untouched — `OTAEnabled = 1` and `nvngx_update.exe` runs at init.

## P1.2 — PASSED, 2026-09-03

**DLSS-NR's tuning parameters are live per evaluate on GPU 1.** Three evaluates
on one command list, one deterministic input, three sentinel-filled outputs.
Log of the `14:44:05` launch.

| Comparison | Result |
|---|---|
| Sentinel survivors | **A=0 B=0 C=0** of 921,600 — all three outputs fully written |
| `A(0.00)` vs `B(1.60)` | differing **903,460 (98.03%)**, mean 3.968, max 31 |
| `A(0.00)` vs `C(0.00)` **[CONTROL]** | **0 (0.00%)** — byte-identical |
| `CreateFeature` | Success, 1350 ms (cold, after an NVIDIA app update) |

**Why the control matters.** A and C ran at the same intensity with B between
them. Byte-identical output proves `DLSSNR.Reset` discards
`dlssnr_prev_output`, that evaluate order does not contaminate results, and
therefore that the A-vs-B difference is attributable to intensity alone. Two
evaluates could not have established any of that.

**Dose-response, against the same input pattern:**

| `DLSSNR.Intensity` | pixels changed vs input | mean abs delta |
|---|---|---|
| 0.00 | 13,728 (1.49%) | 1.985 |
| 0.84 | 914,752 (99.26%) | 5.324 |

Intensity 0 is near-passthrough. The model tracks the value; it does not merely
respond to it.

**Reproduced a third time, 2026-09-03 `16:16` and `16:34`, across a reboot.**
Sentinels `A=B=C=0`; `A(0.00)` vs `B(1.60)` = **903,460 (98.03%)**; control
`A(0.00)` vs `C(0.00)` = **0**. Byte-identical to the `14:44` run despite a
system restart and reassigned LUIDs (GPU 1 `0x128a8`, previously `0x1382b`).
Parameter liveness now survives a rebuild, a reboot **and** a LUID change. The
LUID note near the top of this file is why that is worth stating: the add-on
re-derives the target every session, and this is the first result that
demonstrates the re-derivation working across a boot rather than asserting it.

**`PollRuntimeParams - callback is NULL` does not mean parameters are frozen.**
That callback is something the core normally installs and we do not need.
Quality is adjustable at runtime with **no feature rebuild**, so the ~220 ms
`CreateFeature` cost is not on the tuning path.

## P1.3 — cross-adapter transit, **PASSED**, 2026-09-03

**A payload crosses between the two adapters intact, by both routes.** Verified
by byte comparison at 1280×720 and 2560×1440: `differing=0`, sentinel survivors
0, on path A (shared cross-adapter heap) and path A′ (host-pinned heap opened on
both devices).

It took five builds, and two of them died to defects in the probe rather than in
the rig. Both are recorded, because a milestone that keeps only its successes
teaches nothing about how it was reached.

### The two routes, and the shape that made A work

| Route | Mechanism |
|---|---|
| **A** | `CreateHeap(SHARED \| SHARED_CROSS_ADAPTER)` → `CreatePlacedResource` → `CreateSharedHandle` on the **HEAP** → `OpenSharedHandle` on GPU 1 → place a matching resource there |
| **A′** | one `VirtualAlloc` region, `OpenExistingHeapFromAddress` on both devices, a placed resource on each |

**Share the HEAP, not the placed resource.** A committed resource carries its own
implicit heap and can be shared directly; a placed resource cannot, because the
heap owns the memory. Handing `CreateSharedHandle` the resource returns
`E_INVALIDARG` and reads exactly like a capability refusal from the hardware. It
is not one.

Probe design: `transit_probe()` creates **its own** D3D12 device on each
adapter, so the game's device and P0's GPU 1 device are never borrowed. Two
resolutions per run, 1280×720 and 2560×1440. Logs of the `16:16:18` and
`16:34:58` launches.

### Retracted before it was ever recorded as a result

**`A.3 OpenSharedHandle` returning `0x80070057` is NOT a capability result, and
the probe said it was.** The `16:34` build ran a variant matrix at `A.1` and
stopped at the first variant that returned `S_OK` — which was the row carrying
**no cross-adapter tokens at all**. `A.2`/`A.3` therefore ran against a resource
that was never eligible to cross an adapter, and its refusal on the second
adapter is specified behaviour for a plain `SHARED` handle. The log line calling
`A.3` "the call that answers whether the adapters can share" was written into the
probe by the assistant and was wrong in that context.

**That was a defect in our harness, not in the rig**, and it was caught only
because the verdict line printed the winning variant next to the downstream
result. The probe now prints `eligible=YES/NO` beside the A.1 verdict and defines
the downstream result as meaningless unless it reads YES. This is recorded rather than quietly fixed because the
failure mode — *a probe that falls back to a variant which cannot answer the
question, then reports that variant's failure as the answer* — was not covered
by anything in `P1_INSTRUMENT.md` section 00, and now is.

### Finding 1 — committed creation refuses both cross-adapter tokens

`CreateCommittedResource` on the game adapter. Buffer, `ROW_MAJOR`,
`DXGI_FORMAT_UNKNOWN`, height/depth/mips = 1, size an exact multiple of 64 KB,
`HEAP_TYPE_DEFAULT`, node masks 1/1. All five requirements satisfied and echoed
as numbers in the log before the call (`A.0 desc echo`).

| Heap flags | Resource flags | hr |
|---|---|---|
| `SHARED \| SHARED_CROSS_ADAPTER` | `ALLOW_CROSS_ADAPTER` | `0x80070057` |
| `SHARED \| SHARED_CROSS_ADAPTER` | none | `0x80070057` |
| `SHARED` | `ALLOW_CROSS_ADAPTER` | `0x80070057` |
| `SHARED` | none | **`S_OK`** |

Identical at both resolutions. **Each cross-adapter token is refused
independently** — neither is merely invalid in combination with the other.

**Scope, and why writing it narrowly mattered.** This is the **committed**
creation path only. A fifth matrix row — `CreateHeap` + `CreatePlacedResource`
with the same flags — was added afterwards and **PASSED at both resolutions**.
So `SHARED_CROSS_ADAPTER` is not refused on this rig: it is refused on the
*committed* path and accepted on the *explicit heap* path. This entry first read
"untested; nothing here licenses the sentence 'cross-adapter sharing is
unsupported on this rig'", and that caution is the only reason the finding did
not have to be retracted a day later.

### Finding 2 — the runtime sets `SHARED_CROSS_ADAPTER` itself

`OpenExistingHeapFromAddress` over a single `VirtualAlloc` region succeeded on
**both** adapters (`hr=0` on each). `ID3D12Heap::GetDesc()` on both, identical:

| Field | Value |
|---|---|
| `SizeInBytes` | as requested, 64 KB-rounded |
| `Alignment` | 65536 |
| `Properties.Type` | **4 — `CUSTOM`** |
| `CPUPageProperty` | **3 — `WRITE_BACK`** |
| `MemoryPoolPreference` | **1 — `L0`** (system memory) |
| `Flags` | **`0x421`** = `SHARED (0x1) \| SHARED_CROSS_ADAPTER (0x20) \| ALLOW_SHADER_ATOMICS (0x400)` |

**We did not request `SHARED_CROSS_ADAPTER`. The runtime applied it.** So
Finding 1 is not a hardware verdict: the same flag the runtime refuses from us on
a committed resource, it sets unprompted on a heap it builds from host pages.
The two findings together are narrower and more useful than either alone.

`CreatePlacedResource` into that heap failed `0x80070057` on both adapters with
initial state `COMMON` **and** with `GENERIC_READ`, so state was never the
variable. The descriptor carried `Flags = NONE`. **Adding
`ALLOW_CROSS_ADAPTER` fixed it** — confirmed by a four-row matrix over
{flags} × {initial state}. D3D12 pairs a `SHARED_CROSS_ADAPTER` heap with an
`ALLOW_CROSS_ADAPTER` resource, and the runtime having set that heap flag itself
is what made the requirement non-obvious.

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
call. Removed permanently; the source carries the timestamps so it is not
reintroduced. **Recorded as a mid-process contract violation — the layer is
documented as something enabled before any device exists — and explicitly NOT as
a hardware-specific claim.** No 50-series-specific evidence was gathered.

Consequence for method: `ID3D12InfoQueue` is unavailable to this probe, so the
runtime cannot be asked to explain a rejection in words. The replacement is the
variant matrix — vary one field per row and let the difference between two rows
name the cause. That is what produced Finding 1.

### Finding 4 — transit is link-bound, and the link is the rig's floor

The decomposition that settled it. `wait0` is GPU 0's fence wall-clock, `wait1`
GPU 1's; `rec`/`sub` are CPU-side recording and submission.

| 2560×1440 | median |
|---|---|
| `rec0 + sub0 + rec1 + sub1` — all CPU | **0.25 ms** |
| `wait0` — GPU 0 executes | **~36 ms** |
| `wait1` — GPU 1 executes | **2.25 ms** |

CPU overhead is 0.25 ms of a ~37 ms trip, so **pipelining alone cannot recover
this cost** — it is not synchronisation, it is execution. A same-adapter control
(identical two copies, destination local to GPU 0) runs **~17.7 ms for one link
crossing** against ~36 ms for two: almost exactly 2×, an effective **~700–850
MiB/s**.

**That is what a PCIe 3.0 ×2 chipset link delivers** (~1.6 GB/s theoretical),
and this rig is deliberately that worst case — second card on chipset lanes, not
CPU lanes. **These absolute numbers are a property of this interconnect. They do
not judge the architecture, and no figure here may be quoted as if they did.**
What they legitimately decide is A versus A′, because both are measured over the
same link.

**A versus A′ is NOT decided.** A first paired session showed A′ ~11% faster at
1440p; a second session showed the two indistinguishable. Direction did not
reproduce, so that gap is noise until shown otherwise. **Both paths are carried
forward.** Choosing before there is a neural workload to discriminate them means
choosing without the evidence that would.

**Caveat on the control, recorded because it was written down wrongly first.**
The control's own first copy is `UPLOAD heap → texture`, and an upload heap is
system memory — so the control crosses PCIe once. It is a one-crossing baseline,
not a no-crossing one, and `wait0 − control` is therefore **not** "the cost of
crossing". A true zero-link baseline is deliberately not measured: it is not
worth taking until there is a neural workload for it to be a baseline *of*.

### What P1.3 established

| | |
|---|---|
| A payload crosses between the adapters intact | **yes** — both paths, both resolutions, `differing=0` |
| Path A: shared cross-adapter heap | **yes** — share the heap, place on both sides |
| Path A′: one host allocation opened on both devices | **yes** — placed resources need `ALLOW_CROSS_ADAPTER` |
| Which path is better | **undecided, deliberately** |
| Any transit figure that survives leaving this rig | **no** — every number here is link-bound |

## P1.4 — PASSED, 2026-09-04

**DLSS-NR ran on a payload that crossed the bus, and the result came back
unchanged.** This is the result the project was built to test. Log of the
`mgpu.log` run whose `nvngx.log` opens with a clean telemetry block; 1280×720,
path A, synthetic deterministic pattern.

The sequence, in one command flow per leg:

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
| `EvaluateFeature` on transited input | `0x00000001 Success` |
| Round trip incl. the evaluate | 16.07 ms |

**Why the control is the whole verdict.** `ref_local` is P1.2's output A —
captured from that evaluate, not a repeat of it — so the two runs differ in
exactly one variable: whether the input crossed an adapter. "It looks processed"
and "it differs from the input" are both satisfied by an uninitialised buffer,
which is what the sentinel exists to exclude.

**An unplanned cross-check makes it stronger.** `13,728` is the same count P1.2
recorded for `A vs input` at intensity 0.00. Arrived at independently, on a
different code path, with two adapter crossings in between. The transited result
is not merely close to the local one — it sits at the identical distance from
the input, which holds only if the bytes are the same bytes.

**Something previously untested is now settled: NR is deterministic across two
evaluates within one session.** P1.1's bit-identical reproduction was across
process launches. `differing = 0` here required determinism *within* a session,
and had it failed it would have looked exactly like transit corruption — the
probe's inconclusive branch exists for that case and did not fire.

**What P1.4 does NOT establish.** 1280×720 only. A synthetic pattern, **not the
game's frame** — that needs the ReShade effect-runtime finish hook, which stays
in the containment guard until the milestone that uses it, and a synthetic
pattern is what makes a byte-exact verdict possible at all. One crossing each
way with a CPU fence between them, no pipelining, taken during startup. **16.07
ms is not a performance figure**: transit alone at this size measures ~10–11 ms,
so the evaluate added roughly 5 ms on an otherwise idle adapter, and that is an
observation about this run rather than a cost model.

## Reference DLSS-NR evaluation numbers (single-GPU, third-party tool)

Not ours. Measured by **NeuralOverlay** (`Merserk/dlss5-visual-enhancer`) on this
same rig, driver 616.56, and recorded here because it is the target this project
is trying to reproduce on the wrong GPU — and because it is the first evidence
that DLSS-NR is expensive enough to be worth moving at all.

| Quantity | Value |
|---|---|
| `evaluateGPU` | **avg 14.2 ms**, p95 15.0, p99 15.5 — n=120 per window, stable over 1500 frames |
| `evaluateCPU` | 0.9 ms |
| `submit` | 0.08 ms |
| `slotWait` | 0.001 ms |
| Resolution | 2248×1264 (88% of 1440p), `passes=2/2` |
| Reliability | `success=3000 failures=0` |
| Path | `signed-snippet` — it reaches NR through the snippet, as we do |
| Motion source | **QuantMotion**, `source 2248x1264 motion 1976x1112 (88%) flow grid 247x139 searchRadius=3` |
| Config | `guidanceMode=2 autoMask=true uiCorrection=false depthInterval=4 preset=0 style=0` |

**Two things this settles.** `path=signed-snippet` independently corroborates
that the snippet route P1.0c found is *the* route, not something we invented.
And the QuantMotion→NR substitution — flow instead of engine motion vectors —
is **already demonstrated working**, with zero failures over 3000 evaluates. It
has simply never been done across two adapters. That makes P1.4 an engineering
task, not a research question.

**What it does not settle: whether depth may be omitted.** `guidanceMode=2` is
real and logged — an earlier doubt of mine was wrong. But `depthInterval=4` sits
in the same status line, which reads as depth being supplied on a *cadence*, not
absent. That tool has the game's depth buffer and no reason to test the null
case. **P1.1 tested it directly and depth-null was accepted on the first
attempt.**

**Tuning values** used as P1.1's starting point, taken from that tool's working
configuration rather than invented: `Intensity 0.842`, `LocalToneStrength 1.142`,
`LocalStructureStrength 1.092`, `SkinStructureStrength 1.025`, `UseAutoMask 1`.

**The arithmetic this creates.** Colour at 2248×1264 `R10G10B10A2` is 10.8 MiB
one way, 21.7 MiB round trip; over chipset PCIe at ~6 GB/s effective that is
roughly **3.8 ms**, against **14.2 ms** of work removed from the render GPU.
Both transit figures are *theoretical* — `P1_INSTRUMENT.md` exists to replace
them with measured ones, and no such number may be quoted as a result until it
is measured on this rig. The ratio is what makes the architecture worth
building, not a claim in itself.

### Reaching the NGX core without SetupAPI or a third-party add-on

Two independent paths, both verified on this rig:

1. **It is already there.** `GetModuleHandleW(L"_nvngx.dll")` succeeds in the
   game process with only our add-on loaded. The locator is off the critical
   path — it is a portability concern, not a dependency.
2. **`HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore`** holds the path:

   | Value | Type | Data |
   |---|---|---|
   | `FullPath` | `REG_SZ` | `C:\WINDOWS\System32\DriverStore\FileRepository\nv_dispi.inf_amd64_a3944b54ff18b284` |
   | `Installed` | `REG_DWORD` | `0x1` |
   | `ShowDlssIndicator` | `REG_DWORD` | `0x400` |
   | `LogLevel` | `REG_DWORD` | `0x0` |

   Append `\_nvngx.dll`. No `setupapi.lib`, and **no new link library at all** —
   `RegOpenKeyExW` / `RegQueryValueExW` / `RegCloseKey` come from
   `LoadLibraryW(L"advapi32.dll")` + `GetProcAddress`, the same way the NGX
   exports are resolved, so `CMakeLists.txt` stays closed at `dxgi`, `d3d12`.

**The `nv_dispi.inf_amd64_…` hash changes on driver update.** It is recorded
here as an observation of driver **616.56**, in the same spirit as the LUIDs
above: read at runtime, never persisted as configuration, never hardcoded.
Check `Installed == 1` before trusting `FullPath`.

Fallback order, cheapest first: `GetModuleHandleW` → registry `FullPath` →
`LoadLibraryW(L"_nvngx.dll")` on the default search path.

## What passed, and the evidence

| Task | Evidence |
|---|---|
| T1 — add-on loads under stock ReShade | `[MGPU][T1] … API version 20` |
| T2 — adapter selection | `SELECTED adapter[1] luid=…0001382B rule="exclusion (luid != swapchain game luid) + software filter"` |
| T3 — device on GPU 1 | `D3D12CreateDevice hr=0x00000000`, both LUID checks pass |
| T4 — window + pump | `window created: … client=1280x720 (0,0,1280,720)`; closing it left the game running a further 23 s |
| T5 — swapchain + present | `present chain created: … CreateSwapChainForHwnd hr=0x00000000`; `frame 18600` reached; vsync-locked at 210.0 fps |
| T6 — second effect runtime | Satisfied without code: ReShade auto-created it — `Recreated runtime environment on runtime <ptr> ('ReShade2.ini')` on the bridge thread |
| T7 — shader executes on GPU 1 | Satisfied by configuration: QuantMotion enabled in `gpu1.ini` with `DEBUG_FLOW=1`; bridge window turns black, reproduced three times |
| T8 — teardown | `present chain released` → `device released` → `window destroyed` → `class unregistered` → `bridge thread exiting cleanly` |

**Why black proves execution.** The clear colour is `0.5 + 0.5·sin(φ)` on three
channels at 120° offsets, so the three always sum to 1.5 — it is never dark.
Black therefore cannot be the clear showing through. It is `PS_Debug` returning
`MotionToColor` of a zero flow field, which is the correct output for a
spatially uniform input.

## Measurements taken at P0 close

| Quantity | Value | Where from |
|---|---|---|
| Game frame rate, `DEBUG_FLOW` 1 / 0 / 1 | 45.86 / 45.77 / 45.90 fps | ReShade Statistics, in gameplay at 1440p |
| QuantMotion on the game's runtime | 0.183 ms CPU, 0.342 ms GPU, 15 passes | same |
| Motion-vector buffer `tFlow` | 320×180 `RG16F`, **0.220 MiB** | same |
| GPU 1 present loop | 210.0 fps, vsync-locked | 600 frames per 2.857 s, two runs |
| `CrossAdapterRowMajorTextureSupported` | **0** | read on the real adapter — P1 must use shared buffers with placed footprints |
| `D3D12_FEATURE_EXISTING_HEAPS` | **1 on both adapters** | read 2026-09-03 during P1.3 — path A′ is available |

**1920×1080 is owed as a measured point.** The probe measures 1280×720 and
2560×1440; the resolution the viability argument actually rests on is 1080p, and
it is currently reached by interpolating between the other two. Interpolation is
exactly the kind of inference this project keeps catching in itself. Add it once
neural rendering is running end to end, so the point is measured against a real
workload rather than in isolation.

**No no-bridge baseline was taken, deliberately.** Every figure above was measured
with the bridge running, so the cost of the add-on's mere presence is unmeasured.
That is not an oversight to correct early: a baseline is worth taking once there
is a neural workload for it to be a baseline *of*. Taken now, against a pipeline
with no consumer, it would date before it was used. It is owed before any figure
is quoted as a result, not before the next task.

**The game's preset was contaminated during testing.** With the runtimes synced,
enabling QuantMotion wrote it into `ReShadePreset.ini`, so the game carried the
effect through the measurements above. It is constant across all three readings,
so the *deltas* hold; the absolute frame rate does not represent a clean game.

## Reproducing

1. Build via GitHub Actions at `b307367a` or any later commit on `main`. The
   artifact zip contains exactly `nvngx.dll_mgpu_bridge.addon64` and `gpu1.ini`
   — the add-on's extension replaces `.dll`, and the `nvngx.dll_` prefix is what
   satisfies the snippet's caller gate.
2. Place both beside `dxgi.dll`, together with `nvngx_dlssnr.dll`.
3. Apply the `ReShade.ini` and `ReShade2.ini` settings above **with the game
   closed** — ReShade rewrites both on exit.
4. Set `NGXCore\LogLevel` to a non-zero `REG_DWORD` so NGX writes its own log.
5. Launch. Expect exactly **one** `bridge thread spawned` line; UE5's five
   add-on probe cycles must produce none.
6. Focus the bridge window, press **Home** for the GPU 1 overlay.

**Before reading any NGX result, check `nvngx.log`'s first line.** If it is
`NGXInitValidateSnippets: installed NGX API is older than the one used by client
application`, **open the NVIDIA app and relaunch the game** — that is the fix,
reproduced 2026-09-03, and every earlier clearing event (an app update, a
reboot, a settings change) was an instance of it. Relaunching the game alone
does not reliably clear it.

**Void the NGX portion, not the launch.** The transit probe builds its own
devices and never touches NGX, so it produces valid results on a launch where
NGX init failed. An earlier version of this rule voided the whole run and would
have discarded a full set of transit samples.

`ReShade.log` is overwritten on every launch. Copy it, `nvngx.log` and
`nvngx_dlssnr_*.log` aside **together** before relaunching — they are only
interpretable as a set.
