MGPU BRIDGE - INSTALL
=====================

Cross-adapter DLSS Neural Rendering: the game renders on one GPU, the neural
work runs on a second one that is not rendering the game.

This is research code. Read it before you run it - see ACKNOWLEDGEMENTS.md in
the repository, which is not a formality.

THIS IS AN INJECTED PATH, NOT NVIDIA'S. DLSS Neural Rendering is reached here by
injecting through ReShade, not by a game's own DLSS 5 integration. An injected
neural stage sees the frame at a different point than an engine-native one, gets
no engine motion vectors or depth, and makes none of the scheduling decisions a
native integration can. Performance and image quality may differ from NVIDIA's
official implementation in either direction, and the measurements published with
this project were taken on one machine in September 2026 - driver versions, the
DLSS-NR model inside them, and games all move. Treat any absolute number you read
about this project as historical and check your own.


WHAT YOU NEED FIRST
-------------------

1. ReShade 6.8.0 or newer, installed WITH ADD-ON SUPPORT.

   This is the single most common reason nothing happens. The effects-only
   build of ReShade never loads .addon64 files at all, and it does not say so -
   no error, no log line, because the add-on was never loaded to write one. If
   the log has no "Registered add-on \"MGPU Bridge\"" line, this is why.

2. A second NVIDIA GPU in the machine, with a current driver. DLSS-NR comes
   from your driver installation.

3. TWO MONITORS - ONE ON EACH CARD. This is a requirement, not a nicety. The
   neural output is displayed by the card that produced it, so nothing has to
   travel back across the link. With both monitors on the render card, the same
   build measured 33% lower throughput and roughly double the latency on the
   development machine. A headless second card works and is slower, and there is
   nothing to look at.

4. A DirectX 12 game. D3D11 titles do nothing - no game adapter is identified,
   so the bridge stands down and says so in the log. Some Unity titles can be
   forced with -force-d3d12.

NOTHING FROM NVIDIA IS INCLUDED HERE. _nvngx.dll, nvngx_dlssnr.dll and the
DLSS-NR weights come from your own driver install. Do not add them to this
folder to make it work for somebody else.


INSTALL
-------

Copy these files into the folder that contains the game's executable and the
ReShade DLL (usually dxgi.dll):

    nvngx.dll_mgpu_bridge.addon64    the add-on
    mgpu.ini                         its settings
    gpu1.ini                         preset for the bridge's own window
    ReShade2.ini                     config for the bridge's own runtime

The download also contains this README and LICENSE. Those two are for you, not
for the game folder, and copying them there does nothing either way.

gpu1.ini ships EMPTY on purpose - no techniques, no sort order. It is the preset
ReShade assigns to the bridge's own window, so anything enabled in it is drawn
on top of the neural output. A stale copy of this file once carried a motion-flow
debug view and cost a night of wrong diagnoses; CI now fails the build if it
ships with anything enabled.

DO NOT RENAME THE ADD-ON. The filename must contain the literal substring
"nvngx.dll". The DLSS-NR snippet resolves the module owning its caller's return
address, takes that module's file path, and requires it to contain that
substring. Rename the file and the snippet refuses to resolve; the bridge loads,
logs normally, and produces nothing.

mgpu.ini must sit BESIDE the add-on, not in the working directory. Each game
folder needs its own copy. The log line beginning [MGPU][P7.2] names the exact
file that took effect - if it names somewhere unexpected, or is missing, every
setting is at its default no matter what you edited.

Two files are deliberately NOT included, because they are yours and overwriting
them would destroy your existing setup:

    ReShade.ini          your ReShade configuration
    ReShadePreset.ini    your effect preset for the game

See "CHECK YOUR OWN ReShade.ini" below for the two settings worth changing.


RUN
---

Get into gameplay - not a menu - and press CTRL+ALT+F10.

Controls, live, no relaunch:

    Panel               the ReShade overlay (Home). The MGPU Bridge panel is
                        registered on both the game's overlay and the bridge
                        window's, so you can drive it without leaving the game.
    CTRL+ALT+F7         view: neural output -> input -> split
    CTRL+ALT+LEFT/RIGHT move the split seam. Add SHIFT for a coarse step.
                        These need no overlay open, so the seam can be dragged
                        across a face with nothing on screen but the game.
    CTRL+ALT+F8         pick which pass the intensity keys act on
    CTRL+ALT+F9 / F11   intensity down / up

Split shows the frame handed TO the model on the left and what it produced on
the right, in one window, THE SAME FRAME, with a white seam between them. It is
the only honest way to compare the two: no two runs of a game contain the same
frame.


CHECK YOUR OWN ReShade.ini
--------------------------

Two settings in your existing ReShade.ini are worth setting by hand:

  [OVERLAY]
  AutoSavePreset=0

    With this at 1, anything you toggle in the overlay is written back to the
    preset when the game closes. Presets then drift between runs with nobody
    editing a file, and the run you measure is not the run you repeat. Set it
    to 0 while taking measurements.

  [GENERAL]
  PresetPath=.\ReShadePreset.ini

    Whatever you point this at, make sure you know what is enabled in it. Any
    effect enabled for the GAME is drawn before the frame reaches the bridge,
    so it becomes part of what the neural stage sees.


IF SOMETHING LOOKS WRONG, READ THE LOG FIRST
--------------------------------------------

The instrumentation is deliberately loud, and these four lines answer almost
everything. If a log line and this README disagree, believe the log.

  Registered add-on "MGPU Bridge"
      Absent: ReShade has no add-on support, or the file is not in this folder.

  [MGPU][P1.6] ... | N of 14 techniques ENABLED
      Printed once per runtime, for the GAME and for the BRIDGE. Anything
      listed here is being drawn on top of what you are looking at. This line
      exists because a stale bridge preset once drew a motion-flow debug view
      over the neural output and three code hypotheses were spent on it.

  [MGPU][P7.2] mgpu.ini read from ...
      The settings file that actually took effect. Not the one you edited,
      necessarily.

  [MGPU][P4.0] stream REQUESTED - ... passes=N, ...
      Every setting in force for this run, in one line. If it disagrees with
      your mgpu.ini, the P7.2 line above says why.


RUNNING WITHOUT A FRAME BOUND
-----------------------------

THIS BUILD SHIPS WITH Frames=0 - the stream runs until the game closes and
prints no summary. Set Frames to a number (60..100000) if you want a bounded
run that ends with a summary, which is what the measurement runs used.

This is implemented as a bound that is never reached, NOT as a stop-and-restart
control: no neural feature and no texture is ever torn down and rebuilt while
the game is live, because that path has never been exercised and is the riskiest
code that could exist here. Nothing new runs; the same single stream simply
never satisfies the condition that ends it.

SESSIONS BEYOND A FEW MINUTES ARE UNTESTED. The longest clean run recorded is
about five and a half minutes. One development session ended with the game
rendering black on both displays after several minutes; the bridge logged no
fault and the cause was never found. Watch GPU load and temperature.


SCREENSHOTS WITH THE OVERLAY VISIBLE
------------------------------------

PrintScreen saves a clean frame with no overlay, which is usually what you want.
To also get a copy WITH the overlay - useful when you want the pass count and
the split seam visible in the shot - set this in ReShade.ini, and in ReShade2.ini
for the bridge window:

    [SCREENSHOT]
    SaveOverlayShot=1

ReShade then writes a second image alongside the clean one.


KNOWN LIMITATIONS
-----------------

THE ONE BAD FAILURE THIS PROJECT SAW IS EXPLAINED, AND IT WAS THE PASS COUNT.
One development session ended with the game rendering black on both displays
after several minutes, with no fault in any log. It was a SIX-pass run held for
several minutes, which holds the second GPU at its power limit indefinitely.
This build allows at most two passes and the bound is enforced in code, so that
condition is no longer reachable from the settings file. Stability at one and
two passes is still only tested in sessions of minutes, not hours - watch GPU
load and temperature, especially with Frames=0.

The bridge window's own frame rate falls as the pass count rises. The game's
does not. That is the architecture working, not a fault.

Interacting with the bridge window takes keyboard focus away from the game. A
controller sidesteps this entirely.

THE BRIDGE WINDOW OPENS ON THE GAME'S DISPLAY. You have to drag it to the second
monitor once per launch. This is a known defect, not something you configured
wrong: the window is created before the sizing code runs, and that code fits the
window to whichever monitor it is already on - so it sizes correctly to the wrong
display. Move it and Window=fit behaves as documented.

EXTERNAL FPS OVERLAYS FLICKER, AND IT IS NOT YOUR SETUP. This add-on puts a
second swapchain inside the game's process. RivaTuner / MSI Afterburner assume
one swapchain per process and flip between the two present streams; the NVIDIA
overlay does not recognise the bridge window at all. Use ReShade's own counter -
there are two ReShade runtimes here, so each draws its own FPS for its own
window, and the game's rate and the bridge's rate are different numbers by
design. Set ShowFPS=1 under [OVERLAY] in ReShade.ini AND in ReShade2.ini.

For screen recording, use a display capture or a Windows Graphics Capture window
source rather than "game capture", which adds a third Present hook to a process
that already has two.


No warranty. See LICENSE. Not affiliated with, endorsed by, or supported by
NVIDIA.
