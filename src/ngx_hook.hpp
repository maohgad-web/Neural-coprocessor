#pragma once
// MGPU Bridge - [PORT B2, 2026-11-11] the NGX contract from nvngx's own entry.
//
// WHAT THIS IS. An inline hook on _nvngx.dll's D3D12 CreateFeature /
// EvaluateFeature / ReleaseFeature. It reads the parameter block the caller
// (the game, or Streamline on the game's behalf) hands to NGX, and publishes
// the same contract the calibrator's data-scan route publishes: colour, depth
// and motion-vector handles, their extents, the create flags, jitter and
// MVecScale. The consumer side does not know which route filled the table.
//
// WHY. The calibrator reaches the same block by swapping pointers in another
// module's writable data (R101/R115). That route is what this project has run
// since 0.2.x, and on 2077 it was suspected in a 0xC0000374 heap corruption
// (2026-11-02). The hook writes nothing outside nvngx's own entry prologues,
// stops the world for the install, and reads a block that is already in a
// register - it cannot corrupt another module's data.
//
// THE DEPTH ANSWER, since it was asked. Yes, depth comes through this hook
// too: the same block carries DLSS.Depth (handle) and its extent, and
// capture() stores them as `table.depth` / `KEY_DEPTH` exactly as the
// data-scan route does. Vectors and depth are one capture, not two.
//
// WHAT IT DOES NOT DO. It does not choose a source, it does not gate a bind,
// and it does not touch the frame-generator feature's evaluates (id 11): only
// a SuperSampling handle may become the contract, and an evaluate of our own
// DLSS-NR feature (detected by its DLSSNR.* parameters) is skipped.
namespace mgpu
{
namespace ngxhook
{
// Idempotent and cheap to call repeatedly: returns true once the hook is
// live. `want` is the NgxHook key; 0 leaves the process untouched.
bool install(int want);
bool installed();
bool wanted();
}
}
