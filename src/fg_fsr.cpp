// MGPU Bridge - R380 (0.5.1 discovery): AMD FSR frame generation on the neural card. See fg_fsr.hpp.
#include <windows.h>
#include <d3d12.h>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <vector>

#include "fg_fsr.hpp"
#include "sl_camera.hpp"
#include "diag.hpp"
#include "mgpu_ini_parser.hpp"

namespace mgpu
{
namespace fgfsr
{
namespace
{
// ---- The FidelityFX API, mirrored from AMD's public headers (FSR SDK 2.3.0: api/include/ffx_api.h,
//      ffx_api_types.h, dx12/ffx_api_dx12.h, framegeneration/include/ffx_framegeneration.h). Only what this file
//      uses. Every struct starts with a type id, and the id fixes the layout (a changed layout gets a new id), so
//      these are the layouts of FSR 3.1 and of every later provider. Sizes and offsets are asserted below. ----
namespace ffx
{
struct Header { uint64_t type; Header *pNext; };
struct Dim2D { uint32_t width, height; };
struct Float2 { float x, y; };
struct Rect2D { int32_t left, top, width, height; };
struct ResourceDescription { uint32_t type, format, width, height, depth, mipCount, flags, usage; };
struct Resource { void *resource; ResourceDescription description; uint32_t state; };

const uint64_t TYPE_BACKEND_DX12 = 0x0000002ull;              // FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12
const uint64_t TYPE_GLOBALDEBUG1 = 0x0000001ull;              // FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG1
const uint64_t TYPE_GET_PROVIDER_VERSION = 0x0000006ull;      // FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION
const uint64_t TYPE_FG_CREATE = 0x00020001ull;                // ..._CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION
const uint64_t TYPE_FG_CONFIGURE = 0x00020002ull;             // ..._CONFIGURE_DESC_TYPE_FRAMEGENERATION
const uint64_t TYPE_FG_DISPATCH = 0x00020003ull;              // ..._DISPATCH_DESC_TYPE_FRAMEGENERATION
const uint64_t TYPE_FG_PREPARE = 0x00020004ull;               // ..._DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE
const uint64_t TYPE_FG_PREPARE_CAMERA = 0x0002000aull;        // ..._FRAMEGENERATION_PREPARE_CAMERAINFO (required by 3.1.4 / 3.1.5)

const uint32_t CREATE_DISPLAY_RES_MVEC = 1u << 1, CREATE_DEPTH_INVERTED = 1u << 3, CREATE_DEPTH_INFINITE = 1u << 4, CREATE_HDR = 1u << 5;
const uint32_t FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY = 1u << 3;
const uint32_t STATE_UNORDERED_ACCESS = 1u << 1, STATE_COMPUTE_READ = 1u << 2;
const uint32_t RESOURCE_TYPE_TEXTURE2D = 2u;
const uint32_t USAGE_RENDERTARGET = 1u << 0, USAGE_UAV = 1u << 1;
const uint32_t TRANSFER_SRGB = 0u, TRANSFER_PQ = 1u, TRANSFER_SCRGB = 2u;

struct CreateBackendDX12 { Header header; ID3D12Device *device; };
struct CreateFrameGeneration { Header header; uint32_t flags; Dim2D displaySize; Dim2D maxRenderSize; uint32_t backBufferFormat; };
typedef void (*MessageFn)(uint32_t type, const wchar_t *message);
struct ConfigureGlobalDebug1 { Header header; MessageFn fpMessage; uint32_t debugLevel; };
struct QueryProviderVersion { Header header; uint64_t versionId; const char *versionName; };
struct ConfigureFrameGeneration
{
    Header header;
    void *swapChain;
    void *presentCallback;
    void *presentCallbackUserContext;
    void *frameGenerationCallback;
    void *frameGenerationCallbackUserContext;
    bool frameGenerationEnabled;
    bool allowAsyncWorkloads;
    Resource HUDLessColor;
    uint32_t flags;
    bool onlyPresentGenerated;
    Rect2D generationRect;
    uint64_t frameID;
};
struct DispatchPrepare
{
    Header header;
    uint64_t frameID;
    uint32_t flags;
    void *commandList;
    Dim2D renderSize;
    Float2 jitterOffset;
    Float2 motionVectorScale;
    float frameTimeDelta;
    bool unused_reset;
    float cameraNear;
    float cameraFar;
    float cameraFovAngleVertical;
    float viewSpaceToMetersFactor;
    Resource depth;
    Resource motionVectors;
};
struct PrepareCameraInfo { Header header; float cameraPosition[3], cameraUp[3], cameraRight[3], cameraForward[3]; };
struct DispatchFrameGeneration
{
    Header header;
    void *commandList;
    Resource presentColor;
    Resource outputs[4];
    uint32_t numGeneratedFrames;
    bool reset;
    uint32_t backbufferTransferFunction;
    float minMaxLuminance[2];
    Rect2D generationRect;
    uint64_t frameID;
};

typedef uint32_t (*PfnCreate)(void **context, Header *desc, const void *memCb);
typedef uint32_t (*PfnDestroy)(void **context, const void *memCb);
typedef uint32_t (*PfnConfigure)(void **context, const Header *desc);
typedef uint32_t (*PfnQuery)(void **context, Header *desc);
typedef uint32_t (*PfnDispatch)(void **context, const Header *desc);

// The numbers are sizeof / offsetof of AMD's own structs (compiled from the SDK headers, x64).
static_assert(sizeof(Resource) == 48, "FfxApiResource");
static_assert(sizeof(CreateFrameGeneration) == 40 && offsetof(CreateFrameGeneration, backBufferFormat) == 36, "ffxCreateContextDescFrameGeneration");
static_assert(sizeof(ConfigureFrameGeneration) == 144 && offsetof(ConfigureFrameGeneration, HUDLessColor) == 64 &&
              offsetof(ConfigureFrameGeneration, generationRect) == 120 && offsetof(ConfigureFrameGeneration, frameID) == 136, "ffxConfigureDescFrameGeneration");
static_assert(sizeof(DispatchPrepare) == 184 && offsetof(DispatchPrepare, commandList) == 32 && offsetof(DispatchPrepare, cameraNear) == 72 &&
              offsetof(DispatchPrepare, depth) == 88 && offsetof(DispatchPrepare, motionVectors) == 136, "ffxDispatchDescFrameGenerationPrepare");
static_assert(sizeof(PrepareCameraInfo) == 64, "ffxDispatchDescFrameGenerationPrepareCameraInfo");
static_assert(sizeof(DispatchFrameGeneration) == 312 && offsetof(DispatchFrameGeneration, outputs) == 72 &&
              offsetof(DispatchFrameGeneration, numGeneratedFrames) == 264 && offsetof(DispatchFrameGeneration, generationRect) == 284 &&
              offsetof(DispatchFrameGeneration, frameID) == 304, "ffxDispatchDescFrameGeneration");
}   // namespace ffx

HMODULE g_mod = nullptr;
bool g_loaded_by_us = false;
ffx::PfnCreate g_create = nullptr;
ffx::PfnDestroy g_destroy = nullptr;
ffx::PfnConfigure g_configure = nullptr;
ffx::PfnQuery g_query = nullptr;
ffx::PfnDispatch g_dispatch = nullptr;
void *g_ctx = nullptr;
info g_i{};
uint64_t g_frame_id = 0;
LARGE_INTEGER g_last{};
float g_units_m = 1.0f;          // FGFsrUnitsM: view-space units per metre factor FSR asks for
int g_hdr10 = 0;                 // FGFsrHdr10=1: the picture is PQ (HDR10)
unsigned g_msgs = 0;
bool g_said_cam = false;

uint32_t surface_format(DXGI_FORMAT f)   // ffxApiGetSurfaceFormatDX12, the formats the bridge can hand over
{
    switch (f)
    {
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return 3u;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return 4u;
    case DXGI_FORMAT_R32G32_FLOAT:       return 6u;
    case DXGI_FORMAT_R8G8B8A8_UNORM:     return 10u;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:return 12u;
    case DXGI_FORMAT_B8G8R8A8_UNORM:     return 14u;
    case DXGI_FORMAT_R11G11B10_FLOAT:    return 16u;
    case DXGI_FORMAT_R10G10B10A2_UNORM:  return 17u;
    case DXGI_FORMAT_R16G16_FLOAT:       return 18u;
    case DXGI_FORMAT_R16_FLOAT:          return 21u;
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_UNORM:          return 23u;
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:          return 28u;
    default:                             return 0u;   // FFX_API_SURFACE_FORMAT_UNKNOWN
    }
}

ffx::Resource resource_of(ID3D12Resource *r, uint32_t state)   // ffxApiGetResourceDX12, textures only
{
    ffx::Resource o{};
    o.resource = r;
    o.state = state;
    if (r == nullptr) return o;
    const D3D12_RESOURCE_DESC d = r->GetDesc();
    o.description.type = ffx::RESOURCE_TYPE_TEXTURE2D;
    o.description.format = surface_format(d.Format);
    o.description.width = (uint32_t)d.Width;
    o.description.height = d.Height;
    o.description.depth = d.DepthOrArraySize;
    o.description.mipCount = d.MipLevels;
    o.description.flags = 0u;
    o.description.usage = ((d.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ? ffx::USAGE_UAV : 0u) |
                          ((d.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ? ffx::USAGE_RENDERTARGET : 0u);
    return o;
}

void on_message(uint32_t type, const wchar_t *message)
{
    if (message == nullptr || g_msgs >= 24u) return;
    ++g_msgs;
    char l[600];
    snprintf(l, sizeof l, "[MGPU][R380] FSR says (%s): %ls%s", type == 0u ? "error" : "warning", message,
             g_msgs == 24u ? " | further messages are not printed" : "");
    if (type == 0u) mgpu::diag::warn(l); else mgpu::diag::info(l);
}

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

void read_keys()
{
    g_units_m = 1.0f; g_hdr10 = 0;
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
    const char *u = mgpu::config::find(buf.data(), got, "FGFsrUnitsM");
    if (u != nullptr) { const double d = strtod(u, nullptr); if (d > 0.0001 && d < 100000.0) g_units_m = (float)d; }
    const char *h = mgpu::config::find(buf.data(), got, "FGFsrHdr10");
    g_hdr10 = (h != nullptr && atoi(h) == 1) ? 1 : 0;
}

// The DLL: one the game already has in the process, else from the game's folder, else a copy under mgpu\.
bool load(char *where, size_t where_n)
{
    static const wchar_t *resident[2] = { L"amd_fidelityfx_dx12.dll", L"amd_fidelityfx_framegeneration_dx12.dll" };
    for (int k = 0; k < 2 && g_mod == nullptr; ++k)
    {
        g_mod = GetModuleHandleW(resident[k]);
        if (g_mod != nullptr) snprintf(where, where_n, "%ls, already loaded by the game", resident[k]);
    }
    if (g_mod == nullptr)
    {
        wchar_t dir[MAX_PATH] = L"";
        (void)module_dir(dir, MAX_PATH);
        wchar_t cand[4][MAX_PATH];
        _snwprintf_s(cand[0], MAX_PATH, _TRUNCATE, L"amd_fidelityfx_dx12.dll");                                    // the game's folder (default search)
        _snwprintf_s(cand[1], MAX_PATH, _TRUNCATE, L"%lsmgpu\\amd_fidelityfx_framegeneration_dx12.dll", dir);
        _snwprintf_s(cand[2], MAX_PATH, _TRUNCATE, L"%lsmgpu\\amd_fidelityfx_dx12.dll", dir);
        _snwprintf_s(cand[3], MAX_PATH, _TRUNCATE, L"amd_fidelityfx_framegeneration_dx12.dll");
        for (int k = 0; k < 4 && g_mod == nullptr; ++k)
        {
            g_mod = LoadLibraryW(cand[k]);
            if (g_mod != nullptr) { g_loaded_by_us = true; snprintf(where, where_n, "%ls, loaded by the bridge", cand[k]); }
        }
    }
    if (g_mod == nullptr) return false;
    g_create = (ffx::PfnCreate)(void *)GetProcAddress(g_mod, "ffxCreateContext");
    g_destroy = (ffx::PfnDestroy)(void *)GetProcAddress(g_mod, "ffxDestroyContext");
    g_configure = (ffx::PfnConfigure)(void *)GetProcAddress(g_mod, "ffxConfigure");
    g_query = (ffx::PfnQuery)(void *)GetProcAddress(g_mod, "ffxQuery");
    g_dispatch = (ffx::PfnDispatch)(void *)GetProcAddress(g_mod, "ffxDispatch");
    return g_create != nullptr && g_destroy != nullptr && g_configure != nullptr && g_dispatch != nullptr;
}
}   // namespace

bool create(const info &i, char *why, size_t why_n)
{
    g_i = i;
    g_frame_id = 0; g_last.QuadPart = 0; g_msgs = 0u; g_said_cam = false;
    read_keys();
    char where[MAX_PATH * 2 + 60] = "";
    if (!load(where, sizeof where))
    {
        snprintf(why, why_n, "%s", g_mod == nullptr
                 ? "no FidelityFX API DLL was found: amd_fidelityfx_dx12.dll is not loaded by the game and not in its folder, and there is no "
                   "amd_fidelityfx_framegeneration_dx12.dll or amd_fidelityfx_dx12.dll in the mgpu folder. The bridge does not ship AMD's DLL"
                 : "the DLL found does not export the FidelityFX API (ffxCreateContext / ffxConfigure / ffxDispatch)");
        return false;
    }
    const unsigned mw = i.mvec_w != 0u ? i.mvec_w : i.w, mh = i.mvec_h != 0u ? i.mvec_h : i.h;
    const unsigned dw = i.depth_w != 0u ? i.depth_w : i.w, dh = i.depth_h != 0u ? i.depth_h : i.h;
    if (dw != mw || dh != mh)
    {
        snprintf(why, why_n, "FSR needs depth and vectors of one size (its render size) and they are %ux%u and %ux%u. DepthTransport=3 gives depth the render size",
                 dw, dh, mw, mh);
        return false;
    }
    if (surface_format(i.fmt) == 0u)
    {
        snprintf(why, why_n, "the picture's format %u has no FidelityFX surface format in this build", (unsigned)i.fmt);
        return false;
    }
    const bool hdr = (i.fmt == DXGI_FORMAT_R16G16B16A16_FLOAT) || g_hdr10 != 0;
    ffx::CreateBackendDX12 be{};
    be.header.type = ffx::TYPE_BACKEND_DX12;
    be.device = i.ndev;
    ffx::CreateFrameGeneration cd{};
    cd.header.type = ffx::TYPE_FG_CREATE;
    cd.header.pNext = &be.header;
    cd.flags = (i.depth_inverted ? (ffx::CREATE_DEPTH_INVERTED | ffx::CREATE_DEPTH_INFINITE) : 0u) | (hdr ? ffx::CREATE_HDR : 0u) |
               ((mw == i.w && mh == i.h) ? ffx::CREATE_DISPLAY_RES_MVEC : 0u);
    cd.displaySize.width = i.w; cd.displaySize.height = i.h;
    cd.maxRenderSize.width = mw; cd.maxRenderSize.height = mh;
    cd.backBufferFormat = surface_format(i.fmt);
    void *ctx = nullptr;
    const uint32_t rc = g_create(&ctx, &cd.header, nullptr);
    if (rc != 0u || ctx == nullptr)
    {
        snprintf(why, why_n, "ffxCreateContext(frame generation) returned %u from %s (2 = structure type not known to this DLL, 4 = no provider, "
                 "3 = the D3D12 device refused something)", (unsigned)rc, where);
        return false;
    }
    g_ctx = ctx;
    ffx::ConfigureGlobalDebug1 dbg{};
    dbg.header.type = ffx::TYPE_GLOBALDEBUG1;
    dbg.fpMessage = &on_message;
    dbg.debugLevel = 0x3u;   // errors and warnings
    (void)g_configure(&g_ctx, &dbg.header);
    const char *vname = "not reported";
    ffx::QueryProviderVersion pv{};
    pv.header.type = ffx::TYPE_GET_PROVIDER_VERSION;
    if (g_query != nullptr && g_query(&g_ctx, &pv.header) == 0u && pv.versionName != nullptr) vname = pv.versionName;
    char l[900];
    snprintf(l, sizeof l,
             "[MGPU][R380] AMD FSR FRAME GENERATION UP on the neural card (FrameGen=3): provider %s from %s | picture %ux%u format %u | "
             "render size (vectors, depth) %ux%u%s | depth %s | %s | view-to-metres %.3f (FGFsrUnitsM) | no AMD swap chain: the frames go "
             "through the bridge's own pacing thread. Design: davidm71.",
             vname, where, i.w, i.h, (unsigned)i.fmt, mw, mh, (cd.flags & ffx::CREATE_DISPLAY_RES_MVEC) ? " (display resolution)" : "",
             i.depth_inverted ? "inverted, infinite far plane" : "not inverted",
             (i.fmt == DXGI_FORMAT_R16G16B16A16_FLOAT) ? "scRGB (HDR)" : (g_hdr10 != 0 ? "PQ (HDR10, FGFsrHdr10=1)" : "sRGB (SDR)"), (double)g_units_m);
    mgpu::diag::info(l);
    return true;
}

unsigned evaluate(ID3D12GraphicsCommandList *cl, ID3D12Resource *real, ID3D12Resource *mvec, ID3D12Resource *depth,
                  ID3D12Resource *const *outs, unsigned n, float mv_el, bool reset, const mgpu::slcam::camera *cam)
{
    if (g_ctx == nullptr || g_dispatch == nullptr || cl == nullptr || real == nullptr || outs == nullptr || n == 0u) return 1u;
    if (n > 4u) n = 4u;
    LARGE_INTEGER now{}, fq{};
    QueryPerformanceCounter(&now); QueryPerformanceFrequency(&fq);
    float dt_ms = 16.0f;
    if (g_last.QuadPart != 0 && fq.QuadPart != 0) dt_ms = (float)((double)(now.QuadPart - g_last.QuadPart) * 1000.0 / (double)fq.QuadPart);
    if (!(dt_ms >= 1.0f && dt_ms <= 200.0f)) dt_ms = 16.0f;
    g_last = now;
    const uint64_t fid = ++g_frame_id;   // must rise by exactly one per frame

    ffx::ConfigureFrameGeneration cfg{};
    cfg.header.type = ffx::TYPE_FG_CONFIGURE;
    cfg.frameGenerationEnabled = true;
    cfg.allowAsyncWorkloads = false;
    cfg.flags = ffx::FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
    cfg.onlyPresentGenerated = false;
    cfg.generationRect.left = 0; cfg.generationRect.top = 0; cfg.generationRect.width = (int32_t)g_i.w; cfg.generationRect.height = (int32_t)g_i.h;
    cfg.frameID = fid;
    uint32_t rc = g_configure(&g_ctx, &cfg.header);
    if (rc != 0u) return rc;

    const unsigned mw = g_i.mvec_w != 0u ? g_i.mvec_w : g_i.w, mh = g_i.mvec_h != 0u ? g_i.mvec_h : g_i.h;
    const float aspect = (g_i.h != 0u) ? (float)g_i.w / (float)g_i.h : 16.0f / 9.0f;
    const float sgn = g_i.invert ? -1.0f : 1.0f;
    ffx::PrepareCameraInfo ci{};
    ci.header.type = ffx::TYPE_FG_PREPARE_CAMERA;
    ci.cameraUp[1] = 1.0f; ci.cameraRight[0] = 1.0f; ci.cameraForward[2] = 1.0f;   // no camera stated: at the origin, looking down +z
    ffx::DispatchPrepare pr{};
    pr.header.type = ffx::TYPE_FG_PREPARE;
    pr.header.pNext = &ci.header;
    pr.frameID = fid;
    pr.flags = ffx::FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
    pr.commandList = cl;
    pr.renderSize.width = mw; pr.renderSize.height = mh;
    pr.jitterOffset.x = 0.0f; pr.jitterOffset.y = 0.0f;
    // AMD's header: "pixelSpaceMV = motionVector * motionVectorScale". The lane's scale is that factor; times the frames elapsed.
    pr.motionVectorScale.x = sgn * g_i.mvec_scale_x * mv_el;
    pr.motionVectorScale.y = sgn * g_i.mvec_scale_y * mv_el;
    pr.frameTimeDelta = dt_ms;
    pr.unused_reset = false;
    pr.cameraNear = g_i.cam_near;
    pr.cameraFar = g_i.cam_far;
    pr.cameraFovAngleVertical = 2.0f * atanf(tanf(g_i.fov_h_deg * 3.14159265f / 180.0f * 0.5f) / aspect);
    pr.viewSpaceToMetersFactor = g_units_m;
    if (cam != nullptr)
    {   // R379: the game's own camera for this pair
        memcpy(ci.cameraPosition, cam->pos, 12); memcpy(ci.cameraUp, cam->up, 12);
        memcpy(ci.cameraRight, cam->right, 12); memcpy(ci.cameraForward, cam->fwd, 12);
        if (cam->have_lens) { pr.cameraNear = cam->cam_near; pr.cameraFar = cam->cam_far; pr.cameraFovAngleVertical = cam->fov; }
        if (!g_said_cam)
        {
            g_said_cam = true;
            mgpu::diag::info("[MGPU][R380] the game's camera (FGCamera) is handed to FSR: position and axes, and near / far / field of view "
                             "when the game stated them. Said once.");
        }
    }
    pr.depth = resource_of(depth, ffx::STATE_COMPUTE_READ);
    pr.motionVectors = resource_of(mvec, ffx::STATE_COMPUTE_READ);
    rc = g_dispatch(&g_ctx, &pr.header);
    if (rc != 0u) return rc;

    ffx::DispatchFrameGeneration dg{};
    dg.header.type = ffx::TYPE_FG_DISPATCH;
    dg.commandList = cl;
    dg.presentColor = resource_of(real, ffx::STATE_COMPUTE_READ);
    for (unsigned k = 0; k < n; ++k) dg.outputs[k] = resource_of(outs[k], ffx::STATE_UNORDERED_ACCESS);
    dg.numGeneratedFrames = n;
    dg.reset = reset;
    const bool scrgb = (g_i.fmt == DXGI_FORMAT_R16G16B16A16_FLOAT);
    dg.backbufferTransferFunction = scrgb ? ffx::TRANSFER_SCRGB : (g_hdr10 != 0 ? ffx::TRANSFER_PQ : ffx::TRANSFER_SRGB);
    dg.minMaxLuminance[0] = 0.0f;
    dg.minMaxLuminance[1] = (scrgb || g_hdr10 != 0) ? 1000.0f : 1.0f;
    dg.generationRect.left = 0; dg.generationRect.top = 0; dg.generationRect.width = (int32_t)g_i.w; dg.generationRect.height = (int32_t)g_i.h;
    dg.frameID = fid;
    return g_dispatch(&g_ctx, &dg.header);
}

// R397: the vector scale for the next evaluate (bridge thread, before evaluate). fg_gpu1.cpp hands the arm's copy -
// the value already held - unless FGMvScaleNow=1.
void set_scale(float x, float y)
{
    g_i.mvec_scale_x = x; g_i.mvec_scale_y = y;
}

void release()
{
    if (g_ctx != nullptr && g_destroy != nullptr) (void)g_destroy(&g_ctx, nullptr);
    g_ctx = nullptr;
    // The DLL stays loaded: a library the bridge loaded is not unloaded while the game may still hold work of it.
}
}   // namespace fgfsr
}   // namespace mgpu
