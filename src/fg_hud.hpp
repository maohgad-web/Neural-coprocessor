#pragma once

// [PORT C, 2026-11-11] THE READOUT THAT IS VISIBLE DURING PLAY.
//
// Taken from the reference tree's fg_hud.hpp, and kept exactly as it was there
// for the reason its header gives: a LAYERED GDI WINDOW, not a draw into the
// presented texture. Drawing into the frame would put a new pass in front of
// the thing being measured, inside the only present path that exists - the one
// place this project cannot afford a new owner. This window is outside the
// render path: it cannot touch the swapchain, the ring or the generators, and
// if it fails the only loss is one line of text.
//
// Colour-key transparency: the window paints black everywhere and LWA_COLORKEY
// makes black fully transparent, so only the glyphs show.
//
// Header-only on purpose: no new translation unit, no CMake change.

#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cwchar>

namespace mgpu
{
namespace fghud
{
struct state
{
    HWND wnd = nullptr;
    HFONT font = nullptr;
    unsigned long long last_s = 0, last_o = 0;
    ULONGLONG last_ms = 0;
    double rate_s = 0.0, rate_o = 0.0;
    unsigned fails = 0;
};

inline state &st()
{
    static state s;
    return s;
}

inline LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    return DefWindowProcW(h, m, w, l);
}

inline bool create_once(state &s)
{
    if (s.wnd != nullptr) return true;
    if (s.fails >= 3) return false;          // one message per session, not per frame

    HINSTANCE hinst = GetModuleHandleW(nullptr);
    static bool cls = false;
    if (!cls)
    {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = &wnd_proc;
        wc.hInstance = hinst;
        wc.hCursor = nullptr;
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.lpszClassName = L"mgpu_fghud";
        if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            ++s.fails;
            return false;
        }
        cls = true;
    }

    // NOACTIVATE + TRANSPARENT + TOOLWINDOW: never takes focus, never takes a
    // click, never appears in the taskbar.
    s.wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT |
                            WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                            L"mgpu_fghud", L"", WS_POPUP,
                            0, 0, 10, 10, nullptr, nullptr, hinst, nullptr);
    if (s.wnd == nullptr)
    {
        ++s.fails;
        return false;
    }
    SetLayeredWindowAttributes(s.wnd, RGB(0, 0, 0), 255, LWA_COLORKEY);

    s.font = CreateFontW(-26, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         FIXED_PITCH | FF_MODERN, L"Consolas");
    return true;
}

inline void paint(state &s, const wchar_t *txt, int px, int py, int pw, int ph)
{
    SetWindowPos(s.wnd, HWND_TOPMOST, px, py, pw, ph, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    HDC dc = GetDC(s.wnd);
    if (dc == nullptr) return;
    RECT rc{ 0, 0, pw, ph };
    FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(130, 255, 170));
    HGDIOBJ old = (s.font != nullptr) ? SelectObject(dc, s.font) : nullptr;
    TextOutW(dc, 0, 0, txt, (int)wcslen(txt));
    if (old != nullptr) SelectObject(dc, old);
    ReleaseDC(s.wnd, dc);

    MSG msg;
    while (PeekMessageW(&msg, s.wnd, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
}

// Called from the pacing thread once per presented frame. Two atomic loads and
// a comparison; painting is throttled to twice a second. `extra` is appended
// verbatim (pace/landing line), and may be empty.
inline void tick(bool enabled, unsigned long long src, unsigned long long out, const wchar_t *extra)
{
    state &s = st();
    if (!enabled)
    {
        if (s.wnd != nullptr && IsWindowVisible(s.wnd)) ShowWindow(s.wnd, SW_HIDE);
        return;
    }

    const ULONGLONG now = GetTickCount64();
    if (s.last_ms == 0)
    {
        s.last_ms = now; s.last_s = src; s.last_o = out;
        (void)create_once(s);
        return;
    }
    if (now - s.last_ms < 500) return;

    const double dt = (double)(now - s.last_ms) / 1000.0;
    s.rate_s = (dt > 0.0) ? (double)(src - s.last_s) / dt : 0.0;
    s.rate_o = (dt > 0.0) ? (double)(out - s.last_o) / dt : 0.0;
    s.last_ms = now; s.last_s = src; s.last_o = out;
    if (!create_once(s)) return;

    wchar_t txt[160];
    _snwprintf_s(txt, 160, _TRUNCATE, L"FG %.0f / %.0f%s%s",
                 s.rate_s, s.rate_o, (extra != nullptr && extra[0] != 0) ? L"  " : L"",
                 (extra != nullptr) ? extra : L"");

    // Anchored to the FOREGROUND window's top right, pushed down so it sits
    // under ReShade's own counter rather than covering it.
    RECT fr{ 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    HWND fg = GetForegroundWindow();
    RECT r;
    if (fg != nullptr && GetWindowRect(fg, &r)) fr = r;
    const int pw = 460, ph = 34;
    paint(s, txt, fr.right - pw - 12, fr.top + 56, pw, ph);
}

// Repaint the text without waiting for the next throttle window (panel toggles
// it and the operator expects to see the change at once).
inline void force_refresh()
{
    st().last_ms = 0;
}
}   // namespace fghud
}   // namespace mgpu
