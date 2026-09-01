// MGPU Bridge - the bridge thread (T3)
//
// Thread creation uses _beginthreadex, not CreateThread: this thread uses
// CRT facilities (snprintf, std::mutex, std::atomic), and CreateThread
// skips the per-thread CRT initialization.
#include <windows.h>
#include <process.h>
#include <atomic>
#include <cstdio>
#include <mutex>

#include "adapter.hpp"
#include "diag.hpp"
#include "gpu1_context.hpp"
#include "worker.hpp"

namespace mgpu::worker
{
namespace
{
    struct state
    {
        std::atomic<bool> started{false};
        std::mutex cs;
        HANDLE stop_event = nullptr;
        HANDLE thread = nullptr;
        DWORD thread_id = 0;
    };

    state &st()
    {
        static state s;
        return s;
    }

    unsigned __stdcall bridge_main(void *arg)
    {
        (void)arg;
        mgpu::diag::info("[MGPU][T3] bridge thread started - owns all GPU 1 objects "
                         "(window + message pump arrive with T4)");

        // Wait for the T2 selection, or shutdown. The ready event is
        // manual-reset and was created on the game thread before this
        // thread was spawned.
        const HANDLE wait[2] = { mgpu::adapter::ready_event(), st().stop_event };
        const DWORD r = WaitForMultipleObjects(2, wait, FALSE, INFINITE);

        if (r != WAIT_OBJECT_0)
        {
            mgpu::diag::warn("[MGPU][T3] shutdown before the T2 selection completed - exiting");
            mgpu::gpu1::shutdown();
            mgpu::adapter::shutdown();
            mgpu::diag::info("[MGPU][T3] bridge thread exiting");
            return 0;
        }

        mgpu::adapter::selection_result sel;
        mgpu::adapter::get_selection(sel);
        mgpu::gpu1::create_device(sel);   // T3 gate; every outcome logged

        // T3: idle until shutdown. T4 replaces this wait with the window
        // and the message pump (created on this thread, before the
        // swapchain).
        WaitForSingleObject(st().stop_event, INFINITE);
        mgpu::diag::info("[MGPU][T3] shutdown requested - releasing GPU 1 objects");
        mgpu::gpu1::shutdown();
        mgpu::adapter::shutdown();
        mgpu::diag::info("[MGPU][T3] bridge thread exiting cleanly");
        return 0;
    }
}

void ensure_started()
{
    bool expected = false;
    if (!st().started.compare_exchange_strong(expected, true))
        return;

    // Publish the ready event before the thread exists: the _beginthreadex
    // call is the happens-before edge, so the worker's first read of the
    // handle is race-free.
    mgpu::adapter::ready_event();

    std::lock_guard<std::mutex> lk(st().cs);
    st().stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    unsigned tid = 0;
    const uintptr_t th = _beginthreadex(nullptr, 0, bridge_main, nullptr, 0, &tid, nullptr);
    if (th == 0)
    {
        char line[160];
        snprintf(line, sizeof line,
                 "[MGPU][T3] _beginthreadex failed (GetLastError=%lu) - bridge thread not started",
                 (unsigned long)GetLastError());
        mgpu::diag::error(line);
        if (st().stop_event != nullptr)
        {
            CloseHandle(st().stop_event);
            st().stop_event = nullptr;
        }
        st().started = false;   // allow a later device/swapchain event to retry
        return;
    }
    st().thread = static_cast<HANDLE>(th);
    st().thread_id = static_cast<DWORD>(tid);
    char line[160];
    snprintf(line, sizeof line, "[MGPU][T3] bridge thread spawned (thread id 0x%X)",
             (unsigned)st().thread_id);
    mgpu::diag::info(line);
}

void stop()
{
    // Signal-only. Never wait:
    //  - From DllMain we are under the loader lock, and a thread cannot
    //    finish exiting without that lock (its exit dispatches
    //    DLL_THREAD_DETACH to every loaded module): a join would deadlock,
    //    and a timed-out join would leave a live thread pointing into a
    //    DLL that is about to unmap.
    //  - From a ReShade callback the game thread must never block.
    // The mutex is never held across a wait (only to copy the handle).
    //
    // The bridge thread performs the actual teardown (device release,
    // adapter release, final log lines) once it wakes. That is best
    // effort: if the process exits before it wakes, the OS reclaims the
    // GPU objects; if ReShade unloads this module dynamically first, the
    // GPU 1 device is simply leaked. Explicitly in scope at P0 - a hang
    // is not.
    HANDLE ev = nullptr;
    {
        std::lock_guard<std::mutex> lk(st().cs);
        ev = st().stop_event;
    }
    if (ev != nullptr)
        SetEvent(ev);
}
}
