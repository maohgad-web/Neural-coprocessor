// MGPU Bridge - R381 (0.5.1 discovery): the FSR contract route. See alt_ffx_hook.hpp.
// Method and original code: N01950 (ffx_hook.cpp of the PR "non-native frame generation").
#include <windows.h>
#include <d3d12.h>
#include <intrin.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <utility>
#include <vector>

#include "alt_ffx_hook.hpp"
#include "calibrator.hpp"
#include "depth_src.hpp"
#include "diag.hpp"
#include "gpu1_context.hpp"
#include "mgpu_ini_parser.hpp"

#if defined(MGPU_ALT_HOOKS)
#include "safetyhook.hpp"
#endif

namespace mgpu
{
namespace altffx
{
namespace
{
std::atomic<bool> g_want{false};
std::atomic<bool> g_live{false};

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
    const char *v = mgpu::config::find(buf.data(), got, "FfxHook");
    return v != nullptr && atoi(v) == 1;
}

#if defined(MGPU_ALT_HOOKS)
// ---- AMD's API, the part read here (FSR SDK 2.3.0: ffx_api.h, ffx_api_types.h, ffx_upscale.h). Offsets printed
//      by compiling AMD's header, x64. ----
const uint64_t TYPE_CREATE_UPSCALE = 0x00010000ull;     // ffxCreateContextDescUpscale: flags at 16
const uint64_t TYPE_DISPATCH_UPSCALE = 0x00010001ull;   // ffxDispatchDescUpscale, 432 bytes
const size_t D_SIZE = 432;
const size_t D_CMD = 16, D_COLOR = 24, D_DEPTH = 72, D_MVEC = 120, D_OUTPUT = 312;
const size_t D_JITTER = 360, D_MVSCALE = 368, D_RENDER = 376, D_UPSCALE = 384, D_RESET = 408;
const size_t R_PTR = 0, R_FORMAT = 12, R_WIDTH = 16, R_HEIGHT = 20, R_STATE = 40;   // FfxApiResource, 48 bytes
const uint32_t UP_DISPLAY_RES_MVEC = 1u << 1, UP_MVEC_JITTERED = 1u << 2, UP_DEPTH_INVERTED = 1u << 3, UP_HDR = 1u << 0, UP_AUTO_EXPOSURE = 1u << 5;

typedef uint32_t (*pf_create)(void **context, void *desc, const void *memCb);
typedef uint32_t (*pf_dispatch)(void **context, const void *desc);

safetyhook::InlineHook g_h_create{};
safetyhook::InlineHook g_h_dispatch{};
pf_create g_t_create = nullptr;
pf_dispatch g_t_dispatch = nullptr;
uintptr_t g_self_lo = 0, g_self_hi = 0;
std::atomic<bool> g_busy{false};
std::atomic<int> g_tries{0};

std::atomic<unsigned> g_create_flags{0xFFFFFFFFu};     // the newest upscale context's flags; all ones = no create seen
std::atomic<unsigned long long> g_n_dispatch{0}, g_n_upscale{0}, g_n_published{0}, g_n_yield{0}, g_n_fault{0}, g_n_state{0}, g_n_own{0};
unsigned long long g_expect_captures = 0ull;           // the calibrator's capture count after this route's last publish (game's render thread)
std::atomic<bool> g_said_first{false}, g_said_state{false};

inline bool own_call(void *ret)
{
    const uintptr_t ra = reinterpret_cast<uintptr_t>(ret);
    return ra >= g_self_lo && ra < g_self_hi;
}

// The only dereference of the game's descriptor: alone, in a function with no C++ object.
bool copy_bytes(const void *src, unsigned char *dst, size_t n)
{
    __try { memcpy(dst, src, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ffxGetDX12StateFromResourceState (AMD's backend, dx12/ffx_dx12.cpp): the state the game DECLARED, as D3D12 states.
bool d3d_state(uint32_t ffx_state, unsigned &out)
{
    switch (ffx_state)
    {
    case (1u << 4) | (1u << 2): out = (unsigned)D3D12_RESOURCE_STATE_GENERIC_READ; return true;
    case 1u << 1: out = (unsigned)D3D12_RESOURCE_STATE_UNORDERED_ACCESS; return true;
    case (1u << 3) | (1u << 2): out = (unsigned)(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE); return true;
    case 1u << 2: out = (unsigned)D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE; return true;
    case 1u << 3: out = (unsigned)D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE; return true;
    case 1u << 4: out = (unsigned)D3D12_RESOURCE_STATE_COPY_SOURCE; return true;
    case 1u << 5: out = (unsigned)D3D12_RESOURCE_STATE_COPY_DEST; return true;
    case 1u << 0: out = (unsigned)D3D12_RESOURCE_STATE_COMMON; return true;
    case 1u << 8: out = (unsigned)D3D12_RESOURCE_STATE_RENDER_TARGET; return true;
    case 1u << 9: out = (unsigned)D3D12_RESOURCE_STATE_DEPTH_WRITE; return true;
    default: return false;
    }
}

template <class T> T rd(const unsigned char *b, size_t off) { T v; memcpy(&v, b + off, sizeof v); return v; }

void on_upscale(const unsigned char *b)
{
    g_n_upscale.fetch_add(1ull, std::memory_order_relaxed);
    // ONE WRITER: the DLSS route captured since this route's last frame -> the table is DLSS's this frame.
    const unsigned long long cap = mgpu::calibrator::scene_captures();
    if (cap != g_expect_captures)
    {
        g_expect_captures = cap;
        g_n_yield.fetch_add(1ull, std::memory_order_relaxed);
        return;
    }
    void *cmd = rd<void *>(b, D_CMD);
    const unsigned long long color = (unsigned long long)(uintptr_t)rd<void *>(b, D_COLOR + R_PTR);
    const unsigned long long depth = (unsigned long long)(uintptr_t)rd<void *>(b, D_DEPTH + R_PTR);
    const unsigned long long mvec = (unsigned long long)(uintptr_t)rd<void *>(b, D_MVEC + R_PTR);
    const unsigned long long output = (unsigned long long)(uintptr_t)rd<void *>(b, D_OUTPUT + R_PTR);
    const uint32_t rw = rd<uint32_t>(b, D_RENDER), rh = rd<uint32_t>(b, D_RENDER + 4);
    uint32_t uw = rd<uint32_t>(b, D_UPSCALE), uh = rd<uint32_t>(b, D_UPSCALE + 4);
    if (uw == 0u || uh == 0u) { uw = rd<uint32_t>(b, D_OUTPUT + R_WIDTH); uh = rd<uint32_t>(b, D_OUTPUT + R_HEIGHT); }
    const unsigned cf = g_create_flags.load(std::memory_order_relaxed);
    const bool have_cf = (cf != 0xFFFFFFFFu);

    mgpu::calibrator::table t{};
    t.color = color; t.depth = depth; t.mvec = mvec; t.output = output;
    t.mv_scale_x = rd<float>(b, D_MVSCALE); t.mv_scale_y = rd<float>(b, D_MVSCALE + 4);   // AMD: pixelSpaceMV = motionVector * motionVectorScale
    t.jitter_x = rd<float>(b, D_JITTER); t.jitter_y = rd<float>(b, D_JITTER + 4);
    t.reset = rd<unsigned char>(b, D_RESET) != 0u ? 1u : 0u;
    t.render_w = rw; t.render_h = rh; t.display_w = uw; t.display_h = uh;
    t.sub_w = rw; t.sub_h = rh;
    t.have = mgpu::calibrator::KEY_MV_SCALE | mgpu::calibrator::KEY_JITTER | mgpu::calibrator::KEY_RESET |
             mgpu::calibrator::KEY_RENDER_EXT | mgpu::calibrator::KEY_DISPLAY_EXT |
             (color != 0ull ? (unsigned)mgpu::calibrator::KEY_COLOR : 0u) | (depth != 0ull ? (unsigned)mgpu::calibrator::KEY_DEPTH : 0u) |
             (mvec != 0ull ? (unsigned)mgpu::calibrator::KEY_MVEC : 0u) | (output != 0ull ? (unsigned)mgpu::calibrator::KEY_OUTPUT : 0u);
    if (have_cf)
    {
        t.depth_inverted = (cf & UP_DEPTH_INVERTED) ? 1u : 0u;
        t.mv_low_res = (cf & UP_DISPLAY_RES_MVEC) ? 0u : 1u;
        t.mv_jittered = (cf & UP_MVEC_JITTERED) ? 1u : 0u;
        t.is_hdr = (cf & UP_HDR) ? 1u : 0u;
        t.auto_exposure = (cf & UP_AUTO_EXPOSURE) ? 1u : 0u;
        t.have |= mgpu::calibrator::KEY_FLAGS;
    }
    if (!mgpu::calibrator::publish_external(t)) return;   // Calib=0: no table in this session
    g_expect_captures = mgpu::calibrator::scene_captures();
    g_n_published.fetch_add(1ull, std::memory_order_relaxed);

    // The copies, on the game's own list, before FSR reads the same resources - with the states the game declared.
    unsigned ms = 0u, ds = 0u;
    const bool ms_ok = d3d_state(rd<uint32_t>(b, D_MVEC + R_STATE), ms);
    const bool ds_ok = d3d_state(rd<uint32_t>(b, D_DEPTH + R_STATE), ds);
    if (cmd != nullptr && mvec != 0ull)
    {
        if (ms_ok) mgpu::gpu1::stream_mvec_copy_declared(cmd, mvec, ms);
        else g_n_state.fetch_add(1ull, std::memory_order_relaxed);
    }
    if (cmd != nullptr && depth != 0ull && mgpu::depthsrc::wants_fsr())
    {
        if (ds_ok) mgpu::depthsrc::on_declared(cmd, depth, ds, rw, rh);
        else g_n_state.fetch_add(1ull, std::memory_order_relaxed);
    }
    if ((!ms_ok || !ds_ok) && !g_said_state.exchange(true))
    {
        char l[300];
        snprintf(l, sizeof l, "[MGPU][R381] the game declared a resource state this build has no D3D12 state for (vectors 0x%X, depth 0x%X): "
                 "that resource is not copied by this route. Said once.", rd<uint32_t>(b, D_MVEC + R_STATE), rd<uint32_t>(b, D_DEPTH + R_STATE));
        mgpu::diag::warn(l);
    }
    if (!g_said_first.exchange(true))
    {
        char l[620];
        snprintf(l, sizeof l,
                 "[MGPU][R381] FIRST FSR CONTRACT: color=0x%llx depth=0x%llx (format %u, state 0x%X) mvec=0x%llx (format %u, state 0x%X) "
                 "output=0x%llx | render %ux%u display %ux%u | vector scale %.3f,%.3f (to pixels) | jitter %.3f,%.3f | reset %u | "
                 "context flags %s0x%X. Published to the calibrator's table; the vectors are copied with the declared state.",
                 color, depth, rd<uint32_t>(b, D_DEPTH + R_FORMAT), rd<uint32_t>(b, D_DEPTH + R_STATE),
                 mvec, rd<uint32_t>(b, D_MVEC + R_FORMAT), rd<uint32_t>(b, D_MVEC + R_STATE), output, rw, rh, uw, uh,
                 (double)t.mv_scale_x, (double)t.mv_scale_y, (double)t.jitter_x, (double)t.jitter_y, t.reset,
                 have_cf ? "" : "NOT SEEN (the context was made before the hook) ", have_cf ? cf : 0u);
        mgpu::diag::info(l);
    }
}

uint32_t det_create(void **context, void *desc, const void *memCb)
{
    if (!own_call(_ReturnAddress()) && desc != nullptr)
    {
        unsigned char h[24];
        if (copy_bytes(desc, h, sizeof h) && rd<uint64_t>(h, 0) == TYPE_CREATE_UPSCALE)
            g_create_flags.store(rd<uint32_t>(h, 16), std::memory_order_relaxed);
    }
    return g_t_create(context, desc, memCb);
}

uint32_t det_dispatch(void **context, const void *desc)
{
    if (own_call(_ReturnAddress()))
    {
        g_n_own.fetch_add(1ull, std::memory_order_relaxed);
        return g_t_dispatch(context, desc);
    }
    g_n_dispatch.fetch_add(1ull, std::memory_order_relaxed);
    if (desc != nullptr)
    {
        unsigned char b[D_SIZE];
        if (!copy_bytes(desc, b, 16)) g_n_fault.fetch_add(1ull, std::memory_order_relaxed);
        else if (rd<uint64_t>(b, 0) == TYPE_DISPATCH_UPSCALE)
        {
            if (copy_bytes(desc, b, D_SIZE)) on_upscale(b);
            else g_n_fault.fetch_add(1ull, std::memory_order_relaxed);
        }
    }
    return g_t_dispatch(context, desc);
}

void refuse(const char *why)
{
    g_h_create.reset(); g_h_dispatch.reset();
    g_want.store(false, std::memory_order_release);
    char l[360];
    snprintf(l, sizeof l, "[MGPU][R381] FfxHook=1 REFUSED: %s. The FSR DLL is as it was; the bridge runs as it does without the key.", why);
    mgpu::diag::error(l);
}
#endif   // MGPU_ALT_HOOKS
}   // namespace

bool wanted() { return g_want.load(std::memory_order_relaxed); }
bool pending() { return g_want.load(std::memory_order_relaxed) && !g_live.load(std::memory_order_relaxed); }

void init()
{
    if (!key_is_one()) return;
#if defined(MGPU_ALT_HOOKS)
    static const int anchor = 0;
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&anchor), &self) || self == nullptr)
    {
        mgpu::diag::error("[MGPU][R381] FfxHook=1: the bridge's own image could not be found. Nothing is installed.");
        return;
    }
    const unsigned char *base = reinterpret_cast<const unsigned char *>(self);
    const IMAGE_DOS_HEADER *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    const IMAGE_NT_HEADERS *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    g_self_lo = reinterpret_cast<uintptr_t>(base);
    g_self_hi = g_self_lo + nt->OptionalHeader.SizeOfImage;
    g_want.store(true, std::memory_order_release);
    mgpu::diag::info("[MGPU][R381] FfxHook=1: the FSR contract route (N01950's inline hook on the FidelityFX API). It installs "
                     "when the game has loaded its FSR DLL; until then one check per present. It reads the game's FSR upscale "
                     "call; while the game's DLSS evaluates, it stays out.");
    try_install();
#else
    mgpu::diag::warn("[MGPU][R381] FfxHook=1 is in mgpu.ini but THIS BUILD DOES NOT CARRY the route (SafetyHook was not fetched "
                     "when the build was configured). The key is ignored.");
#endif
}

void try_install()
{
#if defined(MGPU_ALT_HOOKS)
    if (!pending()) return;
    if (g_busy.exchange(true, std::memory_order_acquire)) return;
    struct unbusy { ~unbusy() { g_busy.store(false, std::memory_order_release); } } ub;
    static const wchar_t *names[4] = { L"amd_fidelityfx_dx12.dll", L"amd_fidelityfx_loader_dx12.dll",
                                       L"amd_fidelityfx_upscaler_dx12.dll", L"ffx_api.dll" };
    HMODULE api = nullptr; const wchar_t *which = L"";
    for (int k = 0; k < 4 && api == nullptr; ++k) { api = GetModuleHandleW(names[k]); which = names[k]; }
    if (api == nullptr)
    {
        if (g_tries.fetch_add(1, std::memory_order_relaxed) == 0)
            mgpu::diag::info("[MGPU][R381] no FidelityFX API DLL is loaded yet (amd_fidelityfx_dx12.dll, amd_fidelityfx_loader_dx12.dll, "
                             "amd_fidelityfx_upscaler_dx12.dll, ffx_api.dll). Checked again on every present. Said once.");
        return;
    }
    void *e_dispatch = reinterpret_cast<void *>(GetProcAddress(api, "ffxDispatch"));
    void *e_create = reinterpret_cast<void *>(GetProcAddress(api, "ffxCreateContext"));
    if (e_dispatch == nullptr) { refuse("the DLL exports no ffxDispatch"); return; }
    auto hd = safetyhook::InlineHook::create(e_dispatch, reinterpret_cast<void *>(&det_dispatch), safetyhook::InlineHook::StartDisabled);
    if (!hd) { refuse("SafetyHook could not build a trampoline for ffxDispatch"); return; }
    g_h_dispatch = std::move(*hd);
    g_t_dispatch = g_h_dispatch.original<pf_dispatch>();
    if (e_create != nullptr)
    {
        auto hc = safetyhook::InlineHook::create(e_create, reinterpret_cast<void *>(&det_create), safetyhook::InlineHook::StartDisabled);
        if (hc) { g_h_create = std::move(*hc); g_t_create = g_h_create.original<pf_create>(); }
    }
    g_expect_captures = mgpu::calibrator::scene_captures();
    const bool ok_c = g_h_create ? g_h_create.enable().has_value() : false;
    if (!g_h_dispatch.enable().has_value()) { refuse("ffxDispatch's entry could not be patched"); return; }
    g_live.store(true, std::memory_order_release);
    char l[520];
    snprintf(l, sizeof l, "[MGPU][R381] HOOK LIVE on %ls %p: ffxDispatch=YES ffxCreateContext=%s. The game's FSR upscale call now "
             "states its depth, vectors, scale, jitter and render size to the bridge; the bridge's own FidelityFX calls "
             "(image %p-%p) pass straight through. Method: N01950.", which, (void *)api, ok_c ? "YES" : "no",
             (void *)g_self_lo, (void *)g_self_hi);
    mgpu::diag::info(l);
#endif
}

void uninstall()
{
#if defined(MGPU_ALT_HOOKS)
    if (!g_live.exchange(false, std::memory_order_acq_rel)) return;
    g_want.store(false, std::memory_order_release);
    g_h_create.reset(); g_h_dispatch.reset();
    char l[420];
    snprintf(l, sizeof l, "[MGPU][R381] hook removed. Totals: game dispatches %llu, of them FSR upscale %llu | published %llu | "
             "left to DLSS %llu | state not mapped %llu | read faults %llu | the bridge's own calls %llu.",
             g_n_dispatch.load(std::memory_order_relaxed), g_n_upscale.load(std::memory_order_relaxed),
             g_n_published.load(std::memory_order_relaxed), g_n_yield.load(std::memory_order_relaxed),
             g_n_state.load(std::memory_order_relaxed), g_n_fault.load(std::memory_order_relaxed), g_n_own.load(std::memory_order_relaxed));
    mgpu::diag::info(l);
#endif
}
}   // namespace altffx
}   // namespace mgpu
