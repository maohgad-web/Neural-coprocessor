// MGPU Bridge - the bridge thread (T3; window + message pump arrive with T4)
//
// Thread creation uses _beginthreadex, not CreateThread: this thread uses
// CRT facilities (snprintf, std::mutex, std::atomic), and CreateThread
// skips the per-thread CRT initialization. Signature (6 arguments):
//   uintptr_t _beginthreadex(void *security, unsigned stack_size,
//       unsigned (__stdcall *start_address)(void *), void *arglist,
//       unsigned initflag, unsigned *thrdaddr);
//
// The one-shot re-arms: UE5's probe cycles tear the device down two or
// three times before the real render device appears, and ReShade sometimes
// re-attaches the module without a fresh LoadLibrary ("Loading externally
// registered add-on") - statics such as `started` survive. A thread that
// exited on a probe's teardown would otherwise never be replaced, and the
// T2 selection (now deferred to the real device's swapchain) would have no
// thread left to create the T3 device.
//
// T4: the window and the message pump. A window belongs to the thread that
// called CreateWindowExW, and only that thread may pump its messages - so
// register class, create window, PeekMessage/DispatchMessage, DestroyWindow
// and UnregisterClass all happen on this thread. Nothing touches the window
// from a ReShade callback, and nothing blocks the game thread waiting on it.
// The stop event is the single shutdown signal for this thread; WM_QUIT is
// never the exit signal (a WM_QUIT in the queue would be drained and
// dispatched to nothing, and the loop would keep waiting - a hang with no
// error anywhere). The device-removal poll T3 never built is folded into the
// pump loop: a 250 ms timed wait that calls gpu1::device_removed_reason and
// logs only on the transition away from S_OK (the value is sticky once
// removed).
#include <windows.h>
#include <process.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>

#include "adapter.hpp"
#include "diag.hpp"
#include "gpu1_context.hpp"
#include "worker.hpp"

// T4 (brief section 00, exception 1): the add-on's own module handle,
// defined in dllmain.cpp and captured at DLL_PROCESS_ATTACH.
// GetModuleHandle(nullptr) returns the game's module, not ours, so the
// window class's hInstance must be this handle.
namespace mgpu { HMODULE module_handle(); }

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

    // The thread has finished: reset the stop event and re-arm the
    // one-shot so a later device/swapchain event can spawn a fresh bridge
    // thread. The stop event is Reset, not Close: it must be unset for the
    // re-armed thread's first wait, and keeping it live means stop()
    // (signal-only, from the game thread or DllMain) never touches a
    // closed handle. The ready event is reset by adapter::shutdown(), so a
    // re-armed thread cannot wake on this run's stale selection either.
    void rearm()
    {
        std::lock_guard<std::mutex> lk(st().cs);
        if (st().stop_event != nullptr)
            ResetEvent(st().stop_event);
        // _beginthreadex returns a handle the caller owns; dropping the
        // pointer does not release it. UE5 loads and unloads the add-on
        // once per adapter probe - five cycles per launch on this rig - so
        // an unclosed handle here is a per-launch leak, not a theoretical
        // one. Closing our own handle from inside the thread it refers to
        // is safe: the handle keeps the kernel object alive independently
        // of the thread, and nothing waits on it (see stop(): the teardown
        // is signal-only and never joins).
        if (st().thread != nullptr)
        {
            CloseHandle(st().thread);
            st().thread = nullptr;
        }
        st().thread_id = 0;
        st().started = false;
    }

    // T4, requirement 4: the window procedure stays minimal. WM_CLOSE and
    // WM_DESTROY signal the same shutdown path the add-on unload uses
    // (worker::stop - signal only, never waits). Everything else is passed
    // to DefWindowProcW. The user closing this window must not close the
    // game: we signal our own shutdown, tear down our side (in the ordered
    // teardown after the loop exits), and leave the game running. No
    // rendering, no D3D calls, no logging beyond these two messages.
    //
    // WM_CLOSE is handled here (not by DefWindowProcW) so the system does
    // not DestroyWindow immediately: the window is destroyed by the ordered
    // teardown on this same thread, which is what keeps "no DestroyWindow
    // failure in the log" true.
    LRESULT CALLBACK bridge_wndproc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        (void)wParam; (void)lParam;
        switch (msg)
        {
        case WM_CLOSE:
            mgpu::diag::info("[MGPU][T4] WM_CLOSE - the bridge window was closed by the user; "
                             "signalling bridge shutdown (the game is not affected)");
            stop();
            return 0;
        case WM_DESTROY:
            mgpu::diag::info("[MGPU][T4] WM_DESTROY - the bridge window was destroyed; "
                             "signalling bridge shutdown (the game is not affected)");
            stop();
            return 0;
        default:
            return DefWindowProcW(hWnd, msg, wParam, lParam);
        }
    }

    unsigned __stdcall bridge_main(void *arg)
    {
        (void)arg;
        mgpu::diag::info("[MGPU][T3] bridge thread started - owns all GPU 1 objects "
                         "(window + message pump arrive with T4)");

        // Wait for the T2 selection, or shutdown. The ready event is
        // manual-reset, was created on the game thread before this thread
        // was spawned, and is set only when the selection is *decided* -
        // an adapter selected, or a terminal refusal logged.
        const HANDLE wait[2] = { mgpu::adapter::ready_event(), st().stop_event };
        const DWORD r = WaitForMultipleObjects(2, wait, FALSE, INFINITE);

        if (r != WAIT_OBJECT_0)
        {
            mgpu::diag::warn("[MGPU][T3] shutdown before the T2 selection completed - exiting");
            mgpu::gpu1::shutdown();
            mgpu::adapter::shutdown();
            mgpu::diag::info("[MGPU][T3] bridge thread exiting");
            rearm();
            return 0;
        }

        mgpu::adapter::selection_result sel;
        mgpu::adapter::get_selection(sel);
        if (sel.valid && sel.selected_adapter == nullptr)
        {
            // Stale wake-up: a re-armed thread that saw a prior run's
            // selection after its adapter reference was released. Refuse
            // rather than guess - a null adapter would mean
            // D3D12CreateDevice on the default adapter, the silent
            // wrong-adapter failure T2 exists to prevent.
            mgpu::diag::warn("[MGPU][T3] stale selection (no adapter reference) - exiting "
                             "without creating a device");
            mgpu::adapter::shutdown();
            mgpu::diag::info("[MGPU][T3] bridge thread exiting");
            rearm();
            return 0;
        }
        // T3 gate; every outcome logged. The return value is whether a
        // device exists now (created, or already present) - T4 gates the
        // window on it.
        const bool have_device = mgpu::gpu1::create_device(sel);

        // ---- T4: the window and the message pump (bridge thread only) ----
        // The window is created only when the device exists - never on a
        // cycle with no device, and only after create_device returns true.
        // A window whose thread is about to exit is worse than no window.
        const DWORD tid = GetCurrentThreadId();
        char line[320];
        // The class name embeds the HMODULE so a stale class from an
        // unmapped module can never be reused (its lpfnWndProc would point
        // into unmapped memory). A reload at a different base produces a
        // different name; the same still-mapped module produces the same
        // name, which is what makes ERROR_CLASS_ALREADY_EXISTS unambiguous.
        // Two encodings of the same ASCII name: the narrow form feeds the
        // log lines (diag takes const char *); the wide form is what the
        // W APIs register, create and unregister (WNDCLASSEXW.lpszClassName
        // is LPCWSTR - a narrow char[] would not compile). The wide form is
        // built with MultiByteToWideChar (kernel32, always linked) rather
        // than StringCchPrintfW (strsafe.lib), which the closed CMakeLists
        // does not link.
        char class_name[64];
        wchar_t class_name_w[64];
        snprintf(class_name, sizeof class_name, "MGPU_Bridge_Wnd_%p",
                 (void *)mgpu::module_handle());
        const int converted = MultiByteToWideChar(CP_UTF8, 0, class_name, -1,
                                                  class_name_w, 64);
        if (converted == 0)
        {
            // Cannot happen for this ASCII input into a 64-wide buffer, but
            // if it did, an empty class name would make RegisterClassExW
            // fail and the stop-and-report path below would catch it.
            class_name_w[0] = L'\0';
        }

        bool class_registered = false;
        HWND hwnd = nullptr;
        bool pump = false;
        bool failed_permanently = false;
        bool have_chain = false;   // T5: the present chain was created

        if (have_device)
        {
            // Requirement 1: register the window class on the bridge thread,
            // hInstance = the add-on's own HMODULE (not the game's).
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.style = CS_HREDRAW | CS_VREDRAW;
            wc.lpfnWndProc = bridge_wndproc;
            wc.hInstance = mgpu::module_handle();
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hIcon = nullptr;
            wc.hbrBackground = nullptr;
            wc.lpszMenuName = nullptr;
            wc.lpszClassName = class_name_w;
            wc.hIconSm = nullptr;

            const ATOM atom = RegisterClassExW(&wc);
            if (atom == 0)
            {
                const DWORD gle = GetLastError();
                if (gle == ERROR_CLASS_ALREADY_EXISTS)
                {
                    // Safe to proceed (the class is ours - this same
                    // still-mapped module registered it on an earlier
                    // cycle) and a defect report: our teardown missed
                    // UnregisterClass. Log at error; do not retry, do not
                    // delete-and-reregister, and do not fall back to a name.
                    snprintf(line, sizeof line,
                             "[MGPU][T4] ERROR_CLASS_ALREADY_EXISTS on class \"%s\" - a prior "
                             "cycle's teardown missed UnregisterClass (same still-mapped module); "
                             "proceeding with the existing class, thread id 0x%X",
                             class_name, (unsigned)tid);
                    mgpu::diag::error(line);
                    class_registered = true;   // the class exists; teardown will unregister it
                }
                else
                {
                    // Stop-and-report: any other RegisterClassExW failure.
                    // No window is created; the wait loop runs with the pump
                    // branch omitted but keeps the 250 ms timeout (the
                    // device-removal poll still has to run).
                    snprintf(line, sizeof line,
                             "[MGPU][T4] RegisterClassExW failed (GetLastError=%lu) class=\"%s\" "
                             "thread id 0x%X - no window will be created; P0 cannot proceed past "
                             "T4 in this run",
                             (unsigned long)gle, class_name, (unsigned)tid);
                    mgpu::diag::error(line);
                    failed_permanently = true;
                }
            }
            else
            {
                class_registered = true;

                // Requirement 2: create the window, client area exactly
                // 1280x720. CreateWindowExW's width/height are the OUTER
                // dimensions, so compute them with AdjustWindowRect against
                // the same style - otherwise the client area comes out
                // smaller than 720 lines by the title bar and borders.
                //
                // T5 (brief section 06, gotcha 8): the style drops
                // WS_THICKFRAME and WS_MAXIMIZEBOX - non-resizable and
                // non-maximizable, which removes ResizeBuffers from P0
                // entirely. The identical style value feeds AdjustWindowRect
                // below: a style change in one place only would silently
                // break the 1280x720 client rect that T4 fixed (load-bearing
                // for the flow pyramid's coarsest level).
                const DWORD wnd_style =
                    WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
                RECT rc{0, 0, 1280, 720};
                AdjustWindowRect(&rc, wnd_style, FALSE);
                const int width = rc.right - rc.left;
                const int height = rc.bottom - rc.top;

                // T5 (section 09): no WS_VISIBLE. A window created visible
                // takes foreground activation from the game the moment it
                // appears, and the game's borderless-fullscreen presentation
                // drops to windowed-with-borders with the taskbar showing.
                // The window is shown without activation below.
                hwnd = CreateWindowExW(
                    0,
                    class_name_w,
                    L"MGPU Bridge (GPU 1)",
                    wnd_style,
                    CW_USEDEFAULT, CW_USEDEFAULT,
                    width, height,
                    nullptr, nullptr,
                    mgpu::module_handle(),
                    nullptr);
                if (hwnd == nullptr)
                {
                    const DWORD gle = GetLastError();
                    snprintf(line, sizeof line,
                             "[MGPU][T4] CreateWindowExW failed (GetLastError=%lu) class=\"%s\" "
                             "thread id 0x%X - the class is registered but no window exists; P0 "
                             "cannot proceed past T4 in this run",
                             (unsigned long)gle, class_name, (unsigned)tid);
                    mgpu::diag::error(line);
                    failed_permanently = true;
                    // class_registered stays true; teardown will unregister it.
                }
                else
                {
                    // Log the class name, the window handle, the client
                    // rect, and the owning thread id (acceptance).
                    RECT cr{};
                    GetClientRect(hwnd, &cr);
                    snprintf(line, sizeof line,
                             "[MGPU][T4] window created: class=\"%s\" hwnd=0x%p client=%dx%d "
                             "(%d,%d,%d,%d) thread id 0x%X (client area is the T4-fixed 1280x720)",
                             class_name, (void *)hwnd,
                             cr.right - cr.left, cr.bottom - cr.top,
                             cr.left, cr.top, cr.right, cr.bottom,
                             (unsigned)tid);
                    mgpu::diag::info(line);
                    pump = true;

                    // T5 (section 09): show the window without stealing
                    // activation. On a valid hwnd ShowWindow is expected to
                    // succeed; if it fails the window stays hidden - a
                    // swapchain presents to a hidden window fine - so log
                    // the input and continue.
                    if (ShowWindow(hwnd, SW_SHOWNOACTIVATE) == FALSE)
                    {
                        const DWORD gle = GetLastError();
                        snprintf(line, sizeof line,
                                 "[MGPU][T5] ShowWindow(SW_SHOWNOACTIVATE) failed (GetLastError=%lu) "
                                 "hwnd=0x%p - the window stays hidden; the present chain still runs",
                                 (unsigned long)gle, (void *)hwnd);
                        mgpu::diag::error(line);
                    }

                    // T5: the present chain on the GPU 1 device, against
                    // this hwnd (bridge thread only). Failure is not fatal
                    // to the window: the pump must stay (a window whose
                    // thread stops pumping stalls the shell - section 09),
                    // and the loop falls back to the T4 250 ms structure
                    // with nothing to present.
                    if (mgpu::gpu1::create_present_chain(hwnd))
                    {
                        have_chain = true;

                        // P1.0: the NGX probe. Once, here, on the bridge
                        // thread - the chain exists and the present loop has
                        // not started, so CreateFeature's ~1.16 s stalls
                        // nothing. 1280x720 is the T4-fixed client size of
                        // this window, which is also what the chain was
                        // created against. The return value is deliberately
                        // ignored: the probe logs its own verdict, and a
                        // failure must not change how the bridge behaves.
                        (void)mgpu::gpu1::ngx_probe(1280, 720);
                    }
                    else
                        mgpu::diag::error("[MGPU][T5] no present chain on this cycle - the window "
                                          "stays up without presenting (see the [MGPU][T5] creation "
                                          "lines for the failing call)");
                }
            }
        }
        else
        {
            // No device on this cycle (a terminal refusal). No window, no
            // class. The wait loop runs without the pump branch but keeps
            // the 250 ms timeout so the device-removal poll still runs (a
            // no-op with no device, but the structure is uniform).
            snprintf(line, sizeof line,
                     "[MGPU][T4] no device on this cycle - no window created; waiting without the "
                     "pump branch (thread id 0x%X)",
                     (unsigned)tid);
            mgpu::diag::info(line);
        }

        // Requirement 3: the pumping loop, replacing the post-device
        // WaitForSingleObject(INFINITE). One loop, one thread - no second
        // thread for the pump. The stop event is the single shutdown
        // signal; WM_QUIT is never the exit signal.
        //
        // T5 reshapes the pump path: with a present chain the loop is
        // vsync-paced - Present(1, 0) blocks on vblank (~16 ms), so the
        // 250 ms timed wait goes away for the present path. The stop event
        // is checked non-blocking at the top of every frame, messages are
        // drained every frame, and the device-removal poll moves from the
        // 250 ms timer to a frame counter (every 60 frames, ~1 s: same
        // intent, one loop). The no-chain path (no window, or a window
        // whose present chain failed to create) keeps the T4 structure
        // exactly as it was.
        bool removed_logged = false;
        unsigned long long frame = 0;   // T5: completed presents
        for (;;)
        {
            if (have_chain)
            {
                // T5: the present loop. Vsync (Present(1, 0)) is the
                // pacing mechanism - there is no timed wait.
                const DWORD sw = WaitForSingleObject(st().stop_event, 0);
                if (sw == WAIT_OBJECT_0)
                    break;   // shutdown
                if (sw == WAIT_FAILED)
                {
                    // A failed wait is not transient (an invalid handle,
                    // etc.); spinning would teach nothing. Log and tear
                    // down cleanly (the T4 rule, kept).
                    snprintf(line, sizeof line,
                             "[MGPU][T5] wait failed (GetLastError=%lu) thread id 0x%X - tearing down",
                             (unsigned long)GetLastError(), (unsigned)tid);
                    mgpu::diag::error(line);
                    break;
                }

                // Drain messages until the queue is empty, then present.
                // WM_QUIT is never the exit signal (the stop event is) -
                // discard it without dispatching, so a stray WM_QUIT cannot
                // be mistaken for a shutdown.
                MSG m;
                while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE) != FALSE)
                {
                    if (m.message == WM_QUIT)
                        continue;
                    TranslateMessage(&m);
                    DispatchMessageW(&m);
                }

                // The colour must animate: a static clear cannot
                // distinguish "presenting" from "presented once and hung."
                // Driven by the frame counter, not a clock: three
                // phase-shifted sinusoids, 180 frames per revolution
                // (~3 s at the vblank pace). The modulo keeps the argument
                // to sin bounded.
                constexpr unsigned long long REV_PERIOD = 180;
                constexpr float TAU = 6.283185307179586f;
                const float ph = TAU * static_cast<float>(frame % REV_PERIOD) /
                                 static_cast<float>(REV_PERIOD);
                const float cr = 0.5f + 0.5f * std::sin(ph);
                const float cg = 0.5f + 0.5f * std::sin(ph + TAU / 3.0f);
                const float cb = 0.5f + 0.5f * std::sin(ph + 2.0f * TAU / 3.0f);

                if (!mgpu::gpu1::present_frame(cr, cg, cb))
                {
                    // The first failure is already logged with its step,
                    // HRESULT and removal reason (one-shot, in
                    // gpu1_context). Stop presenting: the loop exits to
                    // the ordered teardown.
                    break;
                }
                ++frame;
                if (frame == 1)
                {
                    mgpu::diag::info("[MGPU][T5] first successful present (frame 1) - the "
                                     "vsync-paced present loop is alive");
                }
                else if (frame % 600 == 0)
                {
                    snprintf(line, sizeof line,
                             "[MGPU][T5] present loop alive: frame %llu thread id 0x%X",
                             (unsigned long long)frame, (unsigned)tid);
                    mgpu::diag::info(line);
                }

                // The device-removal poll, moved from the 250 ms timer to
                // a frame counter (every 60 frames, ~1 s at vblank): same
                // intent, one loop. T4's line verbatim - it identifies the
                // T4 acceptance item, which this branch now hosts.
                if (frame % 60 == 0)
                {
                    HRESULT reason = S_OK;
                    if (mgpu::gpu1::device_removed_reason(reason) && reason != S_OK && !removed_logged)
                    {
                        snprintf(line, sizeof line,
                                 "[MGPU][T4] device removed: GetDeviceRemovedReason hr=0x%08X (sticky - "
                                 "logged once on the transition away from S_OK), thread id 0x%X",
                                 (unsigned)reason, (unsigned)tid);
                        mgpu::diag::error(line);
                        removed_logged = true;
                    }
                }
                continue;
            }

            // No present chain: the T4 structure, unchanged.
            DWORD wr;
            if (pump)
                wr = MsgWaitForMultipleObjects(1, &st().stop_event, FALSE, 250, QS_ALLINPUT);
            else
                wr = WaitForSingleObject(st().stop_event, 250);

            if (wr == WAIT_FAILED)
            {
                // A failed wait is not transient (an invalid handle, etc.);
                // spinning would teach nothing. Log and tear down cleanly.
                snprintf(line, sizeof line,
                         "[MGPU][T4] wait failed (GetLastError=%lu) thread id 0x%X - tearing down",
                         (unsigned long)GetLastError(), (unsigned)tid);
                mgpu::diag::error(line);
                break;
            }

            if (wr == WAIT_OBJECT_0)
                break;   // shutdown

            if (pump && wr == WAIT_OBJECT_0 + 1)
            {
                // Messages are waiting: drain them until the queue is empty,
                // then loop. WM_QUIT is never the exit signal (the stop
                // event is) - discard it without dispatching, so a stray
                // WM_QUIT cannot be mistaken for a shutdown.
                MSG m;
                while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE) != FALSE)
                {
                    if (m.message == WM_QUIT)
                        continue;
                    TranslateMessage(&m);
                    DispatchMessageW(&m);
                }
                continue;
            }

            // WAIT_TIMEOUT: the poll tick (250 ms). Call the removal-reason
            // accessor and log only on the transition away from S_OK - the
            // value is sticky once removed (brief section 09), so a healthy
            // device is silent by construction.
            HRESULT reason = S_OK;
            if (mgpu::gpu1::device_removed_reason(reason) && reason != S_OK && !removed_logged)
            {
                snprintf(line, sizeof line,
                         "[MGPU][T4] device removed: GetDeviceRemovedReason hr=0x%08X (sticky - "
                         "logged once on the transition away from S_OK), thread id 0x%X",
                         (unsigned)reason, (unsigned)tid);
                mgpu::diag::error(line);
                removed_logged = true;
            }
        }

        // Requirement 5: ordered teardown, on this same thread and in this
        // order, before rearm(). Everything the thread owns is released
        // before it re-arms, so a replacement thread's RegisterClassExW
        // cannot race this thread's cleanup.
        //
        // T5 reorders it: the present chain (and the device) go FIRST.
        // The T4 order (DestroyWindow first) is wrong once a swapchain
        // exists - the swapchain holds a reference to the window it was
        // created against and would outlive it. gpu1::shutdown() drains
        // the GPU before releasing the chain, and is a no-op for the chain
        // when none was created (the no-window path is unaffected).
        mgpu::diag::info("[MGPU][T5] shutdown - ordered teardown (gpu1::shutdown [present chain -> "
                         "device] -> DestroyWindow -> UnregisterClass -> adapter::shutdown)");

        mgpu::gpu1::shutdown();

        if (hwnd != nullptr)
        {
            // DestroyWindow must be called from the thread that created the
            // window (this thread); from any other thread it returns FALSE
            // with ERROR_ACCESS_DENIED.
            if (DestroyWindow(hwnd) == FALSE)
            {
                snprintf(line, sizeof line,
                         "[MGPU][T4] DestroyWindow failed (GetLastError=%lu) hwnd=0x%p thread id 0x%X",
                         (unsigned long)GetLastError(), (void *)hwnd, (unsigned)tid);
                mgpu::diag::error(line);
            }
            else
            {
                mgpu::diag::info("[MGPU][T4] window destroyed");
            }
            hwnd = nullptr;
        }

        if (class_registered)
        {
            // Unregister the class we registered (or that a prior cycle of
            // this same still-mapped module left behind). The class name is
            // per-module, so this targets exactly our class.
            if (UnregisterClassW(class_name_w, mgpu::module_handle()) == FALSE)
            {
                // Not fatal: the OS unregisters a class automatically when
                // the owning module unmaps, so it may already be gone. Log
                // for the record.
                snprintf(line, sizeof line,
                         "[MGPU][T4] UnregisterClassW returned FALSE (GetLastError=%lu) class=\"%s\" "
                         "(not fatal - the class may already be gone)",
                         (unsigned long)GetLastError(), class_name);
                mgpu::diag::info(line);
            }
            else
            {
                mgpu::diag::info("[MGPU][T4] window class unregistered");
            }
            class_registered = false;
        }

        mgpu::adapter::shutdown();
        mgpu::diag::info("[MGPU][T4] bridge thread exiting cleanly");

        if (failed_permanently)
        {
            // Do not re-arm: a failed cycle must not spawn a replacement
            // thread and retry (the "P0 cannot proceed past T4" line was
            // already logged at the failure site). Close the thread handle
            // to avoid the per-cycle leak, but leave started = true so
            // ensure_started() never spawns a replacement.
            std::lock_guard<std::mutex> lk(st().cs);
            if (st().thread != nullptr)
            {
                CloseHandle(st().thread);
                st().thread = nullptr;
            }
            st().thread_id = 0;
            return 0;
        }

        rearm();
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
    // A re-arm after a prior thread exited reuses the stop event (that
    // thread's rearm() left it reset, see above); only the very first
    // start - or a retry after a failed start, which closed it - creates
    // one.
    if (st().stop_event == nullptr)
        st().stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    unsigned tid = 0;
    const uintptr_t th = _beginthreadex(nullptr, 0, bridge_main, nullptr, 0, &tid);
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
    st().thread = reinterpret_cast<HANDLE>(th);
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
    // The bridge thread performs the actual teardown (T4: DestroyWindow,
    // UnregisterClass, device release, adapter release, final log lines,
    // re-arm) once it wakes. That is best effort: if the process exits
    // before it wakes, the OS reclaims the GPU objects; if ReShade unloads
    // this module dynamically first, the GPU 1 device is simply leaked.
    // Explicitly in scope at P0 - a hang is not.
    HANDLE ev = nullptr;
    {
        std::lock_guard<std::mutex> lk(st().cs);
        ev = st().stop_event;
    }
    if (ev != nullptr)
        SetEvent(ev);
}
}