// MGPU Bridge - the DX11 producer. See dx11_producer.hpp. R227.
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "dx11_producer.hpp"
#include "diag.hpp"
#include "adapter.hpp"
#include "gpu1_context.hpp"
#include "mgpu_ini_parser.hpp"

namespace mgpu::dx11producer
{
namespace
{
std::atomic<int> g_mode{0};                 // 0 off, 1 on
std::atomic<bool> g_inited{false};
bool g_ready = false, g_failed = false;
unsigned g_w = 0, g_h = 0, g_fmt = 0;

// ---- D3D11 side (the game's device; not owned) ----
ID3D11Device *g_dev11 = nullptr;
ID3D11Texture2D *g_color11 = nullptr;       // ours, SHARED_NTHANDLE
ID3D11Texture2D *g_depth11 = nullptr;       // ours, SHARED_NTHANDLE, made at the first depth
ID3D11Texture2D *g_mvec11 = nullptr;        // R232: ours, SHARED_NTHANDLE, made at the first vector surface
ID3D11Fence *g_fence11 = nullptr;
HANDLE g_color_h = nullptr, g_depth_h = nullptr, g_fence_h = nullptr, g_mvec_h = nullptr;
unsigned g_dw = 0, g_dh = 0, g_dfmt = 0;    // the depth texture's own description
unsigned g_mw = 0, g_mh = 0, g_mfmt = 0;    // R232: the vector texture's own description
bool g_mvec_fresh = false;                  // R232: a vector copy landed since the last finish_effects

// ---- D3D12 side (ours, on the game's adapter) ----
ID3D12Device *g_dev12 = nullptr;
ID3D12CommandQueue *g_queue = nullptr;
ID3D12CommandAllocator *g_alloc[2] = {};    // two in flight: the list of frame f-1 may still run
ID3D12GraphicsCommandList *g_list = nullptr;
ID3D12Fence *g_fence12 = nullptr;           // the D3D11 fence, opened here
ID3D12Fence *g_done = nullptr;              // ours: our list of frame f has executed
HANDLE g_done_ev = nullptr;
ID3D12Resource *g_color12 = nullptr;        // the shared colour texture, opened here
ID3D12Resource *g_depth12 = nullptr;        // the shared depth texture, opened here
ID3D12Resource *g_mvec12 = nullptr;         // R232: the shared vector texture, opened here

unsigned long long g_value = 0;             // shared fence: D3D11 signals, D3D12 waits
unsigned long long g_done_value = 0;
unsigned long long g_frames = 0, g_depth_frames = 0, g_depth_remakes = 0, g_waits = 0;
unsigned long long g_mvec_frames = 0, g_mvec_remakes = 0;   // R232
unsigned long long g_wait_ms_total = 0;

template <class T> void rel(T *&p) { if (p != nullptr) { p->Release(); p = nullptr; } }

bool addon_dir(wchar_t *out, size_t n)
{
    HMODULE hm = nullptr;
    static int anchor = 0;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&anchor), &hm))
        return false;
    const DWORD len = GetModuleFileNameW(hm, out, (DWORD)n);
    if (len == 0 || len >= n) return false;
    wchar_t *slash = wcsrchr(out, L'\\');
    if (slash == nullptr) return false;
    slash[1] = L'\0';
    return true;
}

int read_key()
{
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    if (!addon_dir(dir, MAX_PATH)) return 0;
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%lsmgpu.ini", dir);
    FILE *f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || f == nullptr) return 0;
    static char buf[mgpu::config::MAX_BYTES + 2u];
    const size_t got = fread(buf, 1, mgpu::config::MAX_BYTES + 1u, f);
    fclose(f);
    if (got > mgpu::config::MAX_BYTES) return 0;
    buf[got] = '\0';
    const char *v = mgpu::config::find(buf, got, "DX11");
    return (v != nullptr && *v == '1') ? 1 : 0;
}

void fail(const char *step, HRESULT hr)
{
    g_failed = true;
    char l[320];
    snprintf(l, sizeof l, "[MGPU][DX11P] STOPPED at %s: hr=0x%08X. The stream will not arm on this "
                          "title; the game is untouched. The step named is the one to solve.",
             step, (unsigned)hr);
    mgpu::diag::error(l);
}

// R234. A non-fatal failure: the lane named is absent for the session, the
// producer carries on with what it has. fail() stays for the things the
// frame itself needs (colour, the fence, the queue).
void note_absent(const char *what, const char *step, HRESULT hr)
{
    char l[420];
    snprintf(l, sizeof l, "[MGPU][DX11P] %s ABSENT for this session: %s hr=0x%08X. Colour (and whatever else "
                          "was made) continues; the stream sees no %s on this path.",
             what, step, (unsigned)hr, what);
    mgpu::diag::warn(l);
}

// R234. The typeless twin of a format, for the ladder's last rung. Copies
// between a typed format and its typeless twin are legal (same group).
unsigned typeless_of(unsigned f)
{
    switch ((DXGI_FORMAT)f)
    {
    case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R32G32B32A32_UINT: case DXGI_FORMAT_R32G32B32A32_SINT:
        return DXGI_FORMAT_R32G32B32A32_TYPELESS;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM: case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SNORM: case DXGI_FORMAT_R16G16B16A16_SINT:
        return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    case DXGI_FORMAT_R32G32_FLOAT: case DXGI_FORMAT_R32G32_UINT: case DXGI_FORMAT_R32G32_SINT:
        return DXGI_FORMAT_R32G32_TYPELESS;
    case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_UINT:
        return DXGI_FORMAT_R10G10B10A2_TYPELESS;
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R8G8B8A8_SNORM: case DXGI_FORMAT_R8G8B8A8_SINT:
        return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R16G16_UNORM: case DXGI_FORMAT_R16G16_UINT:
    case DXGI_FORMAT_R16G16_SNORM: case DXGI_FORMAT_R16G16_SINT:
        return DXGI_FORMAT_R16G16_TYPELESS;
    case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R32_UINT: case DXGI_FORMAT_R32_SINT:
        return DXGI_FORMAT_R32_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    default: return 0u;
    }
}

// R234. What the game's device says about a format, once per format, so a
// refused shared texture is read against facts and not guessed at.
void describe_format_once(unsigned fmt)
{
    static unsigned seen[8]; static unsigned n = 0;
    for (unsigned i = 0; i < n; ++i) if (seen[i] == fmt) return;
    if (n < 8) seen[n++] = fmt;
    UINT sup = 0;
    const HRESULT hr = g_dev11->CheckFormatSupport((DXGI_FORMAT)fmt, &sup);
    char l[420];
    snprintf(l, sizeof l,
             "[MGPU][DX11P] game device feature level 0x%04X | CheckFormatSupport(fmt=%u) hr=0x%08X bits=0x%08X: "
             "TEXTURE2D=%d SHADER_LOAD=%d SHADER_SAMPLE=%d RENDER_TARGET=%d CPU_LOCKABLE=%d TYPED_UAV=%d",
             (unsigned)g_dev11->GetFeatureLevel(), fmt, (unsigned)hr, (unsigned)sup,
             (sup & D3D11_FORMAT_SUPPORT_TEXTURE2D) != 0, (sup & D3D11_FORMAT_SUPPORT_SHADER_LOAD) != 0,
             (sup & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE) != 0, (sup & D3D11_FORMAT_SUPPORT_RENDER_TARGET) != 0,
             (sup & D3D11_FORMAT_SUPPORT_CPU_LOCKABLE) != 0, (sup & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW) != 0);
    mgpu::diag::info(l);
}

// R235. The other direction: the texture is created on OUR D3D12 device
// (shared heap, RENDER_TARGET flag), its NT handle exported, and the game's
// D3D11 device opens it. The strict sharing validation then sits on the
// D3D12 side. TR-12: D3D11-side creation refused R32G32_FLOAT on every rung
// while CheckFormatSupport allowed everything asked. Three hrs out, so the
// ladder line says which step refused.
bool make_reverse(unsigned w, unsigned h, unsigned fmt, ID3D11Texture2D **out11, HANDLE *outh,
                  ID3D12Resource **out12, HRESULT &hr_create, HRESULT &hr_handle, HRESULT &hr_open)
{
    hr_create = hr_handle = hr_open = E_FAIL;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = (DXGI_FORMAT)fmt; rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    hr_create = g_dev12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON,
                                                 nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void **>(out12));
    if (FAILED(hr_create)) return false;
    hr_handle = g_dev12->CreateSharedHandle(*out12, nullptr, GENERIC_ALL, nullptr, outh);
    if (FAILED(hr_handle)) { rel(*out12); return false; }
    ID3D11Device1 *dev1 = nullptr;
    hr_open = g_dev11->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void **>(&dev1));
    if (SUCCEEDED(hr_open))
    {
        hr_open = dev1->OpenSharedResource1(*outh, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(out11));
        dev1->Release();
    }
    if (FAILED(hr_open)) { rel(*out12); CloseHandle(*outh); *outh = nullptr; return false; }
    return true;
}


unsigned g_carrier_fmt = 0;                   // R237: chosen by carrier_census; 0 = none shareable
// R235. Once, at build: which carrier formats this pair of devices will
// share, each direction - so a typed copy, if one is ever needed, is sized
// from facts. Everything made here is released at once.
void carrier_census()
{
    const unsigned F[] = { DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
                           DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32_UINT, DXGI_FORMAT_R32_FLOAT };
    g_carrier_fmt = 0u;
    char l[640]; int n = snprintf(l, sizeof l, "[MGPU][DX11P] carrier census 256x256 (fmt: D3D11-made SRV|RTV / D3D12-made D3D11-opened):");
    for (unsigned f : F)
    {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = 256; td.Height = 256; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = (DXGI_FORMAT)f; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
        ID3D11Texture2D *t = nullptr;
        const HRESULT a = g_dev11->CreateTexture2D(&td, nullptr, &t); rel(t);
        ID3D11Texture2D *t11 = nullptr; ID3D12Resource *t12 = nullptr; HANDLE hh = nullptr; HRESULT c, hd, o;
        const bool rv = make_reverse(256, 256, f, &t11, &hh, &t12, c, hd, o);
        rel(t11); rel(t12); if (hh != nullptr) CloseHandle(hh);
        if (SUCCEEDED(a) && g_carrier_fmt == 0u &&
            (f == DXGI_FORMAT_R16G16_FLOAT || f == DXGI_FORMAT_R16G16B16A16_FLOAT || f == DXGI_FORMAT_R32G32B32A32_FLOAT))
            g_carrier_fmt = f;   // R237: first shareable of RG16F, RGBA16F, RGBA32F (the array's order)
        n += snprintf(l + n, sizeof l - (size_t)n, " %u:%s/%s(0x%08X)", f, SUCCEEDED(a) ? "ok" : "no",
                      rv ? "ok" : "no", (unsigned)(FAILED(c) ? c : (FAILED(hd) ? hd : o)));
        if (n >= (int)sizeof l - 40) break;
    }
    mgpu::diag::info(l);
}

// R234. The shared texture, by a ladder: the same flags colour and depth use
// first, then fewer binds, then no bind (copies only), then the typeless
// twin. Every rung's hr is in one line, with the rung that made it. fatal:
// the frame cannot go on without it (colour) - fail(); else note_absent().
// *made_typeless says the last rung was the one: the caller decides whether
// a typeless surface is usable where it was headed.
bool make_shared_tex(unsigned w, unsigned h, unsigned fmt, ID3D11Texture2D **out11, HANDLE *outh,
                     ID3D12Resource **out12, const char *what, bool fatal, bool *made_typeless)
{
    describe_format_once(fmt);
    if (made_typeless != nullptr) *made_typeless = false;
    const unsigned tl = typeless_of(fmt);
    struct rung { const char *name; unsigned fmt; UINT bind; };
    const rung R[] = {
        { "SRV|RTV",      fmt, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET },
        { "SRV",          fmt, D3D11_BIND_SHADER_RESOURCE },
        { "no-bind",      fmt, 0u },
        { "typeless SRV", tl,  D3D11_BIND_SHADER_RESOURCE },
    };
    HRESULT hrs[4] = { E_FAIL, E_FAIL, E_FAIL, E_FAIL };
    int made = -1;
    HRESULT rv_c = E_FAIL, rv_h = E_FAIL, rv_o = E_FAIL;   // R235: the reverse rung's three steps
    bool reverse = false;
    for (int i = 0; i < 4 && made < 0; ++i)
    {
        if (R[i].fmt == 0u) { hrs[i] = E_INVALIDARG; continue; }   // no typeless twin
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = (DXGI_FORMAT)R[i].fmt; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = R[i].bind;
        td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
        hrs[i] = g_dev11->CreateTexture2D(&td, nullptr, out11);
        if (SUCCEEDED(hrs[i])) made = i;
    }
    if (made < 0 && make_reverse(w, h, fmt, out11, outh, out12, rv_c, rv_h, rv_o)) { made = 4; reverse = true; }
    {
        char l[520];
        snprintf(l, sizeof l,
                 "[MGPU][DX11P] shared texture %s %ux%u fmt=%u ladder: SRV|RTV=0x%08X SRV=0x%08X no-bind=0x%08X "
                 "typeless(%u) SRV=0x%08X | D3D12-made/D3D11-opened: create=0x%08X handle=0x%08X open=0x%08X -> %s",
                 what, w, h, fmt, (unsigned)hrs[0], (unsigned)hrs[1], (unsigned)hrs[2], tl, (unsigned)hrs[3],
                 (unsigned)rv_c, (unsigned)rv_h, (unsigned)rv_o,
                 made < 0 ? "NONE made" : (made == 4 ? "D3D12-made, D3D11-opened (R235)" : R[made].name));
        mgpu::diag::info(l);
    }
    if (made < 0)
    {
        char s[96]; snprintf(s, sizeof s, "D3D11 CreateTexture2D(%s, SHARED_NTHANDLE)", what);
        if (fatal) fail(s, hrs[0]); else note_absent(what, s, hrs[0]);
        return false;
    }
    if (made == 3 && made_typeless != nullptr) *made_typeless = true;
    if (reverse) return true;   // R235: handle and D3D12 twin already made by make_reverse
    IDXGIResource1 *res1 = nullptr;
    HRESULT hr = (*out11)->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void **>(&res1));
    if (FAILED(hr)) { if (fatal) fail("QueryInterface(IDXGIResource1)", hr); else note_absent(what, "QueryInterface(IDXGIResource1)", hr); return false; }
    hr = res1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, outh);
    res1->Release();
    if (FAILED(hr)) { char s[96]; snprintf(s, sizeof s, "CreateSharedHandle(%s)", what); if (fatal) fail(s, hr); else note_absent(what, s, hr); return false; }
    hr = g_dev12->OpenSharedHandle(*outh, __uuidof(ID3D12Resource), reinterpret_cast<void **>(out12));
    if (FAILED(hr)) { char s[96]; snprintf(s, sizeof s, "D3D12 OpenSharedHandle(%s)", what); if (fatal) fail(s, hr); else note_absent(what, s, hr); return false; }
    return true;
}

bool build()
{
    HRESULT hr;
    // Our D3D12 device on the game's adapter. The LUID is the game's.
    IDXGIDevice *dxgidev = nullptr;
    hr = g_dev11->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&dxgidev));
    if (FAILED(hr)) { fail("QueryInterface(IDXGIDevice)", hr); return false; }
    IDXGIAdapter *adapter = nullptr;
    hr = dxgidev->GetAdapter(&adapter);
    dxgidev->Release();
    if (FAILED(hr)) { fail("IDXGIDevice::GetAdapter", hr); return false; }
    hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), reinterpret_cast<void **>(&g_dev12));
    adapter->Release();
    if (FAILED(hr)) { fail("D3D12CreateDevice(game adapter)", hr); return false; }
    const LUID luid = g_dev12->GetAdapterLuid();

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = g_dev12->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void **>(&g_queue));
    if (FAILED(hr)) { fail("D3D12 CreateCommandQueue", hr); return false; }
    for (unsigned i = 0; i < 2u; ++i)
    {
        hr = g_dev12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), reinterpret_cast<void **>(&g_alloc[i]));
        if (FAILED(hr)) { fail("D3D12 CreateCommandAllocator", hr); return false; }
    }
    hr = g_dev12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc[0], nullptr, __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void **>(&g_list));
    if (FAILED(hr)) { fail("D3D12 CreateCommandList", hr); return false; }
    g_list->Close();
    hr = g_dev12->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void **>(&g_done));
    if (FAILED(hr)) { fail("D3D12 CreateFence(done)", hr); return false; }
    g_done_ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // The shared fence (D3D11.4) and its D3D12 twin.
    ID3D11Device5 *dev5 = nullptr;
    hr = g_dev11->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void **>(&dev5));
    if (FAILED(hr)) { fail("QueryInterface(ID3D11Device5) - no D3D11.4 fences", hr); return false; }
    hr = dev5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence), reinterpret_cast<void **>(&g_fence11));
    dev5->Release();
    if (FAILED(hr)) { fail("ID3D11Device5::CreateFence(SHARED)", hr); return false; }
    hr = g_fence11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &g_fence_h);
    if (FAILED(hr)) { fail("ID3D11Fence::CreateSharedHandle", hr); return false; }
    hr = g_dev12->OpenSharedHandle(g_fence_h, __uuidof(ID3D12Fence), reinterpret_cast<void **>(&g_fence12));
    if (FAILED(hr)) { fail("D3D12 OpenSharedHandle(fence)", hr); return false; }

    // The shared colour texture: the back buffer's size and format. The
    // producer sizes the slot from THIS resource's description (R30 v2), so
    // it must be the back buffer's, not a guess.
    if (!make_shared_tex(g_w, g_h, g_fmt, &g_color11, &g_color_h, &g_color12, "colour", true, nullptr)) return false;

    // Tell the selection whose adapter the game renders on. This is what
    // init_swapchain does for a D3D12 chain; on D3D11 the chain cannot say
    // it, the device's adapter can.
    mgpu::adapter::note_game_luid_d3d11((unsigned long long)luid.LowPart, (long)luid.HighPart);
    // Real vectors are D3D12-only in this build: MVec=3 would hold the arm
    // forever. The stream reads this at the request and uses the synthetic
    // field instead, saying so.
    mgpu::gpu1::stream_note_no_real_vectors("DX11: the bind hook and the NGX tap are D3D12 in this build");

    char l[420];
    snprintf(l, sizeof l,
             "[MGPU][DX11P] PRODUCER BUILT on the game's adapter luid=0x%08X-0x%08X: shared colour "
             "%ux%u fmt=%u, a D3D11.4 shared fence, our D3D12 device + queue. Depth follows the tap "
             "at its first frame. The existing producer, ring and seals run on this device from here.",
             (unsigned)luid.HighPart, (unsigned)luid.LowPart, g_w, g_h, g_fmt);
    mgpu::diag::info(l);
    carrier_census();   // R235
    return true;
}

// The depth shared texture mirrors the tap's own description; made at the
// first frame that has one, remade if the tap's resource changes size.
bool g_depth_refused = false, g_mvec_refused = false;   // R234: asked once, absent for the session

bool ensure_depth(ID3D11Texture2D *tap)
{
    if (g_depth_refused) return false;
    D3D11_TEXTURE2D_DESC d{};
    tap->GetDesc(&d);
    if (g_depth11 != nullptr && d.Width == g_dw && d.Height == g_dh && (unsigned)d.Format == g_dfmt) return true;
    if (g_depth11 != nullptr)
    {
        // A size change mid-run: the producer's arm sized the slot from the
        // old one and will reject the new (R63 size-rejected). Remake ours so
        // the copy is at least well-formed; the count says it happened.
        rel(g_depth12); rel(g_depth11);
        if (g_depth_h != nullptr) { CloseHandle(g_depth_h); g_depth_h = nullptr; }
        ++g_depth_remakes;
    }
    g_dw = d.Width; g_dh = d.Height; g_dfmt = (unsigned)d.Format;
    bool tl = false;
    if (!make_shared_tex(g_dw, g_dh, g_dfmt, &g_depth11, &g_depth_h, &g_depth12, "depth", false, &tl))
    { g_depth_refused = true; return false; }
    if (tl)
    {
        // The arm sizes the depth region from this resource's format and NR
        // binds a typed view of it: a typeless surface cannot be fed. Discovery
        // only - the ladder line above says what the device would share.
        mgpu::diag::warn("[MGPU][DX11P] depth could be shared only as TYPELESS on this device; the stream's depth "
                         "region takes a typed format, so depth is ABSENT for this session. Next piece: a typed copy.");
        rel(g_depth12); rel(g_depth11); if (g_depth_h != nullptr) { CloseHandle(g_depth_h); g_depth_h = nullptr; }
        g_depth_refused = true; return false;
    }
    char l[200];
    snprintf(l, sizeof l, "[MGPU][DX11P] depth shared texture %ux%u fmt=%u follows the tap (remakes so far %llu)",
             g_dw, g_dh, g_dfmt, g_depth_remakes);
    mgpu::diag::info(l);
    return true;
}

// R232. ensure_depth's shape for the vector surface.

// ---- R237: the typed copy, for a vector format this device will not share ----
//
// Direct first: ensure_mvec tries to share the game's own format (the R234
// ladder, the R235 reverse rung). Only when every rung refuses does this run:
// one pixel-shader pass on the game's D3D11 device reads the vectors as
// float2 through an SRV and writes them into a shared CARRIER whose format
// the census chose (first shareable of RG16F, RGBA16F, RGBA32F). The hop and
// the stream never learn which path ran. Every piece of immediate-context
// state the pass touches is read before and put back after.
bool g_mvec_converted = false;                // this session's vectors go through the pass
ID3D11VertexShader *g_cv_vs = nullptr;
ID3D11PixelShader *g_cv_ps = nullptr;
ID3D11RenderTargetView *g_cv_rtv = nullptr;   // on g_mvec11 (the carrier)
ID3D11ShaderResourceView *g_cv_srv = nullptr; // on the game's vector resource
ID3D11Resource *g_cv_src = nullptr;           // the resource g_cv_srv was made for (not owned)
bool g_cv_shaders_tried = false;

unsigned float_view_of(unsigned f)
{
    switch ((DXGI_FORMAT)f)
    {
    case DXGI_FORMAT_R32G32_TYPELESS:       return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS:       return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    default: return f;
    }
}

bool cv_shaders()
{
    if (g_cv_vs != nullptr && g_cv_ps != nullptr) return true;
    if (g_cv_shaders_tried) return false;
    g_cv_shaders_tried = true;
    typedef HRESULT (WINAPI *pfn_d3dcompile)(LPCVOID, SIZE_T, LPCSTR, const void *, void *,
                                             LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    pfn_d3dcompile compile = dc ? (pfn_d3dcompile)GetProcAddress(dc, "D3DCompile") : nullptr;
    if (compile == nullptr) { note_absent("mvec", "LoadLibrary(d3dcompiler_47)/D3DCompile for the typed copy", E_FAIL); return false; }
    static const char VS[] =
        "float4 main(uint id : SV_VertexID) : SV_Position {"
        " float2 p = float2((id << 1) & 2, id & 2);"
        " return float4(p * float2(2, -2) + float2(-1, 1), 0, 1); }";
    static const char PS[] =
        "Texture2D<float4> src : register(t0);"
        "float4 main(float4 pos : SV_Position) : SV_Target {"
        " return float4(src.Load(int3((int2)pos.xy, 0)).xy, 0, 0); }";
    ID3DBlob *b = nullptr, *e = nullptr;
    HRESULT hr = compile(VS, sizeof VS - 1, "mgpu_cv_vs", nullptr, nullptr, "main", "vs_5_0", 0, 0, &b, &e);
    if (SUCCEEDED(hr)) hr = g_dev11->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_cv_vs);
    if (b) b->Release(); if (e) e->Release(); b = e = nullptr;
    if (FAILED(hr)) { note_absent("mvec", "typed copy vertex shader (vs_5_0)", hr); return false; }
    hr = compile(PS, sizeof PS - 1, "mgpu_cv_ps", nullptr, nullptr, "main", "ps_5_0", 0, 0, &b, &e);
    if (SUCCEEDED(hr)) hr = g_dev11->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_cv_ps);
    if (b) b->Release(); if (e) e->Release();
    if (FAILED(hr)) { note_absent("mvec", "typed copy pixel shader (ps_5_0)", hr); rel(g_cv_vs); return false; }
    return true;
}

// The pass. Context state touched: RTV/DSV slot 0, viewport 0, VS, PS, PS SRV
// t0, input layout, topology, blend, depth-stencil and rasterizer state.
bool cv_run(ID3D11DeviceContext *ctx, ID3D11Resource *src, unsigned src_fmt)
{
    if (!cv_shaders()) return false;
    if (g_cv_rtv == nullptr)
    {
        if (FAILED(g_dev11->CreateRenderTargetView(g_mvec11, nullptr, &g_cv_rtv))) return false;
    }
    if (g_cv_srv == nullptr || g_cv_src != src)
    {
        rel(g_cv_srv);
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = (DXGI_FORMAT)float_view_of(src_fmt);
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        if (FAILED(g_dev11->CreateShaderResourceView(src, &sd, &g_cv_srv))) { g_cv_srv = nullptr; return false; }
        g_cv_src = src;
    }
    // save
    ID3D11RenderTargetView *o_rtv = nullptr; ID3D11DepthStencilView *o_dsv = nullptr;
    ctx->OMGetRenderTargets(1, &o_rtv, &o_dsv);
    UINT nvp = 1; D3D11_VIEWPORT o_vp{}; ctx->RSGetViewports(&nvp, &o_vp);
    ID3D11VertexShader *o_vs = nullptr; ID3D11PixelShader *o_ps = nullptr;
    ctx->VSGetShader(&o_vs, nullptr, nullptr); ctx->PSGetShader(&o_ps, nullptr, nullptr);
    ID3D11ShaderResourceView *o_srv = nullptr; ctx->PSGetShaderResources(0, 1, &o_srv);
    ID3D11InputLayout *o_il = nullptr; ctx->IAGetInputLayout(&o_il);
    D3D11_PRIMITIVE_TOPOLOGY o_topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED; ctx->IAGetPrimitiveTopology(&o_topo);
    ID3D11BlendState *o_bs = nullptr; FLOAT o_bf[4] = {}; UINT o_mask = 0; ctx->OMGetBlendState(&o_bs, o_bf, &o_mask);
    ID3D11DepthStencilState *o_ds = nullptr; UINT o_ref = 0; ctx->OMGetDepthStencilState(&o_ds, &o_ref);
    ID3D11RasterizerState *o_rs = nullptr; ctx->RSGetState(&o_rs);
    // ours
    ID3D11RenderTargetView *rtvs[1] = { g_cv_rtv };
    ID3D11ShaderResourceView *nulls[1] = { nullptr };
    ctx->PSSetShaderResources(0, 1, nulls);   // the source may be bound as a target: unbind first
    ctx->OMSetRenderTargets(1, rtvs, nullptr);
    D3D11_VIEWPORT vp{}; vp.Width = (FLOAT)g_mw; vp.Height = (FLOAT)g_mh; vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);
    ctx->VSSetShader(g_cv_vs, nullptr, 0); ctx->PSSetShader(g_cv_ps, nullptr, 0);
    ID3D11ShaderResourceView *srvs[1] = { g_cv_srv };
    ctx->PSSetShaderResources(0, 1, srvs);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
    ctx->OMSetDepthStencilState(nullptr, 0);
    ctx->RSSetState(nullptr);
    ctx->Draw(3, 0);
    // restore
    ctx->PSSetShaderResources(0, 1, nulls);
    ctx->OMSetRenderTargets(1, &o_rtv, o_dsv);
    if (nvp != 0) ctx->RSSetViewports(1, &o_vp);
    ctx->VSSetShader(o_vs, nullptr, 0); ctx->PSSetShader(o_ps, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &o_srv);
    ctx->IASetInputLayout(o_il);
    ctx->IASetPrimitiveTopology(o_topo);
    ctx->OMSetBlendState(o_bs, o_bf, o_mask);
    ctx->OMSetDepthStencilState(o_ds, o_ref);
    ctx->RSSetState(o_rs);
    rel(o_rtv); rel(o_dsv); rel(o_vs); rel(o_ps); rel(o_srv); rel(o_il); rel(o_bs); rel(o_ds); rel(o_rs);
    return true;
}

void cv_release()
{
    rel(g_cv_rtv); rel(g_cv_srv); g_cv_src = nullptr;
}

bool ensure_mvec(ID3D11Texture2D *src)
{
    if (g_mvec_refused) return false;
    D3D11_TEXTURE2D_DESC d{};
    src->GetDesc(&d);
    if (g_mvec11 != nullptr && d.Width == g_mw && d.Height == g_mh && (unsigned)d.Format == g_mfmt) return true;
    if (g_mvec11 != nullptr)
    {
        rel(g_mvec12); rel(g_mvec11);
        if (g_mvec_h != nullptr) { CloseHandle(g_mvec_h); g_mvec_h = nullptr; }
        ++g_mvec_remakes;
    }
    g_mw = d.Width; g_mh = d.Height; g_mfmt = (unsigned)d.Format;
    cv_release(); g_mvec_converted = false;
    bool tl = false;
    if (!make_shared_tex(g_mw, g_mh, g_mfmt, &g_mvec11, &g_mvec_h, &g_mvec12, "mvec", false, &tl))
    {
        // R237: the game's format will not share here. The carrier the census
        // chose, if any, and the pass. Same size; the format is the carrier's.
        if (g_carrier_fmt == 0u)
        {
            mgpu::diag::warn("[MGPU][DX11P] vectors: no carrier format shares on this device either (census) - "
                             "vectors ABSENT for this session.");
            g_mvec_refused = true; return false;
        }
        bool tl2 = false;
        if (!make_shared_tex(g_mw, g_mh, g_carrier_fmt, &g_mvec11, &g_mvec_h, &g_mvec12, "mvec-carrier", false, &tl2) || tl2)
        { g_mvec_refused = true; return false; }
        if (!cv_shaders()) { g_mvec_refused = true; return false; }
        g_mvec_converted = true;
        char c[300];
        snprintf(c, sizeof c, "[MGPU][DX11P] vectors: CONVERTED on the game's device, fmt=%u -> carrier fmt=%u %ux%u, one pixel "
                              "pass per evaluate (ps_5_0). The hop and the stream see the carrier. %s",
                 g_mfmt, g_carrier_fmt, g_mw, g_mh,
                 (g_carrier_fmt == DXGI_FORMAT_R16G16_FLOAT && (g_mfmt == DXGI_FORMAT_R32G32_FLOAT || g_mfmt == DXGI_FORMAT_R32G32_TYPELESS))
                     ? "Half precision from a 32-bit source." : "No precision lost.");
        mgpu::diag::info(c);
        return true;
    }
    if (tl)
    {
        mgpu::diag::warn("[MGPU][DX11P] vectors could be shared only as TYPELESS on this device; the stream's MVec "
                         "region takes a typed format, so vectors are ABSENT for this session. Next piece: a typed copy.");
        rel(g_mvec12); rel(g_mvec11); if (g_mvec_h != nullptr) { CloseHandle(g_mvec_h); g_mvec_h = nullptr; }
        g_mvec_refused = true; return false;
    }
    char l[260];
    snprintf(l, sizeof l, "[MGPU][DX11P] vectors: DIRECT copy - shared texture %ux%u fmt=%u follows the game's DLSS MotionVectors "
                          "(remakes so far %llu). The arm sizes the MVec region from this.",
             g_mw, g_mh, g_mfmt, g_mvec_remakes);
    mgpu::diag::info(l);
    return true;
}

void barrier(ID3D12Resource *r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER t{};
    t.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    t.Transition.pResource = r;
    t.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    t.Transition.StateBefore = a;
    t.Transition.StateAfter = b;
    g_list->ResourceBarrier(1, &t);
}
}   // namespace

void init(void *device, void *swapchain, unsigned width, unsigned height, unsigned dxgi_format)
{
    bool expect = false;
    if (!g_inited.compare_exchange_strong(expect, true, std::memory_order_acq_rel)) return;
    (void)swapchain;
    if (read_key() != 1) return;
    if (device == nullptr || width == 0 || height == 0) { fail("init: no D3D11 device or an empty chain", E_INVALIDARG); return; }
    g_dev11 = reinterpret_cast<ID3D11Device *>(device);
    g_w = width; g_h = height; g_fmt = dxgi_format;
    mgpu::diag::info("[MGPU][DX11P] DX11=1: building the D3D11 producer - a D3D12 device of ours on the "
                     "game's adapter, fed from the game's D3D11 context through shared textures and a "
                     "shared fence. Experimental. Real vectors and own Reflex are not on this path yet.");
    if (build()) { g_ready = true; g_mode.store(1, std::memory_order_release); }
    else shutdown();
}

bool on() { return g_mode.load(std::memory_order_acquire) == 1; }

bool is_game_device(void *device) { return device != nullptr && g_dev11 != nullptr && device == (void *)g_dev11; }

void on_chain_rebuilt(unsigned width, unsigned height, unsigned dxgi_format)
{
    if (!on()) return;
    if (width == g_w && height == g_h && dxgi_format == g_fmt) return;
    char l[300];
    snprintf(l, sizeof l,
             "[MGPU][DX11P] the game chain was rebuilt %ux%u fmt=%u (was %ux%u fmt=%u). The stream was sized "
             "from the first chain, so the DX11 producer stops feeding it for this session: NR holds its "
             "last frame. Relaunch to re-arm at the new size. (Build 1 limit; the D3D12 path's own "
             "rebuild handling is the model for the next one.)",
             width, height, dxgi_format, g_w, g_h, g_fmt);
    mgpu::diag::warn(l);
    g_failed = true;
}

void on_mvec(void *context, void *resource)
{
    if (!on() || !g_ready || g_failed) return;
    if (context == nullptr || resource == nullptr) return;
    ID3D11DeviceContext *ctx = reinterpret_cast<ID3D11DeviceContext *>(context);
    ID3D11Resource *res = reinterpret_cast<ID3D11Resource *>(resource);
    ID3D11Texture2D *tex = nullptr;
    if (FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&tex))) || tex == nullptr)
        return;   // not a 2D texture: nothing to size a region from
    if (ensure_mvec(tex))
    {
        bool ok = true;
        if (g_mvec_converted) ok = cv_run(ctx, res, g_mfmt);
        else ctx->CopyResource(g_mvec11, res);
        if (ok) g_mvec_fresh = true;
        if (ok)
        if (g_mvec_frames++ == 0ull) mgpu::gpu1::stream_note_real_vectors();   // R232: clears R227's degrade
    }
    tex->Release();
}

void on_finish_effects(void *context, void *back_buffer, void *depth)
{
    if (!on() || !g_ready || g_failed) return;
    if (context == nullptr || back_buffer == nullptr) return;
    ID3D11DeviceContext *ctx = reinterpret_cast<ID3D11DeviceContext *>(context);
    ID3D11Texture2D *bb = reinterpret_cast<ID3D11Texture2D *>(back_buffer);
    ID3D11Texture2D *tap = reinterpret_cast<ID3D11Texture2D *>(depth);

    // ---- D3D11: the copies and the signal, on the game's own context ----
    ctx->CopyResource(g_color11, bb);
    bool have_depth = false;
    if (tap != nullptr && ensure_depth(tap))
    {
        ctx->CopyResource(g_depth11, tap);
        have_depth = true;
        ++g_depth_frames;
    }
    // R232: a vector copy from this frame's evaluate, if one landed. The
    // CopyResource preceded this Signal on the same context, so the hop
    // orders it with colour and depth.
    const bool have_mvec = (g_mvec12 != nullptr) && g_mvec_fresh;
    g_mvec_fresh = false;
    if (g_failed) return;
    ID3D11DeviceContext4 *ctx4 = nullptr;
    HRESULT hr = ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), reinterpret_cast<void **>(&ctx4));
    if (FAILED(hr)) { fail("QueryInterface(ID3D11DeviceContext4)", hr); return; }
    ++g_value;
    hr = ctx4->Signal(g_fence11, g_value);
    ctx4->Release();
    if (FAILED(hr)) { fail("ID3D11DeviceContext4::Signal", hr); return; }

    // ---- D3D12: our queue waits for those copies ON THE GPU, then the
    //      existing producer records its copies into the ring slot ----
    const unsigned ai = (unsigned)(g_frames & 1ull);
    // The allocator of frame f-2 is reused: its list must have executed.
    // Two frames of slack makes that wait a no-op in steady state; a stall
    // of GPU 0 shows up here as g_waits, never as a game-thread wait.
    if (g_frames >= 2ull)
    {
        const unsigned long long need = g_done_value - 1ull;
        if (g_done->GetCompletedValue() < need)
        {
            ++g_waits;
            const ULONGLONG t0 = GetTickCount64();
            g_done->SetEventOnCompletion(need, g_done_ev);
            WaitForSingleObject(g_done_ev, 100);
            g_wait_ms_total += GetTickCount64() - t0;
        }
    }
    hr = g_queue->Wait(g_fence12, g_value);
    if (FAILED(hr)) { fail("ID3D12CommandQueue::Wait(shared fence)", hr); return; }
    g_alloc[ai]->Reset();
    g_list->Reset(g_alloc[ai], nullptr);

    // The producer transitions the colour source RENDER_TARGET -> COPY_SOURCE
    // -> RENDER_TARGET and copies depth with no barrier. The shared textures
    // rest in COMMON (the state sharing hands them over in), so this list
    // puts them where the producer expects and returns them after.
    barrier(g_color12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (have_depth) barrier(g_depth12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (have_mvec)
    {
        // R232: the D3D12 path's own per-frame vector copy into the current
        // slot's MVec region (stream_mvec_copy: inert unless MVec=3 and armed,
        // size-checked against the arm). Before the seal, as on D3D12.
        barrier(g_mvec12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        mgpu::gpu1::stream_mvec_copy(g_list, (unsigned long long)(uintptr_t)g_mvec12);
    }

    mgpu::gpu1::stream_on_finish_effects(
        g_list, g_queue,
        (unsigned long long)(uintptr_t)g_color12,
        have_depth ? (unsigned long long)(uintptr_t)g_depth12 : 0ull,
        (g_mvec12 != nullptr) ? (unsigned long long)(uintptr_t)g_mvec12 : 0ull);

    barrier(g_color12, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
    if (have_depth) barrier(g_depth12, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    if (have_mvec) barrier(g_mvec12, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    hr = g_list->Close();
    if (FAILED(hr)) { fail("D3D12 command list Close", hr); return; }
    ID3D12CommandList *ls[1] = { g_list };
    g_queue->ExecuteCommandLists(1, ls);
    mgpu::gpu1::stream_signal_after_submit(g_queue);   // R408a: SignalAt=2 - this frame, now that its list is submitted
    ++g_done_value;
    g_queue->Signal(g_done, g_done_value);
    ++g_frames;

    if ((g_frames % 1800ull) == 0ull)
    {
        char l[300];
        snprintf(l, sizeof l,
                 "[MGPU][DX11P] frames=%llu depth=%llu vectors=%llu | allocator waits=%llu (%llu ms total) - a wait here is "
                 "GPU 0 behind by two frames, never the game thread | depth remakes=%llu vector remakes=%llu",
                 g_frames, g_depth_frames, g_mvec_frames, g_waits, g_wait_ms_total, g_depth_remakes, g_mvec_remakes);
        mgpu::diag::info(l);
    }
}

void shutdown()
{
    g_mode.store(0, std::memory_order_release);
    g_ready = false;
    if (g_queue != nullptr && g_done != nullptr && g_done_value != 0ull && g_done_ev != nullptr)
    {
        // Bounded: our last list must finish before its allocator goes.
        if (g_done->GetCompletedValue() < g_done_value)
        {
            g_done->SetEventOnCompletion(g_done_value, g_done_ev);
            WaitForSingleObject(g_done_ev, 2000);
        }
    }
    rel(g_mvec12); rel(g_depth12); rel(g_color12); rel(g_fence12); rel(g_done);
    rel(g_list); rel(g_alloc[0]); rel(g_alloc[1]); rel(g_queue); rel(g_dev12);
    if (g_done_ev != nullptr) { CloseHandle(g_done_ev); g_done_ev = nullptr; }
    if (g_fence_h != nullptr) { CloseHandle(g_fence_h); g_fence_h = nullptr; }
    if (g_mvec_h != nullptr) { CloseHandle(g_mvec_h); g_mvec_h = nullptr; }
    if (g_depth_h != nullptr) { CloseHandle(g_depth_h); g_depth_h = nullptr; }
    if (g_color_h != nullptr) { CloseHandle(g_color_h); g_color_h = nullptr; }
    cv_release(); rel(g_cv_vs); rel(g_cv_ps);   // R237
    rel(g_fence11); rel(g_mvec11); rel(g_depth11); rel(g_color11);
    g_dev11 = nullptr;
}
}   // namespace mgpu::dx11producer
