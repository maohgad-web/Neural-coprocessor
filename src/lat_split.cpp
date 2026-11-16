// MGPU Bridge - R402 LatSplit=1: where the time before the neural card goes. See lat_split.hpp.

#include "lat_split.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "diag.hpp"
#include "mgpu_ini_parser.hpp"

namespace mgpu::latsplit
{
namespace
{
const unsigned N = 256u;   // frames remembered (a ring by frame number)

struct stamp { std::atomic<unsigned long long> f{0ull}; std::atomic<long long> t{0}; };
stamp g_sig[N], g_gpu0[N], g_arr[N];

std::atomic<bool> g_on{false}, g_stop{false};
std::atomic<bool> g_have{false};        // the fences are known
std::atomic<ID3D12Fence *> g_sf{nullptr}, g_pf{nullptr};
std::thread *g_ta = nullptr, *g_tb = nullptr;   // bridge thread starts and joins them
HANDLE g_stop_ev = nullptr;
long long g_freq = 0;

// the window (bridge thread)
double g_sum[5] = {}, g_max[5] = {};
unsigned long long g_n = 0ull, g_partial = 0ull, g_seen = 0ull;

// R410: report() runs on the bridge thread UNDER s.cs (seal_consume); a log write there can wait on the disk and the
// game thread's s.cs in the effects step waits behind it (Dawnwalker, external SSD: stutter with LatSplit=1, none
// with LatSplit=0, 2026-11-15 REG 10d / 10e). The line is formatted under the lock and WRITTEN by a watcher thread.
// g_line_state: 0 free, 3 being filled (bridge), 1 ready, 2 being written (a watcher). A report that finds it busy
// is dropped and counted.
char g_line[1100];
std::atomic<int> g_line_state{0};
unsigned long long g_line_dropped = 0ull;

void flush_line()
{
    int ready = 1;
    if (!g_line_state.compare_exchange_strong(ready, 2, std::memory_order_acq_rel)) return;
    mgpu::diag::info(g_line);
    g_line_state.store(0, std::memory_order_release);
}

void put(stamp *a, unsigned long long f, long long t)
{
    stamp &s = a[(unsigned)(f % N)];
    s.t.store(t, std::memory_order_relaxed);
    s.f.store(f, std::memory_order_release);
}

long long get(stamp *a, unsigned long long f)
{
    stamp &s = a[(unsigned)(f % N)];
    if (s.f.load(std::memory_order_acquire) != f) return 0;
    return s.t.load(std::memory_order_relaxed);
}

// One fence: the time each value is reached. A value already passed when the watcher gets to it has no time.
void watch(std::atomic<ID3D12Fence *> *which, stamp *into)
{
    ID3D12Fence *const fence = which->load(std::memory_order_acquire);
    if (fence == nullptr) return;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (ev == nullptr) return;
    unsigned long long next = (unsigned long long)fence->GetCompletedValue() + 1ull;
    while (!g_stop.load(std::memory_order_acquire))
    {
        flush_line();   // R410: the [R402] line is written here, off the bridge thread's s.cs
        const UINT64 done = fence->GetCompletedValue();
        if (done == (UINT64)-1) break;                                 // the device is gone
        if ((unsigned long long)done >= next) { next = (unsigned long long)done + 1ull; continue; }   // passed before it was watched: no time
        if (FAILED(fence->SetEventOnCompletion((UINT64)next, ev))) break;
        HANDLE hs[2] = { ev, g_stop_ev };
        const DWORD w = WaitForMultipleObjects(2, hs, FALSE, 500);
        if (w == WAIT_OBJECT_0)
        {
            LARGE_INTEGER q{};
            QueryPerformanceCounter(&q);
            put(into, next, q.QuadPart);
            ++next;
        }
        else if (w == WAIT_OBJECT_0 + 1) break;                        // stop
        else if (w == WAIT_TIMEOUT) { ResetEvent(ev); }                // nothing came (a menu, a pause): look again
        else break;
    }
    CloseHandle(ev);
}

bool module_ini(wchar_t *path, size_t n)
{
    static const int anchor = 0;
    HMODULE hm = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&anchor), &hm))
        return false;
    wchar_t mod[MAX_PATH];
    const DWORD len = GetModuleFileNameW(hm, mod, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return false;
    wchar_t *slash = wcsrchr(mod, L'\\');
    if (slash == nullptr) return false;
    slash[1] = L'\0';
    return _snwprintf_s(path, n, _TRUNCATE, L"%lsmgpu.ini", mod) > 0;
}

void report()
{
    static const char *const name[5] = { "record -> hand-over call (the game's thread)",
                                         "hand-over call -> the game card has run the frame to it",
                                         "-> the staging copy has moved it across the link",
                                         "-> the bridge thread's poll sees it",
                                         "-> consumed (the neural work)" };
    int free_ = 0;
    if (!g_line_state.compare_exchange_strong(free_, 3, std::memory_order_acq_rel))
    {   // R410: the last line is not written yet - drop this one, keep the window going
        ++g_line_dropped;
        for (unsigned i = 0; i < 5u; ++i) { g_sum[i] = 0.0; g_max[i] = 0.0; }
        g_n = g_partial = 0ull;
        return;
    }
    char *const l = g_line;
    const size_t lsz = sizeof g_line;
    int at = snprintf(l, lsz, "[MGPU][R402] LatSplit, last %llu consumed frame(s) with every time known (%llu more without one of them; "
                      "%llu consumed in all; %llu line(s) dropped while the last was being written). Mean ms (max):", g_n, g_partial, g_seen, g_line_dropped);
    double total = 0.0;
    for (unsigned i = 0; i < 5u && at > 0 && at < (int)lsz; ++i)
    {
        const double m = (g_n != 0ull) ? g_sum[i] / (double)g_n : 0.0;
        total += m;
        at += snprintf(l + at, lsz - (size_t)at, " | %s %.2f (%.2f)", name[i], m, g_max[i]);
    }
    if (at > 0 && at < (int)lsz) snprintf(l + at, lsz - (size_t)at, " | sum %.2f. READ-ONLY.", total);
    g_line_state.store(1, std::memory_order_release);   // R410: a watcher writes it
    for (unsigned i = 0; i < 5u; ++i) { g_sum[i] = 0.0; g_max[i] = 0.0; }
    g_n = g_partial = 0ull;
}
}

void arm()
{
    release();
    wchar_t path[MAX_PATH];
    if (!module_ini(path, MAX_PATH)) return;
    FILE *f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || f == nullptr) return;
    std::vector<char> buf(mgpu::config::MAX_BYTES + 2u, '\0');
    const size_t got = fread(buf.data(), 1, mgpu::config::MAX_BYTES + 1u, f);
    fclose(f);
    if (got > mgpu::config::MAX_BYTES) return;
    buf[got] = '\0';
    const char *v = mgpu::config::find(buf.data(), got, "LatSplit");
    if (v == nullptr || atoi(v) != 1) return;
    LARGE_INTEGER fq{};
    QueryPerformanceFrequency(&fq);
    if (fq.QuadPart <= 0) return;
    g_freq = fq.QuadPart;
    for (unsigned i = 0; i < N; ++i) { g_sig[i].f.store(0ull); g_gpu0[i].f.store(0ull); g_arr[i].f.store(0ull); }
    for (unsigned i = 0; i < 5u; ++i) { g_sum[i] = 0.0; g_max[i] = 0.0; }
    g_n = g_partial = g_seen = 0ull;
    g_line_state.store(0); g_line_dropped = 0ull;   // R410
    g_stop.store(false);
    g_on.store(true, std::memory_order_release);
    mgpu::diag::info("[MGPU][R402] LatSplit=1: the time from the game recording a frame to the bridge consuming it is split into "
                     "its steps, one line every 1200 consumed frames. Read-only. Needs StageCopy (the shipped default); with "
                     "StageCopy off no line is written.");
}

void sig(unsigned long long f, ID3D12Fence *stage_fence, ID3D12Fence *produced_fence)
{
    if (!g_on.load(std::memory_order_relaxed)) return;
    LARGE_INTEGER q{};
    QueryPerformanceCounter(&q);
    put(g_sig, f, q.QuadPart);
    if (!g_have.load(std::memory_order_relaxed) && stage_fence != nullptr && produced_fence != nullptr)
    {
        ID3D12Fence *none = nullptr;
        if (g_sf.compare_exchange_strong(none, stage_fence))
        {   // once: the watchers hold their own references (released in release())
            stage_fence->AddRef(); produced_fence->AddRef();
            g_pf.store(produced_fence, std::memory_order_release);
            g_have.store(true, std::memory_order_release);
        }
    }
}

void consumed(unsigned long long f, unsigned long long qpc_record, unsigned long long qpc_seen, long long qpc_now)
{
    if (!g_on.load(std::memory_order_relaxed)) return;
    if (g_ta == nullptr && g_have.load(std::memory_order_acquire))
    {   // the watchers start here, on the bridge thread, once the game's thread has named the fences
        g_stop_ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (g_stop_ev != nullptr)
        {
            g_ta = new std::thread(watch, &g_sf, g_gpu0);
            g_tb = new std::thread(watch, &g_pf, g_arr);
        }
    }
    ++g_seen;
    const long long t_sig = get(g_sig, f), t_gpu0 = get(g_gpu0, f), t_arr = get(g_arr, f);
    if (t_sig == 0 || t_gpu0 == 0 || t_arr == 0 || qpc_record == 0ull || qpc_seen == 0ull) ++g_partial;
    else
    {
        const double k = 1000.0 / (double)g_freq;
        const double d[5] = { (double)(t_sig - (long long)qpc_record) * k, (double)(t_gpu0 - t_sig) * k, (double)(t_arr - t_gpu0) * k,
                              (double)((long long)qpc_seen - t_arr) * k, (double)(qpc_now - (long long)qpc_seen) * k };
        for (unsigned i = 0; i < 5u; ++i) { g_sum[i] += d[i]; if (d[i] > g_max[i]) g_max[i] = d[i]; }
        ++g_n;
    }
    if (g_seen % 1200ull == 0ull) report();
}

void release()
{
    g_on.store(false, std::memory_order_release);
    g_stop.store(true, std::memory_order_release);
    if (g_stop_ev != nullptr) SetEvent(g_stop_ev);
    if (g_ta != nullptr) { g_ta->join(); delete g_ta; g_ta = nullptr; }
    if (g_tb != nullptr) { g_tb->join(); delete g_tb; g_tb = nullptr; }
    if (g_stop_ev != nullptr) { CloseHandle(g_stop_ev); g_stop_ev = nullptr; }
    g_have.store(false, std::memory_order_release);
    ID3D12Fence *a = g_sf.exchange(nullptr), *b = g_pf.exchange(nullptr);
    if (a != nullptr) a->Release();
    if (b != nullptr) b->Release();
}
}
