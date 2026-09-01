// MGPU Bridge — the bridge thread (T3; window + message pump arrive with T4)
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

    // Game thread, DllMain DLL_PROCESS_DETACH. Signals shutdown and joins
    // with a timeout so process exit can never hang.
    void stop();
}
