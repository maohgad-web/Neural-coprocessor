# Repo manifest — what ships, what stays, what goes

Working document for the pre-publication pass, 2026-09-05. Three columns, and
the point of writing it down is that "obviously that shouldn't ship" is a
judgement that has to be made once per file, deliberately, rather than assumed.

---

## A · Ships in the release artifact

The six files the CI artifact contains. Anything not on this list is not in the
zip.

| File | Note |
|---|---|
| `nvngx.dll_mgpu_bridge.addon64` | the add-on. **The filename is load-bearing** — it must contain the substring `nvngx.dll`. |
| `mgpu.ini` | settings. Shipped default: `Frames=0` (unbounded), `Passes=1`, `Intensity=1.00`, `Window=fit`. |
| `gpu1.ini` | **empty preset** — `Techniques=` and `TechniqueSorting=`, nothing else. CI fails the build if it is non-empty. |
| `ReShade2.ini` | `PresetPath=.\gpu1.ini`, `AutoSavePreset=0`. The second key is what stops preset drift. |
| `README.txt` | install and first-run, written for someone who has only the zip. |
| `LICENSE` | |

---

## B · Lives in the repository, does not ship

Public, but not part of the download. A reader who wants to know how this was
built or why a decision was made comes here.

| File | Why it stays |
|---|---|
### The root — what someone sees on landing

Seven files and three directories. Everything here is addressed to a reader.

| File | Why it stays |
|---|---|
| `README.md` | front door — the result, what it does, install, controls, limits |
| `RESULTS.md` | the measurements, the method behind them, the caveats |
| `ARCHITECTURE.md` | the mechanism: adapter selection, transport, NGX, present path |
| `METHOD.md` | the working rules and what each one cost |
| `ACKNOWLEDGEMENTS.md` | prior work, AI use with named failures, disclaimer |
| `THIRD_PARTY.md` | licences, pinned SHAs, provenance |
| `LICENSE` | |
| `src/`, `CMakeLists.txt`, `.github/`, `assets/`, `.gitignore` | the build |
| `tools/provenance_scan.py` | the shingle scan behind `THIRD_PARTY.md`'s provenance section |
| `tools/mgpu-sysinfo.ps1` | machine description; how `RESULTS.md`'s machine table was produced |

### `history/` — the working record

Kept in full, moved out of the way. These are the primary sources the
reader-facing documents cite, so they are not deleted and their links must
resolve — but they are a record of how the project got here, not instructions to
anyone, and a root full of milestone records and task briefs reads as an
unfinished workspace rather than a product.

| File | |
|---|---|
| `history/P0_RECORD.md` | the verified-facts ledger. §09 invariants, §10 unexplained observations. |
| `history/P1_INSTRUMENT.md` | the instrument design and the four instrument-failure incidents |
| `history/P5_P6_RECORD.md` | **not currently committed — commit it, into `history/`.** |
| `history/VENDOR_LOCK.md` | the rig, the driver, the configuration each result was taken under |

**`Agent_Task.md` is deleted, and the reasoning matters more than the file.** An
earlier version of this manifest argued for keeping it as evidence for
`ACKNOWLEDGEMENTS.md`'s statement that AI wrote most of the code. That argument
does not hold: the relay-agent workflow covered **P0 and P1.0 only**, and
everything after it was written directly. A brief for one early milestone is not
evidence for the authorship of the whole codebase, so keeping it bought no
verifiability — it only put a document in the repository that reads as
scaffolding rather than as a product.

The disclosure stands on its own in `ACKNOWLEDGEMENTS.md`, and the brief remains
in git history for anyone who wants it.

**`AGENT_PROMPT.md` does not exist** and was carried into an earlier version of
this list from memory without checking the tree. Removed.

**Adding a `history/` prefix breaks every cross-reference into these four
files.** Fix the links in the same commit as the move, and re-run check 5 below
afterwards — a moved file with a stale link is worse than a file in the wrong
place.

---

## C · Stale — resolve before publishing

| File | Disposition |
|---|---|
| `github_search_code_patch.md` | superseded working note. **Delete** — it is in git history either way, and nothing cites it. |
| `ngx_probe_patch.md` | same. Its content is in `P0_RECORD.md` §09 and `ARCHITECTURE.md`. |
| `INSTALL.txt` | **`README.md` links it and it does not exist as a repo file** — `dist/README.txt` is the shipped one. Either commit it at the repo root or point the README link at `dist/README.txt`. Do not leave the link broken. |

---

## Checks before tagging

1. `gpu1.ini` in `assets/` is empty of techniques — the CI step enforces it, but
   look at the file.
2. `mgpu.ini` in `assets/` has `Frames=0`.
3. No `.log` file is tracked. `git ls-files | grep -i log` returns nothing.
4. `ext/` is not tracked.
5. Every link in every root document resolves to a file that exists — including
   the ones that now point into `history/`.
6. `THIRD_PARTY.md`'s pinned SHAs match the `env:` block in `build.yml`.
7. The artifact contains six files and no more.
