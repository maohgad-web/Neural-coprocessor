// R390 / R391: the limit and fill-by-step counts of fg_pace.hpp against simulated sources. Not part of the add-on build.
//   g++ -std=c++17 -O1 -I src tests/fg_limit_test.cpp -o fg_limit_test && ./fg_limit_test
#include "fg_pace.hpp"
#include <cstdio>
#include <random>
#include <vector>
using namespace mgpu::fgpace;
// source: evaluated interval (ms) with jitter; n per pair fixed or by fill; display refresh (ms)
static void run(const char *name, double src_ms, double sigma, double refresh_ms, unsigned n_fixed, bool fill, const std::vector<int> &steps, double game_ms)
{
    std::mt19937 rng(11); std::normal_distribution<double> g(0.0, sigma);
    limit_state L; double t = 0, last = -1; unsigned long long reals = 0, gens = 0, pairs = 0; double T = 0;
    for (int i = 0; i < 20000; ++i)
    {
        int st = steps[i % steps.size()];
        double iv = (fill ? st * game_ms : src_ms) + g(rng); if (iv < 1.0) iv = 1.0;
        t += iv;
        const double span = (last >= 0) ? t - last : 0.0; last = t;
        unsigned n = fill ? fill_count((float)st, 3u) : n_fixed;
        unsigned show = L.allow(span, refresh_ms, n);
        if (i >= 1000) { ++reals; gens += show; ++pairs; T += iv; }
    }
    std::printf("  %-44s evaluated %6.1f/s | shown %6.1f/s (real %5.1f + in-between %5.1f) | display %6.1f Hz | pairs real-only %4.1f%%\n",
                name, 1000.0 * pairs / T, 1000.0 * (reals + gens) / T, 1000.0 * reals / T, 1000.0 * gens / T, 1000.0 / refresh_ms, 0.0);
}
// R395: the display check. A scene = seconds at a neural rate WITHOUT the generator; with it running the rate is
// lower by fg_cost_ms per frame (and per extra in-between frame).
struct scene { double secs, rate_off; };
static void gate(const char *name, double disp_hz, unsigned asked, double fg_cost_ms, const std::vector<scene> &sc)
{
    display_gate G; std::mt19937 rng(3); std::normal_distribution<double> J(0.0, 0.7);
    std::printf("  %-50s", name);
    double t = 0; unsigned last = 99;
    for (auto &s : sc)
    {
        double te = t + s.secs * 1000.0;
        while (t < te)
        {
            const unsigned a = G.allowed(asked);
            double dt = 1000.0 / s.rate_off + (a >= 2u ? fg_cost_ms * (a - 1u) : 0.0) + J(rng); if (dt < 4) dt = 4;
            t += dt; G.step(dt, 1000.0 / disp_hz, asked);
            const unsigned b = G.allowed(asked);
            if (b != last) { std::printf(" %s@%.1fs", b < 2u ? "OFF" : (b == 2u ? "2x" : (b == 3u ? "3x" : "4x")), t / 1000.0); last = b; }
        }
        std::printf(" |");
    }
    std::printf("  off %llu, on %llu, tries failed %llu, lowered %llu, raised %llu\n", G.to_off, G.to_on, G.tries_failed, G.lowered, G.raised);
}
int main()
{
    std::printf("FGDisplayCheck (each | is a scene change; 20 s per scene)\n");
    gate("59 Hz, light: 56/s (L7a, the 60 Hz case)", 59, 2, 2.3, { {20, 56} });
    gate("59 Hz, heavy: 30.8/s off = 28.7 on (L6)", 59, 2, 2.3, { {20, 30.8} });
    gate("59 Hz, light -> heavy -> light -> heavy", 59, 2, 2.3, { {20, 56}, {20, 30.8}, {20, 56}, {20, 30.8} });
    gate("59 Hz, edge: 32.6/s off = 30.3 on (60.6 > 59)", 59, 2, 2.3, { {60, 32.6} });
    gate("59 Hz, edge then 10% slower", 59, 2, 2.3, { {30, 32.6}, {30, 29.0} });
    gate("210 Hz, 4x at 56/s", 210, 4, 2.3, { {20, 56} });
    gate("120 Hz, 4x at 56/s", 120, 4, 2.3, { {20, 56} });
    gate("120 Hz, 2x at 56/s", 120, 2, 2.3, { {20, 56} });
    gate("144 Hz, 2x at 80/s -> 60/s", 144, 2, 1.5, { {20, 80}, {20, 60} });
    gate("59 Hz, 2x at 20/s", 59, 2, 2.3, { {20, 20} });

    const double R59 = 1000.0 / 59.0, R210 = 1000.0 / 210.0, R120 = 1000.0 / 120.0;
    std::printf("FGLimit, fixed multiplier\n");
    run("56/s 2x on 59 Hz (L7a's case)", 17.86, 1.5, R59, 1, false, {1}, 0);
    run("28.7/s 2x on 59 Hz (L6)", 34.9, 1.5, R59, 1, false, {1}, 0);
    run("23/s 3x on 59 Hz (stride + 3x)", 43.0, 3.0, R59, 2, false, {1}, 0);
    run("56/s 4x on 210 Hz (L2)", 19.8, 1.5, R210, 3, false, {1}, 0);
    run("56/s 4x on 120 Hz", 17.9, 1.5, R120, 3, false, {1}, 0);
    run("70/s 2x on 59 Hz (source above the display)", 14.3, 1.0, R59, 1, false, {1}, 0);
    run("56/s 2x, refresh not known", 17.86, 1.5, 0.0, 1, false, {1}, 0);
    std::printf("FGFill (+ FGLimit): game 18.9 ms, steps 2,2,1,2,3,2,2,1,2,2,2 (L6's mix, mean 1.9)\n");
    run("fill on 59 Hz", 0, 1.0, R59, 0, true, {2,2,1,2,3,2,2,1,2,2,2}, 18.9);
    run("fill on 210 Hz", 0, 1.0, R210, 0, true, {2,2,1,2,3,2,2,1,2,2,2}, 18.9);
    std::printf("FGFill: game 14.3 ms (70/s), steps 3,2,3,2 on 59 Hz\n");
    run("fill, game above the display", 0, 1.0, R59, 0, true, {3,2,3,2}, 14.3);
    for (unsigned n = 1; n <= 3; ++n) for (unsigned s = 1; s <= n; ++s) { std::printf("  pick n=%u show=%u:", n, s); for (unsigned k = 0; k < s; ++k) std::printf(" %u", limit_pick(n, s, k)); std::printf("\n"); }
    return 0;
}
