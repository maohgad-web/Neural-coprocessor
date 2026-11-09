# Contributors

Code contributed by people other than the primary author, and what it covers.
Every key they added is listed in [`docs/CONTRIBUTED_KEYS.md`](docs/CONTRIBUTED_KEYS.md).

## [NO1950](https://github.com/NO1950)

Three changes to the add-on, from a two-GPU machine where the composition mode
and the overlay could not be used at the same time:

- **The overlay relationship.** The overlay request is refused on the game's
  runtime and opened on the GPU 1 runtime instead, whose swapchain is the
  composition swapchain - the neural picture - so the panel is drawn over the
  live neural frame rather than in place of it. The host runtime is identified
  by having no window and is recorded when the panel opens; input is fed from
  `reshade_overlay` (the only per-frame callback that is not behind a tab
  selection) out of the game runtime's own `ImGuiIO.MousePos`, and the game's
  input is blocked while the panel is up. Opt-in via `PanelOverPicture`.
  Verified on Far Cry 6 and Kingdom Come: Deliverance II, in opposite
  arrangements of the two cards.

- **The P4.1 strict lookup against a proxy-style snippet.** `[P4.1]` resolves
  `NVSDK_NGX_D3D12_PopulateParameters_Impl` strictly from the private snippet,
  which cannot answer while that snippet's surface is made of linker
  forwarders to a module it loads later. The failure line now names the entry
  point.

- **Residual neural rendering at a reduced scale (R299).** The neural stage can run
  at a fraction of native while the picture stays native: the frame is downsampled,
  the model runs small, and its difference is composited back onto the native frame.
  The feature is created once and the scale changes live without rebuilding it.
  Uses the shaders from xenmods/DLSSNR-Cost-Scaler (MIT). Opt-in via `NRScale`.

## [grtninja](https://github.com/grtninja)

Two fixes to the add-on's own code, reviewed and merged from external pull
requests:

- **Adapter selection.** When the game's swapchain-derived adapter LUID is
  absent from DXGI enumeration, the bridge no longer infers the target from
  output counts - a missing game LUID is now a hard identity gate ahead of
  candidate and tiebreak selection. The selection policy was factored into a
  small pure helper so the boundary is directly regression-tested, with
  table-driven coverage for missing-LUID cases, software filtering, no
  hardware candidates, output tiebreaks, LUID high-half mismatch, and
  enumeration reorder.
- **`mgpu.ini` parsing.** The previous reader accepted a silently clipped
  document past its 8192-byte buffer, so settings after the cutoff read as
  absent with no warning - a real problem once the shipped config itself grew
  past that size. The bounded read is now 64 KiB, a document that doesn't fit
  is rejected outright rather than partially applied, and the line-key parser
  is shared with a portable, CPU-only regression suite covering encoding
  (UTF-8 BOM, UTF-16 rejection, malformed UTF-8, embedded NULs, control
  bytes) and boundary conditions.

Thank you for both - especially for the regression coverage that makes these
correctness properties checkable going forward rather than just asserted.

## Mohammed Hasan - [MoHasan9505](https://github.com/MoHasan9505)

His test system: NVIDIA 617.14 (32.0.16.1714) on the RTX 5070, AMD Adrenalin
32.0.31041.1004 on the Radeon AI PRO R9700, and one display connected to the
RTX 5070 over HDMI.

Two findings against the add-on's NGX and teardown code, from a fork where
an AMD card renders the game and an RTX card runs DLSS-NR
([nr-redgreen](https://github.com/MoHasan9505/nr-redgreen)). Both merged in
a guarded shape, under his name:

- **The core `Init` takes four arguments.** The driver's `_nvngx.dll` export
  of `NVSDK_NGX_D3D12_Init` is the four-argument form; called through the
  SDK's five-argument typedef, the driver read a stack address as the
  version and the check passed or failed by ASLR - the intermittent
  `FAIL_OutOfDate` on roughly every other launch (#44). His disassembly on
  driver 617.14 is the first explanation of that failure that holds, and it
  also explains a chase on the primary author's side. Merged as the last
  rung of a ladder: the four-argument call is tried only after the
  five-argument one has returned `FAIL_OutOfDate`, so a driver whose export
  really takes five arguments is never handed a junk parameter.
- **The GPU 0 teardown drain never ran.** The drain checked a fence one line
  after that fence had been released, so it was dead code and the shared
  transfer buffer was freed with no wait. His fix waits on the game-side
  fence, for the last value actually signalled, so a normal exit waits
  nothing. Merged as written, plus a timing line on every teardown and a
  switch the add-on throws itself if the wait ever stalls, because it turns
  on a wait no shipped build had executed.

Thank you for both, and for the disassembly in particular.

## Dominique Tremblay - [lordzaxxon555-art](https://github.com/lordzaxxon555-art)

His test system for this work: RTX 4080 SUPER (game) and RTX 5060 Ti (DLSS-NR), 3840x2160, Starfield.

Work from an iterative Starfield development branch (T1.1 to T1.19), merged as one revision, R300
(`docs/R300_T119.md`). Every part is behind its own `mgpu.ini` key and off by default:

- **The scene-feature filter (R300b, `SceneHandleSet`).** Starfield creates and releases several DLSS
  features while it runs. The calibrator's evaluate filter only accepted the newest one, so when the game
  kept evaluating an older feature the vector table and the evaluate-route copy froze and frames were sealed
  without vectors. He found it in his own logs and fixed it: any latched scene feature is accepted, and the
  handle is moved to a live feature when its own is released.
- **Instruments, in their own files with unit tests.** Per-evaluate temporal record and slot-stamped jitter
  (`TemporalDiag`), GPU 1 job timing with the copy-queue split (`Perf11`), the producer's cost on GPU 0 with
  GPU timestamps (`ProducerDiag`, `Gpu0Timers`), and a detector for a game that stops evaluating its scene
  feature, with every DLSS create and release logged (`FeatureDiag`).
- **Experimental, for testing:** Streamline-tagged vectors as the source and a copy at the tag call
  (`SLTagsMvec`, `SLTagsCapture`), a scheduling experiment (`PerfPrefetch`), a BetterConsole crash guard
  (`BetterConsoleGuard`), and panel additions (`PanelAutoHeight`, `PanelStats`, `PanelPersist`, `FrameCap`).

Thank you - in particular for R300b, which corrected a filter the add-on's own comment said was already right.

## [ZhizhenS](https://github.com/ZhizhenS)

Test system: two RTX 5080s, 3840x2160 at 120 Hz, Stellar Blade. Tested on davidm71's fork and ported here
([write-up](https://github.com/ZhizhenS/mgpu-bridge-fg-flicker-fix)):

- **The bands with frame generation (BB1, `PresentBuffers`, `PresentRaceProbe`).** Traced dark horizontal
  bands to the per-frame clear reaching the screen in Independent Flip, shown by dyeing the clear magenta.
  The clear now runs only when the picture will not cover the back buffer, and the buffer count is a key.
- **The stray panel pointer (PP4).** The pointer stayed on the picture after the panel closed.
