#pragma once

#include <d3d12.h>

// Add-on side loader for b1_imgui.dll. This module deliberately contains no
// ImGui: the private ImGui lives in its own DLL (see B1 plan §3.1).
namespace mgpu::b1_loader
{
    bool init(ID3D12Device *dev, ID3D12CommandQueue *queue, DXGI_FORMAT rtv_format,
              UINT width, UINT height, bool enable);
    void shutdown();
    bool on();
    // B1.2 (2026-11-07): PanelInPresenter=1 AND the private DLL actually loaded. A missing
    // b1_imgui.dll must fall back to the shipped panel path - not to a screen with no panel
    // at all - so the key is only trusted together with the module.
    bool panel_owns_presenter();
    void set_visible(bool visible);
    void new_frame(float dt, float mouse_x, float mouse_y, bool mouse_down, float wheel);
    void render(ID3D12GraphicsCommandList *cl, D3D12_CPU_DESCRIPTOR_HANDLE rtv);
}
