// MGPU Bridge - R371 (0.5.1 discovery): read-only instruments for frame generation.
//
// WHY. Two reporters describe uneven motion with FrameGen=4. Their logs show whether frames
// are missing ([R341]) and whether the source is even ([R87]); they cannot show WHEN frames
// reach the screen: the display's refresh and the vblanks between displayed frames are in no
// log line (VSYNC_LEDGER section 3 names this gap; the I5 pacing line was never built).
// This module is that line, for the frame-generation path only.
//
// WHAT IT READS. Nothing is changed, queued, waited on or bound.
//   - the display refresh, once at the arm, from DWM's composition clock;
//   - after every present of the pacing thread: the time since its previous present, and
//     IDXGISwapChain::GetFrameStatistics. R371a: PresentCount / PresentRefreshCount (the fields
//     that say when a frame reached the display). Where exactly one new frame was reported since
//     the previous reading, the PresentRefreshCount difference is "vblanks between two
//     consecutive displayed frames". Readings with no new frame, and readings with more than
//     one, are counted apart and kept out of the histogram. If the statistics report no
//     displayed frames the line says "not measured" - never "no beat";
//   - at every hand-over (bridge thread): the time since the previous hand-over. Its spread
//     is the jitter the 0.5.1 pacing rule passes to the screen unchanged.
//
// KEY. FGInstr=1. While FrameGen=3 / 4 is armed; since R388 also without frame generation (the baseline of the same lines).
// (text before R388:) FGInstr=1, and only while FrameGen=4 is armed. Absent or 0: on() is one relaxed load
// that is false; nothing else runs. Line: [MGPU][R371], every 1200 presents.
//
// Sources: the vblank histogram follows N01950's on-screen cadence line (PR "non-native
// frame generation", present_frame) and the DWM refresh read follows the same PR; both are
// rewritten here, keyed, and moved off the shared present path into this module.
#pragma once

struct IDXGISwapChain3;

namespace mgpu
{
namespace fginstr
{
void arm(bool on, bool fg_armed = true);  // bridge thread, from fg::arm. R388: fg_armed false = FGInstr=1 without frame generation (the baseline lines)
bool on();                                // one relaxed load
void note_handoff(double dt_ms);
// R386: how long a thread waited for the stream lock. lock_t0() is the clock before the lock is asked for (0 when the
// instruments are off: one relaxed load); the two lock_waited_* take it once the lock is held.
//   present: the thread inside present_frame (the pacing thread with frame generation), from stream_present_source /
//            stream_present_split_left - a wait here sits between the pacing rule's wait and Present.
//   game:    the game's thread at finish_effects.
long long lock_t0();
void lock_waited_present(long long t0);
void lock_waited_game(long long t0);
// R388: the bridge thread's HOLDS of the stream lock in stream_poll, by where the hold ended:
//   end_site 0 = at the wait for the copy, 1 = at the wait for the GPU (the hold that recorded the pass), 2 = the rest.
// fg_recorded: the part of a hold spent inside fg::record (frame generation's own recording).
void bridge_held(long long t0, int end_site);
void fg_recorded(long long t0);
// R390: the display's period in ms from DWM's composition clock, asked now (0 = not read). Works whether or not the
// instruments are on; frame generation's limit (FGLimit) reads it at the arm and every 600 hand-overs.
double refresh_period_ms();
// R394: at the moment the bridge takes a frame (seal_consume, bridge thread). lat_ms = seal to this moment;
// frame = the frame taken; produced = the newest frame the game has sealed; arrived = the newest frame whose
// transfer has completed (the game's fence); qpc_submit = the taken frame's seal time. Splits the age of the
// frame into "sealed but not arrived yet" and "arrived and passed over", in game frames and in ms.
void note_consume(double lat_ms, unsigned long long frame, unsigned long long produced, unsigned long long arrived,
                  unsigned long long qpc_submit);
// R394: the stream's own L2 figure (READY-TO-CONSUME: from the first poll that saw the frame arrived to the take),
// which the shipped code prints only in the run summary. Bridge thread.
void note_ready(double ready_ms);          // bridge thread, from fg::handoff (dt <= 0 = unknown)
void after_present(IDXGISwapChain3 *sc);  // the thread that presented, after a successful Present
void release();                           // bridge thread, from fg::release
}   // namespace fginstr
}   // namespace mgpu
