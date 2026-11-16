// MGPU Bridge - R375 (0.5.1 discovery): a waitable present chain with a frame latency of 1.
//
// WHY. The bridge chain is created with Flags = 0 and never sets a frame latency: DXGI's default
// lets up to three presented frames queue behind the one on screen (PRESENT_STALL_LEDGER, R114b,
// lines 54-56 and 108-112, which proposes exactly this change). Every queued frame is one more
// frame between the neural card's output and the eye. With frame generation the queue holds
// generated and real frames alike, so the cost is paid twice per neural frame.
//
// WHAT. PresentLatency1=1 creates the chain with DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT,
// sets a maximum frame latency of 1, and waits on the chain's object (at most 100 ms) before each
// present is recorded. The wait is on the thread that presents: the bridge thread, or the frame-
// generation pacing thread. Both are the bridge's own. Nothing waits on a game thread (R276).
//
// SOURCE. Zhizhen's fork (`PresentLatency1`, "LP1"): same flag, same latency, same 100 ms wait. The
// key keeps that name so an ini from that fork means the same thing here. Rewritten as a module:
// gpu1_context.cpp gets five calls and no state.
//
// KEY. PresentLatency1=1, read once when the chain is made (a restart applies a change). Absent,
// 0 or any other value: chain_flags() and resize_flags() return 0 - the value the two call sites
// passed before - on() is false, and no other function does anything.
// Line: [MGPU][R375] at the chain's creation, then every 1200 waits (mean / max wait, time-outs).
#pragma once

struct IDXGISwapChain3;

namespace mgpu
{
namespace pwait
{
unsigned chain_flags();                   // create_present_chain: reads the key; the flag, or 0
void after_create(IDXGISwapChain3 *sc);   // the chain exists: latency 1 + the waitable object (only if chain_flags() gave the flag)
unsigned resize_flags();                  // present_resize: ResizeBuffers must repeat the creation flags; 0 unless waitable
void after_resize(IDXGISwapChain3 *sc);   // states the latency again after a successful ResizeBuffers
bool on();                                // one relaxed load
void before_present();                    // the presenting thread: wait for the chain's free slot, 100 ms at most
void release();                           // the chain is gone: close the object
}   // namespace pwait
}   // namespace mgpu
