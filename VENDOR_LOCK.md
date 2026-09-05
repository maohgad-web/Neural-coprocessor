# VENDOR_LOCK — the machine, the vendor stack, and the configuration behind every number

**What a result was taken on.** The exact hardware, driver, ReShade build,
game build and configuration under which everything in `RESULTS.md` and
`history/RECORD.md` was observed. Human-owned and human-observed: nothing here
can be derived from the repository, which is why it is a file rather than a
script.

If a later run disagrees with this file, **this file describes the run that
happened** and the disagreement is the finding.

The name is literal rather than a warning: this is the vendor stack the results
are locked to. A figure quoted without it has lost the half of itself that says
what it is a figure *of*.

**Milestones closed:** P0 2026-09-02 · P1.0–P1.5 2026-09-03/04 · P2.0/P2.1
2026-09-04 · P3.0–P3.2 2026-09-04 · P4.0–P4.2 2026-09-04/05 · P5/P6 2026-09-05 ·
P7 2026-09-05. **Publishable measurements taken 2026-09-05.**
What happened in each is in `history/RECORD.md`; what was verified is in
`history/FACTS.md`.

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
| CPU | ~~Ryzen 5 5600X~~ **Ryzen 5 5600** — corrected 2026-09-05. The X was never on this part; it was written from memory and repeated for three milestones. No number in this file depends on it, which is exactly why it survived. |
| Topology | Second GPU on **chipset** PCIe lanes, not CPU lanes, negotiating **PCIe 3.0 ×2** (~1.6 GB/s theoretical, ~700–1260 MiB/s observed depending on path and discipline). Deliberate worst case. |
| GPU 0 (game) | NVIDIA RTX 5060 Ti 16GB — vendor `0x10DE`, device `0x2D04`, 16050 MB, `outputs=1` |
| GPU 1 (target) | NVIDIA RTX 5060 Ti 16GB — same vendor/device/memory, `outputs=0` |
| Driver | **616.56** through P2; **616.64** from 2026-09-04 (see below) |
| Display | ASUS VG27AQ5A, 2560×1440 @ **210 Hz**, 8-bit RGB, SDR, VRR not supported. **A second display was added 2026-09-04**, one on each card, which is what makes the side-by-side demonstration possible and is why the bridge window has to place itself on a specific monitor rather than at the virtual-desktop origin. |
| ReBAR | ~~**Disabled per-program** for the game executable~~ **Enabled** — confirmed 2026-09-05: on in BIOS and left at the driver default in the NVIDIA control panel, which is what every P4–P7 measurement ran under. The struck-through line described a per-program override that was set during P0/P1 and is not the state any published figure was taken in. **A setting recorded once and never re-read is a perishable fact filed as an invariant.** |
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
| Displays | Two, as rig A now also has |

**Nothing has been run on rig B as of 2026-09-05.** It is listed because it
changes how rig A's numbers must be read: the link, the CPU and the display
topology are all variables the project has been treating as constants. Rig B will
be profiled when there is a neural pipeline worth profiling — which there now is,
so it is next.

**Order of operations when rig B is built, recorded before it happens so it is
not improvised:** take rig A's final logs *immediately before* pulling the NVMe,
because once the drive moves the comparison arm is gone; confirm ReBAR in BIOS on
the new board; verify per-application GPU assignment; freeze Windows and NVIDIA
driver updates before the first measurement, so the vendor stack cannot move
between the two rigs' runs. **The one claim rig B is being built to test is
whether the result survives a CPU-lane, full-width link** — every rig A figure
was taken on chipset-fed PCIe 3.0 ×2, and a result that only exists on a starved
link is a different result.

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
result depends on is `lumenite_QuantMotion.fx` at **2026.06.16**, and §09 of
`history/FACTS.md` documents that file's behaviour.

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

> **This block is correct for P0 and must never be the shipped default.**
> Annotated 2026-09-05 rather than deleted, because it is the configuration that
> made T7 pass and the record of that has to survive.
>
> `DEBUG_FLOW=1` makes QuantMotion output a motion-vector visualisation, which on
> a uniform input is black — that is *why* T7's black window proved execution. In
> a shipping build it draws a debug view over the neural output and looks like a
> bridge failure.
>
> The shipped `gpu1.ini` is **empty of techniques**:
>
> ```
> Techniques=
> TechniqueSorting=
> ```
>
> This was not a hypothetical. `assets/gpu1.ini` was copied out of a working game
> folder and carried the QuantMotion line into every CI artifact for several
> builds; ReShade's `AutoSavePreset=1` then rewrote presets at shutdown and
> re-contaminated them after each manual fix. Both are closed —
> `ReShade2.ini` ships `AutoSavePreset=0`, and `build.yml` has a step that fails
> the build if a shipped preset is non-empty — but the mechanism is the reason the
> check exists.

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

> **The P1.0 → P3.2 milestone results moved to `RECORD.md` on 2026-09-05.**
> They were in this file only because it was the document being written when
> they happened. This file is the rig, the driver, the vendor stack and the
> configuration each result was taken under; the results themselves are the
> record.

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

> **P0's task evidence and its close-of-milestone measurements also moved to
> `RECORD.md`.** Same reason.

## Reproducing

1. Build via GitHub Actions. ~~The artifact contains exactly
   `nvngx.dll_mgpu_bridge.addon64` and `gpu1.ini`~~ — **six files since
   2026-09-05**: `nvngx.dll_mgpu_bridge.addon64`, `mgpu.ini`, `gpu1.ini`,
   `ReShade2.ini`, `README.txt`, `LICENSE`. The `nvngx.dll_` prefix is what
   satisfies the snippet's caller gate.
2. Place all of them beside `dxgi.dll`, together with `nvngx_dlssnr.dll`.
3. Apply the `ReShade.ini` and `ReShade2.ini` settings above **with the game
   closed** — ReShade rewrites both on exit. The shipped `ReShade2.ini` sets
   `AutoSavePreset=0` for exactly that reason.
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
