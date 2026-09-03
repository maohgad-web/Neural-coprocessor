# MGPU Bridge — DLSS Neural Rendering on a second GPU

Private R&D. Not redistributed. See `LICENSE` — all rights reserved.

A ReShade add-on that stands up a **private D3D12 device on the second GPU** of a
running game's process, and runs work there while the game renders on the first
GPU. The question it exists to answer:

> **Can DLSS Neural Rendering be decoupled from the render device and executed on
> a second GPU acting as a neural co-processor?**

**This is not SLI.** SLI splits rendering mid-graph, which is why it needed driver
and engine cooperation and why it died. DLSS-NR is **terminal** — it consumes a
finished colour buffer and produces a finished colour buffer. A terminal stage has
no graph to split. That is the entire architectural claim, and it is why this is
worth trying on hardware SLI could never have used.

**Nothing NVIDIA ships is modified.** No binary patching, no import-table
rewriting, no signature work. The add-on calls documented NGX entry points and is
named so it satisfies NVIDIA's own caller check honestly — see *The rename*
below. Legitimacy is a project goal, not a constraint reluctantly observed.

---

## Where the project actually is

| Milestone | Question | State |
|---|---|---|
| **P0** | Can a second ReShade runtime exist and execute on a second adapter? | **PASSED** 2026-09-02 |
| **P1.0** | Will NGX create a DLSS-NR feature on a headless, non-render adapter? | **PASSED** 2026-09-03 |
| **P1.1** | Does DLSS-NR actually *execute* there? | **PASSED** 2026-09-03 |
| **P1.2** | Are its tuning parameters live per evaluate, or baked at create? | **PASSED** 2026-09-03 |
| **P1.3** | Can a buffer cross between the two adapters? | **OPEN** — see below |
| **P1.4** | Return path, and NR inside the frame loop | not started |
| **P2** | Ring buffer, pipelining, real timing | not started |

### What is established

- **NGX runs on a non-render device.** `CreateFeature(NVSDK_NGX_Feature_Reserved18)`
  returns Success on the headless adapter — real network, real weights, real
  kernels, 388.8 MB resident on GPU 1, clean teardown.
- **DLSS-NR executes there.** 914,752 of 921,600 pixels changed, sentinel
  survivors zero, bit-identical across builds. NVIDIA's own snippet log
  corroborates it pointer-for-pointer.
- **Its parameters are live per evaluate.** Intensity 0.00 vs 1.60 changes 98.03%
  of pixels; the same-intensity control is byte-identical. Quality is tunable at
  runtime with no ~220 ms feature rebuild. Reproduced three times, across a
  rebuild, a reboot and a LUID reassignment.

### What is not

- **No byte has crossed between the two adapters.** P1.3 is open.
- **No transit latency or bandwidth figure exists.** Every number of that kind in
  the documents is theoretical and labelled as such.
- **No performance claim of any kind is supported.** All probe timings are taken
  during game startup, in a menu, while shaders compile. `P0_RECORD.md` section 10
  explains why that is structural and not fixable by care.
- **No no-bridge baseline exists.** The cost of the add-on's mere presence is
  unmeasured, deliberately — a baseline is worth taking once there is a neural
  workload for it to be a baseline *of*.

---

## The documents, and which one to read

| File | What it is | Who owns it |
|---|---|---|
| `README.md` | this — orientation and operation | maintained with the code |
| `P0_RECORD.md` | **frozen** record of P0. Sections 09 (verified API facts) and 10 (open observations) are the standing technical reference for every later milestone. | human-owned; do not extend |
| `VENDOR_LOCK.md` | the exact rig, driver, pins and configuration, plus every P1 result as observed | human-owned |
| `P1_INSTRUMENT.md` | how transit will prove itself — the seal, the negative control, and the catalogue of failures that look like success | human-owned |
| `THIRD_PARTY.md` | licensing of the ReShade and NGX headers, and LumeniteFX's status as an external runtime asset | human-owned |

**`P0_RECORD.md` says its assignment lives in `Agent_Task.md`. That file no longer
exists.** The coding-agent workflow was abandoned during P1 in favour of direct
work; the reference is historical, like the section numbers that document already
warns about.

**If you are a coding agent:** none of these documents is a task list. Read
`P0_RECORD.md` sections 09 and 10 for verified facts. Do not infer work from a
record of finished work.

---

## What the add-on does

1. **Registers with stock ReShade** (add-on API 20). Nothing about ReShade is
   patched or rebuilt.

2. **Selects the adapter the game is not using**, by LUID exclusion — never by
   index, because enumeration order is not stable and LUIDs are reassigned across
   driver restarts and reboots.

   The game's LUID comes from the **swapchain**, not the first device: UE5 probes
   every adapter before settling, and one run captured a software adapter as the
   "game" LUID. Software adapters are excluded by `DXGI_ADAPTER_FLAG_SOFTWARE`
   **or** vendor ID `0x1414`, because the flag alone is unreliable — this rig
   enumerates two "Microsoft Basic Render Driver" adapters and only one sets it.

   If filtering does not leave exactly one candidate the add-on **selects
   nothing**. A missing device is diagnosable; a device on the wrong adapter
   succeeds, logs cleanly, and proves nothing.

3. **Spawns the bridge thread** — every GPU 1 object is created, used and
   destroyed on it — and creates the private D3D12 device, re-verifying the LUID
   in both directions. There is no fallback to the game's adapter, by design.

   The thread starts **only** from `init_swapchain`. Starting it on device events
   spawned one thread per UE5 probe cycle, each able to be executing inside the
   module when ReShade unmapped it, which killed the game process intermittently.

4. **Creates a 1280×720 window** on that thread and pumps its messages there. The
   size keeps an optical-flow pyramid's coarsest level non-degenerate.

5. **Presents on GPU 1**, vsync-paced. ReShade then attaches its own effect
   runtime to that swapchain, and `gpu1.ini` decides what runs there.

6. **Runs the NGX probe** on the GPU 1 device: core `Init` → core
   `GetCapabilityParameters` → snippet `Init_Ext` → snippet
   `PopulateParameters_Impl` → `CreateFeature(Reserved18)` → evaluate → release.

7. **Runs the transit probe** — its own devices on both adapters, so the game's
   device and the bridge's device are never borrowed.

The game's ReShade runtime on GPU 0 is never touched.

---

## Two things that are load-bearing and non-obvious

### The core/snippet split

NGX is two modules and they are not interchangeable:

- **`_nvngx.dll`** — the driver core, in the DriverStore. Owns the **parameter
  block**.
- **`nvngx_dlssnr.dll`** — the feature snippet, in the game directory. Owns the
  **feature and its own session**.

Resolving an entry point from whichever module answers first produces working
code that fails later, with a different error each time you get closer. The
result-code ladder is in `P0_RECORD.md` section 09; the short version:

| Code | Means |
|---|---|
| `0xBAD0000B` | called `CreateFeature` on the **core** — it has no snippet mapping |
| `0xBAD00007` | initialised the core but not the **snippet's own session** |
| `0xBAD00005` | set generic `Width`/`Height` instead of `DLSSNR.Width`/`DLSSNR.Height` |
| `0xBAD00002` | caller gate — see below |
| `0xBAD0000C` | environment/version gate — the run is **void**, see *Before trusting a run* |

The feature reads its **own namespace**. `DLSSNR.Width`, not `Width`. Subrect keys
carry no separator: `DLSSNR.ColorSubrectWidth`.

### The rename

`nvngx_dlssnr.dll` resolves the module owning its caller's return address and
requires that module's file path to contain the literal substring `nvngx.dll`.
A caller that fails the test gets `0xBAD00002 FAIL_PlatformError`.

So CI ships the add-on as **`nvngx.dll_mgpu_bridge.addon64`**. The add-on *is* the
module that calls the snippet, so its own name is what has to satisfy the check.

This is done in `build.yml`'s staging step and only there — `CMakeLists.txt` stays
closed, and the "Verify artifact name" step keeps checking the name CMake actually
produces. **Do not shorten it to a bare `nvngx.dll`**: a directory or executable
with that name breaks process startup, and it must keep the `.addon64` extension
or ReShade will not load it.

---

## Build

**GitHub Actions only** — `.github/workflows/build.yml`. There is no local build.

The workflow, in order: containment check → fetch ReShade `include/` at the pinned
SHA → verify `RESHADE_API_VERSION 20` → fetch NVIDIA NGX `include/` at the pinned
SHA → verify `NVSDK_NGX_Feature_Reserved18` exists → configure → build → verify the
artifact name → stage the deploy set under the renamed filename.

`CMakeLists.txt` is **closed**: link libraries are `dxgi` and `d3d12`, and nothing
is added to it. Win32 APIs outside that set — the registry, for instance — are
reached through `LoadLibraryW` + `GetProcAddress`, the same way NGX exports are.

### The containment guard

A grep over `src/` that **fails the build** if a symbol belonging to a future
milestone appears. It has sequenced the work all along:

```
moved by P1.0   NVSDK_NGX   nvngx
moved by P1.3   SHARED_CROSS_ADAPTER   HEAP_FLAG_SHARED
                CreateSharedHandle     OpenSharedHandle
still guarded   reshade_finish_effects        -> P1.4
                FENCE_FLAG_SHARED             -> P2
                COMMAND_LIST_TYPE_COPY        -> P2
                GetClockCalibration           -> P2
```

**It matches text, not code, and it is right to.** A comment mentioning a guarded
symbol fails the build; the fix is to reword the comment. **Widening the pattern
to make a build pass deletes the guard rather than moving it** — a symbol moves
out in the same commit that first legitimately uses it, and not before.

---

## Deploy

Everything goes in `<game>\Carnal_Instinct_UE5\Binaries\Win64\` — the directory
containing `dxgi.dll`.

1. An **add-on-enabled ReShade build** at the release pinned in `VENDOR_LOCK.md`.
   Confirm a trivial add-on loads first; this one will not load under an
   add-on-disabled build, and that failure looks identical to a broken add-on.
2. **Both files from the CI artifact zip** — `nvngx.dll_mgpu_bridge.addon64` and
   `gpu1.ini` — beside `dxgi.dll`.
3. **`nvngx_dlssnr.dll`** beside `dxgi.dll`. NGX loads it itself; nothing calls
   into it directly. `nvngx_dlss.dll` and `sl.interposer.dll` are **not** required
   — DLSS super-resolution and Streamline are not on the NR path.
4. **LumeniteFX** in `…\reshade-shaders\Shaders\`. `VENDOR_LOCK.md` explains how
   to identify the right pack: the shaders are dated individually and there is no
   single pack version number.
5. Apply the `ReShade.ini` and `ReShade2.ini` settings from `VENDOR_LOCK.md`
   **with the game closed** — ReShade rewrites both on exit, so edits made while
   it is running are lost.
6. Launch. The bridge window appears; press **Home** with it focused for the
   GPU 1 overlay.

**Turn on NGX's own log.** Set
`HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore\LogLevel` to a non-zero
`REG_DWORD`. Logs land in `%PROGRAMDATA%\NVIDIA\NGX\Logs`. This is a driver-side
witness independent of anything this project writes, and it is what cracked P1.0.
It is **not** reachable through an `NVSDK_NGX_FeatureCommonInfo` logging callback
— that was installed at VERBOSE and produced nothing.

---

## Before trusting a run

**Read the first line of `nvngx.log`.** If it is

```
NGXInitValidateSnippets: installed NGX API is older than the one used by client application
```

with no telemetry blocks after it, **the run is void whatever `ReShade.log` says**
and must not be recorded as a result. A healthy run opens with four telemetry
blocks: SuperSampling, FrameGeneration, DeepDVC, SuperSamplingDenoising.

Two clearing events are on record — relaunching the NVIDIA app (which had a
pending update), and a full system restart. Neither is a rule; `n` is small and
the file versions never moved across the outage, so this was never a version
mismatch. **Do not bisect `.ini` files and do not suspect the add-on** — the same
binary passed on either side of it.

**Record the telemetry block's versions with any quoted result.** The NGX stack
drifts under a running experiment: `OTAEnabled = 1`, `nvngx_update.exe` runs at
init, and `StreamlineVersion` was observed moving `2,12,129,0` → `2,14,0,0` on an
untouched game. "Same rig, same driver" does not mean same NGX stack.

---

## Reading the log

Every line carries an `[MGPU]` prefix, so a run is one grep:

```
grep "\[MGPU\]" ReShade.log
```

Prefixes are `[MGPU][T1]`…`[MGPU][T8]` for P0's bring-up and `[MGPU][P1.x]` for
milestone work, so the two are separable in archaeology.

**The add-on logs the inputs to each decision, not the verdict.** The full adapter
table, both LUIDs at the comparison, the HRESULT of every call, and — since P1.3 —
every descriptor field as a number before the call that consumes it. A verdict
cannot be re-examined after the fact; numbers can. This is the single most
valuable habit in the project and it has caught two of its own false results.

### Expected shape of a healthy run

UE5 probes each adapter in turn and ReShade loads and unloads the add-on once per
probe — **five cycles per launch on this rig**. Those cycles log `[MGPU][T1]` and
the adapter table and nothing else. Exactly **one** `bridge thread spawned` line
should appear in the whole log, at the real swapchain.

| What you see | What it means |
|---|---|
| `T2 SELECTED adapter[n] … rule="exclusion …"` | Selection worked; that LUID is the target card. |
| `T2 REFUSING: …` | No unique non-game hardware adapter. Not a crash — the add-on declines to guess. |
| `T3 D3D12CreateDevice hr=0x00000000` | The GPU 1 device is live. |
| `T3 MISMATCH` / `T3 FATAL` | Bound to the wrong adapter. Releases and stops, by design. |
| `T4 window created: … client=1280x720` | Any other client rect means DPI scaling interfered. |
| `T5 present loop alive: frame N` | Every 600 frames. A gap in the sequence is more interesting than its absence. |
| `Recreated runtime environment on … ReShade2.ini` | ReShade attached its own effect runtime to our GPU 1 swapchain. |
| `P1.0c CreateFeature(Reserved18) … Success` | DLSS-NR exists on GPU 1. |
| `P1.2 PROBE PASSED - PARAMETERS ARE LIVE` | Verdict is a byte comparison, unaffected by focus or shader compilation. |
| `P1.3 A.0 desc echo: …` | The exact descriptor submitted, as numbers. Read this before reading any `A.n` failure. |
| `P1.3 A.1 verdict: … eligible=NO` | **A downstream `A.3` result is meaningless.** The winning variant could not cross an adapter. |
| No teardown lines, clean `Finished exiting` | Normal at process exit. **Quitting the game does not exercise teardown — only closing the bridge window does.** |
| Log ends mid-cycle, game gone | A crash inside an add-on unload. This was the `init_device` thread-spawn bug; if it returns, the fix regressed. |

**`ReShade.log` is overwritten on every launch.** Copy it, `nvngx.log` and
`nvngx_dlssnr_*.log` aside together before relaunching — they are only
interpretable as a set, and their timestamps are how you tell a stale one from a
current one.

---

## Working rules that earned their place

These are not style preferences. Each of them exists because its absence cost a
milestone, a launch, or a false result that had to be retracted.

- **A green log is not a passed task.** State the verdict as a comparison against
  a control, not as an observation that something happened.
- **Pre-fill outputs with a sentinel.** "It differs from the input" is also
  satisfied by an uninitialised buffer. P1.1's verdict is only trustworthy because
  `still_sentinel=0` was checked.
- **Run a control at the same setting.** P1.2's A-vs-C byte-identical control is
  what makes A-vs-B attributable to intensity rather than to evaluate order.
- **Split every HRESULT.** One code covering several calls makes "our parameter
  mistake" and "the hardware refused" indistinguishable. P1.3 lost two runs to a
  single collapsed HRESULT.
- **When a call is refused, vary one field per row** rather than reasoning about
  which field is wrong. The difference between two adjacent rows names the cause.
  Three sessions of inference produced nothing; one matrix produced an isolation.
- **A fallback that changes what is being measured is not a fallback.** A P1.3
  probe took the first variant that returned `S_OK`, and that variant could never
  have satisfied the call it was carried into — producing a precise, correctly
  reported, wrong answer. The verdict line now prints whether the winner was
  eligible at all.
- **Do not enable the D3D12 debug layer from here.** Called after devices exist in
  the process, it reset every device including one already running. The layer is
  documented as something enabled before any device exists.
- **Retract in place, struck through, with the reason.** `VENDOR_LOCK.md` keeps a
  wrong header inference visible because the design built on it survived being
  wrong, and that is the useful part.

---

## Files

`ext/reshade/` and `ext/ngx/` are created by CI at build time, are gitignored, and
are never committed. `THIRD_PARTY.md` records the licensing.
