#pragma once
// MGPU Bridge - [FFX, 2026-11-11] the FSR contract from AMD's own FFX API entry.
//
// WHAT THIS IS. An inline hook on `ffxDispatch` in the FidelityFX 3.1 API module
// (`amd_fidelityfx_loader_dx12.dll`, or `ffx_api.dll` when a title ships that name). It reads
// the `ffxDispatchDescUpscale` block the game hands FSR and publishes the same contract the
// NGX hook publishes: colour / depth / motion-vector resources, render and display extents,
// jitter, motion-vector scale, reset, and the create flags. The consumer side does not know
// which API filled the table.
//
// WHY IT EXISTS. NGX is the contract for a DLSS title. A title whose upscaler is FSR never
// calls NVSDK_NGX_* - it hands depth and vectors to AMD's API - so on those titles the bridge
// had no depth/vector source at all and the neural stage fell back to the tap. This is the
// same read at the other API's own entry: nothing is written, one block is copied, and the
// two images go into the same transport the NGX route uses.
//
// WHAT IT DOES NOT DO. It does not choose a source, latch a scene handle, or touch frame
// generation's own dispatches. Only `FFX_API_DISPATCH_DESC_TYPE_UPSCALE` (0x00010001) is read;
// every other dispatch type is passed through untouched.
//
// SELF-PROOF. The dispatch descriptor is a versioned C struct read at fixed offsets, so the
// first upscale dispatch checks what the offsets claim against `ID3D12Resource::GetDesc()`
// (declared width/height/format vs the resource's own). A mismatch marks the layout bad and
// the hook refuses every copy for the session - a wrong offset must cost a frame of telemetry,
// never a copy from the wrong resource.
namespace mgpu
{
namespace ffxhook
{
// Idempotent and cheap to call repeatedly: returns true once the hook is live. `want` is the
// FfxHook key; 0 leaves the process untouched.
bool install(int want);
bool installed();
bool wanted();
}   // namespace ffxhook
}   // namespace mgpu
