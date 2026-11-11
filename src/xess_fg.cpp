// MGPU Bridge - XFG: XeSS Frame Generation on the second card (experimental,
// off by default). See xess_fg.hpp for what it is for and the contract it keeps.
// Everything here runs on the bridge thread.
//
// THIS BUILD IS THE FIRST GATE. arm() loads the Intel libraries, creates the
// XeFG context against GPU 1's device, queries the properties and builds the
// pipelines. It does NOT hand the swapchain over and it does not present through
// the proxy: the bridge's own present path is untouched, so the on-screen result
// of a build with XeFG=1 is identical to a build without it. What this gate
// answers is one question - does XeSS-FG accept this device - and the numbers
// the next step needs (descriptor count, the two heap sizes, the maximum
// interpolation count) are printed while it answers it.

#include "xess_fg.hpp"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <vector>

#include "diag.hpp"
#include "mgpu_ini_parser.hpp"

// IDXGISwapChain itself. The Intel d3d12 header brings in d3d12.h (and with it
// dxgitype.h's DXGI_FORMAT), but the swapchain INTERFACES come from dxgi.h,
// which the ext copy of the Intel header does not include - phase 2's handover
// functions name IDXGISwapChain directly, so this file includes it itself.
#include <dxgi1_5.h>

// The Intel headers are included for their TYPES only. Every function is reached
// through GetProcAddress, so nothing from them is linked and no import table
// entry names a file the user may not have installed.
#include "../ext/xess_fg/xefg_swapchain.h"
#include "../ext/xess_fg/xefg_swapchain_d3d12.h"
#include "../ext/xess_fg/xell.h"
#include "../ext/xess_fg/xell_d3d12.h"

namespace mgpu::xess_fg
{
namespace
{
std::atomic<bool> g_active{false};   // the gate is running or has passed
std::atomic<bool> g_passed{false};   // the gate completed successfully

HMODULE g_fg_lib  = nullptr;         // libxess_fg.dll
HMODULE g_xell_lib = nullptr;        // libxell.dll

xefg_swapchain_handle_t g_ctx   = nullptr;
xell_context_handle_t  g_xell  = nullptr;   // the XeLL context the proxy requires

// What the gate learned.
unsigned          g_frames_req   = 1;        // XeFGFrames, 1..7
unsigned          g_inverted     = 0;        // XeFGInverted
unsigned long long g_max_interp  = 0;
unsigned long long g_desc_count  = 0;
unsigned long long g_buf_heap    = 0;
unsigned long long g_tex_heap    = 0;
unsigned long long g_const_size  = 0;
unsigned          g_w = 0, g_h = 0;

// ---- the functions, by pointer ----
typedef xefg_swapchain_result_t (*pfn_efg_get_version)(xefg_swapchain_version_t*);
typedef xefg_swapchain_result_t (*pfn_efg_create_ctx)(ID3D12Device*, xefg_swapchain_handle_t*);
typedef xefg_swapchain_result_t (*pfn_efg_props)(xefg_swapchain_handle_t,
                                                 const xefg_swapchain_d3d12_init_params_t*,
                                                 uint32_t, uint32_t, DXGI_FORMAT,
                                                 xefg_swapchain_properties_t*);
typedef xefg_swapchain_result_t (*pfn_efg_build)(xefg_swapchain_handle_t, ID3D12PipelineLibrary*,
                                                 uint8_t, uint32_t);
typedef xefg_swapchain_result_t (*pfn_efg_pipe_status)(xefg_swapchain_handle_t);
typedef xefg_swapchain_result_t (*pfn_efg_destroy)(xefg_swapchain_handle_t);
typedef xefg_swapchain_result_t (*pfn_efg_get_init)(xefg_swapchain_handle_t,
                                                    xefg_swapchain_d3d12_init_params_t*);
typedef xell_result_t (*pfn_xell_get_version)(xell_version_t*);
typedef xell_result_t (*pfn_xell_create_ctx)(ID3D12Device*, xell_context_handle_t*);
typedef xell_result_t (*pfn_xell_destroy_ctx)(xell_context_handle_t);
typedef xell_result_t (*pfn_xell_sleep_mode)(xell_context_handle_t, const xell_sleep_params_t*);
typedef xell_result_t (*pfn_xell_marker)(xell_context_handle_t, uint32_t, xell_latency_marker_type_t);

// ---- phase 2: the handover entries, same by-pointer rule ----
typedef xefg_swapchain_result_t (*pfn_efg_init_sc)(xefg_swapchain_handle_t, ID3D12CommandQueue*,
                                                   const xefg_swapchain_d3d12_init_params_t*);
typedef xefg_swapchain_result_t (*pfn_efg_get_sc_ptr)(xefg_swapchain_handle_t, REFIID, void**);
typedef xefg_swapchain_result_t (*pfn_efg_tag_res)(xefg_swapchain_handle_t, ID3D12CommandList*,
                                                   uint32_t, const xefg_swapchain_d3d12_resource_data_t*);
typedef xefg_swapchain_result_t (*pfn_efg_tag_const)(xefg_swapchain_handle_t, uint32_t,
                                                     const xefg_swapchain_frame_constant_data_t*);
typedef xefg_swapchain_result_t (*pfn_efg_set_pid)(xefg_swapchain_handle_t, uint32_t);
typedef xefg_swapchain_result_t (*pfn_efg_last_status)(xefg_swapchain_handle_t,
                                                       xefg_swapchain_present_status_t*);
typedef xefg_swapchain_result_t (*pfn_efg_set_enabled)(xefg_swapchain_handle_t, uint32_t);
// The XeLL link. Intel: "XeSS-FG proxy swap chain will fail to initialize
// without XeLL context" - and it does, with LATENCY_REDUCTION_UNSUPPORTED (-15).
typedef xefg_swapchain_result_t (*pfn_efg_set_latency)(xefg_swapchain_handle_t, void*);
// Intel's own log stream. The library says WHY it skipped an interpolation
// here, and until 2026-10-29 nobody was listening: the 2x=0 runs failed
// silently because the rejection reason went to an unregistered callback.
typedef xefg_swapchain_result_t (*pfn_efg_set_log)(xefg_swapchain_handle_t,
                                                   xefg_swapchain_logging_level_t,
                                                   xefg_swapchain_app_log_callback_t, void*);

pfn_efg_init_sc      efg_init_sc      = nullptr;
pfn_efg_get_sc_ptr   efg_get_sc_ptr   = nullptr;
pfn_efg_tag_res      efg_tag_res      = nullptr;
pfn_efg_tag_const    efg_tag_const    = nullptr;
pfn_efg_set_pid      efg_set_pid      = nullptr;
pfn_efg_last_status  efg_last_status  = nullptr;
pfn_efg_set_enabled  efg_set_enabled  = nullptr;
pfn_efg_set_latency  efg_set_latency  = nullptr;
pfn_efg_set_log      efg_set_log      = nullptr;

// The Intel log callback. WARNING and above only (set at init below). Hard
// budget (discipline #2): the first 40 lines of the run reach ReShade.log,
// then silence - a rejection that repeats every frame is already proven by
// the first 40, and an unbounded Intel stream inside our log would bury our
// own lines. Message pointer is only valid inside the call (SDK), so copy.
static std::atomic<unsigned> g_xelog_n{0};
void efg_log_trampoline(const char* message,
                        xefg_swapchain_logging_level_t level,
                        void* /*userData*/)
{
    if (message == nullptr) return;
    const unsigned n = g_xelog_n.fetch_add(1, std::memory_order_relaxed);
    if (n >= 40u) return;
    char l[512];
    snprintf(l, sizeof l, "[MGPU][XFG][LIB] %s%s",
             (level == XEFG_SWAPCHAIN_LOGGING_LEVEL_ERROR) ? "E " :
             (level == XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING) ? "W " : "",
             message);
    if (n == 39u) mgpu::diag::info("[MGPU][XFG][LIB] (last line of the 40-line budget; further Intel logs dropped)");
    mgpu::diag::info(l);
}

// ---- handover state (bridge thread arms it, present thread reads it) ----
std::atomic<bool>        g_ho_key{false};     // XeFGHandover=1 was read at arm
std::atomic<bool>        g_ho_live{false};    // the proxy chain is in service
// The tag's frame id: Intel requires ids consecutive across presents
// (a gap answers WARNING_FRAMES_ID_MISMATCH and the frame goes 1x). The
// seal number is NOT that sequence - with the game producing faster than
// GPU 1 consumes, the consumer skips frames and the seal ids jump (measured
// 2026-10-29: id_gap 8.6% even at FrameCap=60, 2x stuck at ~75%). So the
// interface carries its own counter, +1 per tagged frame; the seal number
// stays the single source for logs and history keys (discipline #1).
unsigned                 g_ho_seq = 0;        // present thread only
uint32_t                 g_ho_last_tag_id = 0; // the id the last successful tag spent
bool                     g_ho_ctx_pending = false;  // teardown deferred the destroy
bool                     g_ho_lost = false;         // the app chain was consumed, none came back
IDXGISwapChain          *g_ho_chain = nullptr;
unsigned long long       g_ho_tags = 0, g_ho_tag_fails = 0;
unsigned                 g_ho_2x = 0, g_ho_1x = 0;
uint32_t                 g_ho_last_id = 0;      // the id the XeLL markers bracket

pfn_efg_get_version   efg_get_version   = nullptr;
pfn_efg_create_ctx    efg_create_ctx    = nullptr;
pfn_efg_props         efg_props         = nullptr;
pfn_efg_build         efg_build         = nullptr;
pfn_efg_pipe_status   efg_pipe_status   = nullptr;
pfn_efg_destroy       efg_destroy       = nullptr;
pfn_efg_get_init      efg_get_init      = nullptr;
pfn_xell_get_version  xell_get_version  = nullptr;
pfn_xell_create_ctx   xell_create_ctx   = nullptr;
pfn_xell_destroy_ctx  xell_destroy_ctx  = nullptr;
pfn_xell_sleep_mode   xell_sleep_mode   = nullptr;
pfn_xell_marker       xell_marker       = nullptr;

// ---- keys, from mgpu.ini beside the module ----
bool ini_path(wchar_t *path, size_t n)
{
    static const int anchor = 0;
    HMODULE hm = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&anchor), &hm))
        return false;
    wchar_t mod[MAX_PATH];
    const DWORD len = GetModuleFileNameW(hm, mod, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return false;
    wchar_t *slash = wcsrchr(mod, L'\\');
    if (slash == nullptr) return false;
    slash[1] = L'\0';
    _snwprintf_s(path, n, _TRUNCATE, L"%lsmgpu.ini", mod);
    return true;
}

// on: XeFG was 1. frames: XeFGFrames clamped to 1..7 (default 1).
// inverted: XeFGInverted was 1. raw: the XeFG key as written, -1 when absent.
// ho: XeFGHandover was 1 - phase 2, wrap the present chain and emit 2x.
void read_keys(bool &on, unsigned &frames, unsigned &inverted, int &raw, bool &ho)
{
    on = false; frames = 1; inverted = 0; raw = -1; ho = false;
    wchar_t path[MAX_PATH];
    if (!ini_path(path, MAX_PATH)) return;
    FILE *f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || f == nullptr) return;
    std::vector<char> buf(mgpu::config::MAX_BYTES + 2u, '\0');
    const size_t got = fread(buf.data(), 1, mgpu::config::MAX_BYTES + 1u, f);
    fclose(f);
    if (got > mgpu::config::MAX_BYTES) return;   // whole-document rule
    buf[got] = '\0';
    const char *v = mgpu::config::find(buf.data(), got, "XeFG");
    if (v == nullptr) return;
    raw = atoi(v);
    if (raw != 1) return;
    on = true;
    const char *fr = mgpu::config::find(buf.data(), got, "XeFGFrames");
    if (fr != nullptr)
    {
        const int n = atoi(fr);
        if (n >= 1 && n <= 7) frames = (unsigned)n;
    }
    const char *iv = mgpu::config::find(buf.data(), got, "XeFGInverted");
    if (iv != nullptr && atoi(iv) == 1) inverted = 1;
    const char *hv = mgpu::config::find(buf.data(), got, "XeFGHandover");
    if (hv != nullptr && atoi(hv) == 1) ho = true;
}

// The libraries live beside nvngx_dlssnr.dll, in <add-on directory>\mgpu. The
// add-on's own directory is known from GetModuleFileNameW; SetDllDirectory is
// deliberately NOT used - it would change resolution for the whole process,
// including the game's own LoadLibrary calls. LoadLibraryExW with a full path
// touches nothing but these two files.
bool lib_path(wchar_t *path, size_t n, const wchar_t *name)
{
    static const int anchor2 = 0;
    HMODULE hm = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&anchor2), &hm))
        return false;
    wchar_t mod[MAX_PATH];
    const DWORD len = GetModuleFileNameW(hm, mod, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return false;
    wchar_t *slash = wcsrchr(mod, L'\\');
    if (slash == nullptr) return false;
    slash[1] = L'\0';
    _snwprintf_s(path, n, _TRUNCATE, L"%lsmgpu\\%s", mod, name);
    return true;
}

HMODULE load_one(const wchar_t *name)
{
    wchar_t full[MAX_PATH];
    if (!lib_path(full, MAX_PATH, name)) return nullptr;
    if (GetFileAttributesW(full) == INVALID_FILE_ATTRIBUTES) return nullptr;
    // LOAD_WITH_ALTERED_SEARCH_PATH: the Intel dlls may pull their own
    // dependencies from the mgpu folder; the flag scopes that to this load.
    return LoadLibraryExW(full, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
}

void teardown()
{
    g_active.store(false, std::memory_order_relaxed);
    g_passed.store(false, std::memory_order_relaxed);
    // Intel: xefgSwapChainDestroy needs every chain reference dropped first
    // (POINTER_STILL_IN_USE otherwise). While the handover is live the present
    // chain IS a reference, and it outlives stream_release by design - so the
    // destroy waits for handover_disarm, which runs after the chain is gone.
    if (g_ctx != nullptr && efg_destroy != nullptr && !g_ho_live.load(std::memory_order_relaxed))
    {
        (void)efg_destroy(g_ctx);
        g_ctx = nullptr;
    }
    else if (g_ctx != nullptr)
    {
        g_ho_ctx_pending = true;
        return;   // the libraries must stay loaded: the context lives inside them
    }
    // Intel: the XeLL context must be destroyed AFTER the XeSS-FG context. It
    // goes here, between the FG destroy above and the unload below.
    if (g_xell != nullptr && xell_destroy_ctx != nullptr) (void)xell_destroy_ctx(g_xell);
    g_xell = nullptr;
    if (g_fg_lib != nullptr) { FreeLibrary(g_fg_lib); g_fg_lib = nullptr; }
    if (g_xell_lib != nullptr) { FreeLibrary(g_xell_lib); g_xell_lib = nullptr; }
    efg_get_version = nullptr; efg_create_ctx = nullptr; efg_props = nullptr;
    efg_build = nullptr; efg_pipe_status = nullptr; efg_destroy = nullptr;
    efg_get_init = nullptr; xell_get_version = nullptr; xell_create_ctx = nullptr;
    efg_init_sc = nullptr; efg_get_sc_ptr = nullptr; efg_tag_res = nullptr;
    efg_tag_const = nullptr; efg_set_pid = nullptr; efg_last_status = nullptr;
    efg_set_enabled = nullptr;
}

void say(const char *fmt, ...)
{
    char line[600];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof line, _TRUNCATE, fmt, ap);
    va_end(ap);
    mgpu::diag::info(line);
}
}  // namespace

bool arm(ID3D12Device *dev, unsigned width, unsigned height)
{
    char l[600];
    bool on = false; unsigned frames = 1, inverted = 0; int raw = -1; bool ho = false;
    read_keys(on, frames, inverted, raw, ho);
    g_ho_key.store(ho, std::memory_order_relaxed);
    if (!on)
    {
        if (raw != -1)
        {
            snprintf(l, sizeof l, "[MGPU][XFG] XeSS frame generation OFF (XeFG=%d; 1 turns it "
                     "on). The stream presents one frame per game frame, as before.", raw);
            mgpu::diag::info(l);
        }
        return false;
    }
    if (dev == nullptr) return false;

    g_frames_req = frames;
    g_inverted   = inverted;
    g_w = width; g_h = height;
    g_active.store(true, std::memory_order_relaxed);

    // PHASE 2: A CONTEXT CAN OUTLIVE A STREAM. stream_release defers the
    // destroy while the proxy chain is alive, so a reload or a re-arm would
    // otherwise create a SECOND context and orphan the first - and the first
    // one's chain could then never be destroyed. The gate's answer has not
    // changed, and the libraries are still loaded: reuse it.
    if (g_ctx != nullptr)
    {
        g_passed.store(true, std::memory_order_relaxed);
        say("[MGPU][XFG] the gate's context is still alive from the previous arm (its destroy is "
            "waiting for the proxy chain) - reusing it and not running the gate again.");
        return true;
    }

    say("[MGPU][XFG] GATE 1/5 keys: XeFG=1, XeFGFrames=%u (=%ux output), XeFGInverted=%u, "
        "chain %ux%u. Loading the Intel libraries from <add-on>\\mgpu.",
        frames, frames + 1u, inverted, width, height);

    // ---- 1. load the two libraries ----
    g_fg_lib = load_one(L"libxess_fg.dll");
    if (g_fg_lib == nullptr)
    {
        char e[40]; snprintf(e, sizeof e, "0x%08lX", GetLastError());
        snprintf(l, sizeof l, "[MGPU][XFG] OFF for this arm: mgpu\\libxess_fg.dll is missing or "
                 "would not load (GetLastError %s). Get it from the XeSS SDK (3.0.2 or later) and "
                 "put it beside nvngx_dlssnr.dll. The stream runs without frame generation.", e);
        mgpu::diag::error(l);
        teardown();
        return false;
    }
    // XeLL is optional at this gate: it is required for the real path (Intel
    // documents FG + latency reduction together), but the context creation and
    // the property query do not need it. Load it if present, say which happened.
    g_xell_lib = load_one(L"libxell.dll");

    efg_get_version = (pfn_efg_get_version) GetProcAddress(g_fg_lib, "xefgSwapChainGetVersion");
    efg_create_ctx  = (pfn_efg_create_ctx)  GetProcAddress(g_fg_lib, "xefgSwapChainD3D12CreateContext");
    efg_props       = (pfn_efg_props)       GetProcAddress(g_fg_lib, "xefgSwapChainD3D12GetProperties");
    efg_build       = (pfn_efg_build)       GetProcAddress(g_fg_lib, "xefgSwapChainD3D12BuildPipelines");
    efg_pipe_status = (pfn_efg_pipe_status) GetProcAddress(g_fg_lib, "xefgSwapChainGetPipelineBuildStatus");
    efg_destroy     = (pfn_efg_destroy)     GetProcAddress(g_fg_lib, "xefgSwapChainDestroy");
    efg_get_init    = (pfn_efg_get_init)    GetProcAddress(g_fg_lib, "xefgSwapChainD3D12GetInitializationParameters");
    // Phase 2 handover entries. Resolved here but only REQUIRED when the key is
    // on: a build without XeFGHandover still runs the gate on a library that
    // happens to lack them, exactly as before.
    efg_init_sc     = (pfn_efg_init_sc)     GetProcAddress(g_fg_lib, "xefgSwapChainD3D12InitFromSwapChain");
    efg_get_sc_ptr  = (pfn_efg_get_sc_ptr)  GetProcAddress(g_fg_lib, "xefgSwapChainD3D12GetSwapChainPtr");
    efg_tag_res     = (pfn_efg_tag_res)     GetProcAddress(g_fg_lib, "xefgSwapChainD3D12TagFrameResource");
    efg_tag_const   = (pfn_efg_tag_const)   GetProcAddress(g_fg_lib, "xefgSwapChainTagFrameConstants");
    efg_set_pid     = (pfn_efg_set_pid)     GetProcAddress(g_fg_lib, "xefgSwapChainSetPresentId");
    efg_last_status = (pfn_efg_last_status) GetProcAddress(g_fg_lib, "xefgSwapChainGetLastPresentStatus");
    efg_set_enabled = (pfn_efg_set_enabled) GetProcAddress(g_fg_lib, "xefgSwapChainSetEnabled");
    efg_set_latency = (pfn_efg_set_latency) GetProcAddress(g_fg_lib, "xefgSwapChainSetLatencyReduction");
    efg_set_log     = (pfn_efg_set_log)     GetProcAddress(g_fg_lib, "xefgSwapChainSetLoggingCallback");
    if (g_xell_lib != nullptr)
    {
        xell_get_version = (pfn_xell_get_version) GetProcAddress(g_xell_lib, "xellGetVersion");
        xell_create_ctx  = (pfn_xell_create_ctx)  GetProcAddress(g_xell_lib, "xellD3D12CreateContext");
        xell_destroy_ctx = (pfn_xell_destroy_ctx) GetProcAddress(g_xell_lib, "xellDestroyContext");
        xell_sleep_mode  = (pfn_xell_sleep_mode)  GetProcAddress(g_xell_lib, "xellSetSleepMode");
        xell_marker      = (pfn_xell_marker)      GetProcAddress(g_xell_lib, "xellAddMarkerData");
    }
    if (efg_get_version == nullptr || efg_create_ctx == nullptr || efg_props == nullptr ||
        efg_build == nullptr || efg_pipe_status == nullptr || efg_destroy == nullptr)
    {
        mgpu::diag::error("[MGPU][XFG] OFF for this arm: libxess_fg.dll is present but does not "
                          "export the swapchain API this build expects - the SDK version is wrong. "
                          "XeSS SDK 3.0.2 or later is required.");
        teardown();
        return false;
    }

    xefg_swapchain_version_t fv = {};
    (void) efg_get_version(&fv);
    char xl[80]; xl[0] = '\0';
    if (xell_get_version != nullptr)
    {
        xell_version_t xv = {};
        if (xell_get_version(&xv) == XELL_RESULT_SUCCESS)
            snprintf(xl, sizeof xl, ", XeLL %u.%u.%u", xv.major, xv.minor, xv.patch);
    }
    say("[MGPU][XFG] GATE 2/5 libraries loaded: XeSS-FG swapchain %u.%u.%u%s.",
        fv.major, fv.minor, fv.patch, xl);

    // ---- 2. the context, on GPU 1's device ----
    const xefg_swapchain_result_t rc = efg_create_ctx(dev, &g_ctx);
    if (rc != XEFG_SWAPCHAIN_RESULT_SUCCESS || g_ctx == nullptr)
    {
        snprintf(l, sizeof l, "[MGPU][XFG] OFF for this arm: xefgSwapChainD3D12CreateContext on the "
                 "second card returned %d (see the Intel result enum). GPU 1 rejected the device or "
                 "the driver. The stream runs without frame generation.", (int)rc);
        mgpu::diag::error(l);
        teardown();
        return false;
    }
    say("[MGPU][XFG] GATE 3/5 context created on GPU 1's device - XeSS-FG accepts this adapter.");
    // Listen to the library from the moment the context exists. WARNING and
    // above: the 2x=0 investigation needs its rejection reasons, and INFO
    // would flood the 40-line budget with pacing chatter.
    if (efg_set_log != nullptr)
    {
        const xefg_swapchain_result_t lgr =
            efg_set_log(g_ctx, XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING, efg_log_trampoline, nullptr);
        say("[MGPU][XFG] logging callback registered (result=%d).", (int)lgr);
    }
    else
        mgpu::diag::warn("[MGPU][XFG] libxess_fg.dll does not export "
                         "xefgSwapChainSetLoggingCallback - the library's own "
                         "rejection reasons will stay unheard.");

    // ---- 2b. XeLL, WHICH IS NOT OPTIONAL ----
    //
    // Intel's guide says it twice: "XeSS-FG proxy swap chain will fail to
    // initialize without XeLL context" and "Frame generation will be disabled
    // if XeLL is not initialized and enabled". The gate never needed it, so
    // this build shipped without it - and the first real wrap returned -15,
    // LATENCY_REDUCTION_UNSUPPORTED, on 2026-10-28. Create the context on GPU
    // 1's device, connect it to the FG context, and switch latency reduction
    // on: that is the state the proxy checks before it initializes.
    if (xell_create_ctx != nullptr)
    {
        const xell_result_t xr = xell_create_ctx(dev, &g_xell);
        say("[MGPU][XFG] GATE 3b/5 XeLL: xellD3D12CreateContext returned %d (0 = SUCCESS).", (int)xr);
        if (xr == XELL_RESULT_SUCCESS && g_xell != nullptr)
        {
            if (efg_set_latency != nullptr)
            {
                const xefg_swapchain_result_t lr = efg_set_latency(g_ctx, g_xell);
                say("[MGPU][XFG] GATE 3b/5 xefgSwapChainSetLatencyReduction returned %d (0 = "
                    "SUCCESS). This is the link whose absence made InitFromSwapChain answer -15.",
                    (int)lr);
            }
            else
            {
                mgpu::diag::warn("[MGPU][XFG] libxess_fg.dll does not export "
                                 "xefgSwapChainSetLatencyReduction, so the proxy cannot be "
                                 "initialized on this SDK: the handover would decline with -15.");
            }
            if (xell_sleep_mode != nullptr)
            {
                xell_sleep_params_t sp = {};
                sp.bLowLatencyMode = 1;
                const xell_result_t sr = xell_sleep_mode(g_xell, &sp);
                say("[MGPU][XFG] GATE 3b/5 xellSetSleepMode(lowLatency=1) returned %d "
                    "(0 = SUCCESS).", (int)sr);
            }
        }
        else
        {
            mgpu::diag::error("[MGPU][XFG] XELL CONTEXT FAILED. The XeSS-FG proxy swap chain cannot "
                              "initialize without one (Intel's requirement, and the -15 this rig "
                              "produced on 2026-10-28), so frame generation is unavailable on this "
                              "device. Everything else - NR, transport, present - is unaffected.");
        }
    }

    // ---- 3. the properties, before and after the pipeline build ----
    xefg_swapchain_d3d12_init_params_t ip = {};
    ip.pApplicationSwapChain  = nullptr;                 // the gate does not hand over the chain
    ip.initFlags              = (inverted != 0u) ? (uint32_t)XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH
                                                 : (uint32_t)XEFG_SWAPCHAIN_INIT_FLAG_NONE;
    ip.maxInterpolatedFrames  = frames;
    ip.creationNodeMask       = 1;
    ip.visibleNodeMask        = 1;
    ip.pTempBufferHeap        = nullptr;                 // internal allocation at this gate
    ip.pTempTextureHeap       = nullptr;
    ip.pPipelineLibrary       = nullptr;
    ip.uiMode                 = XEFG_SWAPCHAIN_UI_MODE_NONE;

    xefg_swapchain_properties_t pr = {};
    const xefg_swapchain_result_t rp = efg_props(g_ctx, &ip, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, &pr);
    if (rp == XEFG_SWAPCHAIN_RESULT_SUCCESS)
    {
        g_max_interp = pr.maxSupportedInterpolations;
        g_desc_count = pr.requiredDescriptorCount;
        g_buf_heap   = pr.tempBufferHeapSize;
        g_tex_heap   = pr.tempTextureHeapSize;
        g_const_size = pr.constantBufferSize;
    }
    say("[MGPU][XFG] GATE 4/5 properties: result=%d, maxInterpolations=%llu, descriptors=%llu, "
        "tempBufferHeap=%llu B, tempTextureHeap=%llu B, constantBuffer=%llu B "
        "(asked for %u interpolated frame(s) = %ux).",
        (int)rp, g_max_interp, g_desc_count, g_buf_heap, g_tex_heap, g_const_size, frames, frames + 1u);
    if (rp == XEFG_SWAPCHAIN_RESULT_SUCCESS && g_max_interp != 0 && frames > g_max_interp)
        say("[MGPU][XFG] NOTE: this device supports at most %llu interpolated frames per game frame; "
            "XeFGFrames=%u is above it and would be clamped when the path goes live.",
            g_max_interp, frames);

    // ---- 4. build the pipelines, NON-blocking, then poll the status ----
    //
    // Intel's contract: xefgSwapChainGetPipelineBuildStatus may only be called
    // between BuildPipelines and init, and calling it after a BLOCKING build
    // returns WRONG_CALL_ORDER (-21) - gate run 1 printed exactly that. So build
    // non-blocking and poll: SUCCESS means the kernels are compiled, -9
    // (OPERATION_IN_PROGRESS) means keep waiting, anything else is a failure.
    // Polling at arm is cheap: arm already owns multi-millisecond stalls per
    // CreateFeature, and the whole gate is one-shot per launch.
    const uint32_t build_flags = ip.initFlags;
    const xefg_swapchain_result_t rb = efg_build(g_ctx, nullptr, 0, build_flags);
    if (rb != XEFG_SWAPCHAIN_RESULT_SUCCESS)
    {
        snprintf(l, sizeof l, "[MGPU][XFG] OFF for this arm: xefgSwapChainD3D12BuildPipelines "
                 "returned %d. The kernels would not compile on GPU 1. The stream runs without "
                 "frame generation.", (int)rb);
        mgpu::diag::error(l);
        teardown();
        return false;
    }
    xefg_swapchain_result_t pb = efg_pipe_status(g_ctx);
    for (unsigned polls = 0; polls < 60u && pb == XEFG_SWAPCHAIN_RESULT_ERROR_OPERATION_IN_PROGRESS; ++polls)
    {
        Sleep(250);
        pb = efg_pipe_status(g_ctx);
    }
    g_passed.store(pb == XEFG_SWAPCHAIN_RESULT_SUCCESS, std::memory_order_relaxed);
    say("[MGPU][XFG] GATE 5/5 pipelines: build returned %d, polled status=%d (%s). %s",
        (int)rb, (int)pb,
        (pb == XEFG_SWAPCHAIN_RESULT_SUCCESS) ? "SUCCESS"
            : (pb == XEFG_SWAPCHAIN_RESULT_ERROR_OPERATION_IN_PROGRESS) ? "still building after 15 s"
            : "neither SUCCESS nor in-progress",
        (pb == XEFG_SWAPCHAIN_RESULT_SUCCESS)
            ? "GATE PASSED - XeSS-FG is usable on the second card; the swapchain handover is the next step."
            : "reported, not fatal - the stream runs without frame generation until the next step lands.");

    // The context and the libraries stay loaded: the next step needs them and
    // re-running the gate at every arm would pay the pipeline build again.
    // stream_release() tears this down.
    return g_passed.load(std::memory_order_relaxed);
}

bool active() { return g_active.load(std::memory_order_relaxed); }
bool passed() { return g_passed.load(std::memory_order_relaxed); }

void status(bool &on, unsigned &frames, unsigned &inverted,
            unsigned long long &max_interp, unsigned long long &desc_count,
            unsigned long long &buf_heap, unsigned long long &tex_heap)
{
    on = g_active.load(std::memory_order_relaxed);
    frames = g_frames_req; inverted = g_inverted;
    max_interp = g_max_interp; desc_count = g_desc_count;
    buf_heap = g_buf_heap; tex_heap = g_tex_heap;
}

void release()
{
    if (g_fg_lib == nullptr && g_ctx == nullptr) return;   // one relaxed load when off
    // XFG phase 2: DO NOT disarm here. stream_release runs while the present
    // chain - and therefore the proxy chain inside it - is still alive, so
    // dropping the proxy reference and destroying the context now would hand
    // Intel a POINTER_STILL_IN_USE. teardown() sees the live handover, defers
    // the destroy and keeps the two libraries loaded; shutdown() calls
    // handover_disarm() after it has released the chain, and that finishes
    // the teardown.
    teardown();
}

// ================= phase 2: the swapchain handover =================

bool handover_enabled()
{
    // g_xell is part of the condition, not a detail: Intel states that the
    // proxy swap chain cannot initialize without a XeLL context, and -15 on
    // 2026-10-28 was exactly that. Without one there is nothing to try, and
    // attempting it anyway is how a failed init turns into an Intel-side crash
    // on a thread we do not own.
    return g_ho_key.load(std::memory_order_relaxed) &&
           g_passed.load(std::memory_order_relaxed) &&
           g_xell != nullptr &&
           efg_init_sc != nullptr && efg_get_sc_ptr != nullptr &&
           efg_tag_res != nullptr && efg_tag_const != nullptr &&
           efg_set_pid != nullptr && g_ctx != nullptr;
}

bool handover_live() { return g_ho_live.load(std::memory_order_relaxed); }

bool handover_lost() { return g_ho_lost; }

bool handover_arm(ID3D12CommandQueue *present_queue, IDXGISwapChain *app_chain,
                  unsigned width, unsigned height, DXGI_FORMAT backbuffer_format,
                  IDXGISwapChain3 **out_chain3, bool enable_generation)
{
    char l[400];
    if (out_chain3 != nullptr) *out_chain3 = nullptr;
    if (!handover_enabled() || present_queue == nullptr || app_chain == nullptr || out_chain3 == nullptr)
        return false;   // the caller's app_chain reference is NOT consumed here
    xefg_swapchain_d3d12_init_params_t ip = {};
    ip.pApplicationSwapChain = app_chain;   // consumed by Intel on success
    ip.initFlags       = (g_inverted != 0u) ? (uint32_t)XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH
                                            : (uint32_t)XEFG_SWAPCHAIN_INIT_FLAG_NONE;
    ip.maxInterpolatedFrames = (g_frames_req > 0u) ? g_frames_req : 1u;
    ip.creationNodeMask = 1;
    ip.visibleNodeMask  = 1;
    ip.uiMode = XEFG_SWAPCHAIN_UI_MODE_NONE;
    const xefg_swapchain_result_t r = efg_init_sc(g_ctx, present_queue, &ip);
    if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS)
    {
        const char *hint =
            ((int)r == -15) ? " (-15 = LATENCY_REDUCTION_UNSUPPORTED: no XeLL context was connected "
                              "to the FG context, or this device offers no latency reduction)"
          : ((int)r == -19) ? " (-19 = POINTER_STILL_IN_USE: something still holds the application "
                              "chain; a backbuffer reference, or another owner)"
          : ((int)r == -18) ? " (-18 = DXGI_INVALID_CALL: the chain was in a state the proxy will "
                              "not take - mid-resize, or exclusive fullscreen)"
          : "";
        snprintf(l, sizeof l, "[MGPU][XFG][HO] handover FAILED: InitFromSwapChain returned %d%s. "
                 "Intel's guide tells the application to validate the chain's reference count after "
                 "a failed initialization, so the chain is treated as still ours: the bridge keeps "
                 "it and presents 1x. Nothing retries this run (one attempt per chain).",
                 (int)r, hint);
        mgpu::diag::error(l);
        return false;
    }
    // Intel owns app_chain now. The proxy chain to present through comes back
    // from GetSwapChainPtr; the module holds one reference (released at disarm)
    // and hands the caller a second (its IDXGISwapChain3, for S.swapchain).
    IDXGISwapChain *proxy = nullptr;
    const xefg_swapchain_result_t r2 = efg_get_sc_ptr(g_ctx, __uuidof(IDXGISwapChain),
                                                      reinterpret_cast<void **>(&proxy));
    if (r2 != XEFG_SWAPCHAIN_RESULT_SUCCESS || proxy == nullptr)
    {
        snprintf(l, sizeof l, "[MGPU][XFG][HO] handover OFF: GetSwapChainPtr returned %d. "
                 "The proxy consumed the app chain but no chain could be handed back; the "
                 "bridge has no chain to present on until the next arm.", (int)r2);
        mgpu::diag::error(l);
        return false;
    }
    IDXGISwapChain3 *p3 = nullptr;
    const HRESULT q = proxy->QueryInterface(__uuidof(IDXGISwapChain3),
                                            reinterpret_cast<void **>(&p3));
    if (FAILED(q) || p3 == nullptr)
    {
        snprintf(l, sizeof l, "[MGPU][XFG][HO] handover OFF: the proxy is not IDXGISwapChain3 "
                 "(hr=0x%08X) - the bridge needs GetCurrentBackBufferIndex.", (unsigned)q);
        mgpu::diag::error(l);
        proxy->Release();
        g_ho_lost = true;   // Intel owns the app chain now: nothing to present on
        return false;
    }
    g_ho_chain = proxy;   // module reference, released by handover_disarm
    *out_chain3 = p3;     // caller reference
    g_ho_live.store(true, std::memory_order_relaxed);
    g_ho_tags = 0; g_ho_tag_fails = 0; g_ho_2x = 0; g_ho_1x = 0;

    // FRAME GENERATION IS OFF UNTIL IT IS TOLD OTHERWISE. Intel's guide, line
    // 340: "XeSS-FG is disabled by default and can be enabled by calling
    // xefgSwapChainSetEnabled." The 2026-10-28 22:28 run proved it the expensive
    // way - the proxy was live, every tag was accepted (fails=0 across 4200
    // frames) and every present came back framesPresented=1, because nobody
    // ever asked for interpolation.
    // [PORT H2 2b-1, 2026-11-11] enable_generation=false is the 2b-1 gate: take the
    // chain over, prove the proxy presents the SAME picture, and leave interpolation
    // off. Wiring generation before the presenter hands over colour/matrices is the
    // half-connected state that reads as "XeSS does nothing" or as a tagged-frame
    // mismatch - both are worse than 1x.
    if (enable_generation && efg_set_enabled != nullptr)
    {
        const xefg_swapchain_result_t er = efg_set_enabled(g_ctx, 1u);
        snprintf(l, sizeof l, "[MGPU][XFG][HO] xefgSwapChainSetEnabled(1) returned %d (0 = SUCCESS). "
                 "From here the proxy presents an interpolated frame between the application's.",
                 (int)er);
        if (er == XEFG_SWAPCHAIN_RESULT_SUCCESS) mgpu::diag::info(l); else mgpu::diag::warn(l);
    }
    else if (!enable_generation)
    {
        mgpu::diag::info("[MGPU][XFG][HO] 2b-1 arm: handover only - xefgSwapChainSetEnabled is NOT "
                         "called, the proxy stays in passthrough (1x), the picture must match the "
                         "author's chain exactly. This is the gate before tags and generation.");
    }
    else
    {
        mgpu::diag::warn("[MGPU][XFG][HO] libxess_fg.dll does not export xefgSwapChainSetEnabled - "
                         "the proxy stays in passthrough (1x): XeSS-FG is disabled by default.");
    }

    snprintf(l, sizeof l, "[MGPU][XFG][HO] HANDOVER LIVE: the present chain is now XeSS-FG's "
             "proxy (%ux%u fmt %d, %u interpolated frame(s) requested). The present list tags "
             "depth/mvec + matrices and sets the present-id before each Present.",
             width, height, (int)backbuffer_format, ip.maxInterpolatedFrames);
    mgpu::diag::info(l);
    return true;
}

bool tag_frame(ID3D12GraphicsCommandList *cl, unsigned long long frame,
               ID3D12Resource *color, D3D12_RESOURCE_STATES color_state,
               unsigned color_w, unsigned color_h,
               ID3D12Resource *depth, D3D12_RESOURCE_STATES depth_state,
               unsigned depth_w, unsigned depth_h,
               ID3D12Resource *mvec, D3D12_RESOURCE_STATES mvec_state,
               unsigned mvec_w, unsigned mvec_h,
               const float view[16], const float proj[16],
               bool reset_history)
{
    if (!g_ho_live.load(std::memory_order_relaxed) || cl == nullptr) return false;
    char l[300];
    // The 2x investigation (2026-10-29): depth and vectors are the two inputs
    // Intel cannot interpolate without (Quick Start "minimum set"), and a tag
    // that carries neither still spends its id - the library answers
    // "Missing resource" and the frame is 1x anyway. Skip the tag entirely on
    // such a frame: the present passes through 1x, the SEQUENCE below stays
    // intact, and the next complete frame continues without a mismatch.
    if (mvec == nullptr || depth == nullptr)
    {
        static unsigned long long ho_skipped = 0;
        if ((++ho_skipped % 600ull) == 1ull)
        {
            snprintf(l, sizeof l, "[MGPU][XFG][HO] tag skipped on incomplete input x%llu "
                     "(mvec=%s depth=%s): 1x passthrough, sequence not spent.",
                     ho_skipped, mvec ? "ok" : "NULL", depth ? "ok" : "NULL");
            mgpu::diag::info(l);
        }
        return false;
    }
    // Intel's ids must be consecutive (a gap answers FRAMES_ID_MISMATCH and
    // drops that frame to 1x - measured id_gap 8.6% at FrameCap=60). The seal
    // number is not consecutive by design: backpressure skips frames. The
    // interface therefore runs on its own counter; the seal number stays the
    // log's single source (discipline #1 untouched).
    const uint32_t id = ++g_ho_seq;
    bool ok = true;
    auto tag = [&](xefg_swapchain_resource_type_t type, ID3D12Resource *res,
                   D3D12_RESOURCE_STATES st, unsigned w, unsigned h)
    {
        if (res == nullptr) return;   // optional input absent: the library decides
        xefg_swapchain_d3d12_resource_data_t rd = {};
        rd.type = type;
        rd.validity = XEFG_SWAPCHAIN_RV_ONLY_NOW;   // copied on THIS list, now
        rd.resourceBase = { 0, 0 };
        rd.resourceSize = { w, h };
        rd.pResource = res;
        rd.incomingState = st;
        const xefg_swapchain_result_t r = efg_tag_res(g_ctx, cl, id, &rd);
        if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS)
        {
            ok = false;
            if (g_ho_tag_fails < 5u)
            {
                snprintf(l, sizeof l, "[MGPU][XFG][HO] tag type=%d f=%llu returned %d.",
                         (int)type, frame, (int)r);
                mgpu::diag::warn(l);
            }
            ++g_ho_tag_fails;
        }
    };
    // The colour IS the backbuffer the present list has just copied the neural
    // frame into (the sample tags its backbuffer as HUDLESS_COLOR the same way,
    // with uiMode NONE nothing is composed over it). Depth and mvec carry their
    // own extents - on SOTTR the vectors are the display extent and the depth
    // the render extent, and a single size for all three would mis-describe one.
    tag(XEFG_SWAPCHAIN_RES_HUDLESS_COLOR, color, color_state, color_w, color_h);
    tag(XEFG_SWAPCHAIN_RES_DEPTH,         depth, depth_state, depth_w, depth_h);
    tag(XEFG_SWAPCHAIN_RES_MOTION_VECTOR, mvec,  mvec_state,  mvec_w,  mvec_h);
    // The 2x=0 investigation: print the ACTUAL geometry the tag carries. Intel
    // requires depth and motion vectors to share a size, and low-res vectors
    // (the recommended mode, and what a game-internal upscaler produces) need
    // motionVectorScale derived from the RENDER width, not the display width.
    // One line per 600 tags - enough to see a XeSS run's sizes, silent in a
    // normal run. mvec=null or a size mismatch here is the smoking gun.
    {
        static unsigned long long geo_n = 0;
        if ((++geo_n % 600ull) == 1ull)
        {
            char gl[260];
            snprintf(gl, sizeof gl,
                     "[MGPU][XFG][HO] tag geometry f=%llu: color=%ux%u depth=%s%ux%u "
                     "mvec=%s%ux%u scale=(%.6g,%.6g) reset=%u.",
                     frame, color_w, color_h,
                     depth ? "" : "NULL ", depth_w, depth_h,
                     mvec ? "" : "NULL ", mvec_w, mvec_h,
                     mvec_w ? 2.0f / (float)mvec_w : 0.0f,
                     mvec_h ? 2.0f / (float)mvec_h : 0.0f,
                     reset_history ? 1u : 0u);
            mgpu::diag::info(gl);
        }
    }
    if (view != nullptr && proj != nullptr)
    {
        // Phase 3 (motionVectorScale calibration): XeFGMvScaleMul multiplies
        // the pixel->NDC scale - the three-step test the plan names (0.5 / 1 /
        // 2), judged by the smear direction and size on the interpolated
        // frames. Read once per arm (the keys are arm-time keys), default 1.0
        // = exactly the behaviour every run so far had.
        static float mv_mul = -1.0f;
        if (mv_mul < 0.0f)
        {
            mv_mul = 1.0f;
            wchar_t ip[MAX_PATH];
            if (ini_path(ip, MAX_PATH))
            {
                FILE *f = nullptr;
                if (_wfopen_s(&f, ip, L"rb") == 0 && f != nullptr)
                {
                    std::vector<char> buf(mgpu::config::MAX_BYTES + 2u, '\0');
                    const size_t got = fread(buf.data(), 1, mgpu::config::MAX_BYTES + 1u, f);
                    fclose(f);
                    if (got <= mgpu::config::MAX_BYTES)
                    {
                        buf[got] = '\0';
                        const char *v = mgpu::config::find(buf.data(), got, "XeFGMvScaleMul");
                        if (v != nullptr)
                        {
                            const float fv = (float)atof(v);
                            // NaN-safe (discipline #2): reject-unless-in-range.
                            if (fv >= 0.1f && fv <= 4.0f) mv_mul = fv;
                        }
                    }
                }
            }
            snprintf(l, sizeof l, "[MGPU][XFG][HO] motionVectorScale multiplier=%g "
                     "(XeFGMvScaleMul; 1 = 2/render-extent, the phase-2 default).", mv_mul);
            mgpu::diag::info(l);
        }
        xefg_swapchain_frame_constant_data_t cd = {};
        memcpy(cd.viewMatrix, view, sizeof cd.viewMatrix);
        memcpy(cd.projectionMatrix, proj, sizeof cd.projectionMatrix);
        cd.motionVectorScaleX = (2.0f / (float)mvec_w) * mv_mul;   // pixels -> NDC
        cd.motionVectorScaleY = (2.0f / (float)mvec_h) * mv_mul;
        cd.resetHistory = reset_history ? 1u : 0u;
        const xefg_swapchain_result_t r = efg_tag_const(g_ctx, id, &cd);
        if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS)
        {
            ok = false;
            if (g_ho_tag_fails < 5u)
            {
                snprintf(l, sizeof l, "[MGPU][XFG][HO] tag constants f=%llu returned %d.",
                         frame, (int)r);
                mgpu::diag::warn(l);
            }
            ++g_ho_tag_fails;
        }
    }
    if (ok)
    {
        g_ho_last_tag_id = id;   // set_present_id names THIS tag's id, not the seal's
        ++g_ho_tags;
        // The 2x=52% investigation, two counters that settle it in one run:
        //   nov - tags that carried NO motion vector (Intel cannot interpolate
        //         those; the tag geometry line showed ~24% NULL on the XeSS run)
        //   gap - tags whose frame number is not the previous tag + 1. A ring
        //         overrun (game 85/s vs bridge 55/s) skips consumed frames, and
        //         a skipped id is exactly what genResult=4
        //         (FRAMES_ID_MISMATCH) names. gap>0 with nov==0 says pacing, not
        //         the vector lane.
        static unsigned long long ho_nov = 0, ho_gap = 0, ho_last_f = 0;
        if (mvec == nullptr) ++ho_nov;
        if (ho_last_f != 0ull && frame != ho_last_f + 1ull) ++ho_gap;
        ho_last_f = frame;
        if ((g_ho_tags % 600ull) == 1ull)
        {
            snprintf(l, sizeof l, "[MGPU][XFG][HO] tags=%llu fails=%llu (last f=%llu, 2x=%u 1x=%u) "
                     "mvec_missing=%llu id_gap=%llu.",
                     g_ho_tags, g_ho_tag_fails, frame, g_ho_2x, g_ho_1x, ho_nov, ho_gap);
            mgpu::diag::info(l);
        }
    }
    return ok;
}

uint32_t last_tag_id() { return g_ho_last_tag_id; }

bool set_present_id(unsigned long long frame)
{
    if (!g_ho_live.load(std::memory_order_relaxed)) return false;
    const uint32_t id = (uint32_t)(frame & 0xFFFFFFFFull);
    // Intel: "The application must use the same frame ID value for XeLL markers
    // and the XeSS-FG present ID." START brackets the Present this id names;
    // END is added in last_present_status, immediately after it.
    if (g_xell != nullptr && xell_marker != nullptr)
        (void)xell_marker(g_xell, id, XELL_PRESENT_START);
    g_ho_last_id = id;
    return efg_set_pid(g_ctx, id) == XEFG_SWAPCHAIN_RESULT_SUCCESS;
}

bool last_present_status(unsigned &frames_presented, bool &frame_gen_enabled)
{
    if (!g_ho_live.load(std::memory_order_relaxed) || efg_last_status == nullptr) return false;
    xefg_swapchain_present_status_t ps = {};
    const xefg_swapchain_result_t sr = efg_last_status(g_ctx, &ps);
    if (g_xell != nullptr && xell_marker != nullptr)
        (void)xell_marker(g_xell, g_ho_last_id, XELL_PRESENT_END);
    if (sr != XEFG_SWAPCHAIN_RESULT_SUCCESS) return false;
    frames_presented = ps.framesPresented;
    frame_gen_enabled = (ps.isFrameGenEnabled != 0u);
    if (ps.framesPresented >= 2u) ++g_ho_2x; else ++g_ho_1x;

    // Intel's own verdict, every 600 presents. This is the line that separates
    // "the library says generation is off" (SetEnabled was not called, or it
    // refused) from "generation is on and still produced one frame" (a tagging
    // or pacing problem) - two very different next steps.
    static unsigned long long ps_n = 0;
    ++ps_n;
    if ((ps_n % 600ull) == 1ull)
    {
        char l[240];
        snprintf(l, sizeof l, "[MGPU][XFG][HO] present status #%llu: framesPresented=%u "
                 "frameGenEnabled=%u genResult=%d xell=%s (2x=%u 1x=%u).",
                 ps_n, (unsigned)ps.framesPresented, (unsigned)ps.isFrameGenEnabled,
                 (int)ps.frameGenResult,
                 (g_xell != nullptr) ? "context" : "NONE", g_ho_2x, g_ho_1x);
        mgpu::diag::info(l);
    }
    return true;
}

void handover_disarm()
{
    if (g_ho_chain != nullptr) { g_ho_chain->Release(); g_ho_chain = nullptr; }
    g_ho_live.store(false, std::memory_order_relaxed);
    // The chain reference is gone now. If teardown deferred the destroy
    // because the handover was still live, finish it here: the context and
    // the libraries can finally go.
    if (g_ho_ctx_pending)
    {
        g_ho_ctx_pending = false;
        teardown();
    }
}
}  // namespace mgpu::xess_fg
