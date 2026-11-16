// MGPU Bridge - R381 (0.5.1 discovery): the FSR contract route - N01950's FidelityFX hook, as its own module.
//
// WHAT IT IS. N01950's method (PR "non-native frame generation", ffx_hook.cpp): an inline hook (SafetyHook) on
// ffxCreateContext and ffxDispatch of the FidelityFX API DLL the game uses. A title that upscales with FSR hands
// that call its depth, its motion vectors (each with the state it is in), the vector scale, the jitter, the
// render size and the reset flag - the same contract a DLSS title hands NGX. This file reads it and feeds the
// SAME table and the SAME copy doors the DLSS routes feed: the calibrator's table (publish_external), the vector
// lane (stream_mvec_copy_declared, with the declared state) and the depth lane (depth_src, DepthSource=4).
// No second table, no second copy path.
//
// WHY. On a title without NGX the calibrator has no table: the vector lane falls back to the census and frame
// generation gets vectors only where that guess is right. The game's own FSR call states them.
//
// KEY. FfxHook=1. Absent or 0: nothing of this file runs beyond one read of mgpu.ini at the one-shot and one
// relaxed load per present. Needs a build that carries SafetyHook (R377's fetch); otherwise the key is refused in
// one line.
//
// RULES OF THIS PROJECT APPLIED TO THE METHOD.
//   - ONE WRITER. If the game's DLSS wrote the table since this route's last frame, this route stays out for that
//     frame (the DLSS route owns the table while DLSS evaluates). Counted.
//   - The descriptor is read through its type id, 0x00010001, which fixes its layout in AMD's API (a changed
//     layout gets a new id). The offsets were checked by compiling AMD's header (FSR SDK 2.3.0).
//   - The DLL names the PR looked for missed the one FSR 3.1 titles ship. All are looked for:
//     amd_fidelityfx_dx12.dll, amd_fidelityfx_loader_dx12.dll, amd_fidelityfx_upscaler_dx12.dll, ffx_api.dll.
//   - The bridge's own FidelityFX calls (FrameGen=3 uses the same DLL) go straight to the real function.
//
// Lines: [MGPU][R381].
#pragma once

namespace mgpu
{
namespace altffx
{
void init();          // the one-shot in dllmain: reads FfxHook, first attempt
bool wanted();        // FfxHook=1 in a build that carries the route (false again after a refusal)
bool pending();       // wanted and not live yet (one relaxed load): call try_install() once per present
void try_install();   // the FSR DLL loads when the game selects FSR, which can be long after us
void uninstall();
}   // namespace altffx
}   // namespace mgpu
