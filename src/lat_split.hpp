#pragma once

// MGPU Bridge - R402 LatSplit=1 (0.5.1 discovery, read-only, off by default)
//
// WHAT THIS IS FOR
//
// From the game recording a frame to the bridge consuming it takes about 35 ms with one pass (every build since
// 0.3.0); about 10 ms of that is the neural work. The other 25 ms lie before the frame arrives on the neural card
// and were never split. This instrument splits them, per frame, on one clock (QPC):
//
//   record     the game's thread records the frame's copies (the seal's stamp)
//   hand-over  the game's thread hands the frame to the staging copy (stage::signal)
//   on GPU 0   the game card has run the frame up to that hand-over (the stage fence)
//   arrived    the staging copy has moved the slot across the link (the produced fence)
//   seen       the bridge thread's poll sees it
//   consumed   the neural work on it is done
//
// It changes nothing: two threads of its own wait on the two fences and write a time; the game's thread writes one
// time; the bridge thread reads them where it already consumes the frame. Needs StageCopy (the shipped default).
//
// THE CONTRACT
//   - LatSplit=1 in mgpu.ini, read at arm (bridge thread). Absent: sig() and consumed() are one relaxed load.
//   - Never a wait on the game's thread or on the bridge thread. The two watcher threads belong to this module.
//   - One [R402] line every 1200 consumed frames.

#include <windows.h>
#include <d3d12.h>

namespace mgpu::latsplit
{
// Bridge thread, at the stream's arm: reads the key.
void arm();
// The game's thread, in stage::signal after the stage fence was signalled for frame f.
void sig(unsigned long long f, ID3D12Fence *stage_fence, ID3D12Fence *produced_fence);
// Bridge thread, where the frame is consumed. All three are QPC values.
void consumed(unsigned long long f, unsigned long long qpc_record, unsigned long long qpc_seen, long long qpc_now);
// Bridge thread, at the stream's release: stops the watchers.
void release();
}
