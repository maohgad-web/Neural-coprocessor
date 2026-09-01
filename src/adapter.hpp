// MGPU Bridge - adapter enumeration and selection (T2)
//
// Selection pipeline (brief section 06). The rules are ordered so a
// partial application is always safe - the swapchain-derived game LUID
// gate is evaluated before any filter, and the refuse-rather-than-guess
// gate last, so an incomplete set of filters can never produce a wrong
// selection:
//
//   1. The game LUID must come from the swapchain. UE5's probe devices
//      mean the first init_device is not reliably the game's renderer
//      (one run captured a software adapter as the game LUID). init_device
//      is captured only as a *provisional* value; the LUID of the device
//      that owns the swapchain (the device CreateSwapChainForHwnd was
//      called on) overrides it and is the only value a selection may be
//      based on. No swapchain-derived game LUID -> no selection.
//   2. Software adapters (DXGI_ADAPTER_FLAG_SOFTWARE in
//      DXGI_ADAPTER_DESC1::Flags) are still enumerated and logged, but
//      cannot be selected. Flags (hex), VendorId (Microsoft is 0x1414)
//      and DedicatedVideoMemory are logged so the rig can confirm which
//      discriminator actually separates them.
//   3. Refuse rather than guess. If the filter does not leave *exactly
//      one* candidate, select nothing and log loudly - a missing device
//      is diagnosable; a device on the wrong adapter succeeds, logs
//      cleanly, and proves nothing. This replaces the old
//      last-in-enum-order fallback.
//   4. Output count is never the primary discriminator - this project's
//      topology puts the display on the target card, and any rig with
//      displays on both cards breaks an output-count rule silently.
//      Output counts are logged for every adapter and used only as a
//      tie-break when more than two hardware adapters are present.
//
// Diagnostic only, must not influence the selection: per-adapter
// IDXGIAdapter3::QueryVideoMemoryInfo (DXGI_MEMORY_SEGMENT_GROUP_LOCAL) -
// this process's VRAM usage reads multi-GB on the game's adapter and
// near-zero on every other one.
//
// The binding always keys on the LUID, never on the enumeration index
// (index order is not stable across driver restarts, and the LUIDs
// themselves are reassigned across sessions).
#pragma once

#include <windows.h>

// Forward declarations keep ReShade's headers out of this interface;
// only adapter.cpp needs them.
namespace reshade { namespace api { struct device; struct swapchain; } }

namespace mgpu::adapter
{
    struct selection_result
    {
        bool valid = false;               // an adapter was selected
        bool degenerate = false;          // a tie-break was needed; confirm the binding manually
        bool game_luid_known = false;     // known from any source (provisional or swapchain)
        bool game_luid_from_swapchain = false;   // the only source that authorises a selection
        LUID game_luid{};
        LUID selected_luid{};
        UINT selected_index = 0;          // log only - never used for binding
        UINT selected_outputs = 0;
        char selected_desc[128]{};
        const char *rule = "none";
        // AddRef'd IDXGIAdapter1 for T3's D3D12CreateDevice; released by
        // shutdown(). nullptr when no adapter was selected.
        void *selected_adapter = nullptr;
    };

    // Game thread (init_device callback). Logs the device's LUID and
    // captures the game LUID as a *provisional* value when none is known
    // yet. The first device event also enumerates the full adapter table
    // (with the VRAM diagnostics), so the table exists in the log even if
    // no swapchain ever arrives. The selection itself is deferred - it
    // may only proceed once a swapchain-derived game LUID exists (see
    // on_swapchain).
    void on_device(::reshade::api::device *device);

    // Game thread (init_swapchain callback). The device that owns the
    // swapchain is the authoritative game render device; its LUID
    // overrides any provisional value and arms the one-shot selection.
    // Subsequent swapchain events (resizes) are logged, never re-select.
    void on_swapchain(::reshade::api::swapchain *swapchain, bool resize);

    // Releases the selected adapter reference. Bridge thread, at shutdown.
    void shutdown();

    // Worker thread. ready_event() is valid from before the bridge thread
    // is spawned (created on the game thread first). The event is set
    // only when the selection is *decided* - an adapter selected, or a
    // terminal refusal logged. A deferral (awaiting the swapchain-derived
    // game LUID) is not a decision.
    HANDLE ready_event();
    void   get_selection(selection_result &out);

    // T3 instrumentation: log the adapter LUID of any device event in the
    // process (init_device / destroy_device) - including our own T3
    // device, which independently confirms the binding.
    void log_device_luid(const char *event, ::reshade::api::device *device);
}
