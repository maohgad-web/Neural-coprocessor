// MGPU Bridge - PanelOverPicture: the live panel (N01950 contribution).
//
// WHAT IT IS FOR. With DcompOverlay=1 the neural picture is a composition
// visual on top of the game's window, so the game's own ReShade overlay is
// underneath it. The shipped answer (V65) steps the visual aside while the
// panel is open: the panel is usable and the picture is the native one. This
// file is the other answer: the panel is opened on the GPU 1 runtime whose
// swapchain IS the neural picture (the one with no window), so it is drawn
// over the live neural frame. That runtime gets no input from ReShade, so this
// file feeds its ImGui input, and the game's input is blocked while the panel
// is drawn.
//
// OFF BY DEFAULT. PanelOverPicture=1 in mgpu.ini turns it on. The key is read
// ONCE, in DllMain (attach), because the reshade_overlay event this needs must
// be registered there with every other event. Key absent or 0: the event is
// not registered, on() is false, every hook below returns at once, and the
// add-on behaves exactly as 0.3.1.
//
// OWNERSHIP. Contributed in the N01950 pull request; this file is that path's
// home. dllmain.cpp only calls the functions below; gpu1_context.cpp only
// provides dcomp_visual_rooted() and dcomp_game_hwnd().
//
// Log tags: [R292] the key, [R293] the input-release guard, [R294] the panel.
#pragma once

#include <reshade.hpp>

namespace mgpu::live_panel
{
    // DllMain, DLL_PROCESS_ATTACH, at the place of the other registrations.
    // Reads PanelOverPicture once; registers reshade_overlay only when it is 1
    // (and the build has ImGui). One log line only when it is on.
    void attach(void *module);

    // The key, as read at attach, AND an ImGui build. The single answer every
    // dllmain call site uses, so the panel is never half on.
    bool on();

    // R158's init_effect_runtime / destroy_effect_runtime. Records the game's
    // runtime and the windowless GPU 1 runtime; drops the host on destroy.
    void note_runtime(reshade::api::effect_runtime *runtime, bool is_game);
    void runtime_destroyed(reshade::api::effect_runtime *runtime);

    // The panel's state (1 = up on the neural layer).
    bool live();
    void set_live(bool up);

    // The GPU 1 runtime to open the overlay on: the windowless one, or
    // `fallback` (R158's bridge slot) before it has been seen.
    void *host_target(void *fallback);
    // True for the runtime the panel was opened on, or the windowless one.
    bool is_host(void *runtime);
    // R158's apply, the moment open_overlay is called on the GPU 1 side.
    void note_opened_on(void *runtime, bool open);

    // The game chain's present, on its own thread. Blocks the game's input for
    // the next frame while the panel is live AND being drawn (R293).
    void block_game_input(reshade::api::effect_runtime *game_runtime);

    // [PORT PRESENTER, 2026-11-11] B1.1b, copied from nc-base: while the presenter's
    // own small window is up it owns the mouse via raw input; the game's input has to
    // be blocked even though no ReShade panel is live. Read by dllmain's guard.
    void set_b1_input(bool on);
    bool b1_input_active();
}
