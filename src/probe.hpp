#pragma once

// MGPU Bridge - P9.1 lateral acquisition probe
//
// WHAT THIS IS FOR
//
// Two of DLSS-NR's four required buffers have never been fed: Depth has never
// been bound at all, and MVec only as a constant synthetic control field. The
// question that decides everything downstream is not "how do we transport
// them" - it is "can we see them from where we already stand", and that
// question has never been asked of the ReShade API itself.
//
// It is asked here, by subscribing to events this add-on has never used, and
// answered by a log line rather than by an argument. Nothing is transported,
// nothing is bound, nothing reaches GPU 1. This file only watches, counts and
// reports. That is deliberate: the alternative acquisition routes (hooking the
// game's NGX evaluate, intercepting Streamline's tag submission) both turn
// this add-on from a subscriber into a hook, and that cost has to be earned by
// evidence that the cheap route cannot work.
//
// TWO LATERALS, INDEPENDENT BY CONSTRUCTION
//
//   DEPTH  init_resource catalogues every depth-stencil texture the game
//          creates. bind_render_targets_and_depth_stencil and
//          clear_depth_stencil_view then count how often each one is actually
//          used. The most-bound, most-cleared candidate at scene resolution is
//          the scene depth. This is the heuristic ReShade's own generic_depth
//          add-on ships, which is why it is the one being used rather than one
//          invented here.
//
//   MVEC   init_resource catalogues every two-channel float texture in a
//          plausible size band. init_resource_view counts shader resource
//          views created over each. push_descriptors, when the engine binds
//          through root descriptors rather than descriptor tables, adds a
//          third signal. None of these IDENTIFIES motion vectors - nothing in
//          ReShade says "this one is motion vectors" - they establish whether
//          an MVec-shaped resource is visible at all. A candidate list with
//          nothing in it is the finding that kills the whole programme, and it
//          costs one run to get.
//
// COST IS PART OF THE RESULT, NOT A FOOTNOTE
//
// Every callback is timed with QueryPerformanceCounter and the total is
// reported per frame. Subscribing to a per-bind event makes ReShade route the
// game's command lists through its interception path for the whole process, so
// this is not free and pretending otherwise would defeat the point. If the
// depth lateral costs 0.3 ms of game-thread time per frame, that number
// belongs next to whatever it bought.
//
// OFF MEANS UNREGISTERED, NOT EARLY-RETURN
//
// A callback that is registered and returns immediately still costs a call
// through ReShade's dispatch for every draw in the game. So the toggle
// registers and unregisters the events themselves. With the probe off this
// translation unit is inert and the add-on behaves exactly as it did before
// it existed - which is the property that makes it safe to ship on by default
// in a prototype and off by default in anything else.
//
// NOTHING HERE TOUCHES gpu1_context. That file holds no ReShade types by
// design and this one holds nothing else, so the two cannot drift into each
// other. The bridge, the ring, the seal and the pass chain are untouched.

#include <cstdint>

namespace mgpu
{
namespace probe
{

enum class mode : int
{
    off   = 0,
    depth = 1,
    mvec  = 2,
    both  = 3,
};

// Registers or unregisters the event subscriptions to match. Safe to call
// repeatedly with the same value (it does nothing). MUST be called on a thread
// where ReShade event registration is legal - the game's render thread via the
// overlay, or DllMain's attach - and never from the bridge thread.
void set_mode(mode m);
mode get_mode();

// Read once at DLL attach from mgpu.ini's Probe= key. Separate from set_mode
// so the ini is parsed in exactly one place.
mode mode_from_ini();

// R101. The NGX tap's mode, from mgpu.ini's Calib= key. Parsed HERE, in the
// one place that already owns the ini, so no second file can disagree with
// this one about what the user asked for. 0 off, 1 latch, 2 live.
//
// R110. The return value is PACKED and the low byte is unchanged:
//
//     bits 0-7   capture mode, from Calib=      0 off, 1 latch, 2 live
//     bits 8-11  install rung, from CalibRung=  0 both, 1 IAT, 2 data-scan
//     bits 12-15 probe depth, from CalibProbe= 0 off, 1 on-disk reference,
//                                              2 also a private-copy write
//
// Calib says what the tap does once NGX is running. CalibRung says which of
// the two install rungs is allowed to run at all, which is a different
// question and was never answerable before: both rungs ran on every non-zero
// Calib, so Calib=1 and Calib=2 installed identically. With CalibRung absent
// the top bits are zero and this returns exactly what 0.2.1 returned.
//
// mgpu::calibrator::install() is the only consumer and unpacks it there.
int calib_mode();

// R103. Force the transport's source handle. The calibrator reads the
// resource the game itself hands DLSS; when it has one, it is authoritative
// and this overrides whatever this file's ranking picked. Zero is ignored,
// so a title where the calibrator never resolved keeps the ranked pick with
// no special case anywhere.
void set_mvec_override(unsigned long long handle);

// R104. JitterComp (0 off, 1 on) and InvertJitter (1 flips the sign).
// Returns 0, 1 or -1 already combined, so no caller repeats the logic.
int jitter_mode();

// R106. MvecFromEval: 0 barrier-triggered copy (current, default), 1 copy from
// the DLSS evaluate call instead - once per frame, guaranteed.
int eval_copy_mode();

// R180/R182. The Starfield path key: -2 absent (AUTO), 0 off, 1 on.
int sf_path_mode();

// Called from on_reshade_finish_effects on the GAME runtime only. Ticks the
// frame counter and dumps the table every ProbeLogSeconds. Doing the dump here
// rather than from a callback keeps every logging call off the hot path.
void note_frame();

// P11.0 (R48). The GAME effect_runtime, handed over from the same callback,
// immediately BEFORE note_frame so the enumeration dump() runs is of the frame
// being closed. Passed as void * on purpose: this header holds no ReShade
// types, by the same rule that keeps gpu1_context free of them, and the cast
// back happens in one place in probe.cpp against the exact type passed in.
//
// This is arm C of the depth A/B/C comparison. It reads what ReShade's own
// depth detection has BOUND to every texture variable declared ': DEPTH' and
// reports the resource behind it. Nothing is bound, nothing is transported,
// and it costs one enumeration per log dump rather than anything per frame.
//
// P12.0 (R55) adds the COMMAND LIST. With DepthCompare=3 the probe records a
// small copy from ReShade's own depth resource on it, to prove the transition
// and the copy are legal before any of that reaches the transport. Both
// pointers are void * for the same reason: this header holds no ReShade types.
// Pass reshade::api::command_list *, NOT its get_native() - the probe needs
// ReShade's barrier as well as the raw list underneath it.
void note_effects(void *effect_runtime_ptr, void *command_list_ptr);

// P12.2 (R63). THIS FRAME's depth source for the transport, as a raw handle,
// or 0 when ReShade has nothing bound this frame. That zero IS the seal's
// depth_valid: R61 measured that ReShade unbinds depth in menus and scene
// changes, and that the tap then writes a plane of zeros byte-identical to a
// legitimate far-plane reading. Shipping those would hand the model a false
// flat world every time a menu opens.
//
// Resolved once and read every frame - three virtual calls, no string work,
// and no dependence on the probe's own log cadence. Valid whatever MetaProbe
// is set to: the transport must not silently require the probe to be on.
unsigned long long depth_source();

// ---- R145: THE LIVE STATE OF THE DEPTH TAP'S TECHNIQUE ----
//
// tech_scan() already computes this, self-enables the technique when it finds
// it off, and re-runs every 300 frames from depth_source(). This exposes the
// value it maintains. There is no work here - it is a read of an int.
//
//   -2  NO SCAN HAS RUN YET. Not an answer; do not report a fault on it.
//   -1  mgpu_depth_tap.fx was not enumerated on the GAME runtime at all
//    0  enumerated, technique off AND the self-enable did not take
//    1  enumerated and on
//
// WHY IT EXISTS RATHER THAN THE ONE-SHOT IT REPLACES. R142 pushed the tap
// state to the idle screen from log_preset_once, which fires ONCE, at the
// first non-empty enumeration - which is BEFORE tech_scan has self-enabled
// anything. Measured 2026-09-16 on Resonance: [R53] reported TAP = OFF on a
// run where the tap was present, compiled and working, because the snapshot
// was taken a few frames too early and then frozen for the session. A latched
// 0 is a false fault; only this value is safe to show a user.
int tap_state();

// ---- P12.9 (R78): the MVec acquisition handover ----
//
// The velocity buffer is in a KNOWN state at exactly one moment - while it is
// bound as a render target, where D3D12 requires RENDER_TARGET. Only this
// translation unit sees that moment: dllmain does not subscribe to the bind
// event and gpu1_context holds no ReShade types at all.
//
// So the probe issues the barrier itself and then calls a function POINTER
// that dllmain installs. The probe never names a gpu1_context type and
// gpu1_context never names a ReShade one, which is the rule at the top of this
// header honoured rather than bent.

// The identified velocity buffer, or 0 while it is not yet known. Chosen on
// R73's rule: top of the render-target-bind ranking that also sits at the
// DISPLAY extent, which is what the game's own DLSS reports as MVExtent.
// Re-picked every log dump, so an eviction or a resolution change moves it.
unsigned long long mvec_source();

// Installed by dllmain. Called at most ONCE PER FRAME, on the game's render
// thread, with the barrier already issued and restored around it. The callee
// records a copy and nothing else.
void set_mvec_hook(void (*fn)(void *cmd_list_native, unsigned long long resource));

// The presented resolution, from on_init_swapchain. The candidate size band is
// expressed as a fraction of this rather than as absolute pixel counts, so the
// filter follows the user's resolution instead of assuming one. Until it is
// called the band falls back to a loose absolute minimum, which is why it is
// called from the swapchain event and not from the first frame.
void note_scene_size(unsigned int w, unsigned int h);

// Drops every subscription. Called at DLL detach.
void shutdown();

// ---- the overlay readout ----
//
// A snapshot, copied under the lock, so the panel never walks a table another
// thread is writing.

struct entry
{
    unsigned long long handle;
    unsigned int       width;
    unsigned int       height;
    unsigned int       format;
    unsigned long long binds;   // depth: OMSetRenderTargets. mvec: SRVs created.
    unsigned long long clears;  // depth: ClearDepthStencilView. mvec: descriptor pushes.
};

struct readout
{
    mode  m;
    bool  game_device_found;
    unsigned long long frames;

    // Cost, in nanoseconds of game-thread time per frame, averaged over the
    // whole run. The number that says whether this route is affordable.
    double depth_ns_per_frame;
    double mvec_ns_per_frame;

    unsigned int  depth_n;
    unsigned int  mvec_n;
    entry         depth_top[8];
    entry         mvec_top[8];

    // How many resources were examined in total. Distinguishes "this game has
    // no candidate" from "the subscription is not firing", which are the same
    // empty table and completely different problems. Borrowed, with thanks,
    // from OptiScaler's exposure scan, which learned it the hard way.
    unsigned long long examined;
};

void read(readout &out);

} // namespace probe
} // namespace mgpu
