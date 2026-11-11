// MGPU Bridge - the presenter: the layer that decides WHAT reaches the monitor and WHEN.
//
// WHY THIS IS NOT PART OF present_frame ANY MORE
//
// One neural frame reaches the screen through one Present today, and that is correct
// for neural rendering. It is not correct for frame generation: N pictures need N
// presents in N different vblanks, and two Present calls issued back to back are one
// displayed frame - measured on this rig as [R333] 120.0 with interval p50 14.75-15.19 ms
// where 120 fps requires 8.33. The presenter owns a window, a flip chain, a ring of
// slots and its own thread, so a generator can hand it N+1 pictures for one source frame
// and they land in N+1 vblanks. See docs/FRAMEGEN.md.
//
// WHAT IT NEEDS FROM THE REST OF THE ADD-ON
//
// Two things only: the D3D12 device and the command queue the bridge already submits its
// frame work on (the ring copy is recorded into a per-slot list and executed on THAT
// queue, so the slot's contents are ordered with the frame that produced them, and the
// display queue waits on it on the GPU rather than the CPU). Everything else here is
// self-contained: no stream state, no NGX types, no vendor types.
//
// THE XeSS-FG SLICE PLUGS IN THROUGH `hooks`
//
// The frame generator that owns the chain (XeSS-FG) needs a handover at init time and
// four per-present hooks. They live behind a function table instead of direct calls so
// this file compiles and runs with no vendor dependency at all; src/xefg.cpp fills the
// table when that slice is built. A null table means "no provider": every hook is a no-op
// and the presenter is a plain double-buffered flip-chain presenter.
//
// Every setting is a key, and every key is absent-by-default: with no key set the add-on
// presents exactly as it does without this file.
#pragma once

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdint>

namespace mgpu
{
namespace presenter
{
// ---------------------------------------------------------------------------
// What a frame generator needs to know about the frame being presented
// ---------------------------------------------------------------------------

// The two guide textures for THIS frame, copied into the ring slot beside the colour so
// the tag describes the frame that is about to be shown rather than whatever the
// renderer moved on to (a live texture here is a stale depth map, and the symptom is one
// frame of wrong motion every so often - the kind of bug that gets blamed on the
// generator for a week).
struct disp_fg_in
{
    ID3D12Resource *depth = nullptr;
    D3D12_RESOURCE_STATES depth_state = D3D12_RESOURCE_STATE_COMMON;
    ID3D12Resource *mvec = nullptr;
    D3D12_RESOURCE_STATES mvec_state = D3D12_RESOURCE_STATE_COMMON;
};

// Everything else the generator needs that is not in those two textures.
struct disp_fg_params
{
    float jitter_x = 0.0f;
    float jitter_y = 0.0f;
    float mv_scale_x = 1.0f;   // same convention the bridge hands DLSS
    float mv_scale_y = 1.0f;
    float frame_ms = 0.0f;     // how long the source frame took
    unsigned reset = 0;        // 1 on the frames after a start or a discontinuity
};

// ---------------------------------------------------------------------------
// The provider hooks (XeSS-FG / any other generator that owns the chain)
// ---------------------------------------------------------------------------
struct hooks
{
    // Asked once, before anything is created: does this session want the chain handed
    // to the provider? A window's flip chain is created once and DXGI allows one per
    // HWND, so the decision has to be made before disp_init creates anything.
    bool (*handover_wanted)() = nullptr;

    // Create the chain through the provider instead of us (it becomes the window's only
    // one) and return the proxy we present into.
    bool (*display_handover)(ID3D12Device *dev, ID3D12CommandQueue *q, HWND hwnd,
                             const DXGI_SWAP_CHAIN_DESC1 *scd, IDXGIFactory2 *factory,
                             IDXGISwapChain1 **out_sc, IDXGISwapChain3 **out_sc3,
                             const char *where) = nullptr;

    // Per present. gen_tag returns 0 when the frame's guide textures were tagged;
    // gen_active() is false whenever generation is off, so the display thread never has
    // to ask whether to call the rest.
    bool (*gen_active)() = nullptr;
    int  (*gen_tag)(ID3D12CommandList *cl, unsigned present_id, const disp_fg_params *fc,
                    ID3D12Resource *depth, ID3D12Resource *mvec) = nullptr;
    void (*gen_set_present_id)(unsigned present_id) = nullptr;
    void (*gen_after_present)(unsigned *frames_presented, int *gen_result,
                              unsigned *gen_enabled) = nullptr;

    // The provider's own pacer (XeLL's markers). No-ops when pacing is off.
    void (*pace_present_begin)(unsigned frame_id) = nullptr;
    void (*pace_present_end)(unsigned frame_id) = nullptr;
    void (*pace_sim_begin)(unsigned frame_id) = nullptr;
    void (*pace_sim_end)(unsigned frame_id) = nullptr;
    void (*pace_submit_begin)(unsigned frame_id) = nullptr;
    void (*pace_submit_end)(unsigned frame_id) = nullptr;
    void (*pace_report)() = nullptr;

    // Applies the panel's backend/multiplier to the provider. Two integer compares when
    // nothing changed, so it is safe to call every iteration.
    void (*apply_mode)() = nullptr;
};

// Filled by the XeSS-FG slice; safe to call with a zeroed table (or not at all).
void set_hooks(const hooks &h);

// ---------------------------------------------------------------------------
// The presenter
// ---------------------------------------------------------------------------

// FgDisp. 1 = this layer owns the output. Absent or 0 = today's single Present.
bool enabled();
// FgDisp=2: the layer runs in shadow - copy and present on a hidden window, the picture on
// screen stays the composition chain's. The instruments in A0-1 need exactly that.
bool shadow_only();

// Creates the window, the queue and the chain. `dev` and `bq` are the bridge's own
// device and queue (the ring copy is ordered on `bq`). Returns false if the layer cannot
// start, in which case the caller keeps presenting the old way for the whole session -
// a layer that fails to start is a frame cost, not a broken picture.
bool disp_init(ID3D12Device *dev, ID3D12CommandQueue *bq, UINT w, UINT h, DXGI_FORMAT fmt);

// Never blocks, never allocates, never waits on a fence: records the copy of `src` into
// a free ring slot on `bq` and returns. A full ring drops the frame and counts it - the
// game's present thread must keep running no matter what.
//
// `grp` is the burst id: the N+1 pictures that one source frame produces share it, and
// the display thread treats an id as indivisible (it never presents a burst with a hole
// in it, and never drops one sibling out of a burst). 0 = a burst of its own.
bool disp_push(ID3D12Resource *src, D3D12_RESOURCE_STATES src_state, double t_ms,
               int tag_color = -1, int tag_code = 0, int patch_phase = -1,
               double max_late_ms = 25.0, const disp_fg_in *fg = nullptr,
               const disp_fg_params *fpp = nullptr, unsigned grp = 0,
               bool is_gen = false);   // R526: true = the GENERATOR produced this slot

// R532: FgPace=0 turns the R530 pacing off (present as soon as ready).
void     disp_set_pace(int on);
int      disp_pace();
unsigned disp_burst_id();      // a fresh burst id, for one source frame and its siblings
bool     disp_alive();
bool     disp_on_screen();      // P22: OUR window is the one showing (alive, not parked, no panel)
void     disp_stop();
// F4 (2026-11-02): teardown signals for the display thread.
//
// The game exits while this layer is presenting; if the thread is inside a blocking
// Present(1,0) when the window/chain goes, the process never terminates and is left as
// a 4.8 GB / one-thread zombie (measured 06:55, 07:07). disp_note_teardown() marks the
// next few seconds as "do not block in Present" (the thread uses DXGI_PRESENT_DO_NOT_WAIT
// and keeps checking); disp_begin_dying() tells it to drain its queue and leave, which is
// the only way the process can exit cleanly without joining from DllMain.
void     disp_note_teardown();
void     disp_begin_dying();
// B (2026-11-02): the zombie watchdog's terminate limit. Only the game's top-level window
// being destroyed starts the watchdog, and it never terminates anything while this is 0 -
// which is the default, and the value every other title keeps. A title whose process is
// known to hang on exit (Phantom Blade Zero measured 2026-11-02) opts in from its own
// mgpu.ini with ZombieKillMs=10000.
void     disp_set_zombie_kill_ms(int ms);
// A (2026-11-02): DllMain's unload path asks this; true means "TerminateProcess now". It
// requires ZombieKillMs > 0, the game's top-level window gone and no frame for a second.
bool     disp_zombie_kill_now();
bool     disp_owns_swapchain(void *native);   // true only for this layer's own chain
void     disp_set_panel(bool hide);        // asks the display thread to hide the output and drop the queue
void     disp_set_panel_live(bool on);     // the panel is drawn on the neural picture
// ---- B1.2 (2026-11-07): THE PANEL IS THE PRESENTER'S OWN ----
//
// When the panel is drawn into this layer's backbuffer (PanelInPresenter=1 and b1_imgui.dll
// loaded), opening it must NOT hand the screen back. The handover is what P24 measured:
// the composition chain copies 4K and Presents into the same monitor the game chain is
// competing for, and the on-screen rate dropped 150 -> 132/s. These two calls are how the
// rest of the add-on reaches the panel without touching that handover: set is a request
// (any thread), live is the state the display thread has actually taken.
void     disp_b1_panel_set(bool up);
bool     disp_b1_panel_live();
// True while the panel is up. The presenter then stops owning the output for the duration:
// the frame goes out on the composition chain again, which is the chain the GPU 1 runtime
// draws the panel into - that is how the panel ends up over the live picture instead of
// being painted on a chain nobody presents. Same mechanism the measured build used.
bool     panel_live();
double   disp_now_ms();
// A1d: ms since the last frame this layer accepted. Large = up but not being fed.
double   disp_since_last_ms();
double   disp_source_period_ms();          // measured, 0 until enough frames
// >>> R585 vblank phase alignment -- inserted by apply_r585.py P2 accessor decl
double   disp_vblank_ms();
double   disp_vblank_ref_ms();   // R585: a known vblank time in our QPC ms, -1 = unknown
// <<< R585
// 1 = the last measurement window says the panel could not hold the multiplier that is
// running (late bursts, not a low average rate). Consumed once; the caller decides what to
// do with it and says so out loud. FgBackoff.
unsigned disp_backoff_take();
}   // namespace presenter
}   // namespace mgpu
