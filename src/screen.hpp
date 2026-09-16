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
// Text is UPPER CASE A-Z, 0-9, space, '-', '.', ':' and '/'. Anything else
// draws as a space rather than as a wrong glyph, because a wrong glyph in an
// error message is worse than a gap.
//
// R150: '/' WAS ALREADY IN THE TABLE AND THIS COMMENT DID NOT SAY SO. It is
// listed now because the report lines below put a URL on screen and depend on
// it - an undeclared glyph is one refactor away from being dropped as unused.
// ---- THE CODES ----
//
// A number AND a sentence. The number is what a user types into a search box
// or pastes into an issue; the sentence is what lets them fix it without
// asking anyone. Both are on screen, and the same pair goes in the log, so a
// screenshot and a log line can never disagree about what went wrong.
//
// 2xx is setup - something the user can fix by moving a file.
// 3xx is environment - hardware or driver.
// ---- R150: WHEN TO GIVE A FIX, AND WHEN TO ASK FOR THE LOG ----
//
// A screen that tells someone to go and change a setting is making a claim
// about their machine. It is worth making when the claim has been checked and
// the remedy is one action. It is not worth making otherwise, because a wrong
// instruction costs them an evening and then they report the wrong thing.
//
// SO THERE ARE TWO KINDS OF SECOND LINE, AND ONLY TWO.
//
//   A REMEDY, where the diagnosis is confirmed and the fix is one action:
//   205 (the file is missing - copy it) and 204 (the log line prints their own
//   EffectSearchPaths beside ours and the exact line to change).
//
//   A REQUEST FOR THE LOG, everywhere else: where the condition is rare enough
//   that it has never been observed, where a false positive is possible, or
//   where the log cannot yet be searched for the code. 203 and 302 are here,
//   and so is an arm that has held past a minute.
//
// Reporting costs the user one upload and costs us nothing. A wrong remedy
// costs them the evening and then arrives as a bad report anyway.
#define MGPU_REPORT_L2 \
    "SEND RESHADE.LOG TO GITHUB.COM/MAOHGAD-WEB/NEURAL-COPROCESSOR"

// R150. The arm has held past ARM_REPORT_FRAMES. Measured for contrast: a
// healthy Resonance arm completes in 252 frames on the depth lane and about
// 1305 on the velocity lane, twice, within 1.2% of each other.
//
// "IF YOU ARE IN GAMEPLAY" IS THE WHOLE SAFETY OF THIS MESSAGE. R54 measured
// 20 seconds before ReShade bound a depth buffer and R56 measured 35, both
// from a menu, and a player can sit in a menu or a cutscene for as long as
// they like. The screen cannot tell a long menu from a fault - so it does not
// try. It states the condition under which this IS a fault and lets the person
// holding the controller decide, which is the one thing they can do that the
// add-on cannot.
#define MGPU_H207_DEPTH_L1 "NO DEPTH AFTER ONE MINUTE"
#define MGPU_H207_MVEC_L1  "NO MOTION VECTORS AFTER ONE MINUTE"
#define MGPU_H207_L2 \
    "IF YOU ARE IN GAMEPLAY " MGPU_REPORT_L2

#define MGPU_E201_L1 "ERROR 201"
#define MGPU_E201_L2 "NVNGX-DLSSNR.DLL NOT FOUND - PUT IT BESIDE THE GAME EXE"

#define MGPU_E202_L1 "ERROR 202"
#define MGPU_E202_L2 "MGPU-DEPTH-TAP.FX NOT IN RESHADE-SHADERS.SHADERS"

#define MGPU_E203_L1 "ERROR 203"
// R150. WAS "TURN IT ON IN THE RESHADE MENU", AND THAT WAS A GUESS.
// The add-on enables this technique itself; if that did not take, doing it by
// hand may not either, and the reason would be the interesting part. The
// condition has never been observed - after R147 it needs find_technique to
// return a null handle twice in a row on a tap that IS present and compiled.
// An unobserved condition does not get a confident remedy.
#define MGPU_E203_L2 MGPU_REPORT_L2

// R142. The GAME runtime never loaded the tap at all. NOT the same fault as
// 202: the file can be exactly where the guide says and this still fires,
// because what is wrong is the SEARCH PATH of the runtime that owns the
// depth - not the location of the file. The log's [R142] line prints their
// EffectSearchPaths and ours side by side.
#define MGPU_E204_L1 "ERROR 204"
#define MGPU_E204_L2 "GAME RESHADE IS NOT LOADING THE TAP - SEE ERROR 204 IN RESHADE.LOG"

// ---- R145: THE TWO CODES FOR "LOADED, HEALTHY, AND DOING NOTHING" ----
//
// Both were measured on 2026-09-16, on two titles, and both spent a test
// session each. In neither case was anything broken: the add-on had loaded,
// passed every self-check, and was deliberately inert - and the screen said
// "STARTING - THE GAME WILL APPEAR WHEN THE STREAM ARMS", which is a promise
// it had no way to keep. A screen that cannot say "nothing is going to happen"
// is worse than the cycling hue this file replaced, because at least the hue
// did not claim to be fine.
//
// 205 IS AN ERROR AND 206 IS NOT, and the difference is intent. A missing
// mgpu.ini is an install that did not finish - nobody chooses it, and every
// key in the add-on is then at a code default that the shipped file overrides,
// AutoArm and Depth included. AutoArm=0 with a file present is a decision
// somebody made, so it gets the neutral field: stated plainly, not scolded.
#define MGPU_E205_L1 "ERROR 205"
#define MGPU_E205_L2 "NO MGPU.INI BESIDE THE ADD-ON - COPY IT FROM THE ZIP"

#define MGPU_S206_L1 "MGPU BRIDGE"
#define MGPU_S206_L2 "AUTOARM IS OFF IN MGPU.INI - NOTHING WILL ARM BY ITSELF"

// R145. Not codes: the arm names the lane it is held on. "ARMING" for ninety
// seconds is indistinguishable from a hang, and R63/R78 measured 20 to 35
// seconds of real scene as NORMAL - so these stay on the waiting field and say
// what they are waiting for rather than going red on a healthy startup.
#define MGPU_WAIT_DEPTH_L2 "ARMING - WAITING FOR THE GAMES DEPTH BUFFER"
#define MGPU_WAIT_MVEC_L2  "ARMING - WAITING FOR MOTION VECTORS"
#define MGPU_WAIT_ARM_L2   "ARMING"

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
