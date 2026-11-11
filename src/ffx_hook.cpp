// MGPU Bridge - [FFX, 2026-11-11] the FSR contract, read at ffxDispatch.
//
// PROVENANCE. Same mechanics as ngx_hook.cpp: SafetyHook inline detour, SEH around every body
// that touches a caller's memory, a one-line install banner, a census, and a real call that
// always runs whatever the instrument decides. The struct offsets are the FidelityFX SDK
// v1.1.0 ffx_api headers (ffx_api.h / ffx_api_types.h / ffx_upscale.h), which is what the
// shipped `amd_fidelityfx_loader_dx12.dll` in PBZ, Cyberpunk 2077 and KCD2 exports.
//
// THE OFFSETS ARE A CLAIM, NOT A FACT. A loader from a different SDK revision can move them.
// The first upscale dispatch therefore checks every field the offsets produced against the
// resource's own ID3D12Resource::GetDesc() - declared width/height/format against actual. A
// mismatch sets layout_bad and NO copy is ever taken for the session: a wrong offset must cost
// telemetry, never a copy from the wrong texture into the transport.
//
// WHAT THE CALLER SEES. mgpu::calibrator::publish_external() fills the same table the NGX hook
// fills, so [R78]/[R63]/the NR stage keep reading one contract regardless of API. The two
// copies go through the same stream_depth_copy_state / stream_mvec_copy_state the NGX route
// uses, with the state FFX declared, so the barrier pair and its restore are honest.
#include <windows.h>
#include <d3d12.h>

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "ffx_hook.hpp"
#include "calibrator.hpp"
#include "diag.hpp"
#include "gpu1_context.hpp"
#include "safetyhook.hpp"

namespace mgpu
{
namespace ffxhook
{
namespace
{
// ffxReturnCode_t is uint32_t. The v1.1.0 signatures are (context, desc) for dispatch and
// (context, desc, memCb) for create. Dispatch is declared with a third (unused) parameter
// because some 3.0 revisions carried a messaging pointer there; on x64 an extra argument is
// ignored by a two-argument callee, so ONE detour covers both ABIs.
using pf_dispatch = uint32_t (__stdcall *)(void **context, const void *desc, void *messaging);
using pf_create   = uint32_t (__stdcall *)(void **context, void *desc, const void *mem_cb);

safetyhook::InlineHook g_h_dispatch{};
safetyhook::InlineHook g_h_create{};
pf_dispatch g_real_dispatch = nullptr;
pf_create   g_real_create = nullptr;

std::atomic<bool> g_want{false};
std::atomic<bool> g_done{false};
std::atomic<int>  g_tries{0};

std::atomic<unsigned> g_create_flags{0};
std::atomic<bool> g_have_create_flags{false};
std::atomic<unsigned> g_max_upscale_w{0};
std::atomic<unsigned> g_max_upscale_h{0};
std::atomic<unsigned long long> g_n_create{0};
std::atomic<unsigned long long> g_n_dispatch{0};
std::atomic<unsigned long long> g_n_upscale{0};
std::atomic<unsigned long long> g_n_depth_try{0};
std::atomic<unsigned long long> g_n_mvec_try{0};
std::atomic<unsigned long long> g_n_skipped{0};

// The two enum values that matter, straight out of ffx_upscale.h.
constexpr uint64_t kCreateUpscale   = 0x00010000ull;
constexpr uint64_t kDispatchUpscale = 0x00010001ull;

// ---- ffxDispatchDescUpscale, SDK v1.1.0, in bytes ----
//
// header           0   (uint64 type, void* pNext)
// commandList     16
// color           24   FfxApiResource
// depth           72   FfxApiResource
// motionVectors  120   FfxApiResource
// exposure       168   (unused here)
// reactive       216   (unused here)
// transparency   264   (unused here)
// output         312   FfxApiResource
// jitterOffset   360   float x,y
// mvScale        368   float x,y
// renderSize     376   uint32 w,h
// upscaleSize    384   uint32 w,h
// reset          408   bool
// flags          428   uint32
//
// FfxApiResource, in bytes: resource 0, description {type 8, format 12, width 16, height 20,
// depth 24, mipCount 28, flags 32, usage 36}, state 40, sizeof 48 (4 bytes tail padding).
constexpr size_t kOffCommandList = 16;
constexpr size_t kOffColor       = 24;
constexpr size_t kOffDepth       = 72;
constexpr size_t kOffMvec        = 120;
constexpr size_t kOffOutput      = 312;
constexpr size_t kOffJitter      = 360;
constexpr size_t kOffMvScale     = 368;
constexpr size_t kOffRenderSize  = 376;
constexpr size_t kOffUpscaleSize = 384;
constexpr size_t kOffReset       = 408;
constexpr size_t kOffFlags       = 428;

struct res_view
{
    void *resource;
    unsigned type, format, width, height, depth_bits, mips, flags, usage, state;
};

void rd_mem(const unsigned char *base, size_t off, void *out, size_t n)
{
    std::memcpy(out, base + off, n);
}

unsigned long long rd_u64(const unsigned char *b, size_t o)
{
    unsigned long long v = 0; rd_mem(b, o, &v, sizeof v); return v;
}

unsigned rd_u32(const unsigned char *b, size_t o)
{
    unsigned v = 0; rd_mem(b, o, &v, sizeof v); return v;
}

float rd_f32(const unsigned char *b, size_t o)
{
    float v = 0.0f; rd_mem(b, o, &v, sizeof v); return v;
}

void rd_resource(const unsigned char *base, size_t off, res_view &r)
{
    r.resource   = nullptr;
    rd_mem(base, off + 0,  &r.resource, sizeof(void *));
    r.type       = rd_u32(base, off + 8);
    r.format     = rd_u32(base, off + 12);
    r.width      = rd_u32(base, off + 16);
    r.height     = rd_u32(base, off + 20);
    r.depth_bits = rd_u32(base, off + 24);
    r.mips       = rd_u32(base, off + 28);
    r.flags      = rd_u32(base, off + 32);
    r.usage      = rd_u32(base, off + 36);
    r.state      = rd_u32(base, off + 40);
}

// FfxApiResourceState -> D3D12_RESOURCE_STATES. The bit values are NOT the same enum; this is
// the same translation the FSR DX12 backend performs internally.
unsigned ffx_state_to_d3d12(unsigned s)
{
    unsigned d = 0;
    if (s & (1u << 1)) d |= D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if (s & (1u << 2)) d |= D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (s & (1u << 3)) d |= D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (s & (1u << 4)) d |= D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (s & (1u << 5)) d |= D3D12_RESOURCE_STATE_COPY_DEST;
    if (s & (1u << 6)) d |= D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    if (s & (1u << 8)) d |= D3D12_RESOURCE_STATE_RENDER_TARGET;
    // SDK 2.3 added FFX_API_RESOURCE_STATE_DEPTH_ATTACHMENT (bit 9). FSR reads depth as an
    // input; DEPTH_READ is the D3D12 state that means "depth, readable, not being written".
    if (s & (1u << 9)) d |= D3D12_RESOURCE_STATE_DEPTH_READ;
    // COMMON (1<<0) and PRESENT (1<<7) are both D3D12_RESOURCE_STATE_COMMON == 0.
    return d;
}

// Verified once, on the first upscale dispatch. Until it is true nothing is copied and nothing
// is published; after a failure it stays false for the session.
std::atomic<int> g_layout{0};   // 0 = not checked, 1 = good, -1 = refused
std::atomic<bool> g_said_layout_bad{false};

// THE FIRST VERSION OF THIS REJECTED A CORRECT DESCRIPTOR. Two mistakes, both from reading the
// 1.1.0 header as if its fields were DXGI's:
//   1. FfxApiResourceDescription.format is an **FfxApiSurfaceFormat**, not a DXGI_FORMAT. The
//      run's depth read as 28 (=R32_FLOAT in FFX's enum) and mvec as 18 (=R16G16_FLOAT), which
//      compared against DXGI's 28 (R8G8B8A8_UNORM) and 18 (R8G8B8A8_UNORM) and "disagreed".
//   2. FfxApiResourceState gained DEPTH_ATTACHMENT (bit 9) in SDK 2.3 - 0x200 is a legal state,
//      and the old `> 0x1FF` bound refused it.
// What actually proves the layout is pointer identity: the declared width/height must equal the
// resource's own GetDesc(). format is LOGGED in FFX's own enum and never compared.
bool check_one(const char *what, void *res, const res_view &rv)
{
    if (res == nullptr) return true;   // an optional resource being absent is not a layout fault
    // The description's own fields, before any dereference: FFX_API_RESOURCE_TYPE_TEXTURE2D is
    // 2, and FfxApiResourceState is a 10-bit mask in SDK 2.3.
    if (rv.type != 2u || rv.state > 0x3FFu) return false;
    __try
    {
        ID3D12Resource *r = reinterpret_cast<ID3D12Resource *>(res);
        const D3D12_RESOURCE_DESC d = r->GetDesc();
        bool ok = (d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D);
        if (ok && rv.width != 0u && (unsigned)d.Width != rv.width) ok = false;
        if (ok && rv.height != 0u && (unsigned)d.Height != rv.height) ok = false;
        if (!ok)
        {
            char l[360];
            snprintf(l, sizeof l,
                     "[MGPU][R823] layout check detail for %s: desc type=%u ffxfmt=%u %ux%u state=0x%X | "
                     "GetDesc dim=%u %ux%u dxgifmt=%u. (format is FfxApiSurfaceFormat, not DXGI - it is "
                     "logged, never compared.)",
                     what, rv.type, rv.format, rv.width, rv.height, rv.state,
                     (unsigned)d.Dimension, (unsigned)d.Width, (unsigned)d.Height, (unsigned)d.Format);
            mgpu::diag::warn(l);
        }
        return ok;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// One line per reason, ever. Runs on the game's render thread, so it must be cheap after the
// first frame: three atomics and a memcpy.
void inspect(const void *desc_v)
{
    if (desc_v == nullptr) return;
    const unsigned char *d = reinterpret_cast<const unsigned char *>(desc_v);

    const unsigned long long type = rd_u64(d, 0);
    g_n_dispatch.fetch_add(1ull, std::memory_order_relaxed);
    if (type != kDispatchUpscale) return;

    const unsigned long long n = g_n_upscale.fetch_add(1ull, std::memory_order_relaxed) + 1ull;

    res_view color{}, depth{}, mvec{}, output{};
    rd_resource(d, kOffColor,  color);
    rd_resource(d, kOffDepth,  depth);
    rd_resource(d, kOffMvec,   mvec);
    rd_resource(d, kOffOutput, output);

    const unsigned rw = rd_u32(d, kOffRenderSize + 0);
    const unsigned rh = rd_u32(d, kOffRenderSize + 4);
    unsigned uw = rd_u32(d, kOffUpscaleSize + 0);
    unsigned uh = rd_u32(d, kOffUpscaleSize + 4);
    // upscaleSize is OPTIONAL in the descriptor ("assumed maxUpscaleSize otherwise") and PBZ
    // sends 0. The create descriptor carried maxUpscaleSize; without it this published
    // display=render and every consumer of KEY_DISPLAY_EXT believed the title was not upscaling.
    if (uw == 0u || uh == 0u)
    {
        uw = g_max_upscale_w.load(std::memory_order_relaxed);
        uh = g_max_upscale_h.load(std::memory_order_relaxed);
    }
    if (uw == 0u || uh == 0u) { uw = rw; uh = rh; }
    const float jx = rd_f32(d, kOffJitter + 0);
    const float jy = rd_f32(d, kOffJitter + 4);
    const float sx = rd_f32(d, kOffMvScale + 0);
    const float sy = rd_f32(d, kOffMvScale + 4);
    const unsigned reset = rd_u32(d, kOffReset) & 1u;
    const unsigned dflags = rd_u32(d, kOffFlags);
    (void)dflags;

    // Sanity before anything is dereferenced: an FSR render target is a 2D texture with a
    // believable extent. A garbage read fails here, not in GetDesc().
    if (rw < 16u || rh < 16u || rw > 16384u || rh > 16384u) { ++g_n_skipped; return; }
    if (depth.resource == nullptr && mvec.resource == nullptr) { ++g_n_skipped; return; }

    void *cl = nullptr;
    rd_mem(d, kOffCommandList, &cl, sizeof cl);

    // ---- THE SELF-PROOF, ONCE ----
    int want = g_layout.load(std::memory_order_acquire);
    if (want == 0)
    {
        int next = 1;
        __try
        {
            if (cl == nullptr) next = 0;
            if (next == 1 && !check_one("color", color.resource, color))  next = -1;
            if (next == 1 && !check_one("depth", depth.resource, depth))  next = -1;
            if (next == 1 && !check_one("mvec",  mvec.resource,  mvec))   next = -1;
            // The scalar tail is part of the same layout claim: a wrong offset here shows up as a
            // non-finite or absurd jitter/scale, not as a resource mismatch.
            if (next == 1 && (!std::isfinite(jx) || !std::isfinite(jy) ||
                              !std::isfinite(sx) || !std::isfinite(sy))) next = -1;
            if (next == 1 && (std::fabs(jx) > 4.0f || std::fabs(jy) > 4.0f)) next = -1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            next = -1;
        }
        if (next == 0) return;   // not enough truth yet; check again next dispatch
        g_layout.store(next, std::memory_order_release);
        if (next > 0)
        {
            char l[640];
            snprintf(l, sizeof l,
                     "[MGPU][R821] FFX layout check PASSED (struct offsets agree with the resources): "
                     "render=%ux%u upscale=%ux%u | depth %ux%u fmt=%u state=0x%X | mvec %ux%u fmt=%u "
                     "state=0x%X | color %ux%u fmt=%u. The ffxDispatchDescUpscale offsets in ffx_hook.cpp "
                     "are the SDK v1.1.0 layout and this run's loader agrees with them.",
                     rw, rh, uw, uh,
                     depth.width, depth.height, depth.format, depth.state,
                     mvec.width, mvec.height, mvec.format, mvec.state,
                     color.width, color.height, color.format);
            mgpu::diag::info(l);
        }
        else
        {
            if (!g_said_layout_bad.exchange(true))
            {
                char l[760];
                snprintf(l, sizeof l,
                         "[MGPU][R823] FFX LAYOUT REFUSED: the offsets are not this loader's layout. "
                         "desc says render=%ux%u depth=%ux%u fmt=%u mvec=%ux%u fmt=%u; the resources' own "
                         "GetDesc() disagreed (or one of them is not a D3D12 texture). NO depth/vector copy "
                         "is taken this session and the contract stays whatever the NGX/tap route provides. "
                         "This is the fail-closed answer: a wrong offset must cost telemetry, never a copy "
                         "from the wrong resource.",
                         rw, rh, depth.width, depth.height, depth.format,
                         mvec.width, mvec.height, mvec.format);
                mgpu::diag::error(l);
            }
        }
    }
    if (g_layout.load(std::memory_order_acquire) < 0) { ++g_n_skipped; return; }

    // ---- PUBLISH THE SAME TABLE THE NGX HOOK PUBLISHES ----
    mgpu::calibrator::table t{};
    t.color  = (unsigned long long)(uintptr_t)color.resource;
    t.depth  = (unsigned long long)(uintptr_t)depth.resource;
    t.mvec   = (unsigned long long)(uintptr_t)mvec.resource;
    t.output = (unsigned long long)(uintptr_t)output.resource;
    t.render_w = rw; t.render_h = rh;
    t.display_w = uw ? uw : rw; t.display_h = uh ? uh : rh;
    t.mv_scale_x = sx; t.mv_scale_y = sy;
    t.jitter_x = jx; t.jitter_y = jy;
    t.reset = reset;

    // The create flags. FFX's bit layout happens to agree with NGX's on the bits this file
    // already reads: 0 HDR, 3 DEPTH_INVERTED. Bit 1 is the INVERSE question (FFX sets it when
    // vectors are DISPLAY resolution; NGX sets MVLowRes when they are render/low resolution),
    // so it is mapped, not copied.
    const unsigned cf = g_create_flags.load(std::memory_order_relaxed);
    const bool have_cf = g_have_create_flags.load(std::memory_order_relaxed);
    t.create_flags  = cf;
    t.is_hdr        = cf & 1u;
    t.mv_low_res    = ((cf & (1u << 1)) != 0u) ? 0u : 1u;
    t.mv_jittered   = (cf >> 2) & 1u;
    t.depth_inverted = (cf >> 3) & 1u;
    t.auto_exposure = (cf >> 5) & 1u;

    t.have = mgpu::calibrator::KEY_COLOR | mgpu::calibrator::KEY_DEPTH |
             mgpu::calibrator::KEY_MVEC | mgpu::calibrator::KEY_OUTPUT |
             mgpu::calibrator::KEY_MV_SCALE | mgpu::calibrator::KEY_JITTER |
             mgpu::calibrator::KEY_RENDER_EXT | mgpu::calibrator::KEY_DISPLAY_EXT;
    if (have_cf) t.have |= mgpu::calibrator::KEY_FLAGS;
    if (reset != 0u) t.have |= mgpu::calibrator::KEY_RESET;
    // [FFX] hand the unit answer to the arm: the descriptor's motionVectorScale is the factor
    // FSR itself applies, i.e. the UV->pixels conversion when the game hands FSR UV vectors.
    mgpu::gpu1::note_ffx_mv_scale(sx, sy);
    mgpu::calibrator::publish_external(t);

    // ---- AND THE TWO COPIES, ON THE GAME'S OWN COMMAND LIST, BEFORE ITS DISPATCH ----
    if (cl == nullptr) { ++g_n_skipped; return; }

    if (depth.resource != nullptr)
    {
        g_n_depth_try.fetch_add(1ull, std::memory_order_relaxed);
        __try
        {
            mgpu::gpu1::stream_depth_copy_state(
                cl, (unsigned long long)(uintptr_t)depth.resource, rw, rh,
                ffx_state_to_d3d12(depth.state));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            ++g_n_skipped;
        }
    }
    if (mvec.resource != nullptr)
    {
        g_n_mvec_try.fetch_add(1ull, std::memory_order_relaxed);
        __try
        {
            mgpu::gpu1::stream_mvec_copy_state(
                cl, (unsigned long long)(uintptr_t)mvec.resource,
                ffx_state_to_d3d12(mvec.state));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            ++g_n_skipped;
        }
    }

    if (n == 1ull)
    {
        char l[560];
        snprintf(l, sizeof l,
                 "[MGPU][R822] FFX upscale dispatch #1: cmd=0x%p depth=0x%llx (state=0x%X -> 0x%X) "
                 "mvec=0x%llx (state=0x%X -> 0x%X) render=%ux%u upscale=%ux%u reset=%u "
                 "jitter=%.3f,%.3f mvscale=%.3f,%.3f create-flags=0x%X. Depth and vectors are copied "
                 "on the game's own list before its dispatch, exactly as the NGX route does at "
                 "EvaluateFeature.",
                 cl, t.depth, depth.state, ffx_state_to_d3d12(depth.state),
                 t.mvec, mvec.state, ffx_state_to_d3d12(mvec.state),
                 rw, rh, uw, uh, reset, jx, jy, sx, sy, cf);
        mgpu::diag::info(l);
    }
    else if ((n % 300ull) == 0ull)
    {
        char l[420];
        snprintf(l, sizeof l,
                 "[MGPU][R824] FFX census: upscale-dispatches=%llu | depth-copies-tried=%llu "
                 "mvec-copies-tried=%llu skipped=%llu | render=%ux%u | layout=%s | create-flags=0x%X. "
                 "Whether the transport ACCEPTED a copy is [R78] copies= / [R63]; this line only "
                 "counts what reached it.",
                 (unsigned long long)g_n_upscale.load(std::memory_order_relaxed),
                 (unsigned long long)g_n_depth_try.load(std::memory_order_relaxed),
                 (unsigned long long)g_n_mvec_try.load(std::memory_order_relaxed),
                 (unsigned long long)g_n_skipped.load(std::memory_order_relaxed),
                 rw, rh, (g_layout.load(std::memory_order_acquire) > 0) ? "OK" : "REFUSED", cf);
        mgpu::diag::info(l);
    }
}

uint32_t __stdcall det_dispatch(void **context, const void *desc, void *messaging)
{
    __try
    {
        inspect(desc);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        static std::atomic<bool> said{false};
        if (!said.exchange(true))
            mgpu::diag::warn("[MGPU][R823] the ffxDispatch detour raised while reading the descriptor - "
                             "the real dispatch still runs, nothing was copied");
    }
    if (g_h_dispatch) return g_h_dispatch.call<uint32_t>(context, desc, messaging);
    return g_real_dispatch ? g_real_dispatch(context, desc, messaging) : 6u;   // ERROR_PARAMETER
}

uint32_t __stdcall det_create(void **context, void *desc, const void *mem_cb)
{
    __try
    {
        g_n_create.fetch_add(1ull, std::memory_order_relaxed);
        if (desc != nullptr)
        {
            const unsigned char *d = reinterpret_cast<const unsigned char *>(desc);
            if (rd_u64(d, 0) == kCreateUpscale)
            {
                const unsigned flags = rd_u32(d, 16);
                const unsigned mrw = rd_u32(d, 20), mrh = rd_u32(d, 24);
                const unsigned muw = rd_u32(d, 28), muh = rd_u32(d, 32);
                if (muw != 0u && muh != 0u)
                {
                    g_max_upscale_w.store(muw, std::memory_order_relaxed);
                    g_max_upscale_h.store(muh, std::memory_order_relaxed);
                }
                g_have_create_flags.store(true, std::memory_order_relaxed);
                const unsigned prev = g_create_flags.exchange(flags, std::memory_order_relaxed);
                if (prev != flags)
                {
                    char l[460];
                    snprintf(l, sizeof l,
                             "[MGPU][R820] FFX upscale context created: flags=0x%X "
                             "(HDR=%u display-res-mv=%u mv-jitter-cancel=%u depth-inverted=%u "
                             "depth-infinite=%u auto-exposure=%u dynamic-res=%u) | maxRender=%ux%u "
                             "maxUpscale=%ux%u. The flags become the contract's create_flags, the same "
                             "word the NGX route reads at CreateFeature; maxUpscale is what display_* "
                             "uses because the per-frame upscaleSize is optional and this title sends 0.",
                             flags, flags & 1u, (flags >> 1) & 1u, (flags >> 2) & 1u, (flags >> 3) & 1u,
                             (flags >> 4) & 1u, (flags >> 5) & 1u, (flags >> 6) & 1u,
                             mrw, mrh, muw, muh);
                    mgpu::diag::info(l);
                }
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        static std::atomic<bool> said{false};
        if (!said.exchange(true))
            mgpu::diag::warn("[MGPU][R820] the ffxCreateContext detour raised - the real create still runs");
    }
    if (g_h_create) return g_h_create.call<uint32_t>(context, desc, mem_cb);
    return g_real_create ? g_real_create(context, desc, mem_cb) : 6u;
}

void log_stats()
{
    char l[420];
    snprintf(l, sizeof l,
             "[MGPU][R820] FFX hook stats: creates=%llu dispatches=%llu upscale=%llu "
             "depth-tried=%llu mvec-tried=%llu skipped=%llu layout=%s",
             (unsigned long long)g_n_create.load(std::memory_order_relaxed),
             (unsigned long long)g_n_dispatch.load(std::memory_order_relaxed),
             (unsigned long long)g_n_upscale.load(std::memory_order_relaxed),
             (unsigned long long)g_n_depth_try.load(std::memory_order_relaxed),
             (unsigned long long)g_n_mvec_try.load(std::memory_order_relaxed),
             (unsigned long long)g_n_skipped.load(std::memory_order_relaxed),
             (g_layout.load(std::memory_order_acquire) > 0) ? "OK"
                 : (g_layout.load(std::memory_order_acquire) < 0) ? "REFUSED" : "not checked yet");
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

    // install() is called from BOTH the game's finish-present callback and the swapchain-init
    // callback, on the game thread, and can race with itself on a chain rebuild. Two
    // create_inline calls over one prologue is how a trampoline becomes garbage; the NGX hook
    // relies on timing for this and this one does not have to.
    static std::mutex install_cs;
    std::lock_guard<std::mutex> lk(install_cs);
    if (g_done.load(std::memory_order_acquire)) return true;

    // The FidelityFX 3.1 API module. `ffx_api.dll` is the same API under the name some titles
    // ship; whichever is resident is the one the game calls.
    HMODULE api = GetModuleHandleW(L"amd_fidelityfx_loader_dx12.dll");
    if (api == nullptr) api = GetModuleHandleW(L"ffx_api.dll");
    if (api == nullptr)
    {
        const int n = g_tries.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 3)
            mgpu::diag::info("[MGPU][R820] the FidelityFX API module is not resident yet "
                             "(amd_fidelityfx_loader_dx12.dll / ffx_api.dll) - the hook retries on "
                             "the next present. A DLSS title never loads it and this stays silent "
                             "for the session, which is the intended answer.");
        return false;
    }

    g_real_dispatch = (pf_dispatch)GetProcAddress(api, "ffxDispatch");
    g_real_create   = (pf_create)GetProcAddress(api, "ffxCreateContext");
    if (g_real_dispatch == nullptr)
    {
        mgpu::diag::error("[MGPU][R820] REFUSED: the FidelityFX module has no ffxDispatch export - "
                          "nothing is patched; the NGX/tap routes are unchanged.");
        g_want.store(false, std::memory_order_relaxed);
        return false;
    }

    try
    {
        if (!static_cast<bool>(g_h_dispatch))
            g_h_dispatch = safetyhook::create_inline((void *)g_real_dispatch, (void *)&det_dispatch);
        if (g_real_create != nullptr && !static_cast<bool>(g_h_create))
            g_h_create = safetyhook::create_inline((void *)g_real_create, (void *)&det_create);
    }
    catch (...)
    {
        mgpu::diag::error("[MGPU][R820] SafetyHook threw while patching ffxDispatch - the module is "
                          "untouched and this hook is abandoned for the session.");
        g_want.store(false, std::memory_order_relaxed);
        return false;
    }

    if (!static_cast<bool>(g_h_dispatch))
    {
        mgpu::diag::error("[MGPU][R820] REFUSED: SafetyHook could not patch ffxDispatch - the module "
                          "is untouched; the NGX/tap routes are unchanged. NOT retried: an image "
                          "cannot become patchable later in the same process.");
        g_want.store(false, std::memory_order_relaxed);
        return false;
    }

    g_done.store(true, std::memory_order_release);
    char l[560];
    snprintf(l, sizeof l,
             "[MGPU][R820] FFX HOOK LIVE on %p: dispatch=YES create=%s. The FSR upscale descriptor "
             "(colour/depth/motion vectors, extents, jitter, mv scale, reset, create flags) now "
             "reaches the same contract table the NGX hook fills, and the two images are copied on "
             "the game's command list with the state FFX declared.",
             (void *)api, static_cast<bool>(g_h_create) ? "YES" : "no");
    mgpu::diag::info(l);
    log_stats();
    return true;
}
}   // namespace ffxhook
}   // namespace mgpu
