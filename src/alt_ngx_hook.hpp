// MGPU Bridge - R377 (0.5.1 discovery): the alternative NGX route - N01950's inline hook, as its own module.
//
// WHAT IT IS. N01950's method (PR "non-native frame generation", ngx_hook.cpp, "PORT B2"): an inline hook
// (SafetyHook) on three exports of _nvngx.dll - D3D12 CreateFeature / EvaluateFeature / ReleaseFeature. It is a
// second DOOR into the calibrator, not a second capture: the detours call the calibrator's own hook functions,
// the ones the import-table route has always run, so the table, the scene-handle latch, the copy of the vectors
// at the evaluate (R106), the frame-generation map and the depth source (R376) are the same code on both routes.
//
// WHY A USER WOULD CHOOSE IT. The calibrator reaches NGX by swapping pointers: import slots in every module but
// mine (R101) and cached pointers in other modules' writable data (R102). This route writes nothing outside the
// first bytes of three functions of _nvngx.dll. Which of the two a title tolerates is the user's pick.
//
// KEY. NgxHook=1. Absent or 0: nothing of this file runs beyond one read of mgpu.ini at the one-shot and one
// relaxed load per present; no byte of any module is touched; the calibrator installs exactly as before.
// With NgxHook=1 the calibrator's two install rungs do NOT run (one route per session). If the hook cannot be
// installed there is NO fall-back to the rungs - the user asked not to have them - and the log says so at ERROR.
//
// WHAT IS MINE, NOT N01950'S (each is a rule of this project applied to the method):
//   - the hooks are created disabled, the calibrator is armed with the trampolines, then the hooks are enabled:
//     no call can arrive in a detour before the calibrator knows where the real function is;
//   - a call made by the bridge's own module goes straight to the real function (the import route skips my
//     module the same way), by return address - not by looking for my parameter names in the block;
//   - SafetyHook is fetched at a pinned version when the build is configured and is never in the repository.
//     A build without it carries a stub: NgxHook=1 is then refused in one line and the calibrator installs.
//
// Lines: [MGPU][R377].
#pragma once

namespace mgpu
{
namespace altngx
{
void init(int calib_packed);   // the one-shot in dllmain, before calibrator::install: reads NgxHook, first attempt
bool wanted();                 // NgxHook=1 in a build that carries the route: the caller skips calibrator::install
bool pending();                // wanted and not live yet (one relaxed load): call try_install() once per present
void try_install();            // _nvngx.dll loads with the game's DLSS, which can be after us
void uninstall();              // puts the three entries back; before calibrator::uninstall
}   // namespace altngx
}   // namespace mgpu
