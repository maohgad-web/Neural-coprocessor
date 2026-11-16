// MGPU Bridge - R341 FrameGen=4: DLSS Frame Generation on the neural card. See
// fg_gpu1.hpp. Design: Dave (davidm71), DGP1/DLSSG1.

#include "fg_gpu1.hpp"

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <thread>
#include <vector>

#include "diag.hpp"
#include "gpu1_context.hpp"   // present_frame, present_override, present_fence_addref, present_last_fence_value
#include "mgpu_ini_parser.hpp"
#include "../ext/ngx/nvsdk_ngx.h"
#include "fg_pace.hpp"    // R372: pacing rules beyond the 0.5.1 one (pure logic; tests/fg_pace_test.cpp)
#include "fg_instr.hpp"   // R371: read-only instruments (FGInstr=1)
#include "fg_forward.hpp" // R396: the forward warp as the generator (FrameGen=5)
#include "fg_ahead.hpp"   // R399: the list of the generator call for the picked game frame (FGAhead)
#include "depth_src.hpp"  // R376: DepthSource again on the FGLive read
#include "sl_camera.hpp"  // R379: the game's camera block (FGCamera=1)
#include "fg_fsr.hpp"     // R380: AMD FSR frame generation as the generator (FrameGen=3)

namespace mgpu::fg
{
namespace
{
const unsigned SETS = 3u;
const unsigned AH_NONE = 0xFFFFFFFFu;   // R399: set_t::ah_n - no in-between frame follows this neural frame

// ---- NGX entry points (the add-on's ABI) ----
typedef NVSDK_NGX_Result (NVSDK_CONV *pf_init_ext)(unsigned long long, const wchar_t *, ID3D12Device *, NVSDK_NGX_Version, const NVSDK_NGX_Parameter *);
typedef NVSDK_NGX_Result (NVSDK_CONV *pf_get_caps)(NVSDK_NGX_Parameter **);
typedef NVSDK_NGX_Result (NVSDK_CONV *pf_alloc)(NVSDK_NGX_Parameter **);
typedef NVSDK_NGX_Result (NVSDK_CONV *pf_populate)(NVSDK_NGX_Parameter *);
typedef NVSDK_NGX_Result (NVSDK_CONV *pf_create)(ID3D12GraphicsCommandList *, NVSDK_NGX_Feature, NVSDK_NGX_Parameter *, NVSDK_NGX_Handle **);
typedef NVSDK_NGX_Result (NVSDK_CONV *pf_evaluate)(ID3D12GraphicsCommandList *, const NVSDK_NGX_Handle *, const NVSDK_NGX_Parameter *, void *);   // callback as void *, as gpu1_context (P1.1)
typedef NVSDK_NGX_Result (NVSDK_CONV *pf_release)(NVSDK_NGX_Handle *);

struct set_t
{
    ID3D12Resource *real = nullptr;   // copy of nr_final, rests NON_PIXEL_SHADER_RESOURCE
    ID3D12Resource *gen = nullptr;    // the in-between frame, rests UNORDERED_ACCESS
    ID3D12Resource *genx[2] = {};     // R373: in-between frames 2..3 (FGMulti 3 / 4); null unless armed for them
    unsigned gen_n = 0u;              // R373: in-between frames this set holds (0 = none)
    float el = 1.0f;                  // R372: game frames since the previous evaluated frame (R87's count, from record)
    bool fill = false;                // R391: this pair was recorded under FGFill
    float phase = 0.0f;               // R401 (FGPhase): where in the pair its in-between frame sits (0 = the middle, the 0.5.1 frame)
    unsigned fill_n = 0u;             // R391: in-between frames to SHOW for it (0 = a step of one game frame: the real frame alone)
    unsigned long long frame = 0ull;  // NR frame the set holds (0 = none)
    bool has_gen = false;
    bool handed = false;              // handoff() took it (once per recorded frame)
    UINT64 fence = 0ull;              // present fence value after its last present (0 = never presented)
    std::atomic<bool> busy{false};    // handed to the pacing thread, not yet retired
    // R396 (FrameGen=5 only)
    ID3D12Resource *dp = nullptr;     // the set's own copy of this frame's depth (made at the first frame that has one)
    bool has_dp = false;              // dp holds this frame's depth
    std::atomic<bool> fwd{false};     // the pacing thread is to push this frame forward, or is doing so: not rewritten meanwhile
    // R399 (FGAhead, FrameGen 3 / 4). Bridge thread, except ah_n and ah_value: ah_value is written before ah_n is
    // stored, and the pacing thread reads it only after it has read ah_n.
    bool seated = false;              // the generator holds this neural frame as its previous frame
    bool ah_taken = false;            // ahead_take() has decided about the in-between frames that follow this frame
    unsigned ah_plan = 0u;            // in-between frames planned to follow this neural frame (0 = none)
    UINT64 ah_value = 0ull;           // the fence value (fg_ahead) of the list that makes them
    std::atomic<unsigned> ah_n{0u};   // 0 = not decided yet, AH_NONE = none will be made, else the frames that list makes
};

std::atomic<bool> g_on{false};
bool g_armed = false;
arm_info g_i{};
float g_fov = 90.0f, g_near = 0.1f, g_far = 10000.0f;
bool g_invert = false, g_pace = true;
// R371 / R372 / R373 (0.5.1 discovery). Every key absent = the 0.5.1 path, byte for byte.
std::atomic<int> g_pace_mode{1};       // FGPace: 0 no wait, 1 the 0.5.1 rule (per pair), 2 clock on scene time, 3 absolute clock (compare only)
unsigned g_mult_req = 2u;              // FGMulti as read (2..4)
unsigned g_mult_armed = 2u;            // FGMulti at the arm: in-between textures made per set = this - 1
std::atomic<unsigned> g_mult{2u};      // FGMulti in use (<= armed); live with FGLive=1
bool g_live = false;                   // FGLive=1: FGPace / FGMulti re-read from mgpu.ini every 120 hand-overs (bridge thread)
bool g_instr = false;                  // FGInstr=1: fg_instr.cpp
mgpu::fgpace::scene_clock g_scene;     // bridge thread (handoff)
mgpu::fgpace::clock_state g_clk;       // pacing thread
mgpu::fgpace::abs_state g_abs;         // pacing thread
unsigned long long g_r372_alt_jobs = 0ull;   // bridge thread: pairs handed to a rule other than the 0.5.1 one
// R378: the unit DLSSG.MvecScale turns the vectors into. 0 (absent) = the 0.5.1 expression, lane scale x elapsed / extent.
// 1 = lane scale x elapsed: PIXELS at the vector resource, the unit the DLSS-FG SDK states for this parameter.
std::atomic<int> g_mv_unit{0};
std::atomic<bool> g_r378_said{false};
// R384 (FGSkipCarry=1): the game frames of an evaluated frame that record() had to skip (set not retired) are added
// to the next pair's elapsed count, so the vectors cover the whole distance to the generator's previous frame.
std::atomic<int> g_skip_carry{0};
float g_carry_el = 0.0f;                   // bridge thread; always 0 with the key absent
unsigned long long g_carried = 0ull;
// R387 (FGPresentFree=1): paced presents do not take the stream lock (gpu1_context.cpp present_override_free).
bool g_present_free = false;
// R390 (FGLimit=1) / R391 (FGFill=1) / R392 (the panel's live switches). All absent = the paths above, untouched.
std::atomic<int> g_limit{0}, g_fill{0};    // g_fill: 0 off, 1 fill by step, 2 the same with FGMulti's count as the least
std::atomic<int> g_fill_val{1};            // R392: the value the panel's switch puts back (the file's last 1 or 2)
std::atomic<int> g_panel_own{0};           // R392: bit k set = the panel set key k this session (0 FGMvUnit, 1 FGSkipCarry, 2 FGLimit, 3 FGFill); FGLive leaves it alone
mgpu::fgpace::limit_state g_lim;           // bridge thread (handoff)
double g_refresh_ms = 0.0;                 // bridge thread: the display's period from DWM, at the arm and every 600 hand-overs (0 = not known)
double g_game_ms = 0.0;                    // bridge thread: one game frame in ms (hand-over interval / elapsed), slow average
unsigned long long g_fill_pairs = 0ull, g_fill_shown = 0ull, g_fill_alone = 0ull;   // bridge thread
unsigned long long g_fill_cut = 0ull;      // R391b, bridge thread: in-between frames not shown so the real frame is not held longer than the landed rule holds it
// R395 (FGDisplayCheck=1): frame generation only while the display can show its frames. Key absent: one relaxed load.
std::atomic<int> g_dcheck{0};
mgpu::fgpace::display_gate g_dgate;        // bridge thread (record)
LARGE_INTEGER g_dgate_t{};                 // bridge thread: the previous evaluated frame's time
unsigned long long g_dgate_real_only = 0ull;   // bridge thread: evaluated frames shown as the real frame alone because of it
std::atomic<int> g_dgate_ui{0}, g_dgate_ui_disp{0}, g_dgate_ui_would{0};   // for the panel: 0 nothing to say, 1 off, 2.. lowered to this
// R396 (FrameGen=5): the forward warp. Nothing here is touched by another generator.
std::atomic<int> g_warp_fill{2};           // FGWarpFill: 2 no fill, the fetched neural pixel as it is (default, R398), 1 the game's pixel matched to the neural look, 0 the neural neighbour
unsigned long long g_fwd_posted = 0ull, g_fwd_noinput = 0ull, g_fwd_cut = 0ull, g_fwd_gate = 0ull, g_fwd_late = 0ull;   // bridge thread
std::atomic<unsigned long long> g_fwd_stopped{0ull};   // pacing thread: pairs cut short because a newer frame was waiting
bool g_fwd_said = false;
float g_fwd_rgb[3] = { 0.0f, 0.0f, 0.0f };   // bridge thread: the clear colour of the last hand-over (used where the picture does not cover the chain)
// R397 (FGMvScaleNow=1): the vector scale the STREAM uses now, not the copy made at the arm. The stream sets its scale
// while running (units learned or read after the arm); the arm's copy can still be 1.0 then. Key absent: the arm's copy,
// as 0.5.1, for every generator.
std::atomic<int> g_scale_now{0};
float g_now_sx = 1.0f, g_now_sy = 1.0f;    // bridge thread: what the stream handed at the last frame
bool g_now_have = false;
float g_scale_said_x = 0.0f, g_scale_said_y = 0.0f;
std::atomic<int> g_warp_show{0};           // R397 (FGWarpShow=1): the warp tints what it finds uncovered instead of filling it
// R399 (FGAhead=1|2, FrameGen 3 / 4): the in-between frame is made from the neural frame on screen and the game frame
// the neural card has just picked, at the pick; the neural frame is shown the moment it finishes (fg_ahead.hpp).
// Key absent: one relaxed load in ahead_take() and in record(), nothing else.
std::atomic<int> g_ahead{0};               // 0 off, 1 the neural frame starts the generator again (a reset), 2 the neural frame is an ordinary frame with no motion
std::atomic<int> g_ahead_val{1};           // the value the panel's switch puts back (the file's last 1 or 2)
int g_ah_mode_was = 0;                     // bridge thread: the value record() saw last (a change starts the generator again)
unsigned long long g_ah_hist_game = 0ull;  // bridge thread: the game frame the generator holds as its previous frame (0 = not one of ours)
bool g_ah_said_cq = false, g_ah_said_ext = false, g_ah_said_up = false;
unsigned long long g_ah_seats = 0ull, g_ah_pairs = 0ull, g_ah_made = 0ull, g_ah_unseated = 0ull, g_ah_noinput = 0ull,
                   g_ah_cut = 0ull, g_ah_extent = 0ull, g_ah_busy = 0ull, g_ah_failed = 0ull;   // bridge thread
std::atomic<unsigned long long> g_ah_shown{0ull}, g_ah_late{0ull}, g_ah_stopped{0ull}, g_ah_none{0ull};   // pacing thread
// R400 (Marcelo 08:05): a skip rule on what frame generation shows, and the game's own clock for it. Both absent = one
// relaxed load each, the landed path untouched.
//   FGSkipJump=N (2..8): a pair that spans N or more game frames gets no in-between frame; its neural frame is shown the
//   moment it finishes. The span is the seal's count, known when the frame is picked. The generator is still handed
//   the frame (a reset), so the next pair is made from it.
//   FGGameClock=1: the neural frame of a pair is shown on a game present mark, never later than the landed rule shows it.
//   FGGameClock=2: the in-between frame AND the neural frame are shown on game present marks (compare only: it can hold
//   a frame up to one game frame longer).
std::atomic<int> g_skip_jump{0}, g_skip_jump_val{3};
// R401 (Marcelo 13:58, "option 2"): FGPhase=67|75 - the in-between frame of a pair sits at two thirds / three quarters
// of it, not at the middle, and the neural frame waits only the rest of the interval behind it (a third / a quarter,
// not a half). Each picture is still shown at the moment it belongs to. DLSS-G is asked for that one frame (frame N of
// N in its multi-frame terms). FGPhaseAll=1: it is asked for all N in order and the last is shown (if it will not make
// the last one alone). With FrameGen=4, FGMulti=2, FGPace=1, no FGFill. Absent: one relaxed load.
std::atomic<int> g_phase{0}, g_phase_val{75}, g_phase_all{0};
unsigned long long g_ph_pairs = 0ull, g_ph_nofit = 0ull;   // bridge thread
bool g_ph_said = false, g_ph_refused = false;   // refused: the generator returned an error for a late frame in this arm - not asked again
std::atomic<int> g_gclock{0}, g_gclock_val{1};
std::atomic<long long> g_gm_qpc{0}, g_gm_period{0};   // written by the game's present (game_present_mark): the last mark and the game frame, in QPC ticks
unsigned long long g_jump_pairs = 0ull;               // bridge thread
std::atomic<unsigned long long> g_gc_marks{0ull}, g_gc_free{0ull};   // pacing thread: neural frames placed on a mark / by the landed rule
int g_gen = 4;                             // R380: the generator of this arm: 4 NVIDIA DLSS-G (the 0.5.1 path), 3 AMD FSR (fg_fsr.cpp)
unsigned long long g_r379_prev_f = 0ull;   // R379: the frame of the previous evaluate (bridge thread); 0 = none

set_t g_set[SETS];
unsigned g_k = 0u;                     // the set record() writes next
unsigned long long g_fid = 0ull;       // DLSSG frame id (sequential; a gap resets)
unsigned long long g_last_fid = 0ull;

// the feature
bool g_tried = false, g_up = false, g_failed = false;
HMODULE g_snip = nullptr;
pf_create g_create = nullptr; pf_evaluate g_eval = nullptr; pf_release g_rel = nullptr;
NVSDK_NGX_Parameter *g_params = nullptr;
NVSDK_NGX_Handle *g_handle = nullptr;
unsigned g_mfc_max = 0u;
float g_v2c[16] = {}, g_c2v[16] = {}, g_ident[16] = {};

// counters
unsigned long long g_made = 0ull, g_nomv = 0ull, g_busy_skips = 0ull, g_errors = 0ull, g_evals = 0ull;
std::atomic<unsigned long long> g_pres_gen{0}, g_pres_real{0}, g_handoffs{0}, g_dropped{0};
double g_half_sum = 0.0; unsigned g_half_n = 0u;

// the pacing thread
struct job_t { set_t *set = nullptr; bool gen = false; float r = 0, g = 0, b = 0; LARGE_INTEGER t0{}; double half_ms = 8.0;
               // R372 / R373: read only when alt is true (alt false = the 0.5.1 schedule below, untouched)
               bool alt = false; int pace = 1; unsigned gen_n = 0u; double ema_ms = 0.0, c_real = 0.0, span = 0.0;
               // R390 / R391: read only when lim is true. show = in-between frames to present, sel[k] = which of the
               // pair's `have` frames is the k-th one, step_ms = spacing of the frames shown, refresh_ms = display period.
               bool lim = false; unsigned show = 0u, have = 0u; unsigned char sel[3] = { 0, 0, 0 }; double step_ms = 0.0, refresh_ms = 0.0;
               bool fill = false;      // R391: the pair was recorded under FGFill
               // R396 (FrameGen=5): fwd_n != 0 = this job pushes set->real forward fwd_n times; nothing above is read
               unsigned fwd_n = 0u; mgpu::fgfwd::pair fwd{};
               // R399 (FGAhead): in-between frames to show AFTER this job's neural frame (0 = none; never with the key absent)
               unsigned ahead = 0u; };
std::mutex g_mx;
std::condition_variable g_cv;
std::thread *g_thread = nullptr;
bool g_has_job = false, g_stop = false;
job_t g_job;
std::atomic<bool> g_thread_failed{false};
LARGE_INTEGER g_last_handoff{};
double g_ema_ms = 0.0;
ID3D12Fence *g_pfence = nullptr;        // the present fence, AddRef'd
HANDLE g_timer = nullptr;

// ---- ini (the residual reader's rules) ----
bool module_dir(wchar_t *dir, size_t n)
{
    static const int anchor = 0;
    HMODULE hm = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&anchor), &hm)) return false;
    wchar_t mod[MAX_PATH];
    const DWORD len = GetModuleFileNameW(hm, mod, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return false;
    wchar_t *slash = wcsrchr(mod, L'\\');
    if (slash == nullptr) return false;
    slash[1] = L'\0';
    wcscpy_s(dir, n, mod);
    return true;
}

bool read_keys(int &framegen, unsigned long long &app_id)
{
    framegen = 0; app_id = 0x4D475055ull;
    wchar_t dir[MAX_PATH];
    if (!module_dir(dir, MAX_PATH)) return false;
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%lsmgpu.ini", dir);
    FILE *f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || f == nullptr) return false;
    std::vector<char> buf(mgpu::config::MAX_BYTES + 2u, '\0');
    const size_t got = fread(buf.data(), 1, mgpu::config::MAX_BYTES + 1u, f);
    fclose(f);
    if (got > mgpu::config::MAX_BYTES) return false;
    buf[got] = '\0';
    const char *v = mgpu::config::find(buf.data(), got, "FrameGen");
    framegen = (v != nullptr) ? atoi(v) : 0;
    auto fkey = [&](const char *k, float dflt, float lo, float hi) -> float {
        const char *x = mgpu::config::find(buf.data(), got, k);
        if (x == nullptr) return dflt;
        char *e = nullptr; const double d = strtod(x, &e);
        if (e == x || d < lo || d > hi) return dflt;
        return (float)d;
    };
    g_fov  = fkey("FGFov", 90.0f, 20.0f, 170.0f);
    g_near = fkey("FGNear", 0.1f, 0.0001f, 1000.0f);
    g_far  = fkey("FGFar", 10000.0f, 1.0f, 1.0e7f);
    const char *inv = mgpu::config::find(buf.data(), got, "FGInvertMV"); g_invert = inv != nullptr && atoi(inv) == 1;
    const char *pc = mgpu::config::find(buf.data(), got, "FGPace");      g_pace = !(pc != nullptr && atoi(pc) == 0);
    {   // R371 / R372 / R373: absent = the 0.5.1 values (FGPace 1, FGMulti 2, no live re-read, no instruments)
        int pm = (pc != nullptr) ? atoi(pc) : 1; if (pm < 0 || pm > 3) pm = 1;
        g_pace_mode.store(pm, std::memory_order_relaxed);
        const char *fm = mgpu::config::find(buf.data(), got, "FGMulti");
        int mu = (fm != nullptr) ? atoi(fm) : 2; if (mu < 2 || mu > 4) mu = 2;
        g_mult_req = (unsigned)mu;
        const char *lv = mgpu::config::find(buf.data(), got, "FGLive");  g_live = lv != nullptr && atoi(lv) == 1;
        const char *in = mgpu::config::find(buf.data(), got, "FGInstr"); g_instr = in != nullptr && atoi(in) == 1;
        const char *mu1 = mgpu::config::find(buf.data(), got, "FGMvUnit");   // R378
        g_mv_unit.store((mu1 != nullptr && atoi(mu1) == 1) ? 1 : 0, std::memory_order_relaxed);
        const char *sc = mgpu::config::find(buf.data(), got, "FGSkipCarry");   // R384
        g_skip_carry.store((sc != nullptr && atoi(sc) == 1) ? 1 : 0, std::memory_order_relaxed);
        const char *pf = mgpu::config::find(buf.data(), got, "FGPresentFree");   // R387
        g_present_free = pf != nullptr && atoi(pf) == 1;
        const char *lm = mgpu::config::find(buf.data(), got, "FGLimit");   // R390
        g_limit.store((lm != nullptr && atoi(lm) == 1) ? 1 : 0, std::memory_order_relaxed);
        const char *fl = mgpu::config::find(buf.data(), got, "FGFill");    // R391
        { const int fv = (fl != nullptr) ? atoi(fl) : 0; g_fill.store((fv == 1 || fv == 2) ? fv : 0, std::memory_order_relaxed); if (fv == 1 || fv == 2) g_fill_val = fv; }
        g_panel_own.store(0, std::memory_order_relaxed);   // R392: a new arm reads the file again
        const char *dc = mgpu::config::find(buf.data(), got, "FGDisplayCheck");   // R395
        g_dcheck.store((dc != nullptr && atoi(dc) == 1) ? 1 : 0, std::memory_order_relaxed);
        const char *wf = mgpu::config::find(buf.data(), got, "FGWarpFill");   // R396: absent = 1
        { const int wv = (wf != nullptr) ? atoi(wf) : 2; g_warp_fill.store((wv == 0 || wv == 1) ? wv : 2, std::memory_order_relaxed); }
        const char *sn = mgpu::config::find(buf.data(), got, "FGMvScaleNow");   // R397
        g_scale_now.store((sn != nullptr && atoi(sn) == 1) ? 1 : 0, std::memory_order_relaxed);
        const char *ws = mgpu::config::find(buf.data(), got, "FGWarpShow");     // R397
        g_warp_show.store((ws != nullptr && atoi(ws) == 1) ? 1 : 0, std::memory_order_relaxed);
        const char *fp = mgpu::config::find(buf.data(), got, "FGPhase");        // R401: absent = 0
        { const int pv = (fp != nullptr) ? atoi(fp) : 0; const bool po = (pv == 67 || pv == 75); g_phase.store(po ? pv : 0, std::memory_order_relaxed); if (po) g_phase_val = pv; }
        const char *fa = mgpu::config::find(buf.data(), got, "FGPhaseAll");     // R401
        g_phase_all.store((fa != nullptr && atoi(fa) == 1) ? 1 : 0, std::memory_order_relaxed);
        const char *sj = mgpu::config::find(buf.data(), got, "FGSkipJump");     // R400: absent = 0
        { const int jv = (sj != nullptr) ? atoi(sj) : 0; const bool jo = (jv >= 2 && jv <= 8); g_skip_jump.store(jo ? jv : 0, std::memory_order_relaxed); if (jo) g_skip_jump_val = jv; }
        const char *gk = mgpu::config::find(buf.data(), got, "FGGameClock");    // R400: absent = 0
        { const int gv = (gk != nullptr) ? atoi(gk) : 0; const bool go = (gv == 1 || gv == 2); g_gclock.store(go ? gv : 0, std::memory_order_relaxed); if (go) g_gclock_val = gv; }
        const char *ah = mgpu::config::find(buf.data(), got, "FGAhead");        // R399: absent = 0
        { const int av = (ah != nullptr) ? atoi(ah) : 0; g_ahead.store((av == 1 || av == 2) ? av : 0, std::memory_order_relaxed); if (av == 1 || av == 2) g_ahead_val = av; }
    }
    const char *ai = mgpu::config::find(buf.data(), got, "NgxAppId");    if (ai != nullptr) app_id = (unsigned long long)(unsigned)atoi(ai);
    return true;
}

// ---- modules ----
HMODULE find_core()
{
    HMODULE m = GetModuleHandleW(L"_nvngx.dll");
    if (m != nullptr) return m;
    m = LoadLibraryW(L"_nvngx.dll");
    if (m != nullptr) return m;
    wchar_t dir[MAX_PATH] = {}; DWORD sz = sizeof dir;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", L"FullPath", RRF_RT_REG_SZ, nullptr, dir, &sz) == ERROR_SUCCESS)
    {
        wchar_t p[MAX_PATH * 2] = {};
        _snwprintf_s(p, MAX_PATH * 2, _TRUNCATE, L"%s\\_nvngx.dll", dir);
        return LoadLibraryW(p);
    }
    return nullptr;
}

// The driver's model store: ProgramData\NVIDIA\NGX\models\dlssg\versions\<highest>\files\*.bin
// that exports CreateFeature (Dave's route); else <add-on dir>\mgpu\nvngx_dlssg.dll.
HMODULE load_snippet(char *where, size_t where_n)
{
    where[0] = '\0';
    wchar_t pd[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"ProgramData", pd, MAX_PATH) != 0)
    {
        wchar_t root[MAX_PATH] = {}, glob[MAX_PATH] = {};
        _snwprintf_s(root, MAX_PATH, _TRUNCATE, L"%s\\NVIDIA\\NGX\\models\\dlssg\\versions", pd);
        _snwprintf_s(glob, MAX_PATH, _TRUNCATE, L"%s\\*", root);
        unsigned long long best = 0ull; wchar_t best_dir[MAX_PATH] = {};
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(glob, &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do {
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
                unsigned long long v = 0ull; bool okd = fd.cFileName[0] >= L'0' && fd.cFileName[0] <= L'9';
                for (const wchar_t *c = fd.cFileName; okd && *c != 0; ++c)
                { if (*c < L'0' || *c > L'9') okd = false; else v = v * 10ull + (unsigned long long)(*c - L'0'); }
                if (okd && v > best) { best = v; wcscpy_s(best_dir, MAX_PATH, fd.cFileName); }
            } while (FindNextFileW(h, &fd) != 0);
            FindClose(h);
        }
        if (best != 0ull)
        {
            wchar_t fglob[MAX_PATH] = {};
            _snwprintf_s(fglob, MAX_PATH, _TRUNCATE, L"%s\\%s\\files\\*.bin", root, best_dir);
            h = FindFirstFileW(fglob, &fd);
            if (h != INVALID_HANDLE_VALUE)
            {
                do {
                    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
                    wchar_t full[MAX_PATH] = {};
                    _snwprintf_s(full, MAX_PATH, _TRUNCATE, L"%s\\%s\\files\\%s", root, best_dir, fd.cFileName);
                    HMODULE m = LoadLibraryW(full);
                    if (m == nullptr) continue;
                    if (GetProcAddress(m, "NVSDK_NGX_D3D12_CreateFeature") == nullptr) { FreeLibrary(m); continue; }
                    FindClose(h);
                    snprintf(where, where_n, "driver model store %ls", full);
                    return m;
                } while (FindNextFileW(h, &fd) != 0);
                FindClose(h);
            }
        }
    }
    wchar_t dir[MAX_PATH];
    if (!module_dir(dir, MAX_PATH)) return nullptr;
    wchar_t p[MAX_PATH];
    _snwprintf_s(p, MAX_PATH, _TRUNCATE, L"%lsmgpu\\nvngx_dlssg.dll", dir);
    HMODULE m = LoadLibraryW(p);
    if (m != nullptr) snprintf(where, where_n, "mgpu folder %ls", p);
    return m;
}

bool inv4(const float m[16], float out[16])
{
    float inv[16];
    inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
    float det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (det == 0.0f) return false;
    det = 1.0f / det;
    for (int i = 0; i < 16; ++i) out[i] = inv[i] * det;
    return true;
}

void fail_for_arm(const char *why)
{
    g_failed = true;
    g_on.store(false, std::memory_order_release);
    char l[400];
    snprintf(l, sizeof l, "[MGPU][R341] FrameGen=%d OFF for this arm: %s. The bridge presents the real frames as before.", g_gen, why);
    mgpu::diag::error(l);
}

// Loads the snippet, opens the session on the neural device, creates the feature at the
// NR output's size. Records the creation into `cl`. Once per arm.
bool ensure_feature(ID3D12GraphicsCommandList *cl)
{
    if (g_up) return true;
    if (g_tried) return false;
    g_tried = true;
    if (g_gen == 5)
    {   // R396: the forward warp. Nothing of NGX or FSR is loaded or created on this branch.
        (void)cl;
        mgpu::fgfwd::info wi;
        wi.dev = g_i.ndev; wi.w = g_i.w; wi.h = g_i.h; wi.fmt = g_i.fmt; wi.n_max = g_mult_armed; wi.depth_inverted = g_i.depth_inverted;
        char why[460] = "";
        if (!mgpu::fgfwd::create(wi, why, sizeof why)) { fail_for_arm(why); return false; }
        g_up = true;
        return true;
    }
    if (g_gen == 3)
    {   // R380: AMD FSR frame generation. Nothing of NGX is loaded or created on this branch.
        (void)cl;
        mgpu::fgfsr::info fi;
        fi.ndev = g_i.ndev; fi.w = g_i.w; fi.h = g_i.h; fi.fmt = g_i.fmt;
        fi.mvec_w = g_i.mvec_w; fi.mvec_h = g_i.mvec_h; fi.depth_w = g_i.depth_w; fi.depth_h = g_i.depth_h;
        fi.depth_inverted = g_i.depth_inverted; fi.mvec_scale_x = g_i.mvec_scale_x; fi.mvec_scale_y = g_i.mvec_scale_y;
        fi.fov_h_deg = g_fov; fi.cam_near = g_near; fi.cam_far = g_far; fi.invert = g_invert;
        char why[420] = "";
        if (!mgpu::fgfsr::create(fi, why, sizeof why)) { fail_for_arm(why); return false; }
        g_mfc_max = 3u;   // in-between frames per pair this module has textures for (the API allows four)
        g_up = true;
        return true;
    }
    char line[900];
    HMODULE core = find_core();
    char where[MAX_PATH * 2 + 40] = {};
    g_snip = load_snippet(where, sizeof where);
    if (core == nullptr || g_snip == nullptr)
    {
        fail_for_arm(core == nullptr ? "the NGX core (_nvngx.dll) was not found"
                                     : "no nvngx_dlssg.dll in the driver model store or in the mgpu folder (copy one INTO the mgpu folder, not beside the exe)");
        return false;
    }
    auto pick = [&](const char *name, bool prefer_snip) -> FARPROC {
        FARPROC p = GetProcAddress(prefer_snip ? g_snip : core, name);
        if (p == nullptr) p = GetProcAddress(prefer_snip ? core : g_snip, name);
        return p;
    };
    g_create = (pf_create)pick("NVSDK_NGX_D3D12_CreateFeature", true);
    g_eval   = (pf_evaluate)pick("NVSDK_NGX_D3D12_EvaluateFeature", true);
    g_rel    = (pf_release)pick("NVSDK_NGX_D3D12_ReleaseFeature", true);
    pf_get_caps caps = (pf_get_caps)pick("NVSDK_NGX_D3D12_GetCapabilityParameters", false);
    pf_alloc alloc = (pf_alloc)pick("NVSDK_NGX_D3D12_AllocateParameters", false);
    pf_init_ext iext = (pf_init_ext)GetProcAddress(g_snip, "NVSDK_NGX_D3D12_Init_Ext");
    pf_populate pop = (pf_populate)GetProcAddress(g_snip, "NVSDK_NGX_D3D12_PopulateParameters_Impl");
    snprintf(line, sizeof line, "[MGPU][R341] DLSS-G snippet from the %s | CreateFeature %s Evaluate %s Release %s Caps %s Alloc %s Init_Ext %s Populate %s",
             where, g_create ? "ok" : "MISSING", g_eval ? "ok" : "MISSING", g_rel ? "ok" : "missing", caps ? "ok" : "missing",
             alloc ? "ok" : "MISSING", iext ? "ok" : "missing", pop ? "ok" : "missing");
    mgpu::diag::info(line);
    if (g_create == nullptr || g_eval == nullptr || alloc == nullptr) { fail_for_arm("the DLSS-G entry points did not resolve"); return false; }
    NVSDK_NGX_Result r = alloc(&g_params);
    if (r != NVSDK_NGX_Result_Success || g_params == nullptr) { g_params = nullptr; snprintf(line, sizeof line, "AllocateParameters 0x%08X", (unsigned)r); fail_for_arm(line); return false; }
    int fgv = 0; unsigned long long app_id = 0x4D475055ull;
    (void)read_keys(fgv, app_id);
    wchar_t data_path[MAX_PATH];
    if (!module_dir(data_path, MAX_PATH)) wcscpy_s(data_path, MAX_PATH, L".");
    NVSDK_NGX_Result ri = NVSDK_NGX_Result_Success;
    if (iext != nullptr) ri = iext(app_id, data_path, g_i.ndev, NVSDK_NGX_Version_API, g_params);
    if (pop != nullptr) (void)pop(g_params);
    if (caps != nullptr)
    {
        NVSDK_NGX_Parameter *cap = nullptr;
        if (caps(&cap) == NVSDK_NGX_Result_Success && cap != nullptr)
        {
            int avail = -1, needs = -1; unsigned int fir = 0, mfc = 0;
            (void)cap->Get("FrameGeneration.Available", &avail);
            (void)cap->Get("FrameGeneration.NeedsUpdatedDriver", &needs);
            (void)cap->Get("FrameGeneration.FeatureInitResult", &fir);
            (void)cap->Get("DLSSG.MultiFrameCountMax", &mfc);
            g_mfc_max = mfc;
            snprintf(line, sizeof line, "[MGPU][R341] Init_Ext 0x%08X | FrameGeneration.Available=%d NeedsUpdatedDriver=%d FeatureInitResult=0x%08X MultiFrameCountMax=%u (2x used)",
                     (unsigned)ri, avail, needs, fir, mfc);
            mgpu::diag::info(line);
        }
    }
    NVSDK_NGX_Parameter *p = g_params;
    p->Set("CreationNodeMask", 1u);
    p->Set("VisibilityNodeMask", 1u);
    p->Set("Width", g_i.w);
    p->Set("Height", g_i.h);
    p->Set("DLSSG.Width", g_i.w);
    p->Set("DLSSG.Height", g_i.h);
    p->Set("DLSSG.BackbufferFormat", (unsigned int)g_i.fmt);
    p->Set("DLSSG.InternalWidth", g_i.mvec_w != 0u ? g_i.mvec_w : g_i.w);
    p->Set("DLSSG.InternalHeight", g_i.mvec_h != 0u ? g_i.mvec_h : g_i.h);
    p->Set("DLSSG.DynamicResolution", 0u);
    LARGE_INTEGER t0{}, t1{}, fq{};
    QueryPerformanceCounter(&t0);
    r = g_create(cl, NVSDK_NGX_Feature_FrameGeneration, p, &g_handle);
    QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&fq);
    snprintf(line, sizeof line, "[MGPU][R341] CreateFeature(FrameGeneration) on the neural card at %ux%u fmt=%u (vectors %ux%u, depth %ux%u): 0x%08X in %.0f ms",
             g_i.w, g_i.h, (unsigned)g_i.fmt, g_i.mvec_w, g_i.mvec_h, g_i.depth_w, g_i.depth_h, (unsigned)r,
             fq.QuadPart ? (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)fq.QuadPart : 0.0);
    if (r != NVSDK_NGX_Result_Success || g_handle == nullptr)
    {
        g_handle = nullptr;
        char why[120]; snprintf(why, sizeof why, "CreateFeature(FrameGeneration) returned 0x%08X", (unsigned)r);
        fail_for_arm(why);
        return false;
    }
    mgpu::diag::info(line);
    for (int i = 0; i < 16; ++i) g_ident[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    // camera: estimated perspective, row-major, reverse-Z per DepthInverted (Dave's estimate)
    const float aspect = (g_i.h != 0u) ? (float)g_i.w / (float)g_i.h : 16.0f / 9.0f;
    const float hrad = g_fov * 3.14159265f / 180.0f;
    const float vfov = 2.0f * atanf(tanf(hrad * 0.5f) / aspect);
    const float ys = 1.0f / tanf(vfov * 0.5f), xs = ys / aspect;
    const float n = g_near, fa = g_far;
    float A, B;
    if (g_i.depth_inverted) { A = n / (n - fa); B = -fa * n / (n - fa); }
    else                    { A = fa / (fa - n); B = -n * fa / (fa - n); }
    const float pm[16] = { xs,0,0,0,  0,ys,0,0,  0,0,A,1,  0,0,B,0 };
    for (int i = 0; i < 16; ++i) g_v2c[i] = pm[i];
    if (!inv4(g_v2c, g_c2v)) for (int i = 0; i < 16; ++i) g_c2v[i] = g_ident[i];
    g_up = true;
    mgpu::diag::info("[MGPU][R341] NVIDIA DLSS FRAME GENERATION UP on the neural card (FrameGen=4, 2x). Camera estimated from FGFov/FGNear/FGFar; "
                     "the generated frame of each pair is presented first, the real one half an NR interval later (FGPace=1).");
    return true;
}

void set_sub(NVSDK_NGX_Parameter *p, const char *pre, UINT w, UINT h)
{
    char k[96];
    snprintf(k, sizeof k, "DLSSG.%sSubrectBaseX", pre);  p->Set(k, 0u);
    snprintf(k, sizeof k, "DLSSG.%sSubrectBaseY", pre);  p->Set(k, 0u);
    snprintf(k, sizeof k, "DLSSG.%sSubrectWidth", pre);  p->Set(k, w);
    snprintf(k, sizeof k, "DLSSG.%sSubrectHeight", pre); p->Set(k, h);
}

// R397: the vector scale in use. Key absent or nothing handed yet: the arm's copy (0.5.1).
float r397_scale_x() { return (g_scale_now.load(std::memory_order_relaxed) != 0 && g_now_have) ? g_now_sx : g_i.mvec_scale_x; }
float r397_scale_y() { return (g_scale_now.load(std::memory_order_relaxed) != 0 && g_now_have) ? g_now_sy : g_i.mvec_scale_y; }

NVSDK_NGX_Result evaluate(ID3D12GraphicsCommandList *cl, ID3D12Resource *real, ID3D12Resource *mvec, ID3D12Resource *depth,
                          ID3D12Resource *out, float mv_el, bool reset, unsigned long long fid,
                          unsigned mfc = 1u, unsigned mfi = 1u,   // R373: in-between frame mfi of mfc (1 of 1 = the 0.5.1 call)
                          const mgpu::slcam::camera *cam = nullptr)   // R379: the game's camera for this pair, or null (as 0.5.1)
{
    const float aspect = (g_i.h != 0u) ? (float)g_i.w / (float)g_i.h : 16.0f / 9.0f;
    const float hrad = g_fov * 3.14159265f / 180.0f;
    const float vfov = 2.0f * atanf(tanf(hrad * 0.5f) / aspect);
    const float mw = (float)(g_i.mvec_w != 0u ? g_i.mvec_w : g_i.w), mh = (float)(g_i.mvec_h != 0u ? g_i.mvec_h : g_i.h);
    const float sgn = g_invert ? -1.0f : 1.0f;
    NVSDK_NGX_Parameter *p = g_params;
    p->Set("DLSSG.Backbuffer", real);
    p->Set("DLSSG.MVecs", mvec);
    p->Set("DLSSG.Depth", depth);
    p->Set("DLSSG.HUDLess", (ID3D12Resource *)nullptr);
    p->Set("DLSSG.UI", (ID3D12Resource *)nullptr);
    p->Set("DLSSG.UIAlpha", (ID3D12Resource *)nullptr);
    p->Set("DLSSG.BidirectionalDistortionField", (ID3D12Resource *)nullptr);
    p->Set("DLSSG.OutputInterpolated", out);
    p->Set("DLSSG.OutputReal", (ID3D12Resource *)nullptr);
    p->Set("DLSSG.OutputDisableInterpolation", (ID3D12Resource *)nullptr);
    p->Set("DLSSG.MultiFrameCount", mfc);
    p->Set("DLSSG.MultiFrameIndex", mfi);
    p->Set("DLSSG.CameraViewToClip", (void *)g_v2c);
    p->Set("DLSSG.ClipToCameraView", (void *)g_c2v);
    p->Set("DLSSG.ClipToLensClip", (void *)g_ident);
    p->Set("DLSSG.ClipToPrevClip", (void *)g_ident);
    p->Set("DLSSG.PrevClipToClip", (void *)g_ident);
    p->Set("DLSSG.JitterOffsetX", 0.0f);
    p->Set("DLSSG.JitterOffsetY", 0.0f);
    // vectors: the game's pixel units scaled to [-1,1] by 1/extent (Streamline's convention), times the frames elapsed
    // R378 (0.5.1 discovery). With FGMvUnit absent the value is the 0.5.1 expression, unchanged: lane scale x elapsed / extent.
    // The DLSS-FG SDK (310.7.0, parameter reference) says of this parameter: the vectors are expected in screen-space
    // PIXELS at the resolution of the vector resource, and the scale is 1 when they already are, or the resource's
    // width / height when they are normalized. The lane's scale is exactly that factor (it is what the game hands
    // DLSS for the same resource), so pixels = lane scale x elapsed, with NO division by the extent. Eight recorded
    // evaluates of games' own frame generation ([D20][NR1]) carry MvecScale = the vector resource's width, height.
    const bool r378_px = g_mv_unit.load(std::memory_order_relaxed) == 1;
    const float r397_x = r397_scale_x(), r397_y = r397_scale_y();   // R397: the arm's copy unless FGMvScaleNow=1
    const float r378_sx = r378_px ? sgn * r397_x * mv_el : sgn * r397_x * mv_el / mw;
    const float r378_sy = r378_px ? sgn * r397_y * mv_el : sgn * r397_y * mv_el / mh;
    if ((r378_px || g_instr) && !g_r378_said.exchange(true))
    {
        char l8[460];
        snprintf(l8, sizeof l8, "[MGPU][R378] DLSSG.MvecScale in use: %.5f, %.5f (FGMvUnit=%d: %s) | lane scale %.3f, %.3f | "
                 "elapsed %.0f | vector resource %.0fx%.0f | the other unit would be %.5f, %.5f. Said once per arm.",
                 (double)r378_sx, (double)r378_sy, r378_px ? 1 : 0,
                 r378_px ? "pixels at the vector resource" : "the 0.5.1 expression, lane scale x elapsed / extent",
                 (double)r397_x, (double)r397_y, (double)mv_el, (double)mw, (double)mh,
                 (double)(r378_px ? sgn * r397_x * mv_el / mw : sgn * r397_x * mv_el),
                 (double)(r378_px ? sgn * r397_y * mv_el / mh : sgn * r397_y * mv_el));
        mgpu::diag::info(l8);
    }
    p->Set("DLSSG.MvecScaleX", r378_sx);
    p->Set("DLSSG.MvecScaleY", r378_sy);
    p->Set("DLSSG.CameraPinholeOffsetX", 0.0f);
    p->Set("DLSSG.CameraPinholeOffsetY", 0.0f);
    p->Set("DLSSG.CameraPosX", 0.0f); p->Set("DLSSG.CameraPosY", 0.0f); p->Set("DLSSG.CameraPosZ", 0.0f);
    p->Set("DLSSG.CameraUpX", 0.0f);  p->Set("DLSSG.CameraUpY", 1.0f);  p->Set("DLSSG.CameraUpZ", 0.0f);
    p->Set("DLSSG.CameraRightX", 1.0f); p->Set("DLSSG.CameraRightY", 0.0f); p->Set("DLSSG.CameraRightZ", 0.0f);
    p->Set("DLSSG.CameraFwdX", 0.0f); p->Set("DLSSG.CameraFwdY", 0.0f); p->Set("DLSSG.CameraFwdZ", 1.0f);
    p->Set("DLSSG.CameraNear", g_near);
    p->Set("DLSSG.CameraFar", g_far);
    p->Set("DLSSG.CameraFOV", vfov);
    p->Set("DLSSG.CameraAspectRatio", aspect);
    p->Set("DLSSG.ColorBuffersHDR", (unsigned int)(g_i.fmt == DXGI_FORMAT_R16G16B16A16_FLOAT ? 1u : 0u));
    p->Set("DLSSG.DepthInverted", (unsigned int)(g_i.depth_inverted ? 1u : 0u));
    p->Set("DLSSG.CameraMotionIncluded", 1u);
    p->Set("DLSSG.Reset", reset ? 1u : 0u);
    p->Set("DLSSG.AutomodeOverrideReset", 0u);
    p->Set("DLSSG.NotRenderingGameFrames", 0u);
    p->Set("DLSSG.OrthoProjection", 0u);
    p->Set("DLSSG.MvecInvalidValue", 0.0f);
    p->Set("DLSSG.MvecDilated", 0u);
    p->Set("DLSSG.MenuDetectionEnabled", 0u);
    p->Set("DLSSG.BackbufferFrameID", fid);
    if (cam != nullptr)
    {   // R379 (FGCamera=1 and the game stated a camera for this pair): the game's own block replaces the estimate
        // and the identity set above. The pointers are read inside the evaluate call below; `cam` outlives it.
        p->Set("DLSSG.ClipToPrevClip", (void *)cam->clip_to_prev);
        p->Set("DLSSG.PrevClipToClip", (void *)cam->prev_to_clip);
        p->Set("DLSSG.CameraViewToClip", (void *)cam->view_to_clip);
        p->Set("DLSSG.ClipToCameraView", (void *)cam->clip_to_view);
        p->Set("DLSSG.CameraPosX", cam->pos[0]); p->Set("DLSSG.CameraPosY", cam->pos[1]); p->Set("DLSSG.CameraPosZ", cam->pos[2]);
        p->Set("DLSSG.CameraUpX", cam->up[0]);  p->Set("DLSSG.CameraUpY", cam->up[1]);  p->Set("DLSSG.CameraUpZ", cam->up[2]);
        p->Set("DLSSG.CameraRightX", cam->right[0]); p->Set("DLSSG.CameraRightY", cam->right[1]); p->Set("DLSSG.CameraRightZ", cam->right[2]);
        p->Set("DLSSG.CameraFwdX", cam->fwd[0]); p->Set("DLSSG.CameraFwdY", cam->fwd[1]); p->Set("DLSSG.CameraFwdZ", cam->fwd[2]);
        if (cam->have_lens)
        {
            p->Set("DLSSG.CameraNear", cam->cam_near);
            p->Set("DLSSG.CameraFar", cam->cam_far);
            p->Set("DLSSG.CameraFOV", cam->fov);
            p->Set("DLSSG.CameraAspectRatio", cam->aspect);
        }
    }
    set_sub(p, "MVecs", (UINT)mw, (UINT)mh);
    set_sub(p, "Depth", g_i.depth_w != 0u ? g_i.depth_w : g_i.w, g_i.depth_h != 0u ? g_i.depth_h : g_i.h);
    set_sub(p, "InputBackbuffer", g_i.w, g_i.h);
    set_sub(p, "Backbuffer", g_i.w, g_i.h);
    set_sub(p, "OutputInterpolated", g_i.w, g_i.h);
    set_sub(p, "OutputReal", g_i.w, g_i.h);
    return g_eval(cl, g_handle, p, nullptr);
}

void transition(ID3D12GraphicsCommandList *cl, ID3D12Resource *r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER t{};
    t.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    t.Transition.pResource = r; t.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    t.Transition.StateBefore = a; t.Transition.StateAfter = b;
    cl->ResourceBarrier(1, &t);
}

// ---- the pacing thread ----
void wait_until(const LARGE_INTEGER &t0, double ms)
{
    LARGE_INTEGER now{}, fq{};
    QueryPerformanceCounter(&now); QueryPerformanceFrequency(&fq);
    if (fq.QuadPart == 0) return;
    double wait_ms = ms - (double)(now.QuadPart - t0.QuadPart) * 1000.0 / (double)fq.QuadPart;
    if (wait_ms <= 0.2) return;
    if (wait_ms > 30.0) wait_ms = 30.0;
    if (g_timer == nullptr)
    {
        g_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (g_timer == nullptr) g_timer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
    }
    if (g_timer != nullptr)
    {
        LARGE_INTEGER due{}; due.QuadPart = -(LONGLONG)(wait_ms * 10000.0);
        if (SetWaitableTimer(g_timer, &due, 0, nullptr, nullptr, FALSE)) { (void)WaitForSingleObject(g_timer, 40); return; }
    }
    Sleep((DWORD)wait_ms);
}

// R372 / R373 (0.5.1 discovery): present one pair by a rule other than the 0.5.1 one. Called by the pacing
// thread only when job.alt is true. Every frame goes through present_frame exactly like the 0.5.1 block;
// only WHEN each one is shown differs. The rules themselves are fg_pace.hpp (no Windows in them).
bool r372_present_job(const job_t &job, UINT64 &fv)
{
    LARGE_INTEGER fq{};
    QueryPerformanceFrequency(&fq);
    const double k = (fq.QuadPart != 0) ? 1000.0 / (double)fq.QuadPart : 0.0;
    const unsigned n = job.lim ? job.show : job.gen_n;   // in-between frames of this pair to present (R390 / R391: may be 0)
    auto now = [&]() -> double { LARGE_INTEGER q{}; QueryPerformanceCounter(&q); return (double)q.QuadPart * k; };
    auto wait_abs = [&](double t_ms) { LARGE_INTEGER z{}; z.QuadPart = 0; wait_until(z, t_ms); };   // absolute ms on the QPC clock
    auto present = [&](unsigned j) -> bool
    {
        const bool real = (j >= n);
        const unsigned gi = (job.lim && !real) ? (unsigned)job.sel[j < 3u ? j : 2u] : j;   // R390 / R391: the frame chosen for this place
        ID3D12Resource *res = real ? job.set->real : (gi == 0u ? job.set->gen : (gi - 1u < 2u ? job.set->genx[gi - 1u] : nullptr));
        if (res == nullptr) return true;   // not armed for this index: nothing to show, never a failure
        mgpu::gpu1::present_override(res, real ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const bool ok = mgpu::gpu1::present_frame(job.r, job.g, job.b);
        mgpu::gpu1::present_override(nullptr, D3D12_RESOURCE_STATE_COMMON);
        if (ok) (real ? g_pres_real : g_pres_gen).fetch_add(1ull, std::memory_order_relaxed);
        const UINT64 v = mgpu::gpu1::present_last_fence_value(); if (v > fv) fv = v;
        return ok;
    };
    const double t0 = (double)job.t0.QuadPart * k;
    const double ema = (job.ema_ms > 0.0) ? job.ema_ms : 16.0;
    if (k <= 0.0) return mgpu::fgpace::pair_relative(t0, 0.0, n, wait_abs, present);
    // R390 / R391 with the per-pair rule (FGPace 0 / 1): the pair starts at its hand-over, as the 0.5.1 rule does; the
    // frames chosen for it are step_ms apart; with nothing to show the real frame goes out at once.
    if (job.lim && job.pace <= 1) return mgpu::fgpace::pair_relative(t0, (job.pace == 0) ? 0.0 : job.step_ms, n, wait_abs, present);
    if (job.pace == 2)
    {
        double c[4] = {};
        for (unsigned j = 0; j <= n && j < 4u; ++j)
        {
            // R390 / R391: a chosen frame sits at ITS place in the pair (sel + 1 of have + 1), the real frame at the end
            const double ph = (job.lim && j < n) ? (double)(job.sel[j < 3u ? j : 2u] + 1u) / (double)(job.have + 1u)
                                                 : (double)(j + 1u) / (double)(n + 1u);
            c[j] = job.c_real - job.span + job.span * ph;
        }
        double P = ema / (double)(n + 1u);
        if (job.fill && job.step_ms > 0.0) P = job.step_ms;   // R391: under FGFill the frames shown are one game frame apart, whatever the pair's count
        if (job.lim && job.refresh_ms > 0.0 && P < job.refresh_ms) P = job.refresh_ms;   // R390: the tick is never shorter than the display's period
        return mgpu::fgpace::pair_clock(g_clk, P, t0, job.span, c, n + 1u, now, wait_abs, present);
    }
    if (job.pace == 3) return mgpu::fgpace::pair_absolute(g_abs, ema, n, now, wait_abs, present);
    return mgpu::fgpace::pair_relative(t0, (job.pace == 0) ? 0.0 : ema / (double)(n + 1u), n, wait_abs, present);
}

// R372 (FGLive=1 only): FGPace and FGMulti again from mgpu.ini, on the bridge thread, so one session can compare
// the rules. It does not touch g_pace or any 0.5.1 value. FGMulti can only go up to what the arm made textures for.
void r372_read_live()
{
    mgpu::depthsrc::read_live();   // R376: nothing unless DepthSource was in mgpu.ini at init
    wchar_t dir[MAX_PATH];
    if (!module_dir(dir, MAX_PATH)) return;
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%lsmgpu.ini", dir);
    FILE *f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || f == nullptr) return;
    std::vector<char> buf(mgpu::config::MAX_BYTES + 2u, '\0');
    const size_t got = fread(buf.data(), 1, mgpu::config::MAX_BYTES + 1u, f);
    fclose(f);
    if (got > mgpu::config::MAX_BYTES) return;
    buf[got] = '\0';
    const char *pc = mgpu::config::find(buf.data(), got, "FGPace");
    int pm = (pc != nullptr) ? atoi(pc) : 1; if (pm < 0 || pm > 3) pm = 1;
    const char *fm = mgpu::config::find(buf.data(), got, "FGMulti");
    int mu = (fm != nullptr) ? atoi(fm) : 2; if (mu < 2 || mu > 4) mu = 2;
    if ((unsigned)mu > g_mult_armed) mu = (int)g_mult_armed;
    {   // R378: FGMvUnit live, so one session compares the two units
        const char *mu1 = mgpu::config::find(buf.data(), got, "FGMvUnit");
        const int nu = (mu1 != nullptr && atoi(mu1) == 1) ? 1 : 0;
        const int own = g_panel_own.load(std::memory_order_relaxed);   // R392: a key the panel set this session stays the panel's
        if ((own & 1) == 0 && g_mv_unit.exchange(nu, std::memory_order_relaxed) != nu) g_r378_said.store(false, std::memory_order_relaxed);
        const char *sc = mgpu::config::find(buf.data(), got, "FGSkipCarry");   // R384: live, so one session compares
        if ((own & 2) == 0) g_skip_carry.store((sc != nullptr && atoi(sc) == 1) ? 1 : 0, std::memory_order_relaxed);
        const char *dc = mgpu::config::find(buf.data(), got, "FGDisplayCheck");   // R395: live
        g_dcheck.store((dc != nullptr && atoi(dc) == 1) ? 1 : 0, std::memory_order_relaxed);
        const char *wf = mgpu::config::find(buf.data(), got, "FGWarpFill");   // R396: live
        { const int wv = (wf != nullptr) ? atoi(wf) : 2; g_warp_fill.store((wv == 0 || wv == 1) ? wv : 2, std::memory_order_relaxed); }
        const char *sn = mgpu::config::find(buf.data(), got, "FGMvScaleNow");   // R397: live (the panel's switch stays the panel's)
        if ((g_panel_own.load(std::memory_order_relaxed) & 32) == 0) g_scale_now.store((sn != nullptr && atoi(sn) == 1) ? 1 : 0, std::memory_order_relaxed);
        const char *ws = mgpu::config::find(buf.data(), got, "FGWarpShow");     // R397: live
        g_warp_show.store((ws != nullptr && atoi(ws) == 1) ? 1 : 0, std::memory_order_relaxed);
        const char *lm = mgpu::config::find(buf.data(), got, "FGLimit");       // R390: live
        if ((own & 4) == 0) g_limit.store((lm != nullptr && atoi(lm) == 1) ? 1 : 0, std::memory_order_relaxed);
        const char *fl = mgpu::config::find(buf.data(), got, "FGFill");        // R391: live, within the textures the arm made
        if ((own & 8) == 0) { const int fv = (fl != nullptr) ? atoi(fl) : 0; g_fill.store((fv == 1 || fv == 2) ? fv : 0, std::memory_order_relaxed); if (fv == 1 || fv == 2) g_fill_val = fv; }
        const char *fp = mgpu::config::find(buf.data(), got, "FGPhase");       // R401: live
        if ((own & 512) == 0) { const int pv = (fp != nullptr) ? atoi(fp) : 0; const bool po = (pv == 67 || pv == 75); g_phase.store(po ? pv : 0, std::memory_order_relaxed); if (po) g_phase_val = pv; }
        const char *fa = mgpu::config::find(buf.data(), got, "FGPhaseAll");    // R401: live
        g_phase_all.store((fa != nullptr && atoi(fa) == 1) ? 1 : 0, std::memory_order_relaxed);
        const char *sj = mgpu::config::find(buf.data(), got, "FGSkipJump");    // R400: live
        if ((own & 128) == 0) { const int jv = (sj != nullptr) ? atoi(sj) : 0; const bool jo = (jv >= 2 && jv <= 8); g_skip_jump.store(jo ? jv : 0, std::memory_order_relaxed); if (jo) g_skip_jump_val = jv; }
        const char *gk = mgpu::config::find(buf.data(), got, "FGGameClock");   // R400: live
        if ((own & 256) == 0) { const int gv = (gk != nullptr) ? atoi(gk) : 0; const bool go = (gv == 1 || gv == 2); g_gclock.store(go ? gv : 0, std::memory_order_relaxed); if (go) g_gclock_val = gv; }
        const char *ah = mgpu::config::find(buf.data(), got, "FGAhead");       // R399: live (record() starts the generator again at the switch)
        if ((own & 64) == 0) { const int av = (ah != nullptr) ? atoi(ah) : 0; g_ahead.store((av == 1 || av == 2) ? av : 0, std::memory_order_relaxed); if (av == 1 || av == 2) g_ahead_val = av; }
    }
    const int pm_was = g_pace_mode.exchange(pm, std::memory_order_relaxed);
    const unsigned mu_was = g_mult.exchange((unsigned)mu, std::memory_order_relaxed);
    if (pm_was != pm || mu_was != (unsigned)mu)
    {
        char l[300];
        snprintf(l, sizeof l, "[MGPU][R372] FGLive: FGPace %d -> %d, FGMulti %u -> %u (armed for %u). Takes effect on the next pair.",
                 pm_was, pm, mu_was, (unsigned)mu, g_mult_armed);
        mgpu::diag::info(l);
    }
}

// R396 (FrameGen=5), pacing thread: the frames of one pair. All of them are made at once, on the present queue; each
// is then shown at its own time. A newer frame handed over meanwhile is shown instead of the rest of this pair.
bool r396_forward_job(const job_t &job, UINT64 &fv)
{
    if (!mgpu::fgfwd::warp(job.fwd)) return true;   // not made (counted there): the picture keeps what it shows
    for (unsigned i = 0; i < job.fwd_n; ++i)
    {
        if (i > 0u)
        {
            wait_until(job.t0, job.step_ms * (double)i);
            bool newer = false;
            { std::lock_guard<std::mutex> lk(g_mx); newer = g_has_job || g_stop; }
            if (newer) { g_fwd_stopped.fetch_add(1ull, std::memory_order_relaxed); break; }
        }
        ID3D12Resource *const res = mgpu::fgfwd::out(i);
        if (res == nullptr) break;
        mgpu::gpu1::present_override(res, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const bool ok = mgpu::gpu1::present_frame(job.r, job.g, job.b);
        mgpu::gpu1::present_override(nullptr, D3D12_RESOURCE_STATE_COMMON);
        if (!ok) return false;
        g_pres_gen.fetch_add(1ull, std::memory_order_relaxed);
        const UINT64 v = mgpu::gpu1::present_last_fence_value(); if (v > fv) fv = v;
    }
    return true;
}

// R400 (FGGameClock), pacing thread: ms after this job's hand-over of the first game present mark that is not sooner
// than lo_ms after it. The marks are predicted from the last one and the game frame; nothing waits on the game. Below 0:
// no clock to follow (no mark yet, or the game has not presented for four of its frames).
double r400_mark_after(const job_t &job, double lo_ms)
{
    LARGE_INTEGER fq{}, now{};
    QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&now);
    const long long last = g_gm_qpc.load(std::memory_order_relaxed), per = g_gm_period.load(std::memory_order_relaxed);
    if (fq.QuadPart == 0 || last == 0 || per <= 0) return -1.0;
    if (now.QuadPart - last > per * 4) return -1.0;
    const long long lo = job.t0.QuadPart + (long long)(lo_ms * (double)fq.QuadPart / 1000.0);
    long long k = (lo > last) ? (lo - last + per - 1) / per : 0;
    return (double)(last + k * per - job.t0.QuadPart) * 1000.0 / (double)fq.QuadPart;
}

// R399 (FGAhead), pacing thread: the in-between frames that FOLLOW the neural frame this job has just shown. They are
// made by the list ahead_take() ran when the next game frame was picked; each is shown at its place in the hand-over
// interval, or as soon after it as its list has run. A newer neural frame handed over meanwhile is shown instead of
// the rest: the neural frame is never held for an in-between frame.
bool r399_ahead_job(const job_t &job, UINT64 &fv)
{
    const unsigned plan = job.ahead;
    const double ema = (job.ema_ms > 0.0) ? job.ema_ms : 16.0;
    const double step = ema / (double)(plan + 1u);
    const double give_up = ema * 0.85;   // after this the next neural frame is due: nothing is shown in front of it
    auto newer = [&]() -> bool { std::lock_guard<std::mutex> lk(g_mx); return g_has_job || g_stop; };
    LARGE_INTEGER fq{};
    QueryPerformanceFrequency(&fq);
    auto since = [&]() -> double   // ms since this job's hand-over
    {
        LARGE_INTEGER q{}; QueryPerformanceCounter(&q);
        return (fq.QuadPart != 0) ? (double)(q.QuadPart - job.t0.QuadPart) * 1000.0 / (double)fq.QuadPart : 1.0e9;
    };
    for (unsigned i = 0; i < plan; ++i)
    {
        wait_until(job.t0, step * (double)(i + 1u));
        for (;;)
        {
            if (newer()) { g_ah_stopped.fetch_add(1ull, std::memory_order_relaxed); return true; }
            const unsigned made = job.set->ah_n.load(std::memory_order_acquire);
            if (made == AH_NONE || (made != 0u && i >= made)) { if (i == 0u) g_ah_none.fetch_add(1ull, std::memory_order_relaxed); return true; }
            if (made != 0u && mgpu::fgahead::done(job.set->ah_value)) break;   // frame i exists
            const double t = since();
            if (t >= give_up) { g_ah_late.fetch_add(1ull, std::memory_order_relaxed); return true; }
            wait_until(job.t0, t + 0.5);   // not decided or not run yet (the next game frame came late): look again shortly
        }
        ID3D12Resource *const res = (i == 0u) ? job.set->gen : (i - 1u < 2u ? job.set->genx[i - 1u] : nullptr);
        if (res == nullptr) return true;
        mgpu::gpu1::present_override(res, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const bool ok = mgpu::gpu1::present_frame(job.r, job.g, job.b);
        mgpu::gpu1::present_override(nullptr, D3D12_RESOURCE_STATE_COMMON);
        if (!ok) return false;
        g_ah_shown.fetch_add(1ull, std::memory_order_relaxed);
        g_pres_gen.fetch_add(1ull, std::memory_order_relaxed);
        const UINT64 v = mgpu::gpu1::present_last_fence_value(); if (v > fv) fv = v;
    }
    return true;
}

void thread_main()
{
    for (;;)
    {
        job_t job;
        {
            std::unique_lock<std::mutex> lk(g_mx);
            g_cv.wait(lk, [] { return g_has_job || g_stop; });
            if (g_stop) return;
            job = g_job; g_has_job = false;
        }
        bool ok = true;
        UINT64 fv = 0ull;
        if (job.fwd_n != 0u)
        {   // R396 (FrameGen=5): the set is the SOURCE of this pair's frames; its own neural frame was shown at its hand-over
            ok = r396_forward_job(job, fv);
            if (fv > job.set->fence) job.set->fence = fv;
            job.set->fwd.store(false, std::memory_order_release);
            if (!ok)
            {
                mgpu::diag::error("[MGPU][R396] the pacing thread could not present a forward frame - FrameGen=5 stops for this arm; "
                                  "the bridge thread presents the neural frames from the next one.");
                g_thread_failed.store(true, std::memory_order_release);
                return;
            }
            continue;
        }
        if (job.alt) ok = r372_present_job(job, fv);   // R372 / R373: another rule owns this pair (never with the keys absent)
        else {                                         // ---- the 0.5.1 schedule, unchanged ----
        if (job.gen)
        {
            const int r400_gc = g_pace ? g_gclock.load(std::memory_order_relaxed) : 0;   // R400: key absent = this load
            double r400_gen_at = 0.0;
            if (r400_gc == 2)
            {   // the in-between frame on the next game present mark, never later than the landed rule shows the NEURAL frame
                const double m = r400_mark_after(job, 0.0);
                if (m > 0.0 && m <= job.half_ms) { wait_until(job.t0, m); r400_gen_at = m; }
            }
            mgpu::gpu1::present_override(job.set->gen, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ok = mgpu::gpu1::present_frame(job.r, job.g, job.b);
            mgpu::gpu1::present_override(nullptr, D3D12_RESOURCE_STATE_COMMON);
            if (ok) g_pres_gen.fetch_add(1ull, std::memory_order_relaxed);
            const UINT64 v = mgpu::gpu1::present_last_fence_value(); if (v > fv) fv = v;
            double r400_real_at = r400_gen_at + job.half_ms;   // the landed rule (r400_gen_at is 0 unless FGGameClock=2 moved it)
            if (r400_gc != 0)
            {
                const double lo = (r400_gc == 1) ? job.half_ms * 0.5 : r400_gen_at + job.half_ms * 0.6;
                const double hi = (r400_gc == 1) ? job.half_ms : r400_gen_at + job.half_ms * 1.5;
                const double m = r400_mark_after(job, lo);
                if (m >= lo && m <= hi) { r400_real_at = m; g_gc_marks.fetch_add(1ull, std::memory_order_relaxed); }
                else g_gc_free.fetch_add(1ull, std::memory_order_relaxed);
            }
            if (ok && g_pace) wait_until(job.t0, r400_real_at);
        }
        if (ok)
        {
            mgpu::gpu1::present_override(job.set->real, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            ok = mgpu::gpu1::present_frame(job.r, job.g, job.b);
            mgpu::gpu1::present_override(nullptr, D3D12_RESOURCE_STATE_COMMON);
            if (ok) g_pres_real.fetch_add(1ull, std::memory_order_relaxed);
            const UINT64 v = mgpu::gpu1::present_last_fence_value(); if (v > fv) fv = v;
        }
        }                                              // ---- end of the 0.5.1 schedule ----
        if (ok && job.ahead != 0u) ok = r399_ahead_job(job, fv);   // R399 (FGAhead): never with the key absent
        job.set->fence = fv;   // the set is reusable once the present fence passes this (record() checks)
        job.set->busy.store(false, std::memory_order_release);
        if (!ok)
        {
            mgpu::diag::error("[MGPU][R341] the pacing thread could not present a frame - FrameGen=4 stops for this arm; "
                              "the bridge thread presents the real frames from the next one (see the [T5] line above).");
            g_thread_failed.store(true, std::memory_order_release);
            return;
        }
    }
}

void free_sets()
{
    for (unsigned k = 0; k < SETS; ++k)
    {
        if (g_set[k].real != nullptr) { g_set[k].real->Release(); g_set[k].real = nullptr; }
        if (g_set[k].gen != nullptr) { g_set[k].gen->Release(); g_set[k].gen = nullptr; }
        for (unsigned x = 0; x < 2u; ++x) if (g_set[k].genx[x] != nullptr) { g_set[k].genx[x]->Release(); g_set[k].genx[x] = nullptr; }   // R373
        g_set[k].gen_n = 0u; g_set[k].el = 1.0f;
        g_set[k].frame = 0ull; g_set[k].has_gen = false; g_set[k].fence = 0ull; g_set[k].busy.store(false);
        if (g_set[k].dp != nullptr) { g_set[k].dp->Release(); g_set[k].dp = nullptr; }   // R396
        g_set[k].has_dp = false; g_set[k].fwd.store(false);
        g_set[k].seated = false; g_set[k].ah_taken = false; g_set[k].ah_plan = 0u; g_set[k].ah_value = 0ull; g_set[k].ah_n.store(0u);   // R399
    }
}
} // namespace

bool arm(const arm_info &i)
{
    release();
    int fgv = 0; unsigned long long app = 0ull;
    if (!read_keys(fgv, app)) return false;
    if (fgv != 4 && fgv != 3 && fgv != 5)   // key absent / other: the module does not exist (R380: 3 = AMD FSR; R396: 5 = the forward warp)
    {
        // R388: FGInstr=1 without frame generation - only the read-only lines, as the baseline of the same figures
        // (who waits for the stream lock, how long the bridge thread holds it). FGInstr absent: nothing, as before.
        if (g_instr) mgpu::fginstr::arm(true, false);
        return false;
    }
    g_gen = fgv;
    if (i.ndev == nullptr || i.w == 0u || i.h == 0u) return false;
    g_i = i;
    g_tried = g_up = g_failed = false; g_k = 0u; g_fid = g_last_fid = 0ull;
    g_made = g_nomv = g_busy_skips = g_errors = g_evals = 0ull;
    g_pres_gen.store(0); g_pres_real.store(0); g_handoffs.store(0); g_dropped.store(0);
    g_half_sum = 0.0; g_half_n = 0u; g_ema_ms = 0.0; g_last_handoff.QuadPart = 0;
    g_mult_armed = g_mult_req; g_mult.store(g_mult_req, std::memory_order_relaxed);   // R373
    // R391: FGFill=1 in the file at the arm makes the textures for up to three in-between frames per pair (a step
    // of four game frames), whatever FGMulti says; the multiplier in use stays FGMulti. Key absent: nothing changes.
    if (g_fill.load(std::memory_order_relaxed) != 0 && g_mult_armed < 4u) g_mult_armed = 4u;
    g_lim.reset(); g_lim.clear_counts(); g_refresh_ms = mgpu::fginstr::refresh_period_ms(); g_game_ms = 0.0;   // R390
    g_fill_pairs = g_fill_shown = g_fill_alone = 0ull; g_fill_cut = 0ull;
    g_dgate.reset(); g_dgate.clear_counts(); g_dgate_t.QuadPart = 0; g_dgate_real_only = 0ull; g_dgate_ui.store(0);   // R395
    g_now_have = false; g_scale_said_x = g_scale_said_y = 0.0f;   // R397
    g_fwd_posted = g_fwd_noinput = g_fwd_cut = g_fwd_gate = g_fwd_late = 0ull; g_fwd_stopped.store(0ull); g_fwd_said = false;   // R396
    g_r378_said.store(false, std::memory_order_relaxed);   // R378
    g_r379_prev_f = 0ull;   // R379
    g_carry_el = 0.0f; g_carried = 0ull;   // R384
    g_ah_mode_was = 0; g_ah_hist_game = 0ull; g_ah_said_cq = g_ah_said_ext = g_ah_said_up = false;   // R399
    g_jump_pairs = 0ull; g_gc_marks.store(0ull); g_gc_free.store(0ull);   // R400
    g_ph_pairs = g_ph_nofit = 0ull; g_ph_said = false; g_ph_refused = false;   // R401
    g_ah_seats = g_ah_pairs = g_ah_made = g_ah_unseated = g_ah_noinput = g_ah_cut = g_ah_extent = g_ah_busy = g_ah_failed = 0ull;
    g_ah_shown.store(0ull); g_ah_late.store(0ull); g_ah_stopped.store(0ull); g_ah_none.store(0ull);
    mgpu::gpu1::present_override_free(g_present_free);   // R387: absent = false, every paced present takes the stream lock as before
    if (g_present_free)
        mgpu::diag::info("[MGPU][R387] FGPresentFree=1: after the first paced present of this arm, the pacing thread shows its "
                         "frames without taking the stream lock (the frame, its size and its format are the module's own and "
                         "do not change within an arm). Absent: each paced present takes the stream lock twice and queues "
                         "behind the bridge thread while it records its pass. Read the [R371] line (FGInstr=1): the "
                         "presenting thread's wait for the stream lock.");
    g_scene.reset(); g_clk.reset(); g_clk.clear_counts(); g_abs.reset(); g_abs.clear_counts(); g_r372_alt_jobs = 0ull;   // R372 (no pacing thread exists here: release() joined it)
    g_thread_failed.store(false);
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = i.w; d.Height = i.h; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = i.fmt; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    HRESULT h = S_OK;
    for (unsigned k = 0; k < SETS && SUCCEEDED(h); ++k)
    {
        d.Flags = D3D12_RESOURCE_FLAG_NONE;
        h = i.ndev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&g_set[k].real));
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (fgv == 5) continue;   // R396: the forward warp keeps its frames in its own module; a set holds the neural frame only
        if (SUCCEEDED(h)) h = i.ndev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&g_set[k].gen));
        for (unsigned x = 0; x + 1u < g_mult_armed - 1u && SUCCEEDED(h); ++x)   // R373: in-between frames 2..3, only with FGMulti 3 / 4 (absent: none)
            h = i.ndev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&g_set[k].genx[x]));
    }
    if (FAILED(h)) { free_sets(); char l[200]; snprintf(l, sizeof l, "the frame buffers could not be made (0x%08X)", (unsigned)h); fail_for_arm(l); return false; }
    g_pfence = mgpu::gpu1::present_fence_addref();
    g_armed = true;
    g_on.store(true, std::memory_order_release);
    mgpu::fginstr::arm(g_instr);   // R371: FGInstr absent = one store, nothing else
    if (g_mult_armed != 2u || g_pace_mode.load(std::memory_order_relaxed) >= 2 || g_live || g_instr)
    {
        char rl[520];
        snprintf(rl, sizeof rl,
                 "[MGPU][R372] frame generation discovery keys: FGMulti=%u (%u in-between texture(s) per set, %.0f MB more than 2x) "
                 "FGPace=%d (0 no wait, 1 per pair = the 0.5.1 rule at 2x, 2 clock on scene time, 3 absolute clock - compare only) "
                 "FGLive=%d FGInstr=%d. All absent = the 0.5.1 path.",
                 g_mult_armed, g_mult_armed - 1u,
                 (double)((unsigned long long)(g_mult_armed - 2u) * SETS * (unsigned long long)i.w * i.h * (i.fmt == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8ull : 4ull)) / (1024.0 * 1024.0),
                 g_pace_mode.load(std::memory_order_relaxed), g_live ? 1 : 0, g_instr ? 1 : 0);
        mgpu::diag::info(rl);
    }
    if (g_ahead.load(std::memory_order_relaxed) != 0)
    {
        char al[640];
        if (fgv == 5)
            snprintf(al, sizeof al, "[MGPU][R399] FGAhead=%d is in mgpu.ini, but it belongs to FrameGen=3 and 4. FrameGen=5 does not use it.",
                     g_ahead.load(std::memory_order_relaxed));
        else
            snprintf(al, sizeof al, "[MGPU][R399] FGAhead=%d: the neural frame is shown the moment it finishes; the in-between frame(s) "
                     "are made from it and the game frame the neural card picks next, at the pick, and shown during that frame's "
                     "evaluate. The game frame itself is never shown. 1 = the neural frame starts the generator again, 2 = it is "
                     "handed as a frame with no motion. Needs CopyQueue=1 or 2. FGPace, FGFill and FGLimit do not act on these pairs. "
                     "Live with FGLive=1 and in the panel.", g_ahead.load(std::memory_order_relaxed));
        mgpu::diag::info(al);
    }
    char l[400];
    snprintf(l, sizeof l, "[MGPU][R341] FrameGen=%d armed: %u sets of %ux%u fmt=%u on the neural card (%.0f MB), FGFov=%.0f FGNear=%.3f FGFar=%.0f FGInvertMV=%d FGPace=%d. "
             "The %s is created on the first NR frame.", g_gen, SETS, i.w, i.h, (unsigned)i.fmt,
             (double)(2ull * SETS * (unsigned long long)i.w * i.h * (i.fmt == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8ull : 4ull)) / (1024.0 * 1024.0),
             (double)g_fov, (double)g_near, (double)g_far, g_invert ? 1 : 0, g_pace ? 1 : 0,
             g_gen == 3 ? "FSR frame-generation context" : (g_gen == 5 ? "forward warp pass" : "DLSS-G feature"));   // R380 / R396: the last argument; "DLSS-G feature" with FrameGen=4, as before
    mgpu::diag::info(l);
    return true;
}

bool on()
{
    return g_on.load(std::memory_order_relaxed);
}

// R395, bridge thread, once per evaluated neural frame. Returns 0 (no say yet / display period not known),
// 1 (frame generation off) or the multiplier allowed.
static unsigned r395_step()
{
    LARGE_INTEGER now{}, fq{};
    QueryPerformanceCounter(&now); QueryPerformanceFrequency(&fq);
    const double dt = (g_dgate_t.QuadPart != 0 && fq.QuadPart != 0)
                    ? (double)(now.QuadPart - g_dgate_t.QuadPart) * 1000.0 / (double)fq.QuadPart : 0.0;
    g_dgate_t = now;
    const unsigned asked = g_mult.load(std::memory_order_relaxed);
    if (g_dgate.step(dt, g_refresh_ms, asked))
    {
        const unsigned a = g_dgate.allowed(asked);
        const double disp = (g_refresh_ms > 0.0) ? 1000.0 / g_refresh_ms : 0.0, r = g_dgate.rate();
        char l[420];
        if (a < 2u)
            snprintf(l, sizeof l, "[MGPU][R395] FGDisplayCheck: the display shows %.1f frames a second and the neural card makes %.1f; "
                     "2x would be %.1f. Frame generation is OFF until that fits: every neural frame is shown as it is.", disp, r, 2.0 * r);
        else
            snprintf(l, sizeof l, "[MGPU][R395] FGDisplayCheck: the display shows %.1f frames a second and the neural card makes %.1f. "
                     "Frame generation at %ux (FGMulti asks %ux)%s.", disp, r, a, asked,
                     g_dgate.trying ? " - a try: the next check, made with the generator running, keeps it or not" : "");
        mgpu::diag::info(l);
        g_dgate_ui_disp.store((int)(disp + 0.5), std::memory_order_relaxed);
        g_dgate_ui_would.store((int)(2.0 * r + 0.5), std::memory_order_relaxed);
        g_dgate_ui.store((a < 2u) ? 1 : ((a < asked) ? (int)a : 0), std::memory_order_relaxed);
    }
    if (g_dgate.m == 0u) return 0u;
    return g_dgate.allowed(asked);
}

// R399 (FGAhead), bridge thread, in record(): this neural frame becomes the generator's previous frame (the "seat"),
// so the call ahead_take() makes for the NEXT picked game frame gives the frame between this neural frame and that
// game frame. Nothing of this call is shown. mode 1: a reset - the generator starts again from this frame. mode 2:
// an ordinary frame with no motion since the game frame the generator holds, which is true only when that game
// frame is this very frame (the picture moved by nothing, only its look changed); otherwise a reset as well.
static void r399_seat(ID3D12GraphicsCommandList *cl, set_t &st, ID3D12Resource *mvec, ID3D12Resource *depth, float mv_el,
               unsigned long long f, unsigned r395_m, int mode)
{
    g_r379_prev_f = f;
    g_last_fid = 0ull;   // the landed path starts from a reset when the key is switched off
    if (r395_m == 1u) { ++g_dgate_real_only; return; }             // R395: the display cannot show 2x - the neural frame alone
    if (mvec == nullptr || depth == nullptr) { ++g_nomv; return; }   // the generator cannot be called: the neural frame alone
    unsigned n = g_mult.load(std::memory_order_relaxed) - 1u;
    if (g_mfc_max != 0u && n > g_mfc_max) n = g_mfc_max;
    if (r395_m >= 2u && n > r395_m - 1u) n = r395_m - 1u;
    if (n < 1u) n = 1u;
    if (n > 3u) n = 3u;
    while (n > 1u && st.genx[n - 2u] == nullptr) --n;   // only as many as the arm made textures for
    const unsigned long long fid = ++g_fid;
    const bool reset = (mode != 2) || (g_ah_hist_game != f);
    const float el = reset ? mv_el : 0.0f;
    bool ok = false;
    unsigned code = 0u;
    if (g_gen == 3)
    {
        mgpu::fgfsr::set_scale(r397_scale_x(), r397_scale_y());
        ID3D12Resource *const outs[3] = { st.gen, st.genx[0], st.genx[1] };
        code = mgpu::fgfsr::evaluate(cl, st.real, mvec, depth, outs, 1u, el, reset, nullptr);
        ok = (code == 0u);
    }
    else
    {
        const NVSDK_NGX_Result r = evaluate(cl, st.real, mvec, depth, st.gen, el, reset, fid, 1u, 1u, nullptr);
        code = (unsigned)r;
        ok = (r == NVSDK_NGX_Result_Success);
    }
    ++g_evals;
    g_ah_hist_game = 0ull;   // whatever happened, the generator no longer holds a game frame of ours
    if (!ok)
    {
        ++g_errors;
        char l[320];
        snprintf(l, sizeof l, "[MGPU][R399] frame %llu: the generator did not take the neural frame (0x%08X, %llu error(s) so far)%s", f, code, g_errors,
                 g_errors >= 3ull ? " - frame generation OFF for this arm." : ". No in-between frame follows it.");
        mgpu::diag::error(l);
        if (g_errors >= 3ull) { g_on.store(false, std::memory_order_release); g_failed = true; }
        return;
    }
    st.seated = true; st.ah_plan = n; ++g_ah_seats;
}

void record(ID3D12GraphicsCommandList *cl, ID3D12Resource *nr_final, ID3D12Resource *mvec, ID3D12Resource *depth,
            float mv_el, unsigned long long f)
{
    if (!on() || cl == nullptr || nr_final == nullptr) return;
    if (g_thread_failed.load(std::memory_order_acquire)) { g_on.store(false); return; }
    // R395 (FGDisplayCheck=1): counted here, before the busy-set skip, so every evaluated neural frame is seen.
    unsigned r395_m = 0u;   // 0 = no say, 1 = this frame is shown alone (no generator call), 2.. = in-between count ceiling + 1
    if (g_dcheck.load(std::memory_order_relaxed) != 0) r395_m = r395_step();
    else if (g_dgate.m != 0u) { g_dgate.reset(); g_dgate_t.QuadPart = 0; g_dgate_ui.store(0, std::memory_order_relaxed); }   // switched off live: as asked again
    set_t &st = g_set[g_k];
    // FIX7/FIX8 lesson: a set the pacing thread holds, or whose last present the present
    // fence has not passed, is not rewritten. The frame is skipped instead (handoff then
    // drops it: the picture keeps the previous pair). Counted. No fence = no proof = skip.
    if (g_pfence == nullptr) g_pfence = mgpu::gpu1::present_fence_addref();
    if (g_pfence == nullptr || st.busy.load(std::memory_order_acquire) || (st.fence != 0ull && g_pfence->GetCompletedValue() < st.fence)
        || (g_gen == 5 && st.fwd.load(std::memory_order_acquire)))   // R396: the set is still being pushed forward
    {
        ++g_busy_skips;
        // R384: this evaluated frame is never shown and the generator never sees it; its game frames belong to the
        // next pair. Key absent: one relaxed load, nothing kept.
        if (g_skip_carry.load(std::memory_order_relaxed) != 0 && g_carry_el < 64.0f) g_carry_el += mv_el;
        return;
    }
    if (g_carry_el != 0.0f)
    {   // R384: only ever non-zero with FGSkipCarry=1, after a skip
        if (g_skip_carry.load(std::memory_order_relaxed) != 0)
        {
            mv_el += g_carry_el;
            if (++g_carried <= 3ull)
            {
                char cl2[260];
                snprintf(cl2, sizeof cl2, "[MGPU][R384] FGSkipCarry: frame %llu follows %.0f game frame(s) of skipped evaluated frame(s); "
                         "its pair is handed %.0f elapsed frame(s). Logged three times, then counted.", f, (double)g_carry_el, (double)mv_el);
                mgpu::diag::info(cl2);
            }
        }
        g_carry_el = 0.0f;
    }

    if (!g_up && !ensure_feature(cl)) return;   // turned the key off for the arm on failure

    // real = copy of nr_final (UAV -> COPY_SOURCE -> UAV; NPSR -> COPY_DEST -> NPSR)
    transition(cl, nr_final, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(cl, st.real, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    cl->CopyResource(st.real, nr_final);
    transition(cl, st.real, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(cl, nr_final, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    st.frame = f; st.has_gen = false; st.handed = false; st.gen_n = 0u; st.el = mv_el;
    st.fill = false; st.fill_n = 0u;   // R391
    st.phase = 0.0f;                   // R401

    if (g_gen == 5)
    {
        // R396: no generator call here. This neural frame's forward frames are made when the NEXT game frame is taken
        // (forward_take), from that frame's vectors. What is kept now is this frame's depth: the stream's depth texture
        // is written again by a later frame before the warp reads it, so the set holds its own copy.
        st.has_dp = false;
        if (depth != nullptr)
        {
            if (st.dp == nullptr)
            {
                D3D12_RESOURCE_DESC dd = depth->GetDesc();
                dd.Flags = D3D12_RESOURCE_FLAG_NONE;
                D3D12_HEAP_PROPERTIES dhp{}; dhp.Type = D3D12_HEAP_TYPE_DEFAULT;
                if (FAILED(g_i.ndev->CreateCommittedResource(&dhp, D3D12_HEAP_FLAG_NONE, &dd, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                             nullptr, IID_PPV_ARGS(&st.dp))))
                    st.dp = nullptr;   // no copy: the warp runs without the depth test (counted in its summary)
            }
            if (st.dp != nullptr)
            {
                transition(cl, depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
                transition(cl, st.dp, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
                cl->CopyResource(st.dp, depth);
                transition(cl, st.dp, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                transition(cl, depth, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                st.has_dp = true;
            }
        }
        g_k = (g_k + 1u) % SETS;
        return;
    }

    // R399 (FGAhead). The set starts with nothing planned to follow it, whatever the key says (plain stores).
    st.seated = false; st.ah_taken = false; st.ah_plan = 0u; st.ah_value = 0ull; st.ah_n.store(0u, std::memory_order_release);
    {
        const int r399_mode = g_ahead.load(std::memory_order_relaxed);   // key absent: this load, and nothing below runs
        if (r399_mode != g_ah_mode_was)
        {   // switched (live or by the panel): what the generator holds belongs to the other order of frames
            char al[220];
            snprintf(al, sizeof al, "[MGPU][R399] FGAhead %d -> %d at frame %llu: the generator starts again from this frame.", g_ah_mode_was, r399_mode, f);
            mgpu::diag::info(al);
            g_ah_mode_was = r399_mode; g_last_fid = 0ull; g_ah_hist_game = 0ull;
        }
        if (r399_mode != 0)
        {
            r399_seat(cl, st, mvec, depth, mv_el, f, r395_m, r399_mode);
            g_k = (g_k + 1u) % SETS;
            return;
        }
    }

    if (r395_m == 1u)
    {
        ++g_dgate_real_only;   // R395: the display cannot show 2x - the real frame alone, no generator call; the next pair resets
        g_last_fid = 0ull;
    }
    else if (mvec == nullptr || depth == nullptr)
    {
        ++g_nomv;   // no vectors (or depth) this frame: the real frame alone; the next pair resets
        g_last_fid = 0ull;
    }
    else
    {
        const unsigned long long fid = ++g_fid;
        // R400 (FGSkipJump=N): a pair of N or more game frames gets no in-between frame - the generator takes the frame
        // as a reset, exactly like a first frame, and handoff() shows it at once. Key absent: one relaxed load.
        const int r400_n = g_skip_jump.load(std::memory_order_relaxed);
        const bool r400_jump = (r400_n != 0) && (mv_el >= (float)r400_n);
        if (r400_jump) ++g_jump_pairs;
        const bool reset = (g_last_fid == 0ull) || (fid != g_last_fid + 1ull) || r400_jump;
        g_last_fid = fid;
        // R373: in-between frames wanted for this pair. 1 (FGMulti absent) = the 0.5.1 call, 1 of 1.
        unsigned r373_n = g_mult.load(std::memory_order_relaxed) - 1u;
        if (g_mfc_max != 0u && r373_n > g_mfc_max) r373_n = g_mfc_max;   // the driver's ceiling when it reports one (0 = not reported: try)
        if (r395_m >= 2u && r373_n > r395_m - 1u) r373_n = r395_m - 1u;   // R395: no more than the display can show
        if (r373_n < 1u) r373_n = 1u;
        // R391 (FGFill=1): in-between frames = the game frames this evaluated frame skipped (mv_el - 1, the R87
        // count, with R384's carry in it). A step of one game frame is still evaluated ONCE, so the generator's
        // own history stays in sequence; that frame is not shown. Key absent: one relaxed load.
        const int r391_mode = g_fill.load(std::memory_order_relaxed);
        if (r391_mode != 0)
        {
            unsigned r391_cap = g_mult_armed - 1u;                                  // what the arm made textures for
            if (g_mfc_max != 0u && r391_cap > g_mfc_max) r391_cap = g_mfc_max;      // the driver's ceiling when it reports one
            if (r391_cap > 3u) r391_cap = 3u;
            if (r395_m >= 2u && r391_cap > r395_m - 1u) r391_cap = r395_m - 1u;   // R395
            st.fill = true;
            st.fill_n = mgpu::fgpace::fill_count(mv_el, r391_cap);
            if (r391_mode == 2)
            {   // FGFill=2: never fewer than FGMulti asks for (a step of one game frame keeps its in-between frame,
                // which a display faster than the game has room for; FGLimit holds it back where it has not)
                unsigned r391_floor = g_mult.load(std::memory_order_relaxed) - 1u;
                if (r391_floor > r391_cap) r391_floor = r391_cap;
                if (st.fill_n < r391_floor) st.fill_n = r391_floor;
            }
            r373_n = (st.fill_n < 1u) ? 1u : st.fill_n;
        }
        // R379: the game's camera across this pair (previous evaluated frame -> this one). FGCamera absent: one
        // relaxed load and the null pointer, i.e. the 0.5.1 call.
        mgpu::slcam::camera r379_cam;
        const bool r379_have = mgpu::slcam::on() && !reset && mgpu::slcam::pair(g_r379_prev_f, f, r379_cam);
        const mgpu::slcam::camera *const r379_p = r379_have ? &r379_cam : nullptr;
        g_r379_prev_f = f;
        if (g_gen == 3)
        {   // R380: AMD FSR - one dispatch makes every in-between frame of the pair
            mgpu::fgfsr::set_scale(r397_scale_x(), r397_scale_y());   // R397: the arm's copy (the value it already holds) unless FGMvScaleNow=1
            unsigned fn = r373_n > 3u ? 3u : r373_n;
            while (fn > 1u && st.genx[fn - 2u] == nullptr) --fn;   // only as many as the arm made textures for
            ID3D12Resource *const outs[3] = { st.gen, st.genx[0], st.genx[1] };
            const unsigned rc = mgpu::fgfsr::evaluate(cl, st.real, mvec, depth, outs, fn, mv_el, reset, r379_p);
            ++g_evals;
            if (rc != 0u)
            {
                ++g_errors;
                char l[300];
                snprintf(l, sizeof l, "[MGPU][R380] frame %llu: FSR frame generation returned %u (%llu error(s) so far)%s", fid, rc, g_errors,
                         g_errors >= 3ull ? " - FrameGen=3 OFF for this arm." : ". This pair shows the real frame only.");
                mgpu::diag::error(l);
                if (g_errors >= 3ull) { g_on.store(false, std::memory_order_release); g_failed = true; }
            }
            else if (!reset) { st.has_gen = true; st.gen_n = fn; g_made += fn; }   // a reset frame has no history: real only
        }
        else
        {   // ---- FrameGen=4: the DLSS-G evaluates, unchanged ----
        // R401 (FGPhase): the one in-between frame of the pair is asked for at two thirds / three quarters of it - frame
        // N of N. Only for the plain 2x pair (FGMulti 2, no FGFill, not a reset). Key absent: one relaxed load and the
        // 0.5.1 call, 1 of r373_n.
        unsigned r401_c = r373_n, r401_i = 1u;
        bool r401_reset = reset;
        {
            const int r401_ph = g_phase.load(std::memory_order_relaxed);
            if (r401_ph != 0 && r373_n == 1u && r391_mode == 0 && !reset && !g_ph_refused)
            {
                const unsigned want = (r401_ph == 67) ? 2u : 3u;
                if (g_mfc_max != 0u && want > g_mfc_max)
                {
                    ++g_ph_nofit;
                    if (!g_ph_said)
                    {
                        g_ph_said = true;
                        char pl[260];
                        snprintf(pl, sizeof pl, "[MGPU][R401] FGPhase=%d needs frame %u of %u from the generator and the driver reports %u at most. "
                                 "The pair keeps its middle frame. Said once; counted.", r401_ph, want, want, g_mfc_max);
                        mgpu::diag::warn(pl);
                    }
                }
                else
                {
                    r401_c = want; r401_i = want;
                    if (g_phase_all.load(std::memory_order_relaxed) != 0)
                        for (unsigned q = 1u; q < want; ++q)   // FGPhaseAll=1: the earlier frames in order, into the same texture; the last one stays
                        {
                            (void)evaluate(cl, st.real, mvec, depth, st.gen, mv_el, false, fid, want, q, r379_p);
                            ++g_evals;
                        }
                }
            }
        }
        const NVSDK_NGX_Result r = evaluate(cl, st.real, mvec, depth, st.gen, mv_el, r401_reset, fid, r401_c, r401_i, r379_p);
        if (r != NVSDK_NGX_Result_Success && r401_i != 1u)
        {   // R401: the generator would not make that frame. Not asked again in this arm; the pairs go back to the middle frame.
            g_ph_refused = true;
            char pr[300];
            snprintf(pr, sizeof pr, "[MGPU][R401] the generator returned 0x%08X for frame %u of %u (FGPhaseAll=%d). FGPhase is not used again in "
                     "this arm; the pairs keep their middle frame. Try FGPhaseAll=1 if it was 0.", (unsigned)r, r401_i, r401_c,
                     g_phase_all.load(std::memory_order_relaxed));
            mgpu::diag::warn(pr);
        }
        ++g_evals;
        if (r != NVSDK_NGX_Result_Success)
        {
            ++g_errors;
            char l[300];
            snprintf(l, sizeof l, "[MGPU][R341] frame %llu: EvaluateFeature(FrameGeneration) 0x%08X (%llu error(s) so far)%s", fid, (unsigned)r, g_errors,
                     g_errors >= 3ull ? " - FrameGen=4 OFF for this arm." : ".");
            mgpu::diag::error(l);
            if (g_errors >= 3ull) { g_on.store(false, std::memory_order_release); g_failed = true; }
        }
        else if (!reset)   // a reset frame has no history: real only
        {
            st.has_gen = true; ++g_made; st.gen_n = 1u;
            if (r401_i != 1u) { st.phase = (float)r401_i / (float)(r401_c + 1u); ++g_ph_pairs; }   // R401
            for (unsigned x = 1u; x < r373_n && x - 1u < 2u && st.genx[x - 1u] != nullptr; ++x)   // R373: frames 2..n of n
            {
                const NVSDK_NGX_Result rx = evaluate(cl, st.real, mvec, depth, st.genx[x - 1u], mv_el, false, fid, r373_n, x + 1u, r379_p);
                ++g_evals;
                if (rx != NVSDK_NGX_Result_Success)
                {
                    ++g_errors;
                    char lx[300];
                    snprintf(lx, sizeof lx, "[MGPU][R373] frame %llu: EvaluateFeature(FrameGeneration) in-between %u of %u 0x%08X (%llu error(s) so far)%s", fid, x + 1u, r373_n, (unsigned)rx, g_errors,
                             g_errors >= 3ull ? " - FrameGen=4 OFF for this arm." : ".");
                    mgpu::diag::error(lx);
                    if (g_errors >= 3ull) { g_on.store(false, std::memory_order_release); g_failed = true; }
                    break;
                }
                st.gen_n = x + 1u; ++g_made;
            }
        }
        }   // ---- end of FrameGen=4 ----
    }
    g_k = (g_k + 1u) % SETS;
}

// R399 (FGAhead): the counts, every 1200 hand-overs and at release. Bridge thread.
static void r399_line(const char *when, unsigned long long n)
{
    char ws[220], l[900];
    mgpu::fgahead::summary(ws, sizeof ws);
    snprintf(l, sizeof l, "[MGPU][R399] FGAhead=%d %s %llu handoffs: neural frames handed to the generator %llu | in-between frames "
             "made %llu for %llu neural frame(s), shown %llu | not made: the neural frame was not handed to the generator %llu, no vectors / "
             "depth / copy-queue fence %llu, a cut (more than 8 game frames) %llu, the game frame is not the neural frame's size or format "
             "%llu, the list was busy %llu, the generator or the list failed %llu | not shown: nothing was made %llu, its list had not "
             "run in time %llu, a newer neural frame came first %llu | %s.",
             g_ahead.load(std::memory_order_relaxed), when, n, g_ah_seats, g_ah_made, g_ah_pairs, g_ah_shown.load(), g_ah_unseated, g_ah_noinput,
             g_ah_cut, g_ah_extent, g_ah_busy, g_ah_failed, g_ah_none.load(), g_ah_late.load(), g_ah_stopped.load(), ws);
    mgpu::diag::info(l);
}

bool handoff(float r, float g, float b)
{
    // R341a (review F6): once the pacing thread exists, EVERY frame is the module's until stop() - also
    // after the feature failed and g_on went false - so present_frame never has two callers. A thread
    // that failed has exited: the bridge presents again, alone.
    if (g_thread_failed.load(std::memory_order_acquire)) { g_on.store(false); return false; }
    if (!on()) { if (g_thread != nullptr) { g_dropped.fetch_add(1ull); return true; } return false; }
    // the set record() wrote last
    const unsigned k = (g_k + SETS - 1u) % SETS;
    set_t &st = g_set[k];
    // Before the pacing thread exists the bridge thread presents as today. Once it exists, EVERY
    // frame is this module's: a frame record() skipped is dropped here (counted), never presented
    // by the bridge thread beside the pacing thread (present_frame has one caller at a time).
    const bool started = (g_thread != nullptr);
    if (st.frame == 0ull || st.handed) { if (started) g_dropped.fetch_add(1ull); return started; }
    st.handed = true;
    LARGE_INTEGER now{}, fq{};
    QueryPerformanceCounter(&now); QueryPerformanceFrequency(&fq);
    if (g_last_handoff.QuadPart != 0 && fq.QuadPart != 0)
    {
        const double dt = (double)(now.QuadPart - g_last_handoff.QuadPart) * 1000.0 / (double)fq.QuadPart;
        if (dt > 3.0 && dt < 60.0) g_ema_ms = (g_ema_ms <= 0.0) ? dt : (g_ema_ms * 0.85 + dt * 0.15);
    }
    const double r372_dt = (g_last_handoff.QuadPart != 0 && fq.QuadPart != 0)
                         ? (double)(now.QuadPart - g_last_handoff.QuadPart) * 1000.0 / (double)fq.QuadPart : 0.0;   // R371 / R372: this hand-over's interval
    g_last_handoff = now;
    job_t job; job.set = &st; job.gen = st.has_gen; job.r = r; job.g = g; job.b = b; job.t0 = now;
    g_fwd_rgb[0] = r; g_fwd_rgb[1] = g; g_fwd_rgb[2] = b;   // R396: plain stores; read only by forward_take on this thread
    job.half_ms = (g_ema_ms > 0.0) ? g_ema_ms * 0.5 : 8.0;
    job.ahead = st.seated ? st.ah_plan : 0u;   // R399 (FGAhead): 0 with the key absent
    if (st.phase > 0.0f && st.phase < 1.0f)    // R401 (FGPhase): the neural frame waits the REST of the interval behind its in-between frame
        job.half_ms = ((g_ema_ms > 0.0) ? g_ema_ms : 16.0) * (double)(1.0f - st.phase);
    {   // R371 / R372 / R373. With every key absent: alt stays false and the job is the 0.5.1 job.
        mgpu::fginstr::note_handoff(r372_dt);   // one relaxed load when FGInstr is absent
        if (g_live && (g_handoffs.load(std::memory_order_relaxed) % 120ull) == 119ull) r372_read_live();
        const int pm = g_pace_mode.load(std::memory_order_relaxed);
        job.gen_n = st.has_gen ? (st.gen_n != 0u ? st.gen_n : 1u) : 0u;
        job.pace = pm; job.ema_ms = g_ema_ms;
        job.alt = job.gen && (pm >= 2 || job.gen_n > 1u || (pm == 0 && g_pace));
        if (pm == 2) g_scene.next((double)now.QuadPart * ((fq.QuadPart != 0) ? 1000.0 / (double)fq.QuadPart : 0.0), r372_dt, (double)st.el, job.c_real, job.span);
        // R395: the display's period again every 600 hand-overs (here: no lock is held)
        if (g_dcheck.load(std::memory_order_relaxed) != 0 && (g_handoffs.load(std::memory_order_relaxed) % 600ull) == 598ull)
            g_refresh_ms = mgpu::fginstr::refresh_period_ms();
        // R390 (FGLimit) / R391 (FGFill). Both absent: lim stays false and nothing below runs.
        const bool r390_on = g_limit.load(std::memory_order_relaxed) != 0;
        if (r390_on || st.fill)
        {
            const unsigned long long hn = g_handoffs.load(std::memory_order_relaxed);
            if (r390_on && (hn % 600ull) == 599ull) g_refresh_ms = mgpu::fginstr::refresh_period_ms();   // the display may have changed
            if (r372_dt > 3.0 && r372_dt < 250.0 && st.el >= 1.0f)
            {   // one game frame in ms, followed slowly
                const double gm = r372_dt / (double)st.el;
                g_game_ms = (g_game_ms <= 0.0) ? gm : g_game_ms + (gm - g_game_ms) / 16.0;
            }
            const unsigned have = job.gen_n;                                    // in-between frames this pair holds
            const unsigned want = st.fill ? (st.fill_n < have ? st.fill_n : have) : have;
            // the pair's own time: with FGFill the game frames it covers (the R87 count), otherwise the hand-over interval
            const unsigned r391_el = (st.el >= 1.5f) ? (unsigned)(st.el + 0.5f) : 1u;
            const double pair_ms = (st.fill && g_game_ms > 0.0) ? g_game_ms * (double)r391_el
                                                                : ((g_ema_ms > 0.0) ? g_ema_ms : 16.0);
            const double step = pair_ms / (double)(want + 1u);                  // the pair's frames evenly over it
            unsigned fit = want, cut = 0u;
            double hold = step * (double)want;                                  // how long after the hand-over the real frame is shown
            if (st.fill)
            {
                // R391b (Marcelo 03:59): the pair starts at its hand-over and the real frame is never shown later than
                // the landed rule shows it (half the hand-over interval). In-between frames that do not fit before
                // that are not shown, the oldest first.
                const double cap = (g_ema_ms > 0.0) ? g_ema_ms * 0.5 : 8.0;
                fit = mgpu::fgpace::fill_fit(cap, step, want);
                cut = want - fit; g_fill_cut += (unsigned long long)cut;
                hold = step * (double)fit; if (hold > cap) hold = cap;
            }
            unsigned show = fit;
            if (r390_on) show = g_lim.allow(r372_dt > 0.0 ? r372_dt : g_ema_ms, g_refresh_ms, fit);
            if (show > 3u) show = 3u;
            job.lim = true; job.fill = st.fill; job.have = have; job.show = show; job.refresh_ms = r390_on ? g_refresh_ms : 0.0;
            for (unsigned q = 0; q < show; ++q) job.sel[q] = (unsigned char)(cut + mgpu::fgpace::limit_pick(fit, show, q));
            // without FGFill: the frames shown evenly over the hand-over interval, as the 0.5.1 rule does it (half at 2x)
            job.step_ms = st.fill ? ((show != 0u) ? hold / (double)show : 0.0) : pair_ms / (double)(show + 1u);
            job.alt = true;   // also a pair with nothing to show: its real frame goes out at once
            if (st.fill) { ++g_fill_pairs; g_fill_shown += show; if (want == 0u) ++g_fill_alone; }
        }
        if (job.alt) ++g_r372_alt_jobs;
    }
    g_half_sum += job.half_ms; ++g_half_n;
    st.busy.store(true, std::memory_order_release);
    {
        std::unique_lock<std::mutex> lk(g_mx);
        if (g_thread == nullptr) { g_stop = false; g_thread = new std::thread(thread_main); }
        if (g_has_job)
        {   // the newest wins. R396: a waiting forward job gives its set back through its own flag
            if (g_job.fwd_n != 0u) g_job.set->fwd.store(false, std::memory_order_release);
            else g_job.set->busy.store(false, std::memory_order_release);
            g_dropped.fetch_add(1ull);
        }
        g_job = job; g_has_job = true;
    }
    g_cv.notify_one();
    const unsigned long long n = g_handoffs.fetch_add(1ull, std::memory_order_relaxed) + 1ull;
    if (n % 1200ull == 0ull)
    {
        char l[520];
        snprintf(l, sizeof l, "[MGPU][R341] FrameGen=%d after %llu handoffs: generated %llu | presented gen %llu real %llu | dropped (thread busy) %llu | "
                 "skipped: set not retired %llu, no vectors/depth %llu | evaluate errors %llu | half interval mean %.2f ms (NR interval %.2f ms)",
                 g_gen, n, g_made, g_pres_gen.load(), g_pres_real.load(), g_dropped.load(), g_busy_skips, g_nomv, g_errors,
                 g_half_n ? g_half_sum / g_half_n : 0.0, g_ema_ms);
        mgpu::diag::info(l);
        if (g_ah_seats != 0ull) r399_line("after", n);   // R399
        if (g_phase.load(std::memory_order_relaxed) != 0 || g_ph_pairs != 0ull)
        {   // R401
            char l5[420];
            snprintf(l5, sizeof l5, "[MGPU][R401] after %llu handoffs: FGPhase=%d FGPhaseAll=%d - pairs whose in-between frame was made late in the "
                     "pair and whose neural frame waited the rest %llu | pairs kept at the middle because the driver does not make that many "
                     "frames %llu | this hand-over's wait %.2f ms of a %.2f ms interval.", n, g_phase.load(std::memory_order_relaxed),
                     g_phase_all.load(std::memory_order_relaxed), g_ph_pairs, g_ph_nofit, job.half_ms, g_ema_ms);
            mgpu::diag::info(l5);
        }
        if (g_skip_jump.load(std::memory_order_relaxed) != 0 || g_gclock.load(std::memory_order_relaxed) != 0 || g_jump_pairs != 0ull || g_gc_marks.load() != 0ull)
        {   // R400
            LARGE_INTEGER fq4{}; QueryPerformanceFrequency(&fq4);
            char l4[520];
            snprintf(l4, sizeof l4, "[MGPU][R400] after %llu handoffs: FGSkipJump=%d - pairs shown as the neural frame alone because they span that "
                     "many game frames or more %llu | FGGameClock=%d - neural frames shown on a game present mark %llu, by the landed rule "
                     "(no mark in reach) %llu | the game's frame %.2f ms.", n, g_skip_jump.load(std::memory_order_relaxed), g_jump_pairs,
                     g_gclock.load(std::memory_order_relaxed), g_gc_marks.load(), g_gc_free.load(),
                     (fq4.QuadPart != 0) ? (double)g_gm_period.load(std::memory_order_relaxed) * 1000.0 / (double)fq4.QuadPart : 0.0);
            mgpu::diag::info(l4);
        }
    }
    return true;
}

void stop()
{
    g_on.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_stop = true;
    }
    g_cv.notify_one();
    if (g_thread != nullptr)
    {
        if (g_thread->joinable()) g_thread->join();
        delete g_thread; g_thread = nullptr;
    }
    { std::lock_guard<std::mutex> lk(g_mx); g_has_job = false; }
}

void release()
{
    const bool was = g_armed;
    stop();   // a no-op when the caller already stopped it outside s.cs (the normal path)
    // drain: the last present's copy may still read a set on the present queue
    if (g_pfence != nullptr)
    {
        UINT64 need = 0ull;
        for (unsigned k = 0; k < SETS; ++k) if (g_set[k].fence > need) need = g_set[k].fence;
        if (need != 0ull && g_pfence->GetCompletedValue() < need)
        {
            HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (ev != nullptr)
            {
                if (SUCCEEDED(g_pfence->SetEventOnCompletion(need, ev)))
                    if (WaitForSingleObject(ev, 2000) != WAIT_OBJECT_0)
                        mgpu::diag::warn("[MGPU][R341] release: the present queue did not retire the last frame-generation present in 2 s; freeing anyway.");
                CloseHandle(ev);
            }
        }
        g_pfence->Release(); g_pfence = nullptr;
    }
    mgpu::gpu1::present_override_free(false);   // R387: the next arm's first paced present reads the geometry again
    if (was && g_carried != 0ull)
    {
        char cl3[200];
        snprintf(cl3, sizeof cl3, "[MGPU][R384] FGSkipCarry: %llu pair(s) carried the frames of a skipped set.", g_carried);
        mgpu::diag::info(cl3);
    }
    if (was && (g_lim.pairs != 0ull || g_fill_pairs != 0ull))
    {
        char l9[800];
        snprintf(l9, sizeof l9, "[MGPU][R390] FGLimit / FGFill summary: display period %.3f ms (0 = not read) | limit: %llu pair(s), in-between "
                 "frames shown %llu, held back %llu, pairs shown as the real frame alone %llu | fill: %llu pair(s), in-between frames shown "
                 "%llu, pairs of one game frame (nothing to fill) %llu, in-between frames not shown so the real frame "
                 "is not held longer than without the key %llu | one game frame %.2f ms.",
                 g_refresh_ms, g_lim.pairs, g_lim.gen_shown, g_lim.gen_held, g_lim.real_only, g_fill_pairs, g_fill_shown, g_fill_alone,
                 g_fill_cut, g_game_ms);
        mgpu::diag::info(l9);
    }
    if (was && (g_dgate.to_off != 0ull || g_dgate.lowered != 0ull || g_dgate.to_on != 0ull || g_dgate_real_only != 0ull))
    {
        char l10[420];
        snprintf(l10, sizeof l10, "[MGPU][R395] FGDisplayCheck summary: switched off %llu time(s), on again %llu (tries that did not hold %llu), "
                 "multiplier lowered %llu, raised %llu | %llu neural frame(s) shown alone because of it.",
                 g_dgate.to_off, g_dgate.to_on, g_dgate.tries_failed, g_dgate.lowered, g_dgate.raised, g_dgate_real_only);
        mgpu::diag::info(l10);
    }
    if (g_gen == 5)
    {   // R396: no other generator reaches this block
        if (was)
        {
            char ws[420], l11[760];
            mgpu::fgfwd::summary(ws, sizeof ws);
            snprintf(l11, sizeof l11, "[MGPU][R396] FrameGen=5 summary: pairs handed to the pacing thread %llu | not handed: no vectors or no "
                     "copy-queue fence %llu, a cut (more than 8 game frames) %llu, the display check %llu, the frame before was still "
                     "being shown %llu | pairs cut short for a newer frame %llu | %s.",
                     g_fwd_posted, g_fwd_noinput, g_fwd_cut, g_fwd_gate, g_fwd_late, g_fwd_stopped.load(), ws);
            mgpu::diag::info(l11);
        }
        mgpu::fgfwd::release();
    }
    if (was && g_ah_seats != 0ull) r399_line("released after", g_handoffs.load());   // R399
    mgpu::fgahead::release();   // R399: waits (bounded) for its last list, which may still write a set; nothing to do when it was never made
    if (g_gen == 3) mgpu::fgfsr::release();   // R380: FrameGen=4 never reaches this call
    if (g_handle != nullptr && g_rel != nullptr) g_rel(g_handle);
    g_handle = nullptr;
    g_params = nullptr;   // the parameter block belongs to the NGX session, which the add-on never shuts down (V48)
    free_sets();
    if (g_timer != nullptr) { CloseHandle(g_timer); g_timer = nullptr; }
    mgpu::fginstr::release();   // R371
    if (was && g_r372_alt_jobs != 0ull)
    {
        char rl[420];
        snprintf(rl, sizeof rl, "[MGPU][R372] pacing summary: %llu pair(s) by a rule other than the 0.5.1 one | clock: shown %llu late %llu empty ticks %llu skipped %llu grid restarts %llu cushion %.2f ms | absolute: pairs %llu re-anchored %llu.",
                 g_r372_alt_jobs, g_clk.shown, g_clk.late, g_clk.gaps, g_clk.skipped, g_clk.rebases, g_clk.cushion, g_abs.pairs, g_abs.resync);
        mgpu::diag::info(rl);
    }
    if (was)
    {
        char l[400];
        snprintf(l, sizeof l, "[MGPU][R341] FrameGen=%d released: generated %llu, presented gen %llu real %llu, dropped %llu, skipped %llu/%llu, errors %llu.",
                 g_gen, g_made, g_pres_gen.load(), g_pres_real.load(), g_dropped.load(), g_busy_skips, g_nomv, g_errors);
        mgpu::diag::info(l);
    }
    g_armed = false; g_up = false; g_tried = false;
}

// R397. Bridge thread, once per evaluated frame, before forward_take() / record(): the vector scale the stream uses now.
// Stored always (two plain stores); used only with FGMvScaleNow=1. Says the two values once, and again when they change.
void stream_scale(float x, float y)
{
    g_now_sx = x; g_now_sy = y; g_now_have = true;
    if (g_scale_now.load(std::memory_order_relaxed) == 0 || !on()) return;
    if (x == g_scale_said_x && y == g_scale_said_y) return;
    g_scale_said_x = x; g_scale_said_y = y;
    g_r378_said.store(false, std::memory_order_relaxed);   // the [R378] line is said again with the new value
    char l[320];
    snprintf(l, sizeof l, "[MGPU][R397] FGMvScaleNow=1: the vector scale in use is the stream's, %.3f, %.3f (the copy made at the arm "
             "is %.3f, %.3f). Said when it changes.", (double)x, (double)y, (double)g_i.mvec_scale_x, (double)g_i.mvec_scale_y);
    mgpu::diag::info(l);
}

// R396 (FrameGen=5). Bridge thread, in the consume list of game frame B, before record(): B's vectors, depth and input
// colour are bound for B's evaluate, which has not run yet. The newest neural frame (the set record() wrote last, already
// handed over and shown) is given to the pacing thread to be pushed along B's vectors, `n` frames evenly over one
// hand-over interval. Returns at once for every other generator.
void forward_take(ID3D12Resource *mvec, ID3D12Resource *depth, ID3D12Resource *game, float mv_el,
                  unsigned long long f, ID3D12Fence *unpack, unsigned long long unpack_value)
{
    if (g_gen != 5) return;
    if (!on() || !g_up || g_thread == nullptr) return;   // the thread starts with the first hand-over
    (void)f;
    set_t &a = g_set[(g_k + SETS - 1u) % SETS];
    if (a.frame == 0ull || !a.handed || a.real == nullptr) return;
    if (a.fwd.load(std::memory_order_acquire)) { ++g_fwd_late; return; }
    if (mvec == nullptr || unpack == nullptr)
    {
        // no vectors this frame, or the inline unpack (no fence to order the read behind): the neural frame alone
        ++g_fwd_noinput;
        if (!g_fwd_said)
        {
            g_fwd_said = true;
            mgpu::diag::warn(unpack == nullptr
                ? "[MGPU][R396] FrameGen=5: this frame was unpacked inline, not by the copy queue - there is no fence to read its "
                  "vectors behind, so no forward frame is made for it. FrameGen=5 needs CopyQueue=1 or 2. Said once; counted."
                : "[MGPU][R396] FrameGen=5: a frame without vectors - no forward frame for it. Said once; counted.");
        }
        return;
    }
    if (!(mv_el >= 1.0f) || mv_el > 8.0f) { ++g_fwd_cut; return; }   // a cut or a stall: nothing is pushed across it
    unsigned n = g_mult.load(std::memory_order_relaxed);
    if (n > g_mult_armed) n = g_mult_armed;
    if (g_dcheck.load(std::memory_order_relaxed) != 0 && g_dgate.m != 0u)
    {   // R395: no more frames than the display shows; below 2x the neural frame alone
        const unsigned al = g_dgate.allowed(n);
        if (al < 2u) { ++g_fwd_gate; return; }
        n = al;
    }
    if (n < 2u) n = 2u;
    if (n > 4u) n = 4u;
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    const float mw = (float)(g_i.mvec_w != 0u ? g_i.mvec_w : g_i.w), mh = (float)(g_i.mvec_h != 0u ? g_i.mvec_h : g_i.h);
    const float sgn = g_invert ? -1.0f : 1.0f;
    job_t job; job.set = &a; job.r = g_fwd_rgb[0]; job.g = g_fwd_rgb[1]; job.b = g_fwd_rgb[2]; job.t0 = now;
    job.fwd_n = n;
    job.step_ms = ((g_ema_ms > 0.0) ? g_ema_ms : 16.0) / (double)n;   // the pair's frames evenly over one hand-over interval
    job.fwd.neural = a.real;
    job.fwd.mvec = mvec;
    job.fwd.depth_b = depth;
    job.fwd.depth_a = (a.has_dp && depth != nullptr) ? a.dp : nullptr;
    job.fwd.game = game;
    // the game's vector unit as a share of the picture: lane scale / the vector resource's extent (pixels there)
    job.fwd.uv_x = sgn * r397_scale_x() / mw;   // R397: the arm's copy unless FGMvScaleNow=1
    job.fwd.uv_y = sgn * r397_scale_y() / mh;
    job.fwd.show = g_warp_show.load(std::memory_order_relaxed);
    job.fwd.el = mv_el;
    job.fwd.n = n;
    job.fwd.fill = g_warp_fill.load(std::memory_order_relaxed);
    job.fwd.unpack = unpack;
    job.fwd.unpack_value = unpack_value;
    a.fwd.store(true, std::memory_order_release);
    {
        std::unique_lock<std::mutex> lk(g_mx);
        if (g_has_job)
        {
            if (g_job.fwd_n != 0u) g_job.set->fwd.store(false, std::memory_order_release);
            else g_job.set->busy.store(false, std::memory_order_release);
            g_dropped.fetch_add(1ull);
        }
        g_job = job; g_has_job = true;
    }
    g_cv.notify_one();
    ++g_fwd_posted; g_made += n;
}

// R400 (FGGameClock). The GAME's thread, at its present: one relaxed load with the key absent; with it, the time of
// this present and the game frame it closes (followed slowly). Plain stores, nothing waited for.
void game_present_mark()
{
    if (g_gclock.load(std::memory_order_relaxed) == 0) return;
    LARGE_INTEGER now{}, fq{};
    QueryPerformanceCounter(&now); QueryPerformanceFrequency(&fq);
    const long long was = g_gm_qpc.exchange(now.QuadPart, std::memory_order_relaxed);
    if (was == 0 || fq.QuadPart == 0) return;
    const long long d = now.QuadPart - was;
    if (d < fq.QuadPart / 1000 || d > fq.QuadPart / 5) return;   // under 1 ms or over 200 ms: not a frame of the cadence
    const long long p = g_gm_period.load(std::memory_order_relaxed);
    g_gm_period.store((p <= 0) ? d : p + (d - p) / 8, std::memory_order_relaxed);
}

// R399 (FGAhead, FrameGen 3 / 4). Bridge thread, in the consume list of game frame B, BEFORE record(): B has just been
// picked; its colour, vectors and depth are unpacked (or will be, behind `unpack`), its neural output does not exist yet.
// The newest neural frame A (already handed over and on screen) is what the generator holds as its previous frame; B's
// own picture is handed as the current one, with B's vectors over the game frames from A to B - the call record() used
// to make with B's neural output, one evaluate earlier. The frame(s) it gives are written into A's set and shown by
// the pacing thread after A. The list runs on `queue` (the neural queue) ahead of B's own list.
void ahead_take(ID3D12CommandQueue *queue, ID3D12Resource *mvec, ID3D12Resource *depth, ID3D12Resource *game, float mv_el,
                unsigned long long f, ID3D12Fence *unpack, unsigned long long unpack_value)
{
    if (g_ahead.load(std::memory_order_relaxed) == 0) return;   // key absent: this load
    if ((g_gen != 3 && g_gen != 4) || !on() || !g_up || g_thread == nullptr) return;
    set_t &a = g_set[(g_k + SETS - 1u) % SETS];
    if (a.frame == 0ull || !a.handed || a.ah_taken) return;
    a.ah_taken = true;
    auto none = [&](unsigned long long &count) { ++count; a.ah_n.store(AH_NONE, std::memory_order_release); };
    if (!a.seated || a.ah_plan == 0u) { none(g_ah_unseated); return; }
    if (mvec == nullptr || depth == nullptr || game == nullptr || a.real == nullptr || a.gen == nullptr) { none(g_ah_noinput); return; }
    if (unpack == nullptr || queue == nullptr)
    {
        none(g_ah_noinput);
        if (!g_ah_said_cq)
        {
            g_ah_said_cq = true;
            mgpu::diag::warn("[MGPU][R399] FGAhead: this frame was unpacked inline, not by the copy queue - there is no fence to read "
                             "the picked frame behind, so no in-between frame is made. FGAhead needs CopyQueue=1 or 2. Said once; counted.");
        }
        return;
    }
    if (!(mv_el >= 1.0f) || mv_el > 8.0f) { none(g_ah_cut); return; }   // a cut or a stall: nothing is made across it
    {   // the generator is handed the picked frame's picture where it has always been handed the neural one
        const D3D12_RESOURCE_DESC gd = game->GetDesc(), nd = a.real->GetDesc();
        if (gd.Width != nd.Width || gd.Height != nd.Height || gd.Format != nd.Format)
        {
            none(g_ah_extent);
            if (!g_ah_said_ext)
            {
                g_ah_said_ext = true;
                char el[360];
                snprintf(el, sizeof el, "[MGPU][R399] FGAhead: the game frame on the neural card is %llux%u format %u, the neural frame %llux%u "
                         "format %u. The generator needs both ends alike, so no in-between frame is made. Said once; counted.",
                         (unsigned long long)gd.Width, (unsigned)gd.Height, (unsigned)gd.Format,
                         (unsigned long long)nd.Width, (unsigned)nd.Height, (unsigned)nd.Format);
                mgpu::diag::warn(el);
            }
            return;
        }
    }
    if (!mgpu::fgahead::up())
    {
        char why[200];
        if (g_ah_said_up || !mgpu::fgahead::create(g_i.ndev, queue, why, sizeof why))
        {
            if (!g_ah_said_up)
            {
                g_ah_said_up = true;
                char ul[360];
                snprintf(ul, sizeof ul, "[MGPU][R399] FGAhead: %s. No in-between frame is made in this arm; the neural frames are shown as they finish.", why);
                mgpu::diag::error(ul);
            }
            none(g_ah_failed);
            return;
        }
    }
    ID3D12GraphicsCommandList *const cl = mgpu::fgahead::begin();
    if (cl == nullptr) { none(g_ah_busy); return; }
    unsigned n = a.ah_plan;
    // The picked frame's three textures rest in COMMON after the copy queue (gpu1_context.cpp, tex_in_rest); B's own
    // list finds them there again.
    transition(cl, game, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(cl, mvec, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(cl, depth, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    mgpu::slcam::camera cam;   // R379: the game's camera from A to B, when it stated one
    const bool cam_have = mgpu::slcam::on() && mgpu::slcam::pair(a.frame, f, cam);
    const mgpu::slcam::camera *const cam_p = cam_have ? &cam : nullptr;
    const unsigned long long fid = ++g_fid;
    unsigned made = 0u, code = 0u;
    if (g_gen == 3)
    {
        mgpu::fgfsr::set_scale(r397_scale_x(), r397_scale_y());
        ID3D12Resource *const outs[3] = { a.gen, a.genx[0], a.genx[1] };
        code = mgpu::fgfsr::evaluate(cl, game, mvec, depth, outs, n, mv_el, false, cam_p);
        ++g_evals;
        if (code == 0u) made = n;
    }
    else
    {
        const NVSDK_NGX_Result r = evaluate(cl, game, mvec, depth, a.gen, mv_el, false, fid, n, 1u, cam_p);
        ++g_evals;
        code = (unsigned)r;
        if (r == NVSDK_NGX_Result_Success)
        {
            made = 1u;
            for (unsigned x = 1u; x < n && x - 1u < 2u && a.genx[x - 1u] != nullptr; ++x)   // frames 2..n of n
            {
                const NVSDK_NGX_Result rx = evaluate(cl, game, mvec, depth, a.genx[x - 1u], mv_el, false, fid, n, x + 1u, cam_p);
                ++g_evals;
                if (rx != NVSDK_NGX_Result_Success) { ++g_errors; break; }
                made = x + 1u;
            }
        }
    }
    transition(cl, depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    transition(cl, mvec, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    transition(cl, game, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    if (made == 0u)
    {
        mgpu::fgahead::cancel();
        g_ah_hist_game = 0ull;   // the generator was called and its work never runs: the next neural frame starts it again
        ++g_errors;
        char l[320];
        snprintf(l, sizeof l, "[MGPU][R399] frame %llu: the generator call for the picked game frame failed (0x%08X, %llu error(s) so far)%s", f, code, g_errors,
                 g_errors >= 3ull ? " - frame generation OFF for this arm." : ". No in-between frame follows this neural frame.");
        mgpu::diag::error(l);
        if (g_errors >= 3ull) { g_on.store(false, std::memory_order_release); g_failed = true; }
        none(g_ah_failed);
        return;
    }
    const unsigned long long v = mgpu::fgahead::submit(unpack, unpack_value);
    if (v == 0ull) { g_ah_hist_game = 0ull; none(g_ah_failed); return; }
    g_ah_hist_game = f;   // the generator holds game frame f now; f's neural frame is seated on it in record()
    a.ah_value = (UINT64)v;
    a.ah_n.store(made, std::memory_order_release);
    ++g_ah_pairs; g_ah_made += made; g_made += made;
}

// R392: the panel's live switches (any thread; atomics only). what: 0 FGMvUnit, 1 FGSkipCarry, 2 FGLimit, 3 FGFill,
// 4 FGDisplayCheck.
int ui_get(int what)
{
    switch (what)
    {
    case 4: return g_dcheck.load(std::memory_order_relaxed);
    case 5: return g_scale_now.load(std::memory_order_relaxed);
    case 6: return g_ahead.load(std::memory_order_relaxed);   // R399
    case 7: return g_skip_jump.load(std::memory_order_relaxed);   // R400
    case 9: return g_phase.load(std::memory_order_relaxed);       // R401
    case 8: return g_gclock.load(std::memory_order_relaxed);      // R400
    case 0: return g_mv_unit.load(std::memory_order_relaxed);
    case 1: return g_skip_carry.load(std::memory_order_relaxed);
    case 2: return g_limit.load(std::memory_order_relaxed);
    case 3: return g_fill.load(std::memory_order_relaxed);
    default: return 0;
    }
}

int ui_set(int what, int value)
{
    const int v = value != 0 ? 1 : 0;
    int set = v;
    static const char *name[5] = { "FGMvUnit", "FGSkipCarry", "FGLimit", "FGFill", "FGDisplayCheck" };
    if (what == 5)
    {   // R397
        const int w5 = g_scale_now.exchange(v, std::memory_order_relaxed);
        g_panel_own.fetch_or(32, std::memory_order_relaxed);
        g_scale_said_x = g_scale_said_y = 0.0f;   // benign: read on the bridge thread, makes the [R397] line say itself again
        if (w5 != v) { char l5[160]; snprintf(l5, sizeof l5, "[MGPU][R392] panel: FGMvScaleNow %d -> %d.", w5, v); mgpu::diag::info(l5); }
        return v;
    }
    if (what == 9)
    {   // R401: on = the file's own 67 or 75 (75 when the file never said)
        const int s9 = v != 0 ? g_phase_val.load(std::memory_order_relaxed) : 0;
        const int w9 = g_phase.exchange(s9, std::memory_order_relaxed);
        g_panel_own.fetch_or(512, std::memory_order_relaxed);
        if (w9 != s9) { char l9[160]; snprintf(l9, sizeof l9, "[MGPU][R392] panel: FGPhase %d -> %d, live from the next pair.", w9, s9); mgpu::diag::info(l9); }
        return s9;
    }
    if (what == 7 || what == 8)
    {   // R400: on = the file's own value (FGSkipJump 3, FGGameClock 1 when the file never said)
        std::atomic<int> &a = (what == 7) ? g_skip_jump : g_gclock;
        const int s7 = v != 0 ? ((what == 7) ? g_skip_jump_val : g_gclock_val).load(std::memory_order_relaxed) : 0;
        const int w7 = a.exchange(s7, std::memory_order_relaxed);
        g_panel_own.fetch_or(what == 7 ? 128 : 256, std::memory_order_relaxed);
        if (w7 != s7) { char l7[160]; snprintf(l7, sizeof l7, "[MGPU][R392] panel: %s %d -> %d, live from the next pair.", what == 7 ? "FGSkipJump" : "FGGameClock", w7, s7); mgpu::diag::info(l7); }
        return s7;
    }
    if (what == 6)
    {   // R399: on = the file's own 1 or 2 (1 when the file never said)
        const int s6 = v != 0 ? g_ahead_val.load(std::memory_order_relaxed) : 0;
        const int w6 = g_ahead.exchange(s6, std::memory_order_relaxed);
        g_panel_own.fetch_or(64, std::memory_order_relaxed);
        if (w6 != s6) { char l6[160]; snprintf(l6, sizeof l6, "[MGPU][R392] panel: FGAhead %d -> %d, live from the next neural frame.", w6, s6); mgpu::diag::info(l6); }
        return s6;
    }
    if (what < 0 || what > 4) return 0;
    if (what == 4)
    {   // R395: not an owner bit - FGLive re-reads it like the file says, and the panel writes the file
        const int w4 = g_dcheck.exchange(v, std::memory_order_relaxed);
        if (w4 != v) { char l4[160]; snprintf(l4, sizeof l4, "[MGPU][R392] panel: FGDisplayCheck %d -> %d.", w4, v); mgpu::diag::info(l4); }
        return v;
    }
    int was = v;
    switch (what)
    {
    case 0: was = g_mv_unit.exchange(v, std::memory_order_relaxed); if (was != v) g_r378_said.store(false, std::memory_order_relaxed); break;
    case 1: was = g_skip_carry.exchange(v, std::memory_order_relaxed); break;
    case 2: was = g_limit.exchange(v, std::memory_order_relaxed); break;
    case 3: set = v != 0 ? g_fill_val.load(std::memory_order_relaxed) : 0; was = g_fill.exchange(set, std::memory_order_relaxed); break;
    }
    g_panel_own.fetch_or(1 << what, std::memory_order_relaxed);
    if (was != set)
    {
        char l[200];
        snprintf(l, sizeof l, "[MGPU][R392] panel: %s %d -> %d, live from the next pair.", name[what], was, set);
        mgpu::diag::info(l);
    }
    return set;
}

// R395: what the display check decided, for the panel. 0 = nothing to say, 1 = frame generation off, 2.. = lowered to
// this multiplier. disp / would: the display's frames a second and what 2x would be.
int ui_display_check(int *disp, int *would)
{
    if (disp != nullptr) *disp = g_dgate_ui_disp.load(std::memory_order_relaxed);
    if (would != nullptr) *would = g_dgate_ui_would.load(std::memory_order_relaxed);
    return (g_dcheck.load(std::memory_order_relaxed) != 0) ? g_dgate_ui.load(std::memory_order_relaxed) : 0;
}
}
