# Architecture

How the bridge works, and the handful of facts that are load-bearing and
non-obvious. `RESULTS.md` has the measurements; `METHOD.md` has the rules the
project measures by.

---

## The claim, and why it is not SLI

SLI split rendering **mid-graph**. That is why it needed driver and engine
cooperation, why it broke on every new rendering technique, and why it died.

DLSS Neural Rendering is **terminal**: it consumes a finished colour buffer and
produces a finished colour buffer. A terminal stage has no graph to split. It
can be picked up, carried to another device, executed there, and carried back  - 
or not carried back at all, if that device drives its own display.

That is the entire architectural claim. Everything below is the machinery for
testing it, and `RESULTS.md` is what it measured.

**Nothing NVIDIA ships is modified on disk, and no code is patched in memory.**
No binary patching, no signature work, no decompilation of anything. The add-on
calls documented NGX entry points and is named so that it satisfies NVIDIA's own
caller check honestly - see *The caller gate* below.

**What the add-on does modify, since 0.2.0, is import tables**, and this
document claimed otherwise until 0.2.2. The calibrator swaps `GetProcAddress`
pointers in the import tables of the modules resident in the process - 17 slots
across 130 modules on a typical launch - so that the NGX calls a title makes can
be observed. It is a pointer swap rather than a code patch, every slot is
restored on exit, and `Calib=0` disables it entirely. It is still import-table
modification, and it is named here rather than left implied: an integrity check
that compares a module's import table against the file on disk sees this process
as modified. Read the anti-cheat section of the README before running any of
this near an online title.

---

## The path a frame takes

```
GPU 0 (game)                          GPU 1 (bridge)
────────────                          ──────────────
game renders
    │
    ├── ReShade finish_effects event, game's command list open
    │   ├── 192-byte seal   ──┐
    │   ├── colour buffer   ──┤  one command list, seal recorded first,
    │   ├── depth  (Depth=1)──┤  each lane carrying its own validity
    │   └── vectors (MVec=3)──┤  flag for this frame
    │                         │
    │   Signal(produced)      │  shared fence, on the GAME's queue,
    │   ONE EVENT LATER    ───┼──  after the copies were submitted
    │                         ▼
    │                    cross-adapter shared heap, ring of 6 slots
    │                         │
    │                         ▼
    │                    seal checked: identity, order, age, contract
    │                         │
    │                         ▼
    │                    unpack → DLSS-NR x N passes → ping-pong
    │                         │
    │                         ▼
    │                    present on GPU 1's own swapchain and window
```

The game's own rendering is never touched. The bridge reads the finished frame
and does its work elsewhere.

---

## Adapter selection

By **LUID exclusion**, never by index. Enumeration order is not stable, and
LUIDs are themselves reassigned across driver restarts and reboots - this rig
has produced at least three different pairs across sessions. Nothing about the
selection is ever persisted.

The game's LUID comes from the **swapchain**, not from the first device event.
UE5 probes every adapter before settling, and one run captured a *software*
adapter as the "game" LUID. `init_device` is captured only as a provisional
value; the LUID of the device that owns the swapchain overrides it, and only a
swapchain-derived LUID authorises a selection.

Software adapters are excluded by `DXGI_ADAPTER_FLAG_SOFTWARE` **or** vendor ID
`0x1414`, because the flag alone is unreliable: this rig enumerates two
"Microsoft Basic Render Driver" adapters and only one sets it.

**If filtering does not leave exactly one candidate, the add-on selects
nothing.** A missing device is diagnosable. A device on the wrong adapter
succeeds, logs cleanly, and proves nothing.

`outputs=` is **not** a statement about what is physically plugged in - it
follows the Windows main-display setting. The same cabling produced
`outputs=1 / outputs=1` with one main display and `outputs=2 / outputs=0` with
the other. Selection never consults it.

## The bridge thread

Every GPU 1 object is created, used and destroyed on one thread, which also
owns the window and pumps its messages.

It is spawned **only** from `init_swapchain`. Spawning it on device events
created one thread per UE5 probe cycle - five per launch on this rig - each able
to still be executing inside the module when ReShade unmapped it. That killed
the game process intermittently, and intermittently is the worst way to find
out. `init_swapchain` fires once, on the real render device, after the probes
are over.

---

## The transport

**A shared heap, not a shared resource.** A committed resource carries its own
implicit heap and can be shared directly; a placed resource cannot, because the
heap owns the memory. Handing `CreateSharedHandle` a placed resource returns
`E_INVALIDARG`, which reads exactly like a capability refusal from the hardware
and is not one.

Two routes both work and both are carried:

| route | mechanism |
|---|---|
| **A** | `CreateHeap(SHARED \| SHARED_CROSS_ADAPTER)` → `CreatePlacedResource` → `CreateSharedHandle` on the **heap** → `OpenSharedHandle` on GPU 1 |
| **A'** | one `VirtualAlloc` region, `OpenExistingHeapFromAddress` on both devices |

Neither has beaten the other reproducibly, so neither has been chosen.

**The heap is created on the game's device.** Its command list can only
reference resources from the device that made it.

### The seal

192 bytes at the head of each ring slot - **seal v3** since 0.2.0, 64 bytes
through 0.1.0 - copied into the slot **ahead of the pixels in the same command
list**, so submission order guarantees it lands first. It carries frame index,
submit timestamp, geometry and slot index, and since 0.2.0 the geometry and
per-frame validity of the depth and vector lanes as well. A frame that cannot
supply one of them is marked so and the consumer leaves that input unbound,
rather than binding whatever the previous frame left there.

Identity, ordering, age and contract mismatches all fall out of comparing it
against what was expected. None of them is reachable by a one-shot probe, and
none is visible to a person watching a window - a stream that consistently
delivers frame N-4 looks perfect on static content.

**Scope: the seal proves identity, order and age. It does not verify pixels per
frame.** That was established once, byte-exact, and re-proving it every frame
would be measuring the instrument.

### The fence

One shared fence whose value **is** the count of frames whose copies are known
to have been submitted, signalled on the game's own queue.

**Signalled one event after the copies were recorded, never the same one.**
ReShade executes the list after the event returns; a signal placed in the same
event sits ahead of our own copies and clears before the data exists. That
produces a plausible frame most of the time and a torn one occasionally, which
is the worst available failure shape.

### Skip-to-newest

Every seal is checked - identity and ordering are the point and cost
microseconds - but the **neural work** runs only on a frame nothing newer has
superseded. Counted separately as `nr skipped`, because choosing not to denoise
a stale frame is not the transport losing one.

Before this existed, a two-pass run computed 5,302 evaluates against 451
presents: **83% of finished neural work discarded**, which is what turned a
17.5 ms budget into a 9 fps window.

---

## The neural stage

### The core/snippet split

NGX is two modules and they are not interchangeable:

- **`_nvngx.dll`** - the driver core, in the DriverStore. Owns the **parameter
  block**.
- **`nvngx_dlssnr.dll`** - the feature snippet, in the game directory. Owns the
  **feature and its own session**.

Resolving an entry point from whichever module answers first produces working
code that fails later, with a different error each time you get closer:

| code | means |
|---|---|
| `0xBAD0000B` | called `CreateFeature` on the **core** - it has no snippet mapping |
| `0xBAD00007` | initialised the core but not the **snippet's own session** |
| `0xBAD00005` | set generic `Width`/`Height` instead of `DLSSNR.Width`/`DLSSNR.Height` |
| `0xBAD00002` | caller gate - see below |
| `0xBAD0000C` | `FAIL_OutOfDate` - see below |

The working sequence, in order:

1. `NVSDK_NGX_D3D12_Init` - **core**
2. `GetCapabilityParameters` - **core**
3. `NVSDK_NGX_D3D12_Init_Ext` - **snippet**, passing the core's block
4. `PopulateParameters_Impl` - **snippet**
5. `Set("DLSSNR.Width")`, `Set("DLSSNR.Height")` - the namespaced keys, not the
   generic ones
6. `CreateFeature(Reserved18)` - **snippet**

The feature reads its **own namespace**: `DLSSNR.Width`, not `Width`. Subrect
keys carry no separator: `DLSSNR.ColorSubrectWidth`.

**`GetCapabilityParameters` returns the core's shared block, not a per-caller
one.** Two consumers in one process therefore write into each other's
parameters, and one calling `DestroyParameters` pulls the block out from under
the other. This is why the one-shot probe chain is opt-in and off by default.

### The caller gate

`nvngx_dlssnr.dll` resolves the module owning its caller's return address, takes
that module's file path, and requires it to contain the literal substring
`nvngx.dll`. A caller that fails gets `0xBAD00002 FAIL_PlatformError`.

So the add-on ships as **`nvngx.dll_mgpu_bridge.addon64`**. The add-on *is* the
module that calls the snippet, so its own filename is what has to satisfy the
check. The rename happens in the CI staging step and nowhere else, so
`CMakeLists.txt` stays closed and CI keeps verifying the name CMake produces.

**Do not shorten it to a bare `nvngx.dll`** - a directory or executable with
that name breaks process startup - and it must keep the `.addon64` extension or
ReShade will not load it.

### `FAIL_OutOfDate` at Init does not stop neural rendering

Reproduced repeatedly: the core `Init` returns `0xBAD0000C` while
`CreateFeature` and every `EvaluateFeature` succeed and the picture is correct.

`CreateFeature`, `ReleaseFeature` and `EvaluateFeature` all resolve to the
**snippet** - the export line says `used:snippet` on every launch - so a core
`Init` failure never touches the path that does the work.

The operational fix, reproduced: **open the NVIDIA app, then relaunch the
game.** Every earlier clearing event - an app update, a reboot, a settings
change - was an instance of that.

### What the model actually is

From the snippet's own log:

```
DLSSNR: 1 config(s) available:
  [0] CC_Control_History_Blend_Quantize_With_Teacher_honest_tench_..._weights
      (backbone: crazy-cuckoo)
```

It is a **preset**, not a model - `CG2RFindWeightByPreset` resolves a preset
number to a weight set, and "Model A/B/C" in other tools is a label over that.
**This DLL build ships exactly one.** A model dropdown wired to it would resolve
to the same weights whatever was picked, which is indistinguishable from a knob
with a subtle effect. If that line ever reports more than one, a preset control
becomes worth building.

The weight heap is ~140.9 MB and is refcounted across features: N feature
handles cost N sets of history buffers and one set of weights.

### Passes

`Passes=N` runs DLSS-NR N times per frame, one feature handle each,
ping-ponging between two output textures. All handles are created at arm  - 
179-220 ms each, one-time rather than per-resolution - so changing the count live
costs nothing.

**One handle per pass, not one handle evaluated twice.** The feature carries
temporal history, so a single handle run twice in a frame would have its history
be "the previous pass" rather than "the previous frame". The GPU cost would be
identical and the measurement still valid, but the picture would ghost - an
artefact of the test rig that looks exactly like a real fault.

**Latency scales with passes; the game's frame rate does not.** The bridge
window's own output rate falls as the count climbs. That is the architecture
working, and it is the one thing about the demo that reliably reads as a bug to
someone who has not been told.

#### The maximum is two, and it is a power decision

Measured on the development rig at 1080p, against a ~16.7 ms frame period:

| passes | GPU 1 work per frame | duty cycle | GPU 1 power |
|---|---|---|---|
| 1 | 8.3 ms | ~50% | ~49 W |
| 2 | 17.5 ms | ~100% | ~145 W |
| 3 | 26.0 ms | over budget | ~180 W - the card's limit |

Two passes fill the card. Three exceed the frame period, so the card clamps at
its power limit and further passes are served on throttled clocks - three, four,
five and six all drew an identical ~180 W, which is the limiter rather than a
coincidence. **Past two, a pass buys latency rather than picture.**

That alone would be an argument for a documented recommendation. What makes it a
hard bound in code is the other end: this add-on ships with `Frames=0`, so a run
is unbounded. It holds the second GPU at whatever load it reaches for as long as
the game is open, in a window the user has very likely minimised - and on a
larger card at a higher resolution those watt figures scale with the hardware,
not with the numbers above. Sustained maximum board power is the condition under
which a marginally seated power connector fails.

**The failure was observed, not hypothesised.** One development session ended
with the game rendering black on both displays after several minutes, with no
fault in any log. It went unexplained for two days and was briefly blamed on the
title's engine. It was a six-pass run. `MAX_PASSES` is 2, enforced in
`gpu1_context.cpp`; a larger value in `mgpu.ini` is clamped and the log names the
value that was asked for.

The multi-pass ghosting hypothesis that motivated six passes is **not settled by
any of this** - it was never tested, because no ghosting scene was ever captured.
Testing it later means raising the constant in a local build. It does not ship
raised.

`DLSSNR.Reset` is set on the **first frame only**. Every probe before the stream
existed set it on every evaluate, because each was an independent experiment and
history between them would have contaminated the control. In a stream
`dlssnr_prev_output` is temporal history and is meant to carry. If output ever
looks smeared or ghosted, setting it back to 1 is the first diagnostic.

### The payload, and what 0.2.0 changed

**Through 0.1.0, nothing but the colour buffer crossed the link.** That was not
an omission, it was a measurement: two evaluates on one captured frame,
identical except for whether a depth texture was bound, produced
**byte-identical output over all 3,686,400 pixels**. The depth was a
front-to-back gradient rather than a clear, which is what makes it decisive - a
flat depth carries no more information than none, so an identical result would
have been ambiguous, and a sweeping plane removes that reading. Motion was
derived from colour on GPU 1. The ~2x payload the architecture had budgeted for
did not exist.

**0.2.0 sends all three lanes.** Colour, the game's depth and the game's
velocity buffer are copied into the same ring slot under one seal and bound on
GPU 1. The payload the architecture budgeted for is now real, and then some: on
a 4K title the depth lane alone is 33.2 MB per frame, and the vector lane is
whatever extent the engine chose for it, which is not always the smaller of the
two. One title measured for this release writes velocity at the display extent
while colour renders at about 65% of it, so its vector lane is **larger** than
its colour lane. That is the engine's choice and not a sizing error.

**The byte-identical result above has not been re-run against the 0.2.x
parameter set, and it should be.** It is a statement about the parameters 0.1.0
wrote, at `preset=0`. 0.2.0 writes a different set. The image-stability
improvement from feeding real depth and velocity is an observation about how
frames look, not a controlled byte comparison, and those are not the same kind
of evidence. Re-running that one experiment on 0.2.x would settle it in an
afternoon.

### What is sent and what is used are counted separately

They come apart constantly, so the log keeps them apart:

- the **producer** copies a lane into the slot - `copies` / `missing`
- the **consumer** unpacks it, checks the seal and binds it - `unpacked` /
  `bound` / `skipped`

A frame can be sealed and transported and still not be bound, and a lane can be
transported for hundreds of frames before it first carries anything. On one UE5
title measured for this release the vector lane ran at 100% in steady state
while its first thousand frames carried nothing at all - which reads as a
cumulative percentage climbing for minutes, and means nothing until it settles.

---

## The calibrator, and where motion vectors come from

New in 0.2.0, and the subsystem this document did not describe until 0.2.2.

The bridge needs two things it cannot get by watching ReShade: which NGX feature
handle belongs to the title's own DLSS, and where the engine keeps its velocity
buffer. Neither is exposed by any API. The calibrator observes the title's own
NGX calls to find out.

### Two install routes, and `CalibRung`

| rung | what it does |
|---|---|
| 1 | swaps the `GetProcAddress` pointer in the import tables of the resident modules |
| 2 | scans module data sections for already-cached NGX entry points and replaces those |

Both run in order by default. `CalibRung` selects one alone - `0` both, `1` the
import table, `2` the data scan - so a title that faults during install can be
narrowed to one route without a new build. `Calib=0` disables the whole thing
and restores every slot.

**The order inside the install is load-bearing, and getting it wrong shipped a
crash.** Through 0.2.1 a slot was patched before the real entry point behind it
had been published, leaving a window - measured at 33 to 46 ms - in which a call
arriving through a patched slot found nothing behind it. The hook then returned
failure **without writing the caller's out handle**, so the caller read an
uninitialised pointer and the fault surfaced inside the title's own Streamline
module rather than in this add-on. It reproduced on every launch on one
reporter's machine and about one launch in ten here, with no hardware difference
between them, which is the signature of a race rather than a configuration
problem. 0.2.2 publishes the pointers before patching the first slot, resolves
late if a hook is reached before install finished, and zeroes the out handle on
every failure path.

### The two routes to a velocity buffer, and `MvecFromEval`

Engines announce a render target through a resource barrier, and the transport
can read the velocity buffer there. Some engines do not: they write velocity
with a compute shader and announce nothing, and on those the barrier route fires
zero times. The log says so directly - `BIND PATH VISIBILITY:
OMSetRenderTargets fired=0 | BeginRenderPass fired=0 | ResourceBarrier
fired=1074 -> BIND PATH IS BLIND TO US`. Both a Frostbite title and a UE5 title
measured for 0.2.2 read exactly that.

The fallback is to copy at the title's own DLSS evaluate, which is a place the
buffer is known to be live. That route only exists while the game is running
DLSS or DLAA, because with neither there is no evaluate to copy at.

| `MvecFromEval` | |
|---|---|
| `0` | the announced route only. The A/B control - nothing can override it |
| `1` | the DLSS evaluate, from the first frame |
| `2` | auto, and the shipped default: the announced route, falling back to the evaluate after 300 frames have passed with nothing delivered |

**The address was never the hard part; the trigger was.** Both routes were
handed the same correct resource pointer in the runs that established this. What
separated a lane carrying 71% of frames from one carrying none was which moment
the copy was taken at.

### What this costs the process

The calibrator's own overhead is measured and logged per run - about 6 us per
frame on the reporter's rig at 12,213 evaluates. The cost that matters is not
that one. It is that this is import-table modification inside a live game
process, and the README's anti-cheat section is where that is discussed.

---

## The present path

The bridge owns its own swapchain, window and present loop on GPU 1.

**The backbuffer format follows the game's format.** It was once hardcoded to
`R10G10B10A2_UNORM`, which was correct for the title it was written against and
silently wrong for every other one - `CopyTextureRegion` does not convert, so a
guard correctly refused the copy every frame and the window fell through to its
no-output fallback. The chain now takes its format from the seal.

**The present is gated on new frames**, not on the display's refresh. Presenting
at 210 Hz when frames arrive at 60 spends GPU 1 bandwidth re-showing frames
already on screen, on the same link the payload uses.

`Window=` chooses the shape: `crop` is a 1280x720 bordered window, `match` sizes
window and swapchain to the source, `fit` goes borderless at the monitor size
with the swapchain still at source size so DXGI scales on presentation. The
window is placed at **its own monitor's origin**, not the virtual desktop's.

**A stall in this loop is a known open fault.** On rare occasion GPU 1's present
fence stops advancing while the device stays healthy and the fence sits exactly
one behind. It has not been isolated, it is not fixed, and 0.2.2 handles it by
failing fast and telling the player to restart rather than waiting: a bridge
that hangs silently is a worse failure than one that says what happened. The
standing observation is that it is more likely when the bridge window is not on
top on the monitor it presents to, which points at compositing, but
`occluded-presents=0` does **not** rule that out - `DXGI_STATUS_OCCLUDED` is a
success code and only fires on full occlusion, so a visible-but-uncomposited
window returns `S_OK`.

`Present=split` puts the frame handed **to** the model on the left of a movable
seam and what it **produced** on the right - the same frame, in one window. It
exists because no two runs of a game contain the same frame, so any comparison
made across two captures is confounded by everything that changed in between.
The seam moves on hotkeys precisely so it can be dragged across a face with
nothing on screen but the game.

---

## Reading the log

Every line carries an `[MGPU]` prefix, so a run is one grep:

```
grep "\[MGPU\]" ReShade.log
```

Four lines answer most questions:

| line | what it settles |
|---|---|
| `Registered add-on "MGPU Bridge"` | ReShade has add-on support and found the file |
| `[P1.6] ... N of M techniques ENABLED` | what is being drawn on top of what you are looking at, **per runtime** |
| `[P7.2] mgpu.ini read from ...` | which settings file actually took effect |
| `[P4.0] stream REQUESTED - ...` | every setting in force, in one line |

And the shape of a healthy run:

| what you see | what it means |
|---|---|
| the add-on loading and unloading several times at startup | UE5 probing each adapter. Expected. Exactly **one** `bridge thread spawned` line should appear in the whole log |
| `T2 SELECTED adapter[n] ... rule="exclusion ..."` | selection worked; that LUID is the target card |
| `T2 REFUSING: ...` | no unique non-game hardware adapter. Not a crash - the add-on declines to guess |
| `T3 MISMATCH` / `T3 FATAL` | bound to the wrong adapter. Releases and stops, by design |
| `SEAL ... gap=1 ... OK` | the transport is healthy. A gap in the sequence is more interesting than its absence |
| a burst of `DROPPED` / `REORDERED` / `STALE` right after a window resize | the resize draining GPU 1 while the producer keeps running. The log says so before it happens |
| log ends mid-cycle, game gone | a crash inside an add-on unload. This was the `init_device` thread-spawn bug; if it returns, that fix regressed |

**The add-on logs the inputs to each decision, not the verdict** - the full
adapter table, both LUIDs at the comparison, the HRESULT of every call, every
descriptor field as a number before the call that consumes it. A verdict cannot
be re-examined after the fact; numbers can. This is the single most valuable
habit in the project and it has caught several of its own false results.

**`ReShade.log` is overwritten on every launch.** Copy it, `nvngx.log` and
`nvngx_dlssnr_*.log` aside **together** before relaunching - they are only
interpretable as a set.
