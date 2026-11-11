// MGPU Bridge - R341 FrameGen=4: DLSS Frame Generation on the neural card. See
// fg_gpu1.hpp. Design: Dave (davidm71), DGP1/DLSSG1.

#include "fg_gpu1.hpp"
#include "fg_hud.hpp"   // [PORT C] the during-play readout: a layered GDI window, outside the render path

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
#include "presenter.hpp"      // [H2 FIX 3] disp_burst_id: one id per source frame
#include "fg_mode.hpp"        // [PORT H5] mgpu::fgmode: the panel's backend and multiplier
#include "mgpu_ini_parser.hpp"
#include "../ext/ngx/nvsdk_ngx.h"

namespace mgpu::fg
{
namespace
{
// [PORT FG-PANEL 2026-11-11] 4, not 3. Measured on PBZ: with 3 sets and an NR
// interval of ~24 ms, 551 of 2400 frames were refused because the set's last present
// had not retired - a quarter of the pairs went out as real-only. The 4th set costs
// one more real+gen pair (~66 MB on the neural card) and buys a full extra
// interval of retirement headroom.
const unsigned SETS = 4u;

// [PORT H5, 2026-11-11] MULTI-FRAME. One source frame can carry more than one generated
// picture: DLSS-G's MultiFrameCount/MultiFrameIndex pair asks for each of them in its own
// EvaluateFeature call, and each writes its own OutputInterpolated texture. 3x needs two
// generated frames per source frame, 4x three - so a set carries MAX_GEN of them, and a
// set that only asked for 2x uses the first and leaves the rest idle (0 MB of extra work:
// the textures are made once at arm, not per frame).
//
// The provider's own ceiling is DLSSG.MultiFrameCountMax (5 on this card, [R341]); the
// request is clamped by it AND by fgmode's MULTI_MAX, so nothing here can ask for a frame
// the model cannot produce.
const unsigned MAX_GEN = 3u;

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
    ID3D12Resource *gen[MAX_GEN] = {};  // [H5] the in-between frames, each rests UNORDERED_ACCESS
    unsigned gen_n = 0u;              // [H5] how many of them this set holds (0 = real only)
    unsigned long long frame = 0ull;  // NR frame the set holds (0 = none)
    bool has_gen = false;
    bool handed = false;              // handoff() took it (once per recorded frame)
    UINT64 fence = 0ull;              // present fence value after its last present (0 = never presented)
    std::atomic<bool> busy{false};    // handed to the pacing thread, not yet retired
};

std::atomic<bool> g_on{false};
bool g_armed = false;
arm_info g_i{};
float g_fov = 90.0f, g_near = 0.1f, g_far = 10000.0f;
bool g_invert = false, g_pace = true;

// [PORT FG-PANEL, 2026-11-11] THE HOT SWITCH. Read by record() only: false
// skips the EvaluateFrameGeneration call, so the real frame is still copied and
// still presented by the same pacing thread - cadence unchanged, feature kept,
// nothing freed. True resumes generation on the next frame with a reset pair
// (g_last_fid cleared), because the history across a pause is not a valid pair.
std::atomic<bool> g_live{true};
std::atomic<bool> g_hud{true};   // [PORT C] the readout is on unless FgHud=0
// [PORT DISPLAY-LAYER 1/3, 2026-11-11] THE LANDING LEDGER.
//
// The reference tree's display layer exists to control WHEN a composed frame
// lands, and the first thing it owns is the measurement of that landing. This
// is that measurement and nothing else: no new owner, no new present call, the
// author's pacing thread stays the only caller. It records, for every present
// the pacing thread makes, how late it was against its own target (the handoff
// time for the generated frame, handoff + half interval for the real one).
//
// It is the first slice of the presenter port on purpose: the three-owner
// failure mode (game / bridge / display threads all changing at load) is a
// TRANSITION problem, and a transition can only be made safe against numbers
// somebody is measuring. Stage B (the ring the pacing thread takes frames from)
// uses these same numbers as its target; stage A (our chain, for XeSS' swapchain
// handover) is the only one that changes ownership, and it is not this slice.
double g_land_gen_ms = 0.0, g_land_real_ms = 0.0, g_land_worst_ms = 0.0;
unsigned long long g_land_n = 0ull, g_land_late = 0ull;
double g_land_sum_g = 0.0, g_land_sum_r = 0.0;
// [PORT DISPLAY-LAYER 2/3, 2026-11-11] THE ABSOLUTE LANDING PACER.
//
// The author's pacing sleeps "half the measured NR interval" after the generated
// frame, which is RELATIVE: every late present pushes the next one later, and a
// single stall is never absorbed. The display layer's rule is ABSOLUTE: each
// composed frame has a target instant on the output clock, the pacer sleeps to
// it, and the schedule re-syncs only when the pacer is more than one refresh
// behind - so a short stall is absorbed by the next frames instead of shifting
// the whole timeline.
//
// The refresh interval is MEASURED from consecutive real presents (a 2x pair
// spans two refreshes), smoothed both ways, and rejected when it is absurd
// (>2 ms .. <50 ms window, 0.6..1.6x of the current value). Nothing here changes
// WHO presents: the pacing thread is still the only caller.
bool g_abs = true;                  // DisplayPace=0 restores the author's relative sleep
double g_int_ms = 0.0;              // refresh interval; SOURCE MATTERS (see g_int_src)
int g_int_src = 0;                  // 0 = measured from presents (fallback), 1 = the output refresh from the swapchain
LARGE_INTEGER g_abs_next{};         // next REAL frame target on the QPC clock
LARGE_INTEGER g_last_real{};        // last real present instant
unsigned long long g_resync = 0ull, g_abs_n = 0ull;

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
unsigned long long g_hot_off_skips = 0ull;   // [PORT FG-PANEL] frames the hot switch kept gen-less
std::atomic<unsigned long long> g_pres_gen{0}, g_pres_real{0}, g_handoffs{0}, g_dropped{0};
double g_half_sum = 0.0; unsigned g_half_n = 0u;

// the pacing thread
struct job_t { set_t *set = nullptr; bool gen = false; float r = 0, g = 0, b = 0; LARGE_INTEGER t0{}; double half_ms = 8.0; double step_ms = 8.0; unsigned gen_n = 0u; };
std::mutex g_mx;
std::condition_variable g_cv;
std::thread *g_thread = nullptr;
// [PORT DISPLAY-LAYER 3/3, 2026-11-11] THE JOB RING. Newest-wins was a 1-deep queue:
// a job that arrived while the pacing thread was busy was thrown away, and that
// frame never reached the display. Three deep, oldest-first, drop-oldest only when
// full: the pacer then presents the backlog against ABSOLUTE targets (in the past,
// so immediately) and catches up instead of skipping. The set a dropped job held is
// released here so record() can reuse it.
bool g_stop = false;
constexpr unsigned JOBQ = 3u;
job_t g_q[JOBQ];
unsigned g_qn = 0u;
unsigned g_qpeak = 0u;   // [PORT 3/3] deepest the job ring ever got (1 = the old behaviour)
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
    const char *dp = mgpu::config::find(buf.data(), got, "DisplayPace"); g_abs = !(dp != nullptr && atoi(dp) == 0);   // [PORT 2/3]
    const char *hd = mgpu::config::find(buf.data(), got, "FgHud");        g_hud = !(hd != nullptr && atoi(hd) == 0);      // [PORT C]
    const char *ai = mgpu::config::find(buf.data(), got, "NgxAppId");    if (ai != nullptr) app_id = (unsigned long long)(unsigned)atoi(ai);
    // [PORT FG-PANEL, 2026-11-11] the panel records the REQUESTED backend and
    // multiplier. Only DLSS 2x is built here, so say so instead of silently
    // running 2x while the panel shows x3/x4 or FSR/XeSS.
    const char *fbm = mgpu::config::find(buf.data(), got, "FgMulti");
    const int fg_multi = (fbm != nullptr) ? atoi(fbm) : 2;
    const char *fbb = mgpu::config::find(buf.data(), got, "FgBackend");
    const int fg_backend = (fbb != nullptr) ? atoi(fbb) : 1;
    if (framegen != 0 && fg_backend != 1)
        mgpu::diag::warn("[MGPU][R341] FG backend from the panel is not built in this build: this module is the "
                         "DLSS-G path only (FgBackend=1). XeSS-FG is the display layer's provider arm, not this one.");
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
    snprintf(l, sizeof l, "[MGPU][R341] FrameGen=4 OFF for this arm: %s. The bridge presents the real frames as before.", why);
    mgpu::diag::error(l);
}

// Loads the snippet, opens the session on the neural device, creates the feature at the
// NR output's size. Records the creation into `cl`. Once per arm.
bool ensure_feature(ID3D12GraphicsCommandList *cl)
{
    if (g_up) return true;
    if (g_tried) return false;
    g_tried = true;
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
            snprintf(line, sizeof line, "[MGPU][R341] Init_Ext 0x%08X | FrameGeneration.Available=%d NeedsUpdatedDriver=%d FeatureInitResult=0x%08X MultiFrameCountMax=%u (this arm asks %ux from FgMulti)",
                     (unsigned)ri, avail, needs, fir, mfc, (unsigned)mgpu::fgmode::get_multi());
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

// [PORT H5, 2026-11-11] ONE EvaluateFeature CALL PRODUCES ONE GENERATED FRAME.
//
// 2X is mfc=1 / mfi=1 - the pair the author shipped, byte for byte. For NX the caller
// asks the provider for mfc generated frames and calls this once per frame with
// mfi = 1..mfc, each writing its own `out`. MultiFrameCount is NOT "how many frames to
// show": it is the length of the sequence this index belongs to, so both numbers move
// together or the library answers a mismatch and the frame falls back to real-only.
NVSDK_NGX_Result evaluate_mf(ID3D12GraphicsCommandList *cl, ID3D12Resource *real, ID3D12Resource *mvec, ID3D12Resource *depth,
                             ID3D12Resource *out, float mv_el, bool reset, unsigned long long fid,
                             unsigned mfc, unsigned mfi)
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
    //
    // [FFX, 2026-11-11] ONE LANE SCALE CANNOT SERVE BOTH CONSUMERS. NR is armed at the UV->pixel
    // factor (2562x1441 for FSR), so `mvec_scale_x / mw` here collapsed back to ~1.0; with the
    // lane at 1.0 it collapsed to 1/2564. Both are wrong for a UV field: the raw value IS the
    // frame fraction, so DLSS-G needs 1.0 (or 2.0 if it takes NDC). FgMvUv selects that path and
    // leaves the pixel-unit formula byte for byte when it is 0.
    float fg_sx = sgn * g_i.mvec_scale_x * mv_el / mw;
    float fg_sy = sgn * g_i.mvec_scale_y * mv_el / mh;
    if (g_i.fg_uv != 0)
    {
        const float k = (g_i.fg_uv == 2) ? 2.0f : 1.0f;
        fg_sx = sgn * k * mv_el;
        fg_sy = sgn * k * mv_el;
        static std::atomic<bool> said{false};
        if (!said.exchange(true))
        {
            char l[360];
            snprintf(l, sizeof l,
                     "[MGPU][R341] FG vector units: FgMvUv=%d -> DLSSG.MvecScale=%.3f,%.3f for a UV field "
                     "(lane NR scale %.1f,%.1f is NOT used here). If the generated frames lag or overshoot, "
                     "the other value (1 vs 2) is the whole difference.",
                     g_i.fg_uv, (double)fg_sx, (double)fg_sy,
                     (double)g_i.mvec_scale_x, (double)g_i.mvec_scale_y);
            mgpu::diag::info(l);
        }
    }
    p->Set("DLSSG.MvecScaleX", fg_sx);
    p->Set("DLSSG.MvecScaleY", fg_sy);
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

void thread_main()
{
    for (;;)
    {
        job_t job;
        unsigned burst = 0u;   // [H2 FIX 3] one burst id for this source frame's pair
        {
            std::unique_lock<std::mutex> lk(g_mx);
            g_cv.wait(lk, [] { return g_qn > 0u || g_stop; });
            if (g_stop) return;
            job = g_q[0];
            for (unsigned qi = 1u; qi < g_qn; ++qi) g_q[qi - 1u] = g_q[qi];
            --g_qn;
        }

        // ---- [PORT DISPLAY-LAYER 2/3, 2026-11-11] ABSOLUTE LANDING ----
        // Targets are on the QPC clock, not "sleep half an interval": a short stall
        // is absorbed by the following frames instead of shifting the whole timeline.
        bool ok = true;
        UINT64 fv = 0ull;
        LARGE_INTEGER fq{}, now{};
        QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&now);
        const double tick_ms = (fq.QuadPart != 0) ? 1000.0 / (double)fq.QuadPart : 0.0;
        // [PORT 2/3b] PREFER THE OUTPUT CLOCK. Only when the swapchain never gave one do we
        // fall back to measuring our own presents - which is exactly the feedback loop that
        // locked this pacer at 41 Hz earlier today.
        if (g_int_ms <= 1.0)
        {
            const double hz = mgpu::gpu1::display_refresh_hz();
            if (hz > 20.0 && hz < 500.0) { g_int_ms = 1000.0 / hz; g_int_src = 1; }
        }
        const double int_ms = (g_int_ms > 1.0) ? g_int_ms : 0.0;
        const LONGLONG int_tick = (tick_ms > 0.0) ? (LONGLONG)(int_ms / tick_ms + 0.5) : 0;
        // [PORT 2/3c, 2026-11-11] THE TARGET CLOCK IS THE SOURCE, NOT THE REFRESH.
        // One real frame per handoff, so the real target advances by the HANDOFF
        // interval (measured from the game, independent of our presents); the
        // generated frame sits half of that before it. Using the refresh (6.06 ms
        // at 165 Hz) as the advance made 50 fps content ask for 165 Hz of targets
        // and every frame needed a resync (measured: 1123 of 1200).
        const double src_ms = (g_ema_ms > 1.0) ? g_ema_ms : 0.0;
        const LONGLONG src_tick = (tick_ms > 0.0 && src_ms > 0.0) ? (LONGLONG)(src_ms / tick_ms + 0.5) : 0;
        const bool abs_ok = g_abs && src_tick > 0 && int_tick > 0 &&
                            !mgpu::gpu1::presenter_owns_output();   // [H2 FIX 2] one timing owner
        auto wait_abs = [&](LONGLONG target)
        {
            if (tick_ms <= 0.0) return;
            LARGE_INTEGER z{}; z.QuadPart = 0;
            wait_until(z, (double)target * tick_ms);
        };
        auto land_note = [&](double err)
        {
            g_land_sum_g += (err < 0.0) ? 0.0 : err; ++g_land_n;
            if (err > g_land_worst_ms) g_land_worst_ms = err;
            if (err > 8.0) ++g_land_late;
        };

        LONGLONG gen_t = 0, real_t = 0;
        if (abs_ok)
        {
            if (g_abs_next.QuadPart == 0) g_abs_next = now;
            if (now.QuadPart > g_abs_next.QuadPart + src_tick) { g_abs_next = now; ++g_resync; }
            real_t = g_abs_next.QuadPart;
            ++g_abs_n;
        }

        // [PORT H5, 2026-11-11] N GENERATED FRAMES, THEN THE REAL ONE.
        //
        // The 2X arm's "generated frame half an interval before the real one" generalises
        // to: with `ng` generated frames in this source interval, frame i (0-based) lands
        // (ng - i) / (ng + 1) of an interval before the real one - so the last generated
        // frame is still exactly half an interval ahead at ng = 1, which is the 2X cadence
        // the author measured and nothing here changes it for 2X.
        //
        // [FIX D1, 2026-11-11] THE DENOMINATOR WAS ng, NOT ng + 1. With ng = 1 that put
        // the generated frame a WHOLE source interval before the real one - exactly on the
        // previous pair's real frame - and the next job's generated frame on this one, so
        // each pair presented twice in one vblank and then left a hole. The same file's
        // job.step_ms already divided by (gen_n + 1), and nc-base's measured burst does
        // `step = fsr_period / (gn + 1)`; both agree with the comment above, not with the
        // old code.
        //
        // The same burst id covers all of them (disp_burst_id is taken once per SOURCE
        // frame), so the ring can drop a whole burst and never reorder one; is_gen is true
        // for the generated ones and false for the real one, which is what the reader's
        // eff_mult is computed from.
        const unsigned ng = job.gen ? ((job.gen_n != 0u) ? job.gen_n : 1u) : 0u;
        for (unsigned gi = 0u; gi < ng && ok; ++gi)
        {
            if (abs_ok)
            {
                const double back = (double)src_tick * (double)(ng - gi) / (double)(ng + 1u);
                gen_t = real_t - (LONGLONG)back;
                wait_abs(gen_t);
            }
            LARGE_INTEGER t1{}; QueryPerformanceCounter(&t1);
            const double e1 = abs_ok ? ((double)(t1.QuadPart - gen_t) * tick_ms)
                                     : ((double)(t1.QuadPart - job.t0.QuadPart) * tick_ms);
            g_land_gen_ms = e1; land_note(e1);
            burst = mgpu::presenter::disp_burst_id();
            mgpu::gpu1::present_override_gen(burst, true);
            mgpu::gpu1::present_override(job.set->gen[gi], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ok = mgpu::gpu1::present_frame(job.r, job.g, job.b);
            mgpu::gpu1::present_override(nullptr, D3D12_RESOURCE_STATE_COMMON);
            mgpu::gpu1::present_override_gen(0u, false);
            if (ok) g_pres_gen.fetch_add(1ull, std::memory_order_relaxed);
            const UINT64 v = mgpu::gpu1::present_last_fence_value(); if (v > fv) fv = v;
            if (ok && g_pace && !abs_ok) wait_until(job.t0, job.step_ms * (double)(gi + 1u));
        }
        if (ok)
        {
            if (abs_ok) wait_abs(real_t);
            LARGE_INTEGER t2{}; QueryPerformanceCounter(&t2);
            const double e2 = abs_ok ? ((double)(t2.QuadPart - real_t) * tick_ms)
                                     : (((double)(t2.QuadPart - job.t0.QuadPart) * tick_ms) - job.step_ms * (double)ng);
            g_land_real_ms = e2; land_note(e2);
            mgpu::gpu1::present_override_gen(burst, false);
            mgpu::gpu1::present_override(job.set->real, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            ok = mgpu::gpu1::present_frame(job.r, job.g, job.b);
            mgpu::gpu1::present_override(nullptr, D3D12_RESOURCE_STATE_COMMON);
            mgpu::gpu1::present_override_gen(0u, false);
            if (ok) g_pres_real.fetch_add(1ull, std::memory_order_relaxed);
            const UINT64 v = mgpu::gpu1::present_last_fence_value(); if (v > fv) fv = v;

            LARGE_INTEGER tr{}; QueryPerformanceCounter(&tr);
            if (g_last_real.QuadPart != 0 && tick_ms > 0.0)
            {
                const double span = (double)(tr.QuadPart - g_last_real.QuadPart) * tick_ms;
                const double per = job.gen ? span * 0.5 : span;
                if (g_int_src == 0 && per > 2.0 && per < 50.0)
                {
                    if (g_int_ms <= 0.0) g_int_ms = per;
                    else if (per > g_int_ms * 0.6 && per < g_int_ms * 1.6)
                        g_int_ms = g_int_ms * 0.9 + per * 0.1;
                }
            }
            g_last_real = tr;

            // [PORT C] the readout. Outside the render path on purpose: a layered GDI
            // window cannot touch the swapchain, the ring or the generators, and if it
            // fails the only loss is one line of text.
            {
                wchar_t ex[96];
                _snwprintf_s(ex, 96, _TRUNCATE, L"| %.2f ms | resync %llu", g_int_ms, g_resync);
                mgpu::fghud::tick(g_hud.load(std::memory_order_relaxed),
                                  g_handoffs.load(std::memory_order_relaxed),
                                  g_pres_gen.load(std::memory_order_relaxed) + g_pres_real.load(std::memory_order_relaxed),
                                  ex);
            }
        }
        if (abs_ok) g_abs_next.QuadPart += src_tick;   // one REAL frame per handoff

        job.set->fence = fv;   // the set is reusable once the present fence passes this (record() checks)
        job.set->busy.store(false, std::memory_order_release);
        if (!ok)
        {
            mgpu::diag::error("[MGPU][R341] the pacing thread could not present a frame - FrameGen=4 stops for this arm; "
                              "the bridge thread presents the real frames from the next one (see the [T5] line above).");
            g_thread_failed.store(true, std::memory_order_release);
            return;
        }
        if (abs_ok && g_abs_n != 0ull && (g_abs_n % 1200ull) == 0ull)
        {
            char l[420];
            snprintf(l, sizeof l,
                     "[MGPU][R342] ABSOLUTE PACER: interval=%.2f ms (%s) resync=%llu of %llu | landing mean=%.2f ms worst=%.2f ms late>8ms=%llu",
                     g_int_ms, (g_int_src != 0) ? "swapchain" : "measured", g_resync, g_abs_n,
                     (g_land_n != 0ull) ? (g_land_sum_g / (double)g_land_n) : 0.0,
                     g_land_worst_ms, g_land_late);
            mgpu::diag::info(l);
            // [PORT 2/3] WATCHDOG: if a quarter of the frames need a resync, the output
            // clock is not keeping up. The author's relative sleep is the known-stable
            // path, so the arm falls back to it instead of living on the edge.
            if (g_abs && g_abs_n != 0ull && g_resync * 4ull > g_abs_n)
            {
                g_abs = false;
                mgpu::diag::warn("[MGPU][R342] ABSOLUTE PACER: more than one frame in four needed a resync - "
                                  "falling back to the author's relative pacing for this arm (DisplayPace=0).");
            }
            snprintf(l, sizeof l, "[MGPU][R342] job ring peak=%u (1 = the old newest-wins queue), dropped=%llu", g_qpeak, g_dropped.load());
            mgpu::diag::info(l);
        }
    }
}

void free_sets()
{
    for (unsigned k = 0; k < SETS; ++k)
    {
        if (g_set[k].real != nullptr) { g_set[k].real->Release(); g_set[k].real = nullptr; }
        for (unsigned gi = 0; gi < MAX_GEN; ++gi)
            if (g_set[k].gen[gi] != nullptr) { g_set[k].gen[gi]->Release(); g_set[k].gen[gi] = nullptr; }
        g_set[k].gen_n = 0u;
        g_set[k].frame = 0ull; g_set[k].has_gen = false; g_set[k].fence = 0ull; g_set[k].busy.store(false);
    }
}
} // namespace

bool arm(const arm_info &i)
{
    release();
    int fgv = 0; unsigned long long app = 0ull;
    if (!read_keys(fgv, app) || fgv != 4) return false;   // key absent / other: the module does not exist
    if (i.ndev == nullptr || i.w == 0u || i.h == 0u) return false;
    g_i = i;
    g_tried = g_up = g_failed = false; g_k = 0u; g_fid = g_last_fid = 0ull;
    g_made = g_nomv = g_busy_skips = g_errors = g_evals = 0ull; g_hot_off_skips = 0ull;
    g_pres_gen.store(0); g_pres_real.store(0); g_handoffs.store(0); g_dropped.store(0);
    g_half_sum = 0.0; g_half_n = 0u; g_ema_ms = 0.0; g_last_handoff.QuadPart = 0;
    g_int_ms = 0.0; g_int_src = 0; g_abs_next.QuadPart = 0; g_last_real.QuadPart = 0; g_resync = 0ull; g_abs_n = 0ull;
    g_land_sum_g = 0.0; g_land_n = 0ull; g_land_late = 0ull; g_land_worst_ms = 0.0;
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
        // [H5] MAX_GEN outputs per set. 2X uses the first and the rest stay as they were
        // created; the cost is paid once at arm (~33 MB each at 4K RGBA8).
        for (unsigned gi = 0; gi < MAX_GEN && SUCCEEDED(h); ++gi)
            h = i.ndev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&g_set[k].gen[gi]));
    }
    if (FAILED(h)) { free_sets(); char l[200]; snprintf(l, sizeof l, "the frame buffers could not be made (0x%08X)", (unsigned)h); fail_for_arm(l); return false; }
    g_pfence = mgpu::gpu1::present_fence_addref();
    g_armed = true;
    g_on.store(true, std::memory_order_release);
    char l[400];
    snprintf(l, sizeof l, "[MGPU][R341] FrameGen=4 armed: %u sets of %ux%u fmt=%u on the neural card (%.0f MB, %u generated slot(s) per set), FGFov=%.0f FGNear=%.3f FGFar=%.0f FGInvertMV=%d FGPace=%d. "
             "The DLSS-G feature is created on the first NR frame.", SETS, i.w, i.h, (unsigned)i.fmt,
             (double)((1ull + (unsigned long long)MAX_GEN) * SETS * (unsigned long long)i.w * i.h * (i.fmt == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8ull : 4ull)) / (1024.0 * 1024.0),
             MAX_GEN,
             (double)g_fov, (double)g_near, (double)g_far, g_invert ? 1 : 0, g_pace ? 1 : 0);
    mgpu::diag::info(l);
    return true;
}

bool on()
{
    return g_on.load(std::memory_order_relaxed);
}

void record(ID3D12GraphicsCommandList *cl, ID3D12Resource *nr_final, ID3D12Resource *mvec, ID3D12Resource *depth,
            float mv_el, unsigned long long f)
{
    if (!on() || cl == nullptr || nr_final == nullptr) return;
    if (g_thread_failed.load(std::memory_order_acquire)) { g_on.store(false); return; }
    set_t &st = g_set[g_k];
    // FIX7/FIX8 lesson: a set the pacing thread holds, or whose last present the present
    // fence has not passed, is not rewritten. The frame is skipped instead (handoff then
    // drops it: the picture keeps the previous pair). Counted. No fence = no proof = skip.
    if (g_pfence == nullptr) g_pfence = mgpu::gpu1::present_fence_addref();
    if (g_pfence == nullptr || st.busy.load(std::memory_order_acquire) || (st.fence != 0ull && g_pfence->GetCompletedValue() < st.fence))
    { ++g_busy_skips; return; }

    if (!g_up && !ensure_feature(cl)) return;   // turned the key off for the arm on failure

    // real = copy of nr_final (UAV -> COPY_SOURCE -> UAV; NPSR -> COPY_DEST -> NPSR)
    transition(cl, nr_final, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(cl, st.real, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    cl->CopyResource(st.real, nr_final);
    transition(cl, st.real, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(cl, nr_final, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    st.frame = f; st.has_gen = false; st.handed = false;

    if (mvec == nullptr || depth == nullptr)
    {
        ++g_nomv;   // no vectors (or depth) this frame: the real frame alone; the next pair resets
        g_last_fid = 0ull;
    }
    else if (!g_live.load(std::memory_order_relaxed))
    {
        // [PORT FG-PANEL] hot OFF: no Evaluate, no gen frame. The real frame is
        // already copied above, so the pacing thread presents it normally. The
        // pair history is dropped so switching back on starts a fresh pair.
        ++g_hot_off_skips;
        g_last_fid = 0ull;
    }
    else
    {
        const unsigned long long fid = ++g_fid;
        const bool reset = (g_last_fid == 0ull) || (fid != g_last_fid + 1ull);
        g_last_fid = fid;
        // [PORT H5, 2026-11-11] HOW MANY GENERATED FRAMES THIS SOURCE FRAME CARRIES.
        //
        // mgpu::fgmode encodes GENERATED frames per real frame (1 = 2x, 2 = 3x, 3 = 4x) -
        // see fg_mode.hpp's own encoding note, and gpu1_context.cpp's [R446] line is what
        // put the panel's FgMulti into that form. The provider's MultiFrameCountMax is the
        // ceiling, MAX_GEN is this array's own bound. 2X is mfc = 1 and the loop below
        // runs exactly once - the shipped path, unchanged.
        unsigned mfc = (unsigned)mgpu::fgmode::get_multi();   // generated frames per real frame
        if (mfc < 1u) mfc = 1u;
        if (mfc > MAX_GEN) mfc = MAX_GEN;
        if (g_mfc_max != 0u && mfc > g_mfc_max) mfc = g_mfc_max;
        static unsigned said_mfc = ~0u;
        if (said_mfc != mfc)
        {
            said_mfc = mfc;
            char l[260];
            snprintf(l, sizeof l, "[MGPU][R341] multi-frame: %ux (%u generated frame(s) per source frame, "
                     "provider ceiling %u, panel asks %d)", mfc + 1u, mfc, g_mfc_max, mgpu::fgmode::get_multi());
            mgpu::diag::info(l);
        }
        st.gen_n = 0u;
        unsigned made = 0u;
        for (unsigned gi = 0u; gi < mfc; ++gi)
        {
            const NVSDK_NGX_Result r = evaluate_mf(cl, st.real, mvec, depth, st.gen[gi], mv_el, reset, fid, mfc, gi + 1u);
            ++g_evals;
            if (r != NVSDK_NGX_Result_Success)
            {
                ++g_errors;
                char l[320];
                snprintf(l, sizeof l, "[MGPU][R341] frame %llu: EvaluateFeature(FrameGeneration) index %u/%u 0x%08X "
                         "(%llu error(s) so far)%s", fid, gi + 1u, mfc, (unsigned)r, g_errors,
                         g_errors >= 3ull ? " - FrameGen=4 OFF for this arm." : ".");
                mgpu::diag::error(l);
                if (g_errors >= 3ull) { g_on.store(false, std::memory_order_release); g_failed = true; }
                made = 0u;
                break;
            }
            ++made;
        }
        // A reset frame has no history, so its generated pictures are not presented (the
        // author's rule, kept): the real frame alone goes out and the next pair is clean.
        // mfc == 0 must NOT arm a generated frame either: the loop ran zero times, and
        // presenting gen[0] after that would show a texture nobody wrote (measured shape:
        // fgmode forces multi=1 for the FSR backend, so this is reachable in one key edit).
        if (!reset && mfc >= 1u && made == mfc) { st.has_gen = true; st.gen_n = mfc; g_made += mfc; }
        else st.gen_n = 0u;
    }
    g_k = (g_k + 1u) % SETS;
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
    g_last_handoff = now;
    job_t job; job.set = &st; job.gen = st.has_gen; job.r = r; job.g = g; job.b = b; job.t0 = now;
    job.gen_n = st.gen_n;
    job.half_ms = (g_ema_ms > 0.0) ? g_ema_ms * 0.5 : 8.0;
    // [H5] one step = one presented frame's share of the source interval. For 2X this IS
    // half_ms, so the shipped cadence is reproduced exactly.
    {
        const unsigned nf = (job.gen_n != 0u) ? (job.gen_n + 1u) : 1u;
        job.step_ms = (g_ema_ms > 0.0) ? g_ema_ms / (double)nf : 8.0;
    }
    g_half_sum += job.half_ms; ++g_half_n;
    st.busy.store(true, std::memory_order_release);
    {
        std::unique_lock<std::mutex> lk(g_mx);
        if (g_thread == nullptr) { g_stop = false; g_thread = new std::thread(thread_main); }
        if (g_qn == JOBQ)
        {
            // full: drop the OLDEST job - it is the one the output clock has already
            // passed, and its set must be released or record() will skip that set forever.
            g_q[0].set->busy.store(false, std::memory_order_release);
            g_dropped.fetch_add(1ull);
            for (unsigned qi = 1u; qi < g_qn; ++qi) g_q[qi - 1u] = g_q[qi];
            --g_qn;
        }
        g_q[g_qn++] = job;
        if (g_qn > g_qpeak) g_qpeak = g_qn;
    }
    g_cv.notify_one();
    const unsigned long long n = g_handoffs.fetch_add(1ull, std::memory_order_relaxed) + 1ull;
    if (n % 1200ull == 0ull)
    {
        char l[520];
        snprintf(l, sizeof l, "[MGPU][R341] FrameGen=4 after %llu handoffs: generated %llu | presented gen %llu real %llu | dropped (thread busy) %llu | "
                 "skipped: set not retired %llu, no vectors/depth %llu | evaluate errors %llu | half interval mean %.2f ms (NR interval %.2f ms)",
                 n, g_made, g_pres_gen.load(), g_pres_real.load(), g_dropped.load(), g_busy_skips, g_nomv, g_errors,
                 g_half_n ? g_half_sum / g_half_n : 0.0, g_ema_ms);
        mgpu::diag::info(l);
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
    { std::lock_guard<std::mutex> lk(g_mx); for (unsigned qi = 0u; qi < g_qn; ++qi) g_q[qi].set->busy.store(false, std::memory_order_release); g_qn = 0u; }
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
    if (g_handle != nullptr && g_rel != nullptr) g_rel(g_handle);
    g_handle = nullptr;
    g_params = nullptr;   // the parameter block belongs to the NGX session, which the add-on never shuts down (V48)
    free_sets();
    if (g_timer != nullptr) { CloseHandle(g_timer); g_timer = nullptr; }
    if (was)
    {
        char l[400];
        snprintf(l, sizeof l, "[MGPU][R341] FrameGen=4 released: generated %llu, presented gen %llu real %llu, dropped %llu, skipped %llu/%llu, errors %llu.",
                 g_made, g_pres_gen.load(), g_pres_real.load(), g_dropped.load(), g_busy_skips, g_nomv, g_errors);
        mgpu::diag::info(l);
    }
    g_armed = false; g_up = false; g_tried = false;
}

// [PORT FG-PANEL] read-only counters for the panel / overlay.
// [PORT FG-PANEL] the hot switch, read by the panel and by record().
void ui_set_enabled(bool on)
{
    const bool was = g_live.exchange(on, std::memory_order_acq_rel);
    if (was == on) return;
    mgpu::diag::info(on
        ? "[MGPU][R341] HOT SWITCH: frame generation ENABLED again - generation resumes on the next frame with a "
          "fresh pair (the history across the pause is not a valid pair)."
        : "[MGPU][R341] HOT SWITCH: frame generation DISABLED - the real frame keeps being copied and presented by "
          "the same pacing thread; the feature is kept, so switching back on needs no restart.");
}
bool ui_enabled() { return g_live.load(std::memory_order_relaxed); }
bool ui_up() { return g_up; }

// [PORT C] the readout toggle, for the panel and the ini key.
void ui_set_hud(bool on) { g_hud.store(on, std::memory_order_relaxed); mgpu::fghud::force_refresh(); }
bool ui_hud() { return g_hud.load(std::memory_order_relaxed); }

// [PORT DISPLAY-LAYER 1/3] the landing ledger, read-only. mean/worst are ms.
void ui_landing(double &mean, double &worst, unsigned long long &n, unsigned long long &late)
{
    n = g_land_n; late = g_land_late; worst = g_land_worst_ms;
    mean = (g_land_n != 0ull) ? (g_land_sum_g / (double)g_land_n) : 0.0;
}

// [PORT DISPLAY-LAYER 2/3] pace state, read-only for the panel.
void ui_pace(double &interval_ms, unsigned long long &resync, unsigned long long &abs_frames, int &mode)
{
    interval_ms = g_int_ms; resync = g_resync; abs_frames = g_abs_n; mode = g_abs ? 1 : 0;
}

void ui_status(unsigned long long &gen_made, unsigned long long &gen_presented,
               unsigned long long &real_presented, unsigned long long &dropped,
               unsigned long long &errors)
{
    gen_made = g_made;
    gen_presented = g_pres_gen.load(std::memory_order_relaxed);
    real_presented = g_pres_real.load(std::memory_order_relaxed);
    dropped = g_dropped.load(std::memory_order_relaxed);
    errors = g_errors;
}
}
