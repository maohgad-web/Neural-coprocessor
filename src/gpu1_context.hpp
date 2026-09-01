// MGPU Bridge — the private D3D12 device on the selected adapter (T3)
//
// Bridge thread only: every GPU 1 object is created, used and destroyed
// on the bridge thread. T3 does exactly one thing — create the device
// against the T2 selection, re-verify the binding LUID, log. The window
// and swapchain arrive with T4/T5.
#pragma once

#include <windows.h>

#include "adapter.hpp"

namespace mgpu::gpu1
{
    // Bridge thread only. Returns false when no device was created
    // (invalid selection, creation failure, or LUID mismatch). Every
    // failure is already logged with its inputs; there is never a
    // fallback to the game's adapter.
    bool create_device(const adapter::selection_result &sel);

    // Bridge thread only. Releases the device if one exists.
    void shutdown();

    bool has_device();
}
