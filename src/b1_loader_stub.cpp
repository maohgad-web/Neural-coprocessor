// MGPU Bridge - b1_loader, fail-closed stub for the 0.5.1 presenter port.
//
// WHAT THIS REPLACES. nc-base's b1_loader.cpp loads b1_imgui.dll and registers the
// panel body with it (b1_panel.hpp + panel_body_b1.hpp + the 96 KB panel_body.inc).
// That whole chain is a separate feature - the private ImGui panel - and it is NOT
// part of this port. The presenter only ever asks it three things:
//
//   on()                     - is the private panel loaded?
//   panel_owns_presenter()   - Key + module, per B1.2.
//   init / new_frame / render / set_visible
//
// With the DLL absent, the real b1_loader answers false to the first two and does
// nothing in the rest. This stub is that state, written down: the presenter then
// runs the shipped panel path (ReShade's own ImGui), which is what 0.5.1 uses today.
//
// THIS IS A CUT, NOT A REWRITE. presenter.cpp is byte-identical to nc-base; the cut
// is confined to this file and is recorded in AUDIT_20261111_presenter_port_ZH.md.

#include "b1_loader.hpp"
#include "diag.hpp"

namespace mgpu::b1_loader
{
namespace
{
bool g_said = false;
void say_once()
{
    if (g_said) return;
    g_said = true;
    mgpu::diag::info("[MGPU][B1] the private ImGui panel is not part of this build "
                     "(b1_imgui.dll absent by design); the presenter uses the shipped panel path");
}
}   // namespace

bool init(ID3D12Device *, ID3D12CommandQueue *, DXGI_FORMAT, UINT, UINT, bool enable)
{
    if (enable) say_once();
    return false;
}

void shutdown() {}
bool on() { return false; }
bool panel_owns_presenter() { return false; }
void set_visible(bool) {}
void new_frame(float, float, float, bool, float) {}
void render(ID3D12GraphicsCommandList *, D3D12_CPU_DESCRIPTOR_HANDLE) {}
}   // namespace mgpu::b1_loader
