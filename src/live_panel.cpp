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

int read_key(void *module)
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
    const char *k = mgpu::config::find(buf, got, "PanelOverPicture");
    return (k != nullptr && *k == '1') ? 1 : 0;
}

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
        const bool from_game = (btn & 8u) != 0u && gdw > 0.0f && gdh > 0.0f;

        float u = 0.0f, v = 0.0f;
        bool ldown = false, rdown = false;
        if (from_game)
        {
            u = gx / gdw;
            v = gy / gdh;
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
            char pr[560];
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

        why = from_game ? "fed (game runtime io)" : "fed (desktop fallback)";
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
    int v = -1;
    if (!g_key.compare_exchange_strong(v, read_key(module), std::memory_order_acq_rel)) return;
    if (g_key.load(std::memory_order_acquire) != 1) return;   // off: nothing registered, no line
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
    if (g_live.load(std::memory_order_relaxed) == 0) return;

    const unsigned long long now  = GetTickCount64();
    const unsigned long long last = g_host_drawn_ms.load(std::memory_order_relaxed);
    const unsigned long long age  = (now >= last) ? now - last : 0ull;

    if (age <= GUARD_BLOCK_MS)
    {
        game_runtime->block_input_next_frame();
        return;
    }
    if (age >= GUARD_RELEASE_MS && g_live.exchange(0, std::memory_order_relaxed) != 0)
    {
        char l[300];
        snprintf(l, sizeof l,
                 "[MGPU][R293] the panel has not been drawn for %llu ms: the game's input is released "
                 "and the panel state is cleared. The overlay key opens it again.%s",
                 age, (g_guard_said.exchange(1, std::memory_order_relaxed) == 0) ? " (first time this run)" : "");
        mgpu::diag::warn(l);
    }
}
}   // namespace mgpu::live_panel
