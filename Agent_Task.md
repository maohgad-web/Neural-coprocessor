# Agent Task — current assignment

**Status: no task is assigned.**

This file always holds the current assignment and nothing else. Which milestone is
running, and where it stands, is not stated here and is not yours to infer — it
comes from the instruction you were given at the start of your session. The
filename never changes, so that instruction never changes either.

If you are a coding agent and you are reading this, you have not been given work.
**Stop here and say so.** Do not infer a task from the repository, from
`P0_RECORD.md`, from open questions in the source comments, or from anything that
looks unfinished. Everything currently in `src/` is committed, verified on
hardware, and frozen.

---

## What is done

P0 closed on 2026-09-02. A second, independent ReShade effect runtime runs on a
second physical GPU inside the game's process and executes a shader there, with
the game unaffected. Nothing crosses between the adapters yet.

- `P0_RECORD.md` — what was built, why, and every verified fact. **Reference
  only.** Sections 09 and 10 are the technical asset; the rest is history.
- `VENDOR_LOCK.md` — the exact rig, driver, ReShade build and configuration that
  made P0 pass, and the measurements taken at close.
- `README.md` — what the add-on does and how to deploy and read it.

## Carried forward for whoever writes the next brief

Two facts that must appear in the brief for cross-adapter transit, recorded here
so they are not lost between milestones. They are notes to the author of that
brief, not work:

1. **The containment check in `.github/workflows/build.yml` will fail the first
   transit commit, by design.** Its pattern rejects `SHARED_CROSS_ADAPTER`,
   `HEAP_FLAG_SHARED`, `CreateSharedHandle`, `OpenSharedHandle`,
   `FENCE_FLAG_SHARED` and `COMMAND_LIST_TYPE_COPY` — six of the ten symbols it
   greps for, and precisely the ones transit needs. (`NVSDK_NGX`, `nvngx`,
   `GetClockCalibration` and `reshade_finish_effects` are the other four and stay
   out of scope until later milestones.) The brief must name which symbols move
   from forbidden to expected, and the workflow must be edited in the same
   change. An agent that hits a red build without being told this will assume it
   broke something.

2. **`CrossAdapterRowMajorTextureSupported = 0` on this hardware.** Shared
   textures are unavailable; transit must use a shared buffer with
   `GetCopyableFootprints` and placed footprints at both ends.

## Working rules that carry forward

These were adopted during P0, each after a specific failure. `P0_RECORD.md`
section 02 has the full form.

- A green log is not a passed task. Verify acceptance against the code, not
  against the log line that would appear if the code existed.
- Check every requirement against the interface it assumes, before writing it.
- When a choice is reversible and cheap, make it and move on. When it is
  irreversible, or the brief contradicts itself, stop and report.
- Findings go in your report. This file and `P0_RECORD.md` are human-owned.
