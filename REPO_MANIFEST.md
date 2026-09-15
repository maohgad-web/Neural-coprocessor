# Repo manifest \- what ships, what stays

Rewritten 2026\-09\-15 against the tree as it actually is. It supersedes the
2026\-09\-05 pre\-publication pass, whose disposition list is resolved and recorded
at the bottom.

The point of writing it down has not changed: "obviously that should not ship"
is a judgement that has to be made once per file, deliberately, rather than
assumed.

* * *

## A \- Ships in the release artifact

**Seven files.** Anything not on this list is not in the zip, and the CI step
counts them recursively so an accidental addition fails the build.

| File | Note |
| --- | --- |
| `nvngx.dll_mgpu_bridge.addon64` | the add\-on. **The filename is load\-bearing** \- it must contain the substring `nvngx.dll`. |
| `mgpu.ini` | settings. CI enforces `Frames=0`, `Depth=1`, `MVec=3`. |
| `gpu1.ini` | **empty preset** \- no techniques, no technique sorting, no debug preprocessor definitions. CI enforces all three. |
| `ReShade2.ini` | `PresetPath=.\gpu1.ini`, `AutoSavePreset=0`. The second key is what stops preset drift. |
| `reshade-shaders/Shaders/mgpu_depth_tap.fx` | required because `Depth=1` ships. Without it ReShade never binds a depth buffer and the bridge waits instead of arming. |
| `README.txt` | install and first run, written for someone who has only the zip. |
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
| `src/` | the add\-on sources |
| `assets/` | what the artifact is built from, plus `README.txt` |
| `.github/` | `workflows/build.yml` (CI and packaging), `FUNDING.yml` |
| `tests/` | adapter selection and ini parser tests, and the script that runs them |
| `tools/` | `provenance_scan.py`, the shingle scan behind `THIRD_PARTY.md` |
| `docs/` | `0.2.0/` run logs for the five titles the README lists, `scenes/` comparison images |
| `reference/` | a sample `ReShade.ini` and a complete `ReShade.log` |
| `history/` | the working record \- see below |
| `workarounds/` | unsupported arrangements that worked here. Not part of the add\-on |

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

* * *

## Checks before tagging

1. `gpu1.ini` in `assets/` is empty of techniques \- the CI step enforces it, but look at the file.
2. `mgpu.ini` in `assets/` has `Frames=0`, `Depth=1`, `MVec=3`.
3. No `.log` file is tracked outside `docs/` and `reference/`.
4. `ext/` is not tracked.
5. Every link in every root document resolves to a file that exists \- including the ones that point into `history/` and `workarounds/`.
6. `THIRD_PARTY.md`'s pinned SHAs match the `env:` block in `build.yml`.
7. The artifact contains seven files and no more.

* * *

## Resolved from the 2026\-09\-05 pass

Recorded rather than deleted, so the reasoning is not lost.

| File | What happened |
| --- | --- |
| `github_search_code_patch.md` | deleted. Superseded working note, in git history, nothing cited it |
| `ngx_probe_patch.md` | deleted. Content is in `history/FACTS.md` section 09 and `ARCHITECTURE.md` |
| `INSTALL.txt` | resolved. The shipped install text is `assets/README.txt`; there is no `dist/` directory and no root `INSTALL.txt` |
| `AGENT_PROMPT.md` | never existed. It was carried into an earlier manifest from memory without checking the tree |
| `tools/mgpu-sysinfo.ps1` | does not exist. Listed by the 2026\-09\-05 manifest and never in the tree. `tools/` holds `provenance_scan.py` and nothing else |

* * *

## Open \- resolve before the next tag

| File | Disposition |
| --- | --- |
| `gitignore.txt` | root, 1294 bytes, alongside the real 411\-byte `.gitignore`. It is not a gitignore and nothing references it. **Delete it**, or say here what it is for |
