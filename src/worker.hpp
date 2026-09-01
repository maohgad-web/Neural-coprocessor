// MGPU Bridge - the bridge thread (T3; window + message pump arrive with T4)
//
// The structural separation: everything GPU 1 is created, used and
// destroyed on this thread; the game thread runs ReShade's callbacks,
// must never block, and never touches GPU 1 objects.
#pragma once

namespace mgpu::worker
{
    // Game thread, from the first init_device / init_swapchain. Spawns the
    // thread exactly once. Deliberately NOT called from DllMain: a thread
    // created from DllMain runs under the loader lock.
    void ensure_started();

    // Signal-only shutdown; never waits. Safe from DllMain (which runs
    // under the loader lock: a thread cannot finish exiting without that
    // lock, so any join there would deadlock) and from ReShade callbacks
    // (the game thread must never block). The bridge thread tears down
    // the GPU 1 objects itself once it wakes; its final log lines are
    // best effort (see the comment in stop()).
    void stop();
}
