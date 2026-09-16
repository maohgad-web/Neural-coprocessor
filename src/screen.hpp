// ---------------------------------------------------------------------------
// screen.hpp - R108. THE IDLE SCREEN.
//
// WHAT IT REPLACES. When the bridge cannot arm, GPU 1 cleared its window to a
// cycling saturated colour. A full-screen flat hue is the harshest thing a
// display can do, it reads as a fault even when nothing is wrong, and it says
// nothing about WHAT is missing - which cost a whole evening on Cyberpunk,
// where the only problem was an empty reshade-shaders\Shaders folder.
//
// WHAT MAKES IT POSSIBLE WITH NO SHADER. ClearRenderTargetView takes a SCISSOR
// RECT ARRAY. gpu1_context already uses that for the seam marker. A scissored
// clear is a filled rectangle, and a filled rectangle is all a bitmap font
// needs - so this draws readable text with no pixel shader, no vertex buffer,
// no font file and no ImGui. It works in exactly the situation where nothing
// else does, because the thing that is missing IS the shader folder.
//
// STATE LIVES IN THE RHYTHM, NOT THE HUE. Everything is a dark neutral: an 8%
// grey field with one soft bar sweeping across it, which is the universal
// visual language for "waiting, not broken". Colour is spent only on an error,
// and then only a dull red - so when colour appears it actually means
// something.
// ---------------------------------------------------------------------------
#pragma once

#include <d3d12.h>

namespace mgpu
{
namespace screen
{

enum state
{
    st_idle    = 0,   // slow dim sweep. Something is missing, nothing is wrong.
    st_waiting = 1,   // brighter, quicker. Armed, no data yet.
    st_error   = 2    // dull red pulse. The only place saturation is used.
};

// Draws the whole idle presentation into rtv: field, sweep, and up to two
// lines of text centred in the upper third. The render target must already be
// in RENDER_TARGET state - the caller's existing clear has it there.
//
// Text is UPPER CASE A-Z, 0-9, space, '-', '.' and ':'. Anything else draws as
// a space rather than as a wrong glyph, because a wrong glyph in an error
// message is worse than a gap.
// ---- THE CODES ----
//
// A number AND a sentence. The number is what a user types into a search box
// or pastes into an issue; the sentence is what lets them fix it without
// asking anyone. Both are on screen, and the same pair goes in the log, so a
// screenshot and a log line can never disagree about what went wrong.
//
// 2xx is setup - something the user can fix by moving a file.
// 3xx is environment - hardware or driver.
#define MGPU_E201_L1 "ERROR 201"
#define MGPU_E201_L2 "NVNGX-DLSSNR.DLL NOT FOUND - PUT IT BESIDE THE GAME EXE"

#define MGPU_E202_L1 "ERROR 202"
#define MGPU_E202_L2 "MGPU-DEPTH-TAP.FX NOT IN RESHADE-SHADERS.SHADERS"

#define MGPU_E203_L1 "ERROR 203"
#define MGPU_E203_L2 "EFFECT NOT ENABLED - TURN IT ON IN THE RESHADE MENU"

// R142. The GAME runtime never loaded the tap at all. NOT the same fault as
// 202: the file can be exactly where the guide says and this still fires,
// because what is wrong is the SEARCH PATH of the runtime that owns the
// depth - not the location of the file. The log's [R142] line prints their
// EffectSearchPaths and ours side by side.
#define MGPU_E204_L1 "ERROR 204"
#define MGPU_E204_L2 "GAME RESHADE IS NOT LOADING THE TAP - SEE ERROR 204 IN RESHADE.LOG"

#define MGPU_E301_L1 "ERROR 301"
#define MGPU_E301_L2 "NO SECOND ADAPTER FOUND"

#define MGPU_E302_L1 "ERROR 302"
#define MGPU_E302_L2 "DLSS-NR REFUSED TO CREATE ON THIS DRIVER"

#define MGPU_IDLE_L1 "MGPU BRIDGE"
#define MGPU_IDLE_L2 "WAITING FOR THE FIRST FRAME"

void draw(ID3D12GraphicsCommandList *cl,
          D3D12_CPU_DESCRIPTOR_HANDLE rtv,
          unsigned width, unsigned height,
          unsigned long long frame,
          int st,
          const char *line1,
          const char *line2);

} // namespace screen
} // namespace mgpu
