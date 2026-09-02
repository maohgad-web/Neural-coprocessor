# Agent Task — current assignment

## P1.0 — Does NGX initialise on the second GPU?

**This is the whole task.** No transit, no game data, no inference, no image. Call
NGX against the GPU 1 device that already exists, read the return codes, log them.
It is deliberately small: everything else in this milestone depends on the answer,
and nothing else should be built until it is known.

If NGX will not initialise on a headless, non-game adapter, the entire
cross-adapter pipeline has no consumer and the project changes shape. That is why
this comes first.

**You cannot compile this and you cannot run it.** Builds happen in GitHub
Actions, which you cannot see; the program needs two physical GPUs and a specific
game, which a human has and you do not. Nothing in section 5 is verified by
running anything — every acceptance item is checked by reading the code that
implements it. Write as though the first person to see your code execute will be
someone holding your test instructions and nothing else, because that is exactly
what happens.

Two consequences worth stating plainly:

- **The CI step in 3.1 is the riskiest thing in this task**, because it is the
  only part whose failure you cannot reason about locally — a bad URL, a wrong
  path after extraction or a missing file surfaces only as a red build somebody
  else has to read. Model it line by line on the ReShade fetch that already works
  in that file, and change as little as the difference demands.
- **The log lines in section 4 are a specification, not a result.** The timings in
  them come from a reference run on the *other* GPU. Do not treat them as
  something to match, and do not calibrate anything against them.

---

## 1 · What already exists

P0 built and froze all of this. Do not modify it except where section 3 says so.

`src/gpu1_context.cpp` owns a private `ID3D12Device` on the second GPU, a
`ID3D12CommandQueue` (DIRECT), a command allocator, a command list, a fence and a
swapchain. They are created on the **bridge thread** and destroyed there.

**Every one of those pointers is private to that translation unit, behind a
mutex.** `P0_RECORD.md` section 01 lists that as an invariant, and the comment at
`has_present_chain()` records that the accessor to reach them "is written with T6,
not now" — T6 was satisfied without construction, so **that accessor was never
written**. The only things `gpu1_context` exposes today are `has_device`,
`has_present_chain` and `device_removed_reason`.

**This task does not add a getter.** A getter that hands out the raw device
breaks the invariant that stops a released pointer being used after teardown. The
probe runs *inside* `gpu1_context.cpp`, where the pointers already are.

## 2 · What the reference implementation does

A working single-GPU DLSS-NR run on this same rig produced the sequence below.
This is not a design; it is an observation from a real log, and it is the contract
to imitate.

```
[RenoDX] DLSS-NR direct: using force-loaded NVIDIA parameter provider
         C:\WINDOWS\system32\DriverStore\FileRepository\nv_dispi.inf_amd64_<hash>\_nvngx.dll
[RenoDX] DLSS-NR direct: attached snippet <game>\Binaries\Win64\nvngx_dlssnr.dll
[RenoDX] DLSS-NR direct: backend-owned NVIDIA NGX core initialization succeeded
[RenoDX] DLSS-NR direct: Init_Ext succeeded for device 0x1d1f6a2ffe0
[RenoDX] DLSS-NR direct: created private output 1: size=2560x1440 format=24 flags=0x5
[RenoDX] DLSS-NR direct: CreateFeature(Reserved18) succeeded: handle=... performance=6 preset=1
[RenoDX] DLSS-NR direct: EvaluateFeature succeeded: evaluation=1 options_revision=2 result=0x00000001
```

Facts to take from it, all observed rather than assumed:

- **`_nvngx.dll` is the parameter provider**, loaded from the driver's DriverStore
  directory. `nvngx_dlssnr.dll` sits beside `dxgi.dll` in the game folder and is
  loaded by NGX itself — we never call into it.
- **`Reserved18` is the DLSS-NR feature id**, and it is a published enum name.
- **`CreateFeature` took 1.16 seconds** in that run (`00:21:32:383` → `00:21:33:541`).
  Budget for it; see section 4.
- **NGX ran with no ReShade effects loaded at all** — the shader search path failed
  to resolve and `ReShadePreset.ini` was empty. NGX needs a device, a queue, a
  command list, a fence and resources. It does **not** need an effect runtime. GPU
  1 already has all five.
- The reference add-on is configured with `DirectNeuralRenderingForceNgxCore=1`.
  Without it the same log reports `NVNGX parameter module not found: nvngx.dll`.

## 3 · What to build

### 3.1 · NGX headers, fetched by CI

Add a step to `.github/workflows/build.yml`, modelled exactly on the existing
ReShade fetch, that downloads `NVIDIA/DLSS` at a **pinned SHA** and places the
contents of its `include/` directory at `ext/ngx/`. Pin the SHA in the workflow's
`env:` block beside `RESHADE_SHA`, and record it in your report so it can be
copied into `VENDOR_LOCK.md`.

- Repo: `NVIDIA/DLSS` · branch `main` · path `include/`
- Needed: `nvsdk_ngx.h`, `nvsdk_ngx_defs.h`, `nvsdk_ngx_params.h`,
  `nvsdk_ngx_d3d12.h`. Fetch the whole `include/` directory rather than
  cherry-picking files; read the headers to confirm which of them include which.
- Add `ext/ngx/` to `.gitignore` alongside `ext/reshade/`. **These headers are
  never committed.**
- Add a verification step, like the existing `RESHADE_API_VERSION 20` check, that
  greps the fetched tree for the `Reserved18` enum name and fails the build if it
  is absent. A silently-changed header is worse than a missing one.
- Add a `## NVIDIA NGX headers` section to `THIRD_PARTY.md` recording the repo,
  the pinned SHA, that the headers are fetched at build time and never
  redistributed, and the licence the repository states. **Read that licence and
  quote it accurately — do not summarise it from memory.**

**Headers only. No new link library.** `P0_RECORD.md` section 09 forbids adding
one, and that rule stands. Every NGX entry point is reached through
`GetProcAddress`; see 3.2.

### 3.2 · Reaching the NGX entry points

The NGX SDK normally ships a static library that loads `_nvngx.dll` for you. We
cannot link it, so load the module and resolve the entry points by hand. Attempt
in this order, logging which one succeeded:

1. `GetModuleHandleW(L"_nvngx.dll")` — if a DLSS add-on is present in the process
   it has already force-loaded the module, and this costs nothing.
2. `LoadLibraryW(L"_nvngx.dll")` — the loader may resolve it from the driver's
   path.
3. If both fail, **stop and report**. Do not attempt to locate the DriverStore
   directory by enumerating the filesystem or reading the registry: the reference
   implementation's method for finding it is not recorded, and guessing a path
   layout is exactly the class of assumption that has cost this project rig
   cycles. A clean `NGX module not found` is a useful result — it tells us the
   probe needs a locator, which is then its own small task.

Resolve at minimum `NVSDK_NGX_D3D12_Init_Ext`,
`NVSDK_NGX_D3D12_GetCapabilityParameters` (or the current equivalent — **read the
header**), `NVSDK_NGX_D3D12_CreateFeature`, `NVSDK_NGX_D3D12_ReleaseFeature` and
`NVSDK_NGX_D3D12_Shutdown1`. Log each name with whether `GetProcAddress` returned
non-null, **before** calling any of them. A missing export must be reported by
name, not discovered as a crash.

### 3.3 · The probe

Add `bool ngx_probe()` to `src/gpu1_context.cpp`, declared in its header. It runs
**on the bridge thread**, exactly once, **after `create_present_chain()` succeeds
and before the present loop starts**. Nothing is presenting yet at that point, so
the 1.16-second `CreateFeature` stalls nothing.

Sequence, each step logged with its return code before the next is attempted:

1. Resolve the module and entry points (3.2).
2. `NVSDK_NGX_D3D12_Init_Ext` against the GPU 1 device.
3. Obtain the capability parameters.
4. `CreateFeature` with `Reserved18` at **1280×720** — the bridge window's size,
   not the game's 2560×1440. This probe is about whether the adapter accepts the
   feature at all; use what GPU 1 actually has.
5. Immediately `ReleaseFeature` and `Shutdown1`. **The probe leaves nothing
   behind.** It answers a question and tears down.

**Every step must be able to fail without taking the process with it.** If any
call fails, log the code, skip the remaining steps, tear down what did succeed,
and return false. `ngx_probe()` returning false must not stop the bridge — the
window and present loop carry on exactly as they do today, and P0's behaviour is
unchanged. This is a probe, not a dependency.

Do not call `EvaluateFeature`. There is nothing to evaluate — no input resources
exist — and its success or failure would tell us nothing that `CreateFeature`
has not already answered.

### 3.4 · Containment

`.github/workflows/build.yml` rejects `NVSDK_NGX` and `nvngx`. **Move exactly
those two out of the pattern, in the same commit that first needs them.** Leave
the other eight — `SHARED_CROSS_ADAPTER`, `HEAP_FLAG_SHARED`, `CreateSharedHandle`,
`OpenSharedHandle`, `FENCE_FLAG_SHARED`, `COMMAND_LIST_TYPE_COPY`,
`GetClockCalibration`, `reshade_finish_effects` — exactly as they are. This task
transits nothing, so none of them are needed.

The workflow's job name, step name and error text still say "P0". Update them to
say P1 in the same commit, or the build will refuse code while naming the wrong
milestone.

## 4 · Log lines

Prefix everything `[MGPU][P1.0]`. **Do not reuse `[MGPU][T1]`…`[T8]`** — those are
P0's and the archived logs must stay greppable.

Log inputs, not verdicts. Expected shape of a successful run:

```
[MGPU][P1.0] ngx module: GetModuleHandleW(_nvngx.dll) -> 0x00007ffd... (already resident)
[MGPU][P1.0] ngx exports: Init_Ext=ok GetCapabilityParameters=ok CreateFeature=ok ReleaseFeature=ok Shutdown1=ok
[MGPU][P1.0] Init_Ext: result=0x00000001 device=<gpu1 device ptr> luid=00000000-0001382B
[MGPU][P1.0] capability params: result=0x00000001
[MGPU][P1.0] CreateFeature(Reserved18): result=0x00000001 handle=0x... 1280x720 elapsed=1163ms
[MGPU][P1.0] ReleaseFeature: result=0x00000001
[MGPU][P1.0] Shutdown1: result=0x00000001
[MGPU][P1.0] PROBE PASSED — NGX initialises and creates a feature on the non-game adapter
```

And of a failing one — **a failure here is a valid result, not a defect**:

```
[MGPU][P1.0] Init_Ext: result=0xBAD00006 device=... luid=...
[MGPU][P1.0] PROBE FAILED at Init_Ext — see result code above. Bridge continues.
```

Log the **numeric** result of every NGX call, and its symbolic name if the header
defines one you can map. A raw code we can look up later is worth more than a
message that interprets it wrongly. Print the LUID alongside, so the log proves
which adapter was asked.

Time `CreateFeature` and log the elapsed milliseconds. It is the one number this
task produces that later work will need, and it costs a `QueryPerformanceCounter`
pair.

## 5 · Acceptance

1. `ngx_probe()` exists, runs once on the bridge thread after the present chain is
   created, and is called from exactly one place.
2. Every NGX call's numeric result reaches the log before the next call is made.
3. Every failure path releases what it created and returns false.
4. A false return leaves the bridge running: window, present loop and teardown
   behave exactly as P0 shipped them.
5. Nothing is left initialised when the probe returns — no feature handle, no NGX
   session.
6. `ext/ngx/` is gitignored and not committed. No new link library.
7. Only `NVSDK_NGX` and `nvngx` left the containment pattern.

**Check each of these against the code that implements it, not against the log
line that would appear if it did.** A probe that was never called and a probe
that found nothing produce identical logs — this project has already shipped
exactly that once.

## 6 · Your report must include

The usual, plus specifically:

- The pinned `NVIDIA/DLSS` SHA, for `VENDOR_LOCK.md`.
- The exact entry-point names you resolved, **as the header spells them** — if
  `GetCapabilityParameters` is named something else in the pinned version, say so.
- The licence `NVIDIA/DLSS` states, quoted.
- Step-by-step test instructions: what to deploy, what to launch, what to grep
  for, and what each of the two outcomes above looks like. Say explicitly that a
  `PROBE FAILED` line is a successful test run, so nobody reports it as a bug.

---

# Reference — read before writing code

- `P0_RECORD.md` — the finished previous milestone. **Reference only.**
  **Section 09** is the verified-facts ledger and opens with the table of
  repositories, branches and paths where you verify anything it does not cover.
  You have no web search; that table is how you work. **Section 10** is unexplained
  observations — check it before concluding you caused something.
- `VENDOR_LOCK.md` — the rig, driver, ReShade build and configuration.
- `P1_INSTRUMENT.md` — a design, not verified fact, covering how cross-adapter
  *transit* proves itself. **It does not apply to this task.** This task moves no
  bytes across the bus. Read it as background if you like; do not build anything
  from it, and do not treat it as a reason to add work you were not given.
- `README.md` — yours to update, and it must describe what the code actually does
  when you are finished.

## Working rules

- **A green log is not a passed task.** Verify acceptance against the code, not
  against the log line that would appear if the code existed.
- **Check every requirement against the interface it assumes, before writing it.**
  Read the header. A signature you recall is not a signature you verified.
- **When a choice is reversible and cheap, make it and move on**, and say what you
  chose. When it is irreversible, or this brief contradicts itself or a verified
  fact, stop and report.
- Findings go in your report. `P0_RECORD.md`, `VENDOR_LOCK.md`, `P1_INSTRUMENT.md`,
  `THIRD_PARTY.md` and this file are human-owned — except the one `THIRD_PARTY.md`
  section 3.1 asks you to add.
