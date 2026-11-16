// MGPU Bridge - R372 (0.5.1 discovery): pacing rules for FrameGen=4, as pure logic.
//
// WHY A HEADER WITH NO WINDOWS AND NO D3D IN IT. The rules below decide WHEN each frame of
// a pair is shown. They are arithmetic on one clock, so they are written against three
// callbacks (now, wait_until, present) and can be run against a simulated game on any
// compiler: tests/fg_pace_test.cpp does that. fg_gpu1.cpp supplies the real callbacks.
//
// All times are MILLISECONDS on the caller's clock. A "pair" is the frames one real frame
// brings: n_gen in-between frames (index 0..n_gen-1, in scene order) and then the real one
// (index n_gen).
//
// THE RULES (key FGPace; chosen by the caller):
//   pair_relative  the 0.5.1 rule generalised to n in-between frames: frame i at
//                  t0 + i*step, the real frame at t0 + n*step. With n = 1 and
//                  step = half the hand-over interval this IS the 0.5.1 schedule; with
//                  n > 1 it is the per-pair rule of davidm71's fork (DLSSG2) and of
//                  N01950's PR (relative mode).
//   pair_clock     one tick grid; for each tick the frame nearest in SCENE time is shown.
//                  The grid FOLLOWS the real frames (slow average of the delay between a
//                  frame's scene time and its hand-over, plus a cushion), so a late tick
//                  carries on from where it is instead of restarting. Design: davidm71,
//                  fork branch fps-hud, src/fg_clock.hpp (FC1, MIT, derived from this
//                  project). Rewritten here against this module's types; credit Dave.
//   pair_absolute  a clock anchored at the first pair that advances by the source interval
//                  and re-anchors when a pair is more than one interval late. Design:
//                  N01950's PR "non-native frame generation" (DisplayPace). COMPARE ONLY:
//                  its phase against the hand-overs is not controlled (it depends on how the
//                  source interval moved since the anchor), which is what the test shows.
//
// scene_clock estimates each real frame's SCENE time from what the stream already knows:
// how many game frames passed since the previous evaluated frame (the R87 count the
// consumer hands fg::record). A real frame that is 3 game frames after the last one is
// placed 3 units later than one that is 2 after - so on a source the neural card samples
// unevenly, pair_clock shows motion at the right speed.
#pragma once

#include <cmath>

namespace mgpu
{
namespace fgpace
{

struct scene_clock
{
    double c_last = 0.0;      // scene time of the previous real frame
    double ema_dt = 0.0;      // hand-over interval, slow
    double ema_el = 0.0;      // elapsed game frames per evaluated frame, slow
    bool   have = false;
    void reset() { have = false; c_last = 0.0; ema_dt = 0.0; ema_el = 0.0; }

    // t0: this hand-over; dt_ms: time since the previous hand-over (<= 0 = unknown);
    // el: game frames since the previous evaluated frame (>= 1; R87's count).
    // Out: c = this real frame's scene time, span = scene time since the previous one.
    void next(double t0, double dt_ms, double el, double &c, double &span)
    {
        if (!(el >= 1.0)) el = 1.0;
        if (el > 16.0) el = 16.0;
        const bool usable = (dt_ms > 1.0 && dt_ms < 250.0);
        if (!have || !usable)
        {
            // first pair, or a stall (pause, loading): start again from the hand-over
            ema_dt = usable ? dt_ms : (ema_dt > 0.0 ? ema_dt : 16.0);
            ema_el = el;
            span = ema_dt;
            c = t0;
            c_last = c;
            have = true;
            return;
        }
        ema_dt += (dt_ms - ema_dt) / 16.0;
        ema_el += (el - ema_el) / 16.0;
        const double per_game_frame = (ema_el > 0.0) ? ema_dt / ema_el : ema_dt;
        span = el * per_game_frame;
        c = c_last + span;
        // Never let the estimate run ahead of the hand-over that carries the frame: a frame
        // cannot have been rendered after it was handed over.
        if (c > t0) c = t0;
        c_last = c;
    }
};

template <class Wait, class Present>
bool pair_relative(double t0, double step, unsigned n_gen, Wait wait_until, Present present)
{
    for (unsigned i = 0; i < n_gen; ++i)
    {
        if (i > 0u && step > 0.0) wait_until(t0 + step * (double)i);
        if (!present(i)) return false;
    }
    if (n_gen > 0u && step > 0.0) wait_until(t0 + step * (double)n_gen);
    return present(n_gen);
}

struct clock_state
{
    double next_tick = 0.0;   // when the next frame is due
    double L = 0.0;           // scene time -> display time, followed slowly
    double jit = 0.0;         // how much the hand-over wobbles around L
    bool   have = false;
    unsigned long long shown = 0, late = 0, gaps = 0, skipped = 0, rebases = 0;
    double err_sum = 0.0;     // |scene time shown - scene time wanted|
    double cushion = 0.0;
    void reset() { have = false; next_tick = 0.0; L = 0.0; jit = 0.0; }
    void clear_counts() { shown = late = gaps = skipped = rebases = 0; err_sum = 0.0; }
};

// c[0..M-1]: scene times in order, c[M-1] the real frame's. span: scene time since the
// previous real frame. t0: the hand-over. P: the tick. Every frame index that is shown goes
// through present(j); the real frame (M-1) is always shown. Returns false if a present failed.
template <class Now, class Wait, class Present>
bool pair_clock(clock_state &st, double P, double t0, double span, const double *c, unsigned M,
                Now now, Wait wait_until, Present present)
{
    if (M == 0u || !(P > 0.0)) return true;
    const unsigned N = M - 1u;
    // The delay the per-pair schedule would have for the first frame of this pair.
    const double base = (t0 - c[M - 1u]) + span * (double)N / (double)(N + 1u);
    if (!st.have) { st.L = base; st.jit = P * 0.25; }
    else
    {
        const double d = base - st.L;
        st.L += d / 8.0;
        st.jit += (std::fabs(d) - st.jit) / 16.0;
    }
    double cushion = 2.0 * st.jit;
    if (cushion < P * 0.5) cushion = P * 0.5;
    if (cushion > P * 2.0) cushion = P * 2.0;
    st.cushion = cushion;
    const double Lc = st.L + cushion;

    double t = now();
    const double first_ideal = c[0] + Lc;
    const double start = (t > first_ideal) ? t : first_ideal;
    if (!st.have || st.next_tick < t - 4.0 * P || st.next_tick > start + 4.0 * P)
    {
        if (st.have) ++st.rebases;   // after a stall: start the grid again
        st.next_tick = start;
        st.have = true;
    }

    unsigned idx = 0u;
    int empty = 0;
    while (idx < M)
    {
        t = now();
        if (t > st.next_tick + P * 0.5) { ++st.late; st.next_tick = t; }   // late: carry on from now
        const double want = st.next_tick - Lc;
        if (c[idx] - want > P * 0.5)
        {
            // nothing to show yet for this tick: leave it empty (bounded)
            ++st.gaps;
            st.next_tick += P;
            if (++empty > 8) { st.next_tick = c[idx] + Lc; empty = 0; ++st.rebases; }
            continue;
        }
        empty = 0;
        unsigned j = idx;   // the frame nearest to what this tick should show, never back in time
        while (j + 1u < M && std::fabs(c[j + 1u] - want) <= std::fabs(c[j] - want)) ++j;
        wait_until(st.next_tick);
        if (!present(j)) return false;
        st.err_sum += std::fabs(c[j] - want);
        st.skipped += (unsigned long long)(j - idx);
        ++st.shown;
        idx = j + 1u;
        st.next_tick += P;
    }
    return true;
}

struct abs_state
{
    double clock = 0.0;
    bool   have = false;
    unsigned long long pairs = 0, resync = 0;
    void reset() { have = false; clock = 0.0; }
    void clear_counts() { pairs = 0; resync = 0; }
};

template <class Now, class Wait, class Present>
bool pair_absolute(abs_state &st, double src, unsigned n_gen, Now now, Wait wait_until, Present present)
{
    const double t = now();
    if (!st.have) { st.clock = t; st.have = true; }
    if (src > 0.0 && t > st.clock + src) { st.clock = t; ++st.resync; }
    const double real_t = st.clock;
    ++st.pairs;
    for (unsigned i = 0; i < n_gen; ++i)
    {
        wait_until(real_t - src * (double)(n_gen - i) / (double)(n_gen + 1u));
        if (!present(i)) return false;
    }
    wait_until(real_t);
    if (!present(n_gen)) return false;
    st.clock += src;
    return true;
}

// ---- R390 (FGLimit=1): never ask the display for more frames than it shows ----
// Marcelo's shape (2026-11-15): the REAL frame of every pair is always shown; an in-between frame is shown only
// when the display has a free refresh slot for it. Nothing is switched off: at 56 evaluated frames/s on a 59 Hz
// display the result is 56 real + about 3 in-between frames per second.
// One count of display slots: each pair adds the slots that passed since the previous pair (its hand-over
// interval / the refresh period); the real frame spends one, each in-between frame shown spends one. Both numbers
// are measured on the machine (hand-over interval, the display's own period), nothing is tuned to a rig.
struct limit_state
{
    double slots = 0.0;                 // display slots not yet used (may be a fraction; never banks more than one)
    unsigned long long pairs = 0, gen_shown = 0, gen_held = 0, real_only = 0;
    void reset() { slots = 0.0; }
    void clear_counts() { pairs = gen_shown = gen_held = real_only = 0; }
    // span_ms: time since the previous pair was handed over. refresh_ms: the display period (<= 0 = not known:
    // nothing is held back). n_gen: in-between frames this pair has. Returns how many of them to show.
    unsigned allow(double span_ms, double refresh_ms, unsigned n_gen)
    {
        ++pairs;
        if (!(refresh_ms > 0.0) || !(span_ms > 0.0)) { gen_shown += n_gen; return n_gen; }
        double add = span_ms / refresh_ms;
        if (add > 8.0) add = 8.0;                       // a stall is not a store of slots
        slots += add;
        slots -= 1.0;                                   // the real frame, always
        unsigned show = 0;
        while (show < n_gen && slots >= 1.0) { slots -= 1.0; ++show; }
        if (slots > 1.0) slots = 1.0;                   // one spare slot at most is carried to the next pair
        if (slots < -1.0) slots = -1.0;                 // a source faster than the display does not run up a debt
        gen_shown += show; gen_held += (n_gen - show);
        if (show == 0u && n_gen != 0u) ++real_only;
        return show;
    }
};

// Which of a pair's n in-between frames (0-based, in time order) is the k-th of `show` shown ones: spread evenly,
// so one of three is the middle one and two of three are the outer two.
inline unsigned limit_pick(unsigned n, unsigned show, unsigned k)
{
    if (n == 0u || show == 0u) return 0u;
    if (show >= n) return (k < n) ? k : n - 1u;
    unsigned i = (unsigned)(((double)k + 0.5) * (double)n / (double)show);
    return (i < n) ? i : n - 1u;
}

// ---- R391 (FGFill=1): in-between frames = the game frames the evaluated frame skipped ----
// `el` is the count the stream already hands fg::record: game frames since the previous evaluated frame (R87).
// A step of 1 has nothing to fill, 2 gets one frame at 1/2, 3 gets two at 1/3 and 2/3. The screen then shows one
// frame per GAME frame and every evaluated frame is kept - no stride, the game is never asked to slow down.
inline unsigned fill_count(float el, unsigned max_gen)
{
    if (!(el >= 1.0f)) return 0u;
    unsigned steps = (unsigned)(el + 0.5f);
    if (steps < 1u) steps = 1u;
    unsigned n = steps - 1u;
    return (n > max_gen) ? max_gen : n;
}


// ---- R391b (FGFill): how many of a pair's in-between frames fit before its real frame ----
// The pair starts at its hand-over (a new neural frame is never made to wait for the pair before it) and its real
// frame is shown no later than cap_ms after the hand-over - the landed rule's own hold, half the hand-over interval.
// The frames are step_ms apart; the ones that do not fit are not shown, the oldest first.
// History: R391 / R391a delayed a pair's start to keep frames in order ("start later"); taken out, Marcelo 2026-11-15.
inline unsigned fill_fit(double cap_ms, double step_ms, unsigned n)
{
    if (n == 0u || !(step_ms > 0.0) || !(cap_ms > 0.0)) return n;
    const double fit = std::floor(cap_ms / step_ms + 0.5);
    return (fit <= 0.0) ? 0u : ((fit >= (double)n) ? n : (unsigned)fit);
}

// ---- R395 (FGDisplayCheck=1): frame generation only while the display can show its frames ----
// Marcelo 2026-11-15 04:14: when the display cannot show 2x, frame generation goes OFF (real frames only) - "I would
// rather it read as broken than people see the stutter" (the 60 Hz case, L7a). The limiter (R390) was the other
// answer; simulated and judged worse.
// Fed once per EVALUATED neural frame (also the ones whose set was busy: those are the 60 Hz case's own symptom, so
// the hand-over rate would hide it). Decides every 90 frames (the first time after 45) and holds:
//   on  -> lower the multiplier while (multiplier x neural rate) is more than 3% above the display; below 2x = OFF.
//          One step up when the next multiplier has 5% of room.
//   off -> the neural rate is now WITHOUT the generator's own cost, so "2x fits within 10%" is only a reason to
//          TRY: switch on, and the next decision (made on the rate with the generator running) keeps it or not.
//          A try that fails is not repeated until the neural rate has moved by more than 10%.
// refresh_ms 0 (not known) or asked < 2: nothing is decided, m stays as it is.
struct display_gate
{
    unsigned m = 0u;            // 0 = not decided (as asked), 1 = frame generation off, 2.. = this multiplier at most
    double avg_ms = 0.0;        // evaluated-frame interval, followed slowly
    unsigned n = 0u, first = 45u;
    bool trying = false;        // switched on from off; the next decision confirms it
    double tried_rate = 0.0;    // neural rate (off) of the last try that FAILED (0 = none)
    double try_rate = 0.0;
    unsigned long long to_off = 0, to_on = 0, lowered = 0, raised = 0, tries_failed = 0;
    void reset() { m = 0u; avg_ms = 0.0; n = 0u; first = 45u; trying = false; tried_rate = 0.0; try_rate = 0.0; }
    void clear_counts() { to_off = to_on = lowered = raised = tries_failed = 0; }
    unsigned allowed(unsigned asked) const { return (m == 0u || m > asked) ? asked : m; }   // 1 = off
    double rate() const { return (avg_ms > 0.0) ? 1000.0 / avg_ms : 0.0; }
    // true when the decision changed
    bool step(double dt_ms, double refresh_ms, unsigned asked)
    {
        if (dt_ms > 3.0 && dt_ms < 250.0) avg_ms = (avg_ms <= 0.0) ? dt_ms : avg_ms + (dt_ms - avg_ms) / 32.0;
        if (++n < first) return false;
        n = 0u; first = 90u;
        if (!(avg_ms > 0.0) || !(refresh_ms > 0.0) || asked < 2u) return false;
        const double r = 1000.0 / avg_ms, disp = 1000.0 / refresh_ms;
        const unsigned cur = allowed(asked);
        unsigned next = cur;
        if (cur >= 2u)
        {
            while (next >= 2u && (double)next * r > disp * 1.03) --next;
            if (next == cur && cur < asked && (double)(cur + 1u) * r <= disp * 0.95) next = cur + 1u;
            if (next < 2u)
            {
                next = 1u; ++to_off;
                if (trying) { tried_rate = try_rate; ++tries_failed; }
            }
            else if (next < cur) ++lowered;
            else if (next > cur) ++raised;
            if (trying && next >= 2u) tried_rate = 0.0;   // the try held
            trying = false;
        }
        else
        {
            const bool moved = tried_rate <= 0.0 || std::fabs(r - tried_rate) > 0.10 * tried_rate;
            if (2.0 * r <= disp * 1.10 && moved) { next = 2u; trying = true; try_rate = r; ++to_on; }
        }
        if (next == cur) return false;
        m = next;
        return true;
    }
};

}   // namespace fgpace
}   // namespace mgpu
