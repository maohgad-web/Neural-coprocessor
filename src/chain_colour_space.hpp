// MGPU Bridge - swap-chain colour-space declaration (H1).
//
// WHY THIS FILE EXISTS
// CreateSwapChainForHwnd() leaves a swap chain in its default colour space,
// DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 (sRGB, gamma 2.2). A bridge that
// hands that chain an HDR10 frame - ST.2084 transfer, Rec.2020 primaries, in
// R10G10B10A2_UNORM - has its PQ codes decoded as if they were sRGB: the
// mid-tones collapse and the picture reads as milky/grey. The present path
// cannot repair that afterwards; the chain has to be told once, at creation.
//
// GATING (the rule for every new path: key absent = behaviour unchanged)
//   "ChainColorSpace" in mgpu.ini, read next to the add-on module:
//     absent, or negative -> SetColorSpace1 is never called. The chain keeps
//                            exactly the default it had before this file.
//     0                   -> sRGB (the DXGI default, stated on purpose)
//     1                   -> DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P2020
//     12                  -> DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020  (HDR10/PQ)
//     auto                -> R353 (0.5.0): follow the GAME's chain. dllmain records
//                            the game swap chain's colour space (ReShade's
//                            swapchain::get_color_space) at init and on every
//                            present; here it maps to the DXGI value: sRGB -> no
//                            call (the default), scRGB linear -> 1 (G10 P709),
//                            HDR10 PQ -> 12. Unknown / HLG -> no call, one line.
//   Any other value is handed to DXGI, which rejects what it cannot present -
//   the result is logged either way, so a wrong value is visible, never silent.
//
// SIDE EFFECTS: none. No resource, no barrier, no present-path change, no
// allocation. If the call fails the bridge continues exactly as before.
#pragma once

struct IDXGISwapChain3;

namespace mgpu {
namespace chain {

// Called once, from create_present_chain(), after the swap chain exists and
// before the first frame is presented into it.
void apply_colour_space(IDXGISwapChain3 *sc3);

// R353: the GAME chain's colour space as ReShade reports it, recorded by dllmain
// (init_swapchain and present, game chain only). Our own small code, so this file
// holds no ReShade type: 0 unknown, 1 sRGB non-linear, 2 extended sRGB linear
// (scRGB), 3 HDR10 ST.2084, 4 HDR10 HLG. One relaxed store.
void note_game_colour_space(int code);

} // namespace chain
} // namespace mgpu