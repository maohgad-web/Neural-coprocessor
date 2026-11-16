// MGPU Bridge - PanelOverPicture: the live panel. See live_panel.hpp.
//
// Contributed in the N01950 pull request (Far Cry 6 and Kingdom Come:
// Deliverance II on the contributor's rig), moved into its own file with its
// key read at attach, its event registered only with the key, R-numbered
// lines, and the R293 input-release guard.

#include <windows.h>
#include <atomic>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <cwchar>

// Same ImGui preamble as dllmain.cpp (P7.1): ImTextureID must be ImU64 and the
// version must match reshade_overlay.hpp. Without ImGui, everything below
// compiles to "off".
#if defined(__has_include)
#  if __has_include(<imgui.h>)
#    define MGPU_HAVE_IMGUI 1
#    define ImTextureID ImU64
#    include <imgui.h>
#  endif
#endif

#include <reshade.hpp>

#include "live_panel.hpp"
#include "diag.hpp"
#include "gpu1_context.hpp"
#include "mgpu_ini_parser.hpp"

namespace mgpu::live_panel
{
namespace
{
// ---- R292: the key, read once at attach --------------------------------------
std::atomic<int> g_key{-1};   // -1 not read, 0 off, 1 on
std::atomic<void *> g_module{nullptr};    // R295: reading PanelFreeCursor on every open
std::atomic<int> g_free_cursor{0};        // R295: PanelFreeCursor

int read_flag(void *module, const char *name)
{
    wchar_t mod[MAX_PATH] = {};
    const DWORD len = (module != nullptr) ? GetModuleFileNameW((HMODULE)module, mod, MAX_PATH) : 0;
    if (len == 0 || len >= MAX_PATH) return 0;
    wchar_t *slash = wcsrchr(mod, L'\\');
    if (slash == nullptr) return 0;
    slash[1] = L'\0';
    wchar_t path[MAX_PATH] = {};
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%lsmgpu.ini", mod);
    FILE *f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || f == nullptr) return 0;
    static char buf[mgpu::config::MAX_BYTES + 2u];
    const size_t got = fread(buf, 1, mgpu::config::MAX_BYTES + 1u, f);
    fclose(f);
    if (got > mgpu::config::MAX_BYTES) return 0;
    buf[got] = '\0';
    const char *k = mgpu::config::find(buf, got, name);
    return (k != nullptr && *k == '1') ? 1 : 0;
}

int read_key(void *module) { return read_flag(module, "PanelOverPicture"); }

// ---- the panel's state ------------------------------------------------------
std::atomic<int>    g_live{0};            // 1 = the panel is up on the neural layer
std::atomic<void *> g_game{nullptr};      // the game's effect runtime
std::atomic<void *> g_windowless{nullptr};// the GPU 1 runtime with no window = the neural picture
std::atomic<void *> g_host{nullptr};      // the runtime the panel was actually opened on
std::atomic<int>    g_said_host{0};
std::atomic<int>    g_probe{0};

// R293: the last time the host's reshade_overlay event fired (GetTickCount64).
std::atomic<unsigned long long> g_host_drawn_ms{0};
std::atomic<int> g_guard_said{0};
constexpr unsigned long long GUARD_BLOCK_MS   = 250;    // drawn this recently: block the game's input
constexpr unsigned long long GUARD_RELEASE_MS = 2000;   // not drawn this long: the panel is gone

// R296. THE INPUT GUARD. PanelFreeCursor gives the panel its own pointer
// through GetCursorPos/GetAsyncKeyState, but the game still reads its raw mouse
// through WM_INPUT on its own window, so dragging a slider also flies the
// camera. While the panel is live the bridge chains this window procedure in
// front of whatever proc the game (and ReShade) left on the window and swallows
// the mouse messages - raw and legacy - before either of them sees them.
// Nothing the panel needs passes through here: the panel polls the OS directly.
static WNDPROC g_prev_proc = nullptr;
static HWND    g_guard_hwnd = nullptr;
static std::atomic<int> g_swallow{0};
static std::atomic<unsigned long long> g_sw_input{0}, g_sw_mouse{0};

// R295b. ShowCursor is a process-wide display counter, not a per-window flag.
// Every TRUE this guard issues while the panel is up has to be given back when
// the panel goes away, or the game's own ShowCursor(FALSE) can no longer hide
// the pointer in game. The count is what we called, not what we asked for: a
// call that leaves the counter negative is still one call. Atomic because the
// guard runs on the game's present thread and the close event is not promised
// to arrive on the same one.
static std::atomic<int> g_cursor_shown{0};

static void release_cursor()
{
    int given = 0;
    while (g_cursor_shown.load(std::memory_order_relaxed) > 0)
    {
        ShowCursor(FALSE);
        g_cursor_shown.fetch_sub(1, std::memory_order_relaxed);
        ++given;
    }
    if (given > 0)
    {
        char l[200];
        snprintf(l, sizeof l, "[MGPU][R295b] cursor display counter restored: %d ShowCursor(FALSE) as the panel "
                              "left - the game hides its own cursor again from here.", given);
        mgpu::diag::info(l);
    }
}

static bool is_guarded_mouse_msg(UINT m)
{
    switch (m)
    {
    case WM_INPUT:
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
    case WM_MOUSEHOVER: case WM_MOUSELEAVE:
        return true;
    default:
        return false;
    }
}

static LRESULT CALLBACK guard_wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    if (g_swallow.load(std::memory_order_relaxed) != 0)
    {
        if (m == WM_INPUT) g_sw_input.fetch_add(1, std::memory_order_relaxed);
        else if (is_guarded_mouse_msg(m)) g_sw_mouse.fetch_add(1, std::memory_order_relaxed);
        if (is_guarded_mouse_msg(m)) return (m == WM_MOUSEWHEEL || m == WM_MOUSEHWHEEL) ? TRUE : 0;
        if (m == WM_SETCURSOR)       return TRUE;   // the game must not blank the pointer we just freed
    }
    if (g_prev_proc != nullptr) return CallWindowProcW(g_prev_proc, h, m, wp, lp);
    return DefWindowProcW(h, m, wp, lp);
}

static void ensure_input_guard()
{
    HWND h = (HWND)mgpu::gpu1::dcomp_game_hwnd();
    if (h == nullptr) return;
    const LONG_PTR cur = GetWindowLongPtrW(h, GWLP_WNDPROC);
    if (h == g_guard_hwnd && cur == (LONG_PTR)&guard_wndproc) return;
    if (cur == (LONG_PTR)&guard_wndproc) { g_guard_hwnd = h; return; }
    g_prev_proc = (WNDPROC)cur;
    g_guard_hwnd = h;
    SetLastError(0);
    const LONG_PTR prev = SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)&guard_wndproc);
    if (prev == 0 && GetLastError() != 0)
    {
        static std::atomic<int> said{0};
        if (said.exchange(1, std::memory_order_relaxed) == 0)
            mgpu::diag::warn("[MGPU][R296] input guard: SetWindowLongPtr on the game window FAILED - the panel "
                             "still works, but the game keeps receiving the mouse.");
        return;
    }
    static std::atomic<int> said{0};
    if (said.exchange(1, std::memory_order_relaxed) == 0)
    {
        char wl[300] = {}, wt[200] = {};
        GetClassNameA(h, wl, sizeof wl - 1);
        GetWindowTextA(h, wt, sizeof wt - 1);
        char wg[620];
        snprintf(wg, sizeof wg, "[MGPU][R297] guard target: hwnd=%p class=%s title=%s", (void *)h, wl, wt);
        mgpu::diag::info(wg);
        mgpu::diag::info("[MGPU][R296] input guard installed on the game window (previous proc chained): while "
                         "the panel is live WM_INPUT and the legacy mouse messages are swallowed before the game "
                         "and ReShade see them; the panel polls the OS itself (PanelFreeCursor=1).");
    }
}

// R297. THE RAW-INPUT SUSPENSION. The R296 guard swallows WM_INPUT on the game
// window and the camera still flew, so the game's mouse is not read through that
// window's messages. Windows will tell this process which raw devices it has
// registered - and the same call takes them away and puts them back. The list is
// snapshotted when the panel comes up and replayed verbatim when it closes, so
// the game is back to normal without a restart.
static RAWINPUTDEVICE g_rid[16];
static UINT g_rid_count = 0;
static bool g_rid_off = false;

static UINT query_rid(RAWINPUTDEVICE *out, UINT cap)
{
    UINT need = 0;
    if (GetRegisteredRawInputDevices(nullptr, &need, sizeof(RAWINPUTDEVICE)) == (UINT)-1) return 0;
    if (need == 0) return 0;
    if (need > cap) need = cap;
    UINT got = need;
    if (GetRegisteredRawInputDevices(out, &got, sizeof(RAWINPUTDEVICE)) == (UINT)-1) return 0;
    UINT keep = 0;
    for (UINT i = 0; i < got && i < cap; ++i)
        if (out[i].usUsagePage != 0) out[keep++] = out[i];   // R298: the buffer comes back padded with zeros
    return keep;
}
static BOOL drop_rid(const RAWINPUTDEVICE *src, UINT n)
{
    RAWINPUTDEVICE rem[16];
    if (n > 16) n = 16;
    for (UINT i = 0; i < n; ++i)
    {
        rem[i] = src[i];
        rem[i].dwFlags = RIDEV_REMOVE;
        rem[i].hwndTarget = nullptr;
    }
    return RegisterRawInputDevices(rem, n, sizeof(RAWINPUTDEVICE));
}
static void log_rid(const char *why, UINT n, const RAWINPUTDEVICE *dev)
{
    char l[640];
    int off = snprintf(l, sizeof l, "[MGPU][R297] raw input %s: %u device(s)", why, n);
    for (UINT i = 0; i < n && off > 0 && off < (int)sizeof l - 100; ++i)
        off += snprintf(l + off, sizeof l - off, " [page=0x%X usage=0x%X flags=0x%X hwnd=%p]",
                        dev[i].usUsagePage, dev[i].usUsage, dev[i].dwFlags, (void *)dev[i].hwndTarget);
    mgpu::diag::info(l);
}
static void suspend_raw_input()
{
    if (g_rid_off) return;
    UINT n = query_rid(g_rid, 16);
    if (n == 0) return;
    g_rid_count = n;
    const BOOL ok = drop_rid(g_rid, n);
    g_rid_off = true;
    log_rid(ok ? "suspended while the panel is live" : "SUSPEND-WAIT: first RIDEV_REMOVE failed", n, g_rid);
    if (!ok) g_rid_off = false;   // let the next frame retry

}
static void enforce_raw_input()
{
    if (!g_rid_off) { suspend_raw_input(); return; }
    RAWINPUTDEVICE cur[16];
    UINT n = query_rid(cur, 16);
    if (n != 0)
    {
        drop_rid(cur, n);   // the game re-registered them: take them away again
        static unsigned long long again = 0;
        if ((++again % 300ull) == 1ull) log_rid("taken away again (the game re-registered)", n, cur);
    }
}
static void resume_raw_input()
{
    if (!g_rid_off) return;
    g_rid_off = false;
    if (g_rid_count != 0)
    {
        if (RegisterRawInputDevices(g_rid, g_rid_count, sizeof(RAWINPUTDEVICE)))
            log_rid("restored when the panel closed", g_rid_count, g_rid);
        else
            mgpu::diag::warn("[MGPU][R297] raw input restore FAILED - the game may not see the mouse until it is "
                             "restarted.");
    }
}

// The pointer as ReShade tracks it on the GAME runtime (it survives raw input
// with RIDEV_NOLEGACY; GetCursorPos does not). One atomic each, so the game
// thread (writer) and the bridge thread (reader) cannot tear a pair apart.
std::atomic<unsigned long long> g_pos{0};    // (float x << 32) | float y
std::atomic<unsigned long long> g_disp{0};   // the game runtime's DisplaySize, same packing
std::atomic<unsigned> g_btn{0};              // bit0 L, bit1 R, bit2 M, bit3 = position valid

inline unsigned long long pack2(float a, float b)
{
    unsigned ua = 0, ub = 0;
    memcpy(&ua, &a, 4);
    memcpy(&ub, &b, 4);
    return ((unsigned long long)ua << 32) | (unsigned long long)ub;
}
inline void unpack2(unsigned long long v, float &a, float &b)
{
    const unsigned ua = (unsigned)(v >> 32), ub = (unsigned)(v & 0xFFFFFFFFu);
    memcpy(&a, &ua, 4);
    memcpy(&b, &ub, 4);
}

#if defined(MGPU_HAVE_IMGUI)
// ---- the input feed ------------------------------------------------------------
// The GPU 1 runtime's ImGuiIO is written by nobody: ReShade feeds input only
// into a runtime that has a window, and a composition swapchain has none. So
// its io fields are free, and writing them is the panel's input path. The
// ImGui event API is not in the add-on function table; the plain fields are
// declared in imgui.h and need no import. ImGui derives clicks from them at
// the next NewFrame (one frame of latency).
//
// The pointer is normalised over the GAME runtime's display and scaled into
// this runtime's display, so the two may differ in size. GetCursorPos is the
// fallback only (frozen under raw input).
void feed_input(reshade::api::effect_runtime *runtime)
{
    // Every exit carries a reason; a change of reason is logged at once.
    static const char *last_why = "(nothing yet)";
    const char *why = "ok";

    do
    {
        if (runtime == nullptr) { why = "no runtime"; break; }
        if (!mgpu::gpu1::dcomp_overlay_mode()) { why = "DcompOverlay off"; break; }
        if (g_live.load(std::memory_order_relaxed) == 0)
        {
            // PP4: nothing else writes this runtime's io, and this event keeps its
            // ImGui frames running with the panel closed, so ImGui's pointer stayed
            // on the picture where the panel last had it.
            if (is_host((void *)runtime)) ImGui::GetIO().MouseDrawCursor = false;
            why = "panel not live";
            break;
        }

        void *host = g_host.load(std::memory_order_relaxed);
        if (host == nullptr) host = g_windowless.load(std::memory_order_relaxed);
        // R298: the game runtime calls in every frame while the panel is open.
        // Its reason never changes, but it shared last_why with the host, so
        // the two alternated and every call logged a CHANGED line (86% of an
        // FC6 log). It returns here without logging; nothing else differs.
        if ((void *)runtime != host) return;

        // R293: the host is drawing the panel now.
        g_host_drawn_ms.store(GetTickCount64(), std::memory_order_relaxed);

        ImGuiIO &io = ImGui::GetIO();

        const unsigned btn = g_btn.load(std::memory_order_relaxed);
        float gx = 0.0f, gy = 0.0f, gdw = 0.0f, gdh = 0.0f;
        unpack2(g_pos.load(std::memory_order_relaxed), gx, gy);
        unpack2(g_disp.load(std::memory_order_relaxed), gdw, gdh);
        // R295: with PanelFreeCursor=1 the OS cursor is released, so the desktop
        // position is the live one; the game runtime's io is frozen under raw input.
        const bool free_cursor = g_free_cursor.load(std::memory_order_relaxed) != 0;
        const bool from_game = !free_cursor && (btn & 8u) != 0u && gdw > 0.0f && gdh > 0.0f;

        float u = 0.0f, v = 0.0f;
        bool ldown = false, rdown = false;
        if (from_game)
        {
            u = gx / gdw;
            v = gy / gdh;
            // Unreachable with PanelFreeCursor on (from_game is false then), so
            // these stay exactly as 0.5.1 reads them: the key-off path is untouched.
            ldown = (btn & 1u) != 0u;
            rdown = (btn & 2u) != 0u;
        }
        else
        {
            HWND hwnd = (HWND)mgpu::gpu1::dcomp_game_hwnd();
            if (hwnd == nullptr) { why = "no game window recorded (desktop fallback)"; break; }
            RECT rc{};
            if (GetClientRect(hwnd, &rc) == FALSE) { why = "GetClientRect failed"; break; }
            const int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
            if (cw <= 0 || ch <= 0) { why = "game client is empty"; break; }
            POINT tl{rc.left, rc.top};
            POINT p{};
            if (ClientToScreen(hwnd, &tl) == FALSE) { why = "ClientToScreen failed"; break; }
            if (GetCursorPos(&p) == FALSE) { why = "GetCursorPos failed"; break; }
            u = (float)(p.x - tl.x) / (float)cw;
            v = (float)(p.y - tl.y) / (float)ch;
            ldown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            rdown = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
        }
        if (u < 0.0f) u = 0.0f; else if (u > 1.0f) u = 1.0f;
        if (v < 0.0f) v = 0.0f; else if (v > 1.0f) v = 1.0f;

        const float want_x = u * io.DisplaySize.x;
        const float want_y = v * io.DisplaySize.y;

        // Feed probe every 120 fed frames: what ImGui had before this write,
        // and whether it thinks the pointer is over its UI.
        if ((g_probe.fetch_add(1, std::memory_order_relaxed) % 120) == 0)
        {
            char pr[1024];
            if (free_cursor)
            {
                // R295: with the pointer freed, the facts worth printing are the
                // desktop position, the clip rect and whether the cursor is visible
                // at all - the game runtime's io is frozen under raw input.
                POINT dp{};
                const int dok = (GetCursorPos(&dp) != FALSE) ? 1 : 0;
                RECT clip{};
                GetClipCursor(&clip);
                CURSORINFO cinfo{};
                cinfo.cbSize = sizeof cinfo;
                const int shown = (GetCursorInfo(&cinfo) != FALSE && (cinfo.flags & CURSOR_SHOWING) != 0) ? 1 : 0;
                snprintf(pr, sizeof pr,
                         "[MGPU][R294] feed probe: src=%s free=1 write pos=(%.0f,%.0f) L=%d R=%d | "
                         "read back pos=(%.0f,%.0f) valid=%d | cursor=%d wantCapture=%d down0=%d | "
                         "game io=(%.0f,%.0f) disp=(%.0f,%.0f) | desk=(%ld,%ld) ok=%d "
                         "clip=(%ld,%ld)-(%ld,%ld) shown=%d | norm=(%.3f,%.3f)",
                         from_game ? "game runtime io" : "desktop GetCursorPos (fallback)",
                         (double)want_x, (double)want_y, (int)ldown, (int)rdown,
                         (double)io.MousePos.x, (double)io.MousePos.y,
                         (int)(io.MousePos.x != -FLT_MAX && io.MousePos.y != -FLT_MAX),
                         (int)io.MouseDown[0], (int)io.MouseDrawCursor, (int)io.WantCaptureMouse,
                         (double)gx, (double)gy, (double)gdw, (double)gdh,
                         (long)dp.x, (long)dp.y, dok,
                         (long)clip.left, (long)clip.top, (long)clip.right, (long)clip.bottom, shown,
                         (double)u, (double)v);
            }
            else
            {
                // Key absent or 0: the line is byte for byte the one 0.5.1 prints.
                snprintf(pr, sizeof pr,
                         "[MGPU][R294] feed probe: src=%s write pos=(%.0f,%.0f) L=%d R=%d | read back "
                         "before this write: pos=(%.0f,%.0f) valid=%d L=%d | cursor=%d wantCapture=%d | "
                         "game io pos=(%.0f,%.0f) disp=(%.0f,%.0f) | norm=(%.3f,%.3f)",
                         from_game ? "game runtime io" : "desktop GetCursorPos (fallback)",
                         (double)want_x, (double)want_y, (int)ldown, (int)rdown,
                         (double)io.MousePos.x, (double)io.MousePos.y,
                         (int)(io.MousePos.x != -FLT_MAX && io.MousePos.y != -FLT_MAX),
                         (int)io.MouseDown[0], (int)io.MouseDrawCursor, (int)io.WantCaptureMouse,
                         (double)gx, (double)gy, (double)gdw, (double)gdh, (double)u, (double)v);
            }
            mgpu::diag::info(pr);
        }

        io.MousePos = ImVec2(want_x, want_y);
        io.MouseDrawCursor = true;   // ReShade sets this for runtimes it feeds; this one is ours
        io.MouseDown[0] = ldown;
        io.MouseDown[1] = rdown;
        io.MouseDown[2] = (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;
        io.KeyCtrl  = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        io.KeyShift = (GetAsyncKeyState(VK_SHIFT)   & 0x8000) != 0;
        io.KeyAlt   = (GetAsyncKeyState(VK_MENU)    & 0x8000) != 0;
        io.KeySuper = false;

        // ESC closes the panel (edge-triggered). The live flag is NOT cleared
        // here: it follows the close event (dllmain's open-overlay handler),
        // so a close that does not take leaves a working panel, not a stranded
        // one. HOME is the reliable close.
        static int esc_prev = 0;
        const int esc = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0 ? 1 : 0;
        if (esc != 0 && esc_prev == 0)
            runtime->open_overlay(false, reshade::api::input_source::keyboard);
        esc_prev = esc;

        why = from_game ? "fed (game runtime io)" : (free_cursor ? "fed (desktop, PanelFreeCursor)" : "fed (desktop fallback)");
    } while (0);

    static int beat = 0;
    if (why != last_why || (++beat % 600) == 0)
    {
        char pr[640];
        snprintf(pr, sizeof pr,
                 "[MGPU][R294] feed: %s%s | live=%d host=%p runtime=%p | io pos=(%.0f,%.0f) "
                 "cursor=%d wantCapture=%d down0=%d",
                 why, (why != last_why) ? "  <- CHANGED" : "",
                 g_live.load(std::memory_order_relaxed),
                 g_host.load(std::memory_order_relaxed), (void *)runtime,
                 (double)ImGui::GetIO().MousePos.x, (double)ImGui::GetIO().MousePos.y,
                 (int)ImGui::GetIO().MouseDrawCursor, (int)ImGui::GetIO().WantCaptureMouse,
                 (int)ImGui::GetIO().MouseDown[0]);
        mgpu::diag::info(pr);
        last_why = why;
    }
}

// ---- the frame hook ------------------------------------------------------------
// reshade_overlay fires once per frame for every runtime whose overlay is open,
// after ReShade's overlay window loop - so it runs whatever tab is selected,
// with the ImGui context current. A registered overlay's own callback runs only
// while its tab is selected. Registered only with PanelOverPicture=1 (attach).
void on_reshade_overlay(reshade::api::effect_runtime *runtime)
{
    feed_input(runtime);

    // Harvest the GAME runtime's pointer. Registering this event keeps ReShade
    // feeding that runtime's ImGuiIO from its own raw-input tracking every
    // present, even though its overlay is refused.
    if ((void *)runtime == g_game.load(std::memory_order_relaxed))
    {
        ImGuiIO &gio = ImGui::GetIO();
        const float x = gio.MousePos.x, y = gio.MousePos.y;
        const float dw = gio.DisplaySize.x, dh = gio.DisplaySize.y;
        const bool ok = (x != -FLT_MAX && y != -FLT_MAX && dw > 0.0f && dh > 0.0f);
        if (ok)
        {
            g_pos.store(pack2(x, y), std::memory_order_relaxed);
            g_disp.store(pack2(dw, dh), std::memory_order_relaxed);
        }
        unsigned b = ok ? 8u : 0u;
        if (gio.MouseDown[0]) b |= 1u;
        if (gio.MouseDown[1]) b |= 2u;
        if (gio.MouseDown[2]) b |= 4u;
        g_btn.store(b, std::memory_order_relaxed);
    }
}
#endif   // MGPU_HAVE_IMGUI
}   // namespace

// THE COST, which is ReShade's: registering reshade_overlay makes
// has_addon_event<reshade_overlay>() true for the whole process, which turns off
// draw_gui's early-out, so every runtime runs an empty ImGui frame per present,
// panel closed or not (the contributor measured tens of microseconds; not yet
// measured here - PO-1). It is also what keeps block_input_next_frame()
// applying. That is why it is registered only with PanelOverPicture=1.
void attach(void *module)
{
    g_module.store(module, std::memory_order_relaxed);   // R295
    int v = -1;
    if (!g_key.compare_exchange_strong(v, read_key(module), std::memory_order_acq_rel)) return;
    if (g_key.load(std::memory_order_acquire) != 1) return;   // off: nothing registered, no line
    reload_keys();   // R295: PanelFreeCursor, re-read on every open too
#if defined(MGPU_HAVE_IMGUI)
    reshade::register_event<reshade::addon_event::reshade_overlay>(on_reshade_overlay);
    mgpu::diag::info("[MGPU][R292] PanelOverPicture=1: with DcompOverlay=1 an open panel is drawn by the "
                     "GPU 1 runtime over the live neural frame, and the game's input is blocked while it is "
                     "drawn (R293). Absent or 0: the shipped behaviour, and none of this code runs.");
#else
    mgpu::diag::warn("[MGPU][R292] PanelOverPicture=1, but this build has no ImGui: the key is ignored and "
                     "the shipped behaviour stays.");
#endif
}

bool on()
{
#if defined(MGPU_HAVE_IMGUI)
    return g_key.load(std::memory_order_acquire) == 1;
#else
    return false;
#endif
}

void note_runtime(reshade::api::effect_runtime *runtime, bool is_game)
{
    if (!on() || runtime == nullptr) return;
    if (is_game) { g_game.store(runtime, std::memory_order_relaxed); return; }
    if (runtime->get_hwnd() != nullptr) return;   // only the windowless chain is the neural picture
    g_windowless.store(runtime, std::memory_order_relaxed);
    if (g_said_host.exchange(1, std::memory_order_relaxed) == 0)
    {
        char l[300];
        snprintf(l, sizeof l,
                 "[MGPU][R294] live panel host = runtime %p: it has no window, so it is the composition "
                 "swapchain - the neural picture. The panel is opened and fed on this runtime.",
                 (void *)runtime);
        mgpu::diag::info(l);
    }
}

void runtime_destroyed(reshade::api::effect_runtime *runtime)
{
    if (!on() || runtime == nullptr) return;
    void *r = runtime;
    if (g_windowless.load(std::memory_order_relaxed) == r)
        g_windowless.store(nullptr, std::memory_order_relaxed);   // re-recorded by note_runtime
    if (g_host.load(std::memory_order_relaxed) == r)
    {
        g_host.store(nullptr, std::memory_order_relaxed);
        if (g_live.exchange(0, std::memory_order_relaxed) != 0)
            mgpu::diag::info("[MGPU][R294] the panel's host runtime is being destroyed (the neural chain was "
                             "re-armed or resized): the panel is dropped and the game's input is released. "
                             "The overlay key opens it again on the new runtime.");
    }
}

bool live() { return g_live.load(std::memory_order_relaxed) != 0; }

void set_live(bool up)
{
    g_live.store(up ? 1 : 0, std::memory_order_relaxed);
    if (!up)
    {
        g_swallow.store(0, std::memory_order_relaxed);
        resume_raw_input();
        release_cursor();   // R295b: the close event and the guard timeout share one release
    }
    if (up) g_host_drawn_ms.store(GetTickCount64(), std::memory_order_relaxed);   // R293: grace until the first draw
}

void *host_target(void *fallback)
{
    void *w = g_windowless.load(std::memory_order_relaxed);
    return (w != nullptr) ? w : fallback;
}

bool is_host(void *runtime)
{
    if (runtime == nullptr) return false;
    return runtime == g_host.load(std::memory_order_relaxed) ||
           runtime == g_windowless.load(std::memory_order_relaxed);
}

void note_opened_on(void *runtime, bool open)
{
    if (!on() || runtime == nullptr) return;
    if (open)
    {
        g_host.store(runtime, std::memory_order_relaxed);
        g_host_drawn_ms.store(GetTickCount64(), std::memory_order_relaxed);
    }
}

// R293. THE INPUT-RELEASE GUARD. The game's input is blocked only while the
// host runtime is actually drawing the panel. An overlay the player cannot see
// made games look frozen while they ran fine; blocking on a flag alone could
// bring that back (host not presenting, window covered, a close event that was
// missed). Drawn within GUARD_BLOCK_MS: block. Not drawn for GUARD_RELEASE_MS:
// the panel is treated as gone - the flag is cleared and the input released.
void block_game_input(reshade::api::effect_runtime *game_runtime)
{
    if (!on() || game_runtime == nullptr) return;
    if (g_live.load(std::memory_order_relaxed) == 0)
    {
        g_swallow.store(0, std::memory_order_relaxed);
        release_cursor();   // R295b: a close that did not come through set_live()
        return;
    }

    const unsigned long long now  = GetTickCount64();
    const unsigned long long last = g_host_drawn_ms.load(std::memory_order_relaxed);
    const unsigned long long age  = (now >= last) ? now - last : 0ull;

    if (age <= GUARD_BLOCK_MS)
    {
        // R295 / PanelFreeCursor: with the panel up, take the pointer back from the
        // game. The clip is cleared every frame (the game re-clips its own window on
        // its timeline, and ours lands after it); the cursor is made visible only if
        // the game is hiding it. No SetCursorPos: if the game recentres the pointer
        // each frame, freeing the clip is not enough and the probe says so.
        if (g_free_cursor.load(std::memory_order_relaxed) != 0)
        {
            ensure_input_guard();
            g_swallow.store(1, std::memory_order_relaxed);
            enforce_raw_input();
            static unsigned long long guard_frames = 0;
            if ((++guard_frames % 180ull) == 0ull)
            {
                char gl[300];
                snprintf(gl, sizeof gl, "[MGPU][R297] guard counters: swallowed WM_INPUT=%llu mouse_msgs=%llu, raw_input=%s",
                         g_sw_input.load(std::memory_order_relaxed), g_sw_mouse.load(std::memory_order_relaxed),
                         g_rid_off ? "suspended" : "live");
                mgpu::diag::info(gl);
            }
            ClipCursor(nullptr);
            CURSORINFO ci{};
            ci.cbSize = sizeof ci;
            if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) == 0 &&
                g_cursor_shown.load(std::memory_order_relaxed) < 8)
            {
                for (int i = 0; i < 8; ++i)
                {
                    ShowCursor(TRUE);
                    g_cursor_shown.fetch_add(1, std::memory_order_relaxed);   // R295b: given back in release_cursor()
                    ci.cbSize = sizeof ci;
                    if (!GetCursorInfo(&ci) || (ci.flags & CURSOR_SHOWING) != 0) break;
                }
            }
            static std::atomic<int> said{0};
            if (said.exchange(1, std::memory_order_relaxed) == 0)
                mgpu::diag::info("[MGPU][R295] PanelFreeCursor=1: the cursor clip is cleared and the game's "
                                 "hidden cursor is shown while the panel is live. The probe line prints the "
                                 "desktop position, the clip rect and whether the cursor is visible.");
        }
        else
        {
            g_swallow.store(0, std::memory_order_relaxed);
            resume_raw_input();
            release_cursor();
        }
        game_runtime->block_input_next_frame();
        return;
    }
    if (age >= GUARD_RELEASE_MS && g_live.exchange(0, std::memory_order_relaxed) != 0)
    {
        g_swallow.store(0, std::memory_order_relaxed);
        resume_raw_input();
        release_cursor();
        char l[300];
        snprintf(l, sizeof l,
                 "[MGPU][R293] the panel has not been drawn for %llu ms: the game's input is released "
                 "and the panel state is cleared. The overlay key opens it again.%s",
                 age, (g_guard_said.exchange(1, std::memory_order_relaxed) == 0) ? " (first time this run)" : "");
        mgpu::diag::warn(l);
    }
}
// R295. PanelFreeCursor is re-read whenever the overlay key fires, so one game
// session can A/B it by editing mgpu.ini and toggling the panel; PanelOverPicture
// itself is still read once, at attach.
void reload_keys()
{
    void *m = g_module.load(std::memory_order_relaxed);
    if (m == nullptr) return;
    const int fc = read_flag(m, "PanelFreeCursor");
    const int fc0 = g_free_cursor.exchange(fc, std::memory_order_relaxed);
    if (fc0 != fc)
    {
        char l[220];
        snprintf(l, sizeof l, "[MGPU][R295] panel input key: PanelFreeCursor=%d", fc);
        mgpu::diag::info(l);
    }
}

bool free_cursor()  { return g_free_cursor.load(std::memory_order_relaxed) != 0; }

}   // namespace mgpu::live_panel
