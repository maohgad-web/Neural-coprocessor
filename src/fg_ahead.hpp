#pragma once

// MGPU Bridge - R399 FGAhead (0.5.1 discovery, off by default): the in-between frame made AHEAD of the neural frame
//
// WHAT THIS IS FOR (Marcelo, 2026-11-15)
//
// With frame generation the finished neural frame is locked behind the generated one: the generator needs it as
// the second end of its pair, so the generated frame is shown first and the neural frame half an interval later.
// At a heavy evaluate that lock is long, and every picture on screen is old.
//
// FGAhead takes the second end from what is already on the neural card: the game frame the neural card has just
// PICKED to enhance next. Its colour, vectors and depth are there one evaluate before its neural output exists.
// The picture order becomes
//
//     neural frame A (shown the moment it finishes)
//     generated frame, between neural A and the picked game frame B (shown during B's evaluate)
//     neural frame B (shown the moment it finishes)
//
// The game frame itself is never presented. Nothing is held: the lock is gone.
//
// THIS FILE is only the small command list the generator call for the picked frame is recorded into. It runs on
// the NEURAL queue, submitted before the frame's own neural list, so it is ordered behind the previous frame's
// work and ahead of this frame's evaluate - the same queue the generator has always run on. The rule that decides
// what is made and when it is shown is in fg_gpu1.cpp (ahead_take, the seat in record, the pacing thread).
//
// THE CONTRACT
//   - Nothing here exists until fg_gpu1.cpp calls create(), which it does only with FGAhead=1|2 and FrameGen=3|4.
//   - begin() / submit() / cancel() / release() on the bridge thread. done() on any thread (a fence read).
//   - Never a wait on the bridge thread except in release() (bounded). A list whose turn has not passed is refused
//     (counted), never waited for.

#include <windows.h>
#include <d3d12.h>

namespace mgpu::fgahead
{
// The list type follows the queue's. False + `why` when something could not be made (nothing is left behind).
bool create(ID3D12Device *dev, ID3D12CommandQueue *queue, char *why, size_t n);
bool up();

// An open list to record the generator call into, or nullptr when the list of this turn is still running (counted).
ID3D12GraphicsCommandList *begin();

// Closes the list, orders it behind the copy queue's unpack of the picked frame (`unpack` at `unpack_value`), runs it
// and signals. Returns the fence value that says it is done, 0 when it could not be run.
unsigned long long submit(ID3D12Fence *unpack, unsigned long long unpack_value);

// Closes an open list without running it (the generator call failed while recording).
void cancel();

// True once the list submit() returned `value` for has run.
bool done(unsigned long long value);

void summary(char *out, size_t n);
void release();
}
