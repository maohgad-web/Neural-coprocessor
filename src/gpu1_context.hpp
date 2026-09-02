// MGPU Bridge - the private D3D12 device on the selected adapter (T3;
// the device-removal poll accessor arrives with T4)
//
// Bridge thread only: every GPU 1 object is created, used and destroyed
// on the bridge thread. T3 does exactly one thing - create the device
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

    // T4 (brief section 00, exception 4): poll the device's removal reason
    // without exposing the ID3D12Device. The raw pointer never leaves this
    // translation unit - handing it out would put it outside the mutex that
    // guards it, and shutdown() could release it between the caller's read
    // and its use. Returns false when no device exists (nothing to poll;
    // `out` is set to S_OK). Returns true and sets `out` to
    // ID3D12Device::GetDeviceRemovedReason() (S_OK when healthy, otherwise
    // the removal reason) when a device exists. Takes this file's lock, so
    // the call is made with the device pointer still guarded. The value is
    // sticky once removed (brief section 09), so the caller logs only on the
    // transition away from S_OK.
    bool device_removed_reason(HRESULT &out);
}
