// MGPU Bridge — the bridge thread (T3)
#include <windows.h>
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

    DWORD WINAPI bridge_main(LPVOID)
    {
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

    // Publish the ready event before the thread exists — CreateThreadW is
    // the happens-before edge, so the worker's first read of the handle is
    // race-free.
    mgpu::adapter::ready_event();

    std::lock_guard<std::mutex> lk(st().cs);
    st().stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE h = CreateThreadW(nullptr, 0, bridge_main, nullptr, 0, &st().thread_id);
    if (h == nullptr)
    {
        char line[160];
        snprintf(line, sizeof line,
                 "[MGPU][T3] CreateThreadW failed (GetLastError=%lu) - bridge thread not started",
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
    st().thread = h;
    char line[160];
    snprintf(line, sizeof line, "[MGPU][T3] bridge thread spawned (thread id 0x%X)", st().thread_id);
    mgpu::diag::info(line);
}

void stop()
{
    std::lock_guard<std::mutex> lk(st().cs);
    if (!st().started.load())
        return;
    if (st().stop_event != nullptr)
        SetEvent(st().stop_event);
    if (st().thread != nullptr)
    {
        const DWORD r = WaitForSingleObject(st().thread, 5000);
        if (r == WAIT_TIMEOUT)
            mgpu::diag::warn("[MGPU][T3] bridge thread did not exit within 5000 ms - not waiting "
                             "longer (process is exiting)");
        else
            CloseHandle(st().thread);
        st().thread = nullptr;
    }
}
}
