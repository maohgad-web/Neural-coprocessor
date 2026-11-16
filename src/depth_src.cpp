// MGPU Bridge - R376 (0.5.1 discovery): the game's own depth as a source for the depth lane.
// See depth_src.hpp. Off unless DepthSource is 1, 2 or 3.
#include <windows.h>
#include <d3d12.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <vector>

#include "depth_src.hpp"
#include "diag.hpp"
#include "gpu1_context.hpp"   // stream_depth_from_game, depth_lane_t
#include "mgpu_ini_parser.hpp"

namespace mgpu
{
namespace depthsrc
{
namespace
{
std::atomic<int> g_mode{0};
std::atomic<bool> g_present{false};
std::atomic<bool> g_contract{false};        // R382: DepthContract=1 (N01950's key)
std::atomic<unsigned> g_assumed_state{0xC0u};   // DepthSourceState, mode 1 only

std::atomic<unsigned long long> g_attempts{0}, g_copies{0}, g_sealed{0};
std::atomic<unsigned long long> g_ref_fmt{0}, g_ref_size{0}, g_ref_ms{0}, g_ref_state{0}, g_ref_life{0}, g_not_live{0};
std::atomic<bool> g_said_fmt{false}, g_said_size{false}, g_said_ms{false}, g_said_state{false}, g_said_life{false}, g_said_first{false};

// One source resource remembered, so GetDevice / GetCopyableFootprints run when it changes, not per frame.
// Written and read on the game's thread that makes the call; two routes never run in the same mode.
ID3D12Resource *g_last_res = nullptr;
unsigned g_last_w = 0u, g_last_h = 0u, g_last_fmt = 0u;
DXGI_FORMAT g_last_copy_fmt = DXGI_FORMAT_UNKNOWN;

bool read_ini(int &mode, unsigned &state, bool &have, bool &contract)
{
    mode = 0; state = 0xC0u; have = false; contract = false;
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
    const char *v = mgpu::config::find(buf.data(), got, "DepthSource");
    if (v != nullptr) { have = true; mode = atoi(v); if (mode < 0 || mode > 4) mode = 0; }
    const char *dc = mgpu::config::find(buf.data(), got, "DepthContract");   // R382
    if (dc != nullptr && atoi(dc) == 1) { contract = true; have = true; }
    const char *sv = mgpu::config::find(buf.data(), got, "DepthSourceState");
    if (sv != nullptr) { const unsigned long x = strtoul(sv, nullptr, 16); if (x != 0ul && x <= 0xFFFFFul) state = (unsigned)x; }
    return true;
}

// Formats CopyTextureRegion may copy into the lane's footprint: the lane's own typeless family, one plane.
bool same_family(unsigned lane_fmt, unsigned src_fmt)
{
    switch ((DXGI_FORMAT)lane_fmt)
    {
    case DXGI_FORMAT_R32_FLOAT:
        return src_fmt == (unsigned)DXGI_FORMAT_R32_TYPELESS || src_fmt == (unsigned)DXGI_FORMAT_D32_FLOAT ||
               src_fmt == (unsigned)DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_UNORM:
        return src_fmt == (unsigned)DXGI_FORMAT_R16_TYPELESS || src_fmt == (unsigned)DXGI_FORMAT_D16_UNORM ||
               src_fmt == (unsigned)DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R16_FLOAT:
        return src_fmt == (unsigned)DXGI_FORMAT_R16_TYPELESS || src_fmt == (unsigned)DXGI_FORMAT_R16_FLOAT;
    default:
        return false;
    }
}

struct job
{
    ID3D12GraphicsCommandList *cl;
    ID3D12Resource *res;
    unsigned state;        // the state the resource is in on this list, declared (tag) or assumed (evaluate)
    unsigned box_w, box_h; // the size the caller knows the depth image has inside the resource (0 = not known)
    unsigned box_x, box_y;
    int route;
};

void say_once(std::atomic<bool> &flag, const char *text)
{
    if (!flag.exchange(true)) mgpu::diag::info(text);
}

// Runs under gpu1's guard, with the lane of the slot the next seal signs.
bool record(void *ctx, const mgpu::gpu1::depth_lane_t &ln)
{
    job &j = *static_cast<job *>(ctx);
    const D3D12_RESOURCE_DESC d = j.res->GetDesc();
    char l[420];
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.SampleDesc.Count != 1u)
    {
        g_ref_ms.fetch_add(1, std::memory_order_relaxed);
        say_once(g_said_ms, "[MGPU][R376] DepthSource: the game's depth is multisampled or not a 2D texture - not copied, "
                            "the tap carries depth for these frames. Said once; the count is on the periodic line.");
        return false;
    }
    if (!same_family(ln.format, (unsigned)d.Format))
    {
        g_ref_fmt.fetch_add(1, std::memory_order_relaxed);
        if (!g_said_fmt.exchange(true))
        {
            snprintf(l, sizeof l, "[MGPU][R376] DepthSource: the game's depth is format %u and the depth lane is format %u - "
                     "not one copyable family (a depth + stencil format, or another width), so it is not copied and the "
                     "tap carries depth for these frames. Said once.", (unsigned)d.Format, ln.format);
            mgpu::diag::info(l);
        }
        return false;
    }
    // Size: the same size as the lane, or a larger resource whose depth image the caller knows to be lane-sized.
    const bool exact = ((unsigned)d.Width == ln.width && d.Height == ln.height);
    const bool boxed = !exact && j.box_w == ln.width && j.box_h == ln.height &&
                       (unsigned long long)j.box_x + ln.width <= d.Width && j.box_y + ln.height <= d.Height;
    if (!exact && !boxed)
    {
        g_ref_size.fetch_add(1, std::memory_order_relaxed);
        if (!g_said_size.exchange(true))
        {
            snprintf(l, sizeof l, "[MGPU][R376] DepthSource: the game's depth is %ux%u (image %ux%u) and the depth lane is %ux%u - "
                     "not copied, the tap carries depth for these frames. DepthTransport=3 sizes the lane to the game's "
                     "render size. Said once.", (unsigned)d.Width, d.Height, j.box_w, j.box_h, ln.width, ln.height);
            mgpu::diag::info(l);
        }
        return false;
    }
    // The footprint format for THIS source is asked of the device, once per resource, not assumed.
    if (j.res != g_last_res || (unsigned)d.Width != g_last_w || d.Height != g_last_h || (unsigned)d.Format != g_last_fmt)
    {
        ID3D12Device *dev = nullptr;
        if (FAILED(j.res->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void **>(&dev))) || dev == nullptr) return false;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT f{};
        dev->GetCopyableFootprints(&d, 0, 1, 0, &f, nullptr, nullptr, nullptr);
        dev->Release();
        g_last_res = j.res; g_last_w = (unsigned)d.Width; g_last_h = d.Height; g_last_fmt = (unsigned)d.Format;
        g_last_copy_fmt = f.Footprint.Format;
    }
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = j.res;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    dst.pResource = ln.dst;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = ln.offset;
    dst.PlacedFootprint.Footprint.Format = (g_last_copy_fmt != DXGI_FORMAT_UNKNOWN) ? g_last_copy_fmt : (DXGI_FORMAT)ln.format;
    dst.PlacedFootprint.Footprint.Width = ln.width;
    dst.PlacedFootprint.Footprint.Height = ln.height;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = ln.row_pitch;
    D3D12_BOX box{};
    box.left = j.box_x; box.top = j.box_y; box.front = 0;
    box.right = j.box_x + ln.width; box.bottom = j.box_y + ln.height; box.back = 1;

    const bool barrier = (j.state != (unsigned)D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_RESOURCE_BARRIER b{};
    if (barrier)
    {
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = j.res;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = (D3D12_RESOURCE_STATES)j.state;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        j.cl->ResourceBarrier(1, &b);
    }
    j.cl->CopyTextureRegion(&dst, 0, 0, 0, &src, boxed ? &box : nullptr);
    if (barrier)
    {   // back to exactly the state it had
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = (D3D12_RESOURCE_STATES)j.state;
        j.cl->ResourceBarrier(1, &b);
    }
    if (!g_said_first.exchange(true))
    {
        snprintf(l, sizeof l, "[MGPU][R376] DepthSource=%d ACTIVE: first copy of the game's own depth, %s, resource 0x%llx %ux%u "
                 "format %u (footprint format %u) state 0x%X (%s) into slot %u of the depth lane %ux%u. Frames it lands on "
                 "are sealed with this depth; every other frame takes the tap.",
                 g_mode.load(std::memory_order_relaxed), j.route == 1 ? "at the game's DLSS evaluate" : (j.route == 3 ? "at the game's FSR upscale call" : "at the Streamline tag"),
                 (unsigned long long)(uintptr_t)j.res, (unsigned)d.Width, d.Height, (unsigned)d.Format,
                 (unsigned)dst.PlacedFootprint.Footprint.Format, j.state, j.route == 1 ? "ASSUMED, DepthSourceState" : "declared by the game",
                 ln.slot, ln.width, ln.height);
        mgpu::diag::info(l);
    }
    return true;
}

void attempt(job &j)
{
    const unsigned long long n = g_attempts.fetch_add(1, std::memory_order_relaxed) + 1ull;
    if (mgpu::gpu1::stream_depth_from_game(&record, &j)) g_copies.fetch_add(1, std::memory_order_relaxed);
    if (n % 1200ull == 0ull)
    {
        char l[520];
        snprintf(l, sizeof l, "[MGPU][R376] DepthSource=%d, totals: attempts %llu | copied %llu | frames sealed with the game's depth %llu | "
                 "refused: format %llu, size %llu, multisampled %llu, state not declared %llu, lifecycle %llu | "
                 "stream not live, lane busy or slot already filled %llu. A frame without a copy takes the tap.",
                 g_mode.load(std::memory_order_relaxed), n, g_copies.load(std::memory_order_relaxed), g_sealed.load(std::memory_order_relaxed),
                 g_ref_fmt.load(std::memory_order_relaxed), g_ref_size.load(std::memory_order_relaxed), g_ref_ms.load(std::memory_order_relaxed),
                 g_ref_state.load(std::memory_order_relaxed), g_ref_life.load(std::memory_order_relaxed),
                 n - g_copies.load(std::memory_order_relaxed) - g_ref_fmt.load(std::memory_order_relaxed) -
                     g_ref_size.load(std::memory_order_relaxed) - g_ref_ms.load(std::memory_order_relaxed) -
                     g_ref_state.load(std::memory_order_relaxed) - g_ref_life.load(std::memory_order_relaxed));
        mgpu::diag::info(l);
    }
}
}   // namespace

void init()
{
    int m = 0; unsigned st = 0xC0u; bool have = false, contract = false;
    (void)read_ini(m, st, have, contract);
    g_contract.store(contract, std::memory_order_release);
    g_present.store(have, std::memory_order_release);
    g_assumed_state.store(st, std::memory_order_release);
    g_mode.store(have ? m : 0, std::memory_order_release);
    if (!have) return;
    char l[520];
    snprintf(l, sizeof l, "[MGPU][R376] DepthSource=%d (0 the tap, 1 the depth the game hands DLSS - state ASSUMED 0x%X, DepthSourceState; "
             "2 the depth the game tags for Streamline, tags valid only now; 3 the same, every lifecycle - both need SLTags=1 or 2; "
             "4 the depth the game hands its FSR upscale call, declared state - needs FfxHook=1). "
             "Needs MVec=3 armed. A frame whose copy does not land takes the tap as before. FGLive=1 re-reads this key while running.",
             m, st);
    mgpu::diag::info(l);
    if (contract)
        mgpu::diag::info("[MGPU][R376] DepthContract=1 (N01950's key): the depth of whichever contract call arrives is copied - at a DLSS "
                         "evaluate as DepthSource=1 does, at an FSR upscale call as DepthSource=4 does.");
}

bool present() { return g_present.load(std::memory_order_relaxed); }
int mode() { return g_mode.load(std::memory_order_relaxed); }
bool wants_dlss() { return g_mode.load(std::memory_order_relaxed) == 1 || g_contract.load(std::memory_order_relaxed); }
bool wants_fsr() { return g_mode.load(std::memory_order_relaxed) == 4 || g_contract.load(std::memory_order_relaxed); }

void read_live()
{
    if (!g_present.load(std::memory_order_relaxed)) return;
    int m = 0; unsigned st = 0xC0u; bool have = false, contract = false;
    if (!read_ini(m, st, have, contract) || !have) return;
    g_contract.store(contract, std::memory_order_release);
    g_assumed_state.store(st, std::memory_order_release);
    const int was = g_mode.exchange(m, std::memory_order_acq_rel);
    if (was != m)
    {
        char l[160];
        snprintf(l, sizeof l, "[MGPU][R376] live: DepthSource %d -> %d. Takes effect on the next frame.", was, m);
        mgpu::diag::info(l);
    }
}

void on_dlss_evaluate(void *cmd_list, unsigned long long depth, unsigned render_w, unsigned render_h,
                      unsigned depth_x, unsigned depth_y)
{
    if (!wants_dlss() || cmd_list == nullptr || depth == 0ull) return;
    job j{};
    j.cl = reinterpret_cast<ID3D12GraphicsCommandList *>(cmd_list);
    j.res = reinterpret_cast<ID3D12Resource *>((void *)(uintptr_t)depth);
    j.state = g_assumed_state.load(std::memory_order_relaxed);
    j.box_w = render_w; j.box_h = render_h; j.box_x = depth_x; j.box_y = depth_y;
    j.route = 1;
    attempt(j);
}

void on_sl_tag(void *cmd_buffer, unsigned long long resource, unsigned declared_state, int lifecycle)
{
    const int m = g_mode.load(std::memory_order_relaxed);
    if (m < 2 || cmd_buffer == nullptr || resource == 0ull) return;
    if (declared_state == 0xFFFFFFFFu)
    {
        g_attempts.fetch_add(1, std::memory_order_relaxed);
        g_ref_state.fetch_add(1, std::memory_order_relaxed);
        say_once(g_said_state, "[MGPU][R376] DepthSource: the game tagged its depth with sl::Resource.state unset, so there is no "
                               "state to transition from or restore - not copied, the tap carries depth. Said once.");
        return;
    }
    if (m == 2 && lifecycle != 0)
    {
        g_attempts.fetch_add(1, std::memory_order_relaxed);
        g_ref_life.fetch_add(1, std::memory_order_relaxed);
        if (!g_said_life.exchange(true))
        {
            char l[300];
            snprintf(l, sizeof l, "[MGPU][R376] DepthSource=2 takes depth tags that are valid only now and this one is lifecycle=%d - "
                     "not copied, the tap carries depth. DepthSource=3 takes every lifecycle. Said once.", lifecycle);
            mgpu::diag::info(l);
        }
        return;
    }
    job j{};
    j.cl = reinterpret_cast<ID3D12GraphicsCommandList *>(cmd_buffer);
    j.res = reinterpret_cast<ID3D12Resource *>((void *)(uintptr_t)resource);
    j.state = declared_state;
    j.box_w = j.box_h = j.box_x = j.box_y = 0u;
    j.route = 2;
    attempt(j);
}

void on_declared(void *cmd_list, unsigned long long resource, unsigned state, unsigned render_w, unsigned render_h)
{
    if (!wants_fsr() || cmd_list == nullptr || resource == 0ull) return;
    job j{};
    j.cl = reinterpret_cast<ID3D12GraphicsCommandList *>(cmd_list);
    j.res = reinterpret_cast<ID3D12Resource *>((void *)(uintptr_t)resource);
    j.state = state;
    j.box_w = render_w; j.box_h = render_h; j.box_x = j.box_y = 0u;
    j.route = 3;
    attempt(j);
}

void note_sealed() { g_sealed.fetch_add(1, std::memory_order_relaxed); }
}   // namespace depthsrc
}   // namespace mgpu
