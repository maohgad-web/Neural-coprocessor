// MGPU Bridge - R371 (0.5.1 discovery): read-only instruments for frame generation.
// See fg_instr.hpp. Off unless FGInstr=1 and FrameGen=4 is armed.
#include <windows.h>
#include <dwmapi.h>
#include <dxgi1_4.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>

#include "fg_instr.hpp"
#include "diag.hpp"

#pragma comment(lib, "dwmapi.lib")

namespace mgpu
{
namespace fginstr
{
namespace
{
std::atomic<bool> g_on{false};
double g_refresh_hz = 0.0;          // DWM composition clock at the arm (0 = not read)

// ---- pacing thread only ----
LARGE_INTEGER g_freq{};
LARGE_INTEGER g_last_present{};
unsigned long long g_presents = 0ull;
double g_pi_sum = 0.0, g_pi_sq = 0.0, g_pi_max = 0.0;   // present-to-present, this window
unsigned long long g_pi_n = 0ull;
// R371a: the frame statistics. PresentCount / PresentRefreshCount say when a frame reached the display
// (Microsoft, DXGI_FRAME_STATISTICS). SyncRefreshCount is only the vblank count at the statistics' own clock
// sample: part 1 read it as "the vblank of the newest frame", which it is not (run I1, 2026-11-14).
bool g_have_prev = false;
// R403: from the return of Present to the vblank the frame statistics name for that frame. The stamp is taken when
// Present has returned; the statistics are read only at our own presents, so the figure is an upper bound by up to
// one present interval.
unsigned g_pd_id[64] = {}; long long g_pd_t[64] = {}; unsigned g_pd_seen = 0u;
double g_pd_sum = 0.0, g_pd_max = 0.0; unsigned long long g_pd_n = 0ull, g_pd_miss = 0ull, g_pd_odd = 0ull;
unsigned g_pc_prev = 0u, g_prc_prev = 0u;   // PresentCount / PresentRefreshCount of the previous reading
unsigned g_pc_w0 = 0u, g_src_w0 = 0u;       // PresentCount / SyncRefreshCount at the first reading of this window
unsigned g_pc_w1 = 0u, g_src_w1 = 0u;       // ... and at the newest
bool g_have_w0 = false;
unsigned long long g_vbl[10] = {};  // vblanks between two consecutive displayed frames: [0]..[8], [9] = 9 or more
unsigned long long g_stat_ok = 0ull;        // readings where exactly one new frame was reported (the histogram)
unsigned long long g_stat_same = 0ull;      // readings where no new frame was reported since the previous reading
unsigned long long g_stat_multi = 0ull, g_multi_frames = 0ull, g_multi_vbl = 0ull;   // more than one frame between two readings
unsigned long long g_stat_none = 0ull;      // GetFrameStatistics failed
HRESULT g_stat_last_hr = S_OK;

// ---- R386: waits for the stream lock ----
// presenting thread only (the same thread that calls after_present and report)
double g_lp_sum = 0.0, g_lp_max = 0.0; unsigned long long g_lp_n = 0ull, g_lp_over = 0ull;
// game thread writes, presenting thread reads and clears
std::atomic<unsigned long long> g_lg_n{0}, g_lg_us_sum{0}, g_lg_us_max{0}, g_lg_over{0};
// R388: bridge thread writes, presenting thread reads and clears
struct acc_t
{
    std::atomic<unsigned long long> n{0}, us_sum{0}, us_max{0}, over{0};
    void add(unsigned long long us)
    {
        n.fetch_add(1ull, std::memory_order_relaxed);
        us_sum.fetch_add(us, std::memory_order_relaxed);
        unsigned long long m = us_max.load(std::memory_order_relaxed);
        while (us > m && !us_max.compare_exchange_weak(m, us, std::memory_order_relaxed)) { }
        if (us > 1000ull) over.fetch_add(1ull, std::memory_order_relaxed);
    }
    void take(unsigned long long &cn, double &mean_ms, double &max_ms, unsigned long long &cover)
    {
        cn = n.exchange(0ull, std::memory_order_relaxed);
        const unsigned long long su = us_sum.exchange(0ull, std::memory_order_relaxed);
        max_ms = (double)us_max.exchange(0ull, std::memory_order_relaxed) / 1000.0;
        cover = over.exchange(0ull, std::memory_order_relaxed);
        mean_ms = cn ? (double)su / (double)cn / 1000.0 : 0.0;
    }
};
acc_t g_hold[3], g_fgrec;
// R394: bridge thread writes, presenting thread reads and clears. Counts are stored x1000 so the mean keeps decimals.
acc_t g_lat, g_newer, g_inflight, g_passed, g_gframe, g_ready;
unsigned long long g_cons_prev_frame = 0ull, g_cons_prev_qpc = 0ull;   // bridge thread only
bool g_fg_armed = true;
bool us_since(long long t0, unsigned long long &us)
{
    if (g_freq.QuadPart <= 0) return false;
    LARGE_INTEGER q{};
    QueryPerformanceCounter(&q);
    const long long d = (long long)q.QuadPart - t0;
    if (d < 0) return false;
    us = (unsigned long long)((double)d * 1000000.0 / (double)g_freq.QuadPart);
    return us <= 5000000ull;
}

// ---- hand-over stats: written by the bridge thread, read by the pacing thread ----
std::mutex g_hmx;
double g_h_sum = 0.0, g_h_sq = 0.0, g_h_max = 0.0, g_h_min = 0.0;
unsigned long long g_h_n = 0ull;

void clear_window()
{
    g_pi_sum = g_pi_sq = g_pi_max = 0.0; g_pi_n = 0ull;
    for (unsigned i = 0; i < 10u; ++i) g_vbl[i] = 0ull;
    g_stat_ok = g_stat_same = g_stat_multi = g_multi_frames = g_multi_vbl = g_stat_none = 0ull;
    g_have_w0 = false;
    g_lp_sum = g_lp_max = 0.0; g_lp_n = g_lp_over = 0ull;   // R386
    g_pd_sum = g_pd_max = 0.0; g_pd_n = g_pd_miss = g_pd_odd = 0ull;   // R403
}

void report()
{
    double hs = 0.0, hq = 0.0, hmax = 0.0, hmin = 0.0; unsigned long long hn = 0ull;
    {
        std::lock_guard<std::mutex> lk(g_hmx);
        hs = g_h_sum; hq = g_h_sq; hmax = g_h_max; hmin = g_h_min; hn = g_h_n;
        g_h_sum = g_h_sq = g_h_max = g_h_min = 0.0; g_h_n = 0ull;
    }
    const double pim = g_pi_n ? g_pi_sum / (double)g_pi_n : 0.0;
    const double piv = g_pi_n ? g_pi_sq / (double)g_pi_n - pim * pim : 0.0;
    const double hm = hn ? hs / (double)hn : 0.0;
    const double hv = hn ? hq / (double)hn - hm * hm : 0.0;
    const unsigned pc_adv = g_have_w0 ? (g_pc_w1 - g_pc_w0) : 0u;     // frames the statistics say were displayed
    const unsigned src_adv = g_have_w0 ? (g_src_w1 - g_src_w0) : 0u;  // vblanks that passed, by the statistics' clock
    // R386
    const unsigned long long lgn = g_lg_n.exchange(0ull, std::memory_order_relaxed);
    const unsigned long long lgs = g_lg_us_sum.exchange(0ull, std::memory_order_relaxed);
    const unsigned long long lgm = g_lg_us_max.exchange(0ull, std::memory_order_relaxed);
    const unsigned long long lgo = g_lg_over.exchange(0ull, std::memory_order_relaxed);
    // R388: its own line (the [R371] line is long enough; the log queue cuts a line at 2048 characters)
    unsigned long long bhn[3] = {}, bho[3] = {}, bfn = 0ull, bfo = 0ull;
    double bhmean[3] = {}, bhmx[3] = {}, bfmean = 0.0, bfmx = 0.0;
    for (int i = 0; i < 3; ++i) g_hold[i].take(bhn[i], bhmean[i], bhmx[i], bho[i]);
    g_fgrec.take(bfn, bfmean, bfmx, bfo);
    char lw[1500];
    snprintf(lw, sizeof lw,
             "[MGPU][R386] THE STREAM LOCK, same window as the [R371] line above (frame generation %s), all in ms. "
             "WAITS FOR IT - the presenting thread (%s): mean %.3f max %.2f, over 1 ms %llu of %llu take(s) | "
             "the game's thread at finish_effects: mean %.3f max %.2f, over 1 ms %llu of %llu frame(s). "
             "THE BRIDGE THREAD'S HOLDS OF IT, by where the hold ended - at the wait for the copy: mean %.3f max %.2f, over 1 ms %llu of %llu | "
             "at the wait for the GPU (the hold that RECORDED THE PASS): mean %.3f max %.2f, over 1 ms %llu of %llu | "
             "the rest: mean %.3f max %.2f, over 1 ms %llu of %llu | of the recording hold, inside frame generation's own "
             "record: mean %.3f max %.2f over %llu call(s). A wait cannot be longer than the hold it queued behind. READ-ONLY.",
             g_fg_armed ? "ARMED" : "OFF - the baseline",
             g_fg_armed ? "the pacing thread, between the pacing wait and Present" : "the bridge thread, before Present",
             g_lp_n ? g_lp_sum / (double)g_lp_n : 0.0, g_lp_max, g_lp_over, g_lp_n,
             lgn ? (double)lgs / (double)lgn / 1000.0 : 0.0, (double)lgm / 1000.0, lgo, lgn,
             bhmean[0], bhmx[0], bho[0], bhn[0], bhmean[1], bhmx[1], bho[1], bhn[1], bhmean[2], bhmx[2], bho[2], bhn[2],
             bfmean, bfmx, bfn);
    char l[1600];
    if (g_stat_ok + g_stat_multi != 0ull && pc_adv != 0u)
        snprintf(l, sizeof l,
                 "[MGPU][R371] FG INSTRUMENT, last %llu presents | display refresh %.2f Hz (DWM, at the arm; 0 = not read) | "
                 "frames reported displayed %u, vblanks passed %u (frame statistics, this window) | "
                 "vblanks between two consecutive displayed frames: 0=%llu 1=%llu 2=%llu 3=%llu 4=%llu 5=%llu 6=%llu 7=%llu 8=%llu 9+=%llu "
                 "over %llu reading(s) with exactly one new frame | no new frame at the reading %llu | more than one frame "
                 "between readings %llu (%llu frames over %llu vblanks, not in the histogram) | no statistics %llu | "
                 "present-to-present ms: mean %.2f sd %.2f max %.2f | "
                 "hand-over interval ms (bridge thread): mean %.2f sd %.2f min %.2f max %.2f over %llu. READ-ONLY.",
                 g_pi_n, g_refresh_hz, pc_adv, src_adv,
                 g_vbl[0], g_vbl[1], g_vbl[2], g_vbl[3], g_vbl[4], g_vbl[5], g_vbl[6], g_vbl[7], g_vbl[8], g_vbl[9],
                 g_stat_ok, g_stat_same, g_stat_multi, g_multi_frames, g_multi_vbl, g_stat_none,
                 pim, piv > 0.0 ? std::sqrt(piv) : 0.0, g_pi_max,
                 hm, hv > 0.0 ? std::sqrt(hv) : 0.0, hmin, hmax, hn);
    else
        snprintf(l, sizeof l,
                 "[MGPU][R371] FG INSTRUMENT, last %llu presents | display refresh %.2f Hz (DWM, at the arm; 0 = not read) | "
                 "vblanks between displayed frames: NOT MEASURED - the chain's frame statistics did not report displayed "
                 "frames (PresentCount advanced %u, vblanks passed %u, readings without a new frame %llu, calls that failed "
                 "%llu, last hr=0x%08X). This is a gap of the instrument, not an even cadence | "
                 "present-to-present ms: mean %.2f sd %.2f max %.2f | "
                 "hand-over interval ms (bridge thread): mean %.2f sd %.2f min %.2f max %.2f over %llu. READ-ONLY.",
                 g_pi_n, g_refresh_hz, pc_adv, src_adv, g_stat_same, g_stat_none, (unsigned)g_stat_last_hr,
                 pim, piv > 0.0 ? std::sqrt(piv) : 0.0, g_pi_max,
                 hm, hv > 0.0 ? std::sqrt(hv) : 0.0, hmin, hmax, hn);
    mgpu::diag::info(l);
    mgpu::diag::info(lw);   // R386 / R388
    {   // R394: its own line
        unsigned long long n1 = 0, o = 0, n2 = 0, n3 = 0, n4 = 0, n5 = 0, n6 = 0;
        double latm = 0, latx = 0, nwm = 0, nwx = 0, ifm = 0, ifx = 0, pam = 0, pax = 0, gfm = 0, gfx = 0, rdm = 0, rdx = 0;
        g_lat.take(n1, latm, latx, o); g_newer.take(n2, nwm, nwx, o); g_inflight.take(n3, ifm, ifx, o);
        g_passed.take(n4, pam, pax, o); g_gframe.take(n5, gfm, gfx, o); g_ready.take(n6, rdm, rdx, o);
        char la[1200];
        snprintf(la, sizeof la,
                 "[MGPU][R394] THE FRAME THE BRIDGE TAKES, same window. [L2] READY-TO-CONSUME (from the first poll that saw the "
                 "frame arrived to the take - the bridge's own part): mean %.2f ms max %.2f over %llu | [SEAL] lat (from the "
                 "game RECORDING the frame to the take; the shipped notes L1 / L2 say this is mostly the game's CPU running "
                 "ahead of its own card): mean %.2f ms max %.2f over %llu | at the take the game had sealed %.2f newer "
                 "frame(s) (mean, max %.0f): %.2f not arrived yet (max %.0f) + %.2f arrived and passed over (max %.0f) | one "
                 "game frame %.2f ms. READ-ONLY.",
                 rdm, rdx, n6, latm, latx, n1, nwm, nwx, ifm, ifx, pam, pax, gfm);
        mgpu::diag::info(la);
    }
    {   // R403: its own line
        char lp[520];
        snprintf(lp, sizeof lp, "[MGPU][R403] PRESENT TO DISPLAY, same window: from the return of Present to the vblank the chain's frame "
                 "statistics name for that frame: mean %.2f ms max %.2f over %llu frame(s) | frames whose present was not in the "
                 "table %llu | values outside -5..250 ms, not counted %llu. An upper bound by up to one present interval. READ-ONLY.",
                 (g_pd_n != 0ull) ? g_pd_sum / (double)g_pd_n : 0.0, g_pd_max, g_pd_n, g_pd_miss, g_pd_odd);
        mgpu::diag::info(lp);
    }
}
}   // namespace

void arm(bool on_key, bool fg_armed)
{
    g_on.store(false, std::memory_order_release);
    if (!on_key) return;
    g_fg_armed = fg_armed;
    {   // R388: nothing left over from an earlier arm
        unsigned long long a = 0ull, b = 0ull; double c = 0.0, d = 0.0;
        for (int i = 0; i < 3; ++i) g_hold[i].take(a, c, d, b);
        g_fgrec.take(a, c, d, b);
        g_lat.take(a, c, d, b); g_newer.take(a, c, d, b); g_inflight.take(a, c, d, b); g_passed.take(a, c, d, b); g_gframe.take(a, c, d, b); g_ready.take(a, c, d, b);   // R394
        g_cons_prev_frame = 0ull; g_cons_prev_qpc = 0ull;
        g_lg_n.store(0ull); g_lg_us_sum.store(0ull); g_lg_us_max.store(0ull); g_lg_over.store(0ull);
    }
    QueryPerformanceFrequency(&g_freq);
    g_last_present.QuadPart = 0;
    g_presents = 0ull;
    g_have_prev = false;
    g_stat_last_hr = S_OK;
    clear_window();
    {
        std::lock_guard<std::mutex> lk(g_hmx);
        g_h_sum = g_h_sq = g_h_max = g_h_min = 0.0; g_h_n = 0ull;
    }
    g_refresh_hz = 0.0;
    DWM_TIMING_INFO dti{};
    dti.cbSize = sizeof dti;
    if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &dti)) && dti.qpcRefreshPeriod != 0ull && g_freq.QuadPart > 0)
        g_refresh_hz = (double)g_freq.QuadPart / (double)dti.qpcRefreshPeriod;
    char l[420];
    snprintf(l, sizeof l,
             "[MGPU][R371] FGInstr=1: frame-generation instruments ON for this arm%s. Display refresh %.2f Hz from DWM's "
             "composition clock (0 = not read). Every 1200 presents: vblanks between displayed frames, present-to-present "
             "spacing and the hand-over interval, then the [R386] line: who waited for the stream lock and how long the "
             "bridge thread held it. Read-only: nothing is waited on, queued or changed.",
             fg_armed ? "" : " - FRAME GENERATION IS OFF (FrameGen is not 3 or 4): this is the baseline of the same lines",
             g_refresh_hz);
    mgpu::diag::info(l);
    g_on.store(true, std::memory_order_release);
}

bool on() { return g_on.load(std::memory_order_relaxed); }

void note_handoff(double dt_ms)
{
    if (!on() || !(dt_ms > 0.0) || dt_ms > 1000.0) return;
    std::lock_guard<std::mutex> lk(g_hmx);
    g_h_sum += dt_ms; g_h_sq += dt_ms * dt_ms;
    if (g_h_n == 0ull || dt_ms < g_h_min) g_h_min = dt_ms;
    if (dt_ms > g_h_max) g_h_max = dt_ms;
    ++g_h_n;
}

// R386
long long lock_t0()
{
    if (!on()) return 0;
    LARGE_INTEGER q{};
    QueryPerformanceCounter(&q);
    return q.QuadPart != 0 ? (long long)q.QuadPart : 1;
}

void lock_waited_present(long long t0)
{
    if (g_freq.QuadPart <= 0) return;
    LARGE_INTEGER q{};
    QueryPerformanceCounter(&q);
    const double ms = (double)((long long)q.QuadPart - t0) * 1000.0 / (double)g_freq.QuadPart;
    if (!(ms >= 0.0) || ms > 5000.0) return;
    g_lp_sum += ms; ++g_lp_n;
    if (ms > g_lp_max) g_lp_max = ms;
    if (ms > 1.0) ++g_lp_over;
}

void lock_waited_game(long long t0)
{
    if (g_freq.QuadPart <= 0) return;
    LARGE_INTEGER q{};
    QueryPerformanceCounter(&q);
    const long long d = (long long)q.QuadPart - t0;
    if (d < 0) return;
    const unsigned long long us = (unsigned long long)((double)d * 1000000.0 / (double)g_freq.QuadPart);
    if (us > 5000000ull) return;
    g_lg_n.fetch_add(1ull, std::memory_order_relaxed);
    g_lg_us_sum.fetch_add(us, std::memory_order_relaxed);
    unsigned long long m = g_lg_us_max.load(std::memory_order_relaxed);
    while (us > m && !g_lg_us_max.compare_exchange_weak(m, us, std::memory_order_relaxed)) { }
    if (us > 1000ull) g_lg_over.fetch_add(1ull, std::memory_order_relaxed);
}

// R390
double refresh_period_ms()
{
    LARGE_INTEGER fq{};
    QueryPerformanceFrequency(&fq);
    DWM_TIMING_INFO dti{};
    dti.cbSize = sizeof dti;
    if (fq.QuadPart <= 0 || !SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &dti)) || dti.qpcRefreshPeriod == 0ull) return 0.0;
    return (double)dti.qpcRefreshPeriod * 1000.0 / (double)fq.QuadPart;
}

// R394 (bridge thread)
void note_consume(double lat_ms, unsigned long long frame, unsigned long long produced, unsigned long long arrived,
                  unsigned long long qpc_submit)
{
    if (!on() || !(lat_ms >= 0.0) || lat_ms > 5000.0) return;
    g_lat.add((unsigned long long)(lat_ms * 1000.0));
    const unsigned long long newer = (produced > frame) ? produced - frame : 0ull;
    unsigned long long infl = (arrived != 0ull && produced > arrived) ? produced - arrived : 0ull;
    if (infl > newer) infl = newer;
    const unsigned long long passed = (arrived != 0ull) ? newer - infl : 0ull;
    g_newer.add(newer * 1000ull);
    if (arrived != 0ull) { g_inflight.add(infl * 1000ull); g_passed.add(passed * 1000ull); }
    if (g_cons_prev_frame != 0ull && frame > g_cons_prev_frame && qpc_submit > g_cons_prev_qpc && g_freq.QuadPart > 0)
    {
        const double ms = (double)(qpc_submit - g_cons_prev_qpc) * 1000.0 / (double)g_freq.QuadPart / (double)(frame - g_cons_prev_frame);
        if (ms > 0.5 && ms < 500.0) g_gframe.add((unsigned long long)(ms * 1000.0));
    }
    g_cons_prev_frame = frame; g_cons_prev_qpc = qpc_submit;
}

void note_ready(double ready_ms)
{
    if (!on() || !(ready_ms >= 0.0) || ready_ms > 5000.0) return;
    g_ready.add((unsigned long long)(ready_ms * 1000.0));
}

// R388 (bridge thread)
void bridge_held(long long t0, int end_site)
{
    unsigned long long us = 0ull;
    if (end_site < 0 || end_site > 2 || !us_since(t0, us)) return;
    g_hold[end_site].add(us);
}

void fg_recorded(long long t0)
{
    unsigned long long us = 0ull;
    if (us_since(t0, us)) g_fgrec.add(us);
}

void after_present(IDXGISwapChain3 *sc)
{
    if (!on() || sc == nullptr || g_freq.QuadPart <= 0) return;
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (g_last_present.QuadPart != 0)
    {
        const double ms = (double)(now.QuadPart - g_last_present.QuadPart) * 1000.0 / (double)g_freq.QuadPart;
        if (ms > 0.0 && ms < 1000.0)
        {
            g_pi_sum += ms; g_pi_sq += ms * ms; ++g_pi_n;
            if (ms > g_pi_max) g_pi_max = ms;
        }
    }
    g_last_present = now;

    {   // R403: this present's number and the time it returned
        UINT r403_last = 0;
        if (SUCCEEDED(sc->GetLastPresentCount(&r403_last))) { g_pd_id[r403_last & 63u] = r403_last; g_pd_t[r403_last & 63u] = now.QuadPart; }
    }
    DXGI_FRAME_STATISTICS fs{};
    const HRESULT hr = sc->GetFrameStatistics(&fs);
    if (SUCCEEDED(hr))
    {
        if (fs.PresentCount != g_pd_seen && fs.SyncQPCTime.QuadPart != 0)
        {   // R403: a frame newly reported displayed
            g_pd_seen = fs.PresentCount;
            const unsigned ix = fs.PresentCount & 63u;
            if (g_pd_id[ix] != fs.PresentCount || g_pd_t[ix] == 0) ++g_pd_miss;
            else
            {
                const double pd = (double)(fs.SyncQPCTime.QuadPart - g_pd_t[ix]) * 1000.0 / (double)g_freq.QuadPart;
                if (pd > -5.0 && pd < 250.0) { g_pd_sum += pd; ++g_pd_n; if (pd > g_pd_max) g_pd_max = pd; }
                else ++g_pd_odd;
            }
        }
        if (!g_have_w0) { g_have_w0 = true; g_pc_w0 = fs.PresentCount; g_src_w0 = fs.SyncRefreshCount; }
        g_pc_w1 = fs.PresentCount; g_src_w1 = fs.SyncRefreshCount;
        if (g_have_prev)
        {
            const unsigned dp = fs.PresentCount - g_pc_prev;          // frames displayed since the previous reading
            const unsigned dr = fs.PresentRefreshCount - g_prc_prev;  // vblanks between the two newest displayed frames
            if (dp == 0u) ++g_stat_same;                              // the statistics have not moved: nothing to count
            else if (dp == 1u) { ++g_vbl[(dr > 9u) ? 9u : dr]; ++g_stat_ok; }
            else { ++g_stat_multi; g_multi_frames += dp; g_multi_vbl += dr; }   // cannot be split per frame: counted apart
        }
        g_have_prev = true; g_pc_prev = fs.PresentCount; g_prc_prev = fs.PresentRefreshCount;
    }
    else
    {
        ++g_stat_none;
        g_stat_last_hr = hr;
        g_have_prev = false;   // a gap in the readings is not a long frame
    }

    if (++g_presents % 1200ull == 0ull)
    {
        report();
        clear_window();
    }
}

void release()
{
    g_on.store(false, std::memory_order_release);
}
}   // namespace fginstr
}   // namespace mgpu
