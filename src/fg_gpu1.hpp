#pragma once

// MGPU Bridge - R341 FrameGen=4: NVIDIA DLSS Frame Generation on the NEURAL
// card (0.5.0 discovery, Experimental, off by default)
//
// WHAT THIS IS FOR
//
// The neural card finishes a denoised frame per NR evaluate. With frame
// generation it also makes the in-between frame of the previous and the current
// NR output, and the bridge shows two frames per evaluate: the generated one
// first, the real one half an interval later. The game is not touched; the game
// card does nothing more. The cost is about half an NR interval of latency on
// the bridge's picture.
//
// Design: Dave (davidm71), DGP1/DLSSG1 in his fork (MIT, permission 2026-10-21):
// drive NVIDIA's nvngx_dlssg snippet directly on the neural device the way NR and
// SR are driven - Init_Ext, CreateFeature(FrameGeneration), Evaluate per frame
// with the game's vectors and depth that the stream already carries; camera
// matrices ESTIMATED from a FOV / near / far the ini gives (a ReShade add-on has
// no matrices); clip-to-previous-clip identity with CameraMotionIncluded=1 (the
// game's vectors carry the camera). The present side is a pacing thread. This
// file is our implementation of that design, DLSS-G only (his FSR 3 path, the
// mod's own generator, multi-frame and HUD-less are not in this round).
//
// THE CONTRACT
//   - FrameGen=4 in mgpu.ini, read at arm. Any other value or no line: nothing is
//     built, record() and handoff() return at once - byte-identical.
//   - Keys with it: FGFov (deg, default 90), FGNear (0.1), FGFar (10000),
//     FGInvertMV (0/1), FGPace (1 = the generated frame leads by half the
//     measured NR interval; 0 = both back to back, for A/B).
//   - The snippet: the driver's model store (ProgramData\NVIDIA\NGX\models\dlssg)
//     else <add-on dir>\mgpu\nvngx_dlssg.dll. Never a copy beside the exe.
//   - NEVER fatal, never blocking the stream: a failure to come up turns the key
//     off for the arm, loudly; three failed evaluates do the same. A set whose
//     last present has not retired is skipped (counted), never waited for.
//   - Three buffer sets {real, gen}: set k is written by record() for NR frame N
//     while the present thread may still read set k-1 or k-2 (FIX7/FIX8 lesson:
//     the set is reusable only when the present fence passed its last present).
//   - WHILE UP, EVERY FRAME IS PRESENTED BY THE PACING THREAD (with or without a
//     generated frame), so present_frame is never entered from two threads.
//     Off, the bridge thread presents as today.
//   - Threads: arm()/record()/release() on the bridge thread under s.cs;
//     handoff() on the bridge thread after the present gate (no stream lock
//     needed: it reads the module's own state); the pacing thread calls
//     mgpu::gpu1::present_frame with the source override and reads its fence.
//   - Instrument: one [R341] line at start, then every 1200 handoffs: generated,
//     presented (gen / real), skipped for a busy set, skipped for no vectors,
//     evaluate errors, mean half interval. PresentMon on the bridge window shows
//     the doubled present rate.

#include <windows.h>
#include <d3d12.h>

namespace mgpu::fg
{
struct arm_info
{
    ID3D12Device *ndev = nullptr;        // the neural device (borrowed)
    unsigned w = 0, h = 0;               // the NR output extent
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;   // the NR output format (linear twin)
    unsigned mvec_w = 0, mvec_h = 0;     // the game's vector extent (0 = unknown -> w,h)
    unsigned depth_w = 0, depth_h = 0;   // the depth extent (0 = unknown -> w,h)
    bool depth_inverted = true;
    float mvec_scale_x = 1.0f, mvec_scale_y = 1.0f;
    // [FFX, 2026-11-11] FgMvUv: 0 = the shipped pixel-unit conversion (lane_scale/extent),
    // 1 = the game's vectors are UV and DLSS-G wants the frame fraction, 2 = UV x2 (NDC).
    // This is SEPARATE from the NR scale on purpose: NR wants UV->pixels, DLSS-G wants the
    // raw vector already divided by the frame extent, and one number cannot serve both.
    int fg_uv = 0;
};

// At arm, after the stream's textures exist. Reads the key. True when FrameGen=4 is on.
bool arm(const arm_info &i);

// One relaxed load.
bool on();

// Bridge thread, in the consume list, after nr_final is final (UNORDERED_ACCESS) and
// before the mvec/depth textures go back to their resting state. `mvec` / `depth`
// are the frame's bound textures in NON_PIXEL_SHADER_RESOURCE or nullptr when not
// bound this frame. Copies nr_final into a set and evaluates the in-between frame.
void record(ID3D12GraphicsCommandList *cl, ID3D12Resource *nr_final,
            ID3D12Resource *mvec, ID3D12Resource *depth, float mv_el, unsigned long long f);

// Bridge thread, where present_frame would be called. True: the module presents this
// frame (the caller must NOT call present_frame). False: the caller presents as today.
bool handoff(float r, float g, float b);

// Bridge thread, BEFORE s.cs is taken on the way to stream_release (R341a, review F4): stops and
// joins the pacing thread. The thread's present_frame takes s.cs through stream_present_source,
// so joining it under s.cs would deadlock. Idempotent; a no-op when the thread never started.
void stop();

// Bridge thread, in stream_release BEFORE the stream's textures are freed (under s.cs is fine
// once stop() ran): drains the present queue (bounded), releases the feature, frees the sets.
void release();

// [PORT FG-PANEL, 2026-11-11] the HOT SWITCH. Off skips generation only: the
// real frame is still copied and presented by the same pacing thread, the
// feature is kept, and on resumes with a fresh pair. Anything read at arm time
// (FrameGen / FgBackend / FgMulti) still needs a restart.
void ui_set_enabled(bool on);

// [PORT C] the during-play readout (layered GDI window, outside the render path).
void ui_set_hud(bool on);
bool ui_hud();
bool ui_up();   // the feature is created and armed right now (read-only)
bool ui_enabled();

// [PORT FG-PANEL, 2026-11-11] READ-ONLY counters for the panel / overlay: the
// same numbers [R341] prints, so panel and log cannot disagree. Plain loads -
// this is a display, no decision is taken on a possibly torn value.
// [PORT DISPLAY-LAYER 1/3] landing ledger: mean/worst late-ness in ms, and how many
// presents were more than 8 ms late. Read-only; the pacing thread is still the only
// present caller.
void ui_landing(double &mean, double &worst, unsigned long long &n, unsigned long long &late);

void ui_pace(double &interval_ms, unsigned long long &resync, unsigned long long &abs_frames, int &mode);

void ui_status(unsigned long long &gen_made, unsigned long long &gen_presented,
               unsigned long long &real_presented, unsigned long long &dropped,
               unsigned long long &errors);
}
