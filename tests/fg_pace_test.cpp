// R372: fg_pace.hpp against a simulated game. Not part of the add-on build.
//   g++ -std=c++17 -O1 -I src tests/fg_pace_test.cpp -o fg_pace_test && ./fg_pace_test
// Prints, per rule: the spread of on-screen frame times, the spread of (display time -
// scene time) = how unevenly motion is shown, and how long the real frame is held.
#include "fg_pace.hpp"
#include <cstdio>
#include <random>
#include <vector>
using namespace mgpu::fgpace;
struct shown_t { double t, scene; bool real; };
static void stats(const char *name, const std::vector<shown_t> &v, double L0)
{
    std::vector<double> d, lat; double rsum = 0; unsigned rn = 0;
    for (size_t i = 200; i + 1 < v.size(); ++i) { d.push_back(v[i + 1].t - v[i].t); lat.push_back(v[i].t - v[i].scene); if (v[i].real) { rsum += v[i].t - v[i].scene; ++rn; } }
    auto sd = [](const std::vector<double> &x) { double m = 0; for (double a : x) m += a; m /= x.size(); double s = 0; for (double a : x) s += (a - m) * (a - m); return std::sqrt(s / x.size()); };
    double m = 0; for (double a : d) m += a; m /= d.size();
    std::printf("  %-10s shown=%5zu frame time mean=%6.2f sd=%5.2f | motion sd=%5.2f | real held %6.2f ms\n", name, v.size(), m, sd(d), sd(lat), rn ? rsum / rn - L0 : 0.0);
}
// mode: 1 relative, 2 clock, 3 absolute. spans: game frames per evaluated frame (cycled).
static void run(int mode, unsigned n, double game_ms, const std::vector<int> &spans, double sigma, const char *name)
{
    std::mt19937 rng(7); std::normal_distribution<double> g(0.0, sigma);
    const double L0 = 40.0; std::vector<shown_t> v;
    scene_clock sc; clock_state cs; abs_state as;
    double scene = 0, last_h = -1, ema = 0, clockt = 0, free_at = 0;
    for (int i = 0; i < 4000; ++i)
    {
        const int el = spans[i % spans.size()];
        scene += el * game_ms;
        double h = scene + L0 + g(rng); if (h < last_h + 1.0) h = last_h + 1.0;
        const double dt = (last_h >= 0) ? h - last_h : 0.0;
        if (dt > 3.0 && dt < 60.0) ema = (ema <= 0.0) ? dt : ema * 0.85 + dt * 0.15;
        last_h = h;
        double c_real = 0, span = 0; sc.next(h, dt, (double)el, c_real, span);
        const double true_prev = scene - el * game_ms;
        std::vector<double> c(n + 1), truth(n + 1);
        for (unsigned k = 0; k <= n; ++k) { c[k] = c_real - span + span * (k + 1) / (n + 1); truth[k] = true_prev + el * game_ms * (k + 1) / (n + 1); }
        clockt = (h > free_at) ? h : free_at;
        auto now = [&] { return clockt; };
        auto wait = [&](double t) { if (t > clockt) clockt = t; };
        auto present = [&](unsigned j) { v.push_back({ clockt, truth[j], j == n }); return true; };
        const double e = ema > 0 ? ema : 16.0;
        if (mode == 1) pair_relative(h, e / (n + 1), n, wait, present);
        else if (mode == 2) pair_clock(cs, e / (n + 1), h, span, c.data(), n + 1, now, wait, present);
        else pair_absolute(as, e, n, now, wait, present);
        free_at = clockt;
    }
    stats(name, v, L0);
}
int main()
{
    std::printf("EVEN source 16.94 ms, 2x, hand-over jitter 1.5 ms\n");
    run(1, 1, 16.94, { 1 }, 1.5, "relative"); run(2, 1, 16.94, { 1 }, 1.5, "clock"); run(3, 1, 16.94, { 1 }, 1.5, "absolute");
    std::printf("EVEN source 18.2 ms, 4x, jitter 1.5 ms\n");
    run(1, 3, 18.2, { 1 }, 1.5, "relative"); run(2, 3, 18.2, { 1 }, 1.5, "clock"); run(3, 3, 18.2, { 1 }, 1.5, "absolute");
    std::printf("UNEVEN source: game 11.1 ms, evaluated every 3,3,2 game frames (Leawinter-type), 2x, jitter 1.5 ms\n");
    run(1, 1, 11.1, { 3, 3, 2 }, 1.5, "relative"); run(2, 1, 11.1, { 3, 3, 2 }, 1.5, "clock"); run(3, 1, 11.1, { 3, 3, 2 }, 1.5, "absolute");
    std::printf("UNEVEN source, 3x\n");
    run(1, 2, 11.1, { 3, 3, 2 }, 1.5, "relative"); run(2, 2, 11.1, { 3, 3, 2 }, 1.5, "clock");
    return 0;
}
