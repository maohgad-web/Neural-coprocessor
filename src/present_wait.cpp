// MGPU Bridge - R375 (0.5.1 discovery): a waitable present chain with a frame latency of 1.
// See present_wait.hpp. Off unless PresentLatency1=1.
#include <windows.h>
#include <dxgi1_4.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <vector>

#include "present_wait.hpp"
#include "diag.hpp"
#include "mgpu_ini_parser.hpp"

namespace mgpu
{
namespace pwait
{
namespace
{
std::atomic<bool> g_want{false};        // the key said 1 when the chain's flags were asked for
std::atomic<bool> g_on{false};          // the waitable object exists
std::atomic<HANDLE> g_obj{nullptr};

// ---- the presenting thread only (present_frame has one caller at a time) ----
LARGE_INTEGER g_freq{};
unsigned long long g_waits = 0ull, g_timeouts = 0ull;
double g_sum_ms = 0.0, g_max_ms = 0.0;

// The key, from mgpu.ini beside the add-on. Its own reader: ui_ini_read's 12-slot mirror caches
// the first keys asked for (MVEC_NONNGX_LEDGER 1.3), so a new key never goes through it.
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
    const char *v = mgpu::config::find(buf.data(), got, "PresentLatency1");
    return v != nullptr && atoi(v) == 1;
}

void state_latency(IDXGISwapChain3 *sc, const char *when)
{
    const HRESULT lh = sc->SetMaximumFrameLatency(1);
    HANDLE h = g_obj.load(std::memory_order_acquire);
    if (SUCCEEDED(lh) && h == nullptr)
    {
        h = sc->GetFrameLatencyWaitableObject();
        g_obj.store(h, std::memory_order_release);
    }
    g_on.store(h != nullptr, std::memory_order_release);
    char l[420];
    snprintf(l, sizeof l,
             "[MGPU][R375] PresentLatency1=1 (%s): maximum frame latency 1 hr=0x%08X, waitable object %s. Each present "
             "now waits (100 ms at most) on the presenting thread until the chain has a free slot, so at most one frame "
             "is queued behind the one on screen. No game thread waits. Absent or 0: the chain as before (Flags=0).",
             when, (unsigned)lh, h != nullptr ? "ready" : "MISSING - presenting without the wait");
    mgpu::diag::info(l);
}
}   // namespace

unsigned chain_flags()
{
    const bool w = key_is_one();
    g_want.store(w, std::memory_order_release);
    return w ? (unsigned)DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT : 0u;
}

void after_create(IDXGISwapChain3 *sc)
{
    if (!g_want.load(std::memory_order_acquire) || sc == nullptr) return;
    // an object left by a chain that was never torn down through release() belongs to a dead chain
    HANDLE old = g_obj.exchange(nullptr, std::memory_order_acq_rel);
    if (old != nullptr) CloseHandle(old);
    QueryPerformanceFrequency(&g_freq);
    g_waits = g_timeouts = 0ull; g_sum_ms = g_max_ms = 0.0;
    state_latency(sc, "chain created");
}

unsigned resize_flags()
{
    return g_want.load(std::memory_order_acquire) ? (unsigned)DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT : 0u;
}

void after_resize(IDXGISwapChain3 *sc)
{
    if (!g_want.load(std::memory_order_acquire) || sc == nullptr) return;
    (void)sc->SetMaximumFrameLatency(1);   // the object is the chain's and stays; the latency is stated again
}

bool on() { return g_on.load(std::memory_order_relaxed); }

void before_present()
{
    const HANDLE h = g_obj.load(std::memory_order_acquire);
    if (h == nullptr) return;
    LARGE_INTEGER a{}, b{};
    QueryPerformanceCounter(&a);
    const DWORD r = WaitForSingleObject(h, 100);
    QueryPerformanceCounter(&b);
    const double ms = (g_freq.QuadPart > 0) ? (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)g_freq.QuadPart : 0.0;
    g_sum_ms += ms; if (ms > g_max_ms) g_max_ms = ms;
    if (r != WAIT_OBJECT_0) ++g_timeouts;
    if (++g_waits % 1200ull == 0ull)
    {
        char l[320];
        snprintf(l, sizeof l,
                 "[MGPU][R375] PresentLatency1: last 1200 waits for the chain's free slot: mean %.2f ms, max %.2f ms, "
                 "not signalled within 100 ms %llu (those presents went ahead).",
                 g_sum_ms / 1200.0, g_max_ms, g_timeouts);
        mgpu::diag::info(l);
        g_sum_ms = g_max_ms = 0.0; g_timeouts = 0ull;
    }
}

void release()
{
    g_on.store(false, std::memory_order_release);
    g_want.store(false, std::memory_order_release);
    HANDLE h = g_obj.exchange(nullptr, std::memory_order_acq_rel);
    if (h != nullptr) CloseHandle(h);
}
}   // namespace pwait
}   // namespace mgpu
