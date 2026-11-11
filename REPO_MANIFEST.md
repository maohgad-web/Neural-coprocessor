# Repo manifest \- what ships, what stays

Rewritten 2026\-09\-15 against the tree as it actually is. It supersedes the
2026\-09\-05 pre\-publication pass, whose disposition list is resolved and recorded
at the bottom.

The point of writing it down has not changed: "obviously that should not ship"
is a judgement that has to be made once per file, deliberately, rather than
assumed.

* * *

## A \- Ships in the release artifact

**Eight files.** Anything not on this list is not in the zip, and the CI step
counts them recursively so an accidental addition fails the build.

| File | Note |
| --- | --- |
| `nvngx.dll_mgpu_bridge.addon64` | the add\-on. **The filename is load\-bearing** \- it must contain the substring `nvngx.dll`. |
| `mgpu.ini` | settings. CI enforces `Frames=0`, `Depth=1`, `MVec=3`. |
| `gpu1.ini` | **empty preset** \- no techniques, no technique sorting, no debug preprocessor definitions. CI enforces all three. |
| `ReShade2.ini` | `PresetPath=.\gpu1.ini`, `AutoSavePreset=0`. The second key is what stops preset drift. |
| `reshade-shaders/Shaders/mgpu_depth_tap.fx` | required because `Depth=1` ships. Without it ReShade never binds a depth buffer and the bridge waits instead of arming. |
| `README.txt` | install and first run, written for someone who has only the zip. |
| `panel_simple.txt` | the layout of the simple panel (`PanelSimple`): which rows it shows, in what order, with their Chinese labels. Optional \- without it the simple panel shows nothing. In `assets/` since 0.4.0, in the artifact since the build after 0.5.1. |
| `LICENSE` |  |

`nvngx_dlssnr.dll` is **not** in the artifact and never will be. The user
supplies it, and it goes in an `mgpu` folder beside the add\-on.

* * *

## B \- Lives in the repository, does not ship

Public, but not part of the download. A reader who wants to know how this was
built, or why a decision was made, comes here.

### The root

| File | Why it stays |
| --- | --- |
| `README.md` | front door \- the result, what it does, install, controls, limits |
| `RESULTS.md` | the measurements, the method behind them, the caveats |
| `ARCHITECTURE.md` | the mechanism: adapter selection, transport, NGX, present path |
| `METHOD.md` | the working rules and what each one cost |
| `ACKNOWLEDGEMENTS.md` | prior work, AI use with named failures, third\-party credit, disclaimer |
| `THIRD_PARTY.md` | licences, pinned SHAs, provenance |
| `VENDOR_LOCK.md` | the machine, driver and configuration every number was taken on |
| `CONTRIBUTORS.md` | code contributed by others |
| `REPO_MANIFEST.md` | this file |
| `CMakeLists.txt`, `.gitignore` | the build |
| `LICENSE` |  |

**`VENDOR_LOCK.md` stays in the root**, decided 2026\-09\-05 and unchanged. It
answers "on what?", which is a question the results themselves raise \- a reader
checking a figure needs it, not just a reader reconstructing the project's path.
It is only the rig, the vendor stack and the reproduce steps; the milestone
results that had accumulated in it are in `history/RECORD.md`.

### Directories

| Directory | Contents |
| --- | --- |
| `src/` | the add\-on sources \- see the module table below |
| `assets/` | what the artifact is built from, plus `README.txt` |
| `.github/` | `workflows/build.yml` (CI and packaging of the add\-on and the launcher), `workflows/launcher.yml` and `workflows/nrcheck.yml` (the launcher and the NR check on their own), `FUNDING.yml` |
| `tests/` | adapter selection and ini parser tests, and the script that runs them; the R300 instrument tests, `check_killswitch.py` and `check_hlsl_names.py` |
| `tools/` | `launcher/` (the MGPU Bridge Launcher, its own download), `nrcheck/` (the DLSS\-NR check and bench the launcher's Diagnose runs), `latency_tools/` (`capture.bat`, `latcmp.py` and the PresentMon build they use), `flicker-probe.ps1` (contributed: a flicker measure from a desktop capture), `provenance_scan.py` (the shingle scan behind `THIRD_PARTY.md`) |
| `docs/` | `0.2.0/` and `0.4.0/` run logs for the titles the README lists, `scenes/` comparison images, `R300_T119.md` (the contributed T1.19 revision: parts, keys, tags), `CONTRIBUTED_KEYS.md` (every contributed key) |
| `reference/` | a sample `ReShade.ini`, a complete `ReShade.log`, and two example `mgpu.ini` files with Super Resolution on (`mgpuQUALITY.ini`, `mgpuPERFORMANCE.ini`) |
| `history/` | the working record \- see below |
| `workarounds/` | unsupported arrangements that worked here. Not part of the add\-on |

### `src/` \- the modules

One `.cpp`/`.hpp` pair per module. Updated 2026\-10\-09 for 0.3.1, 2026\-10\-13 for R300, and 2026\-11\-11 for 0.5.1.

| Module | What it is |
| --- | --- |
| `dllmain.cpp/.hpp` | ReShade entry: registration, the event callbacks, the overlay panel |
| `worker.cpp/.hpp` | the bridge thread: everything on GPU 1 is created, used and destroyed here |
| `gpu1_context.cpp/.hpp` | GPU 1: the private D3D12 device, the stream, ring and seals, NR / SR, the arm |
| `adapter.cpp/.hpp` | adapter enumeration and selection |
| `adapter_selection.cpp/.hpp` | the pure adapter\-choice policy (tested in `tests/`) |
| `mgpu_ini_parser.hpp` | bounded `mgpu.ini` parser primitives (header only, tested in `tests/`) |
| `diag.cpp/.hpp` | structured logging, the `[MGPU]` prefix |
| `log_queue.cpp/.hpp` | log lines leave the game's present thread through this queue |
| `probe.cpp/.hpp` | the lateral acquisition probe: depth and velocity binds |
| `calibrator.cpp/.hpp` | the NGX calibrator: the game's DLSS create and evaluate, D3D12 and D3D11 |
| `sl_probe.cpp/.hpp` | the Streamline interposer probe |
| `sl_tags.cpp/.hpp` | the Streamline tag tap |
| `screen.cpp/.hpp` | the idle and status screen (E2xx codes) |
| `nr16.cpp/.hpp` | FP16 input path (experimental, off by default) |
| `dx11_probe.cpp/.hpp` | DirectX 11: the transport\-hop probe |
| `dx11_producer.cpp/.hpp` | DirectX 11: the producer that feeds the D3D12 stream |
| `mvec_census.cpp/.hpp` | the motion\-vector census and tap (no NGX contract; D3D11 first, R253) |
| `mvec_extract.cpp/.hpp` | the virtual velocity target for D3D12 titles without an NGX contract |
| `tex_census.cpp/.hpp` | screen\-sized texture census (instrument, one key) |
| `fg_map.cpp/.hpp` | the frame\-generation vector map |
| `own_reflex.cpp/.hpp` | own Reflex for titles where the game does not run it |
| `latency_probe.cpp/.hpp` | read\-only latency probe |
| `journal.cpp/.hpp` | what the add\-on did to the game's subsystems, when and why |
| `stall_watch.cpp/.hpp` | the stall watch (`StallWatch`) |
| `discovery.cpp/.hpp` | the discovery calibrator (R&D scouting path) |
| `t119_keys.cpp/.hpp` | R300: the one place the contributed T1.19 keys are read (`docs/R300_T119.md`) |
| `temporal_diag.cpp/.hpp` | R300a: temporal record and policy (`TemporalDiag` and the policy keys) |
| `perf_diag.cpp/.hpp` | R300e: GPU 1 job timing and the prefetch experiment (`Perf11`, `PerfPrefetch`) |
| `feature_diag.cpp/.hpp` | R300f: the scene-feature silence detector (`FeatureDiag`) |
| `producer_diag.cpp/.hpp` | R300g: the producer's cost on GPU 0 (`ProducerDiag`) |
| `producer_gpu_timers.cpp/.hpp` | R300h: GPU 0 timestamps around the producer's copies (`Gpu0Timers`) |
| `live_panel.cpp/.hpp` | contributed: the panel drawn over the live neural picture (`PanelOverPicture`) |
| `panel_simple.cpp/.hpp` | contributed: the simple panel, a short view of the same controls; its layout is `assets/panel_simple.txt` (`PanelSimple`) |
| `startup_trace.cpp/.hpp` | 0.4.0: the start\-up trace and the stop record for one\-display problems (`StartupTrace`, `DcompSafeMode`) |
| `residual.cpp/.hpp` | 0.4.0, the default since 0.5.0: the residual pass \- DLSS\-NR at a reduced size, its difference added back at full size (`NRS`, `Pass2`, `ResidualPass`) |
| `gpu0_stage.cpp/.hpp` | 0.5.0: the copies to the second card leave the game's queue for a staging ring and a copy queue on GPU 0 (`StageCopy`) |
| `fg_gpu1.cpp/.hpp` | 0.5.0: DLSS Frame Generation on the second card and its pacing thread (`FrameGen=4`, experimental) |
| `chain_colour_space.cpp/.hpp` | 0.5.0, contributed: the colour space of the bridge's swap chain, following the game's (`ChainColorSpace`) |

### `history/` \- the working record

Kept in full, moved out of the way. These are the primary sources the
reader\-facing documents cite, so they are not deleted and their links must
resolve. They are a record of how the project got here, not instructions to
anyone, and a root full of milestone records reads as an unfinished workspace
rather than a product.

| File |  |
| --- | --- |
| `history/RECORD.md` | the chronological milestone record, P0 to P7 |
| `history/FACTS.md` | the verified\-facts ledger \- section 09 invariants, section 10 unexplained observations. **This is the file other documents cite.** |
| `history/P1_INSTRUMENT.md` | the instrument design, unedited, and the four instrument\-failure incidents |

`P0_RECORD.md`, `P5_P6_RECORD.md` and the unapplied P4 additions do not exist as
separate files. They were assembled \- by line\-range extraction, not rewritten \-
into `RECORD.md` and `FACTS.md`. The assembly is diffable against the originals
in git history, which is the point: a regeneration from memory could not have
been checked.

Assembling them found a real gap. **P4 had never been merged into any committed
file**, so the project's most consequential finding, that depth is not sampled
and therefore the payload is colour only, existed only in a loose patch.

`Agent_Task.md` is deleted. An earlier manifest argued for keeping it as
evidence for `ACKNOWLEDGEMENTS.md`'s statement that AI wrote most of the code.
That argument does not hold: the relay\-agent workflow covered **P0 and P1.0
only**, and everything after it was written directly. A brief for one early
milestone is not evidence for the authorship of the whole codebase, so keeping
it bought no verifiability. The disclosure stands on its own, and the brief
remains in git history.

### `workarounds/` \- unsupported arrangements

Added 2026\-09\-15. Third\-party configuration for arrangements that are not
supported and are not part of the add\-on. Nothing here is built, packaged or
tested by CI.

`workarounds/single-display/` \- running on one monitor, using Special K to keep
input reaching the game while the bridge window is on top.

| File |  |
| --- | --- |
| `README.md` | what the folder is and what it is not. GitHub renders it on the folder page |
| `Single-Display-Guide.md` | the instructions |
| `Single-Display.txt` | the Special K settings, to be pasted into `Global\default_SpecialK.ini` |

It is deliberately **not** in `assets/`. Anything in `assets/` reads as part of
the release, and this is configuration for someone else's project.

`workarounds/cost-scaler-as-nr-snippet/` \- running a third\-party DLSS\-NR proxy
at `mgpu\nvngx_dlssnr.dll`, so the NR model runs at a reduced internal
resolution while the frame that reaches the screen stays at the game's own
resolution.

| File                         |                                                                                      |
| ---------------------------- | ------------------------------------------------------------------------------------ |
| `README.md`                  | what the folder is and what it is not. GitHub renders it on the folder page          |
| `Cost-Scaler-Mount-Guide.md` | the instructions, the snippet entry point 0.3.1 requires, and what was measured here |
| `MGPU-Keys.txt`              | the `mgpu.ini` keys used for those measurements                                      |
* * *

## Checks before tagging

1. `gpu1.ini` in `assets/` is empty of techniques \- the CI step enforces it, but look at the file.
2. `mgpu.ini` in `assets/` has `Frames=0`, `Depth=1`, `MVec=3`.
3. No `.log` file is tracked outside `docs/` and `reference/`.
4. `ext/` is not tracked.
5. Every link in every root document resolves to a file that exists \- including the ones that point into `history/` and `workarounds/`.
6. `THIRD_PARTY.md`'s pinned SHAs match the `env:` block in `build.yml`.
7. The artifact contains eight files and no more.

* * *

## Resolved from the 2026\-09\-05 pass

Recorded rather than deleted, so the reasoning is not lost.

| File | What happened |
| --- | --- |
| `github_search_code_patch.md` | deleted. Superseded working note, in git history, nothing cited it |
| `ngx_probe_patch.md` | deleted. Content is in `history/FACTS.md` section 09 and `ARCHITECTURE.md` |
| `INSTALL.txt` | resolved. The shipped install text is `assets/README.txt`; there is no `dist/` directory and no root `INSTALL.txt` |
| `AGENT_PROMPT.md` | never existed. It was carried into an earlier manifest from memory without checking the tree |
| `tools/mgpu-sysinfo.ps1` | does not exist. Listed by the 2026\-09\-05 manifest and never in the tree. (What `tools/` holds today is in the Directories table.) |
| `gitignore.txt` | deleted. It was not a gitignore and nothing referenced it |

* * *

## Open \- resolve before the next tag

Nothing open as of 2026\-11\-11.
