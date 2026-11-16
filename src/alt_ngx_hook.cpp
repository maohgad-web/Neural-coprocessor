// MGPU Bridge - R377 (0.5.1 discovery): the alternative NGX route. See alt_ngx_hook.hpp.
// Method and original code: N01950 (ngx_hook.cpp of the PR "non-native frame generation").
#include <windows.h>
#include <d3d12.h>
#include <intrin.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <utility>
#include <vector>

#include "alt_ngx_hook.hpp"
#include "calibrator.hpp"
#include "diag.hpp"
#include "mgpu_ini_parser.hpp"

#if defined(MGPU_ALT_HOOKS)
#include "safetyhook.hpp"
#include "../ext/ngx/nvsdk_ngx.h"
#endif

namespace mgpu
{
namespace altngx
{
namespace
{
std::atomic<bool> g_want{false};
std::atomic<bool> g_live{false};
int g_packed = 0;

bool key_is_one()
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
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%lsmgpu.ini", mod);
    FILE *f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || f == nullptr) return false;
    std::vector<char> buf(mgpu::config::MAX_BYTES + 2u, '\0');
    const size_t got = fread(buf.data(), 1, mgpu::config::MAX_BYTES + 1u, f);
    fclose(f);
    if (got > mgpu::config::MAX_BYTES) return false;
    buf[got] = '\0';
    const char *v = mgpu::config::find(buf.data(), got, "NgxHook");
    return v != nullptr && atoi(v) == 1;
}

#if defined(MGPU_ALT_HOOKS)
using pf_create = NVSDK_NGX_Result (NVSDK_CONV *)(ID3D12GraphicsCommandList *, NVSDK_NGX_Feature,
                                                  NVSDK_NGX_Parameter *, NVSDK_NGX_Handle **);
using pf_eval = NVSDK_NGX_Result (NVSDK_CONV *)(ID3D12GraphicsCommandList *, const NVSDK_NGX_Handle *,
                                                const NVSDK_NGX_Parameter *, void *);
using pf_release = NVSDK_NGX_Result (NVSDK_CONV *)(NVSDK_NGX_Handle *);

safetyhook::InlineHook g_h_create{};
safetyhook::InlineHook g_h_eval{};
safetyhook::InlineHook g_h_release{};
// What a detour calls for "the real function": the trampoline of a patched entry, or the entry itself when
// that entry was not patched. Written before the hooks are enabled, never after.
pf_create  g_t_create = nullptr;
pf_eval    g_t_eval = nullptr;
pf_release g_t_release = nullptr;

uintptr_t g_self_lo = 0, g_self_hi = 0;   // the bridge's own image: its NGX calls are not the game's
std::atomic<bool> g_busy{false};
std::atomic<int> g_tries{0};

std::atomic<unsigned long long> g_n_create{0}, g_n_eval{0}, g_n_release{0}, g_n_own{0}, g_n_late_latch{0};

// Features whose CREATE went through the hook (N01950's registry). A handle that is not here was created
// before the hook was live; only such a handle is a candidate for the late latch below.
constexpr int kMaxFeatures = 8;
std::atomic<unsigned long long> g_reg_handle[kMaxFeatures]{};

bool reg_has(unsigned long long h)
{
    for (int i = 0; i < kMaxFeatures; ++i)
        if (g_reg_handle[i].load(std::memory_order_relaxed) == h) return true;
    return false;
}
void reg_add(unsigned long long h)
{
    if (h == 0ull || reg_has(h)) return;
    for (int i = 0; i < kMaxFeatures; ++i)
    {
        unsigned long long expect = 0ull;
        if (g_reg_handle[i].compare_exchange_strong(expect, h, std::memory_order_relaxed)) return;
    }
}
void reg_del(unsigned long long h)
{
    for (int i = 0; i < kMaxFeatures; ++i)
    {
        unsigned long long expect = h;
        if (g_reg_handle[i].compare_exchange_strong(expect, 0ull, std::memory_order_relaxed)) return;
    }
}

inline bool own_call(void *ret)
{
    const uintptr_t ra = reinterpret_cast<uintptr_t>(ret);
    return ra >= g_self_lo && ra < g_self_hi;
}

NVSDK_NGX_Result NVSDK_CONV det_create(ID3D12GraphicsCommandList *cmd, NVSDK_NGX_Feature id,
                                       NVSDK_NGX_Parameter *params, NVSDK_NGX_Handle **out)
{
    if (own_call(_ReturnAddress()))
    {
        g_n_own.fetch_add(1ull, std::memory_order_relaxed);
        return g_t_create(cmd, id, params, out);
    }
    g_n_create.fetch_add(1ull, std::memory_order_relaxed);
    const NVSDK_NGX_Result r = (NVSDK_NGX_Result)mgpu::calibrator::external_create((void *)cmd, (int)id, (void *)params, (void **)out);
    if (r == NVSDK_NGX_Result_Success && out != nullptr && *out != nullptr)
        reg_add((unsigned long long)(uintptr_t)*out);
    return r;
}

NVSDK_NGX_Result NVSDK_CONV det_eval(ID3D12GraphicsCommandList *cmd, const NVSDK_NGX_Handle *h,
                                     const NVSDK_NGX_Parameter *p, void *cb)
{
    if (own_call(_ReturnAddress()))
    {
        g_n_own.fetch_add(1ull, std::memory_order_relaxed);
        return g_t_eval(cmd, h, p, cb);
    }
    g_n_eval.fetch_add(1ull, std::memory_order_relaxed);
    const unsigned long long hv = (unsigned long long)(uintptr_t)h;
    // N01950's late latch: a feature created BEFORE the hook was live never passed the create, so the calibrator
    // does not know it. If its block carries depth or vectors and is not the frame generator's, it is a scene
    // feature: latch it, so the calibrator's own rules (copy only from an identified handle) can apply.
    if (p != nullptr && hv != 0ull && !reg_has(hv) && !mgpu::calibrator::external_scene_known(hv))
    {
        NVSDK_NGX_Parameter *q = const_cast<NVSDK_NGX_Parameter *>(p);
        ID3D12Resource *pr = nullptr;
        const bool is_fg = q->Get("DLSSG.Backbuffer", &pr) == NVSDK_NGX_Result_Success;
        pr = nullptr;
        const bool has_depth = q->Get(NVSDK_NGX_Parameter_Depth, &pr) == NVSDK_NGX_Result_Success && pr != nullptr;
        pr = nullptr;
        const bool has_mv = q->Get(NVSDK_NGX_Parameter_MotionVectors, &pr) == NVSDK_NGX_Result_Success && pr != nullptr;
        if (!is_fg && (has_depth || has_mv))
        {
            mgpu::calibrator::external_note_scene(hv);
            if (g_n_late_latch.fetch_add(1ull, std::memory_order_relaxed) == 0ull)
            {
                char l[300];
                snprintf(l, sizeof l, "[MGPU][R377] late latch: feature 0x%llx was created before the hook was live; its "
                         "evaluate carries %s%s and is not frame generation, so it is taken as the scene feature.",
                         hv, has_depth ? "depth " : "", has_mv ? "vectors" : "");
                mgpu::diag::info(l);
            }
        }
    }
    return (NVSDK_NGX_Result)mgpu::calibrator::external_evaluate((void *)cmd, (const void *)h, (const void *)p, cb);
}

NVSDK_NGX_Result NVSDK_CONV det_release(NVSDK_NGX_Handle *h)
{
    if (own_call(_ReturnAddress()))
    {
        g_n_own.fetch_add(1ull, std::memory_order_relaxed);
        return g_t_release(h);
    }
    g_n_release.fetch_add(1ull, std::memory_order_relaxed);
    reg_del((unsigned long long)(uintptr_t)h);
    return (NVSDK_NGX_Result)mgpu::calibrator::external_release((void *)h);
}

void refuse(const char *why)
{
    g_h_create.reset(); g_h_eval.reset(); g_h_release.reset();
    g_want.store(false, std::memory_order_release);   // stops the per-present attempts
    char l[520];
    snprintf(l, sizeof l, "[MGPU][R377] NgxHook=1 REFUSED: %s. _nvngx.dll is as it was. NO NGX ROUTE IS ACTIVE in this "
             "session: NgxHook=1 asks for the calibrator's own install not to run, so it has not run and will not. "
             "The game's DLSS table, the copy of the vectors at the evaluate and DepthSource=1 are not available. "
             "Remove NgxHook from mgpu.ini to use the calibrator.", why);
    mgpu::diag::error(l);
}
#endif   // MGPU_ALT_HOOKS
}   // namespace

bool wanted() { return g_want.load(std::memory_order_relaxed); }
bool pending() { return g_want.load(std::memory_order_relaxed) && !g_live.load(std::memory_order_relaxed); }

void init(int calib_packed)
{
    g_packed = calib_packed;
    if (!key_is_one()) return;
#if defined(MGPU_ALT_HOOKS)
    if ((calib_packed & 0xFF) == 0)
    {   // R382: N01950's own instruction for this route is "when enabled, use Calib=0" (PR text, section 3): Calib=0
        // there means "no install rungs". The rungs never run on this route anyway, so the capture is armed live (2).
        g_packed = (calib_packed & ~0xFF) | 2;
        mgpu::diag::info("[MGPU][R377] NgxHook=1 with Calib=0: Calib=0 keeps the calibrator's own install off, which this route "
                         "does in any case. The capture itself is armed (live), fed by the hook.");
    }
    static const int anchor = 0;
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&anchor), &self) || self == nullptr)
    {
        mgpu::diag::error("[MGPU][R377] NgxHook=1: the bridge's own image could not be found, so its own NGX calls could "
                          "not be told from the game's. Nothing is installed; the calibrator installs as usual.");
        return;
    }
    const unsigned char *base = reinterpret_cast<const unsigned char *>(self);
    const IMAGE_DOS_HEADER *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    const IMAGE_NT_HEADERS *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    g_self_lo = reinterpret_cast<uintptr_t>(base);
    g_self_hi = g_self_lo + nt->OptionalHeader.SizeOfImage;
    g_want.store(true, std::memory_order_release);
    mgpu::diag::info("[MGPU][R377] NgxHook=1: the alternative NGX route (N01950's inline hook on three exports of "
                     "_nvngx.dll). The calibrator's import-table swap and data scan do NOT run in this session. "
                     "The hook installs when _nvngx.dll is loaded; until then one check per present.");
    try_install();
#else
    mgpu::diag::warn("[MGPU][R377] NgxHook=1 is in mgpu.ini but THIS BUILD DOES NOT CARRY the alternative route "
                     "(SafetyHook was not fetched when the build was configured). The key is ignored; the calibrator "
                     "installs as usual.");
#endif
}

void try_install()
{
#if defined(MGPU_ALT_HOOKS)
    if (!pending()) return;
    if (g_busy.exchange(true, std::memory_order_acquire)) return;
    struct unbusy { ~unbusy() { g_busy.store(false, std::memory_order_release); } } ub;

    HMODULE core = GetModuleHandleW(L"_nvngx.dll");
    if (core == nullptr)
    {
        if (g_tries.fetch_add(1, std::memory_order_relaxed) == 0)
            mgpu::diag::info("[MGPU][R377] _nvngx.dll is not loaded yet - it loads with the game's DLSS. Checked again "
                             "on every present until it is there. Said once.");
        return;
    }
    void *e_create = reinterpret_cast<void *>(GetProcAddress(core, "NVSDK_NGX_D3D12_CreateFeature"));
    void *e_eval = reinterpret_cast<void *>(GetProcAddress(core, "NVSDK_NGX_D3D12_EvaluateFeature"));
    void *e_release = reinterpret_cast<void *>(GetProcAddress(core, "NVSDK_NGX_D3D12_ReleaseFeature"));
    if (e_eval == nullptr) { refuse("_nvngx.dll exports no NVSDK_NGX_D3D12_EvaluateFeature"); return; }

    // Created DISABLED: the entries are patched only by enable(), after the calibrator has the trampolines.
    auto he = safetyhook::InlineHook::create(e_eval, reinterpret_cast<void *>(&det_eval), safetyhook::InlineHook::StartDisabled);
    if (!he) { refuse("SafetyHook could not build a trampoline for EvaluateFeature"); return; }
    g_h_eval = std::move(*he);
    if (e_create != nullptr)
    {
        auto hc = safetyhook::InlineHook::create(e_create, reinterpret_cast<void *>(&det_create), safetyhook::InlineHook::StartDisabled);
        if (hc) g_h_create = std::move(*hc);
    }
    if (e_release != nullptr)
    {
        auto hr = safetyhook::InlineHook::create(e_release, reinterpret_cast<void *>(&det_release), safetyhook::InlineHook::StartDisabled);
        if (hr) g_h_release = std::move(*hr);
    }
    g_t_eval = g_h_eval.original<pf_eval>();
    g_t_create = g_h_create ? g_h_create.original<pf_create>() : reinterpret_cast<pf_create>(e_create);
    g_t_release = g_h_release ? g_h_release.original<pf_release>() : reinterpret_cast<pf_release>(e_release);

    if (!mgpu::calibrator::arm_external(g_packed, reinterpret_cast<void *>(g_t_create), reinterpret_cast<void *>(g_t_eval),
                                        reinterpret_cast<void *>(g_t_release)))
    {
        refuse("the calibrator was already installed by its own route, or Calib=0");
        return;
    }
    const bool ok_e = g_h_eval.enable().has_value();
    const bool ok_c = g_h_create ? g_h_create.enable().has_value() : false;
    const bool ok_r = g_h_release ? g_h_release.enable().has_value() : false;
    if (!ok_e) { refuse("EvaluateFeature's entry could not be patched"); return; }
    g_live.store(true, std::memory_order_release);
    char l[620];
    snprintf(l, sizeof l,
             "[MGPU][R377] HOOK LIVE on _nvngx.dll %p: CreateFeature=%s EvaluateFeature=YES ReleaseFeature=%s. The game's "
             "NGX calls now reach the calibrator's own hook functions through these entries; the bridge's own calls "
             "(image %p-%p) go straight to the real functions. No import slot and no data word of any module was "
             "written. Method: N01950.",
             (void *)core, ok_c ? "YES" : "no", ok_r ? "YES" : "no", (void *)g_self_lo, (void *)g_self_hi);
    mgpu::diag::info(l);
#endif
}

void uninstall()
{
#if defined(MGPU_ALT_HOOKS)
    if (!g_live.exchange(false, std::memory_order_acq_rel)) return;
    g_want.store(false, std::memory_order_release);
    g_h_create.reset(); g_h_eval.reset(); g_h_release.reset();   // the three entries get their bytes back
    char l[360];
    snprintf(l, sizeof l, "[MGPU][R377] hook removed. Totals: game creates %llu, evaluates %llu, releases %llu | the bridge's "
             "own calls passed straight through %llu | late latches %llu.",
             g_n_create.load(std::memory_order_relaxed), g_n_eval.load(std::memory_order_relaxed),
             g_n_release.load(std::memory_order_relaxed), g_n_own.load(std::memory_order_relaxed),
             g_n_late_latch.load(std::memory_order_relaxed));
    mgpu::diag::info(l);
#endif
}
}   // namespace altngx
}   // namespace mgpu
