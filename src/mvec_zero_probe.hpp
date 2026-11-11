#pragma once
// ============================================================================
// mvec_zero_probe.hpp - [PORT B1] read-only content statistics for a motion
// vector field.
//
// WHY THIS EXISTS, AND WHAT IT IS NOT.
//
// The bridge can prove a vector FIELD ARRIVED (copies, seals, [R78]) and still
// know nothing about what is IN it: a frame that carries 11 MB of zeros looks
// exactly like a frame that carries motion, in every counter the transport
// keeps. Measured on Cyberpunk 2077 (10-09) and again on a non-NGX title
// (11-01, [R257] full-field 0% zero, [R486] pack 0-5%): the same handle can be
// the real velocity buffer one second and an empty twin the next.
//
// This header is the DECODER and the ARITHMETIC only: it takes a mapped
// readback window and returns zero share, mean |x| / |y| and peak magnitude.
// It holds no D3D12 type, no lock and no state, so it can be unit-tested with
// a byte array and it cannot change a decision. The caller decides what to do
// with the numbers; in B1 the caller only logs them.
//
// FORMATS. The bridge's MV lane is always "two channels, 4 bytes per texel"
// (see the landing-set comment in gpu1_context.cpp). These are the DXGI
// formats that family covers, plus R32G32_FLOAT for completeness:
//     34  R16G16_FLOAT     half x, half y
//     37  R16G16_SNORM     signed 16 / 32767
//     10  R16G16_UNORM     unsigned 16 / 65535
//     16  R32G32_FLOAT     float x, float y
//     41  R32_FLOAT        float x, y assumed 0 (a one-channel field is not a
//                          velocity buffer; it is reported so the caller can see
//                          that the format is wrong rather than read zeros)
//
// ZERO. A texel counts as zero when BOTH components are below 1e-6. "Exactly
// zero" is the wrong test for half floats in practice (a denormal or -0.0
// prints as zero and is zero); 1e-6 is far below one pixel of motion at any
// resolution this bridge runs.
// ============================================================================

#include <cmath>
#include <cstring>
#include <cstdint>

namespace mgpu
{
namespace mzero
{

struct stats
{
    double   zero_pct = 0.0;    // share of sampled texels with |x| and |y| < 1e-6
    double   mean_abs_x = 0.0;
    double   mean_abs_y = 0.0;
    double   max_mag = 0.0;     // max sqrt(x^2 + y^2)
    unsigned nan_count = 0u;
    unsigned n = 0u;            // texels actually decoded
};

inline float half_to_float(std::uint16_t h) noexcept
{
    const unsigned sign = (unsigned)(h >> 15) & 1u;
    const unsigned expo = (unsigned)(h >> 10) & 0x1Fu;
    const unsigned mant = (unsigned)h & 0x3FFu;
    double v;
    if (expo == 0u)
        v = (mant == 0u) ? 0.0 : ((double)mant * (1.0 / 16777216.0));
    else if (expo == 0x1Fu)
        v = (mant == 0u) ? 3.4028235e38 : 0.0;          // inf, and NaN folded to 0 (counted by the caller below)
    else
        v = (1.0 + (double)mant / 1024.0) * std::pow(2.0, (double)((int)expo - 15));
    return (float)(sign ? -v : v);
}

inline bool is_nan_half(std::uint16_t h) noexcept
{
    return ((h >> 10) & 0x1Fu) == 0x1Fu && ((h & 0x3FFu) != 0u);
}

// Decodes one texel. Returns false for a format this probe does not understand,
// which the caller reports rather than silently counting zeros.
inline bool decode2(unsigned fmt, const unsigned char *p, double &x, double &y,
                    bool &nan_seen) noexcept
{
    nan_seen = false;
    switch (fmt)
    {
    case 34u:   // R16G16_FLOAT
    {
        std::uint16_t hx, hy;
        std::memcpy(&hx, p, 2);
        std::memcpy(&hy, p + 2, 2);
        if (is_nan_half(hx) || is_nan_half(hy)) nan_seen = true;
        x = (double)half_to_float(hx);
        y = (double)half_to_float(hy);
        return true;
    }
    case 37u:   // R16G16_SNORM
    {
        std::int16_t sx, sy;
        std::memcpy(&sx, p, 2);
        std::memcpy(&sy, p + 2, 2);
        x = (double)sx / 32767.0;
        y = (double)sy / 32767.0;
        return true;
    }
    case 10u:   // R16G16_UNORM
    {
        std::uint16_t ux, uy;
        std::memcpy(&ux, p, 2);
        std::memcpy(&uy, p + 2, 2);
        x = (double)ux / 65535.0;
        y = (double)uy / 65535.0;
        return true;
    }
    case 16u:   // R32G32_FLOAT
    {
        float fx, fy;
        std::memcpy(&fx, p, 4);
        std::memcpy(&fy, p + 4, 4);
        if (!(fx == fx) || !(fy == fy)) nan_seen = true;
        x = (double)fx;
        y = (double)fy;
        return true;
    }
    case 41u:   // R32_FLOAT - one channel; y is not in the resource at all
    {
        float fx;
        std::memcpy(&fx, p, 4);
        if (!(fx == fx)) nan_seen = true;
        x = (double)fx;
        y = 0.0;
        return true;
    }
    default:
        return false;
    }
}

// width/height are in texels, pitch in bytes. Never reads outside
// [0, pitch*height): the caller passes the mapped size it actually got.
inline stats sample(const unsigned char *base, unsigned pitch, unsigned width, unsigned height,
                    unsigned fmt) noexcept
{
    stats st{};
    if (base == nullptr || pitch == 0u || width == 0u || height == 0u) return st;

    const unsigned bpt = (fmt == 16u) ? 8u : 4u;          // bytes per texel
    if (pitch < width * bpt) return st;                   // a short row is a bad window, not zero content

    double sx = 0.0, sy = 0.0;
    unsigned zeros = 0u;
    for (unsigned y = 0u; y < height; ++y)
    {
        const unsigned char *row = base + (std::size_t)y * pitch;
        for (unsigned x = 0u; x < width; ++x)
        {
            double vx = 0.0, vy = 0.0;
            bool isnan = false;
            if (!decode2(fmt, row + (std::size_t)x * bpt, vx, vy, isnan)) return st;
            if (isnan) ++st.nan_count;
            const double ax = std::fabs(vx), ay = std::fabs(vy);
            if (ax < 1e-6 && ay < 1e-6) ++zeros;
            sx += ax; sy += ay;
            const double mag = std::sqrt(vx * vx + vy * vy);
            if (mag > st.max_mag) st.max_mag = mag;
            ++st.n;
        }
    }
    if (st.n == 0u) return st;
    st.zero_pct = 100.0 * (double)zeros / (double)st.n;
    st.mean_abs_x = sx / (double)st.n;
    st.mean_abs_y = sy / (double)st.n;
    return st;
}

// ---- [PORT O4b, 2026-11-11] THE SAME IDEA FOR ONE-CHANNEL DEPTH ----
//
// A depth field's failure is not "motion too large" but "it is a constant": a
// cleared buffer is a plane of zeros or a plane of ones, and both reproject
// perfectly and tell the model nothing. So this reports zero share AND range
// (min/max) AND mean - a constant plane shows min == max, which no zero test
// can distinguish from a legitimate far-plane reading.
//
// Formats: 41 R32_FLOAT, 40 R32_TYPELESS, 19 D32_FLOAT (4 bytes, read as
// float); 54/55/56 are 16-bit and decoded as unorm for completeness.
struct scalar_stats
{
    double   zero_pct = 0.0;
    double   min_v = 0.0;
    double   max_v = 0.0;
    double   mean = 0.0;
    unsigned nan_count = 0u;
    unsigned n = 0u;
};

inline scalar_stats sample_scalar(const unsigned char *base, unsigned pitch, unsigned width,
                                  unsigned height, unsigned fmt) noexcept
{
    scalar_stats st{};
    if (base == nullptr || pitch == 0u || width == 0u || height == 0u) return st;
    const unsigned bpt = (fmt == 54u || fmt == 55u || fmt == 56u) ? 2u : 4u;
    if (pitch < width * bpt) return st;

    double sum = 0.0, mn = 1e30, mx = -1e30;
    unsigned zeros = 0u;
    for (unsigned y = 0u; y < height; ++y)
    {
        const unsigned char *row = base + (std::size_t)y * pitch;
        for (unsigned x = 0u; x < width; ++x)
        {
            const unsigned char *p = row + (std::size_t)x * bpt;
            double v = 0.0;
            if (bpt == 2u)
            {
                std::uint16_t u;
                std::memcpy(&u, p, 2);
                v = (double)u / 65535.0;
            }
            else if (fmt == 41u || fmt == 40u || fmt == 19u)
            {
                float f;
                std::memcpy(&f, p, 4);
                if (!(f == f)) { ++st.nan_count; ++st.n; continue; }
                v = (double)f;
            }
            else
            {
                return st;   // unknown format: no number rather than a false one
            }
            if (std::fabs(v) < 1e-7) ++zeros;
            if (v < mn) mn = v;
            if (v > mx) mx = v;
            sum += v;
            ++st.n;
        }
    }
    if (st.n == 0u) return st;
    st.zero_pct = 100.0 * (double)zeros / (double)st.n;
    st.min_v = (mn < 1e29) ? mn : 0.0;
    st.max_v = (mx > -1e29) ? mx : 0.0;
    st.mean = sum / (double)st.n;
    return st;
}

} // namespace mzero
} // namespace mgpu
