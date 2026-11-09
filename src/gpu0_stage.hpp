#pragma once

// MGPU Bridge - R333 StageCopy: GPU 0 async staging (0.5.0 discovery, off by default)
//
// WHAT THIS IS FOR
//
// Without it the GAME's own command list copies colour, depth and the vectors
// straight into gxfer, the cross-adapter heap in system memory: every byte
// crosses the render card's PCIe link inside the game's frame, on the game's
// graphics queue, and the back buffer cannot return to RENDER_TARGET until the
// colour copy is done. At 2560x1440 that is ~44 MB per frame, ~55 MB at 4K.
//
// With it the game's list copies into a VRAM staging ring on GPU 0 with the SAME
// slot layout (a VRAM-to-VRAM copy, a fraction of a millisecond), and a COPY
// queue we create on the game's device moves the whole slot to gxfer on the DMA
// engine while the game renders its next frame. gfence (the produced-count
// fence) is then signalled by that copy queue instead of the game's queue, so
// its meaning for GPU 1 is unchanged: frame f's slot is complete in gxfer. The
// seal protocol, the consumer and the cross-adapter heap are not touched.
//
// Design: Dave (davidm71), OPT5 in his fork (MIT, permission given 2026-10-21),
// measured on his rig: in-frame copies on the game's queue 3.40 -> 0.49 ms at
// 1440p, DMA 5.5 ms off the game's queue, 6,005 slots, 0 failures, +6.8% game
// fps. This is our implementation of that design in its own file, following the
// nr16 / residual rule: no gpu1_context type in here, D3D12 objects and plain
// values are handed in.
//
// THE CONTRACT
//   - Key StageCopy=1 in mgpu.ini, read at arm. Absent or 0: nothing is built,
//     dst() returns gxfer, signal() is exactly gq->Signal(gfence, f). Byte-identical.
//   - NEVER FATAL. Any failure to come up turns the key off for the run, loudly,
//     and the shipped path runs.
//   - Back-pressure is Dave's, kept as he had it: after handing frame f to the
//     copy queue the game's queue is told to Wait - ON THE GPU, never the CPU -
//     for gfence f+2-RING, so stage slot k is not rewritten before its DMA has
//     left it. Satisfied RING-2 frames in advance in a healthy run; when it is
//     not, the DMA engine is that far behind and throttling the producer is the
//     same back-pressure the ring already relies on. Logged as a count.
//   - Costs when on: RING x slot bytes of GPU 0 VRAM (~180 MB at 1440p, ~400 MB
//     at 4K for RING=6), one copy queue, RING prerecorded copy lists.
//   - Instrument: our Gpu0Timers=1 (R300h) already times the in-frame copies on
//     the game's queue; this module adds the DMA time from copy-queue timestamps
//     (when the device supports them) and prints one [R333] line every 1200
//     frames handed over: slots staged, failures, DMA ms, back-pressure waits.
//   - Threads: arm() and release() on the bridge thread (stream arm / release,
//     under s.cs); dst() and signal() on the game thread, under s.cs (the same
//     sites that wrote gxfer and signalled gfence). stream_mvec_copy's dst() call
//     runs under mvec_cs on the game's render thread: one relaxed load.

#include <windows.h>
#include <d3d12.h>

namespace mgpu::stage
{
// At arm, after gxfer and gfence exist. `ring` is stream_state::RING. Reads the key.
// True when StageCopy is UP; false when off or when it failed to come up (then off).
bool arm(ID3D12Device *gdev, ID3D12CommandQueue *gq, ID3D12Resource *gxfer,
         unsigned long long heap_bytes, unsigned long long slot_bytes, unsigned ring);

// One relaxed load.
bool active();

// Where the game's list writes this frame's slot: the staging ring when up, gxfer
// otherwise. Same layout either way.
ID3D12Resource *dst(ID3D12Resource *gxfer);

// THE PRODUCED-COUNT SIGNAL, routed. Returns true when gfence was signalled at f
// (the caller records gsignaled = f, R275). Off: exactly gq->Signal(gfence, f).
bool signal(ID3D12CommandQueue *gq, ID3D12Fence *gfence, unsigned long long f);

// From stream_release, BEFORE gxfer is released: drains the copy queue (bounded)
// and frees everything.
void release();
}
