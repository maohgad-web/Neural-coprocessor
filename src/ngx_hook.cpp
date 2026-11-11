// MGPU Bridge - [PORT B2, 2026-11-11] the NGX contract from nvngx's own entry.
//
// PROVENANCE. The mechanics (SafetyHook inline detours, per-feature registry,
// scene filter, once-per-frame gate) are the nc-base implementation
// (ngx_hook.cpp there, R518zp..R522). This file is that code with everything
// that belonged to the fork's later layers removed: no provider adapter, no
// contract snapshot, no vector funnel note, no dump/scan instruments. What is
// left is what the contract needs: patch three exports, read the block, latch
// the scene handle, hand the block to calibrator::evaluate_external.
//
// WHY THE HOOK TARGETS _nvngx.dll AND NOT nvngx.dll: _nvngx is the driver's
// core; nvngx.dll (the app wrapper) forwards to it. Patching the core means
// every caller - the game, Streamline, anything - passes through it, and it is
// the same module the calibrator's IAT route reaches.
//
// THE DEPTH QUESTION. Yes: this hook captures depth as well as motion vectors.
// The block it reads is the game's DLSS block, which carries DLSS.Depth and
// its extent alongside DLSS.MotionVectors; capture() stores both in the same
// table (KEY_DEPTH / KEY_MVEC) in the same call. There is no separate depth
// hook and no separate capture.
//
// SAFETY. Every detour body is SEH-wrapped: it also runs during init and
// pipeline creation, and a half-wrapped hook corrupts the state we are
// reading. The real call always runs, whatever the instrument does.
#include <windows.h>
#include <d3d12.h>

#include <atomic>
#include <cstdint>
#include <cstdio>

#include "ngx_hook.hpp"
#include "calibrator.hpp"
#include "diag.hpp"
#include "safetyhook.hpp"
#include "../ext/ngx/nvsdk_ngx.h"

namespace mgpu
{
namespace ngxhook
{
namespace
{
using pf_create = NVSDK_NGX_Result (NVSDK_CONV *)(ID3D12GraphicsCommandList *, NVSDK_NGX_Feature,
                                                  NVSDK_NGX_Parameter *, NVSDK_NGX_Handle **);
using pf_eval = NVSDK_NGX_Result (NVSDK_CONV *)(ID3D12GraphicsCommandList *, const NVSDK_NGX_Handle *,
                                                const NVSDK_NGX_Parameter *, void *);
using pf_release = NVSDK_NGX_Result (NVSDK_CONV *)(NVSDK_NGX_Handle *);

safetyhook::InlineHook g_h_create{};
safetyhook::InlineHook g_h_eval{};
safetyhook::InlineHook g_h_release{};
pf_create  g_real_create = nullptr;
pf_eval    g_real_eval = nullptr;
pf_release g_real_release = nullptr;

std::atomic<bool> g_want{false};
std::atomic<bool> g_done{false};
std::atomic<int>  g_tries{0};

std::atomic<unsigned long long> g_n_create{0};
std::atomic<unsigned long long> g_n_eval{0};
std::atomic<unsigned long long> g_n_scene{0};
std::atomic<unsigned long long> g_n_skip_other{0};
std::atomic<unsigned long long> g_n_skip_own{0};
std::atomic<unsigned long long> g_n_no_handle{0};

// handle -> feature id, filled at CreateFeature and cleared at Release. The
// evaluate path looks the handle up; only the SuperSampling bucket may become
// the contract, so the frame-generation feature (id 11, one evaluate per
// generated frame) can never overwrite the scene's.
constexpr int kMaxFeatures = 8;
std::atomic<unsigned long long> g_reg_handle[kMaxFeatures]{};
std::atomic<int> g_reg_fid[kMaxFeatures]{};

// Once per frame, for the scene evaluate only. g_frames is the bridge's own
// counter (calibrator::frame_now_external), so "once per frame" here means the
// same frame the rest of the pipeline numbers.
std::atomic<unsigned long long> g_last_scene_frame{~0ull};

int fid_of(unsigned long long h)
{
    for (int i = 0; i < kMaxFeatures; ++i)
        if (g_reg_handle[i].load(std::memory_order_relaxed) == h)
            return g_reg_fid[i].load(std::memory_order_relaxed);
    return -1;
}

void reg_add(unsigned long long h, int fid)
{
    for (int i = 0; i < kMaxFeatures; ++i)
    {
        unsigned long long expect = 0ull;
        if (g_reg_handle[i].compare_exchange_strong(expect, h, std::memory_order_relaxed))
        {
            g_reg_fid[i].store(fid, std::memory_order_relaxed);
            return;
        }
        if (expect == h) { g_reg_fid[i].store(fid, std::memory_order_relaxed); return; }
    }
}

void reg_del(unsigned long long h)
{
    for (int i = 0; i < kMaxFeatures; ++i)
        if (g_reg_handle[i].load(std::memory_order_relaxed) == h)
        {
            g_reg_handle[i].store(0ull, std::memory_order_relaxed);
            g_reg_fid[i].store(0, std::memory_order_relaxed);
            return;
        }
}

// True when the block is OUR DLSS-NR feature's (the bridge's own evaluate),
// not the game's contract. The NR feature carries DLSSNR.* parameters; a
// scene block does not.
bool is_own_block(const NVSDK_NGX_Parameter *p)
{
    if (p == nullptr) return false;
    float probe = 0.0f;
    return const_cast<NVSDK_NGX_Parameter *>(p)->Get("DLSSNR.MVecScaleX", &probe) ==
           NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result NVSDK_CONV det_create(ID3D12GraphicsCommandList *cmd, NVSDK_NGX_Feature id,
                                       NVSDK_NGX_Parameter *params, NVSDK_NGX_Handle **out)
{
    const unsigned long long n = g_n_create.fetch_add(1ull, std::memory_order_relaxed);
    if (n == 0ull)
        mgpu::diag::info("[MGPU][R803] NGX inline hook: CreateFeature entered (first one) - the contract "
                         "route is live");

    NVSDK_NGX_Result r = NVSDK_NGX_Result_Fail;
    __try
    {
        r = g_h_create ? g_h_create.call<NVSDK_NGX_Result>(cmd, id, params, out)
                       : (g_real_create ? g_real_create(cmd, id, params, out) : NVSDK_NGX_Result_Fail);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        mgpu::diag::warn("[MGPU][R803] CreateFeature detour raised - the real call's failure is reported as is");
        r = NVSDK_NGX_Result_Fail;
    }

    __try
    {
        if (r == NVSDK_NGX_Result_Success && out != nullptr && *out != nullptr)
        {
            const unsigned long long hv = (unsigned long long)(uintptr_t)*out;
            reg_add(hv, (int)id);
            // fgmap learn, the R134 census, the scene latch AND the create flags
            // (DepthInverted is bit 3). A title whose evaluate block carries no
            // flags still gets them, because they are a CREATE-time fact.
            mgpu::calibrator::after_create_external((int)id, (void *)params, (void **)out, (int)r);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        mgpu::diag::warn("[MGPU][R803] CreateFeature bookkeeping raised - the create itself stands");
    }
    return r;
}

NVSDK_NGX_Result NVSDK_CONV det_eval(ID3D12GraphicsCommandList *cmd, const NVSDK_NGX_Handle *h,
                                     const NVSDK_NGX_Parameter *p, void *cb)
{
    __try
    {
        g_n_eval.fetch_add(1ull, std::memory_order_relaxed);
        const unsigned long long hv = (unsigned long long)(uintptr_t)h;
        const int fid = fid_of(hv);
        bool scene = (fid == (int)NVSDK_NGX_Feature_SuperSampling) ||
                     (fid < 0 && mgpu::calibrator::is_scene_handle_external(hv));
        // The hook can be installed AFTER the game created its DLSS feature (the
        // module loads with the game's DLSS, which can be after our first
        // present). Classify the block itself in that case: a DLSS-SR block
        // carries the scene's depth or motion vectors and neither DLSSG.* nor
        // DLSSNR.*. Adopting it here is what makes a late install work at all.
        if (fid < 0 && !scene)
        {
            ID3D12Resource *pr = nullptr;
            const bool has_depth = p != nullptr &&
                const_cast<NVSDK_NGX_Parameter *>(p)->Get(NVSDK_NGX_Parameter_Depth, &pr) ==
                    NVSDK_NGX_Result_Success && pr != nullptr;
            pr = nullptr;
            const bool has_mv = p != nullptr &&
                const_cast<NVSDK_NGX_Parameter *>(p)->Get(NVSDK_NGX_Parameter_MotionVectors, &pr) ==
                    NVSDK_NGX_Result_Success && pr != nullptr;
            pr = nullptr;
            const bool is_fg = p != nullptr &&
                const_cast<NVSDK_NGX_Parameter *>(p)->Get("DLSSG.Backbuffer", &pr) ==
                    NVSDK_NGX_Result_Success;
            if (!is_own_block(p) && !is_fg && (has_depth || has_mv))
            {
                mgpu::calibrator::note_scene_handle_external(hv);
                scene = true;
            }
        }
        if (!scene)
        {
            g_n_skip_other.fetch_add(1ull, std::memory_order_relaxed);
        }
        else if (is_own_block(p))
        {
            g_n_skip_own.fetch_add(1ull, std::memory_order_relaxed);
        }
        else
        {
            const unsigned long long f = mgpu::calibrator::frame_now_external();
            const unsigned long long last = g_last_scene_frame.exchange(f, std::memory_order_relaxed);
            if (last != f)
            {
                // Before the real evaluate: the caller has finished writing the
                // buffers, which is why it is handing them over.
                mgpu::calibrator::evaluate_external(cmd, h, p);
                g_n_scene.fetch_add(1ull, std::memory_order_relaxed);
            }
        }
        if (fid < 0 && !scene) g_n_no_handle.fetch_add(1ull, std::memory_order_relaxed);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        mgpu::diag::warn("[MGPU][R803] EvaluateFeature detour raised - the real call still runs");
    }

    if (g_h_eval) return g_h_eval.call<NVSDK_NGX_Result>(cmd, h, p, cb);
    return g_real_eval ? g_real_eval(cmd, h, p, cb) : NVSDK_NGX_Result_Fail;
}

NVSDK_NGX_Result NVSDK_CONV det_release(NVSDK_NGX_Handle *h)
{
    const unsigned long long hv = (unsigned long long)(uintptr_t)h;
    const NVSDK_NGX_Result r = g_h_release ? g_h_release.call<NVSDK_NGX_Result>(h)
                                           : (g_real_release ? g_real_release(h) : NVSDK_NGX_Result_Fail);
    __try
    {
        reg_del(hv);
        mgpu::calibrator::release_scene_handle_external(hv);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        mgpu::diag::warn("[MGPU][R803] ReleaseFeature bookkeeping raised - the release itself stands");
    }
    return r;
}

void log_stats()
{
    char l[420];
    snprintf(l, sizeof l,
             "[MGPU][R803] hook stats: creates=%llu evaluates=%llu scene-captured=%llu "
             "skip(other feature)=%llu skip(our NR feature)=%llu unknown-handle=%llu",
             (unsigned long long)g_n_create.load(std::memory_order_relaxed),
             (unsigned long long)g_n_eval.load(std::memory_order_relaxed),
             (unsigned long long)g_n_scene.load(std::memory_order_relaxed),
             (unsigned long long)g_n_skip_other.load(std::memory_order_relaxed),
             (unsigned long long)g_n_skip_own.load(std::memory_order_relaxed),
             (unsigned long long)g_n_no_handle.load(std::memory_order_relaxed));
    mgpu::diag::info(l);
}
}   // namespace

bool installed() { return g_done.load(std::memory_order_acquire); }
bool wanted()    { return g_want.load(std::memory_order_relaxed); }

bool install(int want)
{
    g_want.store(want != 0, std::memory_order_relaxed);
    if (want == 0) return false;
    if (g_done.load(std::memory_order_acquire)) return true;

    HMODULE core = GetModuleHandleW(L"_nvngx.dll");
    if (core == nullptr)
    {
        const int n = g_tries.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 3)
            mgpu::diag::info("[MGPU][R803] _nvngx.dll is not resident yet - the hook retries on the next "
                             "present (the module loads with the game's DLSS, which can be after us)");
        return false;
    }

    g_real_create  = (pf_create)GetProcAddress(core, "NVSDK_NGX_D3D12_CreateFeature");
    g_real_eval    = (pf_eval)GetProcAddress(core, "NVSDK_NGX_D3D12_EvaluateFeature");
    g_real_release = (pf_release)GetProcAddress(core, "NVSDK_NGX_D3D12_ReleaseFeature");
    if (g_real_eval == nullptr)
    {
        mgpu::diag::error("[MGPU][R803] REFUSED: _nvngx.dll has no D3D12 EvaluateFeature - nothing is "
                          "patched; the calibrator stays the only contract route");
        return false;
    }

    // Never hook the same entry twice: a retry after a partial failure (create
    // hooked, evaluate refused) would otherwise re-patch create, and two
    // InlineHooks over one prologue is how a trampoline becomes garbage.
    if (g_real_create != nullptr && !static_cast<bool>(g_h_create))
        g_h_create = safetyhook::create_inline((void *)g_real_create, (void *)&det_create);
    if (!static_cast<bool>(g_h_eval))
        g_h_eval = safetyhook::create_inline((void *)g_real_eval, (void *)&det_eval);
    if (g_real_release != nullptr && !static_cast<bool>(g_h_release))
        g_h_release = safetyhook::create_inline((void *)g_real_release, (void *)&det_release);

    if (!static_cast<bool>(g_h_eval))
    {
        mgpu::diag::error("[MGPU][R803] REFUSED: SafetyHook could not patch EvaluateFeature - _nvngx.dll "
                          "is untouched; the calibrator stays the only contract route. NOT retried: the "
                          "image cannot become patchable later in the same process.");
        g_want.store(false, std::memory_order_relaxed);   // stop the per-present retry loop
        return false;
    }

    g_done.store(true, std::memory_order_release);
    char l[560];
    snprintf(l, sizeof l,
             "[MGPU][R803] HOOK LIVE on _nvngx.dll %p: create=%s eval=YES release=%s. The contract "
             "(colour, depth, vectors, extents, flags, jitter, MVecScale) now arrives at nvngx's own "
             "entry; no other module's writable data is written. Ported from nc-base R518zp.",
             (void *)core,
             static_cast<bool>(g_h_create) ? "YES" : "no",
             static_cast<bool>(g_h_release) ? "YES" : "no");
    mgpu::diag::info(l);
    log_stats();
    return true;
}
}   // namespace ngxhook
}   // namespace mgpu
