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
//                            R369 (0.5.1): and it keeps following. A game states HDR
//                            with a SetColorSpace1 AFTER its chain exists; when the
//                            bridge chain was created first, R353 read sRGB and never
//                            looked again. The present path now compares the recorded
//                            value with the one last decided for and, on a change,
//                            makes the same checked call (HDR -> SDR sets 0 again).
//   Any other value is handed to DXGI, which rejects what it cannot present -
//   the result is logged either way, so a wrong value is visible, never silent.
//
// SIDE EFFECTS: none. No resource, no barrier, no allocation. If the call fails
// the bridge continues exactly as before. Present path (R369): `auto` only, a few
// relaxed loads per present; any other value or no key = one relaxed load.
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

// R369: called on the bridge side once per present, before Present. Does nothing
// unless the key read `auto` at creation and the game's recorded colour space
// differs from the one the chain was last decided for.
void follow_game_colour_space(IDXGISwapChain3 *sc3);

// R369: true on the calling thread while this file is inside SetColorSpace1 on the
// bridge chain. dllmain's init_swapchain uses it so that chain is never recorded
// as the game's (with FrameGen=4 the call is made on the pacing thread).
bool own_call();

// R369: the bridge's own ResizeBuffers ran - `auto` states the colour space again
// on the next present (the format can change in that resize). No-op otherwise.
void bridge_chain_resized();

} // namespace chain
} // namespace mgpu