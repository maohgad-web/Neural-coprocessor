// MGPU Bridge - R379 (0.5.1 discovery): the game's own camera for frame generation, from Streamline.
//
// WHY. The DLSS-FG SDK lists the camera block as required engine data for every evaluate: clipToPrevClip /
// prevClipToClip (how the camera moved between the two frames), cameraViewToClip / clipToCameraView, position,
// axes, near / far, field of view. R341 hands over the identity for the first pair and an estimate from the
// FGFov / FGNear / FGFar keys for the rest: the generator is told the camera never moves.
// A Streamline title states that whole block itself, once per frame, in sl::Constants through the exported
// slSetConstants (sl_core_api.h, sl_consts.h) - whether or not it runs its own frame generation.
//
// WHAT. FGCamera=1 taps slSetConstants exactly as sl_tags taps slSetTagForFrame: the game's import slot is
// pointed at a thunk here that copies the struct (after checking its structType GUID) and calls the real function
// with the arguments untouched. One pointer store per slot at install, read-only afterwards, on the game's own
// calling thread, with a try-lock: the game's thread never waits. sl.* modules are not touched.
// Each seal is stamped with the number of the newest constants call; for a pair of evaluated frames the
// per-frame matrices between the two stamps are multiplied, because a pair can span more than one game frame
// (R87's elapsed count) and clipToPrevClip describes one.
//
// KEY. FGCamera=1 (with FrameGen=4). Absent or 0: nothing is installed; on() is one relaxed load at two doors.
// When the block for a pair is not available (no call, struct not recognised, a gap, more than 8 frames) the
// evaluate gets what it gets today, and the case is counted. Lines: [MGPU][R379].
#pragma once

namespace mgpu
{
namespace slcam
{
struct camera
{
    float clip_to_prev[16], prev_to_clip[16];   // across the whole pair, row-major as Streamline states them
    float view_to_clip[16], clip_to_view[16];   // the current frame's
    float pos[3], up[3], right[3], fwd[3];
    float cam_near, cam_far, fov, aspect;
    bool have_lens;                             // near / far / fov / aspect were stated (not Streamline's "invalid")
    unsigned frames;                            // game frames the pair spans
};

void init();                                    // dllmain one-shot: reads FGCamera
bool on();                                      // one relaxed load
void tick();                                    // dllmain, once per game present (D3D12 branch): deferred install after 900 presents
void on_seal(unsigned long long fi);            // gpu1 stream_on_finish_effects (game's present thread)
bool pair(unsigned long long f_prev, unsigned long long f_cur, camera &out);   // bridge thread, fg::record
void uninstall();                               // puts the import slots back
}   // namespace slcam
}   // namespace mgpu
