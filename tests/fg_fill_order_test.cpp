// R391b: frame-generation presentation rules against a simulated source. Not part of the add-on build.
//   g++ -std=c++17 -O2 -I src tests/fg_fill_order_test.cpp -o t && ./t
// (This file replaced the R391 / R391a simulation, whose source model was wrong: it had the hand-overs follow the
//  scene time, which made real frames land on each other. The L6 log shows evenly spaced hand-overs.)
// Fill-rule search, 2026-11-15. Source model = what the L6 log shows: the bridge is a free-running consumer.
// The game seals a frame every g ms; the neural card takes the NEWEST arrived frame when it is free (the ring),
// evaluates for E ms, hands over. Hand-overs are then about E apart (L6: 34.9 ms, sd 1.06) while the game
// frames between them vary (L6 [R87]: 10% one, 82% two, 8% three).
// Each policy turns hand-overs into presents. Judged on what is ON SCREEN over time:
//   age(t) = t - scene time of the frame on screen.  mean = how old the picture is (latency);  sd = how unevenly
//   the picture advances (judder).  Two displays: 59 Hz (frame latched at each vblank) and continuous.
#include "fg_pace.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <random>
#include <string>
#include <vector>
struct pair_t { double t, scene, prev_scene, age; int el; };
struct pres { double t, scene; bool real; };
static std::vector<pair_t> source(double g, double sg, double E, double sE, int n, int *hist)
{
    std::mt19937 rng(5); std::normal_distribution<double> N(0, 1);
    std::vector<double> seal; double s = 0; for (int i = 0; i < n * 4 + 100; ++i) { double d = g + sg * N(rng); if (d < 4) d = 4; s += d; seal.push_back(s); }
    const double X = 6.0;   // transfer
    std::vector<pair_t> out; int last = 0; double T = seal[0] + X, prev_scene = seal[0];
    while ((int)out.size() < n)
    {
        int i = last; while (i + 1 < (int)seal.size() && seal[i + 1] + X <= T) ++i;   // newest arrived
        if (i == last) { i = last + 1; T = seal[i] + X; }                                // none newer: wait for the next
        double d = E + sE * N(rng); if (d < 1) d = 1;
        T += d;
        const int el = i - last; hist[el < 4 ? el : 4]++;
        out.push_back({ T, seal[i], prev_scene, T - seal[i], el });
        prev_scene = seal[i]; last = i;
    }
    return out;
}
struct pol { const char *name; int id; double f; };
static std::vector<pres> run(const std::vector<pair_t> &P, const pol &po, double g, double *hold_mean, double *hold_max, double *lost)
{
    std::vector<pres> v; double busy = 0, ema = 0, prev_t = 0, hs = 0, hm = 0; std::deque<double> ages; unsigned long long nl = 0;
    double last_present = -1e9;
    auto show = [&](double t, double sc, bool real) { if (t < busy) t = busy; if (t - last_present < 1.0 && !v.empty()) { if (v.back().real) ++nl; } v.push_back({ t, sc, real }); busy = t; last_present = t; return t; };
    for (size_t k = 0; k < P.size(); ++k)
    {
        const pair_t &p = P[k];
        const double dt = p.t - prev_t; prev_t = p.t; if (k) ema = ema <= 0 ? dt : ema * 0.85 + dt * 0.15;
        const double half = (ema > 0 ? ema : 16.0) * 0.5;
        ages.push_back(p.age); if (ages.size() > 16) ages.pop_front();
        const double c = *std::max_element(ages.begin(), ages.end());
        const double mid = (p.prev_scene + p.scene) * 0.5, fillsc = p.scene - g;   // fill: the game frame before this one
        double rt = p.t;
        switch (po.id)
        {
        case 0: show(p.t, mid, false); rt = show(p.t + po.f * 2.0 * half, p.scene, true); break;            // landed (f = 0.5), or another share of the interval
        case 1: case 2: {   // FGFill=1 / 2 as built (R391b): fill_fit, start at the hand-over, hold <= the landed rule's
                  unsigned want = (unsigned)p.el - 1u; if (want > 3u) want = 3u; if (po.id == 2 && want < 1u) want = 1u;
                  const double step = g * p.el / (double)(want + 1u);
                  const unsigned fit = mgpu::fgpace::fill_fit(half, step, want);
                  double hold = step * fit; if (hold > half) hold = half;
                  for (unsigned q = 0; q < fit; ++q) show(p.t + (fit ? hold / fit : 0.0) * q, p.scene - step * (double)(fit - q), false);
                  rt = show(p.t + hold, p.scene, true); (void)fillsc; } break;
        case 3: rt = show(p.t, p.scene, true); break;                                                          // real frames alone, at arrival
        case 4: { double h = std::min(std::max(c - p.age, 0.0), half); rt = show(p.t + h, p.scene, true); } break;   // real frames alone, ages levelled inside the landed hold
        case 5: { double h = std::min(std::max(c - p.age, 0.0), half);                                          // ages levelled + the mid frame when there is room
                  if (h >= po.f * g) show(p.t, mid, false); rt = show(p.t + h, p.scene, true); } break;
        case 6: { // landed's two frames per pair, the hold of each pair moved by its age: old frame -> shorter hold, fresh -> landed's
                  double h = std::min(std::max(half - (c - p.age < 0 ? 0 : (c - p.age)) * 0.0 - (0.0), 0.0), half);
                  // hold = half - (how much OLDER than the freshest recent frame this one is), never below f*half
                  double mn = *std::min_element(ages.begin(), ages.end()); h = half - (p.age - mn); if (h < po.f * half) h = po.f * half;
                  // the in-between drawn where the screen time says: (hold) before the real frame, in scene time
                  double sc = p.scene - h; if (sc < p.prev_scene) sc = p.prev_scene + 0.25 * (p.scene - p.prev_scene);
                  show(p.t, sc, false); rt = show(p.t + h, p.scene, true); } break;
        case 9: { rt = show(p.t, p.scene, true); const double d = po.f * 2.0 * half; show(p.t + d, p.scene + d, false); } break;   // forward fill: real at arrival, one frame pushed forward
        case 7: { // as 6 but the in-between stays the mid frame (what one DLSS-G evaluate makes today)
                  double mn = *std::min_element(ages.begin(), ages.end()); double h = half - (p.age - mn); if (h < po.f * half) h = po.f * half;
                  show(p.t, mid, false); rt = show(p.t + h, p.scene, true); } break;
        }
        const double h = rt - p.t; hs += h; if (h > hm) hm = h;
    }
    *hold_mean = hs / P.size(); *hold_max = hm; *lost = (double)nl;
    return v;
}
static void judge(const std::vector<pres> &v, double refresh, double *mean, double *sd, double *fps)
{
    const double t0 = v[200].t, t1 = v[v.size() - 200].t; size_t j = 0; double m = 0, m2 = 0; unsigned long long n = 0, uniq = 0; double on = -1; double vb = t0;
    double cur_scene = v[0].scene; size_t cur = 0;
    for (double t = t0; t < t1; t += 0.5)
    {
        if (refresh > 0) { if (t >= vb) { while (j + 1 < v.size() && v[j + 1].t <= vb) ++j; if ((double)j != on) { ++uniq; on = (double)j; } cur_scene = v[j].scene; vb += refresh; } }
        else { while (j + 1 < v.size() && v[j + 1].t <= t) ++j; if (j != cur) { ++uniq; cur = j; } cur_scene = v[j].scene; }
        const double a = t - cur_scene; m += a; m2 += a * a; ++n;
    }
    m /= n; *mean = m; *sd = std::sqrt(m2 / n - m * m); *fps = 1000.0 * uniq / (t1 - t0);
}
int main()
{
    struct { const char *t; double g, sg, E, sE; } S[] = {
        { "HEAVY (L6): game 55/s, evaluate 34.9 ms", 18.2, 1.5, 34.9, 1.0 },
        { "HEAVY, rougher (more steps of three)", 18.2, 3.0, 34.9, 2.5 },
        { "MIDDLE (L6 02:43:45-02:44:26): game 57/s, evaluate 26 ms", 17.5, 1.5, 26.0, 1.0 },
        { "LIGHT (L7c): game 56/s, evaluate 10 ms", 17.9, 1.5, 10.0, 1.0 } };
    pol PO[] = { { "landed (FGPace=1, 2x)", 0, 0.5 }, { "real frames alone, at arrival (FG off)", 3, 0 },
                 { "landed, hold a quarter interval", 0, 0.25 },
                 { "FGFill=1 (R391b)", 1, 0 }, { "FGFill=2, FGMulti=2 (R391b)", 2, 0 },
                 { "real alone, ages levelled", 4, 0 }, { "ages levelled + mid frame if room", 5, 0.5 },
                 { "landed, hold follows the age, own phase", 6, 0.25 }, { "landed, hold follows the age, mid frame", 7, 0.25 },
                 { "landed, hold follows the age (floor 0), mid", 7, 0.0 },
                 { "FORWARD fill: real at arrival + 1 forward", 9, 0.5 } };
    for (auto &s : S)
    {
        int hist[5] = {}; auto P = source(s.g, s.sg, s.E, s.sE, 6000, hist);
        double dts = 0; for (size_t k = 1; k < P.size(); ++k) dts += P[k].t - P[k - 1].t;
        std::printf("%s -> hand-over %.1f ms, steps 1:%d%% 2:%d%% 3:%d%% 4+:%d%%\n", s.t, dts / (P.size() - 1), hist[1] * 100 / 6000, hist[2] * 100 / 6000, hist[3] * 100 / 6000, hist[4] * 100 / 6000);
        std::printf("  %-44s | real held mean/max | real lost/s | 59 Hz: shown/s  picture age  uneven | continuous: shown/s  age  uneven\n", "");
        for (auto &po : PO)
        {
            double hm, hx, lost; auto v = run(P, po, s.g, &hm, &hx, &lost);
            double m1, s1, f1, m2, s2, f2; judge(v, 1000.0 / 59.0, &m1, &s1, &f1); judge(v, 0, &m2, &s2, &f2);
            std::printf("  %-44s | %5.1f / %5.1f ms    | %5.1f       | %5.1f   %5.1f ms   %5.2f ms | %5.1f   %5.1f ms   %5.2f ms\n", po.name, hm, hx,
                        lost * 1000.0 / (v.back().t - v.front().t), f1, m1, s1, f2, m2, s2);
        }
    }
}
