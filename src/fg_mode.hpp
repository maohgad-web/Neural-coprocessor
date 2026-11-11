// mgpu::fgmode - which frame generator owns the output, and at what multiplier.
//
// WHY ONE VALUE AND NOT TWO. The multiplier is a property OF THE BACKEND, not an
// independent knob: FSR-FG's dispatch API has no multi-frame field at all (one call,
// one interpolated frame), so "FSR x3" is not a setting that exists - it is a
// contradiction. Keeping the two in one atomic makes that contradiction
// unrepresentable, instead of leaving a rule that another call site has to remember.
// Selecting FSR (or OFF) therefore collapses the multiplier to 1 (2x) by itself.
//
// SECOND REASON THE MULTIPLIER IS NOT FREE: f_src <= f_disp / N. At 60 source frames
// on a 120 Hz panel, N=2 asks for 180 presents/s that do not exist, and the measured
// failure is not a bad picture - it is the display ring filling, the display thread
// stalling, and the display layer being disabled for the rest of the session
// (2026-10-30, 1554 ms stall). The runtime budget clamp in gpu1_context.cpp is the
// guard; this file only stores the request.
//
// Encoding: backend * 4 + multi, multi = GENERATED frames per real frame
// (1 = 2x, 2 = 3x, 3 = 4x). Header-only, like fg_camera.hpp: one atomic int is not
// worth a translation unit.
#pragma once

#include <atomic>

namespace mgpu
{
namespace fgmode
{
enum : int { OFF = 0, FSR = 1, DLSSG = 2, XEFG = 3 };
enum : int { MULTI_MAX = 3 };

inline std::atomic<int> &var()
{
    static std::atomic<int> v{ FSR * 4 + 1 };   // default = today's behaviour: FSR-FG 2x
    return v;
}

inline int encode(int backend, int multi)
{
    // FSR-FG cannot do multi-frame, and OFF generates nothing: both are 1 by
    // definition. This is the guard, so no caller has to remember it.
    //
    // XeFG USED TO BE IN THIS LIST and is deliberately out of it now: measured
    // 2026-10-31 00:08 with the provider's five sites patched, one Present put
    // FOUR frames on screen (7191 frames / 1800 presents = 3.995, composite
    // 160.1 Hz at a 160 Hz panel). The multiplier is the panel's business again,
    // and 1 here would be the same self-imposed ceiling that made the unlock
    // unusable even when it worked.
    if (backend == FSR || backend == OFF) multi = 1;
    if (multi < 1) multi = 1;
    if (multi > MULTI_MAX) multi = MULTI_MAX;
    return backend * 4 + multi;
}

inline int get()        { return var().load(std::memory_order_relaxed) / 4; }
inline int get_multi()  { const int m = var().load(std::memory_order_relaxed) % 4; return (m < 1) ? 1 : m; }

inline void set(int backend, int multi) { var().store(encode(backend, multi), std::memory_order_relaxed); }
inline void set(int backend)            { set(backend, 1); }

inline const char *name(int v)
{
    return (v == OFF) ? "off" : (v == FSR) ? "fsr" : (v == DLSSG) ? "dlssg" : (v == XEFG) ? "xefg" : "?";
}

// LIVE RATE COUNTERS - so the panel can print "40 / 120" (source frames per second /
// frames actually handed to the display per second) while a multiplier is tested.
//
// Deliberately counters, not rates: the render loop increments, the panel subtracts
// with its own clock. A rate computed by the producer would be stale by the time it
// is drawn, and the two run at completely different rates.
inline std::atomic<unsigned long long> &src_count()
{
    static std::atomic<unsigned long long> v{ 0 };
    return v;
}
inline std::atomic<unsigned long long> &out_count()
{
    static std::atomic<unsigned long long> v{ 0 };
    return v;
}

// 0 = off, 1 = the rate readout is drawn on the neural picture instead of in the panel.
inline std::atomic<int> &overlay()
{
    static std::atomic<int> v{ 0 };
    return v;
}

// Ctrl+Alt+G: generation ON/OFF, and it is the SAME state the panel's "off" line
// writes. One off, reachable from two places - never two offs. Turning it back on
// restores the last (backend, multiplier) pair, because "off then on" must not
// silently demote a 3x session to 2x.
inline int toggle_off()
{
    static int last_b = FSR, last_m = 1;   // one instance program-wide (inline fn)
    const int cur = var().load(std::memory_order_relaxed);
    const int b = cur / 4;
    if (b == OFF)
    {
        var().store(encode(last_b, last_m), std::memory_order_relaxed);
        return last_b;
    }
    last_b = b;
    const int m = cur % 4;
    last_m = (m < 1) ? 1 : m;
    var().store(encode(OFF, 1), std::memory_order_relaxed);
    return OFF;
}

// Ctrl+Alt+B: FSR-FG <-> DLSS-G, always at 2x. The multiplier is chosen on the panel
// (or a future hotkey of its own); cycling backends must not silently carry a 3x
// request into a backend that cannot do it.
inline int cycle()
{
    const int n = (get() == FSR) ? DLSSG : FSR;
    set(n, 1);
    return n;
}
}   // namespace fgmode
}   // namespace mgpu
