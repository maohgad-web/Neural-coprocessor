// ---------------------------------------------------------------------------
// calibrator.cpp - R101. THE CALIBRATOR.
//
// See calibrator.hpp for why this exists. This file is the mechanism.
//
// THE INTERCEPTION POINT, and why it is an import table and not a code patch.
// Both routes into NGX resolve NVSDK_NGX_D3D12_EvaluateFeature by name through
// GetProcAddress - Dragon Sword from its own statically linked SDK glue,
// Plague Tale from inside sl.common.dll. So the function pointer the caller
// ends up holding came out of GetProcAddress, and GetProcAddress itself is an
// IMPORTED symbol sitting in a writable slot in every module's import address
// table. Swapping that slot is a single aligned pointer store. Nothing is
// patched inside anyone's code, no instruction is relocated, no length
// disassembler is needed, and putting it back is the same store in reverse.
// That is why there is no third-party dependency here.
//
// MATCHING BY ADDRESS, NOT BY DLL NAME. Modern binaries import GetProcAddress
// from an API set - api-ms-win-core-libraryloader-l1-2-0.dll and friends -
// rather than from kernel32.dll, and which one varies by toolchain and by
// Windows build. Walking descriptors looking for "kernel32.dll" therefore
// misses real callers. So this walks EVERY descriptor and EVERY thunk in the
// module and compares the SLOT'S CURRENT VALUE against the real
// GetProcAddress address. An api-set forwards to the same function, so the
// resolved pointer is identical and the comparison cannot be fooled by naming.
//
// WHAT WE DELIBERATELY DO NOT PATCH: our own module. gpu1_context resolves the
// same entry point to drive DLSS-NR, and if its IAT were patched our own
// evaluates would be captured as if they were the game's - we would read our
// own settings back and call it ground truth. Skipping self is what keeps the
// tap a measurement of the GAME.
//
// THE ONE HONEST LIMITATION. If a module called GetProcAddress and stored the
// result BEFORE this file installed, patching its import slot afterwards
// changes nothing - it is holding the real pointer in a variable we cannot
// see. The addon loads at ReShade init, which is before a game creates its
// DLSS feature in every title measured so far, so this should not bite. It is
// not silent if it does: resolved=0 with evaluates=0 in the R101 line means
// exactly this and nothing else, and the barrier path is untouched and still
// driving.
// ---------------------------------------------------------------------------

#include "calibrator.hpp"
#include "diag.hpp"
#include "fg_map.hpp"   // D2.0 NR step one (NRFrameFilter=1), read only; off = one load per call site
#include "t119_keys.hpp"   // R300 (T1.19, Dom): R300b SceneHandleSet, R300f2 FeatureDiag - absent = off
#include "gpu1_context.hpp"   // [PORT O2] stream_depth_copy: the game's own depth, at the same evaluate

#include <tlhelp32.h>
#include <intrin.h>   // R245: _ReturnAddress for the nvapi trace
#include <d3d12.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

// The pinned NGX headers, fetched by CI at NGX_SHA and flattened to ext/ngx.
// nvsdk_ngx_d3d12.h does not exist in that tree; the D3D12 entry points are
// declared in nvsdk_ngx.h itself. Same include this project already uses.
#include "../ext/ngx/nvsdk_ngx.h"
#include "../ext/ngx/nvsdk_ngx_defs_dlssg.h"   // D2.0 NR step one: frame generation's public parameter names

namespace mgpu
{
namespace calibrator
{
namespace
{

// ---- KEY NAMES, EVERY ONE CHECKED AGAINST THE PINNED HEADER ----
//
// R101a. The first draft of this file wrote these from memory and hedged the
// uncertain ones with #ifdef fallbacks to string literals. One of them was
// wrong in a way no fallback could catch: there is no
// NVSDK_NGX_Parameter_DLSS_Depth_Inverted. Depth inversion is bit 3 of the
// CREATE FLAGS, not a per-frame key - so the fallback string would have read
// back as "absent" on every title forever, and the have-bits would have
// reported that absence as a fact about the game.
//
// So the fallbacks are gone. Every name below is the macro as it appears in
// NVIDIA/DLSS include/nvsdk_ngx_defs.h at the SHA build.yml pins, READ from
// the file rather than recalled. If the pin moves and a name moves with it,
// this is a compile error - which is the failure we want, not a field that
// quietly stops being populated.
//
// Note NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X: the macro has no
// underscore before Base even though its string value does. That asymmetry is
// in the header, not a typo here.
#define K_COLOR  NVSDK_NGX_Parameter_Color
#define K_DEPTH  NVSDK_NGX_Parameter_Depth
#define K_MVEC   NVSDK_NGX_Parameter_MotionVectors
#define K_OUTPUT NVSDK_NGX_Parameter_Output
#define K_MVSX   NVSDK_NGX_Parameter_MV_Scale_X
#define K_MVSY   NVSDK_NGX_Parameter_MV_Scale_Y
#define K_JX     NVSDK_NGX_Parameter_Jitter_Offset_X
#define K_JY     NVSDK_NGX_Parameter_Jitter_Offset_Y
#define K_RESET  NVSDK_NGX_Parameter_Reset
#define K_W      NVSDK_NGX_Parameter_Width
#define K_H      NVSDK_NGX_Parameter_Height
#define K_OW     NVSDK_NGX_Parameter_OutWidth
#define K_OH     NVSDK_NGX_Parameter_OutHeight
#define K_FLAGS  NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags
#define K_QUAL   NVSDK_NGX_Parameter_PerfQualityValue
#define K_SUB_W  NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width
#define K_SUB_H  NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height
#define K_CSX    NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X
#define K_CSY    NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y
#define K_DSX    NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X
#define K_DSY    NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y
#define K_MSX    NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X
#define K_MSY    NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y
#define K_OSX    NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X
#define K_OSY    NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y
#define K_DYN_MINW NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Width
#define K_DYN_MINH NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Height
#define K_DYN_MAXW NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Width
#define K_DYN_MAXH NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Height

// ---- The two entry points we care about, spelled exactly as resolved ----
const char *const NAME_EVAL   = "NVSDK_NGX_D3D12_EvaluateFeature";
const char *const NAME_CREATE = "NVSDK_NGX_D3D12_CreateFeature";
// R183. The third entry point, and it is here for one reason: 86 CreateFeature
// calls in one session could be 86 leaks or 86 matched pairs and nothing in any
// log distinguishes them. Counting releases is what makes "live" a number.
const char *const NAME_RELEASE = "NVSDK_NGX_D3D12_ReleaseFeature";
// R232 (DX11 contract, A1). The same three on D3D11: a D3D11 title with a
// DLSS contract (Rise of the Tomb Raider, nvngx_dlss 2.3.2) resolves these
// through the same GetProcAddress route. The parameter block is the same
// object with the same names; the resources behind the names are
// ID3D11Resource*. The copy goes to the DX11 producer (set_mvec_hook_d3d11),
// which hops it; the D3D12 copy path is untouched.
const char *const NAME_EVAL11    = "NVSDK_NGX_D3D11_EvaluateFeature";
const char *const NAME_CREATE11  = "NVSDK_NGX_D3D11_CreateFeature";
const char *const NAME_RELEASE11 = "NVSDK_NGX_D3D11_ReleaseFeature";

typedef NVSDK_NGX_Result(NVSDK_CONV *pf_evaluate)(
    ID3D12GraphicsCommandList *InCmdList,
    const NVSDK_NGX_Handle *InFeatureHandle,
    const NVSDK_NGX_Parameter *InParameters,
    void *InCallback);

typedef NVSDK_NGX_Result(NVSDK_CONV *pf_create)(
    ID3D12GraphicsCommandList *InCmdList,
    NVSDK_NGX_Feature InFeatureID,
    NVSDK_NGX_Parameter *InParameters,
    NVSDK_NGX_Handle **OutHandle);

typedef NVSDK_NGX_Result(NVSDK_CONV *pf_release)(NVSDK_NGX_Handle *InHandle);

// R232: the D3D11 signatures (nvsdk_ngx.h). Release is the same shape.
typedef NVSDK_NGX_Result(NVSDK_CONV *pf_evaluate11)(
    ID3D11DeviceContext *InDevCtx,
    const NVSDK_NGX_Handle *InFeatureHandle,
    const NVSDK_NGX_Parameter *InParameters,
    void *InCallback);
typedef NVSDK_NGX_Result(NVSDK_CONV *pf_create11)(
    ID3D11DeviceContext *InDevCtx,
    NVSDK_NGX_Feature InFeatureID,
    NVSDK_NGX_Parameter *InParameters,
    NVSDK_NGX_Handle **OutHandle);

typedef FARPROC(WINAPI *pf_gpa)(HMODULE, LPCSTR);

// ---- STATE ----

std::atomic<int>  g_mode{0};
// R110. Which install rung is allowed to run: 0 both, 1 the import swap
// alone, 2 the data-section scan alone. Separate from g_mode because the two
// answer different questions - g_mode is what we do once NGX is running,
// g_rung is what we are allowed to touch before it ever is.
std::atomic<int>  g_rung{0};
std::atomic<bool> g_installed{false};
const char *g_site = "?";   // R101b: which event installed us

pf_gpa      g_real_gpa   = nullptr;
pf_evaluate g_real_eval  = nullptr;
pf_create   g_real_create= nullptr;
pf_release  g_real_release = nullptr;   // R183
pf_evaluate11 g_real_eval11    = nullptr;   // R232
pf_create11   g_real_create11  = nullptr;   // R232
pf_release    g_real_release11 = nullptr;   // R232
std::atomic<unsigned long long> g_evals11{0}, g_copies11{0}, g_skips11{0};   // R232

HMODULE g_self = nullptr;

// Counters. Every one of these answers a question the log would otherwise have
// to guess at, which is the standing rule in this project after R80.
std::atomic<unsigned long long> g_gpa_calls{0};    // GetProcAddress seen
std::atomic<unsigned long long> g_resolved{0};     // times NGX eval handed out
std::atomic<unsigned long long> g_evals{0};        // evaluates intercepted
std::atomic<unsigned long long> g_captures{0};     // evaluates we read
// R283: the thread of the evaluates capture() reads (own_reflex's thread guard).
std::atomic<unsigned long> g_eval_tid_first{0}, g_eval_tid_last{0};
std::atomic<unsigned>      g_eval_tid_changes{0};
std::atomic<unsigned long long> g_creates{0};      // CreateFeature seen
std::atomic<unsigned long long> g_releases{0};     // R183: ReleaseFeature seen
std::atomic<unsigned long long> g_unlatches{0};    // R180: latched handles freed
std::atomic<unsigned long long> g_relatches{0};    // R180: freed slots reused
std::atomic<unsigned long long> g_latch_refused{0};// R180: set full, handle dropped
// R180. The key's three states, and ABSENT is not the same as OFF.
//   -2  absent   AUTO: the detector runs, the repair does not, and the first
//                 overflow promotes the key to 1 for the next launch.
//    0  explicit OFF: the operator said no. Never promoted, never written.
//    1  ON       the repair is live.
// Same shape as DcompOverlay's -2/-1/0/1, for the same reason: a key nobody
// set and a key somebody set to zero are different facts and the second one
// has to be respected.
std::atomic<int> g_sf_path{-2};
std::atomic<bool> g_sf_promoted{false};
std::atomic<unsigned long long> g_slots{0};        // IAT slots patched
std::atomic<unsigned long long> g_modules{0};      // modules walked
std::atomic<unsigned long long> g_cost_ns{0};      // total ns inside capture
std::atomic<unsigned long long> g_frames{0};

// ---- R106 state. IN THE ANONYMOUS NAMESPACE ON PURPOSE ----
// hook_evaluate is defined below and reads these, so they have to be declared
// above it. The first cut of this put them next to the public setters, which
// sit after the anonymous namespace closes - five undeclared-identifier
// errors, all of them ordering.
typedef void (*pf_mvec_hook)(void *, unsigned long long);
pf_mvec_hook g_mvec_hook_fn = nullptr;
pf_mvec_hook g_mvec_hook11_fn = nullptr;   // R232: (ID3D11DeviceContext*, ID3D11Resource*)
std::atomic<int> g_eval_copy_mode{0};
std::atomic<unsigned long long> g_eval_copies{0};
std::atomic<unsigned long long> g_eval_skips{0};
unsigned long long g_eval_last_frame = 0xFFFFFFFFFFFFFFFFull;

// The tracked feature. Streamline and the SDK both create several features in
// one process - super sampling, frame generation, reflex - and only one of
// them carries the parameter block we want. If we see a CreateFeature we learn
// which handle is which; if we never see one (we installed after creation) we
// fall back to accepting any handle and SAY SO in the log rather than
// pretending we filtered.
std::atomic<unsigned long long> g_sr_handle{0};
// ---- R300b / T1.10 MVEC COVERAGE (Dom; SceneHandleSet=1 only) ----
// Measured on Starfield (his run C): the game creates and releases DLSS features while running
// (creates=1..7). g_sr_handle is set at CREATE and never moved at RELEASE, so when the feature the game
// keeps evaluating is not the newest one created, every evaluate fails `h == g_sr_handle`: the table and
// the R106 copy freeze while evaluates climb (~600 frames per episode), and every frame in that window
// is sealed without vectors. The latch set already knew every live scene feature (R135); the outer test
// never asked it. With the key: take any LATCHED scene handle as well, count per handle, and re-point
// g_sr_handle at a live scene feature when its own is released. Key absent: none of this runs.
// D3D12 hook only, as in T1.19; the D3D11 hook keeps the single-handle test.
struct hstat
{
    std::atomic<unsigned long long> h{0}, evals{0}, taken{0}, copies{0};
    std::atomic<unsigned int> w{0}, hh{0};
};
hstat g_hs[4];
// Totals of handles that were released, so a freed slot can be reused without losing what it measured.
std::atomic<unsigned long long> g_hs_ret_n{0}, g_hs_ret_evals{0}, g_hs_ret_taken{0}, g_hs_ret_copies{0};
void hs_release(unsigned long long hv)
{
    if (hv == 0ull) return;
    for (int i = 0; i < 4; ++i)
        if (g_hs[i].h.load(std::memory_order_relaxed) == hv)
        {
            g_hs_ret_n.fetch_add(1, std::memory_order_relaxed);
            g_hs_ret_evals.fetch_add(g_hs[i].evals.exchange(0ull, std::memory_order_relaxed), std::memory_order_relaxed);
            g_hs_ret_taken.fetch_add(g_hs[i].taken.exchange(0ull, std::memory_order_relaxed), std::memory_order_relaxed);
            g_hs_ret_copies.fetch_add(g_hs[i].copies.exchange(0ull, std::memory_order_relaxed), std::memory_order_relaxed);
            g_hs[i].w.store(0u, std::memory_order_relaxed);
            g_hs[i].hh.store(0u, std::memory_order_relaxed);
            g_hs[i].h.store(0ull, std::memory_order_relaxed);   // free LAST: hs_slot may claim it from here on
            return;
        }
}
int hs_slot(unsigned long long hv)
{
    if (hv == 0ull) return -1;
    for (int i = 0; i < 4; ++i)
    {
        unsigned long long cur = g_hs[i].h.load(std::memory_order_relaxed);
        if (cur == hv) return i;
        if (cur == 0ull)
        {
            unsigned long long z = 0ull;
            if (g_hs[i].h.compare_exchange_strong(z, hv, std::memory_order_relaxed) || z == hv) return i;
        }
    }
    return -1;                                    // a fifth distinct handle is not tracked, not an error
}
std::atomic<bool> g_handle_known{false};

// ---- R135: A SET OF SCENE FEATURES, NOT ONE ----
//
// The comment above says "only one of them carries the parameter block we
// want" and that turned out to be false. MEASURED on Cyberpunk 2077,
// 2026-09-15, one in-game toggle and nothing else changed:
//
//   Ray Reconstruction OFF -> [R134] id=1  | sr-handle=known
//                             eval-copies=7732 eval-skips=0    | copies=7105
//   Ray Reconstruction ON  -> [R134] id=13 | sr-handle=UNFILTERED
//                             eval-copies=0 eval-skips=4219    | copies=0
//
// With Ray Reconstruction on, this title creates NO SuperSampling feature at
// all. The latch below fired only on SuperSampling, so nothing was ever
// latched, every evaluate failed the handle test, and all 4219 of them were
// skipped - correctly, by a guard doing exactly what it was written to do.
// Depth kept crossing throughout, which is what made it read as "this title
// has no velocity buffer". Reported against 0.2.2 by the issue 15 reporter,
// who had reached the same zero on his own rig and reasonably blamed Calib.
//
// SO THE FIX IS THE INPUT SET, NOT THE RULE. Latch every id known to consume
// the SCENE's motion vectors; latch nothing else. Frame generation stays out
// by simply never being added - which matters, because it is the feature that
// caused the corruption this guard exists to prevent: measured on A Plague
// Tale, eval-copies=6002 against 3001 sealed frames, exactly 2:1, every frame
// getting the scene's vectors overwritten by frame generation's.
//
// FOUR SLOTS. If a title ever creates more scene features than that, the log
// says so and the set is full rather than silently wrong.
constexpr unsigned int SCENE_FEATURE_SLOTS = 4u;
std::atomic<unsigned long long> g_scene_handles[SCENE_FEATURE_SLOTS];
std::atomic<unsigned int> g_scene_handle_n{0};

// 13 IS A MEASUREMENT, NOT A HEADER CONSTANT. It is the id this title creates
// with Ray Reconstruction enabled, read off the [R134] line. It is written as
// a number on purpose: the SDK header available to this build does not name
// it, and inventing an enum name for a value nobody has verified is how the
// wrong feature gets latched. If a future SDK names it, replace the literal
// and keep this note.
constexpr unsigned int FEATURE_ID_RAY_RECONSTRUCTION = 13u;

// Is this an id whose evaluate carries the scene's motion vectors?
bool is_scene_feature(unsigned int idv)
{
    return idv == (unsigned int)NVSDK_NGX_Feature_SuperSampling ||
           idv == FEATURE_ID_RAY_RECONSTRUCTION;
}

// Latch, if it is not already in the set. Create is rare - a handful of calls
// per launch - so a linear scan is the right shape and no lock is needed.
// ---- R180: THE SET HAS TO FORGET, AND UNTIL NOW IT COULD NOT ----
//
// MEASURED on Starfield, 2026-09-20, operator's rig:
//
//   creates=1   eval-copies=0      eval-skips=0
//   creates=5   eval-copies=1157   eval-skips=49      <- copies freeze here
//   creates=12  eval-copies=1157   eval-skips=4937    <- and never resume
//
// SCENE_FEATURE_SLOTS is 4. This function was append-only: four handles went
// in, `n >= SCENE_FEATURE_SLOTS` refused the fifth SILENTLY, and from that
// frame on the live handle was never in the set. Every evaluate failed the
// test and was skipped - correctly, by a guard doing exactly what it was
// written to do, against a set that could no longer be updated.
//
// The title was not leaking: releases tracked creates (live=0 or 1 all run),
// so the four handles held here were all DEAD. We were comparing the live
// feature against four corpses.
//
// WHY IT DOES NOT HAPPEN ON EVERY RIG. If the allocator hands back an address
// already in the set, the early return above finds it and no slot is consumed.
// The reporter's machine survived 86 creates with 122 skips; this one died at
// the fifth. Same code, different allocator behaviour - which is why this read
// as title-specific for two days and is not.
//
// THE REPAIR IS TO FREE THE SLOT ON RELEASE, not to grow the array. A bigger
// array postpones the same failure and hides it behind a longer session.
void latch_scene_handle(unsigned long long hv)
{
    if (hv == 0ull) return;                       // 0 is the free marker
    const unsigned int n = g_scene_handle_n.load(std::memory_order_relaxed);
    for (unsigned int i = 0; i < n && i < SCENE_FEATURE_SLOTS; ++i)
        if (g_scene_handles[i].load(std::memory_order_relaxed) == hv) return;
    // R180. Reuse a slot a release has freed before considering the set full.
    // BEHIND THE KEY: with it off this loop finds nothing, because nothing ever
    // frees a slot, and the function behaves exactly as it did in 0.2.4.
    for (unsigned int i = 0; i < n && i < SCENE_FEATURE_SLOTS; ++i)
        if (g_scene_handles[i].load(std::memory_order_relaxed) == 0ull)
        {
            g_scene_handles[i].store(hv, std::memory_order_relaxed);
            g_relatches.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    if (n >= SCENE_FEATURE_SLOTS) { g_latch_refused.fetch_add(1, std::memory_order_relaxed); return; }
    g_scene_handles[n].store(hv, std::memory_order_relaxed);
    // RELEASE, paired with the acquire in is_latched_scene_handle: the handle
    // has to be visible before the count that makes it readable. Writers are
    // the game's own CreateFeature path and are effectively serialised, so the
    // scan above needs nothing stronger.
    g_scene_handle_n.store(n + 1u, std::memory_order_release);
}

// The evaluate-side test. Replaces a single == against g_sr_handle.
bool is_latched_scene_handle(unsigned long long hv)
{
    if (hv == 0ull) return false;                 // R180: never match the free marker
    const unsigned int n = g_scene_handle_n.load(std::memory_order_acquire);
    for (unsigned int i = 0; i < n && i < SCENE_FEATURE_SLOTS; ++i)
        if (g_scene_handles[i].load(std::memory_order_relaxed) == hv) return true;
    return false;
}

// R180. Free the slot when the GAME releases the feature. Called from
// hook_release, on the game's own thread, before the real release runs.
//
// A slot is zeroed rather than compacted. Compacting would have to move a
// handle and shrink the count while is_latched_scene_handle may be walking the
// array, and the failure mode of that race is the bad one: a reader matching a
// stale entry and COPYING from a feature that has been released. Zeroing is a
// single 64-bit store, the reader rejects 0 explicitly, and a lost race costs
// one skipped evaluate - which the counters already report.
void unlatch_scene_handle(unsigned long long hv)
{
    if (g_sf_path.load(std::memory_order_relaxed) != 1) return;   // R180: gated
    if (hv == 0ull) return;
    const unsigned int n = g_scene_handle_n.load(std::memory_order_acquire);
    for (unsigned int i = 0; i < n && i < SCENE_FEATURE_SLOTS; ++i)
        if (g_scene_handles[i].load(std::memory_order_relaxed) == hv)
        {
            g_scene_handles[i].store(0ull, std::memory_order_relaxed);
            g_unlatches.fetch_add(1, std::memory_order_relaxed);
            return;
        }
}

// The create flags, latched at CreateFeature. They are a CREATE-time fact, so
// the evaluate path cannot be relied on to carry them - but it is tried there
// too, because Streamline builds its own block and may keep them in it.
std::atomic<unsigned int> g_flags{0};
std::atomic<bool> g_flags_known{false};

// Bit positions from NVSDK_NGX_DLSS_Feature_Flags in the pinned header:
// IsHDR 0, MVLowRes 1, MVJittered 2, DepthInverted 3, DoSharpening 5,
// AutoExposure 6, AlphaUpscaling 7.
void decode_flags(unsigned int fl, table &t)
{
    t.create_flags   = fl;
    t.is_hdr         = (fl >> 0) & 1u;
    t.mv_low_res     = (fl >> 1) & 1u;
    t.mv_jittered    = (fl >> 2) & 1u;
    t.depth_inverted = (fl >> 3) & 1u;
    t.auto_exposure  = (fl >> 6) & 1u;
}

// ---- THE PUBLISHED TABLE, as a seqlock ----
//
// Written on the game's render thread inside EvaluateFeature, read from
// finish-effects. A mutex here would be a wait on the render thread mid-frame,
// which is DEFECT E exactly, and DEFECT E cost 60 -> 49.8 fps. So: even
// sequence means stable, odd means a write is in progress, and the reader
// retries a bounded number of times and then gives up. A reader that gives up
// costs one frame of staleness. A writer that waits costs the game.
std::atomic<unsigned> g_seq{0};
table g_tbl{};

double g_qpc_to_ns = 0.0;

unsigned long long qpc()
{
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return (unsigned long long)v.QuadPart;
}

void qpc_init()
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpc_to_ns = (f.QuadPart != 0) ? (1000000000.0 / (double)f.QuadPart) : 0.0;
}

// ---- READING THE PARAMETER BLOCK ----
//
// Get() returns a result code per key. A key the game never set comes back
// failed, and that is INFORMATION - it is how we will find out that Streamline
// populates a different subset than the raw SDK path does. So every read sets
// a have-bit and nothing is defaulted silently.
inline bool ok(NVSDK_NGX_Result r)
{
#ifdef NVSDK_NGX_SUCCEED
    return NVSDK_NGX_SUCCEED(r) != 0;
#else
    return r == NVSDK_NGX_Result_Success;
#endif
}

bool get_res(const NVSDK_NGX_Parameter *p, const char *k, unsigned long long &out)
{
    ID3D12Resource *r = nullptr;
    if (ok(p->Get(k, &r)) && r != nullptr)
    {
        out = (unsigned long long)(uintptr_t)r;
        return true;
    }
    // R233. A value the game set as a D3D11 resource comes back failed through
    // the ID3D12Resource** overload - a different vtable slot. TR-10: every
    // resource read null on Rise (D3D11) while the scalars came through. The
    // D3D12 read above is unchanged; this runs only when it found nothing.
    ID3D11Resource *r11 = nullptr;
    if (ok(p->Get(k, &r11)) && r11 != nullptr)
    {
        out = (unsigned long long)(uintptr_t)r11;
        return true;
    }
    return false;
}

bool get_u(const NVSDK_NGX_Parameter *p, const char *k, unsigned int &out)
{
    unsigned int v = 0;
    if (!ok(p->Get(k, &v))) return false;
    out = v;
    return true;
}

bool get_f(const NVSDK_NGX_Parameter *p, const char *k, float &out)
{
    float v = 0.0f;
    if (!ok(p->Get(k, &v))) return false;
    out = v;
    return true;
}

void capture(const NVSDK_NGX_Parameter *p)
{
    if (p == nullptr) return;

    const unsigned long long t0 = qpc();

    table t{};
    t.frame = g_frames.load(std::memory_order_relaxed);
    t.evals = g_evals.load(std::memory_order_relaxed);

    if (get_res(p, K_COLOR,  t.color))  t.have |= KEY_COLOR;
    if (get_res(p, K_DEPTH,  t.depth))  t.have |= KEY_DEPTH;
    if (get_res(p, K_MVEC,   t.mvec))   t.have |= KEY_MVEC;
    if (get_res(p, K_OUTPUT, t.output)) t.have |= KEY_OUTPUT;

    if (get_f(p, K_MVSX, t.mv_scale_x) && get_f(p, K_MVSY, t.mv_scale_y))
        t.have |= KEY_MV_SCALE;
    if (get_f(p, K_JX, t.jitter_x) && get_f(p, K_JY, t.jitter_y))
        t.have |= KEY_JITTER;

    if (get_u(p, K_RESET, t.reset)) t.have |= KEY_RESET;

    // Flags: prefer this block if it carries them, else the value latched at
    // CreateFeature. Either way they are decoded from ONE word, so
    // depth_inverted and mv_low_res can never disagree with each other.
    unsigned int fl = 0;
    if (get_u(p, K_FLAGS, fl))
    {
        decode_flags(fl, t);
        t.have |= KEY_FLAGS;
        g_flags.store(fl, std::memory_order_relaxed);
        g_flags_known.store(true, std::memory_order_relaxed);
    }
    else if (g_flags_known.load(std::memory_order_relaxed))
    {
        decode_flags(g_flags.load(std::memory_order_relaxed), t);
        t.have |= KEY_FLAGS;
    }

    if (get_u(p, K_QUAL, t.perf_quality)) t.have |= KEY_QUALITY;

    if (get_u(p, K_DYN_MINW, t.dyn_min_w) && get_u(p, K_DYN_MINH, t.dyn_min_h) &&
        get_u(p, K_DYN_MAXW, t.dyn_max_w) && get_u(p, K_DYN_MAXH, t.dyn_max_h))
        t.have |= KEY_DYNAMIC;

    if (get_u(p, K_W,  t.render_w) && get_u(p, K_H,  t.render_h))
        t.have |= KEY_RENDER_EXT;
    if (get_u(p, K_OW, t.display_w) && get_u(p, K_OH, t.display_h))
        t.have |= KEY_DISPLAY_EXT;

    unsigned int sw = 0, sh = 0;
    if (get_u(p, K_SUB_W, sw) && get_u(p, K_SUB_H, sh))
    {
        t.sub_w = sw;
        t.sub_h = sh;
        get_u(p, K_CSX, t.color_x); get_u(p, K_CSY, t.color_y);
        get_u(p, K_DSX, t.depth_x); get_u(p, K_DSY, t.depth_y);
        get_u(p, K_MSX, t.mvec_x);  get_u(p, K_MSY, t.mvec_y);
        get_u(p, K_OSX, t.out_x);   get_u(p, K_OSY, t.out_y);
        t.have |= KEY_SUBRECTS;
    }

    // Publish. Odd sequence while the fields are in flux.
    g_seq.fetch_add(1u, std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_release);
    g_tbl = t;
    std::atomic_thread_fence(std::memory_order_release);
    g_seq.fetch_add(1u, std::memory_order_release);

    // R283: which thread evaluates the game's scene. One thread-id read and
    // one load per capture; the stores run only when the thread changes.
    // The count below is relaxed, so this is no ordering promise; own_reflex
    // reads the threads a settle (5 s) after the first capture, or every 30
    // frames, never in the same instant.
    {
        const unsigned long tid = (unsigned long)GetCurrentThreadId();
        const unsigned long was = g_eval_tid_last.load(std::memory_order_relaxed);
        if (tid != was)
        {
            if (was == 0ul) g_eval_tid_first.store(tid, std::memory_order_relaxed);
            else g_eval_tid_changes.fetch_add(1u, std::memory_order_relaxed);
            g_eval_tid_last.store(tid, std::memory_order_release);
        }
    }

    g_captures.fetch_add(1, std::memory_order_relaxed);
    g_cost_ns.fetch_add((unsigned long long)((double)(qpc() - t0) * g_qpc_to_ns),
                        std::memory_order_relaxed);
}

// [FFX, 2026-11-11] publish_external() is defined AFTER the anonymous namespace below -
// putting it here gave it internal linkage and the linker reported it as unresolved.

// ---- D2.0 NR STEP ONE: WHAT EACH EVALUATE SAYS, FOR THE FRAME-GEN MAP ----
//
// NRFrameFilter=1 only (fg_map.hpp). The same public names capture() reads,
// for EVERY feature that evaluates - the frame-gen feature (id 11) included,
// which the copy below skips on purpose (R135). Reads only: nothing published
// into g_tbl, nothing copied, the evaluate itself untouched.
void fgmap_tap(const NVSDK_NGX_Handle *h, const NVSDK_NGX_Parameter *p)
{
    if (p == nullptr) return;
    mgpu::fgmap::eval_rec r{};
    const unsigned long long hv = (unsigned long long)(uintptr_t)h;
    r.scene = g_handle_known.load(std::memory_order_relaxed) && is_latched_scene_handle(hv);
    if (get_res(p, K_COLOR,  r.color))  r.have |= 1u;
    if (get_res(p, K_DEPTH,  r.depth))  r.have |= 2u;
    if (get_res(p, K_MVEC,   r.mvec))   r.have |= 4u;
    if (get_res(p, K_OUTPUT, r.output)) r.have |= 8u;
    if (get_f(p, K_MVSX, r.mv_scale_x) && get_f(p, K_MVSY, r.mv_scale_y)) r.have |= 16u;
    if (get_f(p, K_JX, r.jitter_x) && get_f(p, K_JY, r.jitter_y)) r.have |= 32u;
    if (get_u(p, K_RESET, r.reset)) r.have |= 64u;
    if (!r.scene)
    {
        // Frame generation's own names, every one from nvsdk_ngx_defs_dlssg.h
        // at the pin (a moved name is a compile error, R101a's rule).
        if (get_res(p, NVSDK_NGX_DLSSG_Parameter_Backbuffer,         r.g_backbuffer)) r.g_have |= 1u << 0;
        if (get_res(p, NVSDK_NGX_DLSSG_Parameter_MVecs,              r.g_mvecs))      r.g_have |= 1u << 1;
        if (get_res(p, NVSDK_NGX_DLSSG_Parameter_Depth,              r.g_depth))      r.g_have |= 1u << 2;
        if (get_res(p, NVSDK_NGX_DLSSG_Parameter_HUDLess,            r.g_hudless))    r.g_have |= 1u << 3;
        if (get_res(p, NVSDK_NGX_DLSSG_Parameter_OutputInterpolated, r.g_out_interp)) r.g_have |= 1u << 4;
        if (get_res(p, NVSDK_NGX_DLSSG_Parameter_OutputReal,         r.g_out_real))   r.g_have |= 1u << 5;
        if (get_f(p, NVSDK_NGX_DLSSG_Parameter_MvecScaleX, r.g_mv_scale_x) &&
            get_f(p, NVSDK_NGX_DLSSG_Parameter_MvecScaleY, r.g_mv_scale_y))           r.g_have |= 1u << 6;
        if (get_u(p, NVSDK_NGX_DLSSG_Parameter_Reset,                r.g_reset))      r.g_have |= 1u << 7;
        if (get_u(p, NVSDK_NGX_DLSSG_Parameter_CameraMotionIncluded, r.g_cam_motion)) r.g_have |= 1u << 8;
        if (get_u(p, NVSDK_NGX_DLSSG_Parameter_MvecJittered,         r.g_mv_jittered)) r.g_have |= 1u << 9;
        if (get_u(p, NVSDK_NGX_DLSSG_Parameter_MultiFrameCount,      r.g_mf_count))   r.g_have |= 1u << 10;
        if (get_u(p, NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex,      r.g_mf_index))   r.g_have |= 1u << 11;
        if (get_u(p, NVSDK_NGX_DLSSG_Parameter_NotRenderingGameFrames, r.g_not_rendering)) r.g_have |= 1u << 12;
        {
            unsigned long long id = 0;
            if (ok(p->Get(NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, &id)))
            { r.g_bb_frame_id = id; r.g_have |= 1u << 13; }
        }
        if (get_u(p, NVSDK_NGX_DLSSG_Parameter_MVecsSubrectWidth,  r.g_mv_sub_w) &&
            get_u(p, NVSDK_NGX_DLSSG_Parameter_MVecsSubrectHeight, r.g_mv_sub_h))     r.g_have |= 1u << 14;
        {
            // "Float4x4 as void*" (the header): a pointer to 16 floats the
            // caller owns for this call. Copied, never kept.
            void *m = nullptr;
            if (ok(p->Get(NVSDK_NGX_DLSSG_Parameter_ClipToPrevClip, &m)) && m != nullptr)
            {
                memcpy(r.g_clip_to_prev, m, sizeof r.g_clip_to_prev);
                r.g_have |= 1u << 15;
            }
        }
    }
    mgpu::fgmap::on_evaluate(hv, r);
}

// R121. ngx_module() is defined further down, beside the data scan that first
// needed it. The hooks below now resolve late as a second line of defence, so
// they need it too - declared here rather than moved, because moving it would
// reorder a file this round has no other reason to touch.
HMODULE ngx_module();

// ---- THE HOOKS ----

NVSDK_NGX_Result NVSDK_CONV hook_evaluate(ID3D12GraphicsCommandList *cl,
                                          const NVSDK_NGX_Handle *h,
                                          const NVSDK_NGX_Parameter *p,
                                          void *cb)
{
    g_evals.fetch_add(1, std::memory_order_relaxed);

    // D2.0 NR step one. Off: one load.
    if (mgpu::fgmap::on()) fgmap_tap(h, p);

    const int m = g_mode.load(std::memory_order_relaxed);
    if (m != 0)
    {
        // Filter to the super-sampling feature IF we were present for its
        // creation. If we were not, take everything - a table from the wrong
        // feature is visible in the diff against Dragon Sword, whereas taking
        // nothing teaches us nothing at all.
        bool take = true;
        // R300b (SceneHandleSet=1 only): count per handle, and accept any LATCHED scene handle as well as the
        // newest. Key absent: hsi stays -1 and the test below is the live one.
        const bool r307_on = mgpu::t119::scene_handle_set();
        const unsigned long long hvv = (unsigned long long)(uintptr_t)h;
        int hsi = -1;
        if (r307_on) { hsi = hs_slot(hvv); if (hsi >= 0) g_hs[hsi].evals.fetch_add(1, std::memory_order_relaxed); }
        if (g_handle_known.load(std::memory_order_relaxed))
            take = ((unsigned long long)(uintptr_t)h ==
                    g_sr_handle.load(std::memory_order_relaxed)) ||
                   (r307_on && is_latched_scene_handle(hvv));   // R300b
        if (take && hsi >= 0) g_hs[hsi].taken.fetch_add(1, std::memory_order_relaxed);

        // Latch mode stops reading once it has a table with the fields that
        // matter. Live mode never stops, which is the whole point: a
        // resolution or DLSS preset change destroys these resources and
        // recreates them, and a handle latched across that is a dead pointer.
        if (take && m == 1 &&
            (g_tbl.have & (KEY_MVEC | KEY_MV_SCALE)) == (KEY_MVEC | KEY_MV_SCALE))
            take = false;

        if (take)
        {
            capture(p);

            // ---- R106: THE COPY, TAKEN HERE INSTEAD OF AT A BARRIER ----
            // Before the real evaluate runs, because the game has finished
            // writing the buffer - that is why it is handing it over.
            // R118. ONLY 1 COPIES. 2 is AUTO and means "not yet": it sits
            // inert until gpu1_context measures the barrier route at zero and
            // calls set_eval_copy(1). Testing != 0 here, as this did, would
            // make auto identical to on and there would be no fallback, just
            // a second name for the same setting.
            const int ec = g_eval_copy_mode.load(std::memory_order_relaxed);
            if (ec == 1 && cl != nullptr && g_mvec_hook_fn != nullptr)
            {
                // ---- R106b: ONCE PER FRAME, AND ONLY THE SUPERSAMPLING
                //      FEATURE. THIS IS WHAT WAS CORRUPTING THE PICTURE ----
                //
                // MEASURED, Plague: eval-copies=6002 against 3001 sealed
                // frames. Exactly 2:1. This title creates TWO NGX features
                // (creates=2 - frame generation is in that folder) and both
                // were reaching the copy, so every frame got the scene's
                // vectors overwritten by frame generation's. That is strobing
                // while standing still and colour drift, exactly as reported.
                //
                // Two guards, and BOTH are required. The handle filter alone
                // is not enough because a feature can evaluate more than once
                // per frame; the frame dedupe alone is not enough because the
                // wrong feature might win the race to be first.
                //
                // If sr-handle is not known - we were not present for
                // CreateFeature - we take NOTHING here rather than guess.
                // Publishing the table from an unfiltered evaluate is a
                // reading; COPYING from one is corruption, and the two do not
                // deserve the same permissiveness.
                const unsigned long long fr =
                    g_frames.load(std::memory_order_relaxed);
                // R135. Any LATCHED scene feature, not just SuperSampling.
                // The rule is unchanged - an unidentified handle still copies
                // nothing - only the set of identified handles grew.
                const bool sr_ok =
                    g_handle_known.load(std::memory_order_relaxed) &&
                    is_latched_scene_handle((unsigned long long)(uintptr_t)h);

                if (!sr_ok || g_eval_last_frame == fr)
                {
                    g_eval_skips.fetch_add(1, std::memory_order_relaxed);
                }
                else
                {
                g_eval_last_frame = fr;
                table et;
                if (read(et) && (et.have & KEY_MVEC) != 0u && et.mvec != 0ull)
                {
                    ID3D12Resource *r = (ID3D12Resource *)(uintptr_t)et.mvec;
                    const D3D12_RESOURCE_STATES SR = (D3D12_RESOURCE_STATES)
                        (D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

                    D3D12_RESOURCE_BARRIER b = {};
                    b.Type  = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                    b.Transition.pResource   = r;
                    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    b.Transition.StateBefore = SR;
                    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
                    cl->ResourceBarrier(1, &b);

                    g_mvec_hook_fn((void *)cl, et.mvec);

                    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
                    b.Transition.StateAfter  = SR;
                    cl->ResourceBarrier(1, &b);

                    g_eval_copies.fetch_add(1, std::memory_order_relaxed);
                    if (hsi >= 0)   // R300b
                    {
                        g_hs[hsi].copies.fetch_add(1, std::memory_order_relaxed);
                        g_hs[hsi].w.store(et.sub_w, std::memory_order_relaxed);
                        g_hs[hsi].hh.store(et.sub_h, std::memory_order_relaxed);
                    }
                }
                }
            }
        }
    }

    // R121. Same late resolve as hook_create, for the same reason. Evaluate's
    // failure mode is different - it has no out-handle, so the caller is told
    // Fail and nothing is left dangling - but a DLSS evaluate that silently
    // fails for a whole window is a black frame, and the fix costs one lookup.
    if (g_real_eval == nullptr)
    {
        HMODULE ngx = ngx_module();
        if (ngx != nullptr)
        {
            void *late = (void *)(g_real_gpa ? g_real_gpa(ngx, NAME_EVAL)
                                             : GetProcAddress(ngx, NAME_EVAL));
            if (late != nullptr && late != (void *)&hook_evaluate)
                g_real_eval = (pf_evaluate)late;
        }
    }
    if (g_real_eval == nullptr) return NVSDK_NGX_Result_Fail;
    return g_real_eval(cl, h, p, cb);
}

// ---- R183: COUNT THE RELEASES, AND GET OUT OF THE WAY ----
//
// The mirror of R121's rule on the create side: this hook must call through
// UNCONDITIONALLY and must never swallow a result. A feature the game believes
// it released and we did not pass on is a leak WE caused while measuring one,
// which would be the instrument becoming the fault it was built to find.
//
// The counter is incremented before the call, not after, so a release that
// crashes inside NGX still shows up as attempted. That matters: a create/release
// pair that never completes is a different shape from one that never started.
NVSDK_NGX_Result NVSDK_CONV hook_release(NVSDK_NGX_Handle *h)
{
    g_releases.fetch_add(1, std::memory_order_relaxed);

    // R180. BEFORE the real release, while the handle value is still the one
    // the evaluate filter is comparing against. Afterwards the pointer may be
    // reused by the next create and unlatching it would free the wrong slot.
    if (mgpu::t119::feature_diag())
    {   // R300f2 (T1.15, FeatureDiag=1 only): every release, with whether it was a latched scene feature.
        static std::atomic<unsigned int> rel_budget{400};
        if (rel_budget.load(std::memory_order_relaxed) > 0u)
        {
            rel_budget.fetch_sub(1u, std::memory_order_relaxed);
            const unsigned long long hv0 = (unsigned long long)(uintptr_t)h;
            char rv[300];
            snprintf(rv, sizeof rv,
                     "[MGPU][R300f2] FEATURE RELEASE handle=%p was_latched_scene=%d | creates=%llu releases=%llu (this one included)",
                     (void *)(uintptr_t)hv0, is_latched_scene_handle(hv0) ? 1 : 0,
                     (unsigned long long)g_creates.load(std::memory_order_relaxed),
                     (unsigned long long)g_releases.load(std::memory_order_relaxed));
            mgpu::diag::info(rv);
        }
    }
    unlatch_scene_handle((unsigned long long)(uintptr_t)h);

    // R300b (SceneHandleSet=1 only): g_sr_handle must not outlive its feature. If the one just released was
    // it, move it to a scene feature that is still live (0 if none), never left on a dead handle.
    if (mgpu::t119::scene_handle_set())
    {
        const unsigned long long hv = (unsigned long long)(uintptr_t)h;
        hs_release(hv);
        if (g_sr_handle.load(std::memory_order_relaxed) == hv)
        {
            unsigned long long next = 0ull;
            const unsigned int n = g_scene_handle_n.load(std::memory_order_acquire);
            for (unsigned int i = 0; i < n && i < SCENE_FEATURE_SLOTS; ++i)
            {
                const unsigned long long c = g_scene_handles[i].load(std::memory_order_relaxed);
                if (c != 0ull && c != hv) { next = c; break; }
            }
            g_sr_handle.store(next, std::memory_order_relaxed);   // the next CreateFeature fills it as before
        }
    }

    if (g_real_release == nullptr)
    {
        HMODULE ngx = ngx_module();
        if (ngx != nullptr)
        {
            void *late = (void *)(g_real_gpa ? g_real_gpa(ngx, NAME_RELEASE)
                                             : GetProcAddress(ngx, NAME_RELEASE));
            if (late != nullptr && late != (void *)&hook_release)
                g_real_release = (pf_release)late;
        }
    }
    // No real pointer means we must not pretend to have released anything.
    // Fail is the honest answer and it is what the caller would have got had
    // the export been missing.
    if (g_real_release == nullptr) return NVSDK_NGX_Result_Fail;
    return g_real_release(h);
}

// R232. What hook_create did after the real call, moved here verbatim so the
// D3D11 create (hook_create11) shares it: fgmap learn, the R134 census line,
// the R135 scene-handle latch and the create flags. Nothing API-specific.
static void after_create(NVSDK_NGX_Feature id, NVSDK_NGX_Parameter *p,
                         NVSDK_NGX_Handle **out, NVSDK_NGX_Result r)
{
    // ---- R300f2 (T1.15, FeatureDiag=1 only): EVERY CREATE, WITH ITS PARAMETERS ----
    // R134 below names an id the FIRST time only. Measured (Dom, Starfield): the game creates and releases
    // DLSS features repeatedly, and after one burst it stopped calling Evaluate for minutes. One line per
    // create, up to 400 events; reading parameters changes nothing. Here in after_create, so D3D11 too.
    if (mgpu::t119::feature_diag())
    {
        static std::atomic<unsigned int> ev_budget{400};
        if (ev_budget.load(std::memory_order_relaxed) > 0u)
        {
            ev_budget.fetch_sub(1u, std::memory_order_relaxed);
            unsigned int w = 0u, hh = 0u, ow = 0u, oh = 0u, q = 0u, fl = 0u;
            const bool hw = get_u(p, K_W, w), hhh = get_u(p, K_H, hh), how = get_u(p, K_OW, ow), hoh = get_u(p, K_OH, oh);
            const bool hq = get_u(p, K_QUAL, q), hf = get_u(p, K_FLAGS, fl);
            char ev[460];
            snprintf(ev, sizeof ev,
                     "[MGPU][R300f2] FEATURE CREATE id=%u result=0x%08X handle=%p scene=%d | creates=%llu releases=%llu | "
                     "in=%d x %d out=%d x %d quality=%d createflags=0x%X",
                     (unsigned int)id, (unsigned)r, (out != nullptr) ? (void *)*out : nullptr,
                     is_scene_feature((unsigned int)id) ? 1 : 0,
                     (unsigned long long)g_creates.load(std::memory_order_relaxed),
                     (unsigned long long)g_releases.load(std::memory_order_relaxed),
                     hw ? (int)w : -1, hhh ? (int)hh : -1, how ? (int)ow : -1, hoh ? (int)oh : -1, hq ? (int)q : -1,
                     hf ? fl : 0u);
            mgpu::diag::info(ev);
        }
    }

    // D2.0 NR step one: learn which handle is frame generation. Off: one load.
    if (mgpu::fgmap::on() && r == NVSDK_NGX_Result_Success && out != nullptr && *out != nullptr)
        mgpu::fgmap::on_create((unsigned int)id, (unsigned long long)(uintptr_t)*out);

    // ---- R134: NAME EVERY FEATURE THE GAME CREATES, ONCE PER ID ----
    //
    // The latch below fires ONLY on SuperSampling, and until now nothing in
    // any log said what else a title had created. That is the difference
    // between two Cyberpunk 2077 runs on 0.2.2: one reports
    // "creates=1 sr-handle=known | eval-copies=6751 eval-skips=0" and the
    // other "eval-copies=0 eval-skips=3892". The evaluate fallback copies
    // only when the evaluating handle is the latched one, so a title whose
    // upscale is a DIFFERENT feature - Ray Reconstruction replaces the
    // separate upscale and denoise passes with one DLSS-D feature - creates
    // no SuperSampling handle at all, and every evaluate is correctly and
    // uselessly skipped.
    //
    // THIS LINE DOES NOT FIX THAT. It prints the number needed to fix it,
    // because widening the latch to an enum value nobody has measured is a
    // guess, and a wrong one silently copies frame generation's vectors over
    // the scene's - the exact corruption the filter was added to stop.
    //
    // Sixteen slots, one line each, first time only. No allocation, no lock,
    // and nothing here changes what is latched or copied.
    {
        static std::atomic<unsigned int> seen[16];
        static std::atomic<unsigned int> seen_n{0};
        bool said = false;
        const unsigned int idv = (unsigned int)id;
        const unsigned int n = seen_n.load(std::memory_order_relaxed);
        for (unsigned int i = 0; i < n && i < 16u; ++i)
            if (seen[i].load(std::memory_order_relaxed) == idv) { said = true; break; }
        if (!said && n < 16u)
        {
            seen[n].store(idv, std::memory_order_relaxed);
            seen_n.store(n + 1u, std::memory_order_relaxed);
            char fl[420];
            snprintf(fl, sizeof fl,
                     "[MGPU][R134] GAME CreateFeature: id=%u result=0x%08X handle=%p%s. "
                     "R135: the evaluate-copy filter latches the SCENE features - id=%u "
                     "(SuperSampling) and id=%u (measured as Ray Reconstruction on "
                     "Cyberpunk 2077) - and nothing else. An id absent from that set "
                     "evaluates and is skipped on purpose, which reads as eval-copies=0 "
                     "with eval-skips climbing and a lane that carries nothing while depth "
                     "keeps working. Frame generation is excluded deliberately: copying "
                     "from it overwrote the scene's vectors every frame on A Plague Tale. "
                     "IF THIS LINE NAMES AN ID OUTSIDE THE SET ON A TITLE WHOSE VECTORS "
                     "NEVER ARRIVE, that is the next number to measure - do not guess it.",
                     idv, (unsigned)r,
                     (out != nullptr) ? (void *)*out : nullptr,
                     is_scene_feature(idv)
                         ? " <- scene feature, the latch fires on this one"
                         : " <- NOT a latched scene feature, evaluates from it are skipped",
                     (unsigned int)NVSDK_NGX_Feature_SuperSampling,
                     FEATURE_ID_RAY_RECONSTRUCTION);
            mgpu::diag::info(fl);
        }
    }

    if (ok(r) && out != nullptr && *out != nullptr &&
        is_scene_feature((unsigned int)id))
    {
        // R135. g_sr_handle keeps its old meaning - the most recent scene
        // feature - because the census and the R101 report line both read it
        // and neither should change shape for this. The SET is what the
        // evaluate filter now tests against.
        g_sr_handle.store((unsigned long long)(uintptr_t)(*out),
                          std::memory_order_relaxed);
        latch_scene_handle((unsigned long long)(uintptr_t)(*out));
        g_handle_known.store(true, std::memory_order_relaxed);

        // THE ONE PLACE THE CREATE FLAGS ARE GUARANTEED TO EXIST. Read here,
        // from the block the game just used, before anything else touches it.
        unsigned int fl = 0;
        if (p != nullptr && ok(p->Get(K_FLAGS, &fl)))
        {
            g_flags.store(fl, std::memory_order_relaxed);
            g_flags_known.store(true, std::memory_order_relaxed);
        }
    }
}

NVSDK_NGX_Result NVSDK_CONV hook_create(ID3D12GraphicsCommandList *cl,
                                        NVSDK_NGX_Feature id,
                                        NVSDK_NGX_Parameter *p,
                                        NVSDK_NGX_Handle **out)
{
    g_creates.fetch_add(1, std::memory_order_relaxed);

    // ---- R121: NEVER RETURN WITHOUT ANSWERING THE OUT-HANDLE ----
    //
    // The ordering fix above closes the window that made this reachable. This
    // is the second line of defence, because the failure it produced was a
    // crash in somebody else's module and the cost of being wrong again is
    // too high to rely on one fix.
    //
    // Two changes. FIRST, try to resolve the real entry point here rather
    // than giving up - by the time anything calls us the NGX module is loaded
    // by definition, so a late resolve almost always succeeds. SECOND, if it
    // genuinely cannot be resolved, ZERO THE OUT-HANDLE before returning
    // Fail. A caller that ignores the result and dereferences *out then reads
    // a null it can be blamed for, instead of whatever was on its stack.
    if (g_real_create == nullptr)
    {
        HMODULE ngx = ngx_module();
        if (ngx != nullptr)
        {
            void *late = (void *)(g_real_gpa ? g_real_gpa(ngx, NAME_CREATE)
                                             : GetProcAddress(ngx, NAME_CREATE));
            if (late != nullptr && late != (void *)&hook_create)
                g_real_create = (pf_create)late;
        }
    }
    if (g_real_create == nullptr)
    {
        if (out != nullptr) *out = nullptr;
        mgpu::diag::error(
            "[MGPU][R121] CreateFeature reached our hook with no real entry point behind it. "
            "Returning Fail with the out-handle zeroed. THIS SHOULD NOW BE UNREACHABLE: the "
            "real pointer is published before any slot is patched. If this line appears, the "
            "ordering fix did not take and the window it closed is open again.");
        return NVSDK_NGX_Result_Fail;
    }
    const NVSDK_NGX_Result r = g_real_create(cl, id, p, out);
    after_create(id, p, out, r);   // R232: the post-call block, moved verbatim to after_create above
    return r;
}


// ---- R232: THE D3D11 THREE ----
//
// Same filters as the D3D12 hooks above (scene-feature latch, once per frame,
// nothing copied from an unidentified handle), same table capture, same
// counters. The one difference is the copy: there is no barrier on D3D11 and
// no command list of ours - the resource goes to the DX11 producer through
// g_mvec_hook11_fn, which copies it on the game's context into a shared
// texture and hops it. EvalCopy: 1 copies, 2 (auto) ALSO copies here - on
// D3D11 the evaluate is the only route there is, so auto has nothing to
// fall back from; 0 copies nothing.

NVSDK_NGX_Result NVSDK_CONV hook_evaluate11(ID3D11DeviceContext *ctx,
                                            const NVSDK_NGX_Handle *h,
                                            const NVSDK_NGX_Parameter *p,
                                            void *cb)
{
    g_evals.fetch_add(1, std::memory_order_relaxed);
    g_evals11.fetch_add(1, std::memory_order_relaxed);
    if (mgpu::fgmap::on()) fgmap_tap(h, p);

    const int m = g_mode.load(std::memory_order_relaxed);
    if (m != 0)
    {
        bool take = true;
        if (g_handle_known.load(std::memory_order_relaxed))
            take = ((unsigned long long)(uintptr_t)h ==
                    g_sr_handle.load(std::memory_order_relaxed));
        if (take && m == 1 &&
            (g_tbl.have & (KEY_MVEC | KEY_MV_SCALE)) == (KEY_MVEC | KEY_MV_SCALE))
            take = false;
        if (take)
        {
            capture(p);
            const int ec = g_eval_copy_mode.load(std::memory_order_relaxed);
            if (ec != 0 && ctx != nullptr && g_mvec_hook11_fn != nullptr)
            {
                const unsigned long long fr = g_frames.load(std::memory_order_relaxed);
                const bool sr_ok =
                    g_handle_known.load(std::memory_order_relaxed) &&
                    is_latched_scene_handle((unsigned long long)(uintptr_t)h);
                if (!sr_ok || g_eval_last_frame == fr)
                {
                    g_eval_skips.fetch_add(1, std::memory_order_relaxed);
                    g_skips11.fetch_add(1, std::memory_order_relaxed);
                }
                else
                {
                    g_eval_last_frame = fr;
                    table et;
                    if (read(et) && (et.have & KEY_MVEC) != 0u && et.mvec != 0ull)
                    {
                        g_mvec_hook11_fn((void *)ctx, et.mvec);
                        g_eval_copies.fetch_add(1, std::memory_order_relaxed);
                        g_copies11.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        }
    }

    if (g_real_eval11 == nullptr)
    {
        HMODULE ngx = ngx_module();
        if (ngx != nullptr)
        {
            void *late = (void *)(g_real_gpa ? g_real_gpa(ngx, NAME_EVAL11)
                                             : GetProcAddress(ngx, NAME_EVAL11));
            if (late != nullptr && late != (void *)&hook_evaluate11)
                g_real_eval11 = (pf_evaluate11)late;
        }
    }
    if (g_real_eval11 == nullptr) return NVSDK_NGX_Result_Fail;
    return g_real_eval11(ctx, h, p, cb);
}

NVSDK_NGX_Result NVSDK_CONV hook_release11(NVSDK_NGX_Handle *h)
{
    g_releases.fetch_add(1, std::memory_order_relaxed);
    unlatch_scene_handle((unsigned long long)(uintptr_t)h);
    if (g_real_release11 == nullptr)
    {
        HMODULE ngx = ngx_module();
        if (ngx != nullptr)
        {
            void *late = (void *)(g_real_gpa ? g_real_gpa(ngx, NAME_RELEASE11)
                                             : GetProcAddress(ngx, NAME_RELEASE11));
            if (late != nullptr && late != (void *)&hook_release11)
                g_real_release11 = (pf_release)late;
        }
    }
    if (g_real_release11 == nullptr) return NVSDK_NGX_Result_Fail;
    return g_real_release11(h);
}

NVSDK_NGX_Result NVSDK_CONV hook_create11(ID3D11DeviceContext *ctx,
                                          NVSDK_NGX_Feature id,
                                          NVSDK_NGX_Parameter *p,
                                          NVSDK_NGX_Handle **out)
{
    g_creates.fetch_add(1, std::memory_order_relaxed);
    if (g_real_create11 == nullptr)
    {
        HMODULE ngx = ngx_module();
        if (ngx != nullptr)
        {
            void *late = (void *)(g_real_gpa ? g_real_gpa(ngx, NAME_CREATE11)
                                             : GetProcAddress(ngx, NAME_CREATE11));
            if (late != nullptr && late != (void *)&hook_create11)
                g_real_create11 = (pf_create11)late;
        }
    }
    if (g_real_create11 == nullptr)
    {
        if (out != nullptr) *out = nullptr;
        mgpu::diag::error("[MGPU][R121] D3D11 CreateFeature reached our hook with no real entry point "
                          "behind it. Returning Fail with the out-handle zeroed (R232).");
        return NVSDK_NGX_Result_Fail;
    }
    const NVSDK_NGX_Result r = g_real_create11(ctx, id, p, out);
    {
        static std::atomic<bool> said{false};
        if (!said.exchange(true))
            mgpu::diag::info("[MGPU][R232] the game creates its DLSS feature on D3D11 "
                             "(NVSDK_NGX_D3D11_CreateFeature seen): the D3D11 evaluate tap is live.");
    }
    after_create(id, p, out, r);
    return r;
}


// ---- R236: OBSERVE THE GAME'S NGX INIT ON D3D11 (pass-through) ----
//
// Three outcomes by launch timing on Rise (Marcelo, 2026-10-06): NR fine;
// our NR CreateFeature refused (ERROR 302); a freeze/slowdown shaped like
// Requiem's. Which of the game's NGX calls races ours is not in any log:
// the game's Init loads the one NGX core in the process, and nothing
// timestamps it. These hooks do that and nothing else: every argument is
// forwarded untouched through a 7-slot x64 forwarder (the exported Init
// signatures differ by SDK revision in arity; extra slots are ignored by
// the callee and never read by us beyond logging), the result is returned
// as received. One line per call, with the real result.
typedef unsigned long long(NVSDK_CONV *pf_any7)(unsigned long long, unsigned long long, unsigned long long,
                                                unsigned long long, unsigned long long, unsigned long long,
                                                unsigned long long);
const char *const NAME_INIT11     = "NVSDK_NGX_D3D11_Init";
const char *const NAME_INITEXT11  = "NVSDK_NGX_D3D11_Init_Ext";
const char *const NAME_INITPID11  = "NVSDK_NGX_D3D11_Init_with_ProjectID";
const char *const NAME_SHUTDOWN11 = "NVSDK_NGX_D3D11_Shutdown";
const char *const NAME_SHUTDOWN111= "NVSDK_NGX_D3D11_Shutdown1";
pf_any7 g_real_init11 = nullptr, g_real_initext11 = nullptr, g_real_initpid11 = nullptr,
        g_real_shutdown11 = nullptr, g_real_shutdown111 = nullptr;
std::atomic<unsigned long long> g_game_init11_qpc{0};   // first game NGX init seen (D3D11), qpc
std::atomic<unsigned long long> g_game_init_entered{0}, g_game_init_returned{0};   // R239

static void say_ngx_call(const char *name, unsigned long long r, bool before)
{
    char l[300];
    snprintf(l, sizeof l, "[MGPU][R236] GAME %s %s%s%s", name,
             before ? "entered" : "returned",
             before ? "" : " result=0x",
             before ? "" : "");
    if (!before) { const size_t n = strlen(l); snprintf(l + n, sizeof l - n, "%08llX", r); }
    mgpu::diag::info(l);
}
#define MGPU_NGX_OBSERVE(FN, REALPTR, NAME)                                                            \
    unsigned long long NVSDK_CONV FN(unsigned long long a, unsigned long long b, unsigned long long c,  \
                                     unsigned long long d, unsigned long long e, unsigned long long f,  \
                                     unsigned long long g)                                              \
    {                                                                                                   \
        unsigned long long zero = 0ull;                                                                 \
        g_game_init11_qpc.compare_exchange_strong(zero, qpc(), std::memory_order_acq_rel);              \
        g_game_init_entered.fetch_add(1, std::memory_order_acq_rel);                                    \
        say_ngx_call(NAME, 0ull, true);                                                                 \
        if (REALPTR == nullptr) { g_game_init_returned.fetch_add(1, std::memory_order_acq_rel);          \
                                  return (unsigned long long)NVSDK_NGX_Result_Fail; }                   \
        const unsigned long long r = REALPTR(a, b, c, d, e, f, g);                                      \
        g_game_init_returned.fetch_add(1, std::memory_order_acq_rel);                                   \
        say_ngx_call(NAME, r, false);                                                                   \
        return r;                                                                                       \
    }
MGPU_NGX_OBSERVE(hook_init11,     g_real_init11,     NAME_INIT11)
MGPU_NGX_OBSERVE(hook_initext11,  g_real_initext11,  NAME_INITEXT11)
MGPU_NGX_OBSERVE(hook_initpid11,  g_real_initpid11,  NAME_INITPID11)
MGPU_NGX_OBSERVE(hook_shutdown11, g_real_shutdown11, NAME_SHUTDOWN11)
MGPU_NGX_OBSERVE(hook_shutdown111,g_real_shutdown111,NAME_SHUTDOWN111)
#undef MGPU_NGX_OBSERVE

// ---- R245: the nvapi trace (TraceNvapi=1), see calibrator.hpp ----
std::atomic<int> g_trace_nvapi{0};
std::atomic<unsigned> g_trace_qi_lines{0}, g_trace_gpa_lines{0};
std::atomic<unsigned long long> g_trace_qi_calls{0};
typedef void *(__cdecl *pf_nvapi_qi)(unsigned int);
pf_nvapi_qi g_real_nvapi_qi = nullptr;
const unsigned TRACE_QI_CAP = 600u, TRACE_GPA_CAP = 300u;

// Public ids from the NVAPI headers. NGX's own interfaces are not public and
// print raw; the raw sequence is still the thing to diff between a pass and
// a fail.
const char *nvapi_id_name(unsigned int id)
{
    switch (id)
    {
    case 0x0150E828u: return "NvAPI_Initialize";
    case 0xD22BDD7Eu: return "NvAPI_Unload";
    case 0x6C2D048Cu: return "NvAPI_GetErrorMessage";
    case 0x01053FA5u: return "NvAPI_GetInterfaceVersionString";
    case 0x2926AAADu: return "NvAPI_SYS_GetDriverAndBranchVersion";
    case 0xE5AC921Fu: return "NvAPI_EnumPhysicalGPUs";
    case 0xCEEE8E9Fu: return "NvAPI_GPU_GetFullName";
    case 0x2DDFB66Eu: return "NvAPI_GPU_GetPCIIdentifiers";
    case 0x0694D52Eu: return "NvAPI_DRS_CreateSession";
    case 0x375DBD6Bu: return "NvAPI_DRS_LoadSettings";
    case 0xFCBC7E14u: return "NvAPI_DRS_SaveSettings";
    case 0xDAD9CFF8u: return "NvAPI_DRS_DestroySession";
    case 0xDA8466A0u: return "NvAPI_DRS_GetBaseProfile";
    case 0xEEE566B2u: return "NvAPI_DRS_FindApplicationByName";
    case 0x73BF8338u: return "NvAPI_DRS_GetSetting";
    case 0x577DD202u: return "NvAPI_DRS_SetSetting";
    case 0x4B708B54u: return "NvAPI_D3D_GetCurrentSLIState";
    default: return nullptr;
    }
}

// Module name for a code address (the caller of a hook, via _ReturnAddress).
// GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT: no reference taken, nothing to
// free. Base name only.
void module_of(const void *addr, char *out, size_t n)
{
    HMODULE m = nullptr;
    out[0] = '\0';
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)addr, &m) && m != nullptr)
    {
        char full[MAX_PATH] = {};
        if (GetModuleFileNameA(m, full, sizeof full) != 0)
        {
            const char *b = strrchr(full, '\\');
            snprintf(out, n, "%s", b ? b + 1 : full);
            return;
        }
    }
    snprintf(out, n, "?@0x%p", addr);
}

bool is_ngx_module_name(const char *m)
{
    return _strnicmp(m, "_nvngx", 6) == 0 || _strnicmp(m, "nvngx_", 6) == 0;
}

// R245b: three public NVAPI entry points wrapped so their ARGUMENTS and
// RESULTS are logged, not just the fact of being asked for. Signatures from
// the public nvapi.h (all __cdecl, status is an int). NVDRS_SETTING_V1 layout
// (offsets, bytes): version 0 | settingName NvU16[2048] 4 | settingId 4100 |
// settingType 4104 | settingLocation 4108 | isCurrentPredefined 4112 |
// isPredefinedValid 4116 | predefinedValue (union, 4100) 4120 | currentValue
// (union, 4100) 8220. The name is logged so a wrong layout shows as garbage
// instead of being trusted.
typedef int (__cdecl *pf_drs_get_setting)(void *session, void *profile, unsigned int id, void *setting);
typedef int (__cdecl *pf_drs_find_app)(void *session, const unsigned short *name, void **profile, void *app);
typedef int (__cdecl *pf_sys_drv_ver)(unsigned int *ver, char *branch);
typedef int (__cdecl *pf_nvapi_status_void)(void);
typedef int (__cdecl *pf_drs_session)(void *session);
typedef int (__cdecl *pf_drs_base_profile)(void *session, void **profile);
pf_nvapi_status_void g_real_nvapi_initialize = nullptr;
pf_drs_session       g_real_drs_load_settings = nullptr;
pf_drs_base_profile  g_real_drs_get_base_profile = nullptr;
pf_drs_get_setting g_real_drs_get_setting = nullptr;
pf_drs_find_app    g_real_drs_find_app    = nullptr;
pf_sys_drv_ver     g_real_sys_drv_ver     = nullptr;

void narrow(const unsigned short *w, char *out, size_t n)
{
    size_t i = 0;
    for (; w != nullptr && i + 1 < n && w[i] != 0; ++i) out[i] = (w[i] < 128) ? (char)w[i] : '?';
    out[i] = '\0';
}

// R245c: the three results the core holds before the fork. NvAPI_Initialize
// is the first nvapi call it makes; a status other than 0 there is the
// simplest reason for it to leave the nvapi route.
int __cdecl hook_nvapi_initialize(void)
{
    const void *caller = _ReturnAddress();
    const int st = g_real_nvapi_initialize ? g_real_nvapi_initialize() : -1;
    char from[80]; module_of(caller, from, sizeof from);
    char l[200];
    snprintf(l, sizeof l, "[MGPU][R245][NVAPI] NvAPI_Initialize() -> status=%d | from %s tid=%lu",
             st, from, (unsigned long)GetCurrentThreadId());
    mgpu::diag::info(l);
    return st;
}
int __cdecl hook_drs_load_settings(void *session)
{
    const void *caller = _ReturnAddress();
    const int st = g_real_drs_load_settings ? g_real_drs_load_settings(session) : -1;
    char from[80]; module_of(caller, from, sizeof from);
    char l[200];
    snprintf(l, sizeof l, "[MGPU][R245][DRS] LoadSettings(session=%p) -> status=%d | from %s tid=%lu",
             session, st, from, (unsigned long)GetCurrentThreadId());
    mgpu::diag::info(l);
    return st;
}
int __cdecl hook_drs_get_base_profile(void *session, void **profile)
{
    const void *caller = _ReturnAddress();
    const int st = g_real_drs_get_base_profile ? g_real_drs_get_base_profile(session, profile) : -1;
    char from[80]; module_of(caller, from, sizeof from);
    char l[200];
    snprintf(l, sizeof l, "[MGPU][R245][DRS] GetBaseProfile -> status=%d profile=%p | from %s tid=%lu",
             st, profile ? *profile : nullptr, from, (unsigned long)GetCurrentThreadId());
    mgpu::diag::info(l);
    return st;
}

int __cdecl hook_drs_get_setting(void *session, void *profile, unsigned int id, void *setting)
{
    const void *caller = _ReturnAddress();
    const int st = g_real_drs_get_setting ? g_real_drs_get_setting(session, profile, id, setting) : -1;
    char from[80]; module_of(caller, from, sizeof from);
    char name[120] = "-"; unsigned type = 0xFFFFFFFFu, loc = 0, cur = 0, pre = 0, isp = 0;
    if (setting != nullptr && st == 0)
    {
        const unsigned char *b = (const unsigned char *)setting;
        narrow((const unsigned short *)(b + 4), name, sizeof name);
        memcpy(&type, b + 4104, 4); memcpy(&loc, b + 4108, 4); memcpy(&isp, b + 4112, 4);
        memcpy(&pre, b + 4120, 4); memcpy(&cur, b + 8220, 4);
    }
    char l[400];
    snprintf(l, sizeof l,
             "[MGPU][R245][DRS] GetSetting id=0x%08X -> status=%d | name=\"%s\" type=%u location=%u "
             "currentPredefined=%u predefined=0x%08X CURRENT=0x%08X (%u) | from %s tid=%lu",
             id, st, name, type, loc, isp, pre, cur, cur, from, (unsigned long)GetCurrentThreadId());
    mgpu::diag::info(l);
    return st;
}

int __cdecl hook_drs_find_app(void *session, const unsigned short *appname, void **profile, void *app)
{
    const void *caller = _ReturnAddress();
    const int st = g_real_drs_find_app ? g_real_drs_find_app(session, appname, profile, app) : -1;
    char from[80]; module_of(caller, from, sizeof from);
    char nm[260]; narrow(appname, nm, sizeof nm);
    char l[400];
    snprintf(l, sizeof l, "[MGPU][R245][DRS] FindApplicationByName \"%s\" -> status=%d profile=%p | from %s tid=%lu",
             nm, st, profile ? *profile : nullptr, from, (unsigned long)GetCurrentThreadId());
    mgpu::diag::info(l);
    return st;
}

int __cdecl hook_sys_drv_ver(unsigned int *ver, char *branch)
{
    const void *caller = _ReturnAddress();
    const int st = g_real_sys_drv_ver ? g_real_sys_drv_ver(ver, branch) : -1;
    char from[80]; module_of(caller, from, sizeof from);
    char l[300];
    snprintf(l, sizeof l, "[MGPU][R245][SYS] GetDriverAndBranchVersion -> status=%d version=%u branch=\"%.63s\" | from %s tid=%lu",
             st, ver ? *ver : 0u, (st == 0 && branch) ? branch : "-", from, (unsigned long)GetCurrentThreadId());
    mgpu::diag::info(l);
    return st;
}

void *__cdecl hook_nvapi_qi(unsigned int id)
{
    const void *caller = _ReturnAddress();
    void *p = (g_real_nvapi_qi != nullptr) ? g_real_nvapi_qi(id) : nullptr;
    // R245b: hand out the wrappers for the three we read arguments of. The
    // real pointer is the same for every caller, so storing it once is exact.
    if (p != nullptr)
    {
        if (id == 0x0150E828u) { g_real_nvapi_initialize = (pf_nvapi_status_void)p; p = (void *)&hook_nvapi_initialize; }
        else if (id == 0x375DBD6Bu) { g_real_drs_load_settings = (pf_drs_session)p; p = (void *)&hook_drs_load_settings; }
        else if (id == 0xDA8466A0u) { g_real_drs_get_base_profile = (pf_drs_base_profile)p; p = (void *)&hook_drs_get_base_profile; }
        else if (id == 0x73BF8338u) { g_real_drs_get_setting = (pf_drs_get_setting)p; p = (void *)&hook_drs_get_setting; }
        else if (id == 0xEEE566B2u) { g_real_drs_find_app = (pf_drs_find_app)p; p = (void *)&hook_drs_find_app; }
        else if (id == 0x2926AAADu) { g_real_sys_drv_ver = (pf_sys_drv_ver)p; p = (void *)&hook_sys_drv_ver; }
    }
    g_trace_qi_calls.fetch_add(1, std::memory_order_relaxed);
    const unsigned n = g_trace_qi_lines.fetch_add(1, std::memory_order_relaxed);
    if (n < TRACE_QI_CAP)
    {
        char from[80]; module_of(caller, from, sizeof from);
        const char *nm = nvapi_id_name(id);
        char l[300];
        snprintf(l, sizeof l,
                 "[MGPU][R245][NVAPI] QueryInterface id=0x%08X%s%s%s -> %s | from %s tid=%lu",
                 id, nm ? " (" : "", nm ? nm : "", nm ? ")" : "",
                 p ? "ok" : "NULL", from, (unsigned long)GetCurrentThreadId());
        mgpu::diag::info(l);
    }
    else if (n == TRACE_QI_CAP)
        mgpu::diag::info("[MGPU][R245][NVAPI] cap reached: further QueryInterface lines not written (the count continues)");
    return p;
}

FARPROC WINAPI hook_gpa(HMODULE mod, LPCSTR name)
{
    FARPROC real = (g_real_gpa != nullptr) ? g_real_gpa(mod, name)
                                           : GetProcAddress(mod, name);
    // HIWORD(name)==0 means resolution by ordinal, and the pointer is not a
    // string at all. Dereferencing it is the classic crash in code that does
    // this, so it is checked before any strcmp.
    if (real == nullptr || name == nullptr || ((ULONG_PTR)name >> 16) == 0) return real;

    g_gpa_calls.fetch_add(1, std::memory_order_relaxed);

    // R245: the trace. Off = these two tests and nothing else.
    if (g_trace_nvapi.load(std::memory_order_relaxed) != 0)
    {
        char from[80]; module_of(_ReturnAddress(), from, sizeof from);
        if (is_ngx_module_name(from))
        {
            const unsigned n = g_trace_gpa_lines.fetch_add(1, std::memory_order_relaxed);
            if (n < TRACE_GPA_CAP)
            {
                char tgt[80]; module_of((const void *)mod, tgt, sizeof tgt);
                char l[300];
                snprintf(l, sizeof l, "[MGPU][R245][GPA] %s asks %s for %s | tid=%lu",
                         from, tgt, name, (unsigned long)GetCurrentThreadId());
                mgpu::diag::info(l);
            }
        }
        if (std::strcmp(name, "nvapi_QueryInterface") == 0)
        {
            g_real_nvapi_qi = (pf_nvapi_qi)real;
            char l[200];
            snprintf(l, sizeof l, "[MGPU][R245][NVAPI] nvapi_QueryInterface resolved by %s - forwarder installed", from);
            mgpu::diag::info(l);
            return (FARPROC)&hook_nvapi_qi;
        }
    }

    if (std::strcmp(name, NAME_EVAL) == 0)
    {
        g_real_eval = (pf_evaluate)real;
        g_resolved.fetch_add(1, std::memory_order_relaxed);
        return (FARPROC)&hook_evaluate;
    }
    if (std::strcmp(name, NAME_CREATE) == 0)
    {
        g_real_create = (pf_create)real;
        return (FARPROC)&hook_create;
    }
    if (std::strcmp(name, NAME_RELEASE) == 0)          // R183
    {
        g_real_release = (pf_release)real;
        return (FARPROC)&hook_release;
    }
    // R232: the D3D11 three.
    if (std::strcmp(name, NAME_EVAL11) == 0)
    {
        g_real_eval11 = (pf_evaluate11)real;
        g_resolved.fetch_add(1, std::memory_order_relaxed);
        return (FARPROC)&hook_evaluate11;
    }
    if (std::strcmp(name, NAME_CREATE11) == 0)
    {
        g_real_create11 = (pf_create11)real;
        return (FARPROC)&hook_create11;
    }
    if (std::strcmp(name, NAME_RELEASE11) == 0)
    {
        g_real_release11 = (pf_release)real;
        return (FARPROC)&hook_release11;
    }
    // R236: observe-only.
    if (std::strcmp(name, NAME_INIT11) == 0)      { g_real_init11 = (pf_any7)real;      return (FARPROC)&hook_init11; }
    if (std::strcmp(name, NAME_INITEXT11) == 0)   { g_real_initext11 = (pf_any7)real;   return (FARPROC)&hook_initext11; }
    if (std::strcmp(name, NAME_INITPID11) == 0)   { g_real_initpid11 = (pf_any7)real;   return (FARPROC)&hook_initpid11; }
    if (std::strcmp(name, NAME_SHUTDOWN11) == 0)  { g_real_shutdown11 = (pf_any7)real;  return (FARPROC)&hook_shutdown11; }
    if (std::strcmp(name, NAME_SHUTDOWN111) == 0) { g_real_shutdown111 = (pf_any7)real; return (FARPROC)&hook_shutdown111; }
    return real;
}

// ---- IAT WALK ----
//
// Patches every import slot in `mod` whose CURRENT VALUE is `find`, replacing
// it with `repl`. Value comparison rather than name comparison is what makes
// this immune to api-set naming, and it is also what makes uninstall exact:
// swapping repl back to find visits the same slots.
unsigned patch_module(HMODULE mod, void *find, void *repl)
{
    if (mod == nullptr || find == nullptr) return 0;

    BYTE *base = (BYTE *)mod;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;

    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;

    const IMAGE_DATA_DIRECTORY &dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0 || dir.Size == 0) return 0;

    // volatile because __except returns it: MSVC does not guarantee a
    // non-volatile local modified inside __try survives the unwind.
    volatile unsigned hits = 0;
    IMAGE_IMPORT_DESCRIPTOR *imp =
        (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress);

    // A structured handler is not optional here. Walking another module's
    // headers is reading memory whose layout we are trusting, and a packed or
    // partially unmapped module is a real thing in shipped games. Faulting in
    // the addon's install path would take the game with it.
    __try
    {
        for (; imp->Name != 0; ++imp)
        {
            if (imp->FirstThunk == 0) continue;
            IMAGE_THUNK_DATA *t = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
            for (; t->u1.Function != 0; ++t)
            {
                if ((void *)(uintptr_t)t->u1.Function != find) continue;

                DWORD old = 0;
                if (!VirtualProtect(&t->u1.Function, sizeof(void *),
                                    PAGE_READWRITE, &old))
                    continue;
                t->u1.Function = (ULONGLONG)(uintptr_t)repl;
                VirtualProtect(&t->u1.Function, sizeof(void *), old, &old);
                ++hits;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return (unsigned)hits;
    }
    return (unsigned)hits;
}

// Walks every module in the process except our own. Called at install and
// again from note_frame while we are still waiting for NGX to show up, which
// is how a DLL loaded after us gets patched without hooking LoadLibrary.
unsigned scan_and_patch(void *find, void *repl)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return 0;

    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    unsigned hits = 0, mods = 0;

    if (Module32FirstW(snap, &me))
    {
        do
        {
            if (me.hModule == g_self) continue;   // never wrap our own calls
            ++mods;
            hits += patch_module(me.hModule, find, repl);
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);

    g_modules.store(mods, std::memory_order_relaxed);
    return hits;
}

// ---- R102: THE CACHED-POINTER SCAN. THE RUNG ABOVE THE IMPORT SWAP ----
//
// WHY IT EXISTS. Plague Tale reported slots=44 resolved=0: every import table
// in the process patched, and nobody asked. sl.interposer.dll IS the D3D12
// entry point - it wraps D3D12CreateDevice - so Streamline is loaded and
// initialised BEFORE ReShade's redirect ever runs, which is before this
// add-on can exist. There is no ReShade event earlier than add-on load, so
// the ordering is unwinnable from inside an add-on. Streamline had already
// resolved the entry point and stashed it in its own data section, where an
// import-table patch cannot reach.
//
// THE INSIGHT: we know the exact value to look for. GetProcAddress on the
// already-loaded nvngx module hands us the real address. So walk the WRITABLE
// DATA sections of every module, find 8-byte words equal to that address, and
// swap them. It is the same pointer store the import patch does, just in
// .data instead of the IAT - no code bytes touched, no trampoline, no length
// disassembler, and reversible by the identical mechanism.
//
// ---- R115: WHAT THE RUNG ACTUALLY WROTE, RECORDED ----
//
// Every hit is recorded here before anything is said about it. POD only, and
// filled INSIDE the structured handler where no C++ object may live, so the
// logging happens afterwards from the caller.
//
// WHY IT EXISTS. The comment below asserts that a word equal to the address of
// one specific NGX export IS that pointer - that a false positive cannot
// happen. That is an assumption, it has never been checked, and the whole rung
// rests on it. On Battlefield 6 this rung writes two words into
// sl.common.dll, the module the reporter's game dies inside, and returns
// evaluates=0 on every run we have. Before deciding anything about that, we
// should know what those two words are.
struct data_hit
{
    HMODULE  mod;
    char     section[12];
    unsigned rva;              // from the module base
    unsigned long long was;    // the value we replaced
};
data_hit g_hits[16] = {};
unsigned g_hit_n = 0;

// WHAT IS SKIPPED AND WHY IT MATTERS. Our own module, always: g_real_eval
// holds that exact address, and swapping it would point this file's
// pass-through at itself - unbounded recursion on the first frame. The nvngx
// module itself is skipped for the same class of reason. Executable sections
// are skipped because a matching word there is an immediate operand, not a
// pointer slot.
//
// FALSE POSITIVES. A word equal to the address of one specific NGX export is
// that pointer. Any other module holding it is holding it for the same
// reason, and swapping it is what we want.
//
// WHAT WOULD DEFEAT IT: a caller that re-resolves per call, or keeps the
// pointer only in a register. Then hits=0 and we fall to the import swap,
// which is what already runs today. It cannot make anything worse.
unsigned patch_module_data(HMODULE mod, HMODULE skip, void *find, void *repl)
{
    if (mod == nullptr || mod == g_self || mod == skip || find == nullptr) return 0;

    BYTE *base = (BYTE *)mod;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;

    volatile unsigned hits = 0;
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    const unsigned nsec = nt->FileHeader.NumberOfSections;

    __try
    {
        for (unsigned i = 0; i < nsec; ++i, ++sec)
        {
            if ((sec->Characteristics & IMAGE_SCN_MEM_WRITE) == 0) continue;
            if ((sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0) continue;

            BYTE *p = base + sec->VirtualAddress;
            SIZE_T len = (SIZE_T)sec->Misc.VirtualSize;
            if (len < sizeof(void *)) continue;
            len -= sizeof(void *);

            for (SIZE_T off = 0; off <= len; off += sizeof(void *))
            {
                void **slot = (void **)(p + off);
                if (*slot != find) continue;

                DWORD old = 0;
                if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old))
                    continue;

                // R115. Record before the write, not after: `was` is the whole
                // point and it is gone one line later.
                if (g_hit_n < 16u)
                {
                    data_hit &h = g_hits[g_hit_n++];
                    h.mod = mod;
                    for (unsigned c = 0; c < 8u; ++c) h.section[c] = (char)sec->Name[c];
                    h.section[8] = '\0';
                    h.rva = (unsigned)(sec->VirtualAddress + (unsigned)off);
                    h.was = (unsigned long long)(uintptr_t)*slot;
                }

                *slot = repl;
                VirtualProtect(slot, sizeof(void *), old, &old);
                ++hits;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return (unsigned)hits;
    }
    return (unsigned)hits;
}

// The driver module, by the two names it ships under. Returns nullptr until
// something has loaded it - which is why the scan is retried rather than done
// once at install.
HMODULE ngx_module()
{
    HMODULE m = GetModuleHandleW(L"_nvngx.dll");
    if (m == nullptr) m = GetModuleHandleW(L"nvngx.dll");
    return m;
}

std::atomic<unsigned long long> g_dslots{0};

unsigned scan_cached_pointers()
{
    HMODULE ngx = ngx_module();
    if (ngx == nullptr) return 0;

    void *ev = (void *)(g_real_gpa ? g_real_gpa(ngx, NAME_EVAL)
                                   : GetProcAddress(ngx, NAME_EVAL));
    if (ev == nullptr) return 0;
    void *cr = (void *)(g_real_gpa ? g_real_gpa(ngx, NAME_CREATE)
                                   : GetProcAddress(ngx, NAME_CREATE));
    // R183. Same three routes as create, for symmetry - a release resolved by
    // a route we do not watch is a release we do not count, and an uncounted
    // release reads as a leak. NOTE FOR THE CRASH ENTRY: this adds a third
    // symbol to the scan's write surface. That is a deliberate, stated increase
    // and it belongs in STREAMLINE_LEDGER when that entry is worked.
    void *rl = (void *)(g_real_gpa ? g_real_gpa(ngx, NAME_RELEASE)
                                   : GetProcAddress(ngx, NAME_RELEASE));

    // Already ours: a previous scan took. Nothing to do.
    if (ev == (void *)&hook_evaluate) return 0;

    // ---- R121: PUBLISH THE REAL POINTERS BEFORE PATCHING A SINGLE SLOT ----
    //
    // THIS ORDERING IS THE BUG. These two assignments used to sit AFTER the
    // module walk below, in the `if (hits != 0)` block. The walk crosses 150+
    // modules and was MEASURED at 33-46 ms. For that entire window, a slot in
    // sl.common.dll already pointed at hook_create while g_real_create was
    // still null - so anything that called CreateFeature during the walk hit
    // the early-out in hook_create, got NVSDK_NGX_Result_Fail, and got its
    // out-handle left untouched. A caller that then dereferences that handle
    // reads address zero, inside sl.common.dll.
    //
    // That is the reported crash exactly: 0xC0000005, READ at 0x0, inside
    // sl.common.dll, only when the calibrator installs, identical for Calib=1
    // and Calib=2 because both install the same way.
    //
    // WHY IT IS ONE-IN-TEN ON ONE RIG AND EVERY TIME ON ANOTHER. It is a race
    // against a 33 ms window. A machine that calls CreateFeature inside that
    // window crashes every launch; one that calls it afterwards never does.
    // Nothing about the machines needs to differ except when the title gets
    // round to creating its feature - which is why this looked like a
    // poison, a residue and a timing quirk in turn.
    //
    // The fix is free: the addresses are already resolved above. Publish them
    // first and the window does not exist. Guarded so a rescan cannot
    // overwrite a good pointer with a stale one.
    if (g_real_eval == nullptr) g_real_eval = (pf_evaluate)ev;
    if (cr != nullptr && g_real_create == nullptr) g_real_create = (pf_create)cr;
    if (rl != nullptr && g_real_release == nullptr) g_real_release = (pf_release)rl;   // R183

    unsigned hits = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return 0;

    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    if (Module32FirstW(snap, &me))
    {
        do
        {
            // ---- R112: NAME THE MODULE WE WROTE INTO ----
            //
            // WHY THIS LINE EXISTS. This rung writes eight-byte words into
            // OTHER modules' data sections. Until now the log said how many
            // words - data-slots=2 on Battlefield 6, both runs - and never
            // said WHOSE. That is the one question the reporter's crash
            // actually turns on: sl.common.dll+0x611AA is where it dies, and
            // whether this rung wrote into sl.common.dll is a fact we have
            // been in a position to state all along and did not.
            //
            // Costs nothing when nothing is patched, which is every module
            // but one or two.
            const unsigned before = hits;
            hits += patch_module_data(me.hModule, ngx, ev, (void *)&hook_evaluate);
            if (cr != nullptr)
                hits += patch_module_data(me.hModule, ngx, cr, (void *)&hook_create);
            if (rl != nullptr)
                hits += patch_module_data(me.hModule, ngx, rl, (void *)&hook_release);   // R183
            if (hits != before)
            {
                char ml[420];
                std::snprintf(ml, sizeof ml,
                    "[MGPU][R112] DATA-SCAN WROTE INTO \"%ls\" - %u word(s) swapped in that "
                    "module's writable data. This is the rung that reaches a pointer cached "
                    "before we loaded, and this line names the module it reached into. If a "
                    "title faults inside a module listed here, the two facts are finally "
                    "side by side instead of being inferred from a slot count.",
                    me.szModule, hits - before);
                mgpu::diag::warn(ml);
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);

    if (hits != 0)
    {
        // R121. The two assignments that used to be here have moved ABOVE the
        // walk. Leaving them here as well would be harmless but would leave
        // two places that look like they establish the same fact, and the
        // whole defect was that this one ran too late.
        g_dslots.fetch_add(hits, std::memory_order_relaxed);
        g_resolved.fetch_add(1, std::memory_order_relaxed);
    }
    return hits;
}

// ---- R115: THE REFERENCE PROBE ----
//
// THE QUESTION. R102 writes 8-byte words into other modules' writable data,
// on the claim that a word equal to the address of one NGX export can only be
// a cached pointer to it. On Battlefield 6 it writes two of them into
// sl.common.dll - the module the reporter's game faults inside - and the
// calibrator has reported evaluates=0 on every run since. Either those words
// are a cache nobody calls, or they are not a cache at all.
//
// MODE 1: THE ON-DISK REFERENCE. No load, no execution, no second module.
// Open the module's own file, convert the RVA we wrote to a file offset
// through the section headers, and read the 8 bytes that live there on disk.
//
//   pristine 0 or a small relocation-shaped value, live = an nvngx address
//       -> it IS a runtime cache, written at slInit. The rung is doing what
//          it claims and the only open question is whether writing it is safe.
//   pristine already equal to the live value
//       -> it is NOT a runtime cache. We are writing into something static
//          that merely matched, inside the module that crashes.
//
// MODE 2: THE PRIVATE COPY, which is his test. Windows keys module identity by
// resolved PATH, so a copy of sl.common.dll under mgpu\ loads as a SECOND
// module with its own data - the same trick the private nvngx_dlssnr.dll
// already uses. We load it, read the same RVA, write a sentinel there, read it
// back and put it back.
//
// WHAT MODE 2 CAN AND CANNOT SETTLE, stated so the result is not over-read.
// It settles: that the copy loads, that the RVA is where we think it is, and
// that VirtualProtect plus an 8-byte store works there. It does NOT settle
// whether writing the LIVE copy is safe, because nothing is executing inside a
// module nobody calls - and the hazard, if there is one, is a pointer being
// swapped while Streamline is mid-call through it, not the store itself. An
// aligned 8-byte store is atomic on x64, so there is no torn pointer to find.
//
// Mode 2 also runs sl.common's DllMain a second time in this process. That is
// why it is not the default. On our own rig it is a run; nothing ships.
bool rva_to_file_offset(HMODULE mod, unsigned rva, unsigned &out)
{
    if (mod == nullptr) return false;
    BYTE *base = (BYTE *)mod;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
    {
        const unsigned va = sec->VirtualAddress;
        const unsigned vz = (unsigned)sec->Misc.VirtualSize;
        if (rva >= va && rva < va + vz)
        {
            out = sec->PointerToRawData + (rva - va);
            return true;
        }
    }
    return false;
}

bool read_file_qword(const wchar_t *path, unsigned off, unsigned long long &out)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER li{};
    li.QuadPart = (LONGLONG)off;
    bool ok = false;
    if (SetFilePointerEx(f, li, nullptr, FILE_BEGIN))
    {
        DWORD got = 0;
        unsigned long long v = 0;
        if (ReadFile(f, &v, 8, &got, nullptr) && got == 8) { out = v; ok = true; }
    }
    CloseHandle(f);
    return ok;
}

// <folder of the running exe>\mgpu\<leaf of src>
bool private_copy_path(const wchar_t *src, wchar_t *out, size_t out_n)
{
    wchar_t exe[MAX_PATH * 2] = {};
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH * 2) == 0) return false;
    wchar_t *slash = wcsrchr(exe, L'\\');
    if (slash == nullptr) return false;
    *slash = L'\0';
    const wchar_t *leaf = wcsrchr(src, L'\\');
    leaf = (leaf != nullptr) ? leaf + 1 : src;
    _snwprintf_s(out, out_n, _TRUNCATE, L"%s\\mgpu\\%s", exe, leaf);
    return true;
}

void report_hits(int probe_mode)
{
    if (g_hit_n == 0u) return;

    for (unsigned i = 0; i < g_hit_n; ++i)
    {
        const data_hit &h = g_hits[i];

        wchar_t path[MAX_PATH * 2] = {};
        GetModuleFileNameW(h.mod, path, MAX_PATH * 2);
        const wchar_t *leaf = wcsrchr(path, L'\\');
        leaf = (leaf != nullptr) ? leaf + 1 : path;

        char line[900];
        std::snprintf(line, sizeof line,
            "[MGPU][R115] DATA-SCAN HIT %u/%u: \"%ls\" section %s +0x%X (module base +0x%X) | "
            "replaced 0x%llx with our hook. THIS IS THE WORD ITSELF, which the log has never "
            "named. A writable data section holding one NGX export address is what R102 "
            "assumes is a cached pointer; that assumption has never been checked and this "
            "line is the first half of checking it.",
            i + 1u, g_hit_n, leaf, h.section, h.rva, h.rva, h.was);
        mgpu::diag::warn(line);

        if (probe_mode < 1) continue;

        unsigned foff = 0;
        unsigned long long disk = 0;
        if (!rva_to_file_offset(h.mod, h.rva, foff))
        {
            mgpu::diag::warn("[MGPU][R115] the RVA does not fall inside any section header - "
                             "no on-disk reference for this hit.");
            continue;
        }
        if (!read_file_qword(path, foff, disk))
        {
            mgpu::diag::warn("[MGPU][R115] could not read the module's own file for the "
                             "on-disk reference. Nothing else is affected.");
            continue;
        }

        const bool looks_cached = (disk != h.was);
        std::snprintf(line, sizeof line,
            "[MGPU][R115] ON-DISK REFERENCE for hit %u: file offset 0x%X holds 0x%llx, the "
            "live image held 0x%llx. VERDICT: %s. HOW TO READ IT. Different means the word is "
            "written at RUN TIME - a cache filled after load, which is what R102 claims it is, "
            "and the open question narrows to whether swapping it is safe. THE SAME means it "
            "is NOT a runtime cache: it is static data that merely equalled the address we "
            "searched for, and R102 has been writing into something it does not understand, "
            "inside the module that crashes. No load, no execution and no second module was "
            "involved in producing this line.",
            i + 1u, foff, disk, h.was,
            looks_cached ? "RUNTIME-WRITTEN, consistent with a cached pointer"
                         : "IDENTICAL ON DISK - NOT a runtime cache");
        if (looks_cached) mgpu::diag::info(line);
        else              mgpu::diag::error(line);

        if (probe_mode < 2) continue;

        // ---- MODE 2: the private copy ----
        wchar_t priv[MAX_PATH * 2] = {};
        if (!private_copy_path(path, priv, MAX_PATH * 2)) continue;

        if (GetFileAttributesW(priv) == INVALID_FILE_ATTRIBUTES)
        {
            if (!CopyFileW(path, priv, TRUE))
            {
                std::snprintf(line, sizeof line,
                    "[MGPU][R115] PRIVATE COPY: could not create \"%ls\" (err %lu). The mgpu "
                    "folder beside the exe has to exist and be writable. Skipped.",
                    priv, GetLastError());
                mgpu::diag::warn(line);
                continue;
            }
        }

        std::snprintf(line, sizeof line,
            "[MGPU][R115] PRIVATE COPY step 1/3: loading \"%ls\" as a SECOND module. Windows "
            "keys module identity by resolved path, so this gets its own data. IT ALSO RUNS "
            "THAT MODULE'S DllMain A SECOND TIME IN THIS PROCESS - that is the cost of this "
            "test and the reason it is not the default. IF THIS IS THE LAST R115 LINE, IT DIED "
            "HERE.", priv);
        mgpu::diag::warn(line);

        HMODULE pm = LoadLibraryW(priv);
        if (pm == nullptr)
        {
            std::snprintf(line, sizeof line,
                "[MGPU][R115] PRIVATE COPY: LoadLibrary failed (err %lu). That is a result: the "
                "module will not load standalone, so routing anything through a copy of it is "
                "not available to us.", GetLastError());
            mgpu::diag::error(line);
            continue;
        }

        mgpu::diag::info("[MGPU][R115] PRIVATE COPY step 2/3: reading and writing the SAME RVA "
                         "in the private image. IF THIS IS THE LAST R115 LINE, IT DIED HERE.");

        unsigned long long before = 0, after = 0;
        bool wrote = false;
        void **slot = (void **)((BYTE *)pm + h.rva);
        __try
        {
            before = (unsigned long long)(uintptr_t)*slot;
            DWORD old = 0;
            if (VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old))
            {
                *slot = (void *)(uintptr_t)0xD1A6D1A6D1A6D1A6ull;
                after = (unsigned long long)(uintptr_t)*slot;
                *slot = (void *)(uintptr_t)before;
                VirtualProtect(slot, sizeof(void *), old, &old);
                wrote = true;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { wrote = false; }

        std::snprintf(line, sizeof line,
            "[MGPU][R115] PRIVATE COPY step 3/3 DONE: base 0x%p, same RVA +0x%X held 0x%llx, "
            "sentinel write %s (read back 0x%llx), original restored. WHAT THIS SETTLES: the "
            "copy loads, the RVA is where we think it is, and the store works there. WHAT IT "
            "DOES NOT SETTLE: whether writing the LIVE copy is safe - nothing executes inside "
            "a module nobody calls, and an aligned 8-byte store is atomic on x64, so the store "
            "was never the suspect. The suspect is swapping a pointer while Streamline is "
            "mid-call through it, and only the live module can answer that.",
            (void *)pm, h.rva, before, wrote ? "SUCCEEDED" : "FAILED", after);
        mgpu::diag::warn(line);

        FreeLibrary(pm);
        mgpu::diag::info("[MGPU][R115] PRIVATE COPY unloaded.");
    }
}

// The reverse, for detach. Same walk, swapped arguments.
void unscan_cached_pointers()
{
    HMODULE ngx = ngx_module();
    if (ngx == nullptr || g_real_eval == nullptr) return;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return;
    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    if (Module32FirstW(snap, &me))
    {
        do
        {
            patch_module_data(me.hModule, ngx, (void *)&hook_evaluate,
                              (void *)g_real_eval);
            if (g_real_create != nullptr)
                patch_module_data(me.hModule, ngx, (void *)&hook_create,
                                  (void *)g_real_create);
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
}

std::mutex g_install_cs;

} // namespace

// ---------------------------------------------------------------------------

// [FFX, 2026-11-11] The writer for a block that came from an API other than NGX. OUTSIDE the
// anonymous namespace on purpose: ffx_hook.cpp links to this symbol. Identical seqlock as
// capture(): one writer at a time by construction, because a title upscales through DLSS or
// through FSR, never both in the same frame. g_captures is incremented so read() answers.
// [FFX, 2026-11-11] One increment per external (non-NGX) contract publish. See the header: the
// history gate needs to know a provider handed this frame over even when no DLSS evaluate exists.
static std::atomic<unsigned long long> g_contract_frames{0};

unsigned long long contract_frame_counter()
{
    return g_contract_frames.load(std::memory_order_relaxed);
}

void publish_external(table t)
{
    g_contract_frames.fetch_add(1ull, std::memory_order_relaxed);
    t.frame = g_frames.load(std::memory_order_relaxed);
    t.evals = g_evals.load(std::memory_order_relaxed);

    g_seq.fetch_add(1u, std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_release);
    g_tbl = t;
    std::atomic_thread_fence(std::memory_order_release);
    g_seq.fetch_add(1u, std::memory_order_release);

    g_captures.fetch_add(1, std::memory_order_relaxed);
}

// R245: public, outside the anonymous namespace (the hook and the walker it
// calls are inside it, declared above).
void set_trace_nvapi(bool on)
{
    const int was = g_trace_nvapi.exchange(on ? 1 : 0, std::memory_order_acq_rel);
    if (on && was == 0)
        mgpu::diag::warn("[MGPU][R245] nvapi trace ON (TraceNvapi=1): a debug instrument, not a setting - "
                         "every nvapi_QueryInterface and every GetProcAddress by the NGX core or the NR snippet "
                         "is logged. Set TraceNvapi=0 when the question is answered.");
}

unsigned rescan_now()
{
    if (g_trace_nvapi.load(std::memory_order_relaxed) == 0) return 0;
    if (g_real_gpa == nullptr) return 0;
    const int rung = g_rung.load(std::memory_order_relaxed);
    if (rung != 0 && rung != 1) return 0;
    const unsigned hits = scan_and_patch((void *)g_real_gpa, (void *)&hook_gpa);
    if (hits != 0) g_slots.fetch_add(hits, std::memory_order_relaxed);
    return hits;
}


std::atomic<int> g_jmode{0};
float g_jpx = 0.0f, g_jpy = 0.0f;
bool  g_jprobed = false;

void set_mvec_hook(void (*fn)(void *, unsigned long long))
{
    g_mvec_hook_fn = fn;
}

// R232.
void set_mvec_hook_d3d11(void (*fn)(void *, unsigned long long))
{
    g_mvec_hook11_fn = fn;
}

// R118. The getter, so gpu1_context can ask what the user set WITHOUT
// reading mgpu.ini a second time. The ini is parsed in exactly one place and
// that rule is worth more than the three lines this saves.
// R239. For gpu1_context's own NGX inits: is one of the game's NGX inits
// inside the core right now (entered and not yet returned)?
bool game_ngx_init_in_flight()
{
    return g_game_init_entered.load(std::memory_order_acquire) >
           g_game_init_returned.load(std::memory_order_acquire);
}
bool game_ngx_init_seen()
{
    return g_game_init_entered.load(std::memory_order_acquire) != 0ull;
}

int eval_copy_mode()
{
    return g_eval_copy_mode.load(std::memory_order_relaxed);
}

unsigned long long eval_copies()   // R280o
{
    return g_eval_copies.load(std::memory_order_relaxed);
}

unsigned long long scene_captures()   // R281
{
    return g_captures.load(std::memory_order_relaxed);
}

bool installed()   // R281
{
    return g_installed.load(std::memory_order_relaxed);
}

bool game_ngx()   // R283
{
    return g_resolved.load(std::memory_order_relaxed) != 0ull;
}

void eval_threads(unsigned long *first, unsigned long *last, unsigned *changes)   // R283
{
    *last    = g_eval_tid_last.load(std::memory_order_acquire);
    *first   = g_eval_tid_first.load(std::memory_order_relaxed);
    *changes = g_eval_tid_changes.load(std::memory_order_relaxed);
}

void set_eval_copy(int mode)
{
    g_eval_copy_mode.store(mode, std::memory_order_relaxed);
}

bool g_scale_said = false;

void override_scale(float &sx, float &sy)
{
    if (g_mode.load(std::memory_order_relaxed) == 0) return;

    table t;
    if (!read(t)) return;
    if ((t.have & KEY_MV_SCALE) == 0u) return;
    if (t.mv_scale_x == 0.0f || t.mv_scale_y == 0.0f) return;

    if (!g_scale_said)
    {
        g_scale_said = true;
        char sl[420];
        std::snprintf(sl, sizeof sl,
            "[MGPU][R105] MVecScale NOW READ, NOT TYPED: ini said %.4f,%.4f - the game "
            "says %.4f,%.4f (MVLowRes=%u). If those differ, the ini was wrong and this "
            "is the fix; the ini value is still what gets used on any title where the "
            "calibrator does not resolve.",
            (double)sx, (double)sy, (double)t.mv_scale_x, (double)t.mv_scale_y,
            t.mv_low_res);
        mgpu::diag::info(sl);
    }

    sx = t.mv_scale_x;
    sy = t.mv_scale_y;
}

// T1.19 (Dom, T1.11 audit; JitterFallback=sync|zero only): bring R104's previous-jitter state up to date
// when the slot path applied the offset for this evaluate instead.
void sync_jitter_state()
{
    table t;
    if (!read(t)) return;
    if ((t.have & KEY_JITTER) == 0u || t.mv_jittered != 0u) return;   // same gates as apply_jitter_offset
    g_jpx = t.jitter_x;
    g_jpy = t.jitter_y;
}

unsigned long long feature_creates()   { return g_creates.load(std::memory_order_relaxed); }
unsigned long long feature_releases()  { return g_releases.load(std::memory_order_relaxed); }
unsigned long long feature_evaluates() { return g_evals.load(std::memory_order_relaxed); }   // T1.16: ALL handles

void set_jitter_mode(int mode)
{
    g_jmode.store(mode, std::memory_order_relaxed);
}

// ---- R180 ----
void set_sf_path(int mode)
{
    g_sf_path.store(mode, std::memory_order_relaxed);
}

unsigned long long latch_refused()
{
    return g_latch_refused.load(std::memory_order_relaxed);
}

bool sf_path_should_promote()
{
    // Only from ABSENT. An explicit 0 is the operator's answer and is not
    // second-guessed by a heuristic; an explicit 1 has nothing to promote.
    if (g_sf_path.load(std::memory_order_relaxed) != -2) return false;
    if (g_latch_refused.load(std::memory_order_relaxed) == 0ull) return false;
    bool expected = false;
    // Once per process, whoever asks first.
    return g_sf_promoted.compare_exchange_strong(expected, true,
                                                 std::memory_order_relaxed);
}

void apply_jitter_offset(void *nr_params, float sx, float sy)
{
    const int m = g_jmode.load(std::memory_order_relaxed);
    if (m == 0 || nr_params == nullptr) return;

    table t;
    if (!read(t)) return;
    if ((t.have & KEY_JITTER) == 0u) return;
    if (t.mv_jittered != 0u) return;      // the game already baked it in

    // THE DELTA, not the absolute. A motion vector describes frame N-1 -> N
    // and the two frames were jittered differently, so the correction is the
    // difference between them. That part is settled by reasoning; only the
    // SIGN is left for the rig, which is what InvertJitter is for.
    const float g  = (m < 0) ? -1.0f : 1.0f;
    const float ax = (sx != 0.0f) ? sx : 1.0f;
    const float ay = (sy != 0.0f) ? sy : 1.0f;
    const float ox = g * (t.jitter_x - g_jpx) / ax;
    const float oy = g * (t.jitter_y - g_jpy) / ay;
    g_jpx = t.jitter_x;
    g_jpy = t.jitter_y;

    NVSDK_NGX_Parameter *p = (NVSDK_NGX_Parameter *)nr_params;

    // Two spellings: DLSS-NR's private set is undocumented, MV.Offset.X/Y is
    // NGX's generic name. Setting a key the snippet does not know is inert.
    p->Set("DLSSNR.MVecOffsetX", ox);
    p->Set("DLSSNR.MVecOffsetY", oy);
    p->Set("MV.Offset.X", ox);
    p->Set("MV.Offset.Y", oy);

    if (!g_jprobed)
    {
        g_jprobed = true;
        float a = 0.0f, b = 0.0f;
        const bool k1 = ok(p->Get("DLSSNR.MVecOffsetX", &a));
        const bool k2 = ok(p->Get("MV.Offset.X", &b));
        char jl[520];
        std::snprintf(jl, sizeof jl,
            "[MGPU][R104] JITTER COMP mode=%d | DLSSNR.MVecOffsetX readback=%d (%.6f) | "
            "MV.Offset.X readback=%d (%.6f) | first delta %.6f,%.6f in VECTOR units "
            "(MVecScale %.1f,%.1f, raw jitter %.4f,%.4f). BOTH readbacks 0 means DLSS-NR "
            "has no motion vector offset parameter and this needs a compute pass on the "
            "MVec copy - that is the finding, not a failure. Either one non-zero means the "
            "fix is LIVE and only the sign is open: if the skin looks worse, set "
            "InvertJitter=1 and run again. No rebuild for that.",
            m, (int)k1, (double)a, (int)k2, (double)b, (double)ox, (double)oy,
            (double)ax, (double)ay, (double)t.jitter_x, (double)t.jitter_y);
        mgpu::diag::info(jl);
    }
}

// ---- R110: INSTALL, SAID OUT LOUD, ONE RUNG AT A TIME ----
//
// WHAT THIS ROUND CHANGES AND WHY.
//
// Reported on a title that dies 3 ms after CALIBRATOR INSTALLED, inside
// sl.common.dll. The reporter ran Calib=1 and Calib=2 and got the same
// crash at the same offset, which was read at the time as the mode not
// being the variable. It is stronger than that: the two modes install
// IDENTICALLY. Every line below the mode store ran the same way for
// both, because mode has never had any part in install - it is read at
// evaluate time and nowhere else. So the pair of runs did not narrow
// anything, and could not have.
//
// Two rungs run here, in sequence and unconditionally: the import-table
// swap (R101) and the data-section scan for pointers cached before we
// existed (R102). One of them is what the engine does not like, and the
// log could not name which, for two reasons that are both fixed below:
//
//   1. Nothing was said BEFORE either rung, only after both. A process
//      that dies inside rung A and a process that dies inside rung B
//      leave the identical log - the INSTALLED line absent in both.
//      This is the C0 rule the arm path already follows: announce the
//      step, then take it, so the last line printed is the step that
//      killed it.
//   2. There was no way to run one rung alone. CalibRung= is that way.
//      Two runs, one key, and the answer is which one survives.
//
// Also fixed here: the INSTALLED line reported the IAT hit count only.
// On this title INSTALLED is the LAST calibrator line that will ever
// print - site= and data-slots= live on the periodic line, which needs
// 300 frames and never arrives. The one line that survives now carries
// every field needed to reconstruct what was touched.
//
// g_dslots.fetch_add(0) sat here and was read as a defect. It is not
// one: scan_cached_pointers() adds its own hits at the point it takes
// them, so the counter was always right and this line did nothing at
// all. Removed as dead, not as a fix.
//
// The argument is PACKED - low byte Calib, bits 8-11 CalibRung - so
// that dllmain's single install() line is untouched by this round. See
// probe.hpp.
void install(int packed)
{
    const int mode  = packed & 0xFF;
    const int rung  = (packed >> 8) & 0xF;
    // R115. CalibProbe, bits 12-15. 0 off, 1 on-disk reference only,
    // 2 also load a private copy under mgpu\ and write to it.
    const int probe = (packed >> 12) & 0xF;

    if (mode == 0) return;                       // nothing touched at all
    std::lock_guard<std::mutex> lk(g_install_cs);
    if (g_installed.load(std::memory_order_relaxed)) return;

    qpc_init();
    g_mode.store(mode, std::memory_order_relaxed);
    g_rung.store(rung, std::memory_order_relaxed);

    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&install, &g_self);

    {
        char pre[760];
        std::snprintf(pre, sizeof pre,
            "[MGPU][R110] CALIB INSTALL BEGIN mode=%d rung=%d (%s). Nothing has "
            "been touched yet. If this is the last calibrator line in the log, "
            "the process died in the step named by the next BEGIN line that is "
            "missing - read the two step lines below it, not this one. rung=0 "
            "both, 1 import swap alone, 2 data scan alone; set CalibRung= in "
            "mgpu.ini to run one at a time. CalibProbe=%d (0 off, 1 on-disk reference for "
            "every data-scan hit, 2 also loads a private copy under mgpu\\ and writes to it).",
            mode, rung,
            (rung == 1) ? "import swap alone"
                        : ((rung == 2) ? "data scan alone" : "both"),
            probe);
        mgpu::diag::info(pre);
    }

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    g_real_gpa = (pf_gpa)GetProcAddress(k32, "GetProcAddress");
    if (g_real_gpa == nullptr)
    {
        mgpu::diag::warn("[MGPU][R110] CALIB INSTALL ABORTED: GetProcAddress "
                         "could not be resolved from kernel32. Nothing was "
                         "patched and the calibrator is not installed.");
        return;
    }

    // ---- RUNG A: the import-table swap ----
    unsigned hits = 0;
    if (rung == 0 || rung == 1)
    {
        mgpu::diag::info(
            "[MGPU][R110] CALIB step 1/2 BEGIN: import-table swap. Walking every "
            "module in the process except our own and replacing import slots "
            "whose value is kernel32!GetProcAddress. Pointer stores only - no "
            "instruction byte is modified anywhere.");
        hits = scan_and_patch((void *)g_real_gpa, (void *)&hook_gpa);
        g_slots.fetch_add(hits, std::memory_order_relaxed);
        char sl[256];
        std::snprintf(sl, sizeof sl,
            "[MGPU][R110] CALIB step 1/2 DONE: %u slot(s) across %llu module(s).",
            hits, g_modules.load(std::memory_order_relaxed));
        mgpu::diag::info(sl);
    }
    else
    {
        mgpu::diag::info("[MGPU][R110] CALIB step 1/2 SKIPPED by CalibRung=2.");
    }

    // ---- RUNG B: R102, the cached-pointer scan ----
    // If a caller cached the pointer before we existed - Streamline always
    // does - the import walk found nothing to reach it with, and this does.
    unsigned dh = 0;
    if (rung == 0 || rung == 2)
    {
        mgpu::diag::info(
            "[MGPU][R110] CALIB step 2/2 BEGIN: R102 data-section scan. Reading "
            "the writable data sections of every loaded module looking for the "
            "8-byte word that equals the real NGX entry point. This is the rung "
            "that reaches Streamline, and it is the one that touches memory "
            "belonging to modules we did not load.");
        dh = scan_cached_pointers();
        char dl[256];
        std::snprintf(dl, sizeof dl,
            "[MGPU][R110] CALIB step 2/2 DONE: %u cached pointer(s) swapped.", dh);
        mgpu::diag::info(dl);
    }
    else
    {
        mgpu::diag::info("[MGPU][R110] CALIB step 2/2 SKIPPED by CalibRung=1.");
    }

    // R115. After both rungs and before the INSTALLED line, so the words the
    // data scan wrote are named next to the count of them. Says nothing when
    // nothing was written, and nothing at all when CalibProbe is 0 beyond the
    // hit lines themselves.
    report_hits(probe);

    g_site = (dh != 0) ? "data-scan"
                       : ((hits != 0) ? "iat"
                                      : ((rung == 2) ? "data-nohits" : "iat-nohits"));
    g_installed.store(true, std::memory_order_relaxed);

    char line[700];
    std::snprintf(line, sizeof line,
        "[MGPU][R101] CALIBRATOR INSTALLED mode=%d rung=%d site=%s | slots=%u "
        "data-slots=%u modules=%llu | %u import slot(s) patched across %llu "
        "module(s). This is a POINTER SWAP, not a code patch - no "
        "instruction anywhere in this process was modified, and Calib=0 puts "
        "every slot back. Our own module is skipped so the bridge's own "
        "DLSS-NR evaluates are never mistaken for the game's.",
        mode, rung, g_site, hits, dh,
        g_modules.load(std::memory_order_relaxed),
        hits, g_modules.load(std::memory_order_relaxed));
    mgpu::diag::info(line);
}

void uninstall()
{
    std::lock_guard<std::mutex> lk(g_install_cs);
    if (!g_installed.load(std::memory_order_relaxed)) return;

    unscan_cached_pointers();
    scan_and_patch((void *)&hook_gpa, (void *)g_real_gpa);
    g_mode.store(0, std::memory_order_relaxed);
    g_installed.store(false, std::memory_order_relaxed);
    // R183. The gpa restore above covers the dispatch route for all three
    // symbols. Data-section slots are NOT restored here and were not before
    // this change either - that gap belongs to the crash entry, not to this
    // one, and it is named rather than quietly inherited.
    mgpu::diag::info("[MGPU][R101] CALIBRATOR REMOVED. Import slots restored.");
}

bool read(table &out)
{
    if (g_captures.load(std::memory_order_relaxed) == 0) return false;

    // Bounded, never blocking. Four tries is far more than a writer that only
    // copies a POD struct will ever need, and giving up costs one frame of
    // staleness rather than a wait on the frame path.
    for (int i = 0; i < 4; ++i)
    {
        const unsigned s1 = g_seq.load(std::memory_order_acquire);
        if ((s1 & 1u) != 0u) continue;
        std::atomic_thread_fence(std::memory_order_acquire);
        out = g_tbl;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (g_seq.load(std::memory_order_acquire) == s1) return true;
    }
    return false;
}

void note_frame()
{
    const unsigned long long f =
        g_frames.fetch_add(1, std::memory_order_relaxed) + 1;
    if (!g_installed.load(std::memory_order_relaxed)) return;

    // The periodic report, on the SAME 300-frame cadence as the MVEC
    // copies/missing line, so the two can be read side by side without
    // interpolating between different clocks - which is exactly what reading
    // the Plague Tale run cost.
    if ((f % 300ull) == 0ull) log_summary();

    // A module that loaded after us has an unpatched import table. Rather than
    // hooking LoadLibrary - another interception, another thing to unwind -
    // rescan on a slow cadence until NGX actually resolves, then stop. Thirty
    // snapshots over the first half minute, then nothing.
    if (g_resolved.load(std::memory_order_relaxed) != 0) return;
    if ((f % 60ull) != 0ull || f > 1800ull) return;

    // R110. The rescan runs the SAME rungs install was allowed to run. A
    // CalibRung that excluded a rung at install and then let it back in
    // sixty frames later would make the key a delay rather than a
    // selector, and the run would prove nothing.
    const int rung = g_rung.load(std::memory_order_relaxed);

    if (rung == 0 || rung == 1)
    {
        const unsigned hits = scan_and_patch((void *)g_real_gpa, (void *)&hook_gpa);
        if (hits != 0) g_slots.fetch_add(hits, std::memory_order_relaxed);
    }

    // R102. Retried, not done once: nvngx may not be loaded yet at install,
    // and there is nothing to compare against until it is. Stops the moment
    // anything resolves, by the guard at the top of this function.
    if (rung == 0 || rung == 2)
    {
        if (scan_cached_pointers() != 0) g_site = "data-scan";
    }
}

void log_summary()
{
    table t{};
    const bool have = read(t);

    const unsigned long long f  = g_frames.load(std::memory_order_relaxed);
    const unsigned long long ns = g_cost_ns.load(std::memory_order_relaxed);
    const double per_frame = (f != 0) ? (double)ns / (double)f : 0.0;

    // R117. Widened: the rewritten HOW TO READ IT is longer than the old
    // one, and a truncated explanation on the line that already misled us
    // once would be the same mistake with fewer characters.
    char line[5600];
    int w = std::snprintf(line, sizeof line,
        "[MGPU][R101] CALIBRATOR mode=%d rung=%d site=%s | slots=%llu data-slots=%llu modules=%llu gpa-calls=%llu "
        "| resolved=%llu creates=%llu releases=%llu live=%lld sr-handle=%s "
        "| latch: freed=%llu reused=%llu REFUSED=%llu "
        "| evaluates=%llu captured=%llu "
        "| cost %.0f ns/frame | eval-copies=%llu eval-skips=%llu. "
        "R183: LIVE IS CREATES MINUS RELEASES and it is the field to read. A live "
        "count that climbs without bound means the title is not releasing what it "
        "recreates - a leak on the GAME's adapter, which is the same adapter our "
        "ring heap sits on. A live count that stays at one or two while creates "
        "climbs means recreation is matched and the pressure is somewhere else. A "
        "NEGATIVE live means we are missing creates rather than that releases "
        "exceeded them: some route resolved ReleaseFeature past our hooks, and the "
        "number is then a floor, not a count. R180: REFUSED is the one that must stay at zero. It counts scene features the latch set had no room for, and every one of them is a feature whose evaluates are skipped for the rest of the run. Four slots, freed on release since 0.2.5-dev; before that the set was append-only and a fifth create killed the velocity lane permanently. ",
        g_mode.load(std::memory_order_relaxed),
        g_rung.load(std::memory_order_relaxed), g_site,
        g_slots.load(std::memory_order_relaxed),
        g_dslots.load(std::memory_order_relaxed),
        g_modules.load(std::memory_order_relaxed),
        g_gpa_calls.load(std::memory_order_relaxed),
        g_resolved.load(std::memory_order_relaxed),
        g_creates.load(std::memory_order_relaxed),
        g_releases.load(std::memory_order_relaxed),
        (long long)g_creates.load(std::memory_order_relaxed) -
            (long long)g_releases.load(std::memory_order_relaxed),
        g_handle_known.load(std::memory_order_relaxed) ? "known" : "UNFILTERED",
        g_unlatches.load(std::memory_order_relaxed),
        g_relatches.load(std::memory_order_relaxed),
        g_latch_refused.load(std::memory_order_relaxed),
        g_evals.load(std::memory_order_relaxed),
        g_captures.load(std::memory_order_relaxed),
        per_frame, g_eval_copies.load(std::memory_order_relaxed),
        g_eval_skips.load(std::memory_order_relaxed));

    if (have && w > 0 && w < (int)sizeof line)
    {
        w += std::snprintf(line + w, sizeof line - (size_t)w,
            "|| THE GAME'S OWN TABLE: color=0x%llx depth=0x%llx MVEC=0x%llx "
            "output=0x%llx | MVecScale %.4f,%.4f | jitter %.4f,%.4f | Reset=%u "
            "| CreateFlags=0x%x -> DepthInverted=%u MVLowRes=%u MVJittered=%u "
            "IsHDR=%u AutoExposure=%u | quality=%u | NGX Width/Height %ux%u, "
            "OutWidth/OutHeight %ux%u | dynamic %ux%u..%ux%u | subrect %ux%u at color(%u,%u) "
            "depth(%u,%u) MV(%u,%u) out(%u,%u) | have=0x%x. ",
            t.color, t.depth, t.mvec, t.output,
            (double)t.mv_scale_x, (double)t.mv_scale_y,
            (double)t.jitter_x, (double)t.jitter_y, t.reset,
            t.create_flags, t.depth_inverted, t.mv_low_res, t.mv_jittered,
            t.is_hdr, t.auto_exposure, t.perf_quality,
            t.render_w, t.render_h, t.display_w, t.display_h,
            t.dyn_min_w, t.dyn_min_h, t.dyn_max_w, t.dyn_max_h,
            t.sub_w, t.sub_h, t.color_x, t.color_y, t.depth_x, t.depth_y,
            t.mvec_x, t.mvec_y, t.out_x, t.out_y, t.have);
    }

    if (w > 0 && w < (int)sizeof line)
    {
        std::snprintf(line + w, sizeof line - (size_t)w,
            "|| HOW TO READ IT, REWRITTEN 2026-09-14 AFTER THIS LINE MISLED US "
            "FOR FOUR RUNS. evaluates=0 creates=0 DOES NOT MEAN THE HOOK "
            "FAILED. The first thing to check is whether the title has "
            "upscaling switched ON: with it off, the game never creates a "
            "DLSS feature, never evaluates, and a perfectly working hook "
            "reports exactly these zeros. creates=0 is the tell - an idle "
            "DLSS still creates its feature, an absent one does not. Only "
            "after confirming the setting is on does resolved=0 mean what it "
            "used to say here: nobody asked for the entry point after we "
            "installed, either because it resolved before the addon loaded or "
            "because this producer reaches NGX another way. "
            "sr-handle=UNFILTERED means we were not present for CreateFeature, "
            "so the table may be from frame generation rather than super "
            "sampling - check the extents and the subrect against each other "
            "before trusting it. THE TWO EXTENT PAIRS ARE PRINTED BY THEIR NGX "
            "KEY NAMES ON PURPOSE: this line used to label them render and "
            "display and got the pair the wrong way round on Battlefield 6, "
            "where Width/Height read 1920x1080 while the subrect, MVecScale "
            "and every candidate said the render extent was 1280x720. Trust "
            "the subrect and MVecScale over either pair until that is "
            "understood. MVEC here is the GAME'S OWN answer, and R103 hands it "
            "straight to the transport - so TRANSPORT SOURCE on the R71 line "
            "should equal it, and on every run measured so far it does. THAT "
            "AGREEMENT IS NOT THE SAME AS THE TRANSPORT WORKING: measured "
            "2026-09-14, the source was correct and copies were still ZERO "
            "with MvecFromEval=0, because the barrier trigger never fired on "
            "this engine. The address and the moment are different problems "
            "and this line only settles the address. have=0 bits name the keys "
            "this producer does not populate - Streamline and the raw SDK do "
            "not set the same subset, and the difference is a fact about the "
            "route, not a bug. MVLowRes: 1 means the game's motion vectors are "
            "at RENDER resolution, 0 means display resolution. Read it "
            "together with MVecScale - a low-res field with a display-sized "
            "scale is the shape that makes reprojection look almost right.");
    }
    mgpu::diag::info(line);

    if (mgpu::t119::scene_handle_set())
    {   // R300b: who evaluates, who is taken, who is copied. The line that says whether coverage is fixed.
        char hl[900]; int hw = std::snprintf(hl, sizeof hl,
            "[MGPU][R300b] SCENE HANDLES SceneHandleSet=1 (evals = every NGX evaluate seen; taken = passed the filter; "
            "copies = a vector was recorded):");
        for (int i = 0; i < 4 && hw > 0 && (size_t)hw < sizeof hl; ++i)
        {
            const unsigned long long hv = g_hs[i].h.load(std::memory_order_relaxed);
            if (hv == 0ull) continue;
            hw += std::snprintf(hl + hw, sizeof hl - (size_t)hw,
                " | h=%p evals=%llu taken=%llu copies=%llu extent=%ux%u latched=%d sr=%d",
                (void *)(uintptr_t)hv,
                (unsigned long long)g_hs[i].evals.load(std::memory_order_relaxed),
                (unsigned long long)g_hs[i].taken.load(std::memory_order_relaxed),
                (unsigned long long)g_hs[i].copies.load(std::memory_order_relaxed),
                g_hs[i].w.load(std::memory_order_relaxed), g_hs[i].hh.load(std::memory_order_relaxed),
                is_latched_scene_handle(hv) ? 1 : 0,
                g_sr_handle.load(std::memory_order_relaxed) == hv ? 1 : 0);
        }
        if (hw > 0 && (size_t)hw < sizeof hl)
            hw += std::snprintf(hl + hw, sizeof hl - (size_t)hw,
                " || retired handles=%llu evals=%llu taken=%llu copies=%llu",
                (unsigned long long)g_hs_ret_n.load(std::memory_order_relaxed),
                (unsigned long long)g_hs_ret_evals.load(std::memory_order_relaxed),
                (unsigned long long)g_hs_ret_taken.load(std::memory_order_relaxed),
                (unsigned long long)g_hs_ret_copies.load(std::memory_order_relaxed));
        mgpu::diag::info(hl);
    }
}

// ===========================================================================
// [PORT B2, 2026-11-11] THE INLINE HOOK'S ENTRY POINTS
//
// ngx_hook.cpp reads the parameter block at nvngx's OWN entry points. These
// four functions are the whole interface between it and this file, and every
// one of them is a wrapper over machinery that already existed here:
//
//   note_scene_handle_external    -> latch_scene_handle + the two atomics the
//                                    calibrator's own hook_create sets, so the
//                                    evaluate filters and the [R101] line mean
//                                    the same thing whichever route installed.
//   is_scene_handle_external      -> g_sr_handle / is_latched_scene_handle.
//   release_scene_handle_external -> unlatch_scene_handle.
//   evaluate_external             -> g_evals, capture(), and the R106 eval-copy
//                                    block, copied from hook_evaluate so there
//                                    is ONE copy implementation, not two.
//
// WHY THE COPY BLOCK IS COPIED AND NOT CALLED: hook_evaluate is gated on g_mode
// (Calib=) and would re-enter the real EvaluateFeature. The hook needs the
// body, not the thunk.
// ===========================================================================
void after_create_external(int feature_id, void *params, void **out_handle, int result)
{
    after_create((NVSDK_NGX_Feature)feature_id, reinterpret_cast<NVSDK_NGX_Parameter *>(params),
                 reinterpret_cast<NVSDK_NGX_Handle **>(out_handle), (NVSDK_NGX_Result)result);
}

void note_scene_handle_external(unsigned long long handle)
{
    if (handle == 0ull) return;
    latch_scene_handle(handle);
    g_sr_handle.store(handle, std::memory_order_relaxed);
    g_handle_known.store(true, std::memory_order_relaxed);
}

void release_scene_handle_external(unsigned long long handle)
{
    if (handle == 0ull) return;
    unlatch_scene_handle(handle);
}

bool is_scene_handle_external(unsigned long long handle)
{
    if (handle == 0ull) return false;
    if (g_sr_handle.load(std::memory_order_relaxed) == handle) return true;
    return is_latched_scene_handle(handle);
}

unsigned long long frame_now_external()
{
    return g_frames.load(std::memory_order_relaxed);
}

void evaluate_external(void *cmd_list, const void *handle, const void *params)
{
    ID3D12GraphicsCommandList *cl = reinterpret_cast<ID3D12GraphicsCommandList *>(cmd_list);
    const NVSDK_NGX_Handle *h = reinterpret_cast<const NVSDK_NGX_Handle *>(handle);
    const NVSDK_NGX_Parameter *p = reinterpret_cast<const NVSDK_NGX_Parameter *>(params);
    const unsigned long long hv = (unsigned long long)(uintptr_t)handle;

    g_evals.fetch_add(1, std::memory_order_relaxed);
    if (p != nullptr && mgpu::fgmap::on()) fgmap_tap(h, p);
    if (p != nullptr) capture(p);

    // [PORT B2] the one line that proves the hook route is alive and what it
    // actually captured. Said once per process, BEFORE any copy decision, so a
    // run in which the copy never fires still says whether the contract exists.
    {
        static std::atomic<unsigned> said_capture{0};
        if (said_capture.fetch_add(1, std::memory_order_relaxed) == 0u)
        {
            table ct{};
            if (read(ct))
            {
                char cl_line[520];
                snprintf(cl_line, sizeof cl_line,
                         "[MGPU][R804] CONTRACT from the inline hook: color=0x%llx depth=0x%llx mvec=0x%llx | "
                         "render=%ux%u display=%ux%u | flags=0x%08X inverted=%u mvlow=%u | jitter=%.3f,%.3f "
                         "mvecscale=%.3f,%.3f | have=0x%X",
                         ct.color, ct.depth, ct.mvec, ct.render_w, ct.render_h, ct.display_w, ct.display_h,
                         ct.create_flags, ct.depth_inverted, ct.mv_low_res, ct.jitter_x, ct.jitter_y,
                         ct.mv_scale_x, ct.mv_scale_y, ct.have);
                mgpu::diag::info(cl_line);
            }
            else
            {
                mgpu::diag::info("[MGPU][R804] the inline hook reached a scene evaluate but capture() published "
                                 "nothing this frame (a foreign provider may own the contract).");
            }
        }
    }

    // [PORT O2, 2026-11-11] THE GAME'S OWN DEPTH, AT THE SAME CALL. This runs
    // BEFORE the vector-copy gate below and is independent of it: the depth
    // route must not depend on MvecFromEval. stream_depth_copy is a no-op
    // unless DepthContract=1, and every failed guard leaves the frame to the
    // seal's tap path (one writer per frame, never two).
    if (cl != nullptr && p != nullptr)
    {
        table dt{};
        if (read(dt) && (dt.have & KEY_DEPTH) != 0u && dt.depth != 0ull)
        {
            const unsigned dw = (dt.depth_x != 0u) ? dt.depth_x : dt.render_w;
            const unsigned dh = (dt.depth_y != 0u) ? dt.depth_y : dt.render_h;
            mgpu::gpu1::stream_depth_copy((void *)cl, dt.depth, dw, dh);
        }
    }

    // R118/R106: only mode 1 copies. 2 (AUTO) means "not yet"; gpu1_context
    // switches it to 1 when the barrier route measures zero deliveries.
    const int ec = g_eval_copy_mode.load(std::memory_order_relaxed);
    if (ec != 1 || cl == nullptr || g_mvec_hook_fn == nullptr) return;

    const unsigned long long fr = g_frames.load(std::memory_order_relaxed);
    if (!is_scene_handle_external(hv) || g_eval_last_frame == fr)
    {
        g_eval_skips.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_eval_last_frame = fr;

    table et;
    if (!read(et) || (et.have & KEY_MVEC) == 0u || et.mvec == 0ull) return;

    const int hsi = hs_slot(hv);
    ID3D12Resource *r = (ID3D12Resource *)(uintptr_t)et.mvec;
    const D3D12_RESOURCE_STATES SR = (D3D12_RESOURCE_STATES)
        (D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = SR;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cl->ResourceBarrier(1, &b);

    g_mvec_hook_fn((void *)cl, et.mvec);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = SR;
    cl->ResourceBarrier(1, &b);

    g_eval_copies.fetch_add(1, std::memory_order_relaxed);
    if (g_eval_copies.load(std::memory_order_relaxed) == 1ull)
    {
        char fl[360];
        snprintf(fl, sizeof fl,
                 "[MGPU][R804] FIRST eval-copy from the inline hook: mvec=0x%llx sub=%ux%u, taken on the game's "
                 "own command list before its EvaluateFeature. This is the game's own field, not the census tap's.",
                 et.mvec, et.sub_w, et.sub_h);
        mgpu::diag::info(fl);
    }
    if (hsi >= 0)
    {
        g_hs[hsi].copies.fetch_add(1, std::memory_order_relaxed);
        g_hs[hsi].w.store(et.sub_w, std::memory_order_relaxed);
        g_hs[hsi].hh.store(et.sub_h, std::memory_order_relaxed);
    }
}

} // namespace calibrator
} // namespace mgpu
