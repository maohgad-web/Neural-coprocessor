#pragma once

// MGPU Bridge - R396 FrameGen=5: the forward warp (0.5.1 discovery, Experimental, off by default)
//
// WHAT THIS IS FOR
//
// DLSS-G and FSR make the frame BETWEEN two neural frames, so the newest neural frame waits behind the
// generated one. This generator makes frames AFTER the newest neural frame instead. Nothing waits.
//
// The shape is Marcelo's (2026-11-15): the neural card receives game frames faster than it enhances
// them. When neural frame A is done and the card starts on game frame B, B's motion vectors and depth
// are already there. The forward frame is A's neural pixels fetched through B's vectors: for every
// pixel of B, the neural pixel from where B's vector says that surface was. Every pixel shown is a
// neural pixel. It is the fetch the neural model does for its own history.
//
// THE CONTRACT
//   - FrameGen=5 in mgpu.ini, read at arm. This file is entered from fg_gpu1.cpp only on that value.
//   - One compute pass per shown frame, all of a pair's frames made at once when B is taken. They are
//     made on the PRESENT queue, from the pacing thread, so they do not wait behind the neural work
//     and each present is ordered behind its frame without a CPU wait.
//   - B's textures belong to the stream. They are read once, at the take, behind a queue wait on the
//     copy queue's fence for B. Needs the copy queue (CopyQueue=1 or 2): with the inline unpack the
//     same textures rest in a write state and there is no fence to order against.
//   - Where a surface was hidden in A (A's depth at the source is nearer than B's depth here) or the
//     source is outside the picture, the pixel is uncovered. FGWarpFill=1 (default): the game's own
//     pixel of B, scaled by how the neural pixels around it differ from the game's. FGWarpFill=0: the
//     neural pixel of the nearest neighbour that is as far away as this surface.
//   - Never fatal, never blocking: a pair that cannot be made is counted and the picture keeps the
//     neural frame.
//   - Threads: create() / release() on the bridge thread (release after the pacing thread is joined);
//     warp() and out() on the pacing thread only.

#include <windows.h>
#include <d3d12.h>

namespace mgpu::fgfwd
{
struct info
{
    ID3D12Device *dev = nullptr;          // the neural device (borrowed)
    unsigned w = 0, h = 0;                // the neural output extent
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    unsigned n_max = 2;                   // frames per pair the arm asks for at most (2..4)
    bool depth_inverted = true;
};

// The frames of one pair. Every pointer is borrowed for the call.
struct pair
{
    ID3D12Resource *neural = nullptr;     // neural frame A (rests NON_PIXEL_SHADER_RESOURCE)
    ID3D12Resource *mvec = nullptr;       // frame B's vectors
    ID3D12Resource *depth_b = nullptr;    // frame B's depth, or null
    ID3D12Resource *depth_a = nullptr;    // frame A's depth (the module's own copy), or null
    ID3D12Resource *game = nullptr;       // frame B's colour as the model receives it, or null
    float uv_x = 0.0f, uv_y = 0.0f;       // one vector unit in picture widths / heights, sign included
    float el = 1.0f;                      // game frames from A to B
    unsigned n = 2;                       // frames to make: frame k is pushed (k + 1) / n of the way
    int fill = 2;                         // FGWarpFill: 2 none (default), 1 the game's pixel matched, 0 the neural neighbour
    int show = 0;                         // R397 FGWarpShow=1: uncovered pixels are tinted, not filled
    ID3D12Fence *unpack = nullptr;        // the copy queue's fence, or null
    unsigned long long unpack_value = 0;  // its value for frame B
};

bool create(const info &i, char *why, size_t n);
bool warp(const pair &p);                 // false = nothing was made for this pair (counted)
ID3D12Resource *out(unsigned k);          // rests UNORDERED_ACCESS
void summary(char *line, size_t n);
void release();
}
