// mgpu::fgcamera - the camera constants a frame generator needs, from what we have.
//
// WHY THIS EXISTS. FSR-FG only asks for near / far / vertical FOV (and takes no
// matrices), so the bridge never needed a camera. DLSS-G is different: its
// NVSDK_NGX_DLSSG_Parameter_CameraViewToClip is mandatory. We have no camera at
// all - the NGX table carries jitter, MVecScale and Reset but no near/far/FOV,
// and this process never sees Streamline's slSetConstants.
//
// WHAT IT DOES, AND WHAT IT DOES NOT. It reconstructs a right-handed perspective
// projection matrix from near/far/vFov/aspect and leaves the other three
// matrices that DLSS-G takes (ClipToCameraView, ClipToPrevClip, PrevClipToClip)
// to the caller as identity - the same choice OptiScaler makes when it has no
// real camera (DLSSG_Dx12.cpp:475-497). The motion between frames is carried by
// the game's real motion vectors, not by these matrices, so an identity
// clipToPrevClip is a documented approximation, not a stub-by-accident.
//
// Row-major, D3D depth range [0,1], matching NVSDK_NGX_Mat4x4's layout.
//
// Header-only on purpose: adding a .cpp would mean touching CMakeLists.txt, and
// this module has no state that needs a translation unit of its own.

#pragma once

#include <cmath>
#include <cstring>
#include "diag.hpp"

namespace mgpu {
namespace fgcamera {

struct Params
{
    // R479 defaults follow OptiScaler's fallbacks (Config.h:1099-1103): 60 deg
    // vertical FOV, near 0.1, far 100000. They are deliberately the same numbers
    // already used by the FSR-FG path so the two backends see one camera.
    float nearZ = 0.1f;
    float farZ = 100000.0f;
    float vFovRad = 1.0471976f;          // 60 degrees
    unsigned displayW = 0, displayH = 0;
    float aspect = 16.0f / 9.0f;
    float viewToClip[16] = {};           // row-major
    unsigned long long updates = 0;      // how many times the matrix was rebuilt
};

namespace detail { inline Params &st() { static Params p; return p; } }

// Rebuilds the matrix when the extent changes. Cheap: four multiplies.
inline void update(unsigned w, unsigned h)
{
    Params &p = detail::st();
    if (w == 0u || h == 0u) return;
    if (w == p.displayW && h == p.displayH && p.updates != 0ull) return;

    p.displayW = w;
    p.displayH = h;
    p.aspect = (float)w / (float)h;

    const float f = 1.0f / tanf(p.vFovRad * 0.5f);
    for (int i = 0; i < 16; ++i) p.viewToClip[i] = 0.0f;
    p.viewToClip[0]  = f / p.aspect;                     // m00
    p.viewToClip[5]  = f;                                // m11
    p.viewToClip[10] = p.farZ / (p.nearZ - p.farZ);      // m22
    p.viewToClip[11] = -1.0f;                            // m23
    p.viewToClip[14] = (p.nearZ * p.farZ) / (p.nearZ - p.farZ); // m32
    ++p.updates;
}

inline const Params &get() { return detail::st(); }

// One line, rate-limited by the caller's frame counter so a long session cannot
// turn a diagnostic into a log flood. Prints the four scalars and the two matrix
// cells that prove the matrix was built (m00 depends on aspect, m11 on vFov).
inline void log(unsigned long long frame)
{
    const Params &p = detail::st();
    static unsigned long long last = ~0ull;
    static unsigned long long last_updates = ~0ull;
    if (p.updates == 0ull) return;
    if (last != ~0ull && p.updates == last_updates && frame < last + 600ull) return;
    last = frame;
    last_updates = p.updates;

    char l[240];
    snprintf(l, sizeof l,
             "[MGPU][R479] camera: near=%.3f far=%.1f vfov=%.2fdeg aspect=%.4f (%ux%u) "
             "m00=%.4f m11=%.4f rebuilds=%llu src=defaults",
             (double)p.nearZ, (double)p.farZ, (double)(p.vFovRad * 57.2957795f),
             (double)p.aspect, p.displayW, p.displayH,
             (double)p.viewToClip[0], (double)p.viewToClip[5],
             (unsigned long long)p.updates);
    mgpu::diag::info(l);
}

} // namespace fgcamera
} // namespace mgpu
