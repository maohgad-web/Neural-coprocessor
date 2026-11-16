// MGPU Bridge - R380 (0.5.1 discovery): AMD FSR frame generation on the neural card (FrameGen=3).
//
// WHY. The decision of 2026-10-24 was "FSR 3 and NVIDIA" for frame generation on the neural card; R341 built the
// NVIDIA generator only. This is the other one. It is not bound to a vendor: it runs on any neural card.
// Design and first implementation: davidm71 (his fork, FrameGen=3). Written again here against the public AMD
// FSR SDK headers, as its own module, on the 3.1 structure types so that the FSR DLL a game already ships works.
//
// WHAT. FrameGen=3 makes the generator of fg_gpu1.cpp an FSR frame-generation context on the neural card's
// device, created through the FidelityFX API (ffxCreateContext / ffxConfigure / ffxDispatch) of a DLL that is
// ALREADY ON THE MACHINE - the bridge distributes none:
//   1. amd_fidelityfx_dx12.dll if the game has it loaded, else from the game's folder (FSR 3.1.x: the five
//      exports and the frame-generation provider in one file);
//   2. mgpu\amd_fidelityfx_framegeneration_dx12.dll or mgpu\amd_fidelityfx_dx12.dll (a copy the user put there).
// The context runs with FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY: AMD's swap chain is not used. The
// generated frames land in fg_gpu1's own textures and are shown by its pacing thread, so FGPace, FGMulti,
// PresentLatency1, FGInstr and FGCamera apply to both generators. One dispatch makes every in-between frame of
// a pair (up to four in the API, three here).
//
// KEY. FrameGen=3 (was: no effect). FrameGen=4 and every other value: nothing in this file runs.
// Lines: [MGPU][R380].
#pragma once

#include <windows.h>
#include <d3d12.h>

namespace mgpu
{
namespace slcam { struct camera; }
namespace fgfsr
{
struct info
{
    ID3D12Device *ndev = nullptr;            // the neural device (borrowed)
    unsigned w = 0, h = 0;                   // the picture: generated and real frames
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    unsigned mvec_w = 0, mvec_h = 0;         // the vector resource = FSR's render size
    unsigned depth_w = 0, depth_h = 0;
    bool depth_inverted = true;
    float mvec_scale_x = 1.0f, mvec_scale_y = 1.0f;   // the lane's factor: raw vector -> pixels at the vector resource
    float fov_h_deg = 90.0f, cam_near = 0.1f, cam_far = 10000.0f;   // FGFov / FGNear / FGFar (the game's, when FGCamera has them)
    bool invert = false;                     // FGInvertMV
};

// Bridge thread, first frame after the arm. False = the generator cannot start; `why` says it.
bool create(const info &i, char *why, size_t why_n);
// One pair: every in-between frame into outs[0 .. n-1] (each resting UNORDERED_ACCESS). `real`, `mvec`, `depth`
// rest NON_PIXEL_SHADER_RESOURCE. Returns the FidelityFX return code: 0 = ok.
unsigned evaluate(ID3D12GraphicsCommandList *cl, ID3D12Resource *real, ID3D12Resource *mvec, ID3D12Resource *depth,
                  ID3D12Resource *const *outs, unsigned n, float mv_el, bool reset, const mgpu::slcam::camera *cam);
void set_scale(float x, float y);   // R397: the vector scale for the next evaluate
void release();
}   // namespace fgfsr
}   // namespace mgpu
