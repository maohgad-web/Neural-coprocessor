// MGPU Bridge - Cross-Adapter Bridge, Milestone 0 (Gate P0)
//
// T1: register the add-on with stock ReShade, write one init log line.
// T2: per device event, a provisional game-LUID capture (first d3d12
//     device) and a one-time adapter-table enumeration; the selection
//     itself is deferred to the swapchain-derived game LUID (init_swapchain)
//     - the four rules of brief section 06 live in adapter.hpp/adapter.cpp.
// T3: the bridge thread (spawned on the game thread from
//     on_init_swapchain only, never from on_init_device and never from
//     DllMain) creates the private device on the selected adapter;
//     destroy_device events are logged, and the game's own device
//     release triggers the bridge thread's orderly teardown.
// T4: captures this module's own HMODULE at DLL_PROCESS_ATTACH and exposes
//     it via mgpu::module_handle() - the window class's hInstance must be
//     the add-on's handle, and GetModuleHandle(nullptr) returns the game's
//     module, not ours (brief section 00, exception 1).
//
// Convention verified against crosire/reshade v6.8.0
// (18deaa52de0c425a78b329e9cb3c497281cd00ec), include/reshade.hpp,
// include/reshade_events.hpp and examples/01-fps_limit: add-ons export
// NAME and DESCRIPTION, and DllMain calls reshade::register_addon(hModule),
// which fails cleanly if ReShade is absent or its add-on API is older
// than RESHADE_API_VERSION.
//
// API 20 note (verified in reshade_events.hpp at the pinned SHA): there
// is no reshade_init / reshade_unload addon event, and no
// device_removed / device_restored. The device lifecycle event is
// destroy_device - void (api::device *) - fired before
// ID3D12Device::Release. Teardown is therefore driven from
// destroy_device on the game's device.
//
// get_native() returns uint64_t: unwrapping it to ID3D12Device * needs
// reinterpret_cast, not static_cast (integer to pointer is only
// reinterpret_cast). GetAdapterLuid() takes no parameters and returns
// the LUID by value - there is no SUCCEEDED to check.

#include <windows.h>
#include <d3d12.h>
#include <cstdio>     // P1.6: snprintf. This file had no formatted logging before.

// ---- P6.4: the overlay panel ----
//
// reshade.hpp wires up the ImGui function table ONLY when IMGUI_VERSION_NUM is
// already defined when it is included - the block is #if'd on that - and the
// table is bound inside register_addon. So imgui.h has to come first, in THIS
// translation unit, because this is where register_addon is called.
//
// GUARDED, because the ImGui headers are a dependency this repo may not carry
// and a build that fails on the rig at 3am is worse than a build with no panel.
// Without them the add-on compiles exactly as before and the P6.3 hotkeys are
// the control; the startup log says which of the two you got, so a missing
// panel is never a mystery.
//
// P7.1: TWO THINGS THE HEADER DEMANDS AND WILL NOT INFER.
//   1. ImTextureID must be 8 bytes. reshade_overlay.hpp has a static_assert on
//      exactly that, because ReShade passes a resource_view through it. ImGui's
//      default is a void*, which IS 8 bytes on x64 - the assert would pass - but
//      ReShade itself is compiled with ImTextureID=ImU64 and a type that merely
//      happens to be the same width is not the same type in the structs either
//      side of the table. Define it, the way ReShade's own build does.
//   2. The version must be EXACTLY 19250 (ImGui 1.92.5). reshade_overlay.hpp
//      #errors on anything else - the function table is a version-numbered
//      struct of raw pointers, so a near-miss is a silent ABI mismatch and the
//      header refuses rather than letting it happen. Vendor that exact tag.
#if defined(__has_include)
#  if __has_include(<imgui.h>)
#    define MGPU_HAVE_IMGUI 1
#    define ImTextureID ImU64
#    include <imgui.h>
#  endif
#endif

#include <reshade.hpp>

#include "adapter.hpp"
#include "diag.hpp"
#include "gpu1_context.hpp"
#include "calibrator.hpp"
#include "probe.hpp"
#include "sl_probe.hpp"   // SL1
#include "worker.hpp"

extern "C" __declspec(dllexport) const char *NAME = "MGPU Bridge";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "Cross-Adapter Bridge P0: second ReShade effect runtime on a second GPU";

#define MGPU_STR2(s) #s
#define MGPU_STR(s) MGPU_STR2(s)

// T4 (brief section 00, exception 1): the add-on's own module handle,
// captured at DLL_PROCESS_ATTACH. GetModuleHandle(nullptr) returns the
// *game's* module, not ours, so the T4 window class's hInstance must be
// this handle. The accessor is declared in worker.cpp (the file manifest
// is closed, so no new header is added); it is ordinary C++ linkage,
// resolved within this DLL at link time.
static HMODULE g_module = nullptr;

namespace mgpu
{
    HMODULE module_handle()
    {
        return g_module;
    }
}

// T2: both init_device and init_swapchain are the game's own separate API
// calls - ReShade's CreateDXGIFactory1 hook is fully unwound when they
// fire, so the enumeration's factory creation does not recurse into
// add-on init. (Swapchain CREATION stays off the game thread entirely -
// that is the re-entrancy hazard, handled in T4+.)
// The bridge thread is deliberately NOT started here. UE5 probes every
// adapter before settling, and ReShade loads and unloads this add-on once
// per probe - five cycles per launch on this rig. A thread started here
// would wait on a ready event that cannot be set (rule 2: no selection is
// possible before a swapchain exists), and would then be torn down inside
// ReShade's unload window. That killed the game process on the rig: the
// module unmaps while the thread is still executing inside it and the
// thread faults on unmapped code, with no log line possible because the
// code that would write it is gone. It is a race - the same binary
// launched cleanly minutes earlier - so the fix is to have no thread
// during a probe cycle at all, not to make the teardown faster.
// ensure_started() therefore lives only in on_init_swapchain, which fires
// once, on the real render device.
static void on_init_device(reshade::api::device *device)
{
    // Every device event is logged; the first d3d12 one also captures the
    // provisional game LUID and builds the adapter table. No selection
    // here - brief rule 2 defers it to the swapchain-derived game LUID
    // (on_init_swapchain).
    mgpu::adapter::on_device(device);

    // ---- R101b RETRACTED. init_device was the wrong place. ----
    //
    // MEASURED: Dragon Sword and Dawnwalker stopped LAUNCHING - "Failed to
    // load add-on" on a SECOND D3D12CreateDevice, after the first instance
    // had unregistered. Plague Tale survived because its init order creates
    // the device once.
    //
    // The install site was not the real defect; it EXPOSED one. We patch
    // GetProcAddress slots in ~150 modules and nothing ever put them back, so
    // an add-on unload left every one of those slots pointing into a DLL that
    // is no longer there. install() at init_device made that unload/reload
    // cycle happen with the patches live. uninstall() is now called at
    // detach - see DllMain - and the install goes back to the swapchain
    // one-shot, which launched on every title.
    //
    // The early-install idea is NOT dead, but it needs a module walk that
    // does not depend on ToolHelp and an unload path that is proven first.
}

// ---- R78: THE MOTION VECTOR TRANSPORT'S ONE LINE OF GLUE ----
//
// The probe calls this from the render-target bind event, mid-frame, with the
// game's command list already open and the source already transitioned to
// copy_source - the probe issues that barrier and puts it back, because the
// barrier is a ReShade type and gpu1_context holds none.
//
// A FUNCTION POINTER RATHER THAN A CALL. probe.cpp does not include
// gpu1_context.hpp and must not start: the probe is a diagnostic that has to
// keep working in a build where the stream is inert, and a direct call would
// make the acquisition layer depend on the transport layer. So the transport
// hands the probe a pointer at startup and the probe knows nothing about what
// is on the other end of it.
//
// Inert unless MVec=3 and the stream is armed - the check is inside
// stream_mvec_copy, under the mutex that owns the answer.
static void mgpu_mvec_transport_hook(void *cmd_list_native, unsigned long long resource)
{
    mgpu::gpu1::stream_mvec_copy(cmd_list_native, resource);
}

static void on_init_swapchain(reshade::api::swapchain *swapchain, bool resize)
{
    // The only place the bridge thread is started - see the note above
    // on_init_device. This event fires on the real render device, after
    // the probe cycles are over, so the thread is never alive inside an
    // add-on unload window. Started before on_swapchain so the thread is
    // already waiting when the selection is decided.
    mgpu::worker::ensure_started();
    // The swapchain's device is the authoritative game render device. Its
    // LUID overrides the provisional init_device value and runs the
    // one-shot selection (or a terminal refusal) - see adapter.cpp.
    mgpu::adapter::on_swapchain(swapchain, resize);

    // P9.1. The presented size, handed to the probe so its candidate size band
    // is a fraction of the user's resolution rather than a pixel count somebody
    // guessed. Read here and not at the first frame because a filter that is
    // wrong for the first few hundred resources is a filter that missed the
    // engine's startup allocations, which is when scene buffers are created.
    //
    // R33 DEFECT. THIS WAS UNFILTERED AND IT SILENTLY BROKE THE SIZE BAND.
    // init_swapchain fires for BOTH runtimes in this process, and the bridge's
    // own present chain is 1280x720. It came up after the game's, so the band
    // ended up a fraction of 1280x720 instead of the game's 2560x1440 - and
    // 1664x936 scene depth is 1.69x the area of 1280x720, which the band's
    // upper bound (1.05x) then REJECTS. The lane's candidates survived only
    // because they were catalogued before the bridge chain existed; anything
    // created after it was dropped, silently, and the R33 turn-open test that
    // asks in_scene_band per clear returned false for every frame of a whole
    // run. That is what a wrong filter looks like: not an error, an absence.
    //
    // The fix is the check note_frame twelve lines below has always had. When
    // the game's LUID is not yet known nothing is passed at all, which leaves
    // the band at its permissive fallback (w>=640 && h>=360) - being late with
    // the real size costs a few loose candidates; being wrong about it costs
    // the ones that matter.
    if (swapchain != nullptr)
    {
        if (reshade::api::device *sd = swapchain->get_device())
        {
            mgpu::adapter::selection_result ssel;
            mgpu::adapter::get_selection(ssel);
            bool sc_is_game = false;
            if (ssel.game_luid_known && sd->get_api() == reshade::api::device_api::d3d12)
            {
                if (auto *sd12 = reinterpret_cast<ID3D12Device *>(sd->get_native()))
                {
                    // SL1. Stored, not queried here: the census line wants
                    // both devices and this is the side that can see the
                    // game's. No reference is taken.
                    mgpu::slprobe::note_game_device(sd12);

                    const LUID sl = sd12->GetAdapterLuid();
                    sc_is_game = (sl.LowPart == ssel.game_luid.LowPart &&
                                  sl.HighPart == ssel.game_luid.HighPart);
                }
            }
            const reshade::api::resource bb = swapchain->get_back_buffer(0);
            if (sc_is_game && bb.handle != 0)
            {
                const reshade::api::resource_desc bd = sd->get_resource_desc(bb);
                mgpu::probe::note_scene_size(bd.texture.width, bd.texture.height);
            }
        }
    }

    // P9.1. The probe's one-shot init, here rather than in DllMain: this is a
    // render thread with the loader lock released, so the ini read is a plain
    // file read and register_event is on the thread ReShade raises events on.
    // Guarded because init_swapchain fires again on resize, and re-reading the
    // ini there would silently undo a toggle made in the overlay.
    {
        static bool probe_started = false;
        if (!probe_started)
        {
            probe_started = true;

            // ---- R78: INSTALL THE HOOK BEFORE THE FIRST set_mode ----
            //
            // The only ordering requirement in this block. The probe LATCHES
            // its mvec lane on when a hook is installed, and that latch is
            // read INSIDE set_mode - so a hook installed afterwards would
            // leave the lane at whatever the ini asked for, and the latch
            // would not take effect until the next mode change, which may
            // never come.
            //
            // Unconditional, and deliberately not gated on the ini here: that
            // would mean reading the same key in two files and being able to
            // disagree with ourselves about it. The hook is inert unless
            // MVec=3 and the stream is armed.
            mgpu::probe::set_mvec_hook(&mgpu_mvec_transport_hook);

            mgpu::probe::set_mode(mgpu::probe::mode_from_ini());

            // ---- R101: THE NGX TAP, INSTALLED ON THE SAME ONE-SHOT ----
            //
            // After mode_from_ini, because that call is what parses the ini
            // and therefore what Calib= has been read by. Installing here
            // rather than in DllMain matters for the same reason the probe's
            // init does: the loader lock is released on this thread, and
            // walking every module's import table under the loader lock is
            // how you deadlock a game at startup.
            //
            // Calib=0 returns immediately and touches nothing.
            mgpu::calibrator::install(mgpu::probe::calib_mode());
            mgpu::calibrator::set_jitter_mode(mgpu::probe::jitter_mode());

            // R106. The SAME transport hook the probe uses - one copy path,
            // two possible triggers, so the two can be compared directly
            // instead of being two different pieces of code.
            mgpu::calibrator::set_mvec_hook(&mgpu_mvec_transport_hook);
            mgpu::calibrator::set_eval_copy(mgpu::probe::eval_copy_mode());
        }
    }
}

// ---- P1.6: what is actually painted on each runtime ----
//
// INCIDENT 6, and the cheapest one to have prevented. The bridge window showed
// the neural output with the colours destroyed - psychedelic banding over
// black - and three separate code hypotheses were formed and shipped against
// it. None was the cause. gpu1.ini, the preset ReShade assigns to the BRIDGE
// runtime, still carried "Techniques=Lumenite_QuantMotion@lumenite_QuantMotion.fx"
// with "DEBUG_FLOW=1" from an earlier debugging session. A motion-flow debug
// view was being drawn on top of every neural frame, and nothing in any log
// said so.
//
// The instrument could not see the one thing that was wrong, because the run
// CONFIGURATION - which preset each runtime loaded, and which techniques it
// enabled - was never recorded anywhere. A bisect run under a contaminated
// preset returns a confident, correctly-formatted, wrong answer, which is the
// section 00 failure shape exactly.
//
// So both runtimes now state, once each, the preset they loaded and every
// technique enabled in it. After this, "something was painted over the output"
// is a line in the log rather than a hypothesis about our code.
//
// The technique list is not read on the first event: at that point effects may
// still be compiling and an empty list would be recorded as "nothing enabled",
// which is the same wrong answer in a different costume. The read is retried
// until the runtime enumerates at least one technique, and if it never does,
// THAT is said explicitly instead.
namespace
{
    struct preset_probe
    {
        bool done = false;
        unsigned attempts = 0;
        char names[900] = {};
        size_t used = 0;
        unsigned enabled = 0;
        unsigned total = 0;
    };

    void technique_cb(reshade::api::effect_runtime *rt,
                      reshade::api::effect_technique tech, void *user)
    {
        preset_probe *p = static_cast<preset_probe *>(user);
        ++p->total;
        if (!rt->get_technique_state(tech)) return;
        ++p->enabled;

        char tn[128] = {}; size_t tns = sizeof tn - 1;
        rt->get_technique_name(tech, tn, &tns);
        char en[128] = {}; size_t ens = sizeof en - 1;
        rt->get_technique_effect_name(tech, en, &ens);

        const int wrote = snprintf(p->names + p->used, sizeof p->names - p->used,
                                   "%s%s@%s", (p->used != 0) ? ", " : "", tn, en);
        if (wrote > 0 && (size_t)wrote < sizeof p->names - p->used)
            p->used += (size_t)wrote;
    }

    // `tag` is "GAME" or "BRIDGE". Two independent probes, because the two
    // runtimes load different presets and either one can change what is on
    // screen.
    void log_preset_once(reshade::api::effect_runtime *runtime, const char *tag,
                         preset_probe &p)
    {
        if (p.done) return;
        ++p.attempts;

        p.used = 0; p.enabled = 0; p.total = 0; p.names[0] = '\0';
        runtime->enumerate_techniques(nullptr, technique_cb, &p);

        // Still compiling: say nothing yet rather than record an empty list as
        // a fact. 900 events is roughly fifteen seconds at 60 fps, comfortably
        // past shader compilation on this rig.
        if (p.total == 0 && p.attempts < 900) return;
        p.done = true;

        char preset[512] = {}; size_t ps = sizeof preset - 1;
        runtime->get_current_preset_path(preset, &ps);

        char line[1600];
        if (p.total == 0)
            snprintf(line, sizeof line,
                     "[MGPU][P1.6] %s runtime preset=\"%s\" - NO TECHNIQUES ENUMERATED after %u "
                     "frames. Either the preset is empty or its effects failed to compile. "
                     "Nothing is being drawn on this runtime.",
                     tag, preset, p.attempts);
        else
            snprintf(line, sizeof line,
                     "[MGPU][P1.6] %s runtime preset=\"%s\" | %u of %u techniques ENABLED%s%s. "
                     "This line exists because a stale gpu1.ini once drew a motion-flow debug "
                     "view over the neural output and three code hypotheses were spent on it. "
                     "If anything unexpected is listed here, what is on screen is not what this "
                     "add-on produced.",
                     tag, preset, p.enabled, p.total,
                     (p.enabled != 0) ? ": " : "",
                     (p.enabled != 0) ? p.names : "");
        mgpu::diag::info(line);
    }
}

#if defined(MGPU_HAVE_IMGUI)
// The panel. Drawn into the BRIDGE runtime's overlay - press Home over the
// bridge window to open it.
//
// Everything it touches goes through the plain-scalar accessors in
// gpu1_context.hpp, so this stays the only file that knows about both ReShade
// and the stream.
//
// Deliberately read-then-write, never read-modify-write across a frame: the
// callback runs on the presenting thread, the stream runs on the bridge thread,
// and holding a value across the gap is how a slider fights with a hotkey.
static void draw_mgpu_overlay(reshade::api::effect_runtime *)
{
    // P7.10: THE UNSUPPORTED-API MESSAGE HAS TO LIVE HERE, and it is worth
    // saying why, because the obvious place was the bridge window's title and
    // that place cannot work. On a D3D11 or Vulkan title no D3D12 render device
    // is ever found, so no adapter is selected, so no device is created, so no
    // window exists to carry a title. This panel is registered on the GAME's
    // runtime as well, and ReShade's overlay draws on D3D11 and Vulkan - so it
    // is the one surface that still exists when the add-on has nothing to do.
    // A user who installs this on a D3D11 game and sees nothing at all has been
    // given no way to find out why, and will reasonably conclude it is broken.
    {
        mgpu::adapter::selection_result sel;
        mgpu::adapter::get_selection(sel);
        if (!sel.valid && mgpu::adapter::non_d3d12_swapchain_events() != 0)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f),
                               "D3D11 and Vulkan are not supported. This add-on does nothing here.");
            ImGui::TextDisabled("It hooks ReShade's D3D12 path. Some Unity titles accept -force-d3d12.");
            return;
        }
    }

    // V19. Tell the bridge the panel is open, so AutoArm waits. Settings are
    // read at arm time, so a menu that gets armed out from under the reader is
    // a menu that does nothing.
    mgpu::gpu1::ui_panel_drawn();

    mgpu::gpu1::ui_state st;
    mgpu::gpu1::ui_read(st);

    // One status line, not three. It changes rather than accumulating.
    if (!st.armed)
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                           "Not armed - CTRL+ALT+F10 in gameplay. Settings below apply on arm.");
    else if (st.summarised)
        ImGui::TextUnformatted("Finished - ran to its bound. Restart the game to run another.");

    // ========================= BOX 1: THE BRIDGE =========================
    //
    // Split into two boxes in V11 because the panel had become one column of
    // unrelated things. This box is the NEURAL side - what the bridge does with
    // the frame and how fast it is doing it. The DLSS box below is the
    // UPSCALER's state, which is a different feature with a different handle
    // and, unlike everything here, cannot be changed while armed.
// ---- V12: TWO COLUMNS, NOT ONE TALL STACK ----
    //
    // The single column had grown past a screen, so the DLSS box was below the
    // fold and you had to scroll to find it. Two children side by side, each
    // scrolling on its own, puts both in view at once.
    //
    // IT FALLS BACK TO STACKED BELOW 640 px. A two-column layout in a narrow
    // overlay is two unreadable columns, which is worse than scrolling. The
    // overlay's width is the user's to change and we do not control it.
    const float avail_w = ImGui::GetContentRegionAvail().x;
    const bool  two_col = (avail_w >= 640.0f);
    const float col_w   = two_col
        ? (avail_w - ImGui::GetStyle().ItemSpacing.x) * 0.5f
        : avail_w;
    // Tall enough that neither column scrolls in the common case, short enough
    // to leave the probe section visible underneath.
    const float col_h   = 560.0f;

    ImGui::BeginChild("mgpu_box_bridge", ImVec2(col_w, col_h), true);
    ImGui::SeparatorText("MGPU BRIDGE  -  neural rendering");
    {
    // ---- Model tuning, first and always visible ----
    //
    // Moved to the top in P7.10. These are the controls the reference
    // implementation sets and this one leaves at the feature's defaults, so
    // they are the first thing to reach for when the image looks wrong - and
    // "the image looks wrong" is why most people will open this panel at all.
    // Burying them under four other groups meant nobody found them.
    ImGui::SeparatorText("Model");

    int style_i = (int)(st.style + 0.5f);
    ImGui::TextUnformatted("Style");
    ImGui::SameLine();
    if (ImGui::RadioButton("A", &style_i, 0)) mgpu::gpu1::ui_set_tuning_value(3, 0.0f);
    ImGui::SameLine();
    if (ImGui::RadioButton("B", &style_i, 1)) mgpu::gpu1::ui_set_tuning_value(3, 1.0f);
    ImGui::SameLine();
    if (ImGui::RadioButton("C", &style_i, 2)) mgpu::gpu1::ui_set_tuning_value(3, 2.0f);

    float tone = st.tone_strength;
    if (ImGui::SliderFloat("tone", &tone, 0.0f, 2.0f, "%.2f"))
        mgpu::gpu1::ui_set_tuning_value(0, tone);
    float structure = st.structure_strength;
    if (ImGui::SliderFloat("structure", &structure, 0.0f, 2.0f, "%.2f"))
        mgpu::gpu1::ui_set_tuning_value(1, structure);
    float skin = st.skin_strength;
    if (ImGui::SliderFloat("skin", &skin, 0.0f, 2.0f, "%.2f"))
        mgpu::gpu1::ui_set_tuning_value(2, skin);
    bool mask = st.auto_mask;
    if (ImGui::Checkbox("auto mask", &mask))
        mgpu::gpu1::ui_set_tuning_value(4, mask ? 1.0f : 0.0f);

    if (!st.tuning_on)
        ImGui::TextDisabled("Not applied yet - move any control above to enable the group.");
    else
    {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Applied - this is a tuning run.");
        ImGui::SameLine();
        if (ImGui::SmallButton("reset")) mgpu::gpu1::ui_set_tuning(false);
    }
    ImGui::TextDisabled("Washed or flat colour? Take tone down - 0.00 fixed it on the dev rig.");

    // ---- Passes ----
    ImGui::SeparatorText("Passes");
    int passes = (int)st.passes;
    for (unsigned i = 1; i <= st.max_passes; ++i)
    {
        char lab[8];
        snprintf(lab, sizeof lab, "x%u", i);
        if (i > 1) ImGui::SameLine();
        if (ImGui::RadioButton(lab, &passes, (int)i))
            mgpu::gpu1::ui_set_passes(i);
    }
    // ONE line, and it goes red when the setting it warns about is active. The
    // panel used to carry six lines of explanation here; a warning nobody
    // finishes reading is not a warning. The reasoning is in mgpu.ini.
    if (passes >= 2)
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f),
                           "x2 is free to the game but holds the second GPU near its power "
                           "limit. x1 recommended.");
    else
        ImGui::TextDisabled("x1 leaves the second GPU about half idle.");

    bool neural = st.neural;
    if (ImGui::Checkbox("neural stage", &neural))
        mgpu::gpu1::ui_set_neural(neural);
    ImGui::SameLine();
    ImGui::TextDisabled(st.nr_ok ? "(running)" : "(transport only)");

    // ---- Intensity ----
    ImGui::SeparatorText("Intensity");
    int pr = st.preset;
    if (ImGui::RadioButton("manual", &pr, 0))       mgpu::gpu1::ui_set_preset(0);
    ImGui::SameLine();
    if (ImGui::RadioButton("front-loaded", &pr, 1)) mgpu::gpu1::ui_set_preset(1);
    ImGui::SameLine();
    if (ImGui::RadioButton("back-loaded", &pr, 2))  mgpu::gpu1::ui_set_preset(2);

    float all = st.intensity[0];
    if (ImGui::SliderFloat("all passes", &all, 0.0f, 2.0f, "%.2f"))
        mgpu::gpu1::ui_set_intensity(0, all);
    for (unsigned i = 0; i < st.passes && i < st.max_passes; ++i)
    {
        char lab[24];
        snprintf(lab, sizeof lab, "pass %u", i + 1);
        float v = st.intensity[i];
        if (ImGui::SliderFloat(lab, &v, 0.0f, 2.0f, "%.2f"))
            mgpu::gpu1::ui_set_intensity(i + 1, v);
    }
    if (st.passes == 1)
        ImGui::TextDisabled("At x1 the three shapes are identical - there is one pass to shape.");

    // ---- View ----
    ImGui::SeparatorText("View");
    int pm = st.present_mode;
    if (ImGui::RadioButton("output", &pm, 0)) mgpu::gpu1::ui_set_present_mode(0);
    ImGui::SameLine();
    if (ImGui::RadioButton("input", &pm, 1))  mgpu::gpu1::ui_set_present_mode(1);
    ImGui::SameLine();
    if (ImGui::RadioButton("split", &pm, 2))  mgpu::gpu1::ui_set_present_mode(2);
    float sp = st.split_pos;
    if (ImGui::SliderFloat("seam", &sp, 0.0f, 1.0f, "%.2f"))
        mgpu::gpu1::ui_set_split_pos(sp);
    ImGui::TextDisabled("Split: input left of the seam, output right, same frame.");
    ImGui::TextDisabled("CTRL+ALT+LEFT/RIGHT move it with no overlay open (SHIFT = coarse).");

    // ---- Counters ----
    if (st.armed)
    {
        ImGui::SeparatorText("Counters");
        char c1[200];
        snprintf(c1, sizeof c1,
                 "produced %llu  consumed %llu  dropped %llu  overrun %llu  skipped %llu",
                 st.produced, st.consumed, st.dropped, st.overrun, st.skipped);
        ImGui::TextUnformatted(c1);
        if (st.overrun != 0)
            ImGui::TextDisabled("Overrun climbing = GPU 1 is past its budget at this pass count.");
    }

    // ---- Live ----
    //
    // EVERY NUMBER HERE IS THE ONE ITS END-OF-RUN LOG LINE PRINTS, over the
    // same divisor. If the panel and the log ever disagree, the panel is wrong
    // and it is a bug, not a second opinion.
    if (st.armed)
    {
        ImGui::SeparatorText("Live");
        char lv[260];

        snprintf(lv, sizeof lv, "fps   game %.1f   GPU 1 %.1f",
                 st.fps_produced, st.fps_consumed);
        ImGui::TextUnformatted(lv);
        if (st.fps_consumed + 1.0 < st.fps_produced)
            ImGui::TextDisabled("GPU 1 behind the game - frames are being skipped, not dropped.");

        if (st.gpu1_ts_ok)
        {
            snprintf(lv, sizeof lv,
                     "GPU 1 ms  copy %.3f   unpack %.3f   EVALUATE %.3f   out %.3f",
                     st.gpu1_copy_ms, st.gpu1_unpack_ms, st.gpu1_eval_ms, st.gpu1_out_ms);
            ImGui::TextUnformatted(lv);
            ImGui::TextDisabled(st.sr_on
                ? "Evaluate spans every pass AND the reduce and upscale - not the neural cost alone."
                : "Evaluate spans every pass - divide by the pass count to compare against one.");
        }

        snprintf(lv, sizeof lv, "latency  ready-to-consume %.2f ms   submit-to-consume %.2f ms",
                 st.lat_ready_ms, st.lat_submit_ms);
        ImGui::TextUnformatted(lv);
        ImGui::TextDisabled("Bridge share only - the game's render before and the present after are not in it.");

        snprintf(lv, sizeof lv, "GPU 0 queue  mean %.2f   max %llu frames behind",
                 st.backlog_mean, st.backlog_max);
        if (st.backlog_mean >= 2.5)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f), "%s", lv);
            ImGui::TextDisabled("Queueing deep. Reflex=1 in mgpu.ini, or turn it on in the game.");
        }
        else
        {
            ImGui::TextUnformatted(lv);
            if (st.backlog_mean > 0.0 && st.backlog_mean <= 1.6)
                ImGui::TextDisabled("1.0 is the structural floor, not an error. This is what low latency looks like.");
        }

        snprintf(lv, sizeof lv, "ring  %u slots%s   skipped by window %llu",
                 st.ring_depth,
                 (st.ring_window != 0) ? "  (window ON)" : "  (window off)",
                 st.ring_skipped);
        ImGui::TextUnformatted(lv);

        if (st.reordered != 0 || st.bad_magic != 0 || st.contract != 0)
        {
            snprintf(lv, sizeof lv, "SEAL FAULTS  reordered %llu  bad magic %llu  contract %llu",
                     st.reordered, st.bad_magic, st.contract);
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f), "%s", lv);
        }
    }

    }
    ImGui::EndChild();   // ---- end BOX 1 ----

    if (two_col) ImGui::SameLine();

    // ====================== BOX 2: DLSS AND REFLEX ======================
    //
    // READ-ONLY, AND THAT IS A DECISION RATHER THAN AN OMISSION. Quality,
    // preset and R are baked into an NGX feature handle at arm time. Changing
    // one means releasing and rebuilding that handle mid-stream: a 200-450 ms
    // stall on the bridge thread, down a teardown path that has crashed
    // before. These are set in mgpu.ini and take effect on the next arm.
    ImGui::BeginChild("mgpu_box_dlss", ImVec2(col_w, col_h), true);
    ImGui::SeparatorText("DLSS  -  super resolution and Reflex");
    {
        char dl[300];

        // ================= BEFORE ARM: THE SETTINGS MENU =================
        //
        // EVERY CONTROL HERE WRITES mgpu.ini IMMEDIATELY, and the arm reads
        // that file, so a change made before arming applies to THIS session
        // with no restart. That is the whole reason the menu is worth having,
        // and the reason AutoArm holds while this panel is open.
        //
        // Only settings a user should touch are here. Passes, Depth, MVec and
        // CopyQueue stay out on purpose: they are pipeline internals, and each
        // one can produce a configuration nobody can support from a bug report.
        if (!st.armed && !st.summarised)
        {
            // V26. Reflex is not read here - that control is gone. SRScale IS,
            // because the mode buttons below now write it.
            static bool m_loaded = false;
            static bool m_sr = false;
            static int  m_preset = 0, m_mode = 2;
            // V27: the match-game toggle, and a restart breadcrumb.
            //
            // m_match is DERIVED from SRScale rather than stored in a key of
            // its own. SRScale=0 already means "inherit R from the game's
            // render extent" and there is no second meaning for it, so a
            // separate ini key would be a second source of truth for one fact
            // - which is how SRScale and SRMvLowRes got out of step in the
            // first place. Read it, do not store it.
            static bool m_match = false;
            if (!m_loaded)
            {
                m_loaded = true;
                m_sr     = (mgpu::gpu1::ui_ini_read("SRUpscale", 0) != 0);
                m_preset = mgpu::gpu1::ui_ini_read("SRPreset", 0);
                m_mode   = mgpu::gpu1::ui_ini_read("SRQuality", 2);
                m_match  = (mgpu::gpu1::ui_ini_read("SRScale", 0) == 0);
            }

            // ---- V26: THE MODE SETS R. THE SAME LADDER THE ARMED VIEW USES ----
            //
            // quality 67, balanced 58, performance 50 - percent per axis, and
            // 67 is DLSS Quality's own ratio. Identical to the armed view's
            // APPLY and to stream_state::AUTO_LADDER, because three different
            // ladders in one add-on is how a setting stops meaning anything.
            //
            // WHY THIS REPLACES THE DLAA TOGGLE ENTIRELY. Until now R was
            // INHERITED from the game's render extent, so if the title was
            // rendering at native - DLAA, or no DLSS at all - there was
            // nothing smaller to enlarge and every button in this box did
            // nothing. Four of the five titles measured 2026-09-13 were in
            // exactly that state. The user saw a DLSS section with modes and
            // presets that changed nothing, and a separate "DLAA mode" toggle
            // whose relationship to it was not guessable.
            //
            // With R chosen here, the modes mean the same thing on every
            // title whatever the game's own DLSS is set to, and the DLAA
            // concept disappears from the interface because it was never a
            // mode - it was the absence of one.
            //
            // SRMvLowRes RIDES WITH THE SCALE, ALWAYS. The flag without the
            // scale is the combination that returned FAIL_PlatformError on
            // 2026-09-12. They are one setting and they are written together
            // in every path below.
            //
            // ---- V27: AND THE MATCH-GAME BRANCH WRITES THE OTHER PAIR ----
            //
            // Ledger 6k measured two configurations and they are both valid:
            //
            //   SRScale=67 SRMvLowRes=1   R is ours. Works on every title,
            //                             including one rendering at native.
            //                             The default, and cheaper.
            //   SRScale=0  SRMvLowRes=0   R is the game's own render extent,
            //                             so the vectors ARE at R and MV_Scale
            //                             is untouched. Only does anything
            //                             when the game is itself upscaling.
            //
            // 6k's A/B: evaluate 6.492 ms against 7.459 ms, and L2 10.34
            // against 11.40. That ~1 ms is the AREA, not the flag - run A's R
            // was the game's 1485x835 against run B's 1280x720. The flag
            // itself is a boolean and a scale handed to DLSS.
            //
            // 6k also recorded that mode 0 with the vectors NOT at R crashed
            // at arm, which is why the pair is written together here and why
            // the toggle cannot produce the third combination.
            const auto write_mode = [](int mode)
            {
                mgpu::gpu1::ui_ini_write("SRQuality", mode);
                if (m_match)
                {
                    mgpu::gpu1::ui_ini_write("SRScale",    0);
                    mgpu::gpu1::ui_ini_write("SRMvLowRes", 0);
                }
                else
                {
                    const int scale = (mode == 2) ? 67 : ((mode == 1) ? 58 : 50);
                    mgpu::gpu1::ui_ini_write("SRScale",    scale);
                    mgpu::gpu1::ui_ini_write("SRMvLowRes", 1);
                }
            };

            ImGui::SeparatorText("SECOND GPU");
            // V27. BEFORE THE CONTROLS, for the reason the V12 block below
            // records: a warning printed after the buttons arrives too late
            // for the person who already clicked one. Everything here writes
            // mgpu.ini on the click, and mgpu.ini is read when the stream
            // ARMS - so nothing in this section changes a running session.
            if (ImGui::Checkbox("Enable DLSS on GPU 1", &m_sr))
            {
                mgpu::gpu1::ui_ini_write("SRUpscale", m_sr ? 1 : 0);
                // Turning it on must leave a usable R behind even when the
                // ini came from 0.1.0 and has SRScale=0 in it. write_mode
                // honours m_match, so an ini that already said SRScale=0 comes
                // back with the toggle on rather than being silently reset.
                if (m_sr) write_mode(m_mode);
            }
            ImGui::TextDisabled("Neural rendering runs at a reduced resolution, DLSS enlarges it back.");
            ImGui::TextDisabled("Works whatever the game's own DLSS is set to, including off.");

            // ---- V27: GREYED, NEVER HIDDEN ----
            //
            // These used to be inside "if (m_sr)", so with DLSS on GPU 1 off
            // the whole box vanished. People running it once on a title at
            // DLAA never learned the controls existed at all - they saw an
            // empty section and concluded there was nothing to set. Greying
            // shows what is there and why it is not doing anything, which is
            // the thing a hidden control can never say.
            ImGui::Spacing();
            if (m_sr)
            {
                ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1.0f),
                                   "DLSS on GPU 1 is ENABLED for this run.");
            }
            else
            {
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                                   "DLSS on GPU 1 is DISABLED. CHANGES BELOW ONLY APPLY IF ENABLED.");
            }

            ImGui::BeginDisabled(!m_sr);

            ImGui::TextUnformatted("preset");
            ImGui::SameLine();
            if (ImGui::RadioButton("title default##p", m_preset == 0))
            { m_preset = 0;  mgpu::gpu1::ui_ini_write("SRPreset", 0); }
            ImGui::SameLine();
            if (ImGui::RadioButton("K##p", m_preset == 11))
            { m_preset = 11; mgpu::gpu1::ui_ini_write("SRPreset", 11); }
            ImGui::SameLine();
            if (ImGui::RadioButton("L##p", m_preset == 12))
            { m_preset = 12; mgpu::gpu1::ui_ini_write("SRPreset", 12); }
            ImGui::SameLine();
            if (ImGui::RadioButton("M##p", m_preset == 13))
            { m_preset = 13; mgpu::gpu1::ui_ini_write("SRPreset", 13); }
            ImGui::TextDisabled("A DLL that lacks the preset asked for uses its own instead.");

            // ---- MATCH GAME, BEFORE THE MODE ROW ----
            //
            // It comes first because it decides whether the mode row means
            // anything, and the V12 block further down this file records what
            // happens otherwise: "SAY IT BEFORE THE BUTTONS, NOT AFTER... the
            // first person to use this panel clicked a preset, saw no change,
            // and reasonably concluded the control was broken."
            // Two named modes rather than a checkbox, because they are two
            // ways of choosing R and neither is an option on the other. Both
            // write SRScale and SRMvLowRes through write_mode, so the pair can
            // never be set independently - the combination that crashed at arm
            // on 2026-09-12 is unreachable from this panel.
            ImGui::TextUnformatted("upscaling");
            if (ImGui::RadioButton("Native Upscaling", m_match))
            { m_match = true;  write_mode(m_mode); }
            ImGui::TextDisabled("Upscales from the game's own render resolution. Higher quality.");

            if (ImGui::RadioButton("Experimental Upscaler", !m_match))
            { m_match = false; write_mode(m_mode); }
            ImGui::TextDisabled("Works from a downscaled resolution. More performance, possible");
            ImGui::TextDisabled("cost in quality. Untested.");

            // Greyed rather than hidden, for the same reason as the block
            // above: all three write SRScale, Native Upscaling overrides it,
            // and a row that disappears teaches nobody it was ever an option.
            ImGui::BeginDisabled(m_match);
            ImGui::TextUnformatted("mode  ");
            ImGui::SameLine();
            if (ImGui::RadioButton("quality##m", m_mode == 2))
            { m_mode = 2; write_mode(2); }
            ImGui::SameLine();
            if (ImGui::RadioButton("balanced##m", m_mode == 1))
            { m_mode = 1; write_mode(1); }
            ImGui::SameLine();
            if (ImGui::RadioButton("performance##m", m_mode == 0))
            { m_mode = 0; write_mode(0); }
            ImGui::TextDisabled("Sets the resolution neural rendering runs at, the way DLSS does:");
            ImGui::TextDisabled("quality 67%%, balanced 58%%, performance 50%% of the display.");
            ImGui::EndDisabled();
            if (m_match)
                ImGui::TextDisabled("Greyed: Native Upscaling is on, so the game chooses the resolution.");

            // V28: THE OLD NOTICE IS GONE. It said "if you see ghosting or
            // smearing in motion, turn this off", written when this section
            // had one toggle and "this" could only mean SR. With Native and
            // Experimental named above it, "this" no longer has one referent,
            // and both blurbs already carry the honest framing and the ask for
            // reports. A warning nobody can resolve to a control is noise.

            ImGui::EndDisabled();


            // ---- V26: THERE IS NO "DLAA MODE" ANY MORE, AND THAT IS THE FIX ----
            //
            // V24 removed the DLAA toggle on the grounds that SRScale=50 plus
            // SRMvLowRes=1 was the only panel setting that had ever produced
            // FAIL_PlatformError, and that the V42 guard fixing it had never
            // completed a run. BOTH HALVES OF THAT WERE STALE.
            //
            // The FAIL_PlatformError had one cause - the old guard forcing
            // MVLowRes on with no MV_Scale correction when the vectors were at
            // the display extent - and V42 fixed it. The guard is not unproven
            // either: it took the correct branch on Dawnwalker on 2026-09-13,
            // and a full 6000-frame run at SRScale=50 arrived at
            // "CreateFeature(SuperSampling) Success | R=1280x720 -> D=2560x1440,
            // MVLowRes=1, MVScale fix 0.5000/0.5000" with zero ghosting at
            // 42 fps on GPU 0 - which is the HARD case for ghosting, not the
            // easy one. Every DLAA attempt that failed before that died in
            // CreateFeature(Reserved18), which was the unrelated cubin crash.
            //
            // So DLAA does not come back as a toggle, because it was never a
            // mode. It was the state of having no R of our own. The mode
            // buttons above now choose R on every title, so the controls mean
            // the same thing whatever the game's DLSS is set to, and the thing
            // that used to need a second confusing switch is just the default.
            //
            // FORCE REFLEX stays out, and the reason is a one-way door. The
            // driver mode is PER DEVICE and survives the process, so a launch
            // that crashes leaves GPU 0 in low latency with nothing to undo
            // it - restore() only ever runs from stream_shutdown(), and all
            // three of its call sites are clean-exit paths. Worse still,
            // restore() is gated on `applied`, which is only set when
            // Reflex=1 engaged, so a user who ticks this, crashes, and then
            // UNTICKS IT to be safe has made the residue permanent. A toggle
            // whose off position cannot undo its on position is a trap, and
            // it is not shipping in a panel.
            //
            // The Reflex key still exists in mgpu.ini for our own testing. It
            // is simply not one click away for someone who has no idea what it
            // does. When the residue is self-healing it can come back.
            //
            // ---- ONE RULE, AND IT IS NEVER WRONG ----
            //
            // Everything left in this panel is read AT ARM, so arming is
            // technically enough for it. The panel still says RESTART, because
            // one rule that is sometimes stronger than needed beats two rules
            // where the reader has to work out which one applies. "Applied
            // when you arm" is not a sentence a non-native reader should have
            // to decode to find out whether their setting took.
            ImGui::Separator();
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                               "Saved automatically. RESTART THE GAME to use these settings.");
            ImGui::TextDisabled("The file is mgpu.ini, next to the game's exe.");

            ImGui::Spacing();
            if (ImGui::Button("  START NOW  ")) mgpu::gpu1::ui_request_arm();
            ImGui::SameLine();
            ImGui::TextDisabled("starts the bridge with the settings already saved");
        }
        else
        {
        // NOT a return above: this block sits inside BeginChild, and returning
        // from here would skip EndChild and leave ImGui's stack unbalanced for
        // the rest of the frame. An else costs one brace and cannot do that.
        ImGui::SeparatorText("Super resolution");
        if (!st.sr_requested)
        {
            ImGui::TextDisabled("Off. SRUpscale=1 in mgpu.ini turns it on.");
        }
        else if (!st.sr_on)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                               "Requested but not running - see [MGPU][C2-SR] in the log.");
            ImGui::TextDisabled("The usual reason is that the game already renders at display resolution.");
        }
        else
        {
            snprintf(dl, sizeof dl, "%ux%u  ->  %ux%u   (%.2f Mpx -> %.2f Mpx)",
                     st.sr_w, st.sr_h, st.out_w, st.out_h,
                     (double)st.sr_w * (double)st.sr_h / 1000000.0,
                     (double)st.out_w * (double)st.out_h / 1000000.0);
            ImGui::TextUnformatted(dl);

            static const char *qn[] = { "Max Perf", "Balanced", "Max Quality",
                                        "Ultra Perf", "Ultra Quality", "DLAA" };
            const int qi = (st.sr_quality >= 0 && st.sr_quality < 6) ? st.sr_quality : 1;
            snprintf(dl, sizeof dl, "quality %s   preset %s   R %s",
                     qn[qi],
                     (st.sr_preset == 0) ? "title default" : "forced",
                     (st.sr_scale_pct == 0) ? "inherited from the game"
                                            : "chosen by SRScale");
            ImGui::TextUnformatted(dl);

            snprintf(dl, sizeof dl, "snippet  %s%s",
                     st.sr_snippet_driver ? "DRIVER's copy" : "the game's own copy",
                     (st.sr_snippet_requested && !st.sr_snippet_driver)
                         ? "  (driver copy asked for but not found)" : "");
            ImGui::TextUnformatted(dl);
            if (!st.sr_snippet_driver)
                ImGui::TextDisabled("Preset availability is whatever that DLL carries - an old one lacks K.");

            snprintf(dl, sizeof dl, "motion vectors  mode %u, flag %s, MV scale x%.4f/%.4f",
                     st.sr_mv_mode, st.sr_mv_lowres ? "ON" : "off",
                     st.sr_mv_fix_x, st.sr_mv_fix_y);
            ImGui::TextUnformatted(dl);
            if (st.sr_mv_mode == 1)
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                                   "EXPERIMENTAL. May reduce ghosting - or cause it. Not confirmed on any other rig or game.");
            else if (st.sr_mv_mode == 2)
                ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f),
                                   "Mode 2 derives the flag WITHOUT correcting the scale - this is the combination that ghosted.");
        }

        // ---- V28: THE REFLEX READOUT IS GONE, AND IT IS NOT COMING BACK ----
        //
        // It printed "driver reported before X after Y" from GetSleepStatus,
        // and that call cannot see the TITLE'S OWN Reflex. A game with Reflex
        // enabled in its own settings produced a reading indistinguishable
        // from one where this add-on had set it - so the line was reporting a
        // fact it does not have access to.
        //
        // The panel already admitted the readout was unreliable in two
        // TextDisabled lines beneath it. A number with a disclaimer saying not
        // to believe it is worse than no number: it still anchors the reader,
        // and a bug report quoting it costs a session to unpick.
        //
        // Reflex itself is unchanged - the key still works, it is still 0.3.0,
        // and the GPU 0 queue figure above is the honest instrument. The
        // ordinals and the run C result are in ledger 6l.

        // ---- V12: THE CONTROLS. STAGE AND COMMIT. ----
        //
        // Quality and preset are baked into the NGX feature handle when it is
        // created, so neither can be a live slider: changing one means
        // releasing that handle and building a new one. So the radio buttons
        // stage a choice and APPLY commits it - the rebuild happens at the top
        // of the next poll on the bridge thread, which is the only moment GPU 1
        // has nothing outstanding.
        //
        // R DOES NOT CHANGE with either value, so nothing R-sized is rebuilt:
        // not the transport, not the ring, not the neural stage. That is
        // exactly why these two are safe to expose and why the DLAA lever
        // below is not.
        // ---- V31: THIS SECTION IS NEVER HIDDEN ----
        //
        // It used to be "if (st.sr_on)", so with super resolution off the
        // whole block vanished from the ARMED view - and that is the view
        // almost everyone sees, because AutoArm=1 arms the stream within
        // seconds of the game presenting. Someone opening the panel with Home
        // on a title where SR was off found no quality or preset controls at
        // all, and no way to learn they existed.
        ImGui::SeparatorText("Change quality or preset");
        if (!st.sr_on)
        {
            static const char *qn2[] = { "performance", "balanced", "quality" };
            const char *pn2 = (st.sr_preset == 11) ? "K"
                            : (st.sr_preset == 12) ? "L"
                            : (st.sr_preset == 13) ? "M" : "title default";
            ImGui::TextDisabled("quality: %s",
                                (st.sr_quality >= 0 && st.sr_quality <= 2)
                                    ? qn2[st.sr_quality] : "unknown");
            ImGui::TextDisabled("preset:  %s", pn2);
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                               "Super resolution is not running, so these cannot be changed now.");
            ImGui::TextDisabled("Set SRUpscale=1 in mgpu.ini and restart. The Super resolution");
            ImGui::TextDisabled("box above says whether it was off, or requested and refused.");
        }
        if (st.sr_on)
        {
            // SAY IT BEFORE THE BUTTONS, NOT AFTER. These radios STAGE a
            // choice; nothing happens until APPLY. The first person to use
            // this panel clicked a preset, saw no change, and reasonably
            // concluded the control was broken.
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                               "Pick, then press APPLY. The buttons alone change nothing.");

            static int  want_q = -1, want_p = -1;
            static bool init_done = false;
            if (!init_done) { want_q = st.sr_quality; want_p = st.sr_preset; init_done = true; }

            ImGui::TextUnformatted("Quality");
            ImGui::SameLine();
            if (ImGui::RadioButton("quality", want_q == 2))     want_q = 2;
            ImGui::SameLine();
            if (ImGui::RadioButton("balanced", want_q == 1))    want_q = 1;
            ImGui::SameLine();
            if (ImGui::RadioButton("performance", want_q == 0)) want_q = 0;
            // V18. THE MODE NOW SETS R, WHICH IS WHAT MAKES IT MEAN ANYTHING.
            // Without this the buttons changed only the model's internal mode
            // against an R inherited from the game, and looked broken because
            // nothing on screen moved. Same ladder the inner loop uses.
            ImGui::TextDisabled("Sets R as a share of the display, the way DLSS does:");
            ImGui::TextDisabled("quality 67%%, balanced 58%%, performance 50%%.");

            ImGui::TextUnformatted("Preset");
            ImGui::SameLine();
            if (ImGui::RadioButton("title default", want_p == 0)) want_p = 0;
            ImGui::SameLine();
            if (ImGui::RadioButton("K", want_p == 11)) want_p = 11;
            ImGui::SameLine();
            if (ImGui::RadioButton("L", want_p == 12)) want_p = 12;
            ImGui::SameLine();
            if (ImGui::RadioButton("M", want_p == 13)) want_p = 13;
            ImGui::TextDisabled("A DLL that lacks the preset asked for silently uses its own.");
            ImGui::TextDisabled("Its nvngx_dlss_*.log says which one it HONOURED. That is the answer.");

            const bool dirty = (want_q != st.sr_quality) || (want_p != st.sr_preset);
            ImGui::Separator();
            if (st.sr_rebuild_pending)
            {
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                                   "Applying on the next frame...");
            }
            else if (dirty)
            {
                // Loud, and immediately under the thing that staged it.
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "STAGED - NOT APPLIED YET");
                if (ImGui::Button("  APPLY  "))
                {
                    const int scale = (want_q == 2) ? 67 : ((want_q == 1) ? 58 : 50);
                    mgpu::gpu1::ui_set_sr_request(want_q, want_p, scale);
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("cancel")) { want_q = st.sr_quality; want_p = st.sr_preset; }
                ImGui::TextDisabled("One long frame, about 30-40 ms. Figures before and after");
                ImGui::TextDisabled("are different pipelines - do not pool them.");
            }
            else
            {
                ImGui::TextDisabled("No change staged - the panel matches what is running.");
            }

            if (st.sr_rebuild_count != 0)
            {
                snprintf(dl, sizeof dl, "%u rebuild%s this run, last one %s",
                         st.sr_rebuild_count, (st.sr_rebuild_count == 1) ? "" : "s",
                         (st.sr_rebuild_last_ok == 1) ? "OK" : "FAILED");
                if (st.sr_rebuild_last_ok == 1) ImGui::TextUnformatted(dl);
                else ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f), "%s", dl);
            }
        }

        // ---- V31: THE AUTO READOUT IS GONE, FOR THE REFLEX REASON ----
        //
        // It was a status block with no control, whose off state read "Auto=1
        // and AutoTargetFps in mgpu.ini". That is an instruction to switch on
        // a feature mgpu.ini itself marks "0.3.0, never run" - the live
        // rebuild path it depends on has never executed once.
        //
        // Telling a user how to enable something untested, in the panel, is
        // the same trap the Reflex readout was: it reads as a supported option
        // because it is in the interface. The keys stay in mgpu.ini for
        // testing. When Auto has actually run it comes back with a control
        // rather than a hint.

        // ---- Settings: writes the file, takes effect next launch ----
        //
        // THIS CANNOT BE LIVE, and pretending otherwise would be the worse
        // option. SRUpscale is read at arm, and by the time this view is on
        // screen the arm has already happened. So the panel edits mgpu.ini
        // and says plainly that the next launch is when it matters.
        //
        // V24: no longer headed "Experimental". The two experimental things
        // in it are gone and the only control left is the one the box above
        // reports on, so the old heading now warns about nothing.
        ImGui::SeparatorText("Settings - restart to take effect");
        {
            // V24. Reflex and DLAA were here too - this is the ARMED view's
            // copy of the same two controls the pre-arm menu had. Removing
            // them from one place and leaving them in the other would be the
            // worst of both: the trap still reachable, and now reachable only
            // from the screen a user gets to AFTER something has gone right,
            // which is the least likely place for anyone to look for it.
            // Both go. The reasoning is written out in full at the pre-arm
            // block above and is not repeated here.
            static bool ini_loaded = false;
            static bool want_sr = false;
            // V32: the rest of the settings, which were never in this box.
            // want_match is DERIVED from SRScale rather than stored in a key
            // of its own - SRScale=0 already means "inherit R from the game's
            // render extent" and has no second meaning, so a separate key
            // would be a second source of truth for one fact.
            static int  want_p = 0, want_m = 2;
            static bool want_match = true;
            if (!ini_loaded)
            {
                ini_loaded   = true;
                want_sr      = (mgpu::gpu1::ui_ini_read("SRUpscale", 0) != 0);
                want_p       = mgpu::gpu1::ui_ini_read("SRPreset", 0);
                want_m       = mgpu::gpu1::ui_ini_read("SRQuality", 2);
                want_match   = (mgpu::gpu1::ui_ini_read("SRScale", 0) == 0);
            }

            // SUPER RESOLUTION ITSELF, AND IT GOES FIRST BECAUSE IT GATES THE
            // REST. With the shipped mgpu.ini, SRUpscale is absent and
            // defaults to 0, so the whole box above read "Off - set it in
            // mgpu.ini" and there was no way to turn it on from here at all.
            // A panel that can only report a feature nobody can reach is not a
            // panel. Arm-time key, so it is a restart like the others.
            // Latched, not per-frame. The click lasts one frame and the
            // message has to outlive it or nobody ever sees it.
            static bool wrote_any = false;

            if (ImGui::Checkbox("DLSS Super Resolution on GPU 1", &want_sr))
            {
                wrote_any |= mgpu::gpu1::ui_ini_write("SRUpscale", want_sr ? 1 : 0);
            }
            ImGui::TextDisabled("Neural rendering runs at R, then DLSS enlarges it back to");
            ImGui::TextDisabled("the display. Everything else in this box needs it on.");

            // ---- V32: THE REST OF THE SETTINGS, IN THE SETTINGS BOX ----
            //
            // This box held ONE checkbox. preset, upscaling and mode existed
            // only in the pre-arm menu, which with AutoArm=1 is on screen for
            // a couple of seconds and which almost nobody reaches. So ticking
            // the box here appeared to reveal controls on the next launch,
            // when what it actually did was let SR create, which uncovered the
            // separate live "Change quality or preset" block further up.
            //
            // Everything that writes mgpu.ini now lives here, under the one
            // restart line at the bottom. Greyed rather than hidden when SR is
            // off, because a box that empties itself teaches nobody what it
            // holds.
            //
            // SRMvLowRes RIDES WITH SRScale in both branches. The flag without
            // the scale is what returned FAIL_PlatformError on 2026-09-12, and
            // writing them together is what makes that combination unreachable
            // from this panel.
            const auto write_sr_mode = [](int mode, bool match)
            {
                mgpu::gpu1::ui_ini_write("SRQuality", mode);
                if (match)
                {
                    mgpu::gpu1::ui_ini_write("SRScale",    0);
                    mgpu::gpu1::ui_ini_write("SRMvLowRes", 0);
                }
                else
                {
                    const int sc = (mode == 2) ? 67 : ((mode == 1) ? 58 : 50);
                    mgpu::gpu1::ui_ini_write("SRScale",    sc);
                    mgpu::gpu1::ui_ini_write("SRMvLowRes", 1);
                }
                return true;
            };

            ImGui::Spacing();
            ImGui::BeginDisabled(!want_sr);

            ImGui::TextUnformatted("preset");
            ImGui::SameLine();
            if (ImGui::RadioButton("title default##sp", want_p == 0))
            { want_p = 0;  wrote_any |= mgpu::gpu1::ui_ini_write("SRPreset", 0); }
            ImGui::SameLine();
            if (ImGui::RadioButton("K##sp", want_p == 11))
            { want_p = 11; wrote_any |= mgpu::gpu1::ui_ini_write("SRPreset", 11); }
            ImGui::SameLine();
            if (ImGui::RadioButton("L##sp", want_p == 12))
            { want_p = 12; wrote_any |= mgpu::gpu1::ui_ini_write("SRPreset", 12); }
            ImGui::SameLine();
            if (ImGui::RadioButton("M##sp", want_p == 13))
            { want_p = 13; wrote_any |= mgpu::gpu1::ui_ini_write("SRPreset", 13); }
            ImGui::TextDisabled("A DLL that lacks the preset asked for uses its own instead.");

            ImGui::TextUnformatted("upscaling");
            if (ImGui::RadioButton("Native Upscaling##su", want_match))
            { want_match = true;  wrote_any |= write_sr_mode(want_m, true); }
            ImGui::TextDisabled("Upscales from the game's own render resolution. Higher quality.");

            if (ImGui::RadioButton("Experimental Upscaler##su", !want_match))
            { want_match = false; wrote_any |= write_sr_mode(want_m, false); }
            ImGui::TextDisabled("Works from a downscaled resolution. More performance, possible");
            ImGui::TextDisabled("cost in quality. Untested.");

            ImGui::BeginDisabled(want_match);
            ImGui::TextUnformatted("mode  ");
            ImGui::SameLine();
            if (ImGui::RadioButton("quality##sm", want_m == 2))
            { want_m = 2; wrote_any |= write_sr_mode(2, want_match); }
            ImGui::SameLine();
            if (ImGui::RadioButton("balanced##sm", want_m == 1))
            { want_m = 1; wrote_any |= write_sr_mode(1, want_match); }
            ImGui::SameLine();
            if (ImGui::RadioButton("performance##sm", want_m == 0))
            { want_m = 0; wrote_any |= write_sr_mode(0, want_match); }
            ImGui::TextDisabled("Sets the resolution neural rendering runs at, the way DLSS does:");
            ImGui::TextDisabled("quality 67%%, balanced 58%%, performance 50%% of the display.");
            ImGui::EndDisabled();
            if (want_match)
                ImGui::TextDisabled("Native Upscaling is on.");

            ImGui::EndDisabled();

            ImGui::Spacing();
            if (wrote_any)
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                                   "mgpu.ini written. RESTART THE GAME - this session is unchanged.");
            else
                ImGui::TextDisabled("Changes here edit mgpu.ini. The running session never changes.");
        }
        }   // ---- end of the armed view ----
    }
    ImGui::EndChild();   // ---- end BOX 2 ----

    // ---- P9.1: the lateral acquisition probe ----
    //
    // Drawn on the GAME runtime as well as the bridge one, which is what makes
    // the toggle safe: register_event and unregister_event are called from the
    // render thread of the runtime being drawn, never from the bridge thread.
    ImGui::SeparatorText("Probe (P9.1) - acquisition lateral");
    {
        mgpu::probe::readout pr;
        mgpu::probe::read(pr);

        int pm = (int)pr.m;
        ImGui::TextUnformatted("Mode");
        ImGui::SameLine();
        bool changed = false;
        changed |= ImGui::RadioButton("off",   &pm, 0); ImGui::SameLine();
        changed |= ImGui::RadioButton("depth", &pm, 1); ImGui::SameLine();
        changed |= ImGui::RadioButton("mvec",  &pm, 2); ImGui::SameLine();
        changed |= ImGui::RadioButton("both",  &pm, 3);
        if (changed) mgpu::probe::set_mode((mgpu::probe::mode)pm);

        if (pm != 0)
        {
            char pb[320];
            snprintf(pb, sizeof pb,
                     "game device %s | %llu resources examined | %llu frames",
                     pr.game_device_found ? "found" : "NOT FOUND",
                     pr.examined, pr.frames);
            ImGui::TextUnformatted(pb);

            snprintf(pb, sizeof pb, "cost: depth %.0f ns/frame, mvec %.0f ns/frame",
                     pr.depth_ns_per_frame, pr.mvec_ns_per_frame);
            ImGui::TextUnformatted(pb);

            for (unsigned i = 0; i < pr.depth_n && i < 4; ++i)
            {
                snprintf(pb, sizeof pb, "depth #%u  %ux%u  binds=%llu clears=%llu", i,
                         pr.depth_top[i].width, pr.depth_top[i].height,
                         pr.depth_top[i].binds, pr.depth_top[i].clears);
                ImGui::TextUnformatted(pb);
            }
            for (unsigned i = 0; i < pr.mvec_n && i < 4; ++i)
            {
                snprintf(pb, sizeof pb, "mvec  #%u  %ux%u  srvs=%llu", i,
                         pr.mvec_top[i].width, pr.mvec_top[i].height,
                         pr.mvec_top[i].binds);
                ImGui::TextUnformatted(pb);
            }
            ImGui::TextDisabled("Watching only. Nothing is bound, nothing crosses the bus.");
        }
    }

    ImGui::Separator();
    ImGui::TextDisabled("Do not change resolution, DLSS mode or presets while armed - disarm first.");
    ImGui::TextDisabled("Frame generation is untested. Changes here make this a tuning run.");

    // ---- V20: WHERE TO SEND A LOG ----
    //
    // NOT PROMOTION - THE PROJECT NEEDS OTHER PEOPLE'S LOGS. Two findings are
    // explicitly blocked on "needs more machines": an intermittent crash at
    // arm that has never reproduced on demand, and whether the corrected
    // motion-vector flag ghosts anywhere other than the one rig it was judged
    // on. A user who crashes and sees nothing has no idea a repo exists. A
    // user who crashes and sees this line becomes a data point.
    //
    // Plain copyable text on purpose. Nothing here opens a browser from inside
    // somebody's game, and the path to the log is spelled out because the
    // person reading it has just had something go wrong and should not have to
    // go hunting.
    ImGui::Spacing();
    ImGui::Separator();

    // ---- V25: THE INSTALL LAYOUT, AND IT SITS WITH THE BUG-REPORT LINE ----
    //
    // These two belong together. One says where to send a log when something
    // breaks; this one says what is most likely to be breaking. A file in the
    // wrong folder is not a cosmetic problem here - it is THE crash, proven
    // 2026-09-13: with nvngx_dlssnr.dll beside the executable, the title's own
    // Streamline loads it and binds NGX to the GAME'S GPU a second and a half
    // before this add-on's thread exists, and the neural stage then faults
    // inside NVIDIA's allocator and takes the process down.
    //
    // Cyberpunk never wanted that DLL. Our own 0.1.0 install instructions put
    // it there, so anyone upgrading is in the broken state by default and has
    // no way to know it. That is exactly the case a panel exists to catch.
    //
    // Coloured rather than TextDisabled when wrong: this is the one line in
    // this panel worth interrupting someone for.
    {
        const int layout = mgpu::gpu1::ui_install_layout();
        if (layout == 1)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.30f, 1.0f),
                               "INSTALL PROBLEM - the neural stage will crash this game.");
            ImGui::TextUnformatted("nvngx_dlssnr.dll is next to the game's exe. The game loads it");
            ImGui::TextUnformatted("there and claims the wrong GPU before this add-on starts.");
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                               "MOVE it into the mgpu folder next to the add-on, then restart.");
        }
        else if (layout == 2)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                               "nvngx_dlssnr.dll not found - neural rendering cannot start.");
            ImGui::TextUnformatted("Put it in the mgpu folder next to the add-on, then restart.");
        }
        else
        {
            ImGui::TextDisabled("Install layout OK - nvngx_dlssnr.dll is in its own folder.");
        }
        ImGui::Spacing();
    }

    ImGui::TextDisabled("Something wrong? Send ReShade.log from the game's exe folder to:");
    ImGui::TextDisabled("github.com/maohgad-web/Neural-coprocessor");
}
#endif

// P1.5: the only event this add-on subscribes to that is raised on the GAME's
// render thread with the GAME's command list open. Everything it does is
// one-shot and self-disarming; after a single frame is captured it never
// touches that list again.
//
// The handle conversions live here rather than in gpu1_context.cpp so that
// file keeps its rule of holding no ReShade types - it takes the two native
// pointers and nothing else. get_native() returns uint64_t, not a pointer,
// so these are reinterpret_cast and not static_cast (P0_RECORD section 09).
// L3. Fired after ReShade has submitted the effects list for this frame and
// before the swapchain presents it - which is exactly the ordering the
// next-frame signal in stream_on_finish_effects was working around.
static void on_present(reshade::api::command_queue *queue,
                       reshade::api::swapchain *,
                       const reshade::api::rect *, const reshade::api::rect *,
                       uint32_t, const reshade::api::rect *)
{
    if (queue == nullptr) return;
    mgpu::gpu1::stream_on_present(
        reinterpret_cast<void *>(static_cast<uintptr_t>(queue->get_native())));
}

static void on_reshade_finish_effects(reshade::api::effect_runtime *runtime,
                                      reshade::api::command_list *cmd_list,
                                      reshade::api::resource_view rtv,
                                      reshade::api::resource_view rtv_srgb)
{
    (void)rtv_srgb;
    if (runtime == nullptr || cmd_list == nullptr) return;

    reshade::api::device *dev = runtime->get_device();

    // P12.2/R63. Hoisted, because the depth barrier below is only legal on the
    // GAME runtime's command list while the stream call sits outside the block
    // that knows which runtime this is. Issuing a transition for a GPU 0
    // resource on the bridge runtime's GPU 1 list is not a subtle error.
    bool rt_is_game = false;
    if (dev == nullptr) return;

    // P1.6. Which runtime is this? The same LUID comparison on_destroy_device
    // already makes. Log-only and one-shot per side; nothing below changes
    // behaviour, and the adapter filtering that actually gates the capture and
    // stream paths still happens inside gpu1_context where the game's LUID
    // lives. Placed here rather than at stream arm on purpose: the preset is
    // painting frames from the moment the runtime exists, which is long before
    // anything is armed, so the record has to start there too.
    {
        static preset_probe game_probe, bridge_probe;
        mgpu::adapter::selection_result sel;
        mgpu::adapter::get_selection(sel);
        if (sel.game_luid_known && dev->get_api() == reshade::api::device_api::d3d12)
        {
            if (auto *dev12 = reinterpret_cast<ID3D12Device *>(dev->get_native()))
            {
                const LUID luid = dev12->GetAdapterLuid();
                const bool is_game = (luid.LowPart == sel.game_luid.LowPart &&
                                      luid.HighPart == sel.game_luid.HighPart);
                log_preset_once(runtime, is_game ? "GAME" : "BRIDGE",
                                is_game ? game_probe : bridge_probe);
                // P9.1. The probe's frame tick and its periodic dump. Placed
                // here because this is the one spot that already knows which
                // runtime it is on, and the probe counts the GAME's frames -
                // the bridge runtime presents on its own schedule and counting
                // both would make the ns/frame figure meaningless.
                // P11.0 (R48). note_effects FIRST: it only stores the
                // runtime pointer, and dump() - which note_frame may call on
                // this very line - is what enumerates through it. Reversed,
                // the first dump of the process would have no runtime and the
                // semantic lane would report NOT RUN for one window.
                if (is_game)
                {
                    rt_is_game = true;
                    mgpu::probe::note_effects(runtime, cmd_list);
                    mgpu::probe::note_frame();
                    // R101. Same tick, same runtime, same reason: the tap's
                    // ns/frame figure has to be against the GAME's frames.
                    mgpu::calibrator::note_frame();

                    // ---- R103: HAND THE TRANSPORT THE GAME'S OWN HANDLE ----
                    //
                    // Only when the calibrator actually has one. read()
                    // returns false until something has been captured, and
                    // KEY_MVEC is only set when the Get succeeded, so a title
                    // where the calibrator never resolved falls through to the
                    // barrier probe's pick with no branch of its own.
                    //
                    // Calib=0 is the off switch: no calibrator, no table, no
                    // override, and this build behaves exactly like R99.
                    {
                        mgpu::calibrator::table ct;
                        if (mgpu::calibrator::read(ct) &&
                            (ct.have & mgpu::calibrator::KEY_MVEC) != 0u)
                            mgpu::probe::set_mvec_override(ct.mvec);
                    }
                }
            }
        }
    }

    // The resource behind the view, not the view: the copy source has to be
    // the texture. Adapter filtering happens inside gpu1_context, which is
    // where the game's LUID already lives.
    const reshade::api::resource res = dev->get_resource_from_view(rtv);
    if (res.handle == 0) return;

    // P2.0: the game's immediate queue. effect_runtime declares
    // `virtual command_queue *get_command_queue() = 0;` - it is the queue
    // ReShade itself submits the list above on, which is exactly the queue our
    // copies will be executed on, and therefore the only queue a signal placed
    // behind them can be ordered against. A null queue is not fatal: the
    // capture path falls back to the P1.5 frame counter and says so.
    reshade::api::command_queue *q = runtime->get_command_queue();
    void *q_native = (q != nullptr)
                       ? reinterpret_cast<void *>(static_cast<uintptr_t>(q->get_native()))
                       : nullptr;

    mgpu::gpu1::capture_on_finish_effects(
        reinterpret_cast<void *>(runtime),
        reinterpret_cast<void *>(static_cast<uintptr_t>(cmd_list->get_native())),
        q_native,
        static_cast<unsigned long long>(res.handle));

    // P4.0 rides the same event. Both paths are inert until requested and are
    // independent by construction, so neither can leave the other half-armed.
    //
    // ---- R63: THE DEPTH TRANSITION IS ISSUED HERE, NOT IN gpu1_context ----
    //
    // Two reasons, and the second is the real one.
    //
    // 1. gpu1_context holds no ReShade types, by a design rule older than this
    //    milestone, and ReShade's barrier is a ReShade type.
    // 2. R56 proved that RESHADE'S OWN MAPPING of shader_resource is the
    //    correct transition for a resource ReShade owns - 1080 copies, no
    //    device loss, on a rig with no debug layer to catch a wrong one.
    //    P10.3 already spent a device guessing a raw D3D12 state for a
    //    resource this project did not create. This does not guess.
    //
    // The colour copy inside stream_on_finish_effects transitions the game's
    // own back buffer and stays raw D3D12. The two are different cases and are
    // deliberately not written the same way.
    //
    // Barrier, record, barrier back - all on one list, in order. A handle of 0
    // means ReShade has no depth this frame, and then nothing at all is issued.
    unsigned long long depth_h = rt_is_game ? mgpu::probe::depth_source() : 0ull;
    const reshade::api::resource depth_res = { depth_h };

    if (depth_h != 0)
        cmd_list->barrier(depth_res, reshade::api::resource_usage::shader_resource,
                                     reshade::api::resource_usage::copy_source);

    // ---- R78: the velocity buffer, for the ARM ONLY ----
    //
    // No barrier and no copy here, and that is the difference from depth. By
    // the time this event fires the velocity target has been bound as a render
    // target again and holds whatever the engine last drew into it - the frame
    // it described is gone. Its per-frame copy is issued mid-frame from
    // mgpu_mvec_transport_hook above; all this handle does is let the arm size
    // the slot's MVec region from the real resource. Zero until the probe has
    // published one, which holds the arm - see stream_on_finish_effects.
    const unsigned long long mvec_h = rt_is_game ? mgpu::probe::mvec_source() : 0ull;

    mgpu::gpu1::stream_on_finish_effects(
        reinterpret_cast<void *>(static_cast<uintptr_t>(cmd_list->get_native())),
        q_native,
        static_cast<unsigned long long>(res.handle),
        depth_h,
        mvec_h);

    if (depth_h != 0)
        cmd_list->barrier(depth_res, reshade::api::resource_usage::copy_source,
                                     reshade::api::resource_usage::shader_resource);
}

// T3 instrumentation: in a clean run, no destroy_device with the game's
// LUID appears while the game is running (acceptance). Every line carries
// the device's LUID, so a removal names whose device it was. When the
// destroyed device is the game's, ReShade uninit is imminent: signal the
// bridge thread to begin its orderly teardown (signal only - see
// worker::stop for why nothing here waits).
static void on_destroy_device(reshade::api::device *device)
{
    mgpu::adapter::log_device_luid("destroy_device", device);

    mgpu::adapter::selection_result sel;
    mgpu::adapter::get_selection(sel);
    if (sel.game_luid_known && device != nullptr &&
        device->get_api() == reshade::api::device_api::d3d12)
    {
        if (auto *dev12 = reinterpret_cast<ID3D12Device *>(device->get_native()))
        {
            const LUID luid = dev12->GetAdapterLuid();
            if (luid.LowPart == sel.game_luid.LowPart &&
                luid.HighPart == sel.game_luid.HighPart)
            {
                mgpu::diag::info("[MGPU][T3] game device released - signaling bridge thread "
                                 "teardown (final log lines are best effort)");

                // ---- V17: CLOSE THE NGX SESSION HERE TOO ----
                //
                // stream_shutdown() already runs in the bridge thread's
                // ordered teardown. That teardown only happens if the bridge
                // loop unwinds cleanly, and NOT closing the NGX session is the
                // fault that poisons the NEXT LAUNCH of the game - five clean
                // launches followed by one crash is what a cleanup that
                // usually runs looks like.
                //
                // This event is the earliest and most reliable notice we get
                // that the game's device is going. Calling here means the
                // session is closed on paths where the bridge thread never
                // gets to its teardown at all.
                //
                // SAFE TO CALL TWICE: stream_shutdown clears the pointers it
                // uses and returns early when there is nothing left, so the
                // ordered teardown finding it already done is the normal case
                // rather than an error.
                mgpu::gpu1::stream_shutdown();

                mgpu::worker::stop();
            }
        }
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD fdwReason, LPVOID lpReserved)
{
    switch (fdwReason)
    {
    case DLL_PROCESS_ATTACH:
        // T4 (exception 1): capture our own HMODULE before anything else.
        // Set unconditionally (even if registration is refused below) so
        // the accessor is never null in a loaded module.
        g_module = hModule;
        if (!reshade::register_addon(hModule))
        {
            // ReShade absent (e.g. an add-on-disabled build) or add-on
            // API version mismatch. No log path is guaranteed here - we
            // were refused - and returning FALSE from DllMain would
            // terminate the game process. One DebugView line, then load:
            //   [MGPU][T1] missing + this line present -> registration refused
            //   [MGPU][T1] missing + this line absent  -> add-on file not found
            // (see README, "Reading the log")
            OutputDebugStringA("[MGPU][T1] register_addon failed");
            break;
        }
        reshade::log::message(reshade::log::level::info,
            "[MGPU][T1] MGPU Bridge add-on registered - stock ReShade, add-on API version "
            MGPU_STR(RESHADE_API_VERSION));
        // T2: per-event logging, provisional LUID capture, one-time table
        // enumeration; the selection itself is deferred to the
        // swapchain-derived game LUID.
        reshade::register_event<reshade::addon_event::init_device>(on_init_device);
        reshade::register_event<reshade::addon_event::init_swapchain>(on_init_swapchain);
        // T3: device lifecycle instrumentation + teardown trigger.
        reshade::register_event<reshade::addon_event::destroy_device>(on_destroy_device);
        // P1.5: capture one real frame. Registered last because it is the only
        // subscription that acts on the game's own command list.
        reshade::register_event<reshade::addon_event::reshade_finish_effects>(
            on_reshade_finish_effects);
        // L3: see stream_on_present. Registered unconditionally; the add-on
        // side decides whether to act on it, so the ini can turn the behaviour
        // on and off without a rebuild.
        reshade::register_event<reshade::addon_event::present>(on_present);
        // P9.1. NOT initialised here. The probe reads mgpu.ini, and file I/O
        // inside DllMain runs under the loader lock, where the CRT is entitled
        // to load a locale DLL and deadlock against the lock we are already
        // holding. It is initialised from on_init_swapchain instead, which is
        // on the game's render thread with the loader lock long released - the
        // same thread the probe's own register_event calls belong on anyway.
#if defined(MGPU_HAVE_IMGUI)
        reshade::register_overlay("MGPU Bridge", draw_mgpu_overlay);
        reshade::log::message(reshade::log::level::info,
            "[MGPU][P6.4] overlay panel registered - open the ReShade overlay (Home) over the "
            "BRIDGE window to get passes, the neural on/off and the intensity sliders. The "
            "CTRL+ALT+F8/F9/F11 hotkeys still work and drive the same values.");
#else
        reshade::log::message(reshade::log::level::warning,
            "[MGPU][P7.1] NO OVERLAY PANEL IN THIS BUILD - imgui.h was not on the include path "
            "when this compiled, and reshade.hpp only wires up the ImGui function table when "
            "IMGUI_VERSION_NUM is defined ahead of it. Nothing else is affected: use the "
            "CTRL+ALT+F8 / F9 / F11 hotkeys, which drive exactly the same values. To get the "
            "panel, put ImGui 1.92.5's imgui.h and imconfig.h (tag 3912b3d, the commit ReShade "
            "18deaa52 pins at deps/imgui) somewhere on the include path. A target_include_"
            "directories line pointing at a directory that does not exist is silently ignored "
            "by CMake and __has_include then just says no, which is how this warning survives "
            "an edit to CMakeLists that looked correct.");
#endif
        break;

    case DLL_PROCESS_DETACH:
        // Both unload paths only signal the stop event and return.
        // DllMain runs under the loader lock, and a thread cannot finish
        // exiting without that lock (its exit dispatches
        // DLL_THREAD_DETACH to every loaded module): a join here would
        // deadlock, and a timed-out join would leave a live thread
        // pointing into a DLL that is about to unmap. The mutex is never
        // held across a wait (see worker::stop).
        //
        //   lpReserved != NULL: process exit. Windows terminated every
        //   other thread before this call; the bridge thread is dead and
        //   the OS reclaims its GPU objects. The signal is ceremonial.
        //   lpReserved == NULL: dynamic FreeLibrary (ReShade unloading
        //   us). The bridge thread is live; it tears down the GPU 1
        //   objects itself once it wakes (T4: window, class, device,
        //   adapter - in that order, on its own thread). API 20 has no
        //   reshade_unload event to drive the teardown from earlier, so if
        //   the FreeLibrary happens first the GPU 1 device is simply leaked
        //   - explicitly in scope at P0. A hang is not.
        // ---- V21: CLOSE THE NGX SESSION HERE TOO. LAST RESORT AND IT IS NEEDED. ----
        //
        // Measured 2026-09-13 on a CLEAN exit: ReShade goes
        //     Destroyed runtime environment -> Unregistered add-on -> Exiting
        // and NEVER RAISES destroy_device. So both of the other call sites -
        // on_destroy_device and the bridge thread's ordered teardown - are
        // hooked to events that did not happen, the NGX session stayed open,
        // and the NEXT launch of the game paid for it. That is the "open it
        // twice and it fixes itself" symptom.
        //
        // ONLY ON THE FreeLibrary PATH. lpReserved == NULL means ReShade is
        // unloading us while the process lives, which is when the device is
        // still valid and there is something to close. lpReserved != NULL is
        // process termination: every other thread is already dead, and calling
        // into a vendor DLL there is how DllMain rules get broken.
        //
        // Still under the loader lock, which is why this is the LAST of three
        // call sites rather than the first, and why stream_shutdown announces
        // every vendor call before making it. If the process dies here, the
        // log names which one.
        if (lpReserved == nullptr)
        {
            mgpu::diag::info("[MGPU][T3] FreeLibrary unload - closing the NGX session from "
                             "DllMain, because ReShade does not always raise destroy_device "
                             "and an unclosed session poisons the next launch.");
            mgpu::gpu1::stream_shutdown();
        }
#if defined(MGPU_HAVE_IMGUI)
        reshade::unregister_overlay("MGPU Bridge", draw_mgpu_overlay);
#endif
        // P9.1 before worker::stop: unregistering our events while the game
        // thread may still raise them is the one ordering that matters here,
        // and DllMain is single-threaded with respect to ReShade's dispatch.
        mgpu::probe::shutdown();
        mgpu::worker::stop();
        // R101c: PUT THE IMPORT SLOTS BACK BEFORE THIS DLL GOES AWAY.
        // ~150 modules hold pointers into this image. Unloading without
        // restoring them leaves every one of those slots aimed at unmapped
        // memory, and the next GetProcAddress anywhere in the process walks
        // into it. This is what stopped Dragon Sword launching.
        mgpu::calibrator::uninstall();
        reshade::unregister_addon(hModule);
        break;
    }
    return TRUE;
}