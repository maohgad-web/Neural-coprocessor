# Reference configuration and logs

What a working setup looks like, so you can diff yours against it.

**Nothing in this folder is meant to be dropped into a game directory.** The
files you install come from the release download. These are references: read
them, compare them against what you have, and copy only what you need.

| file | what it is |
|---|---|
| `ReShade.ini` | the GAME runtime's config, machine-specific and cosmetic parts stripped. The keys that actually matter are commented `MATTERS` |
| `ReShade.log` | a complete log from a working run, start to finish |

The bridge's own runtime is configured by `ReShade2.ini`, which ships in the
release download rather than here.

---

## Reading the log

If yours does not work, diff it against this one. Five lines answer almost
everything, and they appear in this order:

```
Registered add-on "MGPU Bridge"
```
Absent means ReShade has no add-on support, or the file is not in the folder.
This is the single most common reason nothing happens, and there is no error
because the add-on was never loaded to write one.

```
[MGPU][T2] SELECTED adapter[N] luid=... desc="..." outputs=N rule="..."
```
Which card the neural work was given to, and why. On a mixed pair this is the
line to check first: the bridge picks whichever card is **not** rendering the
game, and if that is a card DLSS-NR will not run on, nothing will ever appear.

```
[MGPU][P1.6] GAME runtime preset="..." | N of M techniques ENABLED
[MGPU][P1.6] BRIDGE runtime preset="..." | N of M techniques ENABLED
```
Printed once per runtime. Anything listed here is being drawn on top of what
you are looking at. This line exists because a stale bridge preset once drew a
motion-flow debug view over the neural output and cost a night of wrong
diagnoses.

```
[MGPU][P7.2] mgpu.ini read from ...
```
The settings file that actually took effect - not necessarily the one you
edited. It is read from beside the add-on, not from the working directory.

```
[MGPU][P4.0] stream REQUESTED - ... passes=N, ...
```
Every setting in force for this run, in one line. If it disagrees with your
`mgpu.ini`, the P7.2 line above says why.

---

## What is in the reference log, including the parts that look like errors

This log is a real two-minute run on the development machine, not a curated
one, and it contains two things that read as faults. Both are benign and both
are here on purpose, because you will probably see them too and it is better to
know what benign looks like.

**`0xBAD0000C FAIL_OutOfDate` at init.** An environment fault rather than a
defect - the same call succeeds repeatedly on the same machine with the same
binary. The bridge continues and neural rendering still runs. The operational
fix is to open the NVIDIA app and relaunch the game. The log says all of this
on the line after the code.

**One burst of seal faults**, ten lines at 11:34:37 - two `DROPPED`, six
`REORDERED`, one `STALE` - and then nothing for the rest of the run. That is a
hiccup: the transport recovered and returned to `gap=1 OK`.

**The difference that matters is duration, not presence.** A short burst that
stops is a hiccup. Thousands of seal lines running to the end of the file is a
condition, and on this project that has meant the producer outrunning the ring -
a game producing frames much faster than the second card consumes them. If your
log looks like the second case, say so in an issue; it is worth knowing about.

---

## If the bridge window shows a cycling rainbow

That is the liveness colour and it is deliberate. It means the window and its
swapchain are alive and presenting, but **no neural frames are reaching them**.
A static colour could not distinguish "presenting" from "presented once and
hung", so it animates instead.

So the window is working and the stream is not delivering. Check, in order:

1. The `P4.0` arm line. If it is absent, the stream never armed.
2. The `SELECTED adapter` line. If the neural card is one DLSS-NR will not run
   on, that is the answer.
3. Any `FAIL_` or `0xBAD0` result codes.
4. `DROPPED`, `REORDERED` or `OVERRUN` seal lines, which mean frames are
   arriving but being rejected.

---

## If a log line and the documentation disagree

Believe the log, and open an issue with it attached. Feel free to redact your
Windows username out of any paths - only the `[MGPU]` lines are needed.
