// MGPU Bridge — adapter enumeration and selection (T2)
//
// Selection rule: the game's own adapter LUID is captured FIRST, from the
// game's D3D12 device (the first init_device after add-on load), and the
// bridge adapter is the one whose LUID differs from the game's — select
// by exclusion. Active output counts are logged for every adapter and
// used only as a tie-break when more than two adapters are present (the
// target topology may put a display on either card, so "headless" is
// never the primary discriminator). A degenerate selection is logged
// LOUDLY, never silently: a silent bind back to the game's own adapter
// would make everything downstream "work" and prove nothing.
//
// The binding always keys on the LUID, never on the enumeration index
// (index order is not stable across driver restarts).
#pragma once

#include <windows.h>

// Forward declaration keeps ReShade's headers out of this interface;
// only adapter.cpp needs them (for the game-device LUID capture).
namespace reshade { namespace api { struct device; } }

namespace mgpu::adapter
{
    struct selection_result
    {
        bool valid = false;               // an adapter was selected
        bool degenerate = false;          // needed a tie-break/guess; loud warning already logged
        bool game_luid_known = false;
        LUID game_luid{};
        LUID selected_luid{};
        UINT selected_index = 0;          // log only — never used for binding
        UINT selected_outputs = 0;
        char selected_desc[128]{};
        const char *rule = "none";
        // AddRef'd IDXGIAdapter1 for T3's D3D12CreateDevice; released by
        // shutdown(). nullptr when no adapter was selected.
        void *selected_adapter = nullptr;
    };

    // Game thread (first init_device / init_swapchain). One-shot (atomic
    // guard): captures the game LUID when the trigger carries the game's
    // d3d12 device, enumerates every adapter, selects. Returns true when
    // THIS call performed the enumeration.
    bool run_once(::reshade::api::device *game_device, const char *trigger);

    // Releases the selected adapter reference. Bridge thread, at shutdown.
    void shutdown();

    // Worker thread. ready_event() is valid from before the bridge thread
    // is spawned (created on the game thread first).
    HANDLE ready_event();
    void   get_selection(selection_result &out);

    // T3 instrumentation: log the adapter LUID of any device event in the
    // process (init_device / device_removed / device_restored) — including
    // our own T3 device, which independently confirms the binding.
    void log_device_luid(const char *event, ::reshade::api::device *device);
}
