// MGPU Bridge - P9.1 lateral acquisition probe. See probe.hpp for why.
//
// READ THIS BEFORE CHANGING THE SUBSCRIPTION LIST.
//
// ReShade installs its command-list interception for the WHOLE PROCESS when
// any add-on subscribes to a command-list event. Subscribing to draw or
// draw_indexed therefore puts an indirection on every draw call in the game,
// for every add-on, for as long as we are subscribed. That is why the depth
// lateral counts BINDS and CLEARS rather than draws: OMSetRenderTargets fires
// on the order of hundreds of times per frame where draws fire on the order of
// thousands, and a ranking is all that is needed to say which depth-stencil is
// the scene's. If the ranking comes back ambiguous, adding draw attribution is
// the next step and it is a deliberate one, not a default.
//
// The MVec lateral subscribes to nothing warm at all. init_resource and
// init_resource_view fire at creation, which in a shipped game is a startup
// burst and then almost nothing. push_descriptors is the one warm subscription
// and it is included because a D3D12 engine that binds through ROOT
// descriptors will show us the resource directly; an engine that binds through
// descriptor TABLES will not, and that difference is itself a finding worth
// having in the log rather than a gap to be surprised by later.

#include <windows.h>
#include <d3d12.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <mutex>

#include <reshade.hpp>

#include "adapter.hpp"
#include "diag.hpp"
#include "mgpu_ini_parser.hpp"
#include "probe.hpp"
#include "sl_probe.hpp"   // SL1
#include "sl_tags.hpp"    // SLT1

namespace mgpu
{
namespace probe
{
namespace
{

// ---------------------------------------------------------------------------
// Tables
// ---------------------------------------------------------------------------
//
// Fixed size, no allocation, no growth. A game that creates more candidates of
// one kind than the table holds is telling us the filter is too loose, and the
// log says so rather than the table quietly eating render-thread time.

// THE BUILD TAG. It goes in the announce line so that "which probe build
// produced this log" is one grep instead of a round trip - which is exactly
// what it cost once already: a run came back with cadence lines and no clear
// shape lines, and nothing in the log could say whether the new lateral was
// absent, off, or silently failing. BUMP IT WITH EVERY CHANGE TO THIS FILE.
#define PROBE_BUILD "P14.2 (R98: a UNIFORM FRACTION of the scene, and the largest one wins)"

const unsigned MAX_CAND = 64;
const unsigned MAX_DSV  = 128;

struct cand
{
    unsigned long long handle;   // resource handle, the identity
    unsigned int width;
    unsigned int height;
    unsigned int format;
    std::atomic<unsigned long long> a;  // depth: binds.  mvec: SRVs created.
    std::atomic<unsigned long long> b;  // depth: clears. mvec: descriptor pushes.

    // ---- R82: THE OTHER TWO SIGNALS, COUNTED BESIDE THE FIRST ----
    //
    // R80/R81: b stayed at zero for every candidate across two runs and 17
    // live resources, and the log could not say whether that meant the event
    // never fired, the view table never matched, or the engine does not bind
    // velocity this way at all. Three counters answer that in one run.
    std::atomic<unsigned long long> c;  // mvec: RTV binds resolved through the
                                        //       DEVICE (get_resource_from_view)
                                        //       rather than through our table
    std::atomic<unsigned long long> d;  // mvec: BeginRenderPass render targets
    std::atomic<unsigned long long> e;  // mvec: appearances in a barrier
    std::atomic<unsigned int> last_old; // mvec: last barrier's before-state
    std::atomic<unsigned int> last_new; // mvec: last barrier's after-state
};

struct lane
{
    cand t[MAX_CAND];
    std::atomic<unsigned int> n;
    std::atomic<unsigned int> mru;               // last index that matched
    std::atomic<unsigned long long> overflow;
    std::atomic<unsigned long long> ns;          // game-thread nanoseconds spent
};

lane g_depth;
lane g_mvec;

// A depth-stencil VIEW handle maps to a resource handle. Resolving that
// mapping needs a device call, which we refuse to make on a warm path, so the
// mapping is built at init_resource_view (cold) and read at bind time (warm)
// as a plain array scan.
struct dsv_map
{
    unsigned long long view;
    unsigned long long res;
};

dsv_map g_dsv[MAX_DSV];
std::atomic<unsigned int> g_dsv_n;

// ---- R71: the RENDER TARGET view map, and why the mvec rank was wrong ----
//
// R25 ranked mvec candidates by SRVs CREATED over them and got 6/5/3/3 - flat,
// and it called the result "a candidate, not an identification". The rank was
// flat because it was the wrong signal: an SRV creation count is a LIFETIME
// number that says something read the resource once, not how the engine uses
// it per frame.
//
// A velocity buffer is RENDERED INTO, exactly once per frame, in the velocity
// pass. Counting RENDER TARGET binds is therefore a per-frame usage signal
// against a per-lifetime creation signal, and this add-on is ALREADY
// subscribed to the event that carries it - the depth lane has used
// bind_render_targets_and_depth_stencil since P9.1 and has thrown the RTV
// array away every time.
//
// Free in subscription, free in risk: nothing is copied, no state is
// transitioned, no resource of the game's is touched.
dsv_map g_rtv[MAX_DSV];
std::atomic<unsigned int> g_rtv_n;
std::atomic<unsigned long long> g_rtv_binds_seen;
std::atomic<unsigned long long> g_evicted;   // R76: resources destroyed and removed

// ---- R82: THREE ACQUISITION SIGNALS, AND THE DENOMINATOR R80 COULD NOT SEE ----
//
// R81 read the pinned ReShade headers and found three things this probe had
// been doing the hard way:
//
//   1. device::get_resource_from_view(view) EXISTS. on_bind_rtv_dsv is handed
//      views and can ask the device what each one belongs to, on the spot. Our
//      g_rtv table - built from init_resource_view, and therefore empty for
//      everything created while the lane was off - was never necessary.
//
//   2. begin_render_pass is a DIFFERENT EVENT. bind_render_targets_and_depth_
//      stencil is OMSetRenderTargets; begin_render_pass is
//      ID3D12GraphicsCommandList4::BeginRenderPass. An engine that uses render
//      passes raises the second and never the first, and zero binds beside 17
//      live candidates is exactly that shape.
//
//   3. barrier NAMES RESOURCES DIRECTLY - no views, no descriptor tables, no
//      assumption about how the engine binds. A velocity buffer is written as
//      a render target and read as a shader resource every frame, so it
//      transitions once per frame whatever API wrote it.
//
// All three are counted here, in one run, so the next decision is made from
// data instead of from R71's sentence about optical flow.
std::atomic<unsigned long long> g_bind_fires;      // OMSetRenderTargets, at all
std::atomic<unsigned long long> g_bind_rtvs;       // non-zero RTVs in them
std::atomic<unsigned long long> g_bind_via_table;  // matched through g_rtv
std::atomic<unsigned long long> g_bind_via_api;    // matched through the device
std::atomic<unsigned long long> g_rp_fires;        // BeginRenderPass, at all
std::atomic<unsigned long long> g_rp_rts;          // render targets in them
std::atomic<unsigned long long> g_rp_matched;      // resolving to a candidate
std::atomic<unsigned long long> g_bar_fires;       // ResourceBarrier, at all
std::atomic<unsigned long long> g_bar_res;         // resources named in them
std::atomic<unsigned long long> g_bar_matched;     // matching a candidate
std::atomic<unsigned long long> g_bar_ns;          // what barrier costs us

// ---- R78: THE MVEC ACQUISITION HOOK ----
//
// The velocity buffer is only in a KNOWN STATE at one moment: while it is bound
// as a render target, where D3D12 requires RENDER_TARGET. That moment is
// visible to this file and to nothing else - dllmain does not subscribe to the
// bind event and gpu1_context holds no ReShade types at all.
//
// So the probe hands the moment over rather than acting on it. It issues the
// barrier itself, because a barrier is a ReShade type and this file is allowed
// to hold those, and then calls a FUNCTION POINTER that dllmain installs. The
// probe learns nothing about gpu1_context and gpu1_context learns nothing about
// ReShade - which is the rule probe.hpp states, honoured rather than bent.
//
// ONE COPY PER FRAME. R73 measured 0.56 render-target binds per frame on this
// buffer, so a second bind in the same frame would overwrite a payload the
// producer may already have sealed.
typedef void (*mvec_hook_fn)(void *cmd_list_native, unsigned long long resource);
mvec_hook_fn g_mvec_hook = nullptr;
std::atomic<unsigned long long> g_mvec_src;        // the identified velocity buffer
std::atomic<unsigned long long> g_mvec_copies;     // handed over
std::atomic<unsigned long long> g_mvec_skips;      // already done this frame
unsigned long long g_mvec_last_frame = ~0ull;

// The semantic lane's chosen depth resource. Declared HERE rather than beside
// the rest of the semantic lane because on_destroy_resource must clear it, and
// that runs far earlier in this file. R76.
std::atomic<unsigned long long> g_semC_res;   // handle arm C actually copies
std::atomic<unsigned> g_semC_w, g_semC_h;

std::mutex g_table_lock;   // cold paths only (creation), never held at bind

std::atomic<unsigned long long> g_examined;
std::atomic<unsigned long long> g_frames;

// R101. The NGX tap's mode. Default 2 (live) because a latched NGX resource
// handle dies with the next resolution or DLSS preset change, and that is the
// exact failure this tap exists to remove.
std::atomic<int> g_calib{2};

// R110. WHICH RUNG OF THE CALIBRATOR IS ALLOWED TO RUN.
//
// The calibrator installs itself twice over: an import-table swap
// (R101) and a data-section scan for pointers cached before we existed
// (R102). Until now both ran unconditionally, in sequence, on every
// value of Calib - which is why Calib=1 and Calib=2 produced a
// byte-identical install and why a title that dies inside install
// cannot tell us which of the two killed it. Calib selects what
// happens at EVALUATE time; it never selected what happens at INSTALL
// time. This key does.
//
// 0 both, in the existing order. The default, and byte-identical to
//   0.2.1 when the key is absent.
// 1 the import swap alone.
// 2 the data-section scan alone.
std::atomic<int> g_calib_rung{0};

// R115. CalibProbe: what the calibrator says about the words its data scan
// wrote. 0 off, 1 the on-disk reference for every hit, 2 also loads a private
// copy of the module under mgpu\ and writes to it. Diagnostic only - it
// changes nothing about what is patched.
std::atomic<int> g_calib_probe{0};

std::atomic<int> g_sfpath{-2};   // R180/R182: see the read below

// SLT1. The Streamline tag tap's mode, from mgpu.ini's SLTags= key. Parsed
// here with every other key, and OFF by default: this one installs an import
// hook, and an instrument that hooks must be asked for.
std::atomic<int> g_sltags{0};
std::atomic<int> g_jitter{0};   // R104: 0 off, 1 apply, -1 apply negated
std::atomic<int> g_evalcopy{0}; // R106: 0 barrier trigger, 1 evaluate trigger

// How many shader-resource descriptors the engine pushed through ROOT
// descriptors. Not attributed to any candidate on purpose - see
// on_push_descriptors for why a number we cannot attribute is reported on its
// own line rather than added to a table it does not belong in.
std::atomic<unsigned long long> g_root_srv_pushes;
std::atomic<int>  g_mode;
std::atomic<bool> g_depth_on;
std::atomic<bool> g_mvec_on;

// The game's ReShade device. Captured lazily the first time a device is seen
// whose adapter LUID matches the one adapter selection resolved from the
// swapchain, then compared by POINTER thereafter - one compare on the warm
// path instead of a GetAdapterLuid call.
//
// This filter is not optional. Events fire for BOTH runtimes in this process,
// and the bridge runtime's device is ours on GPU 1. Without the filter the
// tables would fill with our own tex_in, tex_out and ring staging textures,
// which look exactly like candidates and are not.
std::atomic<void *> g_game_dev;

double g_qpc_to_ns = 0.0;
unsigned int g_log_seconds = 10;
unsigned long long g_last_dump_qpc = 0;
unsigned int g_scene_w = 0, g_scene_h = 0;

// ---------------------------------------------------------------------------
// R30b - THE CLEAR CADENCE LATERAL
// ---------------------------------------------------------------------------
//
// Depth can only be copied out of a game where its STATE IS KNOWN, and there
// is exactly one such place: ClearDepthStencilView, because D3D12 REQUIRES the
// resource to be in DEPTH_WRITE for a clear. That is a contract, not an
// assumption, and it is where ReShade's own generic_depth copies.
//
// Everything about the acquisition design then turns on three facts about that
// clear that NOBODY HAS MEASURED:
//
//   1. HOW MANY per frame? One makes the clear a reliable per-frame tick. Two
//      means a design that signals there would signal twice - a producer
//      frame-index desync, which is the worst failure this pipeline has.
//      Zero, in menus or cutscenes, means a consumer waiting for a frame that
//      is complete and unannounced.
//   2. WHERE in the frame? The gap from reshade_finish_effects to the NEXT
//      clear is exactly how long the fence signal would be delayed if the
//      signal moved there.
//   3. HOW STABLE is that gap? Its VARIANCE is jitter injected straight into
//      the production clock - and R25a establishes that this pipeline has ONE
//      clock, that it is production, and that its regularity is why the output
//      is stable.
//
// THIS LANE IS WATCH-ONLY like the rest of the file. Nothing is copied, no
// state is changed, no barrier is issued. It costs one atomic load and a
// compare on the clear path, inside the timing bracket that was already there,
// so its cost is already inside the ns/frame figure the depth lane reports.
// ATOMIC, and not for tidiness: clear_depth_stencil_view fires on whichever
// thread recorded that command list, which on a deferred-context or
// multi-list engine is NOT the thread reshade_finish_effects runs on. Every
// warm datum in this file is atomic for exactly that reason.
std::atomic<unsigned long long> g_top_depth{0};   // handle of the #1 depth candidate
std::atomic<unsigned int> g_clr_in_window;  // clears of #1 since last finish_effects
std::atomic<unsigned long long> g_clr_hist[4];   // 0, 1, 2, 3+ per frame
std::atomic<unsigned long long> g_finish_qpc;    // last reshade_finish_effects
std::atomic<unsigned long long> g_gap_n;
std::atomic<unsigned long long> g_gap_sum_ns;
std::atomic<unsigned long long> g_gap_min_ns{~0ull};   // NOT zero: this is a min
std::atomic<unsigned long long> g_gap_max_ns;

// PER-WINDOW AS WELL AS LIFETIME, and this is not decoration. The counters
// above run from process start, so the MAIN MENU, the loading screens and any
// cutscene before you reach gameplay are permanently mixed into them - and the
// decision rule for this lateral is "ANY weight in the 2-or-more bucket kills
// a clear-anchored design". A startup transient would fire that rule against
// a game whose gameplay clears cleanly once per frame, and the wrong design
// would be rejected on the strength of a menu. So every dump reports THIS
// WINDOW beside the lifetime figure, and gameplay windows can be read on
// their own.
unsigned long long g_prev_hist[4] = {};
unsigned long long g_prev_gap_n = 0, g_prev_gap_sum = 0;
std::atomic<unsigned long long> g_gap_w_min{~0ull};
std::atomic<unsigned long long> g_gap_w_max;

// R31. TWO QUESTIONS THE FIRST CADENCE RUN RAISED AND COULD NOT ANSWER.
//
// 1. The #1 candidate is cleared EXACTLY TWICE or not at all - never once, in
//    4578 frames. The leading explanation is that those are not two depth
//    clears but ONE DEPTH CLEAR AND ONE STENCIL CLEAR on the same
//    R32G8_TYPELESS resource, issued as separate calls. The callback already
//    distinguishes them: ClearDepthStencilView passes a null depth pointer
//    when only stencil is cleared and vice versa, and this file was throwing
//    both pointers away. If that is what it is, depth is cleared ONCE per turn
//    and clear-anchored acquisition is alive after all.
//
// 2. There are FIVE depth textures at the identical scene resolution, and the
//    #1 candidate is cleared on only half the frames. So the engine is
//    rotating a POOL and there is no single "the scene depth". How many
//    DISTINCT pool members get cleared in one frame says how deep the rotation
//    is, which is what any per-frame selection has to cope with.
//
// A mechanism that explains a symptom is not evidence it exists. Both of these
// are observable in one more pass and neither is inferred here.
// R33. THE BIND-TIMING LATERAL - the last thing between us and a design.
//
// R32 established that clear-anchored acquisition is safe and once-per-frame,
// and that it delivers depth TWO FRAMES OLD, because the buffer being cleared
// holds its own previous turn and the pool alternates two deep.
//
// The way out, if it exists: D3D12 requires DEPTH_WRITE or DEPTH_READ for a
// resource BOUND as a depth-stencil target, so every
// bind_render_targets_and_depth_stencil is EXACTLY as state-known as a clear.
// The turn holder is bound ~5.8 times per turn. If one of those binds lands
// LATE - after the scene pass has finished writing depth and before the frame
// ends - then depth can be copied there, state-guaranteed AND current, and the
// two-frame problem disappears.
//
// So: per turn, how many binds follow the depth clear, and WHERE THE LAST ONE
// SITS relative to reshade_finish_effects. A small last-bind-to-finish gap is
// a late bind and the design lives. A gap close to a whole frame period means
// every bind is early, the depth is still being written at all of them, and
// two-frames-stale is the best the clear route can do.
//
// CAVEAT, recorded rather than discovered later: turn tracking assumes the
// clear and the binds of one frame are seen in order. An engine recording
// depth passes on several threads in parallel could interleave them, which
// would show up as an implausibly high bind count or a negative-looking gap.
// The histogram makes that visible rather than silent.
std::atomic<unsigned long long> g_turn_res;
std::atomic<unsigned long long> g_turn_clear_qpc;
std::atomic<unsigned long long> g_turn_last_bind_qpc;
std::atomic<unsigned int> g_turn_binds;
std::atomic<unsigned long long> g_bind_hist[5];      // binds after the clear: 0,1,2,3,4+
std::atomic<unsigned long long> g_lb_n, g_lb_sum;
std::atomic<unsigned long long> g_lb_min{~0ull};
std::atomic<unsigned long long> g_lb_max;
std::atomic<unsigned long long> g_cf_n, g_cf_sum;    // depth clear -> finish_effects
// Depth clears on a KNOWN candidate that the size band refused. A silent zero
// in the bind histogram is indistinguishable from "the game never does this";
// this counter is what tells those apart, and it is here because the first
// version of this lateral produced exactly that silent zero for a whole run.
std::atomic<unsigned long long> g_turn_rejected;

// ---------------------------------------------------------------------------
// R37 - DEPTH COMPARE. The A/B arms, on GPU 0, with no transport at all.
// ---------------------------------------------------------------------------
//
// The question is which acquisition point yields CORRECT depth, and it does not
// need the bridge to answer. Both candidate copies happen on the game's own
// command list, into small readback buffers, and are compared on the CPU.
//
//   A  at the DEPTH CLEAR. State is entailed (D3D12 requires DEPTH_WRITE for a
//      clear) and the callback fires BEFORE the clear is recorded, so the copy
//      captures the buffer's previous contents - which with the two-deep
//      rotation R32 found is the depth of TWO turns ago.
//   B  at the Nth BIND after that clear, N taken from the previous frame's
//      count. A bind is equally state-known - D3D12 requires DEPTH_WRITE or
//      DEPTH_READ for a bound depth-stencil target - so the barrier is as safe
//      as A's. The only guess is WHICH bind, and a wrong guess costs incomplete
//      pixels, NOT undefined behaviour. That is why B is worth running.
//
// WITH THE CAMERA STILL, depth(N) == depth(N-2), so A IS THE CORRECT CURRENT
// DEPTH and B can be diffed against a known-good reference rather than against
// an opinion. A near-zero still-camera difference means B captures COMPLETE
// depth and fresh acquisition is available. A large one means depth is still
// being written at B's bind and two-frames-old is the ceiling, which is what
// R35 concluded from timing alone and could not prove.
//
// MOVING the camera then measures the other half: how far apart a two-frame-old
// depth and a current one actually are, in the only units that matter.
//
// SAMPLED, NOT PER FRAME. One 64x64 patch every SAMPLE_EVERY frames, read back
// READ_AFTER frames later. That gap is why no fence is needed: ten frames is
// far beyond any plausible queue depth, and sampling keeps the cost of putting
// two copies on the GAME's command list down to nothing measurable. The
// readback is deliberately UNSYNCHRONISED and a torn read would show as noise
// rather than as a plausible wrong answer.
const unsigned DC_PATCH   = 64;     // 64x64 texels
const unsigned DC_PITCH   = 256;    // 64 * 4 bytes, already 256-aligned
const unsigned DC_BYTES   = DC_PITCH * DC_PATCH;   // fallback only; the real
                                                  // size comes from the API

bool dc_make_buffers(ID3D12Device *dev, UINT64 bytes);   // defined below
const unsigned long long SAMPLE_EVERY = 30;
const unsigned long long READ_AFTER   = 10;

std::atomic<int> g_dc_mode;                 // 0 off, 1 A only, 2 A and B
std::atomic<void *> g_game_d12;             // ID3D12Device*, the game's
ID3D12Resource *g_dc_rb[3] = {};            // readback A, B, C (C = R55, arm C)
std::atomic<bool> g_dc_ready;
std::atomic<unsigned long long> g_dc_copy_frame;   // frame the pair was recorded
std::atomic<unsigned> g_dc_hits[3];                // did each arm record this round
unsigned g_dc_target_bind = 3;              // which bind B copies at
std::atomic<unsigned long long> g_dc_n, g_dc_zero;
// WHY IT DID NOT FIRE. R34 made a rule of this - a zero is a measurement only
// if the instrument can show it was capable of a non-zero - and the first
// version of R37 broke it one round later: pairs=0 with no way to tell whether
// the buffers failed, arm A never ran, or arm B never matched its bind index.
std::atomic<unsigned long long> g_dc_arm_a, g_dc_arm_b, g_dc_armed;
std::atomic<int> g_dc_buf_hr;
// R42. THE WHOLE FOOTPRINT, FROM THE API. P10.0-P10.4 supplied four values by
// hand - format, row pitch, width/height, byte count - and got three of them
// wrong in turn. GetCopyableFootprints on a 64x64 version of the source's own
// desc returns all four, correct for whatever plane 0 of that format actually
// is. Nothing about the layout is assumed any more, including its SIZE, which
// is why the buffers are created here rather than at a compile-time constant.
D3D12_PLACED_SUBRESOURCE_FOOTPRINT g_dc_fp{};
UINT64 g_dc_total = 0;
std::atomic<bool> g_dc_fp_ok;
double g_dc_sum = 0.0, g_dc_max = 0.0;

// One copy of a 64x64 patch out of a depth resource, on the game's list, with
// the barrier its state contract entails. RAW D3D12 on the native list, the
// same way the producer's colour copy has always been done - the add-on API
// would only be a different spelling of the same two transitions.
// Takes a SLOT, not a destination pointer. P10.5 took the pointer and the
// callers checked it for null before calling - but the buffers are created in
// HERE, so the check could never pass and this was never reached: a bootstrap
// deadlock that reported itself honestly as buffers=NO, A-fired=0 and cost one
// run. The slot breaks the circle: the caller no longer needs the buffer to
// exist in order to ask for it to be filled.
// R55: do_barrier=false is arm C. ReShade's own depth resource is not a
// depth-stencil target and this file does not know what raw D3D12 state
// ReShade left it in - so arm C lets RESHADE issue the transition through its
// own API, where resource_usage::shader_resource maps to whatever ReShade
// itself used, and this function only records the copy. Guessing a raw
// D3D12_RESOURCE_STATE here is exactly the class of mistake that took the
// device down in P10.3, and there is still no debug layer to catch it.
void dc_copy(ID3D12GraphicsCommandList *gl, ID3D12Resource *src,
             unsigned slot, D3D12_RESOURCE_STATES before, bool do_barrier = true)
{
    if (gl == nullptr || src == nullptr || slot > 2u) return;

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = src;
    // ALL SUBRESOURCES, NOT PLANE 0. R38: the first version transitioned
    // subresource 0 only. R32G8X24_TYPELESS HAS TWO PLANES - depth and
    // stencil - so that left the resource SPLIT: plane 0 in COPY_SOURCE,
    // plane 1 still in DEPTH_WRITE. A depth-stencil view requires both planes
    // in a compatible state, so the game's next bind of it was invalid and the
    // device was lost. Intermittently, because it depends on whether stencil
    // is live in that view, which is why it showed up "sometimes, looking at
    // the sky". The producer's colour copy in gpu1_context has always used
    // ALL_SUBRESOURCES; the correct pattern was already in this project.
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (do_barrier) gl->ResourceBarrier(1, &b);

    // R40. ASK FOR THE PLANE'S FORMAT, DO NOT ASSUME IT. P10.0 through P10.2
    // hardcoded DXGI_FORMAT_R32_FLOAT. Plane 0 of R32G8X24_TYPELESS is
    // R32_FLOAT_X8X24_TYPELESS, which is NOT the same format, and a
    // CopyTextureRegion whose destination footprint format is incompatible
    // with the source subresource does not copy - it fails, silently without a
    // debug layer, leaving the readback buffer holding whatever the heap held.
    // Which is exactly what "nan and the same 3.3968e38 every run" is: not
    // random garbage, a CONSTANT, because it is the same untouched memory.
    //
    // GetCopyableFootprints answers this from the resource itself. The device
    // comes from the resource rather than from a cached pointer, so it cannot
    // be the wrong one - R39's mistake, made unrepeatable rather than fixed.
    ID3D12Device *rdev = nullptr;
    if (FAILED(src->GetDevice(IID_PPV_ARGS(&rdev))) || rdev == nullptr) return;
    const D3D12_RESOURCE_DESC sd0 = src->GetDesc();   // the REAL dimensions
    D3D12_RESOURCE_DESC sd = sd0;
    sd.Width = DC_PATCH; sd.Height = DC_PATCH;      // the PATCH's footprint
    sd.MipLevels = 1; sd.DepthOrArraySize = 1;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT pf{};
    UINT64 total = 0;
    rdev->GetCopyableFootprints(&sd, 0, 1, 0, &pf, nullptr, nullptr, &total);
    if (total == 0 || pf.Footprint.RowPitch == 0) { rdev->Release(); return; }
    if (!g_dc_fp_ok.load(std::memory_order_relaxed))
    {
        g_dc_fp = pf; g_dc_total = total;
        g_dc_fp_ok.store(true, std::memory_order_release);
    }
    if (g_dc_rb[0] == nullptr) (void)dc_make_buffers(rdev, total);
    rdev->Release();
    ID3D12Resource *const dst = g_dc_rb[slot];
    if (dst == nullptr) return;

    D3D12_TEXTURE_COPY_LOCATION cs{}, cd{};
    cs.pResource = src;
    cs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    cs.SubresourceIndex = 0;
    cd.pResource = dst;
    cd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    cd.PlacedFootprint = pf;      // format, width, height, depth AND row pitch
    // R47. CENTRE THE PATCH. P10.0-P10.7 sampled the TOP-LEFT CORNER, which in
    // this title is sky - far plane, 0.0 in reversed-Z, identical in every
    // frame whatever the camera does. A comparison over that patch cannot tell
    // "arm B is correct" from "there is no geometry here to be wrong about",
    // and the numbers it produced are small for the second reason at least as
    // plausibly as the first. The centre of the frame is where the geometry
    // is, and it is where a still-camera comparison has something to say.
    const UINT sw = (UINT)sd0.Width, sh = (UINT)sd0.Height;
    const UINT bx = (sw > DC_PATCH) ? ((sw - DC_PATCH) / 2u) : 0u;
    const UINT by = (sh > DC_PATCH) ? ((sh - DC_PATCH) / 2u) : 0u;
    const D3D12_BOX box{ bx, by, 0, bx + DC_PATCH, by + DC_PATCH, 1 };
    gl->CopyTextureRegion(&cd, 0, 0, 0, &cs, &box);

    // RESTORE EXACTLY. The game did not ask us to change its resource state
    // and must not be able to tell that we did.
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = before;
    if (do_barrier) gl->ResourceBarrier(1, &b);
}

// R41. THE SENTINEL, AND IT REPLACES THE DEBUG LAYER FOR THIS ONE QUESTION.
//
// dxcpl is NOT available on this rig, so a silently-failing CopyTextureRegion
// cannot be named by validation. It can still be named by arithmetic: a
// READBACK heap is plain system memory and the CPU may write it, so both
// buffers are filled with a value no depth buffer can hold before each sample
// is armed. After the copy, either the value changed - the copy landed - or it
// did not, and that is the whole diagnosis, with no debug layer and no guess.
//
// -12345.0f is chosen because it is outside every depth convention (0..1
// forward, 1..0 reversed) and is not a bit pattern anything produces by
// accident, unlike 0 or NaN which are both things a real copy could deliver.
const float DC_SENTINEL = -12345.0f;

void dc_prefill()
{
    if (!g_dc_fp_ok.load(std::memory_order_acquire) || g_dc_total == 0) return;
    for (unsigned i = 0; i < 3; ++i)
    {
        if (g_dc_rb[i] == nullptr) continue;
        float *p = nullptr;
        D3D12_RANGE none{0, 0};                 // reading nothing
        if (SUCCEEDED(g_dc_rb[i]->Map(0, &none, (void **)&p)) && p != nullptr)
        {
            const size_t nf = (size_t)(g_dc_total / 4);
            for (size_t k = 0; k < nf; ++k) p[k] = DC_SENTINEL;
            D3D12_RANGE all{0, (SIZE_T)g_dc_total};   // wrote everything
            g_dc_rb[i]->Unmap(0, &all);
        }
    }
}

bool dc_make_buffers(ID3D12Device *dev, UINT64 bytes)
{
    if (dev == nullptr || bytes == 0) return false;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = bytes;
    rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN; rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    for (unsigned i = 0; i < 3; ++i)
    {
        if (g_dc_rb[i] != nullptr) continue;
        const HRESULT bh = dev->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(&g_dc_rb[i]));
        g_dc_buf_hr.store((int)bh, std::memory_order_relaxed);
        if (FAILED(bh)) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// R74 - THE MVEC CONTENT PROBE
// ---------------------------------------------------------------------------
//
// R73 identified the velocity buffer BY ROLE: R16G16_FLOAT, display extent
// matching the game's own MVExtent telemetry, and 5044 render-target binds
// against 7 for the next display-extent candidate. That is 720 to 1 and it is
// an identification.
//
// It is not a VERIFICATION. Marcelo's standard, set for depth and correct
// here too, is that a still camera is the control: a velocity field goes to
// ZERO when nothing moves, and no other display-extent two-channel float
// resource does that for the same reason.
//
// And two things decide whether the field is USABLE at all, neither of which
// a bind count can answer: THE UNITS and THE SIGN. Pixels or NDC changes the
// numbers by three orders of magnitude, and NGX has MVecScaleX/Y precisely
// because the two ends may disagree. Feeding a correctly identified buffer in
// the wrong convention is R30's "plausible-looking wrong field" with extra
// steps.
//
// WHY THE READ HAPPENS AT A RENDER TARGET BIND
//
// A bound render target is in D3D12_RESOURCE_STATE_RENDER_TARGET by the API's
// own requirement. Entailed, not guessed - the same argument arm B used for
// depth at a bind, and exactly what P10.3 lacked when it guessed a state for a
// resource this project did not create and lost the device for it.
//
// WHAT THE READ ACTUALLY CONTAINS
//
// The bind happens BEFORE the pass draws, so the contents are the LAST
// COMPLETED write - the previous frame's finished velocity. That is fine for
// every question here and it is stated rather than discovered later.

unsigned short g_mv_dummy_unused = 0;   // keeps the section greppable

// IEEE half to float. R16G16_FLOAT is two halves per texel, so the existing
// float32 readback path cannot be reused and a converter is not optional.
float half_to_float(unsigned short h)
{
    const unsigned s16 = (unsigned)((h >> 15) & 0x1u);
    const unsigned e16 = (unsigned)((h >> 10) & 0x1Fu);
    const unsigned m16 = (unsigned)(h & 0x3FFu);
    unsigned f = 0u;

    if (e16 == 0u)
    {
        if (m16 == 0u)
        {
            f = s16 << 31;                       // signed zero
        }
        else
        {
            // Subnormal: normalise it by hand. Rare in a velocity field, and
            // wrong here would read as a suspiciously small number rather
            // than as an error, which is the kind of thing this file exists
            // to not do.
            unsigned m = m16;
            int e = -1;
            do { m <<= 1; ++e; } while ((m & 0x400u) == 0u);
            m &= 0x3FFu;
            f = (s16 << 31) | ((unsigned)(127 - 15 - e) << 23) | (m << 13);
        }
    }
    else if (e16 == 0x1Fu)
    {
        f = (s16 << 31) | 0x7F800000u | (m16 << 13);   // inf / NaN
    }
    else
    {
        f = (s16 << 31) | ((e16 - 15u + 127u) << 23) | (m16 << 13);
    }

    float out = 0.0f;
    memcpy(&out, &f, sizeof out);
    return out;
}

std::atomic<int> g_mv_mode;                    // ini MVecProbe: 0 off, 1 on
std::atomic<unsigned> g_mv_index;              // which RTV-ranked candidate
std::atomic<unsigned long long> g_mv_target;   // its resource handle
std::atomic<unsigned> g_mv_w, g_mv_h, g_mv_fmt;

ID3D12Resource *g_mv_rb = nullptr;
D3D12_PLACED_SUBRESOURCE_FOOTPRINT g_mv_fp{};
UINT64 g_mv_total = 0;
UINT64 g_mv_stride = 0;
std::atomic<bool> g_mv_fp_ok;
const unsigned MV_N_MAX = 4;
std::atomic<int> g_mv_hr;

int g_mv_state = 0;                            // 0 idle, 1 copied and ripening
unsigned long long g_mv_at = 0;
std::atomic<unsigned long long> g_mv_copies, g_mv_reads;
double g_mv_px[MV_N_MAX] = {}, g_mv_py[MV_N_MAX] = {}, g_mv_pm[MV_N_MAX] = {};
unsigned g_mv_pcov[MV_N_MAX] = {};
double g_mv_magmax = 0;
unsigned g_mv_sent = 0;   // R76: texels the copy never wrote
bool g_mv_have = false;

// R75. FOUR PATCHES, NOT ONE CENTRED ONE.
//
// P12.5's test was "stand still and the magnitude falls to zero". Marcelo
// killed it before it cost a run, and he is right: this is a THIRD-PERSON game
// with idle animations. The centre of the screen is a character who never
// stops moving, and getting a zero reading would mean contriving a scene
// behind a rock to occlude him. A test that requires a contrived scene is a
// test that does not get run.
//
// THE ZERO BASELINE WAS NEVER NECESSARY. What distinguishes a velocity field
// from any other two-channel float buffer is not that it can be zero - it is
// that CAMERA MOTION MOVES ALL OF IT AT ONCE, coherently, in the same
// direction. An idle animation moves a character-shaped part of one patch. A
// camera turn moves every patch on the screen the same way.
//
// So: four patches, far apart, and the reading is whether they AGREE.
//
//   turn the camera left, then right
//     all four X means large and SAME-SIGNED, flipping with the turn
//        -> a velocity field. Nothing else on the screen does that.
//     one patch moves and three do not
//        -> that is the character animating, and it is not the discriminator
//     nothing responds to the turn at all
//        -> not the velocity field, whatever its bind count says
//
// Marcelo does not have to stand still, find a rock, or hold a pose. He has to
// turn the camera, which is the one thing a player does constantly.
const unsigned MV_PATCH = 32;
const unsigned MV_N     = 4;

// Fractions of the frame. Deliberately away from the centre, where the
// character is, and away from the top corners, where R47 found sky.
const float MV_FX[MV_N] = { 0.20f, 0.80f, 0.20f, 0.80f };
const float MV_FY[MV_N] = { 0.35f, 0.35f, 0.72f, 0.72f };

void mv_prefill();   // defined below, next to the sentinel it writes

// Records the patch copy. NO device is cached and none is assumed: the device
// comes from the resource, which is R39's mistake made unrepeatable.
void mv_copy(ID3D12GraphicsCommandList *gl, ID3D12Resource *src)
{
    if (gl == nullptr || src == nullptr) return;

    ID3D12Device *rdev = nullptr;
    if (FAILED(src->GetDevice(IID_PPV_ARGS(&rdev))) || rdev == nullptr) return;

    const D3D12_RESOURCE_DESC sd0 = src->GetDesc();
    D3D12_RESOURCE_DESC sd = sd0;
    sd.Width = MV_PATCH; sd.Height = MV_PATCH;
    sd.MipLevels = 1; sd.DepthOrArraySize = 1;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT pf{};
    UINT64 total = 0;
    rdev->GetCopyableFootprints(&sd, 0, 1, 0, &pf, nullptr, nullptr, &total);
    if (total == 0 || pf.Footprint.RowPitch == 0) { rdev->Release(); return; }

    if (!g_mv_fp_ok.load(std::memory_order_relaxed))
    {
        g_mv_fp = pf; g_mv_total = total;
        g_mv_fp_ok.store(true, std::memory_order_release);
    }
    // One buffer, MV_N patches, each starting on its own 512 boundary because
    // that is what a placed footprint requires.
    g_mv_stride = ((total + 511ull) / 512ull) * 512ull;
    (void)0;
    if (g_mv_rb == nullptr)
    {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = g_mv_stride * MV_N;
        rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_UNKNOWN; rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        const HRESULT bh = rdev->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(&g_mv_rb));
        g_mv_hr.store((int)bh, std::memory_order_relaxed);
        if (FAILED(bh)) { rdev->Release(); return; }
    }
    rdev->Release();
    if (g_mv_rb == nullptr) return;

    // RENDER_TARGET is ENTAILED here - see the note at the top of this section.
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = src;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    // ---- R86: NO BARRIER HERE. THE CALLER OWNS THE WINDOW. ----
    //
    // This used to declare RENDER_TARGET -> COPY_SOURCE and back, because its
    // only caller was the RTV bind event where RENDER_TARGET was entailed.
    // R85 moved it inside fire_mvec_hook, which has ALREADY transitioned the
    // resource to copy_source - so StateBefore named a state the resource was
    // not in, the command list became invalid, and UE5 died on Close() with
    // E_INVALIDARG at D3D12CommandList.cpp:284.
    //
    // R26's note in gpu1_context says exactly this about COPY_DEST and I did
    // it anyway. The resource arrives in COPY_SOURCE, which is what a
    // CopyTextureRegion source must be, and it leaves in COPY_SOURCE for the
    // caller to restore.
    (void)b;

    D3D12_TEXTURE_COPY_LOCATION cs{}, cd{};
    cs.pResource = src; cs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    cs.SubresourceIndex = 0;
    cd.pResource = g_mv_rb; cd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;

    mv_prefill();   // R76: before the copy is recorded, so survival means it never landed

    const UINT sw = (UINT)sd0.Width, sh = (UINT)sd0.Height;
    for (unsigned q = 0; q < MV_N; ++q)
    {
        UINT bx = (UINT)((float)sw * MV_FX[q]);
        UINT by = (UINT)((float)sh * MV_FY[q]);
        if (bx + MV_PATCH > sw) bx = (sw > MV_PATCH) ? (sw - MV_PATCH) : 0u;
        if (by + MV_PATCH > sh) by = (sh > MV_PATCH) ? (sh - MV_PATCH) : 0u;
        const D3D12_BOX box{ bx, by, 0, bx + MV_PATCH, by + MV_PATCH, 1 };
        cd.PlacedFootprint = pf;
        cd.PlacedFootprint.Offset = g_mv_stride * q;
        gl->CopyTextureRegion(&cd, 0, 0, 0, &cs, &box);
    }

    // R86: and no barrier back either - fire_mvec_hook restores the state it
    // read from the engine's own barrier, which is the only correct one.

    g_mv_state = 1;
    g_mv_at = g_frames.load(std::memory_order_relaxed);
    g_mv_copies.fetch_add(1, std::memory_order_relaxed);
}

// R76. THE SENTINEL, WHICH R46 ALREADY PAID FOR ONCE.
//
// P12.6 read 66 successful readbacks of exact zeros with hr=0 and could not
// say whether the copy landed. A FRESH READBACK HEAP IS ZERO-FILLED, so a
// CopyTextureRegion that fails silently - R40's exact failure, with no debug
// layer to catch it - is indistinguishable from a buffer of genuine zeros.
//
// The depth probe has had a sentinel since R46 for precisely this reason and
// it was not carried across. Filling with a pattern no velocity field can
// produce makes "the copy never wrote here" a different answer from "the
// motion was zero".
//
// 0x7C00 is +infinity as an IEEE half. A velocity field cannot contain it and
// the NaN filter in the readback will not silently eat it.
const unsigned short MV_SENTINEL = 0x7C00u;

void mv_prefill()
{
    if (g_mv_rb == nullptr || g_mv_stride == 0 || !g_mv_fp_ok.load(std::memory_order_acquire))
        return;
    unsigned short *p = nullptr;
    D3D12_RANGE none{ 0, 0 };
    if (SUCCEEDED(g_mv_rb->Map(0, &none, (void **)&p)) && p != nullptr)
    {
        const size_t n = (size_t)(g_mv_stride * MV_N) / sizeof(unsigned short);
        for (size_t i = 0; i < n; ++i) p[i] = MV_SENTINEL;
        D3D12_RANGE all{ 0, (SIZE_T)(g_mv_stride * MV_N) };
        g_mv_rb->Unmap(0, &all);
    }
}

std::atomic<unsigned long long> g_clr_depth_only;
std::atomic<unsigned long long> g_clr_stencil_only;
std::atomic<unsigned long long> g_clr_both;
unsigned long long g_top8[8] = {};              // cold-refreshed, read warm
std::atomic<unsigned int> g_top8_n;
std::atomic<unsigned int> g_pool_mask;          // top-8 members cleared this frame
std::atomic<unsigned long long> g_pool_hist[5]; // 0,1,2,3,4+ distinct per frame

inline unsigned long long qpc()
{
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return (unsigned long long)v.QuadPart;
}

void init_timebase()
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpc_to_ns = (f.QuadPart != 0) ? (1000000000.0 / (double)f.QuadPart) : 0.0;
    g_last_dump_qpc = qpc();
}

// Is this the game's device? Lazy capture, then pointer identity.
bool is_game_device(reshade::api::device *dev)
{
    if (dev == nullptr) return false;

    void *known = g_game_dev.load(std::memory_order_relaxed);
    if (known != nullptr) return known == static_cast<void *>(dev);

    // Not resolved yet. The selection only knows the game LUID once
    // init_swapchain has run, so this stays false for the first frames of a
    // launch and then latches. That is correct rather than unfortunate:
    // resources created before the swapchain exists are engine startup
    // allocations, not scene buffers.
    mgpu::adapter::selection_result sel;
    mgpu::adapter::get_selection(sel);
    if (!sel.game_luid_known) return false;
    if (dev->get_api() != reshade::api::device_api::d3d12) return false;

    ID3D12Device *d12 = reinterpret_cast<ID3D12Device *>(dev->get_native());
    if (d12 == nullptr) return false;

    const LUID l = d12->GetAdapterLuid();
    if (l.LowPart != sel.game_luid.LowPart || l.HighPart != sel.game_luid.HighPart)
        return false;

    // R39. AFTER THE LUID CHECK, NEVER BEFORE. P10.1 stored this two lines
    // up, so the FIRST D3D12 device to reach here won - and both runtimes
    // reach here. When that was the bridge's GPU 1 device, R37's readback
    // buffers were created on GPU 1 while the copies were recorded on the
    // GAME's GPU 0 command list. A resource from another adapter in a command
    // list is invalid: nothing valid ever landed, the buffers kept their
    // uninitialised contents, and the comparison read NaN and FLT_MAX-shaped
    // garbage as though it were depth. The same cross-device use is the
    // likeliest source of the access violation at 0x3c.
    //
    // The filter this file already had for g_game_dev is the same filter this
    // needed, six lines apart, and I put the store on the wrong side of it.
    g_game_d12.store(d12, std::memory_order_relaxed);
    g_game_dev.store(static_cast<void *>(dev), std::memory_order_relaxed);
    return true;
}

// ---------------------------------------------------------------------------
// Candidate filters
// ---------------------------------------------------------------------------

bool is_depth_format(unsigned int f)
{
    using reshade::api::format;
    switch (static_cast<format>(f))
    {
    case format::d16_unorm:
    case format::d16_unorm_s8_uint:
    case format::d24_unorm_x8_uint:
    case format::d24_unorm_s8_uint:
    case format::d32_float:
    case format::d32_float_s8_uint:
    case format::r16_typeless:
    case format::r24_g8_typeless:
    case format::r32_typeless:
    case format::r32_g8_typeless:
        return true;
    default:
        return false;
    }
}

// Two-channel, float-ish, which is what every engine this project has met
// stores motion in. SNORM and SINT are included because UE's packed velocity
// and a couple of console-derived paths use them, and excluding a format
// because it is unusual is how a probe misses the answer it exists to find.
bool is_mvec_format(unsigned int f)
{
    using reshade::api::format;
    switch (static_cast<format>(f))
    {
    case format::r16g16_float:
    case format::r16g16_snorm:
    case format::r16g16_unorm:
    case format::r16g16_sint:
    case format::r16g16_typeless:
    case format::r32g32_float:
    case format::r32g32_typeless:
        return true;
    default:
        return false;
    }
}

// A size band rather than an exact match. The scene buffers sit between a
// quarter of the presented area (DLSS Ultra Performance) and the presented
// area itself. Shadow atlases are usually square and often larger than the
// swapchain, so the upper bound removes most of them without a guess about
// what a shadow map looks like.
bool in_scene_band(unsigned int w, unsigned int h)
{
    if (g_scene_w == 0 || g_scene_h == 0) return w >= 640 && h >= 360;
    const double area  = (double)w * (double)h;
    const double scene = (double)g_scene_w * (double)g_scene_h;
    return area >= scene * 0.20 && area <= scene * 1.05;
}

// Linear scan, most-recently-hit first. A ranking table converges fast: after
// the first few frames the scene depth is entry 0 or 1 and the scan is one or
// two compares. This is a probe, and the honest description of the cost is one
// cache line of compares per OMSetRenderTargets, not zero.
int find_index(lane &L, unsigned long long handle)
{
    const unsigned n = L.n.load(std::memory_order_acquire);
    if (n == 0) return -1;

    const unsigned m = L.mru.load(std::memory_order_relaxed);
    if (m < n && L.t[m].handle == handle) return (int)m;

    for (unsigned i = 0; i < n; ++i)
    {
        if (L.t[i].handle == handle)
        {
            L.mru.store(i, std::memory_order_relaxed);
            return (int)i;
        }
    }
    return -1;
}

// Cold path only. Under the lock.
void add_candidate(lane &L, unsigned long long handle, unsigned int w,
                   unsigned int h, unsigned int fmt)
{
    std::lock_guard<std::mutex> lk(g_table_lock);
    const unsigned n = L.n.load(std::memory_order_relaxed);
    for (unsigned i = 0; i < n; ++i)
        if (L.t[i].handle == handle) return;

    // R76: a slot whose handle was zeroed by eviction is FREE. Without this,
    // every resolution change would consume MAX_CAND slots permanently and the
    // table would overflow into silence after a few of them.
    for (unsigned i = 0; i < n; ++i)
        if (L.t[i].handle == 0)
        {
            L.t[i].width = w; L.t[i].height = h; L.t[i].format = fmt;
            L.t[i].a.store(0, std::memory_order_relaxed);
            L.t[i].b.store(0, std::memory_order_relaxed);
            L.t[i].handle = handle;      // published LAST
            return;
        }

    if (n >= MAX_CAND)
    {
        L.overflow.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    L.t[n].handle = handle;
    L.t[n].width  = w;
    L.t[n].height = h;
    L.t[n].format = fmt;
    L.t[n].a.store(0, std::memory_order_relaxed);
    L.t[n].b.store(0, std::memory_order_relaxed);
    L.n.store(n + 1, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

void on_init_resource(reshade::api::device *device,
                      const reshade::api::resource_desc &desc,
                      const reshade::api::subresource_data *,
                      reshade::api::resource_usage,
                      reshade::api::resource resource)
{
    if (!is_game_device(device)) return;
    if (desc.type != reshade::api::resource_type::texture_2d) return;

    const unsigned long long t0 = qpc();
    g_examined.fetch_add(1, std::memory_order_relaxed);

    const unsigned int w   = desc.texture.width;
    const unsigned int h   = desc.texture.height;
    const unsigned int fmt = static_cast<unsigned int>(desc.texture.format);

    if (g_depth_on.load(std::memory_order_relaxed) &&
        (desc.usage & reshade::api::resource_usage::depth_stencil) !=
            reshade::api::resource_usage::undefined &&
        is_depth_format(fmt))
    {
        add_candidate(g_depth, resource.handle, w, h, fmt);
        g_depth.ns.fetch_add((unsigned long long)((qpc() - t0) * g_qpc_to_ns),
                             std::memory_order_relaxed);
        return;
    }

    if (g_mvec_on.load(std::memory_order_relaxed) &&
        is_mvec_format(fmt) && in_scene_band(w, h))
    {
        add_candidate(g_mvec, resource.handle, w, h, fmt);
        g_mvec.ns.fetch_add((unsigned long long)((qpc() - t0) * g_qpc_to_ns),
                            std::memory_order_relaxed);
    }
}

void on_init_resource_view(reshade::api::device *device,
                           reshade::api::resource resource,
                           reshade::api::resource_usage usage_type,
                           const reshade::api::resource_view_desc &,
                           reshade::api::resource_view view)
{
    if (!is_game_device(device)) return;

    const unsigned long long t0 = qpc();

    // Depth lateral: remember view -> resource so the bind callback does not
    // have to ask the device.
    if (g_depth_on.load(std::memory_order_relaxed) &&
        (usage_type & reshade::api::resource_usage::depth_stencil) !=
            reshade::api::resource_usage::undefined)
    {
        std::lock_guard<std::mutex> lk(g_table_lock);
        const unsigned n = g_dsv_n.load(std::memory_order_relaxed);
        bool seen = false;
        for (unsigned i = 0; i < n; ++i)
            if (g_dsv[i].view == view.handle) { seen = true; break; }
        if (!seen && n < MAX_DSV)
        {
            g_dsv[n].view = view.handle;
            g_dsv[n].res  = resource.handle;
            g_dsv_n.store(n + 1, std::memory_order_release);
        }
        g_depth.ns.fetch_add((unsigned long long)((qpc() - t0) * g_qpc_to_ns),
                             std::memory_order_relaxed);
        return;
    }

    // R71: remember RTV -> resource for the mvec candidates, so the bind
    // callback can count how often each is RENDERED INTO without asking the
    // device anything.
    if (g_mvec_on.load(std::memory_order_relaxed) &&
        (usage_type & reshade::api::resource_usage::render_target) !=
            reshade::api::resource_usage::undefined &&
        find_index(g_mvec, resource.handle) >= 0)
    {
        std::lock_guard<std::mutex> lk(g_table_lock);
        const unsigned n = g_rtv_n.load(std::memory_order_relaxed);
        bool seen = false;
        for (unsigned i = 0; i < n; ++i)
            if (g_rtv[i].view == view.handle) { seen = true; break; }
        if (!seen && n < MAX_DSV)
        {
            g_rtv[n].view = view.handle;
            g_rtv[n].res  = resource.handle;
            g_rtv_n.store(n + 1, std::memory_order_release);
        }
    }

    // MVec lateral: an SRV over a candidate is the signal that something reads
    // it. A velocity target that is written and never read as a texture is not
    // what we are looking for.
    if (g_mvec_on.load(std::memory_order_relaxed) &&
        (usage_type & reshade::api::resource_usage::shader_resource) !=
            reshade::api::resource_usage::undefined)
    {
        const int i = find_index(g_mvec, resource.handle);
        if (i >= 0) g_mvec.t[i].a.fetch_add(1, std::memory_order_relaxed);
        g_mvec.ns.fetch_add((unsigned long long)((qpc() - t0) * g_qpc_to_ns),
                            std::memory_order_relaxed);
    }
}

// ---- R78/R82: THE TRANSPORT'S ACQUISITION, LIFTED OUT OF THE RTV LOOP ----
//
// It used to live inside the render-target loop and could therefore only be
// reached through the g_rtv table. R80 is what that cost: the table was empty
// for the whole run, so the hook was unreachable even though the resource was
// live and catalogued. Now any signal that can name the published resource can
// hand the moment over, and g_mvec_last_frame still makes it once per frame no
// matter how many of them do.
//
// R83: THE BARRIER IS NO LONGER AN ASSUMPTION. The caller passes the state the
// resource is ACTUALLY in - read from the engine's own barrier - and this
// transitions from there and back to it. P13.0 hardcoded render_target, which
// was wrong for this title in the most complete way possible: the buffer is
// never a render target at all.
void fire_mvec_hook(reshade::api::command_list *cl, unsigned long long res,
                    reshade::api::resource_usage state)
{
    const unsigned long long want = g_mvec_src.load(std::memory_order_relaxed);
    if (g_mvec_hook == nullptr || want == 0 || res != want || cl == nullptr) return;

    const unsigned long long fr = g_frames.load(std::memory_order_relaxed);
    if (g_mvec_last_frame == fr)
    {
        g_mvec_skips.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_mvec_last_frame = fr;

    const reshade::api::resource r = { want };
    cl->barrier(r, state, reshade::api::resource_usage::copy_source);
    g_mvec_hook(reinterpret_cast<void *>(cl->get_native()), want);

    // ---- R85: THE CONTENT PROBE, IN THE SAME WINDOW ----
    //
    // It was still firing from inside the RTV table loop, which R83 proved
    // never executes in this engine: R85 read copies=0, every patch 0.0000,
    // and ZERO of 4096 sentinel texels surviving - a readback that was never
    // written at all, not a buffer of real zeros. The transport moved to the
    // barrier and this did not.
    //
    // Here it needs no barrier of its own: the resource is already in
    // copy_source, which is what a CopyTextureRegion source must be. The old
    // site relied on RENDER_TARGET being "entailed" and copied from it
    // untransitioned, which was never valid either.
    if (g_mv_mode.load(std::memory_order_relaxed) != 0 && g_mv_state == 0)
    {
        const unsigned long long mvt = g_mv_target.load(std::memory_order_relaxed);
        if (mvt == want)
            mv_copy(reinterpret_cast<ID3D12GraphicsCommandList *>(cl->get_native()),
                    reinterpret_cast<ID3D12Resource *>(static_cast<uintptr_t>(want)));
    }

    cl->barrier(r, reshade::api::resource_usage::copy_source, state);
    g_mvec_copies.fetch_add(1, std::memory_order_relaxed);
}

// WARM. Fires on every ID3D12GraphicsCommandList::OMSetRenderTargets.
void on_bind_rtv_dsv(reshade::api::command_list *cl, uint32_t rtv_count,
                     const reshade::api::resource_view *rtvs,
                     reshade::api::resource_view dsv)
{
    // ---- R71: the RTV half, and it MUST run before the dsv early-return ----
    //
    // A velocity pass binds a colour target and NO depth. Counting render
    // target binds after "if (dsv.handle == 0) return" would miss exactly the
    // passes this is for - which is the shape of mistake this record keeps
    // catching one round too late.
    if (rtvs != nullptr && rtv_count != 0 &&
        g_mvec_on.load(std::memory_order_relaxed))
    {
        const unsigned long long r0 = qpc();
        const unsigned rn = g_rtv_n.load(std::memory_order_acquire);

        // R82. THE NUMBER R80 NEEDED AND DID NOT HAVE. "RTV binds seen = 0"
        // could not be told apart from "this event never fired". Now it can.
        g_bind_fires.fetch_add(1, std::memory_order_relaxed);

        // R81/R82: ask the DEVICE what this view belongs to. No table, so
        // nothing here depends on having been subscribed when the engine
        // created its views - which is the single assumption that broke R80.
        reshade::api::device *const dv = (cl != nullptr) ? cl->get_device() : nullptr;

        // rn is no longer in the loop condition: the direct path below must run
        // even when the table is empty, which is exactly the case that matters.
        for (uint32_t k = 0; k < rtv_count; ++k)
        {
            if (rtvs[k].handle == 0) continue;
            g_bind_rtvs.fetch_add(1, std::memory_order_relaxed);

            if (dv != nullptr)
            {
                const reshade::api::resource rr = dv->get_resource_from_view(rtvs[k]);
                if (rr.handle != 0)
                {
                    const int ci = find_index(g_mvec, rr.handle);
                    if (ci >= 0)
                    {
                        g_mvec.t[ci].c.fetch_add(1, std::memory_order_relaxed);
                        g_bind_via_api.fetch_add(1, std::memory_order_relaxed);
                    }
                    fire_mvec_hook(cl, rr.handle,
                                   reshade::api::resource_usage::render_target);
                }
            }

            for (unsigned i = 0; i < rn; ++i)
            {
                if (g_rtv[i].view != rtvs[k].handle) continue;
                const int c = find_index(g_mvec, g_rtv[i].res);
                if (c >= 0)
                {
                    g_mvec.t[c].b.fetch_add(1, std::memory_order_relaxed);
                    g_rtv_binds_seen.fetch_add(1, std::memory_order_relaxed);
                    // R82: counted HERE, inside the candidate match, so that it
                    // means exactly what via_api means. Counted outside it, the
                    // table route would be scored on the easier bar of "a view
                    // was in the table" and the comparison this run exists for
                    // would not be one.
                    g_bind_via_table.fetch_add(1, std::memory_order_relaxed);
                }

                // The hook is fired from the direct resolve above as well;
                // g_mvec_last_frame stops both paths doing it twice in a frame.
                fire_mvec_hook(cl, g_rtv[i].res,
                               reshade::api::resource_usage::render_target);

                // R74: the content probe fires HERE, where RENDER_TARGET is
                // entailed, and only once per readback round.
                {
                    const unsigned long long want =
                        g_mv_target.load(std::memory_order_relaxed);
                    // R85: MOVED. This site is on the RTV route, which does
                    // not execute in this engine. See fire_mvec_hook.
                    (void)want;
                }
                break;
            }
        }
        g_mvec.ns.fetch_add((unsigned long long)((qpc() - r0) * g_qpc_to_ns),
                            std::memory_order_relaxed);
    }

    if (dsv.handle == 0) return;

    const unsigned long long t0 = qpc();

    const unsigned n = g_dsv_n.load(std::memory_order_acquire);
    for (unsigned i = 0; i < n; ++i)
    {
        if (g_dsv[i].view != dsv.handle) continue;
        const int c = find_index(g_depth, g_dsv[i].res);
        if (c >= 0) g_depth.t[c].a.fetch_add(1, std::memory_order_relaxed);

        // R33. Is this THIS FRAME's turn holder - the resource that took a
        // scene-resolution DEPTH clear? Then this bind is a state-known moment
        // at which its depth is further along than it was at the clear.
        {
            const unsigned long long turn = g_turn_res.load(std::memory_order_relaxed);
            if (turn != 0 && g_dsv[i].res == turn)
            {
                const unsigned bidx =
                    g_turn_binds.fetch_add(1, std::memory_order_relaxed) + 1u;
                g_turn_last_bind_qpc.store(t0, std::memory_order_relaxed);

                // R37 ARM B. The Nth bind, N learned from the previous frame.
                // A bound depth-stencil target is DEPTH_WRITE or DEPTH_READ -
                // both entailed by the API, neither guessed - so this barrier
                // is as safe as A's. DEPTH_WRITE is used because the turn
                // holder is being rendered into; if a title ever binds it
                // read-only at this index the debug layer is what would say so.
                // Mode 2 only: A alone is the safer arm and proves the
                // plumbing before B is switched on.
                if (g_dc_mode.load(std::memory_order_relaxed) == 2 &&
                    g_dc_ready.load(std::memory_order_relaxed) &&
                    g_dc_hits[0].load(std::memory_order_relaxed) == 1u &&
                    g_dc_hits[1].load(std::memory_order_relaxed) == 0u &&
                    bidx == g_dc_target_bind && cl != nullptr)
                {
                    dc_copy(reinterpret_cast<ID3D12GraphicsCommandList *>(
                                static_cast<uintptr_t>(cl->get_native())),
                            reinterpret_cast<ID3D12Resource *>(
                                static_cast<uintptr_t>(g_dsv[i].res)),
                            1u, D3D12_RESOURCE_STATE_DEPTH_WRITE);
                    g_dc_hits[1].store(1u, std::memory_order_relaxed);
                    g_dc_arm_b.fetch_add(1, std::memory_order_relaxed);
                    g_dc_ready.store(false, std::memory_order_relaxed);
                }
            }
        }
        break;
    }

    g_depth.ns.fetch_add((unsigned long long)((qpc() - t0) * g_qpc_to_ns),
                         std::memory_order_relaxed);
}

// Fires on ClearDepthStencilView. Returns false: we never suppress the game's
// own command. Returning true here would delete the game's depth clear, which
// is the single most destructive thing this file could accidentally do, so it
// is written as a literal rather than a variable.
bool on_clear_dsv(reshade::api::command_list *cl, reshade::api::resource_view dsv,
                  const float *depth_value, const uint8_t *stencil_value, uint32_t,
                  const reshade::api::rect *)
{
    if (dsv.handle != 0)
    {
        const unsigned long long t0 = qpc();
        const unsigned n = g_dsv_n.load(std::memory_order_acquire);
        for (unsigned i = 0; i < n; ++i)
        {
            if (g_dsv[i].view != dsv.handle) continue;
            const int c = find_index(g_depth, g_dsv[i].res);
            if (c >= 0) g_depth.t[c].b.fetch_add(1, std::memory_order_relaxed);

            // R30b. Only the #1 candidate - every shadow atlas in the game
            // clears too, and counting those would answer a question nobody
            // asked. g_top_depth is refreshed on the cold path; until it is
            // set this whole block is one compare against zero.
            // R31. WHICH PLANE was cleared, and WHICH pool member. Both are
            // free here - the pointers were already on the stack and the
            // handle was already resolved.
            {
                const bool d = (depth_value != nullptr);
                const bool st = (stencil_value != nullptr);
                if (d && st)      g_clr_both.fetch_add(1, std::memory_order_relaxed);
                else if (d)       g_clr_depth_only.fetch_add(1, std::memory_order_relaxed);
                else if (st)      g_clr_stencil_only.fetch_add(1, std::memory_order_relaxed);

                // R33. A DEPTH clear (not a stencil-only one) on a
                // SCENE-RESOLUTION candidate opens this frame's turn. Role,
                // not identity - R32 established no identity is stable here.
                if (d && c >= 0 &&
                    !in_scene_band(g_depth.t[c].width, g_depth.t[c].height))
                    g_turn_rejected.fetch_add(1, std::memory_order_relaxed);
                if (d && c >= 0 &&
                    in_scene_band(g_depth.t[c].width, g_depth.t[c].height))
                {
                    g_turn_res.store(g_dsv[i].res, std::memory_order_relaxed);
                    g_turn_clear_qpc.store(t0, std::memory_order_relaxed);
                    g_turn_last_bind_qpc.store(0ull, std::memory_order_relaxed);
                    g_turn_binds.store(0u, std::memory_order_relaxed);

                    // R37 ARM A. Recorded BEFORE the clear - this callback runs
                    // ahead of the command, which is the whole reason the
                    // content here is the buffer's previous turn. State is
                    // entailed: D3D12 requires DEPTH_WRITE for a clear.
                    // R55: modes 1 and 2 only. Mode 3 is arm C ALONE and must
                    // not touch the game's own depth buffer at all, so that a
                    // crash in a mode-3 run can only be arm C.
                    if ((g_dc_mode.load(std::memory_order_relaxed) == 1 ||
                         g_dc_mode.load(std::memory_order_relaxed) == 2) &&
                        g_dc_ready.load(std::memory_order_relaxed) &&
                        g_dc_hits[0].load(std::memory_order_relaxed) == 0u &&
                        cl != nullptr)
                    {
                        dc_copy(reinterpret_cast<ID3D12GraphicsCommandList *>(
                                    static_cast<uintptr_t>(cl->get_native())),
                                reinterpret_cast<ID3D12Resource *>(
                                    static_cast<uintptr_t>(g_dsv[i].res)),
                                0u, D3D12_RESOURCE_STATE_DEPTH_WRITE);
                        g_dc_hits[0].store(1u, std::memory_order_relaxed);
                        g_dc_arm_a.fetch_add(1, std::memory_order_relaxed);
                        g_dc_copy_frame.store(g_frames.load(std::memory_order_relaxed),
                                              std::memory_order_relaxed);
                    }
                }

                const unsigned tn = g_top8_n.load(std::memory_order_relaxed);
                for (unsigned q = 0; q < tn && q < 8u; ++q)
                    if (g_top8[q] == g_dsv[i].res)
                    { g_pool_mask.fetch_or(1u << q, std::memory_order_relaxed); break; }
            }

            const unsigned long long top = g_top_depth.load(std::memory_order_relaxed);
            if (top != 0 && g_dsv[i].res == top)
            {
                const unsigned prev =
                    g_clr_in_window.fetch_add(1, std::memory_order_relaxed);
                if (prev == 0)
                {
                    // FIRST clear since the last finish_effects. This gap IS
                    // the signal delay that moving the producer's Signal here
                    // would cost, measured rather than argued.
                    const unsigned long long f0 =
                        g_finish_qpc.load(std::memory_order_relaxed);
                    if (f0 != 0 && t0 > f0)
                    {
                        const unsigned long long ns =
                            (unsigned long long)((double)(t0 - f0) * g_qpc_to_ns);
                        g_gap_n.fetch_add(1, std::memory_order_relaxed);
                        g_gap_sum_ns.fetch_add(ns, std::memory_order_relaxed);
                        unsigned long long m = g_gap_min_ns.load(std::memory_order_relaxed);
                        while (ns < m && !g_gap_min_ns.compare_exchange_weak(
                                   m, ns, std::memory_order_relaxed)) {}
                        m = g_gap_max_ns.load(std::memory_order_relaxed);
                        while (ns > m && !g_gap_max_ns.compare_exchange_weak(
                                   m, ns, std::memory_order_relaxed)) {}
                        m = g_gap_w_min.load(std::memory_order_relaxed);
                        while (ns < m && !g_gap_w_min.compare_exchange_weak(
                                   m, ns, std::memory_order_relaxed)) {}
                        m = g_gap_w_max.load(std::memory_order_relaxed);
                        while (ns > m && !g_gap_w_max.compare_exchange_weak(
                                   m, ns, std::memory_order_relaxed)) {}
                    }
                }
            }
            break;
        }
        g_depth.ns.fetch_add((unsigned long long)((qpc() - t0) * g_qpc_to_ns),
                             std::memory_order_relaxed);
    }
    return false;
}

// ---- R82 SIGNAL 2: ID3D12GraphicsCommandList4::BeginRenderPass ----
//
// bind_render_targets_and_depth_stencil is OMSetRenderTargets. THIS is the
// other way a D3D12 renderer names its targets, and ReShade raises a different
// event for it. A UE5 build that uses render passes raises this one and never
// that one - which is what zero RTV binds beside 17 live candidates looks like
// from the outside.
//
// COUNTING ONLY, AND DELIBERATELY NOT A HOOK. This fires BEFORE the pass runs,
// so the buffer still holds the PREVIOUS frame's vectors at this moment. That
// is true of the bind event too and R78 did not say so; see the note in the
// report. The right acquisition moment is after the pass has written, which is
// what signal 3 can see and this cannot.
bool on_begin_render_pass(reshade::api::command_list *cl, uint32_t count,
                          const reshade::api::render_pass_render_target_desc *rts,
                          const reshade::api::render_pass_depth_stencil_desc *,
                          reshade::api::render_pass_flags)
{
    if (!g_mvec_on.load(std::memory_order_relaxed) || rts == nullptr || count == 0)
        return false;

    const unsigned long long r0 = qpc();
    g_rp_fires.fetch_add(1, std::memory_order_relaxed);

    reshade::api::device *const dv = (cl != nullptr) ? cl->get_device() : nullptr;
    for (uint32_t k = 0; k < count; ++k)
    {
        if (rts[k].view.handle == 0) continue;
        g_rp_rts.fetch_add(1, std::memory_order_relaxed);
        if (dv == nullptr) continue;
        const reshade::api::resource rr = dv->get_resource_from_view(rts[k].view);
        if (rr.handle == 0) continue;
        const int ci = find_index(g_mvec, rr.handle);
        if (ci >= 0)
        {
            g_mvec.t[ci].d.fetch_add(1, std::memory_order_relaxed);
            g_rp_matched.fetch_add(1, std::memory_order_relaxed);
        }
    }
    g_mvec.ns.fetch_add((unsigned long long)((qpc() - r0) * g_qpc_to_ns),
                        std::memory_order_relaxed);
    return false;   // never block the game's command
}

// ---- R82 SIGNAL 3: ID3D12GraphicsCommandList::ResourceBarrier ----
//
// THE ONE THAT DOES NOT CARE HOW THE ENGINE BINDS. It names RESOURCES, not
// views: no table, no descriptor heaps, no assumption about OMSetRenderTargets
// versus BeginRenderPass. A velocity buffer is written as a render target and
// read as a shader resource every frame, so it transitions once per frame
// whatever API wrote it.
//
// It also carries the state the engine ACTUALLY asked for, which is the thing
// the R78 hook currently asserts. And render_target -> shader_resource is the
// moment AFTER the pass has written - the correct acquisition moment, which
// neither of the two bind events can offer.
//
// This is the hottest event this file has ever subscribed to, so it early-outs
// on an empty table and times itself. If ns/frame in the R82 line is not small,
// that is a finding and this subscription comes straight back out.
void on_barrier(reshade::api::command_list *cl, uint32_t count,
                const reshade::api::resource *res,
                const reshade::api::resource_usage *old_states,
                const reshade::api::resource_usage *new_states)
{
    if (!g_mvec_on.load(std::memory_order_relaxed) || res == nullptr || count == 0) return;
    if (g_mvec.n.load(std::memory_order_acquire) == 0) return;

    const unsigned long long r0 = qpc();
    g_bar_fires.fetch_add(1, std::memory_order_relaxed);

    for (uint32_t k = 0; k < count; ++k)
    {
        if (res[k].handle == 0) continue;
        g_bar_res.fetch_add(1, std::memory_order_relaxed);
        const int ci = find_index(g_mvec, res[k].handle);
        if (ci < 0) continue;
        g_mvec.t[ci].e.fetch_add(1, std::memory_order_relaxed);
        g_bar_matched.fetch_add(1, std::memory_order_relaxed);
        if (old_states != nullptr)
            g_mvec.t[ci].last_old.store(
                static_cast<unsigned>(old_states[k]), std::memory_order_relaxed);
        if (new_states != nullptr)
            g_mvec.t[ci].last_new.store(
                static_cast<unsigned>(new_states[k]), std::memory_order_relaxed);

        // ---- R83: THE ACQUISITION MOMENT, AT LAST A REAL ONE ----
        //
        // R83 measured every transition on every candidate ending in
        // unordered_access and never in render_target: this engine writes
        // velocity with a COMPUTE shader through a UAV, which is why 147,439
        // resolved render-target views contained none of them and why four
        // runs of RTV counting were measuring a pass that does not exist here.
        //
        // So the moment is the barrier OUT of unordered_access and INTO a
        // shader-resource state: the compute pass has finished writing and the
        // engine is about to read it. That is AFTER the write, which is what
        // neither bind event could offer - it fixes R78's one-frame staleness
        // as a side effect rather than as a separate fix.
        //
        // We are called AFTER the engine's ResourceBarrier, so the resource is
        // already in new_states[k] and that is what we transition from and
        // restore to.
        if (old_states != nullptr && new_states != nullptr)
        {
            using ru = reshade::api::resource_usage;
            const unsigned sr = static_cast<unsigned>(ru::shader_resource);
            if (old_states[k] == ru::unordered_access &&
                (static_cast<unsigned>(new_states[k]) & sr) != 0)
                fire_mvec_hook(cl, res[k].handle, new_states[k]);
        }
    }
    g_bar_ns.fetch_add((unsigned long long)((qpc() - r0) * g_qpc_to_ns),
                       std::memory_order_relaxed);
}

// WARM, and only on engines that bind through root descriptors. An engine that
// binds through descriptor tables sends bind_descriptor_tables instead, which
// carries a table handle and not its contents - resolving that needs the
// device on a warm path, which this file will not do. If this counter stays at
// zero in a title while SRV creations do not, that is the finding: the engine
// is a table binder and this signal is blind there.
void on_push_descriptors(reshade::api::command_list *,
                         reshade::api::shader_stage,
                         reshade::api::pipeline_layout, uint32_t,
                         const reshade::api::descriptor_table_update &update)
{
    if (update.descriptors == nullptr || update.count == 0) return;
    if (update.type != reshade::api::descriptor_type::shader_resource_view &&
        update.type != reshade::api::descriptor_type::buffer_shader_resource_view)
        return;

    const unsigned long long t0 = qpc();

    // Descriptors of these types are an array of resource_view. We hold the
    // VIEW handle; resolving it to a resource needs the device, and this file
    // does not call the device on a warm path. So nothing is attributed to a
    // candidate here.
    //
    // That is deliberate rather than lazy. Attributing these to a candidate we
    // cannot actually identify would put a confident, correctly-formatted,
    // wrong number in the table - the failure shape this project names in
    // section 00 and has already paid for twice. What is recorded instead is
    // the ONE thing this event can honestly say: how many shader-resource
    // pushes went through root descriptors at all. A large number means the
    // engine is a root binder and a future revision could resolve views here;
    // zero, in a title whose SRV creation count is not zero, means the engine
    // binds through descriptor tables and this signal is blind in it. Both are
    // findings. Neither is a motion vector.
    const reshade::api::resource_view *views =
        static_cast<const reshade::api::resource_view *>(update.descriptors);
    unsigned hits = 0;
    for (uint32_t k = 0; k < update.count && k < 16; ++k)
        if (views[k].handle != 0) ++hits;

    if (hits != 0) g_root_srv_pushes.fetch_add(hits, std::memory_order_relaxed);

    g_mvec.ns.fetch_add((unsigned long long)((qpc() - t0) * g_qpc_to_ns),
                        std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Subscription
// ---------------------------------------------------------------------------

bool g_depth_reg = false;
bool g_mvec_reg = false;
bool g_shared_reg = false;   // init_resource / init_resource_view: both lanes

// ---------------------------------------------------------------------------
// R76 - EVICTION, WHICH R30 NAMED AS FATAL AND THIS FILE THEN NEEDED
// ---------------------------------------------------------------------------
//
// R30, written before any acquisition existed:
//
//   "THE CANDIDATE TABLE IS NEVER EVICTED ... FOR A WATCH-ONLY PROBE THAT IS
//    HARMLESS ... FOR AN ACQUISITION PATH IT IS FATAL - copying from a
//    destroyed resource is device removal, on the game's own command list,
//    which is the one failure this add-on must never cause."
//   "IDENTITY IS THE RAW HANDLE, AND D3D12 REUSES ADDRESSES."
//
// P12.5 gave this file an acquisition path - mv_copy - and left the table
// un-evicted. Then a resolution change destroyed every render target while
// stale view-to-resource entries still pointed at them. That is the crash, and
// it was documented before it was written.
//
// ZEROING THE HANDLE, NOT COMPACTING. Compaction would move entries under a
// warm reader that indexes by position. A zeroed handle simply stops matching
// find_index, which is the only route back to a freed pointer.
void on_destroy_resource(reshade::api::device *device,
                         reshade::api::resource resource)
{
    if (!is_game_device(device)) return;
    const unsigned long long h = resource.handle;
    if (h == 0) return;

    std::lock_guard<std::mutex> lk(g_table_lock);

    unsigned hit = 0;
    {
        const unsigned n = g_depth.n.load(std::memory_order_relaxed);
        for (unsigned i = 0; i < n; ++i)
            if (g_depth.t[i].handle == h) { g_depth.t[i].handle = 0; ++hit; }
    }
    {
        const unsigned n = g_mvec.n.load(std::memory_order_relaxed);
        for (unsigned i = 0; i < n; ++i)
            if (g_mvec.t[i].handle == h) { g_mvec.t[i].handle = 0; ++hit; }
    }
    {
        const unsigned n = g_dsv_n.load(std::memory_order_relaxed);
        for (unsigned i = 0; i < n; ++i)
            if (g_dsv[i].res == h) { g_dsv[i].view = 0; g_dsv[i].res = 0; ++hit; }
    }
    {
        const unsigned n = g_rtv_n.load(std::memory_order_relaxed);
        for (unsigned i = 0; i < n; ++i)
            if (g_rtv[i].res == h) { g_rtv[i].view = 0; g_rtv[i].res = 0; ++hit; }
    }

    // Everything that POINTS at it and would be copied from or barriered.
    if (g_mv_target.load(std::memory_order_relaxed) == h)
    { g_mv_target.store(0ull, std::memory_order_relaxed); ++hit; }
    if (g_mvec_src.load(std::memory_order_relaxed) == h)
    { g_mvec_src.store(0ull, std::memory_order_relaxed); ++hit; }
    if (g_semC_res.load(std::memory_order_relaxed) == h)
    { g_semC_res.store(0ull, std::memory_order_relaxed); ++hit; }
    if (g_turn_res.load(std::memory_order_relaxed) == h)
    { g_turn_res.store(0ull, std::memory_order_relaxed); ++hit; }
    if (g_top_depth.load(std::memory_order_relaxed) == h)
    { g_top_depth.store(0ull, std::memory_order_relaxed); ++hit; }
    for (unsigned q = 0; q < 8u; ++q) if (g_top8[q] == h) { g_top8[q] = 0; ++hit; }

    if (hit != 0) g_evicted.fetch_add(1, std::memory_order_relaxed);
}

void set_shared(bool on)
{
    if (on == g_shared_reg) return;
    if (on)
    {
        reshade::register_event<reshade::addon_event::init_resource>(on_init_resource);
        reshade::register_event<reshade::addon_event::init_resource_view>(on_init_resource_view);
        // R76: the one subscription R30 said this tree did not have and would
        // need the moment anything copied from a catalogued resource.
        reshade::register_event<reshade::addon_event::destroy_resource>(on_destroy_resource);
        // R82. On the SHARED lane deliberately: R80 lost a run because the
        // render-target event lives on the DEPTH lane and the mvec lane needed
        // it. These two are on whenever any lane is.
        reshade::register_event<reshade::addon_event::begin_render_pass>(on_begin_render_pass);
        reshade::register_event<reshade::addon_event::barrier>(on_barrier);
    }
    else
    {
        reshade::unregister_event<reshade::addon_event::begin_render_pass>(on_begin_render_pass);
        reshade::unregister_event<reshade::addon_event::barrier>(on_barrier);
        reshade::unregister_event<reshade::addon_event::init_resource>(on_init_resource);
        reshade::unregister_event<reshade::addon_event::init_resource_view>(on_init_resource_view);
        reshade::unregister_event<reshade::addon_event::destroy_resource>(on_destroy_resource);
    }
    g_shared_reg = on;
}

void set_depth(bool on)
{
    if (on == g_depth_reg) return;
    if (on)
    {
        reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_bind_rtv_dsv);
        reshade::register_event<reshade::addon_event::clear_depth_stencil_view>(on_clear_dsv);
    }
    else
    {
        reshade::unregister_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_bind_rtv_dsv);
        reshade::unregister_event<reshade::addon_event::clear_depth_stencil_view>(on_clear_dsv);
    }
    g_depth_reg = on;
}

void set_mvec(bool on)
{
    if (on == g_mvec_reg) return;
    if (on)
        reshade::register_event<reshade::addon_event::push_descriptors>(on_push_descriptors);
    else
        reshade::unregister_event<reshade::addon_event::push_descriptors>(on_push_descriptors);
    g_mvec_reg = on;
}

// ---------------------------------------------------------------------------
// The dump
// ---------------------------------------------------------------------------

const char *fmt_name(unsigned int f)
{
    using reshade::api::format;
    switch (static_cast<format>(f))
    {
    case format::d16_unorm:          return "D16_UNORM";
    case format::d24_unorm_s8_uint:  return "D24_UNORM_S8_UINT";
    case format::d24_unorm_x8_uint:  return "D24_UNORM_X8_UINT";
    case format::d32_float:          return "D32_FLOAT";
    case format::d32_float_s8_uint:  return "D32_FLOAT_S8_UINT";
    case format::r16_typeless:       return "R16_TYPELESS";
    case format::r24_g8_typeless:    return "R24G8_TYPELESS";
    case format::r32_typeless:       return "R32_TYPELESS";
    case format::r32_g8_typeless:    return "R32G8_TYPELESS";
    case format::r16g16_float:       return "R16G16_FLOAT";
    case format::r16g16_snorm:       return "R16G16_SNORM";
    case format::r16g16_unorm:       return "R16G16_UNORM";
    case format::r16g16_sint:        return "R16G16_SINT";
    case format::r16g16_typeless:    return "R16G16_TYPELESS";
    case format::r32g32_float:       return "R32G32_FLOAT";
    case format::r32g32_typeless:    return "R32G32_TYPELESS";
    default:                         return "other";
    }
}

// Order by primary counter, descending, without sorting the live table.
// R72: by_b sorts on the SECOND counter. P12.3 counted RTV binds and then
// ranked the list by SRV creations anyway, so 964 of 3005 binds - a third of
// them - sat on candidates below the cut and could not be seen. Counting by
// one signal and ordering by another is not a small inconsistency; it is the
// difference between a list that answers the question and one that hides the
// answer in its tail.
void top_n(lane &L, unsigned int *idx, unsigned int &out_n, unsigned int want,
           bool by_b = false)
{
    const unsigned n = L.n.load(std::memory_order_acquire);
    out_n = 0;
    for (unsigned pick = 0; pick < want && pick < n; ++pick)
    {
        int best = -1;
        unsigned long long best_v = 0;
        for (unsigned i = 0; i < n; ++i)
        {
            bool taken = false;
            for (unsigned j = 0; j < out_n; ++j)
                if (idx[j] == i) { taken = true; break; }
            if (taken) continue;

            const unsigned long long v = by_b
                ? L.t[i].b.load(std::memory_order_relaxed)
                : L.t[i].a.load(std::memory_order_relaxed);
            if (best < 0 || v > best_v) { best = (int)i; best_v = v; }
        }
        if (best < 0) break;
        idx[out_n++] = (unsigned)best;
    }
}

// ---------------------------------------------------------------------------
// R48 - THE SEMANTIC LANE (arm C): what ReShade itself thinks depth is
// ---------------------------------------------------------------------------
//
// R46 filed that "consume ReShade's DEPTH semantic" had no semantic lookup and
// was therefore a dependency on one named .fx being loaded with both its
// effect name and its variable name known in advance. That was read off the
// wrong half of the API. The pinned header (crosire/reshade @ 18deaa52) says,
// verbatim:
//
//   update_texture_bindings(semantic, srv, srv_srgb)
//       "Binds new shader resource views to ALL TEXTURE VARIABLES THAT USE
//        THE SPECIFIED SEMANTIC ... texture name : SEMANTIC"
//   enumerate_texture_variables(effect_name, callback, user_data)
//       "... effect_name: ... or NULLPTR to enumerate those of ALL LOADED
//        EFFECTS"
//   get_texture_binding(variable, out_srv, out_srv_srgb)
//       "Gets the shader resource view that is bound to the specified texture
//        variable"
//
// The semantic is a PUSH, not a lookup. Whoever detects depth - ReShade's own
// generic_depth - writes the srv into every variable declared ': DEPTH'. So
// nothing has to be named: walk EVERY texture variable of EVERY loaded effect,
// ask what each one is bound to, resolve the view to its resource, and read
// that resource's own description. A variable bound to a scene-sized
// depth-format resource IS the DEPTH semantic, identified by observation -
// which is the same standard R32 forced on the raw lane when it killed static
// identity.
//
// This is arm C of the A/B/C comparison Marcelo specified. It costs ONE
// enumeration every MetaProbeLogSeconds on the game thread. Not per frame,
// never per draw.
//
// NOTHING IS BOUND OR TRANSPORTED HERE EITHER. The lane reports handles.

struct sem_row
{
    char               name[72];
    unsigned long long srv;
    unsigned long long res;
    unsigned int       width;
    unsigned int       height;
    unsigned int       format;
};

const unsigned SEM_MAX = 12;

// Written and read on the GAME thread only - note_effects stores the runtime
// from on_reshade_finish_effects and dump() runs from note_frame on that same
// callback. No atomics, because there is no second thread to race with. If
// that ever stops being true this comment is the thing that was wrong.
sem_row  g_sem[SEM_MAX] = {};
unsigned g_sem_n    = 0;      // rows recorded
unsigned g_sem_seen = 0;      // variables walked, bound or not
bool     g_sem_ran  = false;  // the enumeration was attempted at least once
void    *g_rt       = nullptr;

// ---- R55: ARM C. Copying from ReShade's own depth resource. ----
//
// R54 established what arm C IS: a resource ReShade owns, that the game never
// renders into, stable for the life of the effect. What it did NOT establish
// is whether this add-on can COPY from it - which is the one thing the whole
// transport rests on, and the one thing that can take the device down.
//
// So it is proved here, in a probe, where a failure costs a test run instead
// of the shipped path. DepthCompare=3 runs arm C ALONE: arms A and B are gated
// off, nothing touches the game's own depth buffer, and a crash in a mode-3
// run therefore has exactly one possible cause.
//
// THE TRANSITION IS RESHADE'S, NOT OURS. This file does not know what raw
// D3D12 state ReShade leaves its copy in, and P10.3 already cost a device on
// exactly that kind of guess. resource_usage::shader_resource -> copy_source
// through ReShade's own barrier maps to whatever ReShade itself used.
// g_semC_res / g_semC_w / g_semC_h are declared EARLY, next to g_evicted:
// R76's on_destroy_resource has to clear g_semC_res and is defined long before
// this section. A variable used by an eviction path belongs above every path
// that can evict.

// R60. WHICH resource arm C reads. Two candidates now exist and they are not
// interchangeable:
//
//   DepthBufferTex     ReShade's own copy of the game's depth. RENDER
//                      resolution, R32G8_TYPELESS, two planes.
//   MGPU_DepthOutTex   our tap's render target. OUTPUT resolution, R32F, one
//                      plane, already resampled by the pass that writes it.
//
// The second is what the transport will carry, so it is what arm C must be
// measured on: proving we can copy the first says nothing about whether the
// second holds real numbers, and it is written by a shader nobody has checked
// the output of.
//
// SELECTED AFTER the enumeration, not inside it, because the tap target is
// enumerated LAST and a first-match rule would always pick the semantic one.
const char SEMC_PREFER[] = "MGPU_DepthOut";
unsigned long long g_semC_pref = 0, g_semC_fallback = 0;
unsigned g_semC_pw = 0, g_semC_ph = 0, g_semC_fw = 0, g_semC_fh = 0;
char g_semC_name[72] = {};

// ---- R63: THE TRANSPORT'S PER-FRAME DEPTH SOURCE ----
//
// Arm C refreshed its handle at the DUMP cadence - every five seconds, 1080
// times over R61's run. R57 listed "proven at one cadence is not proven at
// another" as unknown 4 and it is still open, so the transport does NOT reuse
// that path. It resolves the variable ONCE and then asks for its binding every
// frame, which is three virtual calls and no string work.
//
// get_texture_binding returning nothing IS the validity signal R61 required.
// There is no separate test and no heuristic: ReShade either has depth bound
// this frame or it does not.
//
// SELF-HEALING: an effect reload invalidates the variable handle. Rather than
// carry a stale one, an unbound frame clears it so the next frame re-resolves.
// That costs a short strcmp walk only while depth is absent, which is exactly
// when nothing else is happening.
reshade::api::effect_texture_variable g_tap_var = {};
std::atomic<unsigned long long> g_tap_res;
unsigned long long g_tap_tick = 0;

int      g_dcc_state  = 0;    // 0 idle, 1 copy recorded and ripening
unsigned long long g_dcc_at = 0;
unsigned long long g_dcc_copies = 0, g_dcc_reads = 0;
unsigned g_dcc_sent = 0, g_dcc_good = 0;
double   g_dcc_min = 0.0, g_dcc_max = 0.0, g_dcc_mean = 0.0;
bool     g_dcc_have = false;
int      g_dcc_hr = 0;

void sem_cb(reshade::api::effect_runtime *rt,
            reshade::api::effect_texture_variable var, void *)
{
    ++g_sem_seen;
    if (rt == nullptr || g_sem_n >= SEM_MAX) return;

    sem_row r = {};
    size_t ns = sizeof r.name;
    rt->get_texture_variable_name(var, r.name, &ns);
    r.name[sizeof r.name - 1] = '\0';

    reshade::api::resource_view srv = {}, srv_srgb = {};
    rt->get_texture_binding(var, &srv, &srv_srgb);
    r.srv = srv.handle;

    if (srv.handle != 0)
    {
        if (reshade::api::device *dv = rt->get_device())
        {
            const reshade::api::resource res = dv->get_resource_from_view(srv);
            r.res = res.handle;
            if (res.handle != 0)
            {
                const reshade::api::resource_desc rd = dv->get_resource_desc(res);
                r.width  = rd.texture.width;
                r.height = rd.texture.height;
                r.format = static_cast<unsigned int>(rd.texture.format);
            }
        }
    }

    // R55: remember the one that is a real, bound, depth-format resource.
    // This is what arm C copies from, refreshed every dump, so a resource
    // ReShade swaps out cannot be copied from after it is gone.
    if (r.res != 0 && strstr(r.name, SEMC_PREFER) != nullptr)
    {
        g_semC_pref = r.res; g_semC_pw = r.width; g_semC_ph = r.height;
    }
    else if (r.res != 0 && is_depth_format(r.format))
    {
        g_semC_fallback = r.res; g_semC_fw = r.width; g_semC_fh = r.height;
    }

    g_sem[g_sem_n++] = r;
}

// ---------------------------------------------------------------------------
// R53 - THE TECHNIQUE LANE: removing arm C's dependency on a human
// ---------------------------------------------------------------------------
//
// R49 measured that ReShade binds depth ONLY while an enabled technique
// consumes it, and R52 established that the only such effect on this rig is
// DisplayDepth, which replaces the picture. So arm C, as it stood, required a
// person to tick an unplayable shader on the GAME overlay - on a preset that
// AutoSavePreset=1 silently rewrites on exit.
//
// The pinned header removes that dependency:
//
//   virtual effect_technique find_technique(const char *effect_name,
//                                           const char *technique_name) = 0;
//   virtual void set_technique_state(effect_technique, bool enabled) = 0;
//   virtual void get_technique_effect_name(effect_technique, char *, size_t *) const = 0;
//
// So: enumerate every technique, log the lot, and switch our own tap on. The
// tap is mgpu_depth_tap.fx, which samples depth and discards every pixel.
//
// THE ENABLE HAPPENS AFTER THE ENUMERATION RETURNS, NOT INSIDE THE CALLBACK.
// set_technique_state only flips a flag and cannot invalidate ReShade's
// iteration - but doing it outside costs one find_technique and removes the
// question entirely, which is cheaper than being right about it.
//
// Re-attempted every dump rather than once: a preset switch or an effect
// reload turns techniques off again, and a lane that gives up after one try
// would report a binding that quietly stopped existing.

const char TAP_EFFECT[] = "mgpu_depth_tap";

struct tech_row
{
    char name[64];
    char effect[64];
    bool on;
};

const unsigned TECH_MAX = 16;
tech_row g_tech[TECH_MAX] = {};
unsigned g_tech_n = 0;
unsigned g_tech_seen = 0;
unsigned long long g_tap_enables = 0;   // times we had to switch it on
int      g_tap_state = -1;              // -1 absent, 0 present-off, 1 present-on
// R147. CONSECUTIVE SCANS THAT DID NOT END WITH THE TAP ON.
//
// One bad scan is not a fault. enumerate_techniques can return a partial list
// while ReShade is reloading effects, and find_technique can miss in the same
// window - either one leaves g_tap_state at -1 or 0 for ONE scan, and the next
// scan 300 frames later puts it back to 1. Reported straight through, that is
// a red ERROR 203 or 204 on screen for five seconds on a healthy run, which
// then "recovers" - exactly the behaviour the 12:28 build was shipped with and
// exactly what made three identical launches look like three different bugs.
//
// A fault that clears itself was never a fault. Two in a row, or nothing.
unsigned g_tap_bad_scans = 0;

void tech_cb(reshade::api::effect_runtime *rt,
             reshade::api::effect_technique t, void *)
{
    ++g_tech_seen;
    if (rt == nullptr || g_tech_n >= TECH_MAX) return;

    tech_row r = {};
    size_t n1 = sizeof r.name;
    rt->get_technique_name(t, r.name, &n1);
    size_t n2 = sizeof r.effect;
    rt->get_technique_effect_name(t, r.effect, &n2);
    r.name[sizeof r.name - 1] = '\0';
    r.effect[sizeof r.effect - 1] = '\0';
    r.on = rt->get_technique_state(t);
    g_tech[g_tech_n++] = r;
}

void tech_scan()
{
    if (g_rt == nullptr) return;
    reshade::api::effect_runtime *rt =
        static_cast<reshade::api::effect_runtime *>(g_rt);

    g_tech_n = 0;
    g_tech_seen = 0;
    g_tap_state = -1;
    rt->enumerate_techniques(nullptr, tech_cb, nullptr);

    for (unsigned i = 0; i < g_tech_n; ++i)
    {
        if (strstr(g_tech[i].effect, TAP_EFFECT) == nullptr) continue;
        g_tap_state = g_tech[i].on ? 1 : 0;
        if (g_tech[i].on) continue;

        const reshade::api::effect_technique t =
            rt->find_technique(g_tech[i].effect, g_tech[i].name);
        if (t.handle != 0)
        {
            rt->set_technique_state(t, true);
            g_tech[i].on = true;
            g_tap_state = 1;
            ++g_tap_enables;
        }
    }

    // R147. Counted per SCAN, not per frame: a scan is the only event that
    // can change the answer, and 300 frames apart is the cadence above.
    if (g_tap_state == 1) g_tap_bad_scans = 0;
    else                  ++g_tap_bad_scans;
}

void sem_scan()
{
    if (g_rt == nullptr) return;
    reshade::api::effect_runtime *rt =
        static_cast<reshade::api::effect_runtime *>(g_rt);
    g_sem_n    = 0;
    g_sem_seen = 0;
    g_sem_ran  = true;
    g_semC_pref = 0; g_semC_fallback = 0;
    rt->enumerate_texture_variables(nullptr, sem_cb, nullptr);

    // R60: the tap target wins when it exists. Falling back to the semantic
    // depth means the tap did not compile or was not enabled, and the R55 line
    // says which one was read so a log can never be ambiguous about it.
    if (g_semC_pref != 0)
    {
        g_semC_res.store(g_semC_pref, std::memory_order_relaxed);
        g_semC_w.store(g_semC_pw, std::memory_order_relaxed);
        g_semC_h.store(g_semC_ph, std::memory_order_relaxed);
        snprintf(g_semC_name, sizeof g_semC_name, "MGPU_DepthOutTex (tap target, output res)");
    }
    else if (g_semC_fallback != 0)
    {
        g_semC_res.store(g_semC_fallback, std::memory_order_relaxed);
        g_semC_w.store(g_semC_fw, std::memory_order_relaxed);
        g_semC_h.store(g_semC_fh, std::memory_order_relaxed);
        snprintf(g_semC_name, sizeof g_semC_name, "DepthBufferTex (FALLBACK - tap target absent)");
    }
    else
    {
        g_semC_res.store(0ull, std::memory_order_relaxed);
        snprintf(g_semC_name, sizeof g_semC_name, "NOTHING BOUND YET");
    }
}

void dump()
{
    const unsigned long long frames = g_frames.load(std::memory_order_relaxed);
    const double f = (frames != 0) ? (double)frames : 1.0;

    // R117. 1400 -> 4400. MEASURED, not precautionary: the R71 line has been
    // TRUNCATED in every log this project has collected. Its source ends with
    // "READ THE [R82] LINE BELOW INSTEAD" and that sentence has never once
    // reached a log file - the pointer to the line that actually answers the
    // question was the part being cut off.
    // R191: 4400 -> 5120. The candidate loop below stops once it is within 330
    // bytes of the end, and the tail literal is now ~2.4k, so a run with many
    // candidates could truncate the explanation the tail exists to carry.
    char line[5120];

    if (g_depth_on.load(std::memory_order_relaxed))
    {
        unsigned idx[8]; unsigned k = 0;
        top_n(g_depth, idx, k, 8);
        int w = snprintf(line, sizeof line,
            "[MGPU][P9.1] META SRC depth lane: %u candidate(s), %llu resource(s) examined, "
            "%.0f ns/frame of game-thread time. Ranked by OMSetRenderTargets binds; "
            "clears disambiguate the scene pass from shadow atlases. NOTHING IS BOUND OR "
            "TRANSPORTED - this is a look, not an acquisition.",
            g_depth.n.load(std::memory_order_relaxed),
            g_examined.load(std::memory_order_relaxed),
            (double)g_depth.ns.load(std::memory_order_relaxed) / f);
        for (unsigned i = 0; i < k && w > 0 && w < (int)sizeof line - 120; ++i)
        {
            const cand &c = g_depth.t[idx[i]];
            w += snprintf(line + w, sizeof line - (size_t)w,
                          " | #%u %ux%u %s binds=%llu clears=%llu", i,
                          c.width, c.height, fmt_name(c.format),
                          c.a.load(std::memory_order_relaxed),
                          c.b.load(std::memory_order_relaxed));
        }
        if (g_depth.n.load(std::memory_order_relaxed) == 0)
            snprintf(line + (w > 0 ? w : 0), sizeof line - (size_t)(w > 0 ? w : 0),
                     " | EMPTY. If examined is also 0 the subscription is not firing and the "
                     "game device was never identified; if examined is large the filter "
                     "rejected everything and the filter is wrong, not the game.");
        mgpu::diag::info(line);
    }

    if (g_mvec_on.load(std::memory_order_relaxed))
    {
        unsigned idx[8]; unsigned k = 0;
        top_n(g_mvec, idx, k, 8, /*by_b=*/true);

        // ---- R78: publish the transport's source ----
        //
        // The SAME rule R73 identified it by: top of the RTV-bind ranking that
        // also sits at the DISPLAY extent, which is what the game's own DLSS
        // telemetry reports as MVExtent. Re-picked every dump, so an eviction
        // or a resolution change moves it rather than stranding the transport
        // on a handle that no longer exists.
        {
            // ---- R83: RANKED BY BARRIERS, NOT BY RENDER-TARGET BINDS ----
            //
            // b is zero for every candidate in this title and always will be -
            // the buffer is a UAV, never a render target. e counts appearances
            // in a resource barrier, which is a per-frame usage signal that
            // does not care how the engine writes it. R83 measured 2.33 per
            // frame on the top candidate: written once by compute, read back.
            //
            // The display-extent filter stays. It is still the discriminator
            // that separates the buffer the game hands its own DLSS from the
            // render-extent intermediates, and the top candidate passes it.
            //
            // A direct scan rather than top_n, which sorts on a and b and
            // knows nothing about e.
            unsigned long long pick = 0, best = 0;
            double best_area = 0.0;
            {
                const unsigned cn3 = g_mvec.n.load(std::memory_order_acquire);
                for (unsigned i = 0; i < cn3; ++i)
                {
                    const cand &c = g_mvec.t[i];
                    if (c.handle == 0) continue;                   // evicted
                    const unsigned long long ee = c.e.load(std::memory_order_relaxed);
                    if (ee == 0) continue;
                    // ---- R98: A UNIFORM FRACTION, NOT AN EQUALITY ----
                    //
                    // Requiring c.width == g_scene_w assumed the velocity
                    // buffer sits at DISPLAY extent. Dragon Sword's does,
                    // because its own DLSS consumes it there. A Plague Tale
                    // renders at 2/3 and writes velocity at 1707x960 against a
                    // 2560x1440 swapchain, so every candidate failed this test
                    // and the arm held forever with a 20,503-barrier candidate
                    // sitting at the top of the list.
                    //
                    // Accept anything scaled UNIFORMLY on both axes, at half
                    // the scene or larger and never larger than it. Uniformity
                    // excludes the odd-shaped intermediates; the half-res floor
                    // excludes the small effect buffers.
                    if (g_scene_w == 0 || g_scene_h == 0) continue;
                    if (c.width == 0 || c.height == 0) continue;
                    {
                        const double fx = (double)c.width  / (double)g_scene_w;
                        const double fy = (double)c.height / (double)g_scene_h;
                        if (fx > 1.02 || fy > 1.02) continue;
                        if (fx < 0.50 || fy < 0.50) continue;
                        if (fx < fy * 0.98 || fx > fy * 1.02) continue;
                    }

                    // ---- AND THE LARGEST ACCEPTED WINS, NOT THE BUSIEST ----
                    //
                    // R91 loosened this filter and kept ranking on barrier
                    // COUNT. Dragon Sword's 1248x704 scratch target has 52,235
                    // against the correct buffer's 27,790, so it won and the
                    // title broke on contact. Count rewards being busy. On all
                    // three titles measured, the scene's velocity buffer is
                    // the LARGEST thing uniformly scaled from the scene:
                    //
                    //   Dragon Sword  1920x1080 over 1248x704
                    //   Plague Tale   1707x960  over 1280x720
                    //   Control       2560x1440 over 1280x720
                    //
                    // Count survives only to break a tie between equals.
                    const double area = (double)c.width * (double)c.height;
                    if (area > best_area || (area == best_area && ee > best))
                    { best_area = area; best = ee; pick = c.handle; }
                }
            }
            g_mvec_src.store(pick, std::memory_order_relaxed);
        }
        int w = snprintf(line, sizeof line,
            "[MGPU][P9.1] META SRC mvec lane: %u candidate(s), %.0f ns/frame over %llu frames. "
            "R72: RANKED AND SORTED BY RTV BINDS - how often each is RENDERED INTO, which is a "
            "per-frame usage signal. srvs is kept alongside as the old "
            "created over the resource. THIS DOES NOT IDENTIFY MOTION VECTORS - nothing in "
            "ReShade says which one they are - it says whether an MVec-shaped resource is "
            "visible from a subscriber at all. An empty list with a non-zero examined count "
            "is the result that says the NGX hook has to be earned or abandoned. "
            "root-descriptor SRV pushes=%llu (zero here with a non-zero SRV count above "
            "means this engine binds through descriptor TABLES and push_descriptors is "
            "blind in it - a fact about the engine, not a fault).",
            g_mvec.n.load(std::memory_order_relaxed),
            (double)g_mvec.ns.load(std::memory_order_relaxed) / f,
            frames,
            g_root_srv_pushes.load(std::memory_order_relaxed));
        for (unsigned i = 0; i < k && w > 0 && w < (int)sizeof line - 120; ++i)
        {
            const cand &c = g_mvec.t[idx[i]];
            w += snprintf(line + w, sizeof line - (size_t)w,
                          " | #%u %ux%u %s srvs=%llu RTVBINDS=%llu (%.2f/frame)%s", i,
                          c.width, c.height, fmt_name(c.format),
                          c.a.load(std::memory_order_relaxed),
                          c.b.load(std::memory_order_relaxed),
                          (double)c.b.load(std::memory_order_relaxed) / f,
                          (g_scene_w != 0 && c.width == g_scene_w &&
                           c.height == g_scene_h) ? " <-DISPLAY-EXTENT" : "");
        }
        mgpu::diag::info(line);

        // ---- R71: what the game's OWN DLSS says the extent is ----
        {
            unsigned sw[10] = {}, sh[10] = {}, sc[10] = {}, sn = 0, tot = 0;
            unsigned long long best_rtv = 0; unsigned best_i = 0; bool any = false;
            {
                std::lock_guard<std::mutex> lk(g_table_lock);
                const unsigned cn = g_mvec.n.load(std::memory_order_relaxed);
                tot = cn;
                for (unsigned i = 0; i < cn; ++i)
                {
                    const unsigned w2 = g_mvec.t[i].width, h2 = g_mvec.t[i].height;
                    const unsigned long long rb =
                        g_mvec.t[i].b.load(std::memory_order_relaxed);
                    if (rb > best_rtv) { best_rtv = rb; best_i = i; any = true; }
                    unsigned j = 0;
                    for (; j < sn; ++j) if (sw[j] == w2 && sh[j] == h2) break;
                    if (j == sn && sn < 10) { sw[sn] = w2; sh[sn] = h2; sc[sn] = 0; ++sn; }
                    if (j < 10 && j < sn) ++sc[j];
                }
            }
            int v = snprintf(line, sizeof line,
                "[MGPU][R71] MVEC SIZES over ALL %u candidate(s): ", tot);
            for (unsigned i = 0; i < sn && v > 0 && v < (int)sizeof line - 2600; ++i)
                v += snprintf(line + v, sizeof line - (size_t)v, "%ux%u x%u | ",
                              sw[i], sh[i], sc[i]);
            const size_t at = (v > 0) ? (size_t)v : 0;
            snprintf(line + at, sizeof line - at,
                "|| TRANSPORT SOURCE res=0x%llx (0 means nothing has supplied one yet - "
                "either R103 from the calibrator's own table, or a ranked candidate), handed over BY THE "
                "BARRIER HOOK %llu times, %llu second-binds-in-a-frame skipped. "
                "R191: THIS COUNTER IS ONE ROUTE, NOT THE LANE. It counts fire_mvec_hook only. A title "
                "whose vectors move by the EVALUATE route instead transports perfectly with this reading "
                "ZERO for an entire run - measured on RoboCop Rogue City 2026-09-21, where this said 0 in "
                "all 29 samples while the lane carried 2665 frames. ZERO HERE IS NOT EVIDENCE THAT NO "
                "VECTORS REACHED GPU 1, and it was read that way by two separate readers in one week. "
                "[R78] is the line that answers whether vectors reached the neural card; its producer "
                "copies= is a DIFFERENT counter with a similar name. || EVICTIONS: %llu resources destroyed and removed from the tables (R76 - a "
                "resolution change destroys every render target, and a stale entry is a copy "
                "from freed memory). RTV BINDS SEEN IN TOTAL: %llu. THE RANK ABOVE IS NOW RENDER TARGET BINDS, "
                "NOT SRV CREATIONS. R25 ranked by SRVs created, got 6/5/3/3, and called it \"a "
                "candidate, not an identification\" - that flatness was the SIGNAL being wrong, "
                "not the game. An SRV creation count is a LIFETIME number; a velocity buffer is "
                "RENDERED INTO once per frame, and RTVBINDS is that. THE BAND BEING MATCHED IS "
                "%ux%u, taken from the game's swapchain. The sentence that used to sit here "
                "quoted ColorExtentHeight=936 and MVExtentWidth=2560 as if they were live - they "
                "were hard-coded numbers from one 2026-09 Plague Tale run, printed on every "
                "title. Removed 2026-09-14: a log line must not state another game's telemetry "
                "as though it were this one's. With DLSS ON the render extent is BELOW the "
                "swapchain extent and every candidate moves with it, so a list that suddenly "
                "reads 1280x720 against a 1920x1080 window is the upscaler, not a regression. "
                "RTV BINDS SEEN = 0 with candidates present means THIS EVENT did not see it, "
                "and says nothing whatever about the other two. WHAT R103 CHANGED, AND WHAT IT "
                "DID NOT: when the calibrator has the game's table, TRANSPORT SOURCE above is "
                "the game's OWN MVec address and this whole ranking is bypassed. Measured "
                "2026-09-14 on Battlefield 6 - the source was exactly right and copies were "
                "still ZERO, because the barrier trigger never fired on it. THE ADDRESS AND THE "
                "MOMENT ARE DIFFERENT PROBLEMS. Everything above solves the address. READ THE "
                "[R82] LINE BELOW FOR THE MOMENT.",
                g_mvec_src.load(std::memory_order_relaxed),
                g_mvec_copies.load(std::memory_order_relaxed),
                g_mvec_skips.load(std::memory_order_relaxed),
                g_evicted.load(std::memory_order_relaxed),
                g_rtv_binds_seen.load(std::memory_order_relaxed), g_scene_w, g_scene_h);
            (void)any; (void)best_i; (void)best_rtv;
            mgpu::diag::info(line);

            // ---- R82: WHICH SIGNAL SEES THE VELOCITY BUFFER ----
            {
                char r2[3000];
                const unsigned long long fr2 = g_frames.load(std::memory_order_relaxed);
                const double barns = (fr2 != 0)
                    ? (double)g_bar_ns.load(std::memory_order_relaxed) / (double)fr2
                    : 0.0;
                int w2 = snprintf(r2, sizeof r2,
                    "[MGPU][R82] ACQUISITION SIGNALS - three of them, one run. || RTV VIEW "
                    "TABLE: %u of %u entries. THAT IS THE DENOMINATOR R80 COULD NOT SEE - a "
                    "bind count of zero against an empty table means nothing at all. || "
                    "OMSetRenderTargets: fired=%llu rtvs=%llu | matched via OUR TABLE=%llu | "
                    "matched via get_resource_from_view=%llu || BeginRenderPass: fired=%llu "
                    "targets=%llu matched=%llu || Barrier: fired=%llu resources=%llu "
                    "matched=%llu, costing %.0f ns/frame || ",
                    g_rtv_n.load(std::memory_order_relaxed), MAX_DSV,
                    g_bind_fires.load(std::memory_order_relaxed),
                    g_bind_rtvs.load(std::memory_order_relaxed),
                    g_bind_via_table.load(std::memory_order_relaxed),
                    g_bind_via_api.load(std::memory_order_relaxed),
                    g_rp_fires.load(std::memory_order_relaxed),
                    g_rp_rts.load(std::memory_order_relaxed),
                    g_rp_matched.load(std::memory_order_relaxed),
                    g_bar_fires.load(std::memory_order_relaxed),
                    g_bar_res.load(std::memory_order_relaxed),
                    g_bar_matched.load(std::memory_order_relaxed),
                    barns);

                const unsigned cn2 = g_mvec.n.load(std::memory_order_acquire);
                unsigned shown = 0;
                for (unsigned i = 0; i < cn2 && shown < 6 && w2 > 0 &&
                                     w2 < (int)sizeof r2 - 900; ++i)
                {
                    const cand &q = g_mvec.t[i];
                    const unsigned long long qc = q.c.load(std::memory_order_relaxed);
                    const unsigned long long qd = q.d.load(std::memory_order_relaxed);
                    const unsigned long long qe = q.e.load(std::memory_order_relaxed);
                    if (qc == 0 && qd == 0 && qe == 0) continue;
                    ++shown;
                    w2 += snprintf(r2 + w2, sizeof r2 - (size_t)w2,
                                   "#%u %ux%u rtv=%llu rp=%llu bar=%llu(0x%X->0x%X) | ",
                                   i, q.width, q.height, qc, qd, qe,
                                   q.last_old.load(std::memory_order_relaxed),
                                   q.last_new.load(std::memory_order_relaxed));
                }
                if (shown == 0 && w2 > 0 && w2 < (int)sizeof r2 - 900)
                    w2 += snprintf(r2 + w2, sizeof r2 - (size_t)w2,
                                   "NO CANDIDATE SEEN BY ANY OF THE THREE. | ");

                const size_t a2 = (w2 > 0) ? (size_t)w2 : 0;
                snprintf(r2 + a2, sizeof r2 - a2,
                    "|| HOW TO READ IT. via TABLE=0 while via get_resource_from_view is "
                    "non-zero: the table was the whole bug and R76 through R81 were spent on a "
                    "cache we never needed. BeginRenderPass fired while OMSetRenderTargets did "
                    "not: this engine uses render passes and we were subscribed to the wrong "
                    "event. Barrier matched with a per-frame count near the frame count: use "
                    "THAT permanently - it needs no view, no table and no binding-path "
                    "assumption, and 0x4->0xC0 (render_target -> shader_resource) is the moment "
                    "AFTER the pass wrote, which is the correct moment to copy and is one the "
                    "bind events cannot offer. NOTE THE STALENESS R78 DID NOT STATE: both bind "
                    "events fire BEFORE the pass runs, so a copy taken there carries the "
                    "PREVIOUS frame's vectors. All three silent with candidates present is the "
                    "reading that made NGX hooking the honest next step - and it was taken: "
                    "R101 hooks NGX and MvecFromEval=1 copies from the evaluate. MEASURED "
                    "2026-09-14 on Battlefield 6 with DLSS on: that route carried 1931 of 2705 "
                    "frames while this barrier route carried ZERO on the same run with the "
                    "SAME correct source address. On this engine the bind events are blind and "
                    "the barrier trigger does not fire, so the evaluate is not a fallback here, "
                    "it is the only path. "
                    "Two of the barriers per frame on the published source are OURS.");
                mgpu::diag::info(r2);

                // SL1. Said once, and only once a lane has fired, so an
                // early dump reports earliness rather than blindness.
                mgpu::slprobe::report_acquisition(
                    g_bind_fires.load(std::memory_order_relaxed),
                    g_rp_fires.load(std::memory_order_relaxed),
                    g_bar_fires.load(std::memory_order_relaxed));

                // SLT1. Installed from HERE rather than from the add-on's
                // one-shot, and that placement is the safeguard rather than a
                // convenience: this site only runs once the title has been
                // rendering for a while, so the tap cannot exist during the
                // startup window that is the only place the sl.common fault
                // has ever been seen. tick() defers again on its own count,
                // so both ends of the rule are enforced where they are read.
                mgpu::sltags::tick(g_frames.load(std::memory_order_relaxed),
                                   g_sltags.load(std::memory_order_relaxed));
                mgpu::sltags::report();
            }
        }

        // ---- R74: choose the target, and report what is in it ----
        if (g_mv_mode.load(std::memory_order_relaxed) != 0)
        {
            const unsigned want = g_mv_index.load(std::memory_order_relaxed);
            if (want < k)
            {
                const cand &t = g_mvec.t[idx[want]];
                g_mv_target.store(t.handle, std::memory_order_relaxed);
                g_mv_w.store(t.width, std::memory_order_relaxed);
                g_mv_h.store(t.height, std::memory_order_relaxed);
                g_mv_fmt.store(t.format, std::memory_order_relaxed);
            }

            // COHERENCE. Camera motion moves all four patches the same way; a
            // character animating moves one. This is the number that replaces
            // the still-camera baseline Marcelo killed.
            int agree = 0;
            if (g_mv_have)
            {
                double ax = 0.0;
                for (unsigned q = 0; q < MV_N; ++q) ax += g_mv_px[q];
                const double sgn = (ax >= 0.0) ? 1.0 : -1.0;
                for (unsigned q = 0; q < MV_N; ++q)
                    if (g_mv_px[q] * sgn > 0.0) ++agree;
            }

            snprintf(line, sizeof line,
                "[MGPU][R75] MVEC CONTENT of #%u (res=0x%llx %ux%u %s): copies=%llu reads=%llu "
                "hr=0x%08x | FOUR 32x32 patches at 20/35, 80/35, 20/72, 80/72 percent of the "
                "frame | P0 x=%+.4f y=%+.4f cov=%u%% | P1 x=%+.4f y=%+.4f cov=%u%% | P2 x=%+.4f "
                "y=%+.4f cov=%u%% | P3 x=%+.4f y=%+.4f cov=%u%% | worst magnitude anywhere=%.4f "
                "| UNWRITTEN %u of %u texels still hold the sentinel - ANY of these and the "
                "COPY DID NOT LAND, which is a different failure from a still world and used to "
                "look identical to it | X-SIGN AGREEMENT %d of 4. TURN THE CAMERA LEFT, THEN RIGHT - that is the "
                "whole test, and it needs no still camera and no contrived scene. A VELOCITY "
                "FIELD MOVES ALL FOUR PATCHES AT ONCE, same sign, and the sign FLIPS when the "
                "turn reverses: nothing else on the screen does that. One patch moving while "
                "three do not is the character animating and is NOT the discriminator. No "
                "response to the turn at all means this is not the velocity field whatever its "
                "bind count says - then MVecProbeIndex=1 points at the render-extent buffer. "
                "THE UNITS ARE IN THE MAGNITUDE: tens are PIXELS, under one is NDC or UV, and "
                "that is what decides MVecScaleX/Y. Read at a RENDER TARGET BIND, before the "
                "pass draws, so these are the PREVIOUS frame's finished vectors.",
                want, g_mv_target.load(std::memory_order_relaxed),
                g_mv_w.load(std::memory_order_relaxed),
                g_mv_h.load(std::memory_order_relaxed),
                fmt_name(g_mv_fmt.load(std::memory_order_relaxed)),
                g_mv_copies.load(std::memory_order_relaxed),
                g_mv_reads.load(std::memory_order_relaxed),
                (unsigned)g_mv_hr.load(std::memory_order_relaxed),
                g_mv_px[0], g_mv_py[0], g_mv_pcov[0],
                g_mv_px[1], g_mv_py[1], g_mv_pcov[1],
                g_mv_px[2], g_mv_py[2], g_mv_pcov[2],
                g_mv_px[3], g_mv_py[3], g_mv_pcov[3],
                g_mv_magmax,
                g_mv_have ? g_mv_sent : 0u, (unsigned)(MV_PATCH * MV_PATCH * MV_N),
                agree);
            mgpu::diag::info(line);
        }
    }

    // ---- R30b: the clear cadence of the #1 depth candidate ----
    if (g_depth_on.load(std::memory_order_relaxed))
    {
        unsigned long long h[4], hw[4];
        for (unsigned i = 0; i < 4; ++i)
        {
            h[i]  = g_clr_hist[i].load(std::memory_order_relaxed);
            hw[i] = h[i] - g_prev_hist[i];
            g_prev_hist[i] = h[i];
        }
        const unsigned long long gn  = g_gap_n.load(std::memory_order_relaxed);
        const unsigned long long gs  = g_gap_sum_ns.load(std::memory_order_relaxed);
        const unsigned long long gnw = gn - g_prev_gap_n;
        const unsigned long long gsw = gs - g_prev_gap_sum;
        g_prev_gap_n = gn; g_prev_gap_sum = gs;

        const double mean_us  = (gn  != 0) ? (double)gs  / (double)gn  / 1000.0 : 0.0;
        const double meanw_us = (gnw != 0) ? (double)gsw / (double)gnw / 1000.0 : 0.0;
        const unsigned long long wmin = g_gap_w_min.exchange(~0ull, std::memory_order_relaxed);
        const unsigned long long wmax = g_gap_w_max.exchange(0ull,  std::memory_order_relaxed);
        const double minw_us = (gnw != 0 && wmin != ~0ull) ? (double)wmin / 1000.0 : 0.0;
        const double maxw_us = (gnw != 0) ? (double)wmax / 1000.0 : 0.0;

        snprintf(line, sizeof line,
            "[MGPU][P9.1] CLEAR CADENCE of depth #0 (handle 0x%llX) | THIS WINDOW clears/frame "
            "0:%llu 1:%llu 2:%llu 3+:%llu, finish->next-clear n=%llu mean=%.0f min=%.0f "
            "max=%.0f us | LIFETIME 0:%llu 1:%llu 2:%llu 3+:%llu, n=%llu mean=%.0f us. "
            "READ THE WINDOW, NOT THE LIFETIME: the lifetime figures include the main menu "
            "and every loading screen since process start, and this lateral's decision rule "
            "is sensitive to exactly that contamination. IN A GAMEPLAY WINDOW: a clean 1: "
            "means the clear is a reliable per-frame tick and depth can be paired with the "
            "colour of the frame that wrote it. ANY weight in 2: or 3+: means a design that "
            "signalled at the clear would signal TWICE in a frame - a producer frame-index "
            "desync, and that design is then dead for a reason in this log rather than in an "
            "argument. Weight in 0: is a frame with no clear at all, which is the case a "
            "fallback has to cover. The gap SPREAD, not its mean, is the jitter such a "
            "design would have put on the production clock. NOTHING IS COPIED OR BOUND - "
            "this is still a look.",
            g_top_depth.load(std::memory_order_relaxed),
            hw[0], hw[1], hw[2], hw[3], gnw, meanw_us, minw_us, maxw_us,
            h[0], h[1], h[2], h[3], gn, mean_us);
        mgpu::diag::info(line);

        // ---- R31: what SHAPE those clears are, and how deep the pool is ----
        static unsigned long long p_d = 0, p_s = 0, p_b = 0, p_pool[5] = {};
        const unsigned long long cd = g_clr_depth_only.load(std::memory_order_relaxed);
        const unsigned long long cs = g_clr_stencil_only.load(std::memory_order_relaxed);
        const unsigned long long cb = g_clr_both.load(std::memory_order_relaxed);
        unsigned long long pl[5], plw[5];
        for (unsigned i = 0; i < 5; ++i)
        {
            pl[i] = g_pool_hist[i].load(std::memory_order_relaxed);
            plw[i] = pl[i] - p_pool[i];
            p_pool[i] = pl[i];
        }
        snprintf(line, sizeof line,
            "[MGPU][P9.1] CLEAR SHAPE (every depth-stencil clear, all candidates) | THIS "
            "WINDOW depth-only=%llu stencil-only=%llu both=%llu | LIFETIME depth-only=%llu "
            "stencil-only=%llu both=%llu || POOL, distinct scene-depth candidates cleared per "
            "frame, THIS WINDOW 0:%llu 1:%llu 2:%llu 3:%llu 4+:%llu | LIFETIME 0:%llu 1:%llu "
            "2:%llu 3:%llu 4+:%llu. WHAT THESE DECIDE: if the pairs seen in CLEAR CADENCE are "
            "one DEPTH clear plus one STENCIL clear on the same R32G8 resource, then depth is "
            "cleared ONCE per turn and clear-anchored ACQUISITION is alive - stencil-only "
            "being non-zero and roughly equal to depth-only is that signature. If instead "
            "both/depth-only carry it in pairs, the resource really is depth-cleared twice a "
            "frame. THE POOL HISTOGRAM says how many different scene-resolution depth textures "
            "the engine touches in one frame: a clean 1: means it rotates one per frame and a "
            "per-frame selection is enough; 2: or more means several are live at once and "
            "selection has to be by ROLE rather than by identity.",
            cd - p_d, cs - p_s, cb - p_b, cd, cs, cb,
            plw[0], plw[1], plw[2], plw[3], plw[4],
            pl[0], pl[1], pl[2], pl[3], pl[4]);
        p_d = cd; p_s = cs; p_b = cb;
        mgpu::diag::info(line);

        // ---- R33: is there a LATE, state-known bind of the turn holder? ----
        static unsigned long long p_bh[5] = {}, p_lbn = 0, p_lbs = 0, p_cfn = 0, p_cfs = 0;
        unsigned long long bh[5], bhw[5];
        for (unsigned i = 0; i < 5; ++i)
        {
            bh[i] = g_bind_hist[i].load(std::memory_order_relaxed);
            bhw[i] = bh[i] - p_bh[i];
            p_bh[i] = bh[i];
        }
        const unsigned long long lbn = g_lb_n.load(std::memory_order_relaxed);
        const unsigned long long lbs = g_lb_sum.load(std::memory_order_relaxed);
        const unsigned long long cfn = g_cf_n.load(std::memory_order_relaxed);
        const unsigned long long cfs = g_cf_sum.load(std::memory_order_relaxed);
        const double lb_w = (lbn > p_lbn) ? (double)(lbs - p_lbs) / (double)(lbn - p_lbn) / 1000.0 : 0.0;
        const double lb_l = (lbn != 0) ? (double)lbs / (double)lbn / 1000.0 : 0.0;
        const double cf_w = (cfn > p_cfn) ? (double)(cfs - p_cfs) / (double)(cfn - p_cfn) / 1000.0 : 0.0;
        const double lb_mn = (lbn != 0 && g_lb_min.load(std::memory_order_relaxed) != ~0ull)
            ? (double)g_lb_min.load(std::memory_order_relaxed) / 1000.0 : 0.0;
        const double lb_mx = (double)g_lb_max.load(std::memory_order_relaxed) / 1000.0;
        p_lbn = lbn; p_lbs = lbs; p_cfn = cfn; p_cfs = cfs;

        snprintf(line, sizeof line,
            "[MGPU][P9.1] BIND TIMING of the turn holder (the scene-res resource that took a "
            "DEPTH clear this frame) | binds AFTER that clear, THIS WINDOW 0:%llu 1:%llu 2:%llu "
            "3:%llu 4+:%llu | LIFETIME 0:%llu 1:%llu 2:%llu 3:%llu 4+:%llu | LAST BIND -> "
            "finish_effects: window mean=%.2f, lifetime n=%llu mean=%.2f min=%.2f max=%.2f US | "
            "depth clear -> finish_effects: window mean=%.2f US (MICROseconds - the sums are "
            "nanoseconds and these are divided by 1000; the first build of this line labelled "
            "them ms and was wrong by 1000x). THIS IS THE DESIGN DECISION. "
            "Every bind is as state-known as a clear - D3D12 requires DEPTH_WRITE/READ for a "
            "bound depth-stencil target - so a bind LATE in the frame is a moment where the "
            "depth is both current AND safe to transition. A SMALL last-bind gap (a few ms, "
            "well under the clear->finish span) means such a moment exists and depth can be "
            "acquired FRESH: the two-frame staleness R32 found is then avoidable. A gap close "
            "to the clear->finish span means every bind sits at the START of the turn, the "
            "depth is still being written at all of them, and two-frames-old is the ceiling "
            "for this route. 0: in the histogram means the turn holder was never re-bound "
            "after its clear at all. TURNS REFUSED BY THE SIZE BAND: %llu - if the "
            "histogram is all zero and this is large, the band is wrong and the answer "
            "here is not about the game. Check the SIZE BAND line at startup.",
            bhw[0], bhw[1], bhw[2], bhw[3], bhw[4],
            bh[0], bh[1], bh[2], bh[3], bh[4],
            lb_w, lbn, lb_l, lb_mn, lb_mx, cf_w,
            g_turn_rejected.load(std::memory_order_relaxed));
        mgpu::diag::info(line);

        // ---- R37: A versus B, the still-camera comparison ----
        const int dcm = g_dc_mode.load(std::memory_order_relaxed);
        if (dcm != 0)
        {
            const unsigned long long dn = g_dc_n.load(std::memory_order_relaxed);
            snprintf(line, sizeof line,
                "[MGPU][R37] DEPTH COMPARE mode %d, bind target N=%u | pairs compared=%llu, "
                "identical=%llu | mean |A-B| over the 64x64 patch = %.9f, worst single texel "
                "= %.9f. A IS THE CLEAR-ANCHORED COPY (two turns old, state entailed); B IS "
                "THE Nth-BIND COPY (current frame, state equally entailed). WITH THE CAMERA "
                "HELD STILL depth(N) == depth(N-2), so A IS THE CORRECT CURRENT DEPTH and this "
                "difference is B's error against a known-good reference: near zero means B "
                "captures COMPLETE depth and fresh acquisition is available, a large value "
                "means depth is still being written at bind N and two-frames-old is the "
                "ceiling. THEN MOVE THE CAMERA: the same number becomes how far apart a "
                "two-frame-old depth and a current one actually are, which is the cost of "
                "choosing A. Mode 1 records A only and compares nothing - it exists to prove "
                "the copy and the barrier are sound before B is switched on. Sampled one "
                "patch every %llu frames, read back %llu frames later, unsynchronised on "
                "purpose.",
                dcm, g_dc_target_bind, dn, g_dc_zero.load(std::memory_order_relaxed),
                (dn != 0) ? g_dc_sum / (double)dn : 0.0, g_dc_max,
                SAMPLE_EVERY, READ_AFTER);
            mgpu::diag::info(line);
            snprintf(line, sizeof line,
                "[MGPU][R37] DEPTH COMPARE why: armed=%llu A-fired=%llu B-fired=%llu "
                "readback-buffers=%s (CreateCommittedResource hr=0x%08X). READ THIS BEFORE "
                "READING pairs=0 AS A RESULT. armed=0 means the sampler never ran. "
                "buffers=NO means there was nothing to copy into and neither arm could fire - "
                "check the hr. A-fired=0 with buffers=YES means no scene-resolution DEPTH "
                "clear was seen, which contradicts the CLEAR CADENCE line and would be a bug "
                "here. A-fired>0 with B-fired=0 means arm B never reached its target bind "
                "index, so the target is wrong for this scene rather than the copy being "
                "broken.",
                g_dc_armed.load(std::memory_order_relaxed),
                g_dc_arm_a.load(std::memory_order_relaxed),
                g_dc_arm_b.load(std::memory_order_relaxed),
                (g_dc_rb[0] != nullptr && g_dc_rb[1] != nullptr) ? "YES" : "NO",
                (unsigned)g_dc_buf_hr.load(std::memory_order_relaxed));
            mgpu::diag::info(line);
        }

        // ---- R53: the technique lane, and switching our own tap on. ----
        tech_scan();
        {
            int v = snprintf(line, sizeof line,
                "[MGPU][R53] TECHNIQUE LANE: %u technique(s) across all loaded effects, %u "
                "recorded. TAP = %s. We enable mgpu_depth_tap.fx OURSELVES every dump, "
                "because ReShade binds depth only while an enabled technique consumes it "
                "(R49) and the only other consumer on this rig replaces the picture (R52). "
                "Times we had to switch it on so far: %llu - a number that keeps rising "
                "means something is turning it back off, which is a finding, not noise.",
                g_tech_seen, g_tech_n,
                (g_tap_state < 0) ? "ABSENT - mgpu_depth_tap.fx is not in the shader path, "
                                    "or failed to compile: check the compile lines above"
                                  : (g_tap_state == 1 ? "ON" : "PRESENT BUT OFF - set_technique_state "
                                                              "did not take, which is the bug to chase"),
                g_tap_enables);
            for (unsigned i = 0; i < g_tech_n && v > 0 && v < (int)sizeof line - 120; ++i)
                v += snprintf(line + v, sizeof line - (size_t)v, " | %s:%s %s",
                              g_tech[i].effect, g_tech[i].name,
                              g_tech[i].on ? "ON" : "off");
            mgpu::diag::info(line);
        }

        // ---- R53: what sizes the game's depth candidates actually are. ----
        {
            unsigned sw[10] = {}, sh[10] = {}, sc[10] = {}, sn = 0, total = 0;
            bool at_scene = false;
            {
                std::lock_guard<std::mutex> lk(g_table_lock);
                const unsigned cn = g_depth.n.load(std::memory_order_relaxed);
                total = cn;
                for (unsigned i = 0; i < cn; ++i)
                {
                    const unsigned w2 = g_depth.t[i].width, h2 = g_depth.t[i].height;
                    if (w2 == g_scene_w && h2 == g_scene_h && g_scene_w != 0) at_scene = true;
                    unsigned j = 0;
                    for (; j < sn; ++j) if (sw[j] == w2 && sh[j] == h2) break;
                    if (j == sn && sn < 10) { sw[sn] = w2; sh[sn] = h2; sc[sn] = 0; ++sn; }
                    if (j < 10 && j < sn) ++sc[j];
                }
            }
            int v = snprintf(line, sizeof line,
                "[MGPU][R53] DEPTH SIZES over ALL %u candidate(s), not the top 8: ", total);
            for (unsigned i = 0; i < sn && v > 0 && v < (int)sizeof line - 200; ++i)
                v += snprintf(line + v, sizeof line - (size_t)v, "%ux%u x%u | ",
                              sw[i], sh[i], sc[i]);
            snprintf(line + (v > 0 ? v : 0), sizeof line - (size_t)(v > 0 ? v : 0),
                "|| ANY AT THE SWAPCHAIN SIZE %ux%u: %s. THIS IS THE (c) QUESTION FROM R50. "
                "NO means the game keeps no depth at output resolution and the choice is "
                "between upsampling depth to the model or moving the model to the render "
                "resolution - it is not a search for a buffer that does not exist. Only the "
                "first 10 distinct sizes are listed; the count is over all of them.",
                g_scene_w, g_scene_h, at_scene ? "YES" : "NO");
            mgpu::diag::info(line);
        }

        // ---- R48: arm C. What ReShade's own DEPTH semantic is bound to. ----
        sem_scan();
        {
            int v = snprintf(line, sizeof line,
                "[MGPU][R48] SEMANTIC LANE (arm C): %u texture variable(s) walked across ALL "
                "loaded effects, %u recorded. ReShade PUSHES the depth it detected into every "
                "variable declared ': DEPTH', so a variable bound to a scene-sized "
                "depth-format resource IS that semantic - NO effect name and NO variable name "
                "is guessed here, the binding is read and the resource is asked what it is. "
                "MATCH means the resolved resource is one of the raw lane's own top-8 depth "
                "candidates: arm A and arm C pointing at the same buffer.",
                g_sem_seen, g_sem_n);
            for (unsigned i = 0; i < g_sem_n && v > 0 && v < (int)sizeof line - 160; ++i)
            {
                const sem_row &r = g_sem[i];

                // R53: search the WHOLE candidate table, not the top 8, and
                // report the entry's own counters. This is the copy-versus-live
                // discriminator R51 asked for:
                //   in the table with binds in the thousands -> the game's own
                //     buffer, and acquiring it means transitioning a resource
                //     the game is still using.
                //   in the table with binds = 0 -> the game never renders into
                //     it. ReShade made it.
                //   not in the table at all -> created without depth_stencil
                //     usage, which is what this lane filters on. ReShade made it.
                unsigned long long mb = 0, mc = 0;
                bool in_table = false;
                if (r.res != 0)
                {
                    std::lock_guard<std::mutex> lk(g_table_lock);
                    const unsigned cn = g_depth.n.load(std::memory_order_relaxed);
                    for (unsigned j = 0; j < cn; ++j)
                        if (g_depth.t[j].handle == r.res)
                        {
                            in_table = true;
                            mb = g_depth.t[j].a.load(std::memory_order_relaxed);
                            mc = g_depth.t[j].b.load(std::memory_order_relaxed);
                            break;
                        }
                }
                char verdict[96];
                if (r.res == 0)
                    snprintf(verdict, sizeof verdict, "unbound");
                else if (!in_table)
                    snprintf(verdict, sizeof verdict, "NOT-IN-TABLE -> RESHADE'S OWN");
                else if (mb == 0)
                    snprintf(verdict, sizeof verdict, "in-table binds=0 -> RESHADE'S OWN");
                else
                    snprintf(verdict, sizeof verdict,
                             "in-table binds=%llu clears=%llu -> THE GAME'S LIVE BUFFER", mb, mc);

                v += snprintf(line + v, sizeof line - (size_t)v,
                              " | '%s' res=0x%llx %ux%u %s [%s]",
                              r.name, r.res, r.width, r.height,
                              fmt_name(r.format), verdict);
            }
            const size_t at = (v > 0) ? (size_t)v : 0;
            if (!g_sem_ran)
                snprintf(line + at, sizeof line - at,
                         " | NOT RUN: no game effect_runtime ever reached the probe, so "
                         "note_effects is not being called from on_reshade_finish_effects.");
            else if (g_sem_seen == 0)
                snprintf(line + at, sizeof line - at,
                         " | ZERO VARIABLES: the enumeration RAN and found none, which means no "
                         "effect is loaded on the GAME runtime. This rig compiles "
                         "DisplayDepth.fx; enabling any depth-using effect is what makes the "
                         "semantic exist. An empty list here is about the preset, not the API.");
            else if (g_sem_n != 0)
                snprintf(line + at, sizeof line - at,
                         " || READ IT LIKE THIS: a row with res=0x0 is a variable ReShade has "
                         "bound to nothing, which for a DEPTH variable means ReShade's own "
                         "detection found no depth in this game - and that is arm C's answer, "
                         "not a probe failure. A row with a depth format at scene size and no "
                         "MATCH means ReShade picked a DIFFERENT buffer than our raw lane, "
                         "which is the disagreement worth having.");
            mgpu::diag::info(line);
        }

        // ---- R55: arm C. Did the copy from ReShade's resource work at all? ----
        if (g_dc_mode.load(std::memory_order_relaxed) == 3)
        {
            snprintf(line, sizeof line,
                "[MGPU][R55] ARM C reading %s: copies recorded=%llu, readbacks=%llu, source=%ux%u | "
                "of %u texels %u never written (still the sentinel) and %u are real | "
                "depth min=%.6f mean=%.6f max=%.6f | map hr=0x%08x. THE GAME SURVIVING THIS "
                "RUN IS HALF THE RESULT: it means resource_usage::shader_resource -> "
                "copy_source is the right transition for ReShade's own depth resource and "
                "this add-on can read it. THE OTHER HALF IS THE RANGE: values spread between "
                "0 and 1 are real reversed-Z depth with geometry in the patch. All exactly "
                "0.0 or all exactly 1.0 is a cleared or empty buffer and the copy is landing "
                "somewhere with nothing in it. Sentinels above zero mean the copy did not "
                "cover the patch. copies=0 with the tap ON means ReShade had bound nothing "
                "yet - see the SEMANTIC LANE line above, and give it a scene. READING THE TAP TARGET IS THE POINT OF THIS BUILD: it is what the transport will carry, it is at OUTPUT resolution already, and it is written by a shader whose output nobody has checked.",
                (g_semC_name[0] != '\0') ? g_semC_name : "nothing",
                g_dcc_copies, g_dcc_reads,
                g_semC_w.load(std::memory_order_relaxed),
                g_semC_h.load(std::memory_order_relaxed),
                (unsigned)(DC_PATCH * DC_PATCH), g_dcc_sent, g_dcc_good,
                g_dcc_have ? g_dcc_min : 0.0,
                g_dcc_have ? g_dcc_mean : 0.0,
                g_dcc_have ? g_dcc_max : 0.0,
                (unsigned)g_dcc_hr);
            mgpu::diag::info(line);
        }
    }
}

// ---------------------------------------------------------------------------
// mgpu.ini
// ---------------------------------------------------------------------------
//
// This is a second READ of the file, not a second PARSER: the syntax comes
// from mgpu::config::find, the same primitive gpu1_context uses, so the two
// cannot disagree about what a key looks like. The path resolution mirrors
// gpu1_context's ini_path for the same reason DEFECT G exists - a bare
// relative path resolves against the process CWD, which is not ours and not
// stable per title.

const wchar_t *ini_path()
{
    static wchar_t path[1024];
    static bool done = false;
    if (done) return path;
    done = true;
    path[0] = L'\0';

    HMODULE h = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&ini_path), &h) != FALSE && h != nullptr)
    {
        wchar_t mod[1024];
        const DWORD got = GetModuleFileNameW(h, mod, 1024);
        if (got > 0 && got < 1024)
        {
            size_t cut = 0;
            for (size_t i = 0; mod[i] != L'\0'; ++i)
                if (mod[i] == L'\\' || mod[i] == L'/') cut = i + 1;
            if (cut > 0 && cut + 10 < 1024)
            {
                for (size_t i = 0; i < cut; ++i) path[i] = mod[i];
                const wchar_t *nm = L"mgpu.ini";
                size_t j = cut;
                for (size_t i = 0; nm[i] != L'\0'; ++i) path[j++] = nm[i];
                path[j] = L'\0';
            }
        }
    }
    return path;
}

bool slurp(char *buf, size_t cap)
{
    const wchar_t *p = ini_path();
    FILE *fp = nullptr;
    if (p[0] != L'\0') fp = _wfopen(p, L"rb");
    if (fp == nullptr) fp = fopen("mgpu.ini", "rb");
    if (fp == nullptr) return false;

    const size_t got = fread(buf, 1, cap - 1, fp);
    fclose(fp);
    buf[got] = '\0';
    return mgpu::config::validate(buf, got) == mgpu::config::validation::ok;
}

} // namespace

// ---------------------------------------------------------------------------
// Public
// ---------------------------------------------------------------------------

// DEFECT P9.1-A, found before this code ever ran and fixed here.
//
// These keys were called Probe and ProbeLogSeconds. mgpu.ini has carried a key
// called PROBES since P1.3 - "Probes=1 runs the one-shot probe chain INSTEAD of
// the stream" - and Probe against Probes is one character, in a file a person
// edits by hand.
//
// The parser survives it. mgpu::config::find requires '=' immediately after the
// key, so "Probes=both" cannot match the key "Probe" and each line resolves to
// its own setting. That was verified against the real file, not reasoned about.
//
// The PERSON did not survive it. On the first attempt to use this the operator
// set Probes=both intending to turn this on; ini_read_probes reads Probes as
// (*k == '1'), so "both" was silently false, and a correctly formatted line
// accepted without complaint did nothing at all. That is this project's
// section 00 failure in its purest form, and a parser being technically correct
// is no defence when the file is a human interface.
//
// So: MetaProbe and MetaProbeLogSeconds. META because it is what the log line
// these produce is called, and because nothing else in the file starts that way.
int calib_mode()
{
    // PACKED, and packed rather than given a second accessor because
    // dllmain's one install() line is the only consumer and a second
    // argument there would mean editing a file this round does not
    // otherwise touch. Low byte is the capture mode, exactly as
    // before; bits 8-11 carry the rung. With CalibRung absent the top
    // bits are zero and the value is literally the old value.
    return g_calib.load(std::memory_order_relaxed) |
           (g_calib_rung.load(std::memory_order_relaxed) << 8) |
           (g_calib_probe.load(std::memory_order_relaxed) << 12);
}

int jitter_mode()
{
    return g_jitter.load(std::memory_order_relaxed);
}

int eval_copy_mode()
{
    return g_evalcopy.load(std::memory_order_relaxed);
}

// R180/R182. -2 absent (AUTO), 0 explicitly off, 1 on.
int sf_path_mode()
{
    return g_sfpath.load(std::memory_order_relaxed);
}

// ---- R103: THE CALIBRATOR WINS ----
//
// MEASURED, Dragon Sword: EVICTIONS: 52. The game cycles a POOL of velocity
// buffers and every re-arm landed this file's ranking on a different member
// than the one the game actually feeds DLSS. The two handles agreed at
// startup - which is why the first minutes looked right - and had diverged by
// the end of the run. No amount of barrier counting wins against a pool of
// identical buffers, because they are identical BY CONSTRUCTION.
//
// Asserted every frame from the frame tick rather than folded into dump():
// dump() runs on a seconds cadence and would leave a window where its own
// pick was live. Re-asserting per frame closes that to at most one frame, and
// costs one relaxed store.
void set_mvec_override(unsigned long long handle)
{
    if (handle != 0) g_mvec_src.store(handle, std::memory_order_relaxed);
}

mode mode_from_ini()
{
    static char buf[mgpu::config::MAX_BYTES + 1];

    // R77. A FAILED READ IS NOT AN ANSWER OF "OFF".
    //
    // This returned mode::off when slurp failed, and set_mode(off) unregisters
    // every event. Two things fell out of that, both measured:
    //
    //   ATTACH   the add-on attaches four times and the first three cannot
    //            resolve the ini path yet. Three reads of "off" while the
    //            engine allocated its render targets, which is why the tables
    //            held 3 mvec candidates against 53 (R76).
    //   RESIZE   a resolution change re-inits the swapchain, that path re-reads
    //            the ini, the read fails the same way, and the probe TURNED
    //            ITSELF OFF at 00:46:46 - at the exact moment the new render
    //            targets were being created. The workaround for the first
    //            problem was therefore triggering the second one every time.
    //
    // "I could not read the file" and "the file says off" are different facts
    // and only one of them is a user's intent. Keep what we have.
    if (!slurp(buf, sizeof buf))
    {
        static bool said = false;
        if (!said)
        {
            said = true;
            mgpu::diag::info(
                "[MGPU][R77] ini read FAILED - keeping the current MetaProbe mode instead of "
                "turning off. A failed read is not an answer. If this line appears once at "
                "startup it is the attach race and it is harmless; if it appears and the lanes "
                "then report nothing, the ini genuinely cannot be found and that is the finding.");
        }
        return get_mode();
    }

    {
        const char *dk = mgpu::config::find(buf, strlen(buf), "DepthCompare");
        int dv = 0;
        if (dk != nullptr) dv = atoi(dk);
        g_dc_mode.store((dv < 0 || dv > 3) ? 0 : dv, std::memory_order_relaxed);  // R55: 3 = arm C alone
    }
    {
        // R101. Calib= drives the NGX parameter tap. Absent means 2, not 0:
        // the tap is the acquisition answer this project has been inferring
        // since R70, and a missing key should not silently return us to
        // guessing. Calib=0 is a decision, not a default.
        const char *ck = mgpu::config::find(buf, strlen(buf), "Calib");
        int cv = 2;
        if (ck != nullptr) cv = atoi(ck);
        g_calib.store((cv < 0 || cv > 2) ? 2 : cv, std::memory_order_relaxed);
    }
    {
        // R110. CalibRung: which install rung is allowed to run. Absent
        // means 0 (both), which is 0.2.1's behaviour exactly - a key
        // nobody sets must never change what the bridge does.
        const char *rk = mgpu::config::find(buf, strlen(buf), "CalibRung");
        int rv = 0;
        if (rk != nullptr) rv = atoi(rk);
        g_calib_rung.store((rv < 0 || rv > 2) ? 0 : rv, std::memory_order_relaxed);
    }
    {
        // R115. CalibProbe. Absent means 0.
        const char *pk = mgpu::config::find(buf, strlen(buf), "CalibProbe");
        int pv = 0;
        if (pk != nullptr) pv = atoi(pk);
        g_calib_probe.store((pv < 0 || pv > 2) ? 0 : pv, std::memory_order_relaxed);
    }
    {
        // SLT1. SLTags: the Streamline tag tap. 0 off - and off is the
        // default, because this key installs an import hook on a title where
        // an interception is already under suspicion. 1 reads each buffer
        // type once, 2 also re-reads when a resource pointer changes.
        const char *sk = mgpu::config::find(buf, strlen(buf), "SLTags");
        int sv = 0;
        if (sk != nullptr) sv = atoi(sk);
        g_sltags.store((sv < 0 || sv > 2) ? 0 : sv, std::memory_order_relaxed);
    }
    {
        // R180/R182. SFPath: the Starfield path. ABSENT IS NOT OFF.
        //   -2 absent  AUTO - detector on, repair off, promoted on overflow
        //    0 off     the operator said no; never promoted
        //    1 on
        const char *fk = mgpu::config::find(buf, strlen(buf), "SFPath");
        int fv = -2;
        if (fk != nullptr) fv = (atoi(fk) == 1) ? 1 : 0;
        g_sfpath.store(fv, std::memory_order_relaxed);
    }
    {
        // R104. Two keys because the sign is the ONE thing worth settling on
        // the rig: JitterComp turns it on, InvertJitter flips it, so a second
        // test is one character and needs no rebuild.
        const char *jk = mgpu::config::find(buf, strlen(buf), "JitterComp");
        const char *ik = mgpu::config::find(buf, strlen(buf), "InvertJitter");
        int jv = (jk != nullptr && atoi(jk) != 0) ? 1 : 0;
        if (jv != 0 && ik != nullptr && atoi(ik) != 0) jv = -1;
        g_jitter.store(jv, std::memory_order_relaxed);
    }
    {
        // R106. The copy's TRIGGER, not its source. Off by default because it
        // assumes a resource state the barrier path is told; see calibrator.hpp.
        // R118. THREE VALUES NOW, and 2 is the one that ships.
        //   0 off, hard. Nothing arms it, which is what makes a controlled
        //     A/B run possible - the R116 attribution run could not have been
        //     done against a fallback that armed itself.
        //   1 on from the start, as before.
        //   2 AUTO: off until the transport proves the barrier route is dead
        //     on this title, then armed by gpu1_context. See R118.
        const char *ek = mgpu::config::find(buf, strlen(buf), "MvecFromEval");
        int ev = 0;
        if (ek != nullptr) ev = atoi(ek);
        g_evalcopy.store((ev < 0 || ev > 2) ? 0 : ev, std::memory_order_relaxed);
    }
    {
        // R74. MVecProbe=1 reads the velocity field. MVecProbeIndex picks which
        // of the RTV-ranked candidates - 0 is the top, which on this title is
        // the DISPLAY-extent buffer DLSS itself is fed.
        const char *k2 = mgpu::config::find(buf, strlen(buf), "MVecProbe");
        g_mv_mode.store((k2 != nullptr && atoi(k2) != 0) ? 1 : 0,
                        std::memory_order_relaxed);
        const char *k3 = mgpu::config::find(buf, strlen(buf), "MVecProbeIndex");
        const int mi = (k3 != nullptr) ? atoi(k3) : 0;
        g_mv_index.store((mi >= 0 && mi < 8) ? (unsigned)mi : 0u,
                         std::memory_order_relaxed);
    }

    const char *k = mgpu::config::find(buf, strlen(buf), "MetaProbeLogSeconds");
    if (k != nullptr)
    {
        const int v = atoi(k);
        if (v >= 1 && v <= 600) g_log_seconds = (unsigned)v;
    }

    k = mgpu::config::find(buf, strlen(buf), "MetaProbe");
    if (k == nullptr) return mode::off;

    if (*k == 'd' || *k == 'D') return mode::depth;
    if (*k == 'm' || *k == 'M') return mode::mvec;
    if (*k == 'b' || *k == 'B') return mode::both;
    if (*k == 'o' || *k == 'O') return mode::off;

    const int v = atoi(k);
    return (v >= 0 && v <= 3) ? static_cast<mode>(v) : mode::off;
}

void set_mode(mode m)
{
    if (g_qpc_to_ns == 0.0) init_timebase();

    // DEFECT P9.1-B, same session as P9.1-A and the same failure shape.
    //
    // This used to return here when the mode was unchanged, which meant a build
    // that started at off said NOTHING, ever. An absent [MGPU][P9.1] line was
    // then indistinguishable from a build that does not contain this file at
    // all - and that is exactly the question someone reading the log after a
    // null run needs answered first. The two were told apart by file
    // timestamps, which is not an instrument.
    //
    // The first call always speaks now, off included. One line per launch.
    static bool announced = false;
    const bool first = !announced;
    announced = true;

    if (!first && static_cast<int>(m) == g_mode.load(std::memory_order_relaxed)) return;

    const bool want_depth = (m == mode::depth || m == mode::both);
    bool want_mvec  = (m == mode::mvec  || m == mode::both);

    // ---- R78: THE TRANSPORT LATCHES THE MVEC LANE ON ----
    //
    // WHY THIS EXISTS: R77's log shows this probe flipping to mode off at
    // 55:20 with ZERO "ini read FAILED" lines in it. Two set_mode paths are
    // accounted for and a THIRD is not, and it has not been found. That was a
    // diagnostic nuisance while the mvec lane only counted things. It is a
    // silent data loss now: set_shared(false) unregisters the render-target
    // bind event, which is where the transport's copy is issued from, so an
    // unexplained flip would stop MVec reaching the model with nothing in the
    // log saying so.
    //
    // So once a transport hook is installed, the lane that carries it stays
    // subscribed regardless of what the mode says. This is a LATCH, not a fix:
    // the third path is still unidentified and still needs finding. What it
    // buys is that the run does not silently become a colour-plus-depth run
    // halfway through - and the [R78] copies counter still says plainly
    // whether the hook is firing, so the latch cannot hide a failure either.
    //
    // With no hook installed - every published run to date - this is false and
    // the behaviour is byte-identical to P12.9.
    if (g_mvec_hook != nullptr) want_mvec = true;

    g_depth_on.store(want_depth, std::memory_order_relaxed);
    g_mvec_on.store(want_mvec, std::memory_order_relaxed);

    set_shared(want_depth || want_mvec);
    set_depth(want_depth);
    set_mvec(want_mvec);

    g_mode.store(static_cast<int>(m), std::memory_order_relaxed);

    static const char *const names[4] = { "off", "depth", "mvec", "both" };
    char line[700];
    snprintf(line, sizeof line,
        "[MGPU][P9.1] PROBE CODE IS PRESENT IN THIS BUILD, probe " PROBE_BUILD "; MetaProbe = %s. "
        "This line is printed "
        "even when the answer is off, so that a log with no other P9.1 line means the probe was "
        "OFF rather than ABSENT - those were indistinguishable once and cost a run. "
        "The key is MetaProbe, NOT Probes: Probes is the older P1.3 one-shot probe chain and "
        "reads only the literal 1. OFF MEANS UNREGISTERED, not a callback that returns early: "
        "with the probe off ReShade installs no interception on our account and the add-on is "
        "byte-identical to a build without this file. The depth lane subscribes to "
        "OMSetRenderTargets and ClearDepthStencilView, which are per-bind and not per-draw; "
        "the cost of that choice is reported in ns/frame on every META SRC line so it can be "
        "weighed rather than assumed.",
        names[static_cast<int>(m) & 3]);
    mgpu::diag::info(line);
}

mode get_mode()
{
    return static_cast<mode>(g_mode.load(std::memory_order_relaxed));
}

// P11.0. The GAME effect_runtime, handed over once per frame from the same
// callback that already knows which runtime it is. Storing a pointer is all
// that happens here: the enumeration itself runs in dump(), on the same thread
// and on the same cadence, so the warm path costs one store per frame.
//
// void * rather than the ReShade type because probe.hpp deliberately holds no
// ReShade types - the same rule that keeps gpu1_context free of them. The cast
// back is to the exact type that was passed in, twenty lines apart, in one
// translation unit.
void note_effects(void *effect_runtime_ptr, void *command_list_ptr)
{
    g_rt = effect_runtime_ptr;

    // ---- R63: resolved EVERY FRAME, and BEFORE the probe-mode gate ----
    //
    // Depth=1 and Depth=2 must not silently require MetaProbe=depth. The
    // transport's source and the tap that keeps it alive are both needed
    // whether or not anyone is watching the probe's tables.
    {
        reshade::api::effect_runtime *rt2 =
            static_cast<reshade::api::effect_runtime *>(effect_runtime_ptr);
        if (rt2 != nullptr)
        {
            if (g_tap_var.handle == 0)
                g_tap_var = rt2->find_texture_variable(nullptr, "MGPU_DepthOutTex");

            unsigned long long h = 0;
            if (g_tap_var.handle != 0)
            {
                reshade::api::resource_view srv = {}, srgb = {};
                rt2->get_texture_binding(g_tap_var, &srv, &srgb);
                if (srv.handle != 0)
                {
                    if (reshade::api::device *dv = rt2->get_device())
                        h = dv->get_resource_from_view(srv).handle;
                }
            }
            if (h == 0) g_tap_var = reshade::api::effect_texture_variable{};   // re-resolve
            g_tap_res.store(h, std::memory_order_relaxed);

            // The tap has to be ON for any of this to exist, and tech_scan
            // otherwise only runs from dump(). Throttled hard: a technique
            // enumeration is not a per-frame cost.
            if (g_mode.load(std::memory_order_relaxed) == 0 &&
                (g_tap_tick++ % 300ull) == 0ull)
                tech_scan();
        }
    }

    if (g_mode.load(std::memory_order_relaxed) == 0) return;

    // ---- R55: arm C. Mode 3 only. ----
    if (g_dc_mode.load(std::memory_order_relaxed) != 3) return;
    if (g_dcc_state != 0) return;              // one in flight at a time
    if (command_list_ptr == nullptr) return;

    const unsigned long long h = g_semC_res.load(std::memory_order_relaxed);
    if (h == 0) return;                        // ReShade has bound nothing yet

    reshade::api::command_list *cl =
        static_cast<reshade::api::command_list *>(command_list_ptr);
    ID3D12GraphicsCommandList *gl =
        reinterpret_cast<ID3D12GraphicsCommandList *>(cl->get_native());
    ID3D12Resource *src =
        reinterpret_cast<ID3D12Resource *>(static_cast<uintptr_t>(h));
    if (gl == nullptr || src == nullptr) return;

    // Sentinel first, so "the copy never wrote here" stays distinguishable
    // from "the copy wrote zeros". On the very first pass the buffers do not
    // exist yet and dc_copy creates them, so that sample has no sentinel
    // baseline - which is why the readback discards it.
    dc_prefill();

    const reshade::api::resource res = { h };
    cl->barrier(res, reshade::api::resource_usage::shader_resource,
                     reshade::api::resource_usage::copy_source);
    dc_copy(gl, src, 2, D3D12_RESOURCE_STATE_COMMON, false);
    cl->barrier(res, reshade::api::resource_usage::copy_source,
                     reshade::api::resource_usage::shader_resource);

    g_dcc_state = 1;
    g_dcc_at = g_frames.load(std::memory_order_relaxed);
    ++g_dcc_copies;
}

// R63. THIS FRAME's depth source for the transport, or 0 when ReShade has
// nothing bound - which is exactly the seal's depth_valid. Handle only: the
// consumer of this reads the resource's own description rather than trusting
// a second copy of the dimensions.
// R145. See probe.hpp. g_tech_seen is reset to 0 at the top of every scan and
// incremented once per technique, so zero means NO SCAN HAS COMPLETED rather
// than "a scan found nothing" - and that distinction is the whole point of the
// -2. Reporting g_tap_state's -1 initialiser before any scan would put ERROR
// 204 on screen during normal startup.
int tap_state()
{
    if (g_tech_seen == 0)        return -2;   // no scan has completed
    if (g_tap_state == 1)        return 1;    // healthy, report immediately
    if (g_tap_bad_scans < 2u)    return -2;   // R147: one bad scan is not a fault
    return g_tap_state;                       // -1 or 0, confirmed twice
}

unsigned long long depth_source()
{
    return g_tap_res.load(std::memory_order_relaxed);
}

// R78. THE IDENTIFIED VELOCITY BUFFER, or 0 while it is not yet known.
//
// Chosen on the SAME rule R73 used to identify it: the top candidate by RENDER
// TARGET BINDS that also sits at the DISPLAY extent, which is what the game's
// own DLSS telemetry reports as MVExtent. Refreshed every dump, so an eviction
// or a resolution change moves it rather than stranding it.
unsigned long long mvec_source()
{
    return g_mvec_src.load(std::memory_order_relaxed);
}

// R78. dllmain installs the forwarder. A function pointer, so this file never
// names a gpu1_context type and that file never names a ReShade one.
void set_mvec_hook(void (*fn)(void *cmd_list_native, unsigned long long resource))
{
    g_mvec_hook = fn;
}

void note_frame()
{
    // R78: the same latch, second half. dump() is what RE-PUBLISHES the
    // transport's source, and on_destroy_resource zeroes that handle when the
    // game evicts the resource. If this returned early while a hook was
    // installed, an eviction would strand the transport on 0 forever with
    // nothing left running to pick a new candidate.
    if (g_mode.load(std::memory_order_relaxed) == 0 && g_mvec_hook == nullptr) return;

    const unsigned long long frame_n =
        g_frames.fetch_add(1, std::memory_order_relaxed) + 1;

    // ---- R74: the velocity patch, four frames after it was recorded. ----
    if (g_mv_mode.load(std::memory_order_relaxed) != 0 && g_mv_state == 1 &&
        frame_n >= g_mv_at + 4ull && g_mv_rb != nullptr &&
        g_mv_fp_ok.load(std::memory_order_acquire) && g_mv_total != 0)
    {
        unsigned short *p = nullptr;
        D3D12_RANGE rr{ 0, (SIZE_T)g_mv_total };
        const HRESULT hr = g_mv_rb->Map(0, &rr, (void **)&p);
        g_mv_hr.store((int)hr, std::memory_order_relaxed);
        if (SUCCEEDED(hr) && p != nullptr)
        {
            const unsigned rowh = g_mv_fp.Footprint.RowPitch / 2u;   // row, in HALVES
            double gmax = 0.0;
            unsigned sent_total = 0;
            bool any = false;
            for (unsigned q = 0; q < MV_N; ++q)
            {
                const unsigned short *pq =
                    p + (size_t)(g_mv_stride * q) / sizeof(unsigned short);
                unsigned good = 0, nz = 0;
                double xs = 0, ys = 0, ms = 0;
                for (unsigned y = 0; y < MV_PATCH; ++y)
                {
                    for (unsigned x = 0; x < MV_PATCH; ++x)
                    {
                        const size_t o = (size_t)y * rowh + (size_t)x * 2u;

                        // R76: still the sentinel means the copy NEVER WROTE
                        // HERE. A fresh readback heap is zero-filled, so
                        // without this a silently failed CopyTextureRegion and
                        // a genuinely still world are the same numbers.
                        if (pq[o] == MV_SENTINEL && pq[o + 1u] == MV_SENTINEL)
                        { ++sent_total; continue; }

                        const float vx = half_to_float(pq[o]);
                        const float vy = half_to_float(pq[o + 1u]);
                        if (!(vx == vx) || !(vy == vy)) continue;   // NaN
                        const double m = sqrt((double)vx * vx + (double)vy * vy);
                        if (m > gmax) gmax = m;
                        if (m > 1e-6) ++nz;
                        xs += vx; ys += vy; ms += m; ++good;
                    }
                }
                if (good != 0)
                {
                    g_mv_px[q]   = xs / (double)good;
                    g_mv_py[q]   = ys / (double)good;
                    g_mv_pm[q]   = ms / (double)good;
                    g_mv_pcov[q] = (unsigned)((100u * nz) / good);
                    any = true;
                }
            }
            D3D12_RANGE none{ 0, 0 };
            g_mv_rb->Unmap(0, &none);
            if (any || sent_total != 0)
            {
                g_mv_magmax = gmax;
                g_mv_sent = sent_total;
                g_mv_have = true;
                g_mv_reads.fetch_add(1, std::memory_order_relaxed);
            }
        }
        g_mv_state = 0;
    }

    // ---- R55: arm C readback, four frames after the copy was recorded. ----
    if (g_dc_mode.load(std::memory_order_relaxed) == 3 && g_dcc_state == 1 &&
        frame_n >= g_dcc_at + 4ull && g_dc_rb[2] != nullptr &&
        g_dc_fp_ok.load(std::memory_order_acquire) && g_dc_total != 0)
    {
        float *p = nullptr;
        D3D12_RANGE rr{ 0, (SIZE_T)g_dc_total };
        const HRESULT hr = g_dc_rb[2]->Map(0, &rr, (void **)&p);
        g_dcc_hr = (int)hr;
        if (SUCCEEDED(hr) && p != nullptr)
        {
            const unsigned stride = g_dc_fp.Footprint.RowPitch / 4u;
            unsigned sent = 0, good = 0;
            double mn = 0.0, mx = 0.0, sum = 0.0;
            for (unsigned y = 0; y < DC_PATCH; ++y)
            {
                for (unsigned x = 0; x < DC_PATCH; ++x)
                {
                    const float v = p[(size_t)y * stride + x];
                    if (v == DC_SENTINEL) { ++sent; continue; }
                    if (!(v == v)) continue;          // NaN fails every compare
                    if (good == 0) { mn = v; mx = v; }
                    else { if (v < mn) mn = v; if (v > mx) mx = v; }
                    sum += v;
                    ++good;
                }
            }
            D3D12_RANGE none{ 0, 0 };
            g_dc_rb[2]->Unmap(0, &none);

            if (g_dcc_copies >= 2ull)     // discard the buffer-creating sample
            {
                g_dcc_sent = sent;
                g_dcc_good = good;
                g_dcc_min  = mn;
                g_dcc_max  = mx;
                g_dcc_mean = (good != 0) ? (sum / (double)good) : 0.0;
                g_dcc_have = true;
                ++g_dcc_reads;
            }
        }
        g_dcc_state = 0;
    }

    // R30b. Close the window that ends at THIS finish_effects, bucket it, and
    // stamp the start of the next one.
    {
        const unsigned c = g_clr_in_window.exchange(0, std::memory_order_relaxed);
        g_clr_hist[(c > 3u) ? 3u : c].fetch_add(1, std::memory_order_relaxed);

        // R33. Close this frame's turn, if one was opened.
        {
            const unsigned long long turn = g_turn_res.exchange(0ull, std::memory_order_relaxed);
            if (turn != 0)
            {
                const unsigned bn = g_turn_binds.load(std::memory_order_relaxed);
                g_bind_hist[(bn > 4u) ? 4u : bn].fetch_add(1, std::memory_order_relaxed);

                const unsigned long long now2 = qpc();
                const unsigned long long lb = g_turn_last_bind_qpc.load(std::memory_order_relaxed);
                if (lb != 0 && now2 > lb)
                {
                    const unsigned long long ns =
                        (unsigned long long)((double)(now2 - lb) * g_qpc_to_ns);
                    g_lb_n.fetch_add(1, std::memory_order_relaxed);
                    g_lb_sum.fetch_add(ns, std::memory_order_relaxed);
                    unsigned long long m = g_lb_min.load(std::memory_order_relaxed);
                    while (ns < m && !g_lb_min.compare_exchange_weak(
                               m, ns, std::memory_order_relaxed)) {}
                    m = g_lb_max.load(std::memory_order_relaxed);
                    while (ns > m && !g_lb_max.compare_exchange_weak(
                               m, ns, std::memory_order_relaxed)) {}
                }
                const unsigned long long cq = g_turn_clear_qpc.load(std::memory_order_relaxed);
                if (cq != 0 && now2 > cq)
                {
                    g_cf_n.fetch_add(1, std::memory_order_relaxed);
                    g_cf_sum.fetch_add(
                        (unsigned long long)((double)(now2 - cq) * g_qpc_to_ns),
                        std::memory_order_relaxed);
                }
            }
        }

        // R31. How many DISTINCT top-8 candidates were cleared this frame.
        unsigned m = g_pool_mask.exchange(0u, std::memory_order_relaxed);
        unsigned bits = 0;
        while (m) { bits += (m & 1u); m >>= 1; }
        g_pool_hist[(bits > 4u) ? 4u : bits].fetch_add(1, std::memory_order_relaxed);
    }

    // Refresh the #1 depth candidate periodically rather than per clear: top_n
    // is O(want * n) and the clear path is warm. Every 60 frames is ~1 s, the
    // ranking settles long before that, and the cost lands on the frame tick
    // which is already a cold-ish path.
    if (g_depth_on.load(std::memory_order_relaxed) && (frame_n % 60u) == 1u)
    {
        unsigned idx[8], k = 0;
        top_n(g_depth, idx, k, 8);
        g_top_depth.store((k > 0) ? g_depth.t[idx[0]].handle : 0ull,
                          std::memory_order_relaxed);
        // Written before the count is published, so the warm path never scans
        // a slot that has not been filled in yet.
        for (unsigned q = 0; q < k && q < 8u; ++q) g_top8[q] = g_depth.t[idx[q]].handle;
        g_top8_n.store((k > 8u) ? 8u : k, std::memory_order_release);
    }

    // ---- R37: schedule a sample, and read one back when it is ripe ----
    if (g_dc_mode.load(std::memory_order_relaxed) != 0)
    {

        // B copies at the Nth bind; N is last frame's count, which R35 showed
        // is stable at 3 (or 1 in the other regime). Following it rather than
        // fixing it is what makes this work across both.
        const unsigned lastn = g_turn_binds.load(std::memory_order_relaxed);
        if (lastn >= 1u && lastn <= 8u) g_dc_target_bind = lastn;

        const unsigned long long cf = g_dc_copy_frame.load(std::memory_order_relaxed);
        const unsigned h0 = g_dc_hits[0].load(std::memory_order_relaxed);
        const unsigned h1 = g_dc_hits[1].load(std::memory_order_relaxed);

        if (h0 != 0u && frame_n >= cf + READ_AFTER)
        {
            // UNSYNCHRONISED BY DESIGN - see the note above dc_copy. Ten frames
            // is far past any plausible queue depth and a torn read would look
            // like noise, not like a plausible wrong answer.
            const float *pa = nullptr, *pb = nullptr;
            const unsigned stride = g_dc_fp.Footprint.RowPitch / 4u;
            D3D12_RANGE rr{ 0, (SIZE_T)g_dc_total };
            if (SUCCEEDED(g_dc_rb[0]->Map(0, &rr, (void **)&pa)) && pa != nullptr)
            {
                if (h1 != 0u && g_dc_rb[1] != nullptr &&
                    SUCCEEDED(g_dc_rb[1]->Map(0, &rr, (void **)&pb)) && pb != nullptr)
                {
                    // R46. COUNT THE SENTINELS. R45 named partial fill as the
                    // suspect for the NaN, and this is the one number that
                    // confirms or kills it: every texel the copy did NOT write
                    // still holds -12345.0. The API says the row pitch is 512
                    // for a row carrying 256 bytes of four-byte depth, so if
                    // the copy fills only part of each row - or only part of
                    // the patch - this says exactly how much, and the finite
                    // texels can be compared on their own instead of being
                    // poisoned by the rest.
                    unsigned sa = 0, sb = 0, finite = 0;
                    double fsum = 0.0, fmx = 0.0;
                    double sum = 0.0, mx = 0.0;
                    unsigned n = 0;
                    for (unsigned y = 0; y < DC_PATCH; ++y)
                    {
                        const float *ra = pa + (size_t)y * stride;
                        const float *rb = pb + (size_t)y * stride;
                        for (unsigned x = 0; x < DC_PATCH; ++x)
                        {
                            const float va = ra[x], vb = rb[x];
                            if (va == DC_SENTINEL) ++sa;
                            if (vb == DC_SENTINEL) ++sb;
                            const double d2 = (double)va - (double)vb;
                            const double ad = (d2 < 0.0) ? -d2 : d2;
                            sum += ad; if (ad > mx) mx = ad; ++n;
                            // The comparison that survives a partial fill:
                            // both texels written, both finite, both plausible
                            // depth. NaN fails every comparison including
                            // ad == ad, which is what makes this filter work.
                            if (va != DC_SENTINEL && vb != DC_SENTINEL &&
                                ad == ad && va > -1.0f && va < 2.0f &&
                                vb > -1.0f && vb < 2.0f)
                            { fsum += ad; if (ad > fmx) fmx = ad; ++finite; }
                        }
                    }
                    // R40. THE FIRST FOUR TEXELS OF EACH BUFFER, RAW. Three
                    // rounds were spent inferring what was in these from a
                    // mean that had already gone NaN. Plausible depth is 0..1
                    // (or reversed-Z near 1..0); zeros mean the copy never
                    // landed; anything near 3.4e38 is untouched heap. One
                    // line ends the guessing.
                    {
                        char dl[300];
                        snprintf(dl, sizeof dl,
                                 "[MGPU][R46] FILL: of %u texels, %u still hold the sentinel in "
                                 "A and %u in B - those were NEVER WRITTEN by the copy. %u texel "
                                 "pairs are written, finite and in depth range, and over THOSE "
                                 "the mean |A-B| is %.9f with a worst of %.9f. THIS IS THE "
                                 "COMPARISON: if the written fraction is well under 100%% the "
                                 "NaN was padding all along and these numbers are the answer "
                                 "R37 has been trying to produce. Read them exactly as R37 "
                                 "specifies - near zero on a STILL camera means arm B captures "
                                 "complete depth.",
                                 n, sa, sb, finite,
                                 (finite != 0) ? fsum / (double)finite : 0.0, fmx);
                        mgpu::diag::info(dl);
                        snprintf(dl, sizeof dl,
                                 "[MGPU][R37] RAW A: %.4f %.4f %.4f %.4f | RAW B: %.4f %.4f "
                                 "%.4f %.4f | SENTINEL %.1f | plane fmt=%d pitch=%u bytes=%llu - ANY BUFFER STILL SHOWING IT "
                                 "WAS NEVER WRITTEN BY THE COPY, which is the diagnosis rather "
                                 "than a symptom. Otherwise 0..1 (or 1..0 reversed-Z) is real "
                                 "depth, and ~3.4e38 would mean the sentinel itself did not "
                                 "take, i.e. the Map failed.",
                                 (double)pa[0], (double)pa[1], (double)pa[2], (double)pa[3],
                                 (double)pb[0], (double)pb[1], (double)pb[2], (double)pb[3],
                                 (double)DC_SENTINEL, (int)g_dc_fp.Footprint.Format,
                                 (unsigned)g_dc_fp.Footprint.RowPitch,
                                 (unsigned long long)g_dc_total);
                        mgpu::diag::info(dl);
                    }
                    if (n != 0)
                    {
                        const double mean = sum / (double)n;
                        g_dc_sum += mean;
                        if (mx > g_dc_max) g_dc_max = mx;
                        g_dc_n.fetch_add(1, std::memory_order_relaxed);
                        if (mean < 1e-7) g_dc_zero.fetch_add(1, std::memory_order_relaxed);
                    }
                    D3D12_RANGE nn{0, 0};
                    g_dc_rb[1]->Unmap(0, &nn);
                }
                D3D12_RANGE nn{0, 0};
                g_dc_rb[0]->Unmap(0, &nn);
            }
            g_dc_hits[0].store(0u, std::memory_order_relaxed);
            g_dc_hits[1].store(0u, std::memory_order_relaxed);
        }

        if (h0 == 0u && (frame_n % SAMPLE_EVERY) == 0ull)
        {
            dc_prefill();   // R41: so a copy that does not land is provable
            g_dc_ready.store(true, std::memory_order_relaxed);
            g_dc_armed.fetch_add(1, std::memory_order_relaxed);
        }
    }

    const unsigned long long now = qpc();
    g_finish_qpc.store(now, std::memory_order_relaxed);
    const double elapsed_ns = (double)(now - g_last_dump_qpc) * g_qpc_to_ns;
    if (elapsed_ns < (double)g_log_seconds * 1e9) return;

    g_last_dump_qpc = now;
    dump();
}

void note_scene_size(unsigned int w, unsigned int h)
{
    if (w == g_scene_w && h == g_scene_h) return;
    const unsigned int ow = g_scene_w, oh = g_scene_h;
    g_scene_w = w;
    g_scene_h = h;

    // SAY IT. The candidate size band is a fraction of this number, so a wrong
    // one does not produce an error - it produces an empty table, or a lateral
    // that reports zero for a whole run and looks like a finding. It was
    // previously set from EVERY swapchain in the process, including the
    // bridge's own 1280x720 present chain, which put the band's 1.05x upper
    // bound below the game's own 1664x936 scene depth. dllmain now passes only
    // the GAME's swapchain; this line is how anyone checks that from a log.
    char l[420];
    snprintf(l, sizeof l,
             "[MGPU][P9.1] SIZE BAND set from the GAME swapchain: %ux%u (was %ux%u). "
             "Candidates are accepted between 20%% and 105%% of that area, so at this "
             "setting a %ux%u scene depth is %s. If a lateral reports zero for a whole "
             "run, check this line first.",
             w, h, ow, oh, w, h, "inside the band by construction");
    mgpu::diag::info(l);
}

void shutdown()
{
    set_mode(mode::off);

    // R37. THE READBACK BUFFERS ARE DELIBERATELY NOT RELEASED. They are
    // referenced by copies recorded into the GAME's command list, and this
    // runs at DLL detach with no way to know those have executed. Releasing
    // them here would be freeing under the GPU - the oldest rule in this
    // project - to reclaim 32 KB from a process that is exiting anyway.
    // Leaking them is the safe half of that trade and it is a choice, not an
    // oversight.
}

void read(readout &out)
{
    memset(&out, 0, sizeof out);
    out.m = get_mode();
    out.game_device_found = g_game_dev.load(std::memory_order_relaxed) != nullptr;
    out.frames   = g_frames.load(std::memory_order_relaxed);
    out.examined = g_examined.load(std::memory_order_relaxed);

    const double f = (out.frames != 0) ? (double)out.frames : 1.0;
    out.depth_ns_per_frame = (double)g_depth.ns.load(std::memory_order_relaxed) / f;
    out.mvec_ns_per_frame  = (double)g_mvec.ns.load(std::memory_order_relaxed) / f;

    unsigned idx[8], k = 0;
    top_n(g_depth, idx, k, 8);
    out.depth_n = k;
    for (unsigned i = 0; i < k; ++i)
    {
        const cand &c = g_depth.t[idx[i]];
        out.depth_top[i].handle = c.handle;
        out.depth_top[i].width  = c.width;
        out.depth_top[i].height = c.height;
        out.depth_top[i].format = c.format;
        out.depth_top[i].binds  = c.a.load(std::memory_order_relaxed);
        out.depth_top[i].clears = c.b.load(std::memory_order_relaxed);
    }

    k = 0;
    top_n(g_mvec, idx, k, 8);
    out.mvec_n = k;
    for (unsigned i = 0; i < k; ++i)
    {
        const cand &c = g_mvec.t[idx[i]];
        out.mvec_top[i].handle = c.handle;
        out.mvec_top[i].width  = c.width;
        out.mvec_top[i].height = c.height;
        out.mvec_top[i].format = c.format;
        out.mvec_top[i].binds  = c.a.load(std::memory_order_relaxed);
        out.mvec_top[i].clears = c.b.load(std::memory_order_relaxed);
    }
}

} // namespace probe
} // namespace mgpu
