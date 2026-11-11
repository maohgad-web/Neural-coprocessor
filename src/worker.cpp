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
// P7.10: IDXGIAdapter1::EnumOutputs and DXGI_OUTPUT_DESC, to ask the BRIDGE
// adapter which panels it drives instead of asking Windows where it happened to
// put our window. dxgi1_4.h is what adapter.cpp already includes, so the two
// files agree on the DXGI surface level.
#include <dxgi1_4.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>

#include "adapter.hpp"
#include "diag.hpp"
#include "gpu1_context.hpp"
#include "fg_gpu1.hpp"   // R341
#include "panel_simple.hpp"   // the simple-panel hotkey below
#include "startup_trace.hpp"   // R313
#include "worker.hpp"

// T4 (brief section 00, exception 1): the add-on's own module handle,
// defined in dllmain.cpp and captured at DLL_PROCESS_ATTACH.
// GetModuleHandle(nullptr) returns the game's module, not ours, so the
// window class's hInstance must be this handle.
// P1.3g. Hotkey id, process-unique. RegisterHotKey(nullptr, id, ...) scopes the
// id to the calling THREAD, so a collision is only possible with ourselves.
#define MGPU_HOTKEY_ID 0x4D47   // 'MG'
// P6.3. Three more, ids adjacent to the first so a collision report names the
// block rather than a lone key. CTRL+ALT is kept as the modifier for the same
// reason it was chosen for F10: a bare function key is claimed by games freely,
// and ReShade owns Home. F12 is deliberately avoided - debuggers take it.
#define MGPU_HOTKEY_INT_TARGET 0x4D48   // CTRL+ALT+F8  - cycle which pass
#define MGPU_HOTKEY_INT_DOWN   0x4D49   // CTRL+ALT+F9  - one step down
#define MGPU_HOTKEY_INT_UP     0x4D4A   // CTRL+ALT+F11 - one step up
// P7.4: CTRL+ALT+F7 cycles the view - output, input, split. On the hotkey and
// not only in the panel because the panel is the ReShade overlay, and opening
// the overlay to change the view puts the overlay in the shot. The comparison
// has to be filmable without the instrument on screen.
#define MGPU_HOTKEY_VIEW       0x4D4B   // CTRL+ALT+F7  - cycle present mode
// P7.5: the seam, moved with nothing on screen but the game. Four ids rather
// than one plus a modifier read at press time, because RegisterHotKey delivers
// only the exact combination it was registered for - a SHIFT held over
// CTRL+ALT+LEFT does not arrive as CTRL+ALT+LEFT, it arrives as nothing.
#define MGPU_HOTKEY_SEAM_L     0x4D4C   // CTRL+ALT+LEFT
#define MGPU_HOTKEY_SEAM_R     0x4D4D   // CTRL+ALT+RIGHT
#define MGPU_HOTKEY_SEAM_LC    0x4D4E   // CTRL+ALT+SHIFT+LEFT  (coarse)
#define MGPU_HOTKEY_SEAM_RC    0x4D4F   // CTRL+ALT+SHIFT+RIGHT (coarse)
// V52: CTRL+ALT+F6 unroots the composition visual and roots it again. With
// DcompOverlay=1 the bridge is the TOPMOST visual on the game's window, so the
// game's own ReShade overlay is drawn underneath it - correctly, and
// invisibly. This gets out of its way without touching the stream. REGISTERED
// ONLY IN THAT MODE: in mode 0 nothing is covering the overlay, and an add-on
// that silently claims a process-wide hotkey for a key that does nothing is
// not "no regression".
// F6 and not F12 because Steam's screenshot key is F12.
#define MGPU_HOTKEY_PEEK       0x4D50   // CTRL+ALT+F6
// The simple panel. F4 because F5/F9 are quick save/load on several titles, F6-F11 are taken by
// the keys above, and F12 belongs to Steam.
#define MGPU_HOTKEY_SIMPLE     0x4D51   // CTRL+ALT+F4 - full panel <-> simple panel
#define MGPU_HOTKEY_CURSOR     0x4D52   // CTRL+ALT+F5 - show/hide the desktop cursor (DispCursor)

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
    // [MOUSE PUMP, 2026-11-11] THE PRESENTER'S PROVEN DESIGN, PORTED TO THIS WINDOW.
    //
    // PBZ原文（记忆里的成熟方案）："It swallows clicks and hides the game cursor unless
    // the display thread pumps messages and forwards them to the game window"; "a null
    // foreground means no window owns the keyboard so input dies"; "HTTRANSPARENT proven
    // useless cross-thread"; PBZ needs DispClickThrough=0 (the picture window TAKES the
    // mouse and forwards it; it does not pass through).
    //
    // The window stays WS_EX_NOACTIVATE + MA_NOACTIVATE, so the GAME keeps the foreground
    // (keyboard and raw input are the game's). Only the mouse hits land here - this window
    // is topmost and covers the screen - and each one is converted to the game's client
    // space and posted to the game's window. No AttachThreadInput, no SetForegroundWindow,
    // no ClipCursor: the presenter needs none of them and neither do we.
    static bool input_pump_enabled()
    {
        static int v = -1;
        if (v < 0)
            v = (mgpu::gpu1::dcomp_overlay_mode() == 0 &&
                 mgpu::gpu1::ui_ini_read("DispClickThrough", 0) == 0) ? 1 : 0;
        return v != 0;
    }

    static HWND input_game_window()
    {
        HWND game = (HWND)mgpu::gpu1::dcomp_game_hwnd();
        if (game == nullptr || !IsWindow(game)) game = GetForegroundWindow();
        return game;
    }

    // [H7/B, 2026-11-11] THE NEURAL WINDOW IS ON TOP ONLY WHILE THE GAME IS IN FRONT.
    //
    // THE DEFECT, WITH ITS ADDRESS. The XeFG hand-over path parks this window with
    // SetWindowPos(hwnd, HWND_TOPMOST, ...) - gpu1_context.cpp, the
    // "[H7/A, 2026-11-11] XeFG TAKES THE AUTHOR'S OWN WINDOW CHAIN" block - and nothing ever
    // takes it down again. WS_EX_TOPMOST belongs to the WINDOW, not to the game's activation,
    // so after Alt+Tab the neural picture stays over Explorer, over the browser, over
    // everything: the window cannot be switched away from. That is the whole defect, and it
    // is not a DcompOverlay=1 property - it is one SetWindowPos call in the mode-0 path.
    //
    // THE SHAPE IS presenter.cpp:752-888 (disp_housekeeping), the design that shipped and was
    // measured on PBZ: while the game holds the foreground, show (SW_SHOWNOACTIVATE) and
    // re-assert HWND_TOPMOST; the moment anything else does, SW_HIDE. The stream is never
    // touched - only the window - so the neural frames keep being produced and consumed.
    //
    // WHY NOT JUST "REMOVE WS_EX_TOPMOST": while the game IS in front, activating its window
    // puts it above every ordinary window, ours included, and the neural picture disappears.
    // Topmost while the game is foreground, hidden while it is not, is the only pair that
    // gives both behaviours.
    //
    // WHY HIDING IS SAFE ON THIS CHAIN: it presents with PresentVsync=0 / PresentTear=1, i.e.
    // Present(0, ALLOW_TEARING), so a window that is not composited never blocks the bridge
    // thread on a vblank. R111's 5 s stall was measured on a VISIBLE, COVERED window (its own
    // log says so); a hidden window is not that state.
    static HWND s_game_hwnd = nullptr;
    static HWND s_last_fg = nullptr;
    // [R457, 2026-11-11] Input census for the panel. The operator's report is "after switching
    // windows the mouse that clicked the HOME panel no longer works". Two very different causes
    // produce that: the window stopped receiving mouse messages (z-order / hidden), or it still
    // receives them and the pump forwards them to the game while ReShade's overlay wanted them.
    // These counters are what tells them apart in the next log.
    static std::atomic<unsigned long long> s_mouse_msgs{0};
    static std::atomic<unsigned long long> s_mouse_clicks{0};

    static HWND game_window_for_follow(HWND bridge)
    {
        // Authoritative first: the window the GAME's own swap chain was created against.
        HWND game = (HWND)mgpu::gpu1::dcomp_game_hwnd();
        if (game != nullptr && game != bridge && IsWindow(game)) return game;
        if (s_game_hwnd != nullptr && IsWindow(s_game_hwnd)) return s_game_hwnd;
        // Before that is known: the first foreground window of THIS process that is not the
        // bridge window is the game. Presenter's A1b rule, same reason - a launcher can own
        // the foreground for the first seconds and a hard-coded title match would miss.
        HWND fg = GetForegroundWindow();
        DWORD pid = 0;
        if (fg != nullptr && fg != bridge) GetWindowThreadProcessId(fg, &pid);
        if (fg != nullptr && fg != bridge && pid == GetCurrentProcessId())
            s_game_hwnd = fg;
        return s_game_hwnd;
    }

    // [H7/B-2, 2026-11-11] ROLLBACK WITHOUT A BINARY. `DispFollow=1` turns the foreground
    // follow ON; absent or 0 is the SHIPPED behaviour (the window keeps the topmost bit for the
    // whole session, exactly as before). DEFAULT 0 ON PURPOSE: the verified path must be the
    // default, and this new path is unverified until a run says otherwise. The test rig writes
    // DispFollow=1 explicitly. Read once - a launch-time switch, not a live knob, because the
    // only thing it can do mid-session is strand the window in one of the two states.
    static bool follow_enabled()
    {
        static int v = -1;
        if (v < 0)
        {
            v = (mgpu::gpu1::ui_ini_read("DispFollow", 0) != 0) ? 1 : 0;
            if (v == 0)
                mgpu::diag::info("[MGPU][R454] foreground follow DISABLED (DispFollow absent or 0): "
                                 "the neural window keeps HWND_TOPMOST for the whole session - the "
                                 "shipped behaviour, and Alt+Tab will NOT get it out of the way. "
                                 "Set DispFollow=1 to use the foreground follow.");
        }
        return v != 0;
    }

    // Runs on the bridge thread, which owns this window, from the present loop - the gate
    // sleeps at most 4 ms, so an Alt+Tab takes effect within one or two source frames even
    // while no neural frame is arriving.
    static void follow_foreground(HWND bridge, unsigned long long frame)
    {
        if (bridge == nullptr || !IsWindow(bridge)) return;
        if (!follow_enabled()) return;
        const HWND fg = GetForegroundWindow();
        const HWND game = game_window_for_follow(bridge);
        DWORD fpid = 0;
        if (fg != nullptr) GetWindowThreadProcessId(fg, &fpid);
        // This process owns the game AND the bridge window. The bridge counts as "ours" too:
        // WS_EX_NOACTIVATE keeps it out of the foreground, but if Windows ever hands it over,
        // hiding it would be a self-inflicted black screen.
        const bool game_fg = (fg != nullptr) &&
                             (fg == bridge || fg == game || fpid == GetCurrentProcessId());
        // [H7/B-2, 2026-11-11] DEBOUNCE, AND IT IS ABOUT THE CRASH, NOT ABOUT POLISH.
        //
        // The structural crash this project keeps meeting is a TRANSITION: game thread, bridge
        // thread and the present provider all changing state in the same instant, and a level
        // load is the canonical one. A hide/show is itself a transition of the present target,
        // so it must not be driven by a one-frame foreground flick (a loading screen grabbing
        // focus, a UAC prompt, the taskbar, an engine re-creating its window). Rules:
        //   * SHOW on the first frame the game owns the foreground - never late;
        //   * HIDE only after the game has been out of the foreground CONTINUOUSLY for 250 ms;
        //   * and never during the first 120 presents, so a launch/load sequence cannot hide
        //     the window while the chain and the provider are still settling.
        static const ULONGLONG kHideAfterMs = 250;
        static const unsigned long long kWarmupPresents = 120;
        static ULONGLONG unfocused_since = 0;
        if (game_fg) unfocused_since = 0;
        else if (unfocused_since == 0) unfocused_since = GetTickCount64();
        const ULONGLONG now_ms = GetTickCount64();
        const ULONGLONG out_ms = (unfocused_since != 0) ? (now_ms - unfocused_since) : 0;
        // [H7/B-3, 2026-11-11] AND NEVER WHILE THE CHAIN IS MOVING. A swapchain event is the
        // game resizing / re-creating its chain: a load, a resolution change, a fullscreen
        // toggle. That is precisely the window the structural crash lives in (game thread +
        // bridge thread + present provider all changing), so a hide must not be allowed to
        // land inside it. Keep the window up for 2 s after the last such event; the cost is
        // that an Alt+Tab right after a load leaves the neural picture up for those 2 s.
        const bool chain_moving = mgpu::adapter::ms_since_last_swapchain_event() < 2000ull;
        // [H7/E, 2026-11-11] PANEL + ALT+TAB: CLOSE FIRST, THEN HIDE.
        //
        // The 16:50 run measured both halves of the old design failing at once:
        //   * while the game held the foreground its mirrored overlay captured the mouse
        //     (wantCapture=1) and the bridge window - the one on screen - stopped receiving input
        //     altogether ([R457] froze at 167 msgs / 2 clicks for the whole session);
        //   * with the panel open, Alt+Tab could not switch windows at all because the R156 guard
        //     refused to hide under an open overlay.
        // dllmain now blocks the GAME's overlay in follow mode: it is invisible by construction
        // (the bridge window covers it) and its only observable effect was that capture. What is
        // left is the bridge runtime's overlay, the visible one. When the game loses the
        // foreground, ask THAT runtime to close it on its own thread (R158's pending mechanism),
        // keep the window up until it has closed, then hide normally.
        const bool ov_open = mgpu::gpu1::bridge_overlay_open();
        bool panel_hold = false;
        static ULONGLONG panel_close_at = 0;
        static bool panel_wait_said = false;
        if (game_fg || !ov_open)
        {
            if (panel_close_at != 0)
            {
                mgpu::gpu1::clear_overlay_close_request();
                panel_close_at = 0;
            }
            panel_wait_said = false;
        }
        if (ov_open)
        {
            if (game_fg)
            {
                panel_hold = true;
            }
            else
            {
                const ULONGLONG t = GetTickCount64();
                if (panel_close_at == 0)
                {
                    panel_close_at = t;
                    mgpu::gpu1::request_overlay_close();
                    mgpu::diag::info("[MGPU][R454] panel open and the game lost the foreground: closing "
                                     "the overlay on its own runtime before hiding the window (never hide "
                                     "under an open overlay - the R156 defect). Alt+Tab gets the window "
                                     "back as soon as it has closed.");
                }
                panel_hold = true;
                if (!panel_wait_said && (t - panel_close_at) > 1500ull)
                {
                    panel_wait_said = true;
                    mgpu::diag::warn("[MGPU][R454] the overlay did not close within 1.5 s (its runtime is "
                                     "not presenting). The window stays visible - fail-closed: hiding "
                                     "under an open overlay is what R156 measured as broken.");
                }
            }
        }
        if (mgpu::gpu1::ui_panel_is_open()) panel_hold = true;   // simple panel: no close path of its own
        const bool want = game_fg || (out_ms < kHideAfterMs) || (frame < kWarmupPresents) ||
                          chain_moving || panel_hold;
        // Say it when the hold is what kept the window up, or the operator reads "Alt+Tab did
        // not work" when the code was deliberately refusing during a load/resize.
        static bool hold_said = false;
        if (chain_moving && !game_fg && !hold_said)
        {
            hold_said = true;
            char hl[260];
            snprintf(hl, sizeof hl,
                     "[MGPU][R454] hide HELD: the game's chain moved %llu ms ago (load/resize/"
                     "fullscreen). The window stays up for 2 s after the last swapchain event - "
                     "that is the transition this code refuses to hide inside.",
                     (unsigned long long)mgpu::adapter::ms_since_last_swapchain_event());
            mgpu::diag::info(hl);
        }
        if (game_fg) hold_said = false;
        const bool visible = IsWindowVisible(bridge) != 0;
        const bool changed = (want != visible);
        // [R455] ONE LINE THAT SETTLES "WHY CAN I STILL NOT SWITCH". A game window that is
        // itself WS_EX_TOPMOST stays above every ordinary app after Alt+Tab no matter what this
        // window does; if the next run still cannot switch, this line says whether the game, the
        // bridge, both or neither is the topmost one.
        static bool style_said = false;
        if (!style_said && game != nullptr)
        {
            style_said = true;
            const LONG_PTR gex = GetWindowLongPtrW(game, GWL_EXSTYLE);
            const LONG_PTR bex = GetWindowLongPtrW(bridge, GWL_EXSTYLE);
            char sl[300];
            snprintf(sl, sizeof sl,
                     "[MGPU][R455] window styles: game hwnd=0x%p ex=0x%08X topmost=%d | bridge "
                     "ex=0x%08X topmost=%d. A topmost GAME window is above every normal app after "
                     "Alt+Tab and this follow code cannot change that.",
                     (void *)game, (unsigned)gex, ((gex & WS_EX_TOPMOST) != 0) ? 1 : 0,
                     (unsigned)bex, ((bex & WS_EX_TOPMOST) != 0) ? 1 : 0);
            mgpu::diag::info(sl);
        }
        if (changed)
        {
            // Synchronous on purpose: this thread OWNS the window, and the present loop only
            // drains messages once per iteration - ShowWindowAsync would leave IsWindowVisible
            // stale for a beat and this function would log the same transition again.
            ShowWindow(bridge, want ? SW_SHOWNOACTIVATE : SW_HIDE);
            // The system cursor follows the window that owns the screen. Hidden: give the
            // OTHER app a usable arrow at once (WM_SETCURSOR will not reach us again while we
            // are hidden). Shown: re-apply DispCursor immediately instead of waiting for the
            // next mouse move, or the arrow would sit on the neural picture for a moment.
            if (want)
                mgpu::gpu1::disp_cursor_raise();
            else
                SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            char l[420];
            snprintf(l, sizeof l,
                     "[MGPU][R454] output %s (fg hwnd=0x%p pid=%lu game=0x%p unfocused=%llums "
                     "frame=%llu). %s",
                     want ? "shown" : "hidden", (void *)fg, (unsigned long)fpid, (void *)game,
                     (unsigned long long)out_ms, (unsigned long long)frame,
                     want ? "The game holds the foreground: topmost again."
                          : "Foreground is not the game: the window is out of the way and Alt+Tab "
                            "works. The present chain and the stream are untouched.");
            mgpu::diag::info(l);
        }
        // Topmost on the transition, on any foreground change, and - like the presenter - once
        // every 32 presented frames, because a game window that is itself topmost can end up in
        // front of us without the foreground ever changing (that reads as flicker, not as lag).
        if (want && (changed || fg != s_last_fg || (frame % 32ull) == 0ull))
            SetWindowPos(bridge, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER |
                             SWP_SHOWWINDOW | SWP_ASYNCWINDOWPOS);
        s_last_fg = fg;
    }

    LRESULT CALLBACK bridge_wndproc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
        // [H7/A-c, 2026-11-11] THE WINDOW NEVER *OWNS* THE CURSOR; it may still SET it.
        //
        // DcompOverlay=0 makes this the visible fullscreen window, so Windows asks IT what
        // the cursor should be. The game draws its pointer INTO the frame; the system arrow
        // must therefore be hidden, not left at whatever the desktop had. DispCursor=0
        // (default here, and the PBZ value) hides the system cursor; =1 shows the arrow.
        case WM_SETCURSOR:
            if (input_pump_enabled())
            {
                // Read LIVE and APPLY: DispCursor is also changed by CTRL+ALT+F5 and by the
                // panel row, and those call disp_cursor_apply() themselves - a stationary
                // pointer never sends this message, which is why the first hotkey version
                // changed the flag and nothing on screen.
                (void)mgpu::gpu1::disp_cursor_apply();
            }
            return TRUE;
        // [MOUSE PUMP] Never activate: a click must not take the foreground away from the
        // game, or keyboard/raw-input die (PBZ's own note (3)).
        case WM_MOUSEACTIVATE:
            if (input_pump_enabled()) return MA_NOACTIVATE;
            return DefWindowProcW(hWnd, msg, wParam, lParam);
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
            break;
        }

        // [MOUSE PUMP] The forwarding itself, exactly the presenter's shape: client ->
        // screen -> the game's client, then PostMessage. Wheel coordinates are already
        // screen coordinates, so that case skips ClientToScreen.
        if (input_pump_enabled())
        {
            HWND game = input_game_window();
            if (game != nullptr && game != hWnd)
            {
                switch (msg)
                {
                case WM_MOUSEMOVE:
                case WM_LBUTTONDOWN: case WM_LBUTTONUP:
                case WM_RBUTTONDOWN: case WM_RBUTTONUP:
                case WM_MBUTTONDOWN: case WM_MBUTTONUP:
                case WM_XBUTTONDOWN: case WM_XBUTTONUP:
                {
                    POINT pt{ (LONG)(short)LOWORD(lParam), (LONG)(short)HIWORD(lParam) };
                    s_mouse_msgs.fetch_add(1ull, std::memory_order_relaxed);
                    if (msg != WM_MOUSEMOVE) s_mouse_clicks.fetch_add(1ull, std::memory_order_relaxed);
                    ClientToScreen(hWnd, &pt);
                    ScreenToClient(game, &pt);
                    PostMessageW(game, msg, wParam, MAKELPARAM((short)pt.x, (short)pt.y));
                    return 0;
                }
                case WM_MOUSEWHEEL:
                case WM_MOUSEHWHEEL:
                {
                    POINT pt{ (LONG)(short)LOWORD(lParam), (LONG)(short)HIWORD(lParam) };   // screen coords
                    ScreenToClient(game, &pt);
                    PostMessageW(game, msg, wParam, MAKELPARAM((short)pt.x, (short)pt.y));
                    return 0;
                }
                default:
                    break;
                }
            }
        }
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }

    // ---- P7.10: where the window is born ----
    //
    // THE DEFECT. The window was created at CW_USEDEFAULT and the P7.3 fit code
    // then called MonitorFromWindow to learn which panel it had landed on. That
    // is an inference from where Windows happened to place it, and Windows
    // places it on the primary display - which on the rig this was built on is
    // the GAME's panel. So the window sized itself correctly to the wrong
    // monitor, every launch, and had to be dragged across by hand. It is not
    // cosmetic: presenting GPU 1's output on GPU 0's panel is the cross-adapter
    // present the whole topology exists to avoid, and it puts the thing being
    // measured behind the thing being measured.
    //
    // THE FIX. The bridge adapter knows which outputs it drives. Ask it. That
    // is a statement about the hardware rather than a guess about window
    // placement, and it is the same adapter pointer T2 selected and T3 created
    // the device on, so the window cannot end up on a different card than the
    // neural work by construction.
    //
    // Everything here can only withhold a placement, never produce a wrong one:
    // any failure leaves the origin at CW_USEDEFAULT, which is exactly the old
    // behaviour, and says so in the log.
    // The desktop's own monitor list, used when the adapter will not name its
    // outputs. EnumDisplayMonitors reports what the desktop actually spans,
    // which is independent of how DXGI attributes outputs to adapters.
    struct desktop_monitors
    {
        RECT rect[8]{};
        bool primary[8]{};
        UINT count = 0;
    };

    BOOL CALLBACK monitor_enum_cb(HMONITOR mh, HDC, LPRECT, LPARAM lp)
    {
        desktop_monitors *d = reinterpret_cast<desktop_monitors *>(lp);
        if (d->count >= 8) return FALSE;
        MONITORINFO mi{}; mi.cbSize = sizeof mi;
        if (GetMonitorInfoW(mh, &mi) != FALSE)
        {
            d->rect[d->count]    = mi.rcMonitor;
            d->primary[d->count] = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
            ++d->count;
        }
        return TRUE;
    }

    void enumerate_desktop_monitors(desktop_monitors &out)
    {
        EnumDisplayMonitors(nullptr, nullptr, monitor_enum_cb,
                            reinterpret_cast<LPARAM>(&out));
    }

    struct bridge_placement
    {
        bool known = false;
        int  x = CW_USEDEFAULT, y = CW_USEDEFAULT;
        UINT outputs_total = 0;      // outputs the adapter reports
        UINT outputs_attached = 0;   // of those, attached to the desktop
        char detail[320] = {};
    };

    void pick_bridge_placement(const mgpu::adapter::selection_result &sel,
                               bridge_placement &out)
    {
        const int want = mgpu::gpu1::monitor_index();   // -1 = auto

        IDXGIAdapter1 *ad1 = static_cast<IDXGIAdapter1 *>(sel.selected_adapter);
        if (ad1 == nullptr)
        {
            snprintf(out.detail, sizeof out.detail,
                     "no adapter reference - window placed by Windows (CW_USEDEFAULT)");
            return;
        }

        // Collect the adapter's outputs that are actually attached to the
        // desktop. An output that exists but is not attached has no desktop
        // rectangle to place a window in, so it is counted and skipped.
        RECT rects[8]{};
        char names[8][40]{};
        UINT n_attached = 0;

        for (UINT i = 0; i < 32; ++i)
        {
            IDXGIOutput *o = nullptr;
            if (FAILED(ad1->EnumOutputs(i, &o)) || o == nullptr)
                break;   // DXGI_ERROR_NOT_FOUND: this adapter has no more
            ++out.outputs_total;
            DXGI_OUTPUT_DESC d{};
            if (SUCCEEDED(o->GetDesc(&d)) && d.AttachedToDesktop && n_attached < 8)
            {
                rects[n_attached] = d.DesktopCoordinates;
                WideCharToMultiByte(CP_UTF8, 0, d.DeviceName, -1,
                                    names[n_attached], 39, nullptr, nullptr);
                names[n_attached][39] = '\0';
                ++n_attached;
            }
            o->Release();
        }
        out.outputs_attached = n_attached;

        if (n_attached == 0)
        {
            // DXGI SAYS THIS ADAPTER DRIVES NOTHING. Believe it about the
            // adapter; do not believe it about the machine.
            //
            // Measured on the development rig: a second card with a monitor
            // physically attached and lit reported outputs=0 through
            // EnumOutputs, while the render adapter reported outputs=2. DXGI
            // attributes outputs to the adapter that owns the desktop
            // composition for them, which on a multi-GPU desktop is not
            // reliably the card the cable is in. So the adapter path is the
            // preferred answer and NOT the only one - when it comes back
            // empty, fall through to the desktop's own monitor list.
            //
            // The fallback deliberately does NOT try to work out which card
            // owns which panel, because that is the question DXGI just failed
            // to answer. It uses the only thing that is reliably true: the
            // bridge window should not open on the display the game is using.
            // On a two-monitor rig that is one candidate, which is the whole
            // problem people actually have.
            desktop_monitors mons;
            enumerate_desktop_monitors(mons);

            if (mons.count == 0)
            {
                snprintf(out.detail, sizeof out.detail,
                         "the bridge adapter reports no attached output (%u reported) and no "
                         "desktop monitor could be enumerated either - window placed by Windows",
                         out.outputs_total);
                return;
            }

            UINT pick = 0;
            const char *how = nullptr;
            if (want >= 0 && (UINT)want < mons.count)
            {
                pick = (UINT)want;
                how = "Monitor= from mgpu.ini, indexing DESKTOP monitors (the adapter reported "
                      "none of its own)";
            }
            else
            {
                // First non-primary monitor. The game is on the primary one on
                // essentially every rig this will meet, and "not where the game
                // is" is the requirement - not "which card owns it".
                bool found = false;
                for (UINT i = 0; i < mons.count; ++i)
                    if (!mons.primary[i]) { pick = i; found = true; break; }
                if (!found)
                {
                    snprintf(out.detail, sizeof out.detail,
                             "the bridge adapter reports no attached output and this desktop has "
                             "only one monitor - running HEADLESS or single-screen. This works, "
                             "but a monitor on the second card was worth +33%% throughput and "
                             "roughly half the latency on the rig this was measured on. Window "
                             "placed by Windows");
                    return;
                }
                how = "auto, from the DESKTOP monitor list because the bridge adapter reported "
                      "no outputs of its own: the first non-primary monitor";
            }

            out.known = true;
            out.x = (int)mons.rect[pick].left;
            out.y = (int)mons.rect[pick].top;
            snprintf(out.detail, sizeof out.detail,
                     "%s - monitor %u of %u, rect (%d,%d)-(%d,%d). CHECK THIS ONE: it is placed "
                     "away from the primary display, not proven to be on the second card, because "
                     "DXGI would not say. Set Monitor=<n> if it picked wrong",
                     how, pick, mons.count,
                     (int)mons.rect[pick].left, (int)mons.rect[pick].top,
                     (int)mons.rect[pick].right, (int)mons.rect[pick].bottom);
            return;
        }

        UINT pick = 0;
        const char *how = "auto: the bridge adapter's first attached output";
        if (want >= 0)
        {
            if ((UINT)want < n_attached)
            {
                pick = (UINT)want;
                how = "Monitor= from mgpu.ini";
            }
            else
            {
                // Refuse the index, do not silently substitute a different
                // panel: a setting that quietly does something else is worse
                // than one that fails loudly.
                how = "Monitor= in mgpu.ini names an output this adapter does not have - "
                      "falling back to its first attached output";
            }
        }

        out.known = true;
        out.x = (int)rects[pick].left;
        out.y = (int)rects[pick].top;
        snprintf(out.detail, sizeof out.detail,
                 "%s: output %u of %u attached (%u reported) \"%s\" desktop rect "
                 "(%d,%d)-(%d,%d) - the window is created ON THE CARD THAT DID THE NEURAL WORK, "
                 "so nothing crosses back over the link to be displayed",
                 how, pick, n_attached, out.outputs_total, names[pick],
                 (int)rects[pick].left, (int)rects[pick].top,
                 (int)rects[pick].right, (int)rects[pick].bottom);
    }

    // ---- P7.10: the title bar as the status line ----
    //
    // The window is the only surface a user sees without opening an overlay or
    // reading a log, and it spent every milestone up to here saying the same
    // eight characters whatever the bridge was doing. Now it answers the two
    // questions people actually ask of it - is it armed, and is the neural
    // stage running - plus the one thing that is a hard limit rather than a
    // setting.
    void set_window_title(HWND hwnd, unsigned long long frame)
    {
        if (hwnd == nullptr) return;

        mgpu::gpu1::ui_state st;
        mgpu::gpu1::ui_read(st);

        char t[220];
        if (!st.armed)
            snprintf(t, sizeof t,
                     "MGPU Bridge (GPU 1) - NOT ARMED - press CTRL+ALT+F10 in gameplay "
                     "| D3D12 only");
        else if (st.summarised)
            snprintf(t, sizeof t,
                     "MGPU Bridge (GPU 1) - finished, ran to its bound | D3D12 only");
        else
            snprintf(t, sizeof t,
                     "MGPU Bridge (GPU 1) - armed | x%u | neural %s | %llu frames | D3D12 only",
                     st.passes, st.neural ? (st.nr_ok ? "ON" : "requested") : "off",
                     (unsigned long long)st.consumed);

        wchar_t w[220];
        if (MultiByteToWideChar(CP_UTF8, 0, t, -1, w, 220) != 0)
            SetWindowTextW(hwnd, w);
        (void)frame;
    }

    unsigned __stdcall bridge_main(void *arg)
    {
        (void)arg;
        mgpu::strace::note_bridge_thread();   // R313: first thing - this thread's id (DcompSafeMode and the trace read it)
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
        // R246: the startup NGX probe, deferred (see gpu1_context.hpp).
        bool      ngx_probe_pending = false;
        int       r282_state = 0;   // R282: 0 not decided, 1 holding for the game's scene, 2 no hold, 3 released
        ULONGLONG ngx_probe_t0 = 0;
        const unsigned long long NGX_PROBE_DEFER_MAX_MS = 15000;
        // R247: reload-and-retry after a refused first Init (gpu1_context.hpp).
        bool               ngx_retry_pending = false;
        unsigned           ngx_retry_n = 0;
        const unsigned     NGX_RETRY_MAX = 3;
        const unsigned long long NGX_RETRY_GAP_FRAMES = 30;    // game frames between tries (R247)
        const unsigned long long NGX_RETRY_GAP_MAX_MS = 2000;  // bound if the game stops rendering (R247)
        // R248: tries are spaced by seconds (the R247 run put all three inside
        // 900 ms, which tested fresh modules, not later time), and the first
        // Init and each retry wait, bounded, while a previous instance of this
        // executable is still alive.
        const unsigned long long NGX_RETRY_GAP_MS = 3000;
        const unsigned long long NGX_PREV_INSTANCE_WAIT_MAX_MS = 20000;
        bool               ngx_prev_wait_logged = false;
        ULONGLONG          ngx_prev_wait_t0 = 0;
        unsigned long long ngx_retry_at_frames = 0;
        ULONGLONG          ngx_retry_t0 = 0;
        bool hotkey_ok = false;            // P1.3g: CTRL+ALT+F10 registered
        unsigned manual_runs = 0;          // P1.3g: how many on-demand runs so far

        if (have_device)
        {
            // Requirement 1: register the window class on the bridge thread,
            // hInstance = the add-on's own HMODULE (not the game's).
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.style = CS_HREDRAW | CS_VREDRAW;
            wc.lpfnWndProc = bridge_wndproc;
            wc.hInstance = mgpu::module_handle();
                            // [H7/A-c] NO class cursor: this window must never paint a desktop arrow over the picture.
                wc.hCursor = nullptr;
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

                // P7.10: the origin comes from the BRIDGE ADAPTER's own output,
                // not from wherever Windows would have put the window. It is
                // computed before CreateWindowExW rather than corrected after,
                // because the P7.3 fit code reads the window's monitor - move
                // the window later and the size has already been decided
                // against the wrong panel.
                bridge_placement place;
                pick_bridge_placement(sel, place);
                // Its own buffer: the enclosing `line` is 320 bytes and this
                // message is longer. A truncated log line is a wrong answer
                // that looks like a right one - the same reason the P1.3g
                // hotkey message has a buffer of its own.
                char pline[420];
                // V55. Hand the one fact the hint needs to the side that draws
                // the screen.
                //
                // outputs_ATTACHED, not outputs_total. total counts every
                // output DXGI enumerates for the adapter; attached counts only
                // those with DXGI_OUTPUT_DESC::AttachedToDesktop. A render card
                // sitting headless still has physical connectors, so total can
                // be non-zero on exactly the rig this hint is for - and the
                // hint would then never appear for the people it was written
                // for. Headless is attached == 0 and nothing else.
                mgpu::gpu1::note_bridge_headless(place.outputs_attached == 0);

                snprintf(pline, sizeof pline, "[MGPU][P7.10] window placement - %s", place.detail);
                if (mgpu::strace::on()) mgpu::strace::ev(mgpu::strace::R_BT, "window-placement", place.outputs_attached == 0 ? "bridge adapter headless" : "bridge adapter has outputs");   // R313
                if (place.known) mgpu::diag::info(pline);
                else             mgpu::diag::warn(pline);

                // T5 (section 09): no WS_VISIBLE. A window created visible
                // takes foreground activation from the game the moment it
                // appears, and the game's borderless-fullscreen presentation
                // drops to windowed-with-borders with the taskbar showing.
                // The window is shown without activation below.
                // ---- V18: NoActivate. EXPERIMENTAL, OFF BY DEFAULT. ----
                //
                // The window is already created hidden and shown with
                // SW_SHOWNOACTIVATE, so it does not steal foreground when it
                // APPEARS. But its extended style is 0, which means a CLICK on
                // it activates it and takes foreground from the game - and a
                // game that is not foreground stops taking input, pad included.
                //
                // WS_EX_NOACTIVATE makes the window refuse activation
                // altogether: clicks do not bring it forward and the game keeps
                // the foreground, so keyboard, mouse and XInput keep working
                // while the bridge output is on screen.
                //
                // WS_EX_TOOLWINDOW goes with it to keep the window out of
                // Alt+Tab, for the same reason - an Alt+Tab that lands on the
                // bridge window is the same lost foreground by another route.
                //
                // AUTO BY DEFAULT, and only in DUPLICATE mode. The style only
                // matters when the bridge output shares a picture with the game -
                // in extended mode the two are on different screens and taking
                // foreground is normal, wanted behaviour. window_no_activate()
                // decides: NoActivate=0 off, 1 always, 2 auto (the default),
                // where auto asks QueryDisplayConfig whether one display source
                // is driving two targets.
                DWORD ex_style = 0;
                if (mgpu::gpu1::window_no_activate())
                {
                    ex_style = WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
                    mgpu::diag::info("[MGPU][T4] the bridge window is created "
                                     "WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW. It will not take "
                                     "foreground from the game when clicked and will not appear "
                                     "in Alt+Tab, so the game keeps keyboard, mouse and pad. "
                                     "The line above says whether this was forced or detected. "
                                     "TRADEOFF: the ReShade overlay on THIS window needs focus "
                                     "to take keyboard, so use the GAME's overlay instead - it "
                                     "is the same panel.");
                }

                // [H7/A-b, 2026-11-11] THIS WINDOW IS THE SCREEN NOW. It must not take the
                // FOREGROUND (keyboard/raw input stay the game's), but with the presenter's
                // pump it DOES take the mouse and forwards it.
                //
                // [MOUSE PUMP, 2026-11-11] DispClickThrough=0 is the PBZ setting and the
                // presenter's proven one: the visible window receives the mouse hits and
                // posts them to the game's client area. =1 keeps the old pass-through arm
                // (no pump, no forwarding) as the rollback.
                if (mgpu::gpu1::dcomp_overlay_mode() == 0)
                {
                    ex_style |= WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
                    const bool click_through = mgpu::gpu1::ui_ini_read("DispClickThrough", 0) != 0;
                    if (click_through) ex_style |= WS_EX_TRANSPARENT;
                    snprintf(line, sizeof line,
                             "[MGPU][T4][A] DcompOverlay=0: this window is on screen, created "
                             "WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW%s. DispClickThrough=%d: %s",
                             click_through ? " | WS_EX_TRANSPARENT" : "",
                             click_through ? 1 : 0,
                             click_through
                                 ? "mouse hits pass THROUGH to the game (old arm, no pump)"
                                 : "the window takes the mouse and forwards it to the game "
                                   "(presenter pump; PBZ's proven value)");
                    mgpu::diag::info(line);
                }

                hwnd = CreateWindowExW(
                    ex_style,
                    class_name_w,
                    L"MGPU Bridge (GPU 1) - starting | D3D12 only",
                    wnd_style,
                    place.x, place.y,
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
                    if (mgpu::strace::on()) { char d[64]; snprintf(d, sizeof d, "bridge hwnd=0x%p 1280x720", (void *)hwnd); mgpu::strace::ev(mgpu::strace::R_BT, "window-created", d); }   // R313

                    // T5 (section 09): show the window without stealing
                    // activation.
                    //
                    // ShowWindow's return value is NOT success or failure.
                    // It is nonzero if the window was PREVIOUSLY VISIBLE and
                    // zero if it was previously hidden. This window is
                    // created without WS_VISIBLE, so it always returns FALSE
                    // here, with GetLastError() == 0 - and an earlier version
                    // of this code logged that as an error on every single
                    // launch. There is nothing to check.
                    // ---- V49: A GHOST IS NEVER SHOWN ----
                    //
                    // The first cut of V49 replaced the bridge's SWAPCHAIN and
                    // left this line alone - so there was still an HWND in the
                    // Z-order for an engine to see, and still a window you
                    // could swap to and find a cursor on. A windowless
                    // swapchain behind a window that is still there is not
                    // windowless.
                    //
                    // The window is not destroyed, only never shown: the
                    // message pump, the resize path and the title updates all
                    // still hang off it, and a hidden window costs nothing and
                    // participates in nothing. In mode 0 this line runs exactly
                    // as it always has.
                    // R273b: the first thing on this thread that needs the
                    // mode. Settled here, once, bounded (the V49 wait's 2 s),
                    // so the window, the hotkeys and the present chain below
                    // all read one answer - whoever asked first.
                    const double r313_s0 = mgpu::strace::now_ms();
                    const bool r313_mode = mgpu::gpu1::dcomp_overlay_mode_settled(2000u);
                    if (mgpu::strace::on()) { char d[96]; snprintf(d, sizeof d, "mode=%d, settled in %.1f ms on this thread", r313_mode ? 1 : 0, mgpu::strace::now_ms() - r313_s0); mgpu::strace::ev(mgpu::strace::R_BT, "mode-settled", d); }   // R313
                    if (!r313_mode)
                    {
                        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
                        if (mgpu::strace::on()) mgpu::strace::ev(mgpu::strace::R_BT, "window-shown", "mode 0: ShowWindow(SW_SHOWNOACTIVATE)");
                    }
                    else
                    {
                        if (mgpu::strace::on()) mgpu::strace::ev(mgpu::strace::R_BT, "window-never-shown", "mode 1: hidden, owns the pump only");
                        mgpu::diag::info("[MGPU][V49] the bridge window is created but NEVER "
                                         "SHOWN - it exists only to own the message pump and the "
                                         "resize path. Nothing of the bridge is in the Z-order. "
                                         "If you can swap to a bridge window, this line did not "
                                         "run.");
                    }

                    // P7.10: say what it is doing straight away. The present
                    // loop refreshes this every half second, but if the present
                    // chain fails to create, this is the only title the window
                    // will ever have - and "starting" forever is a lie.
                    set_window_title(hwnd, 0);

                    // P1.3g. A GLOBAL hotkey, deliberately - not a key handled by
                    // the bridge window. A window-scoped key would force the
                    // bridge window to be focused before it could be pressed,
                    // which is one of the three focus conditions we want to be
                    // able to VARY. RegisterHotKey delivers WM_HOTKEY to this
                    // thread's queue whatever holds the foreground, so the game
                    // can stay focused, or the desktop, and the probe still fires.
                    //
                    // Must be registered on the thread that pumps, which is this
                    // one. Ctrl+Alt+F10 rather than a bare function key: ReShade
                    // owns Home, and games claim unmodified F-keys freely.
                    // Failure is logged and non-fatal - the startup run still
                    // happens, we just lose manual triggering.
                    if (hwnd != nullptr)
                    {
                        if (RegisterHotKey(nullptr, MGPU_HOTKEY_ID,
                                           MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F10) != FALSE)
                        {
                            hotkey_ok = true;
                            // P6.3: the intensity keys. Each is independent -
                            // one failing does not cost the others, and the log
                            // names exactly which are available, because a key
                            // that silently did not register is a knob the
                            // operator will press and believe.
                            const bool k_t = RegisterHotKey(nullptr, MGPU_HOTKEY_INT_TARGET,
                                                MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F8) != FALSE;
                            const bool k_d = RegisterHotKey(nullptr, MGPU_HOTKEY_INT_DOWN,
                                                MOD_CONTROL | MOD_ALT, VK_F9) != FALSE;
                            const bool k_u = RegisterHotKey(nullptr, MGPU_HOTKEY_INT_UP,
                                                MOD_CONTROL | MOD_ALT, VK_F11) != FALSE;
                            const bool k_p =
                                mgpu::gpu1::dcomp_overlay_mode() &&
                                RegisterHotKey(nullptr, MGPU_HOTKEY_PEEK,
                                    MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F6) != FALSE;
                            // Registered independently of the peek key, like every other
                            // hotkey here: one failing must not cost the others.
                            const bool k_s = RegisterHotKey(nullptr, MGPU_HOTKEY_SIMPLE,
                                                MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F4) != FALSE;
                            if (k_s)
                                mgpu::diag::info("[MGPU][R330] simple panel hotkey: CTRL+ALT+F4 switches the "
                                                 "panel between the full view and the simple one. It writes "
                                                 "PanelSimple to mgpu.ini, so the choice survives a restart.");   // R330
                            else
                                mgpu::diag::warn("[MGPU][R330] simple panel hotkey CTRL+ALT+F4 FAILED to "
                                                 "register - the checkbox in the full panel still switches it.");
                            // [MOUSE] Live cursor toggle. The arrow is needed in menus and
                            // in the way in an FPS; one key, no restart, persisted to the ini.
                            const bool k_c = RegisterHotKey(nullptr, MGPU_HOTKEY_CURSOR,
                                                MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F5) != FALSE;
                            if (k_c)
                                mgpu::diag::info("[MGPU][MOUSE] desktop-cursor hotkey: CTRL+ALT+F5 toggles "
                                                 "DispCursor (show/hide the system arrow); the simple panel has "
                                                 "a row with the same switch.");
                            else
                                mgpu::diag::warn("[MGPU][MOUSE] desktop-cursor hotkey CTRL+ALT+F5 FAILED to "
                                                 "register - the simple panel row still toggles DispCursor.");
                            if (mgpu::gpu1::dcomp_overlay_mode())
                            {
                                char pk[440];
                                snprintf(pk, sizeof pk,
                                         "[MGPU][V52] peek hotkey: CTRL+ALT+F6 hides and restores "
                                         "the bridge visual = %s. DcompOverlay=1 puts the bridge "
                                         "ABOVE the game's own ReShade overlay, so this is how "
                                         "you reach that overlay - and how you dismiss the "
                                         "run-end screen. It unroots a visual and roots it again; "
                                         "the stream is never touched.",
                                         k_p ? "OK" : "FAILED");
                                if (k_p) mgpu::diag::info(pk);
                                else     mgpu::diag::warn(pk);
                            }
                            const bool k_v = RegisterHotKey(nullptr, MGPU_HOTKEY_VIEW,
                                                MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F7) != FALSE;
                            const bool s_l  = RegisterHotKey(nullptr, MGPU_HOTKEY_SEAM_L,
                                                MOD_CONTROL | MOD_ALT, VK_LEFT) != FALSE;
                            const bool s_r  = RegisterHotKey(nullptr, MGPU_HOTKEY_SEAM_R,
                                                MOD_CONTROL | MOD_ALT, VK_RIGHT) != FALSE;
                            const bool s_lc = RegisterHotKey(nullptr, MGPU_HOTKEY_SEAM_LC,
                                                MOD_CONTROL | MOD_ALT | MOD_SHIFT, VK_LEFT) != FALSE;
                            const bool s_rc = RegisterHotKey(nullptr, MGPU_HOTKEY_SEAM_RC,
                                                MOD_CONTROL | MOD_ALT | MOD_SHIFT, VK_RIGHT) != FALSE;
                            char sk[400];
                            snprintf(sk, sizeof sk,
                                     "[MGPU][P7.5] seam hotkeys: CTRL+ALT+LEFT/RIGHT move the split "
                                     "seam = %s/%s | add SHIFT for a coarse step = %s/%s. They "
                                     "repeat when held and need NO overlay open, so the seam can be "
                                     "dragged across a face while the game is the only thing on "
                                     "screen - which is the point of having them at all.",
                                     s_l ? "OK" : "FAILED", s_r ? "OK" : "FAILED",
                                     s_lc ? "OK" : "FAILED", s_rc ? "OK" : "FAILED");
                            mgpu::diag::info(sk);
                            char vk[300];
                            snprintf(vk, sizeof vk,
                                     "[MGPU][P7.4] view hotkey: CTRL+ALT+F7 cycles output -> input "
                                     "-> split = %s. Split shows the frame handed TO the model on "
                                     "the left and what it produced on the right, THE SAME FRAME, "
                                     "so the two halves cannot disagree about time, camera or "
                                     "lighting. The neural stage is unchanged in all three.",
                                     k_v ? "OK" : "FAILED");
                            mgpu::diag::info(vk);
                            char ik[420];
                            snprintf(ik, sizeof ik,
                                     "[MGPU][P6.3] intensity hotkeys: CTRL+ALT+F8 cycle target "
                                     "(all/p1/p2/...) = %s | CTRL+ALT+F9 down = %s | CTRL+ALT+F11 "
                                     "up = %s. Steps of 0.05, clamped 0.00-2.00, live from the "
                                     "next frame - NGX parameters are set per evaluate, so no "
                                     "re-arm and no relaunch. DOWN and UP repeat when held; the "
                                     "target key does not.",
                                     k_t ? "OK" : "FAILED", k_d ? "OK" : "FAILED",
                                     k_u ? "OK" : "FAILED");
                            if (k_t && k_d && k_u) mgpu::diag::info(ik);
                            else                   mgpu::diag::warn(ik);
                            mgpu::diag::info("[MGPU][P1.3g] hotkey registered: CTRL+ALT+F10 runs the "
                                             "transit probe on demand, from any foreground window. "
                                             "Press it once the game has settled - in gameplay, not "
                                             "the menu - and again with a different window focused; "
                                             "each run labels itself and records which window held "
                                             "the foreground.");
                        }
                        else
                        {
                            char hk[256];
                            snprintf(hk, sizeof hk,
                                     "[MGPU][P1.3g] RegisterHotKey failed (GetLastError=%lu) - "
                                     "CTRL+ALT+F10 is probably owned by another process. Manual runs "
                                     "unavailable this launch; the startup run is unaffected.",
                                     (unsigned long)GetLastError());
                            mgpu::diag::warn(hk);
                        }
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
                        // R246: not here any more unless NgxProbeAtStart=1.
                        // Here is ~1 s into the process, while the game is
                        // still in its own startup, and that is the window
                        // in which the core answers OutOfDate and the answer
                        // sticks for the process. Deferred to the present
                        // loop, after the game's first rendered frame.
                        if (mgpu::gpu1::ngx_probe_at_start())
                        {
                            mgpu::diag::info("[MGPU][R246] NgxProbeAtStart=1: the NGX probe runs now, "
                                             "before the present loop (the pre-R246 place).");
                            (void)mgpu::gpu1::ngx_probe(1280, 720);
                            mgpu::gpu1::note_ngx_probe_ran();   // R290
                            if (mgpu::gpu1::ngx_probe_init_was_outofdate())
                            {
                                ngx_retry_pending = true; ngx_retry_n = 0;
                                ngx_retry_at_frames = mgpu::gpu1::game_effects_frames() + NGX_RETRY_GAP_FRAMES;
                                ngx_retry_t0 = GetTickCount64();
                            }
                        }
                        else
                        {
                            ngx_probe_pending = true;
                            ngx_probe_t0 = GetTickCount64();
                            if (mgpu::strace::on()) mgpu::strace::ev(mgpu::strace::R_BT, "ngx-probe-deferred", "R246: runs from the loop after the game's first frame");   // R313
                            mgpu::diag::info("[MGPU][R246] NGX probe DEFERRED: it runs from the present "
                                             "loop once the game has delivered its first frame "
                                             "(finish_effects), or after 15 s, whichever comes first. "
                                             "The first NGX Init in this process is then made after "
                                             "the game's own startup, not during it. NgxProbeAtStart=1 "
                                             "restores the old place.");
                        }

                        // P1.3: the first milestone that touches the bus.
                        // Runs whatever the NGX probe reported - the two
                        // are independent questions, and a run that
                        // answers only one of them is still worth having.
                        //
                        // R302: the startup run is now behind TransitProbes=1
                        // (its own key, off by default). Before R302 it ran on
                        // every launch with no key to stop it. Far Cry 6 left
                        // the process 245-295 ms after the last queued P1.3
                        // line (A'.3b, heap desc), twice, with no crash dump -
                        // and this probe works on the GAME's own device
                        // (D3D12CreateDevice returns the existing device for an
                        // adapter). P1.3 is a closed question and nothing
                        // downstream reads its result. The same key gates the
                        // P1.4 block inside ngx_probe (gpu1_context.cpp), the
                        // other startup code that works on the game's device.
                        // Not the hotkey: with Probes=1 the hotkey still runs
                        // P1.3, because pressing it is already an explicit ask.
                        // Side effect: transit_probe alternates A/A' by
                        // invocation count, so with the startup run skipped the
                        // first hotkey run takes A'.
                        if (mgpu::strace::on()) mgpu::strace::ev(mgpu::strace::R_BT, "startup-transit-probe", mgpu::gpu1::transit_probes_enabled() ? "RUNS (TransitProbes=1): objects and copies on the GAME's device" : "not run (TransitProbes absent)");   // R313
                        if (mgpu::gpu1::transit_probes_enabled())
                            (void)mgpu::gpu1::transit_probe();
                        else
                            mgpu::diag::info("[MGPU][R302] P1.3 startup transit probe NOT RUN: mgpu.ini "
                                             "TransitProbes is not 1 (the default). Nothing is placed "
                                             "on either GPU at startup and no A./A'. lines follow. "
                                             "TransitProbes=1 runs it here as before R302. This is a "
                                             "setting, not a fault.");
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
        bool r313_fallback_written = false;   // R313 DcompSafeMode=3
        unsigned long long frame = 0;   // T5: completed presents

        // P7.10: AutoArm. Off unless mgpu.ini says otherwise, so every
        // published measurement's conditions are the shipped default and a
        // measurement run still arms by hand, in gameplay, where the operator
        // chose it.
        //
        // Two conditions, not one. The frame count gives the game time to get
        // past shader compilation and its own startup swapchain churn; the
        // swapchain-quiet check is the one that matters, because the stream is
        // armed ONCE against the game's swapchain as it stands at that instant.
        // A rig log shows what happens when that is violated: a ResizeBuffers on
        // the game's chain, and a second later a continuous run of DROPPED and
        // REORDERED seals for the rest of the session.
        const unsigned autoarm_at = mgpu::gpu1::autoarm_frames();
        const unsigned long long AUTOARM_QUIET_MS = 3000;
        bool autoarm_done = (autoarm_at == 0);
        bool autoarm_waiting_logged = false;
        if (autoarm_at != 0)
        {
            snprintf(line, sizeof line,
                     "[MGPU][P7.10] AutoArm is ON: the stream will arm itself after %u presented "
                     "frames AND %llu ms with no swapchain event. For a MEASUREMENT leave it off "
                     "and arm by hand in gameplay - that is what every published figure ran under.",
                     autoarm_at, (unsigned long long)AUTOARM_QUIET_MS);
            mgpu::diag::info(line);
        }

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
                bool run_transit = false;
                // P6.3. Accumulated, not acted on inside the drain: a held key
                // can deliver several messages per drain and each one should
                // count, but the pump must finish first for the same reason the
                // transit probe waits - pumping is what keeps this thread from
                // stalling anything that broadcasts to top-level windows.
                int int_delta = 0;
                unsigned int_cycles = 0;
                unsigned view_cycles = 0;
                int seam_delta = 0;
                int seam_coarse = 0;
                unsigned simple_toggles = 0;   // R330: counted here, acted on after the drain
                while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE) != FALSE)
                {
                    if (m.message == WM_QUIT)
                        continue;
                    if (m.message == WM_HOTKEY && m.wParam == MGPU_HOTKEY_INT_TARGET)
                    { ++int_cycles; continue; }
                    if (m.message == WM_HOTKEY && m.wParam == MGPU_HOTKEY_INT_DOWN)
                    { --int_delta; continue; }
                    if (m.message == WM_HOTKEY && m.wParam == MGPU_HOTKEY_INT_UP)
                    { ++int_delta; continue; }
                    // V52. Acted on HERE rather than counted like the others:
                    // it changes no stream state, so there is nothing for the
                    // present loop to pick up, and it runs on the thread that
                    // owns the composition objects. Unreachable in mode 0 -
                    // the key is not registered there.
                    if (m.message == WM_HOTKEY && m.wParam == MGPU_HOTKEY_PEEK)
                    { (void)mgpu::gpu1::dcomp_peek_toggle(); continue; }
                    // The panel draws its next frame from the flag, so this is immediate and
                    // needs neither a restart nor the panel to be open. R330 (F1): toggle() only
                    // flips PanelSimple; the layout is never touched from this thread. Like
                    // every other key here it is counted now and acted on once the queue is empty -
                    // toggle() rewrites mgpu.ini, and a file rewrite does not belong inside the drain.
                    if (m.message == WM_HOTKEY && m.wParam == MGPU_HOTKEY_SIMPLE)
                    { ++simple_toggles; continue; }
                    // [MOUSE] Immediate, not counted: it changes no stream state, and the
                    // arrow must come back before the operator's next click, not 250 ms later.
                    if (m.message == WM_HOTKEY && m.wParam == MGPU_HOTKEY_CURSOR)
                    { mgpu::gpu1::disp_cursor_set(!mgpu::gpu1::disp_cursor_on()); continue; }
                    if (m.message == WM_HOTKEY && m.wParam == MGPU_HOTKEY_VIEW)
                    { ++view_cycles; continue; }
                    if (m.message == WM_HOTKEY && m.wParam == MGPU_HOTKEY_SEAM_L)
                    { --seam_delta; continue; }
                    if (m.message == WM_HOTKEY && m.wParam == MGPU_HOTKEY_SEAM_R)
                    { ++seam_delta; continue; }
                    if (m.message == WM_HOTKEY && m.wParam == MGPU_HOTKEY_SEAM_LC)
                    { --seam_coarse; continue; }
                    if (m.message == WM_HOTKEY && m.wParam == MGPU_HOTKEY_SEAM_RC)
                    { ++seam_coarse; continue; }
                    // P1.3g. WM_HOTKEY is thread-posted, not window-posted, so
                    // it arrives here with hwnd == nullptr and never reaches a
                    // window procedure. Flag it and run the probe AFTER the
                    // queue is drained rather than inside the drain: the probe
                    // takes ~100 ms, and pumping is what keeps this thread from
                    // stalling anything that broadcasts to top-level windows.
                    if (m.message == WM_HOTKEY && m.wParam == MGPU_HOTKEY_ID)
                    {
                        run_transit = true;
                        continue;
                    }
                    TranslateMessage(&m);
                    DispatchMessageW(&m);
                }

                // P6.3. Target first, then the steps, so pressing F8 and F9 in
                // the same drain does what the operator meant: retarget, then
                // step the thing they just selected.
                for (unsigned c = 0; c < int_cycles; ++c)
                    mgpu::gpu1::intensity_cycle_target();
                // R330: an odd number of presses in one drain is one flip; an even number is none.
                if ((simple_toggles & 1u) != 0u) mgpu::psimple::toggle();
                for (int d = 0; d < int_delta; ++d)  mgpu::gpu1::intensity_step(+1);
                for (int d = 0; d > int_delta; --d)  mgpu::gpu1::intensity_step(-1);

                // P7.4. The view cycles output -> input -> split -> output.
                // Read the current mode rather than tracking one here: the
                // panel can change it too, and two owners of one value is how
                // a control starts lying about what it is showing.
                if (view_cycles != 0)
                {
                    mgpu::gpu1::ui_state vs;
                    mgpu::gpu1::ui_read(vs);
                    int mode = vs.present_mode;
                    for (unsigned c = 0; c < view_cycles; ++c) mode = (mode + 1) % 3;
                    mgpu::gpu1::ui_set_present_mode(mode);
                }

                // P7.5. Seam steps, applied after the view so that pressing F7
                // and an arrow in the same drain switches into split FIRST and
                // then moves the seam, which is what the operator meant.
                for (int d = 0; d < seam_delta;  ++d) mgpu::gpu1::ui_split_move(+1, false);
                for (int d = 0; d > seam_delta;  --d) mgpu::gpu1::ui_split_move(-1, false);
                for (int d = 0; d < seam_coarse; ++d) mgpu::gpu1::ui_split_move(+1, true);
                for (int d = 0; d > seam_coarse; --d) mgpu::gpu1::ui_split_move(-1, true);

                if (run_transit)
                {
                    ++manual_runs;
                    char tag[64];
                    snprintf(tag, sizeof tag, "manual %u @frame %llu",
                             manual_runs, (unsigned long long)frame);
                    // P5.2 / DEFECT C. The probe chain and the stream are now
                    // MUTUALLY EXCLUSIVE, and the log says which one this press
                    // armed. They used to arm together, which put two
                    // independent NGX consumers on the one shared capability
                    // block and produced a neural image with the colours wrong
                    // while every seal counter stayed clean. A silent overlap of
                    // two paths is the section 00a failure shape; naming the
                    // path on every press is what stops it recurring unseen.
                    if (mgpu::gpu1::probes_enabled())
                    {
                        // Its own buffer: the enclosing `line` is 320 bytes and
                        // this message is longer, and a truncated log line is a
                        // wrong answer that looks like a right one.
                        char pl[512];
                        snprintf(pl, sizeof pl,
                                 "[MGPU][P1.3g] hotkey - PROBE CHAIN (mgpu.ini Probes=1): transit "
                                 "probe (%s), then P1.5 capture and the P3.x neural probe. The "
                                 "P4.0 stream is NOT armed on this press - the probes take the "
                                 "same NGX parameter block the stream would hold. The present "
                                 "loop stalls for the duration; a gap in the frame counter here "
                                 "is this, not a fault.", tag);
                        mgpu::diag::info(pl);
                        (void)mgpu::gpu1::transit_probe(tag);
                        // P1.5 rides the same key. It is inert until requested and
                        // one-shot after that, so repeated presses cost nothing.
                        mgpu::gpu1::capture_request();
                    }
                    else
                    {
                        mgpu::diag::info("[MGPU][P4.0] hotkey - STREAM (mgpu.ini Probes absent or "
                                         "0, the default). The one-shot probe chain is NOT run: "
                                         "P1.3, P1.5 and P3.0-P3.2 are closed questions and they "
                                         "share the neural stage's parameter block. Set Probes=1 "
                                         "to run them instead - the stream is then not armed.");
                        // P4.0: one stream per process, armed on demand so the
                        // operator picks gameplay rather than a menu.
                        mgpu::gpu1::stream_request();
                    }
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

                // ---- P7.10: AutoArm. MOVED HERE 2026-09-12. ----
                //
                // IT USED TO SIT AFTER present_frame, AND THAT IS WHY IT
                // CRASHED. AutoArm=1 died at arm on V21, V22 and V23 while
                // arming BY HAND worked on the same builds, in the same
                // session, with the same ini; Reflex=0 made no difference, so
                // the two arms differ by WHERE they happen, not by what they
                // do.
                //
                // The hotkey arms from the message pump above - before
                // stream_poll, before the present gate, before present_frame.
                // AutoArm armed after present_frame, in the tail of the
                // iteration, while claiming in its own log line to be
                // "identical to pressing CTRL+ALT+F10". It was not identical
                // and the log said it was, which is the worst combination:
                // stream_request() creates the NGX features by resetting
                // s.na/s.nl - the SAME single allocator and list the per-frame
                // consume path records into - so the safe moment to arm is the
                // one the hotkey uses, at the top of an iteration, and not the
                // one immediately behind a present.
                //
                // Also evaluated on EVERY iteration now rather than only on
                // ones that got past the present gate. The gate returns false
                // whenever nothing new has arrived, which is most iterations
                // while the game is still loading - exactly the window AutoArm
                // is counting through.
                // R246: the deferred startup probe. Runs once, here, on the
                // bridge thread, the first iteration after the game's first
                // finish_effects (or at the bound). It stalls this loop for
                // the probe's duration exactly as the hotkey probe chain does.
                // R302: "the game is not touched" was wrong - its P1.4 block
                // works on the game's own device. That block now needs
                // TransitProbes=1; the rest of the NGX probe is unchanged.
                if (ngx_probe_pending)
                {
                    const unsigned long long gf = mgpu::gpu1::game_effects_frames();
                    const unsigned long long waited = GetTickCount64() - ngx_probe_t0;
                    // R282 (gpu1_context.cpp). The shipped deferral is unchanged. The
                    // first time it is ready, R282 decides ONCE whether the probe also
                    // waits for the game's first DLSS evaluate, with no time limit. Not
                    // applied: "ready" is exactly the shipped condition, in this same
                    // iteration.
                    const bool shipped_ready = (gf >= 1ull || waited >= NGX_PROBE_DEFER_MAX_MS);
                    if (shipped_ready && r282_state == 0)
                    {
                        char why[320];
                        r282_state = mgpu::gpu1::ngx_probe_scene_hold_decide(why, sizeof why) ? 1 : 2;
                        mgpu::diag::info(why);
                    }
                    // R290: the depth hold's fallback also releases it (it fires only once
                    // the game's DLSS has been seen).
                    if (r282_state == 1 && (mgpu::gpu1::ngx_probe_scene_seen() || mgpu::gpu1::depth_fallback_requested()))
                    {
                        r282_state = 3;
                        const bool seen = mgpu::gpu1::ngx_probe_scene_seen();
                        snprintf(line, sizeof line, "[MGPU][R282] probe released %llu ms after the present chain: %s", waited,
                                 seen ? "the game's first DLSS evaluate was seen."
                                      : "R290 - the game's DLSS was seen and the depth hold reached DepthHoldMax.");
                        mgpu::diag::info(line);
                    }
                    const bool ready = shipped_ready && r282_state != 1;
                    unsigned long prev_pid = 0;
                    const unsigned prev_n = (ready
                                             ? mgpu::gpu1::other_instances_of_this_exe(&prev_pid) : 0u);
                    if (ready && prev_n != 0u)
                    {
                        // R248: a previous instance of this exe is still alive.
                        if (ngx_prev_wait_t0 == 0) ngx_prev_wait_t0 = GetTickCount64();
                        const unsigned long long pw = GetTickCount64() - ngx_prev_wait_t0;
                        if (!ngx_prev_wait_logged)
                        {
                            ngx_prev_wait_logged = true;
                            snprintf(line, sizeof line,
                                     "[MGPU][R248] a PREVIOUS INSTANCE of this executable is still running (pid %lu, %u in "
                                     "all) at the moment of the first NGX Init. Waiting for it to exit before the Init, "
                                     "up to %llu ms.", prev_pid, prev_n, NGX_PREV_INSTANCE_WAIT_MAX_MS);
                            mgpu::diag::warn(line);
                        }
                        if (pw < NGX_PREV_INSTANCE_WAIT_MAX_MS) { /* keep presenting; try again next iteration */ }
                        else
                        {
                            snprintf(line, sizeof line,
                                     "[MGPU][R248] the previous instance (pid %lu) is STILL running after %llu ms - going "
                                     "ahead with the Init anyway.", prev_pid, pw);
                            mgpu::diag::warn(line);
                        }
                    }
                    if (ready &&
                        (prev_n == 0u || (GetTickCount64() - ngx_prev_wait_t0) >= NGX_PREV_INSTANCE_WAIT_MAX_MS))
                    {
                        ngx_probe_pending = false;
                        if (ngx_prev_wait_logged && prev_n == 0u)
                        {
                            snprintf(line, sizeof line,
                                     "[MGPU][R248] the previous instance has exited (%llu ms after it was first seen); "
                                     "making the first NGX Init now.", GetTickCount64() - ngx_prev_wait_t0);
                            mgpu::diag::info(line);
                        }
                        else if (!ngx_prev_wait_logged)
                            mgpu::diag::info("[MGPU][R248] no other instance of this executable is running at the first NGX Init.");
                        snprintf(line, sizeof line,
                                 "[MGPU][R246] running the deferred NGX probe now: game frames seen=%llu, "
                                 "%llu ms after the present chain%s. Its Init is the first NGX Init in "
                                 "this process.",
                                 gf, waited, (gf == 0ull) ? " - BOUND reached, the game has rendered nothing yet" : "");
                        mgpu::diag::info(line);
                        (void)mgpu::gpu1::ngx_probe(1280, 720);
                        mgpu::gpu1::note_ngx_probe_ran();   // R290
                        if (mgpu::gpu1::ngx_probe_init_was_outofdate())
                        {
                            ngx_retry_pending = true; ngx_retry_n = 0;
                            ngx_retry_at_frames = mgpu::gpu1::game_effects_frames() + NGX_RETRY_GAP_FRAMES;
                            ngx_retry_t0 = GetTickCount64();
                            mgpu::diag::warn("[MGPU][R247] the first NGX Init was refused (OutOfDate). On a title with no NGX "
                                             "contract the core and the snippet are this add-on's own, so they are unloaded, "
                                             "reloaded and the Init made again - up to 3 tries, spaced by the game's frames.");
                        }
                    }
                }

                // R247: the retry. Same place as the deferred probe, same
                // thread; the whole probe runs again (locator, loads, Init).
                if (ngx_retry_pending && !ngx_probe_pending)
                {
                    const unsigned long long gf = mgpu::gpu1::game_effects_frames();
                    const unsigned long long waited = GetTickCount64() - ngx_retry_t0;
                    (void)gf; (void)NGX_RETRY_GAP_FRAMES; (void)NGX_RETRY_GAP_MAX_MS;
                    unsigned long rp = 0; unsigned rn = 0; bool held = false;
                    if (waited >= NGX_RETRY_GAP_MS)
                    {
                        rn = mgpu::gpu1::other_instances_of_this_exe(&rp);
                        held = (rn != 0u && waited < NGX_PREV_INSTANCE_WAIT_MAX_MS);
                        if (held && !ngx_prev_wait_logged)
                        {
                            ngx_prev_wait_logged = true;
                            snprintf(line, sizeof line, "[MGPU][R248] retry held: a previous instance (pid %lu) is still "
                                                        "running; retrying once it has exited (bounded).", rp);
                            mgpu::diag::warn(line);
                        }
                    }
                    if (waited >= NGX_RETRY_GAP_MS && !held)
                    {
                        ++ngx_retry_n;
                        char why[400];
                        snprintf(line, sizeof line, "[MGPU][R248] retry %u: %u other instance(s) of this executable alive "
                                                    "(first pid %lu); %llu ms since the last try.",
                                 ngx_retry_n, rn, rp, waited);
                        mgpu::diag::info(line);
                        const bool can = mgpu::gpu1::ngx_reload_for_retry(why, sizeof why);
                        snprintf(line, sizeof line, "[MGPU][R247] retry %u/%u: %s", ngx_retry_n, NGX_RETRY_MAX, why);
                        mgpu::diag::info(line);
                        if (can) (void)mgpu::gpu1::ngx_probe(1280, 720);
                        if (!can || !mgpu::gpu1::ngx_probe_init_was_outofdate())
                        {
                            ngx_retry_pending = false;
                            mgpu::diag::info(can ? "[MGPU][R247] retry: the Init was ACCEPTED - NR can start on this launch "
                                                   "after all. The refused state lived in the modules that were reloaded."
                                                 : "[MGPU][R247] retry: not possible on this title (see the line above).");
                        }
                        else if (ngx_retry_n >= NGX_RETRY_MAX)
                        {
                            ngx_retry_pending = false;
                            mgpu::diag::warn("[MGPU][R247] retry: every try was refused (OutOfDate). The refused state is not "
                                             "inside the modules this add-on loads; it is outside this process. NR will not "
                                             "start on this launch. Relaunching the game is what clears it.");
                        }
                        else
                        {
                            ngx_retry_at_frames = mgpu::gpu1::game_effects_frames() + NGX_RETRY_GAP_FRAMES;
                            ngx_retry_t0 = GetTickCount64();
                        }
                    }
                }

                // V19. THE PANEL ARMS, AND HOLDS AUTOARM WHILE IT IS OPEN.
                // Taken before the AutoArm block so a manual arm always wins.
                if (mgpu::gpu1::ui_take_arm_request())
                {
                    autoarm_done = true;   // manual arm stands down the timer
                    mgpu::diag::info("[MGPU][P7.10] ARM NOW pressed in the panel. Settings are "
                                     "read from mgpu.ini at this moment, so anything changed in "
                                     "the panel before now applies to THIS session.");
                    mgpu::gpu1::stream_request();
                }

                // V55. Hold AutoArm while the one-display hint is on screen -
                // ten seconds of wall clock, not a frame count, because 600
                // frames is ten seconds at 60 Hz and under three at 240 Hz and
                // the line has to be readable on both. Only ever true for a
                // headless single-display rig that is NOT already using the
                // mode, so nobody else's arm timing moves by a millisecond.
                if (!autoarm_done && frame >= autoarm_at &&
                    !mgpu::gpu1::ui_panel_is_open() &&
                    !mgpu::gpu1::autoarm_hint_holding())
                {
                    const unsigned long long quiet =
                        mgpu::adapter::ms_since_last_swapchain_event();
                    if (quiet >= AUTOARM_QUIET_MS)
                    {
                        mgpu::gpu1::ui_state ast;
                        mgpu::gpu1::ui_read(ast);
                        if (ast.armed)
                        {
                            // Armed by hand while we were waiting. Stand down
                            // silently rather than arming a second time.
                            autoarm_done = true;
                        }
                        else
                        {
                            autoarm_done = true;
                            snprintf(line, sizeof line,
                                     "[MGPU][P7.10] AutoArm firing at frame %llu (%llu ms since "
                                     "the last swapchain event), from the same point in the loop "
                                     "the hotkey arms from. If the game is still in a menu the "
                                     "stream is armed against menu frames, which is correct but "
                                     "measures nothing.",
                                     (unsigned long long)frame, quiet);
                            mgpu::diag::info(line);
                            mgpu::gpu1::stream_request();
                        }
                    }
                    else if (!autoarm_waiting_logged)
                    {
                        autoarm_waiting_logged = true;
                        snprintf(line, sizeof line,
                                 "[MGPU][P7.10] AutoArm reached its frame count at %llu but the "
                                 "game's swapchain changed %llu ms ago - waiting for %llu ms of "
                                 "quiet. Arming across a swapchain rebuild is what produces a "
                                 "session-long run of DROPPED and REORDERED seals.",
                                 (unsigned long long)frame, quiet,
                                 (unsigned long long)AUTOARM_QUIET_MS);
                        mgpu::diag::info(line);
                    }
                }

                // P5.1. Poll first, then decide whether there is anything worth
                // presenting. When the stream is running this paces the loop to
                // NEW neural frames instead of to vsync; when it is idle the
                // gate returns true every time and the loop behaves exactly as
                // T5 shipped it. The gate does the waiting itself, so a false
                // return means "nothing new, and we have already slept".
                // R313: the overlay thread's root request / the value-3 deferred root
                // (two relaxed loads when nothing is pending), and the trace's per-pass
                // sample + stop detection (one relaxed load when off).
                mgpu::gpu1::dcomp_apply_pending();
                mgpu::strace::bt_tick();
                // R313 DcompSafeMode=3 fallback: the stop signature fired and the game's
                // presents have stayed flat for DcompSafeFallbackMs -> the next launch
                // runs the bridge's own window (DcompOverlay=0), the configuration that
                // ran on Dlagonborn's machine. R313b: was "DcompSafeMode=2" until value 2
                // was removed. Written once. Only value 3 ever writes this.
                if (!r313_fallback_written && mgpu::strace::safe_mode() == 3 && mgpu::strace::stop_fired() &&
                    mgpu::strace::game_flat_ms() >= (unsigned long long)mgpu::strace::safe_fallback_ms())
                {
                    r313_fallback_written = true;
                    char fb[400];
                    snprintf(fb, sizeof fb,
                             "[MGPU][R313] DcompSafeMode=3 FALLBACK: the game's presents have been flat for %llu ms after the first "
                             "neural frame with the bridge alive and the game's window up. Writing DcompOverlay=0 to mgpu.ini: "
                             "the NEXT launch uses the bridge's own window instead of composing into the game's window. Set "
                             "DcompOverlay=1 again to return to the composition mode.",
                             mgpu::strace::game_flat_ms());
                    mgpu::diag::error_direct(fb);
                    (void)mgpu::gpu1::ui_ini_write_word("DcompOverlay", "0");
                }
                mgpu::gpu1::stream_poll();
                // [H7/B, 2026-11-11] Foreground follow, before the gate: the gate can sleep
                // its 4 ms timeout, and an Alt+Tab must take this window down even while no
                // neural frame is arriving (a game that stops rendering in the background is
                // the normal case, not the exception). Mode 1 never shows this window - the
                // visual lives on the game's own window - so it has nothing to follow.
                if (mgpu::gpu1::dcomp_overlay_mode() == 0)
                    follow_foreground(hwnd, frame);
                if (!mgpu::gpu1::stream_present_gate(4))
                    continue;

                // R341 (FrameGen=4): the pacing thread presents this frame (generated first, real half an
                // interval later). False = not on, or not started yet: present as today.
                const bool r341_paced = mgpu::fg::handoff(cr, cg, cb);

                if (!r341_paced && !mgpu::gpu1::present_frame(cr, cg, cb))
                {
                    // The first failure is already logged with its step,
                    // HRESULT and removal reason (one-shot, in
                    // gpu1_context). Stop presenting: the loop exits to
                    // the ordered teardown.
                    break;
                }
                // P1.5: cheap, self-disarming, and does nothing at all until a
                // frame has actually been captured. Placed after the present so
                // its one expensive poll cannot delay a frame that was ready.
                mgpu::gpu1::capture_poll();

                ++frame;
                if (frame == 1)
                {
                    mgpu::diag::info("[MGPU][T5] first successful present (frame 1) - the "
                                     "vsync-paced present loop is alive");
                    mgpu::strace::note_first_present();   // R313
                }
                else if (frame % 600 == 0)
                {
                    snprintf(line, sizeof line,
                             "[MGPU][T5] present loop alive: frame %llu thread id 0x%X",
                             (unsigned long long)frame, (unsigned)tid);
                    mgpu::diag::info(line);
                }

                // P7.10: the title as a status line. Every 30 frames (~0.5 s)
                // and once at frame 1, so the window says what it is doing
                // before anyone opens an overlay. SetWindowTextW on the thread
                // that owns the window, which is this one.
                if (frame == 1 || frame % 30 == 0)
                    set_window_title(hwnd, frame);

                // [R456, 2026-11-11] Re-assert the cursor about once a second. Between mouse
                // moves the game's own thread can reset the shape (or hide it again with its
                // ShowCursor count); this is the only thing that notices when the arrow silently
                // stops being the one DispCursor asked for. One GetCursorInfo per 30 presents.
                if (input_pump_enabled() && (frame % 30ull) == 0ull)
                    mgpu::gpu1::disp_cursor_raise();

                // [R457] Panel-input census, only while an overlay is open: if the next run still
                // reports a dead panel mouse, this says whether the messages stop arriving or keep
                // arriving and get forwarded past the overlay.
                if (mgpu::gpu1::bridge_overlay_open() && (frame % 300ull) == 0ull)
                {
                    snprintf(line, sizeof line,
                             "[MGPU][R457] panel input: overlay OPEN. This window received %llu mouse "
                             "msg(s), %llu click(s) so far, and the pump is %s. READING: clicks flat at 0 "
                             "while the panel is on screen = the window is not under the pointer (z-order/"
                             "hidden); clicks climbing but the panel ignores them = they are being posted "
                             "to the game instead of reaching ReShade's overlay.",
                             (unsigned long long)s_mouse_msgs.load(std::memory_order_relaxed),
                             (unsigned long long)s_mouse_clicks.load(std::memory_order_relaxed),
                             input_pump_enabled() ? "ON (forwarding to the game)" : "OFF (pass-through arm)");
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
                        if (mgpu::strace::on()) { char w[120]; snprintf(w, sizeof w, "GPU 1 device removed (poll) hr=0x%08X", (unsigned)reason); mgpu::strace::record(w, (unsigned)reason); }   // R313
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
                if (mgpu::strace::on()) { char w[120]; snprintf(w, sizeof w, "GPU 1 device removed (no-chain poll) hr=0x%08X", (unsigned)reason); mgpu::strace::record(w, (unsigned)reason); }   // R313
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
        if (hotkey_ok)
        {
            UnregisterHotKey(nullptr, MGPU_HOTKEY_ID);
            UnregisterHotKey(nullptr, MGPU_HOTKEY_INT_TARGET);
            UnregisterHotKey(nullptr, MGPU_HOTKEY_INT_DOWN);
            UnregisterHotKey(nullptr, MGPU_HOTKEY_INT_UP);
            UnregisterHotKey(nullptr, MGPU_HOTKEY_VIEW);
            // V52: only ever registered in ghost mode.
            if (mgpu::gpu1::dcomp_overlay_mode())
                UnregisterHotKey(nullptr, MGPU_HOTKEY_PEEK);
            UnregisterHotKey(nullptr, MGPU_HOTKEY_SIMPLE);
            UnregisterHotKey(nullptr, MGPU_HOTKEY_SEAM_L);
            UnregisterHotKey(nullptr, MGPU_HOTKEY_SEAM_R);
            UnregisterHotKey(nullptr, MGPU_HOTKEY_SEAM_LC);
            UnregisterHotKey(nullptr, MGPU_HOTKEY_SEAM_RC);
            mgpu::diag::info("[MGPU][P1.3g] hotkeys unregistered (transit + P6.3 intensity)");
        }

        mgpu::diag::info("[MGPU][T5] shutdown - ordered teardown (NGX features -> "
                         "gpu1::shutdown [present chain -> device] -> DestroyWindow -> "
                         "UnregisterClass -> adapter::shutdown)");
        if (mgpu::strace::on()) mgpu::strace::ev(mgpu::strace::R_BT, "bridge-teardown", "ordered teardown begins");   // R313

        // V32. THE NGX FEATURES GO FIRST, BEFORE THE DEVICE THEY WERE CREATED
        // AGAINST. Nothing did this before: stream_release() only ran on a
        // failed arm or a stream that reached its frame bound, so an ordinary
        // game exit abandoned the NR handles, the SR handle, both parameter
        // blocks and the driver snippet DLL - and then the line below released
        // the device underneath them. That is the second-launch crash.
        mgpu::gpu1::stream_shutdown();

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