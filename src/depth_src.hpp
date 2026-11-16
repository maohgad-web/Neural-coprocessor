// MGPU Bridge - R376 (0.5.1 discovery): the game's own depth as a source for the depth lane.
//
// WHY. Depth reaches the neural card by ONE route: ReShade's DEPTH semantic through
// mgpu_depth_tap.fx, copied at finish_effects. Vectors have three routes (the barrier hook, the copy
// at the game's DLSS evaluate - R106, the copy at the Streamline tag - R300d); depth had no twin of
// the last two. Frame generation on the neural card (FrameGen=4) and the neural pass are handed the
// same lane, so which depth fills it is one of the inputs matrix M4 compares.
//
// WHAT. DepthSource picks who fills the lane on the game's card, frame by frame:
//   absent / 0  the tap, as before. Nothing in this file runs beyond one relaxed load at two doors.
//   1           the depth the game hands DLSS (the calibrator's table), copied at that evaluate. The
//               resource's state is not known there - R106's caveat, unchanged - so it is ASSUMED:
//               DepthSourceState (hex, default 0xC0 = pixel + non-pixel shader resource).
//   2           the depth the game TAGS for Streamline, copied at the tag with the state the game
//               declared in sl::Resource (needs SLTags=1 or 2). Tags valid only now (lifecycle 0).
//   3           as 2, every lifecycle.
//   4           (R381) the depth the game hands its FSR upscale call, copied at that call with the state the
//               game declared in the descriptor (needs FfxHook=1).
// A frame whose copy lands is sealed with that depth and the tap's copy is skipped for that frame;
// any other frame takes the tap exactly as before. So the pick is live: no re-arm, and a refused
// source falls back to the tap on its own. Refusals (format family, size, multisampling, state not
// declared) are counted and each reason is said once.
//
// WHERE THE COPY RUNS. On the game's own command list, inside the call the game is already making
// (its DLSS evaluate or its slSetTag call), like the two vector routes. No wait, no new queue.
// The lane itself is gpu1_context's: this file reaches it through stream_depth_from_game(), under
// the same guard as stream_mvec_copy (try-lock, live stream, MVec=3 armed).
//
// LIVE. With the key present, FGLive=1 re-reads it on the bridge thread (fg_gpu1's live read).
// Lines: [MGPU][R376] at init when the key is present, then every 1200 attempts.
#pragma once

namespace mgpu
{
namespace depthsrc
{
void init();          // on_init_swapchain, never DllMain: reads DepthSource / DepthSourceState
bool present();       // the key was in mgpu.ini at init (the doors below are worth installing)
int  mode();          // one relaxed load: 0 tap, 1 DLSS evaluate, 2 / 3 Streamline tag, 4 FSR upscale call
// R382: N01950's key DepthContract=1 ("declared-state depth copy at the evaluate / dispatch") means: the depth
// of whichever contract call arrives. It is honoured as DepthSource 1 at a DLSS evaluate and 4 at an FSR dispatch.
bool wants_dlss();    // mode 1, or DepthContract=1
bool wants_fsr();     // mode 4, or DepthContract=1
void read_live();     // bridge thread: the key again (only when it was present at init)

// calibrator hook_evaluate (game's render thread), scene feature, once per frame; mode 1 only.
void on_dlss_evaluate(void *cmd_list, unsigned long long depth, unsigned render_w, unsigned render_h,
                      unsigned depth_x, unsigned depth_y);
// sl_tags read_one (game's tagging thread); modes 2 / 3 only.
void on_sl_tag(void *cmd_buffer, unsigned long long resource, unsigned declared_state, int lifecycle);
// alt_ffx_hook (game's render thread, inside its FSR upscale call); mode 4 only. `state` is D3D12 states.
void on_declared(void *cmd_list, unsigned long long resource, unsigned state, unsigned render_w, unsigned render_h);
// gpu1 stream_on_finish_effects: a frame was sealed with the game's depth.
void note_sealed();
}   // namespace depthsrc
}   // namespace mgpu
