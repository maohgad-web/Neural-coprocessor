# Single Display Guide

Running MGPU Bridge with one monitor instead of two.

**This is not supported.** Two displays, one per card, is the supported and
measured configuration. What follows is something that worked here. It may work
for you. If it does not, that is the expected outcome and there is no fix
coming. Do not open an issue about it.

* * *

## What you need

- Two GPUs, and MGPU Bridge already installed and working on them.
- **One display, and its cable must be on the NEURAL card** \- the second card,
  the one the bridge presents from. The render card runs dark.
- **A controller.** This is not optional. See below.
- Special K. Pirated game copies do not work with it.

## Why a controller is mandatory

With one screen the bridge window has to share glass with the game, and it sits
on top. Mouse messages go to whichever window is under the cursor, so they land
on the bridge window and die there. Keyboard and gamepad do not work that way \-
they reach the foreground window regardless of what is on top of it, so the game
still gets them.

That is the whole reason this needs a controller, and it is also why no
configuration change fixes the mouse.

* * *

## Steps

If you would rather watch it: https://youtu.be/\_K1H3mgcHy4

1. Install Special K.

2. Download `Single-Display.txt` from the folder this guide is in.

3. Open it and put its contents into:
   
   ```
   C:\Users\USERNAME\AppData\Local\Programs\Special K\Global\default_SpecialK.ini
   ```
   
   On a fresh Special K install that file exists and is empty. Replace what is
   there. Do not append.

4. Start the injection service in SKIF. Installing Special K is not enough on
   its own \- the service has to be running.

5. Launch the game from SKIF.

6. Alt\-tab and press Home to reach the panel and configure DLSS 5.

If the video and this page ever disagree, this page is the one that is current.

* * *

## What the settings do

| Setting | What it fixes |
| --- | --- |
| `[API.Hook] d3d11=false` and `d3d12=false` | Special K and ReShade both hook DXGI. With both active the process crashes at startup. This takes Special K out of that path. It also means Special K's own overlay will not draw, which is expected |
| `[SpecialK.System] TraceLoadLibrary=false` | Special K blocks the bridge from loading `nvngx_dlssnr.dll`. Symptom is ERROR 302 and a transport\-only run with no picture |
| `[Window.System] TreatForegroundAsActive=true` and `RenderInBackground=true` | Keeps the game processing input and rendering while the bridge window is on top |
| `[Input.Keyboard] DisabledToGame=0` | Lets the Home key through so the ReShade panel opens |

* * *

## What it costs

About 7.5 percent higher median frame latency than a monitor per card.

That figure is one comparison, on one title, between two runs that differed by
more than topology alone. Treat it as an indication, not a measurement. It is
noted here because the add\-on's own log currently claims the penalty is far
larger, and on this rig it was not.

* * *

## If it does not work

**You already had a Special K profile for that game.** The global config only
seeds profiles Special K creates from now on. An existing one is untouched.
Delete the profile folder under `Special K\Profiles\` and launch again.

**The settings do not stick.** Special K sometimes rewrites
`[Input.Keyboard] DisabledToGame` back to its own value when a game exits, and
the overlay stops opening on the next launch. Making the profile ini read\-only
prevents it.

**Special K never attached.** If there is no `logs` folder inside that game's
profile folder afterwards, Special K did not inject into the game at all, and
nothing in this guide applies. That happened here with some titles launched as
SKIF custom entries.

* * *

## What this is not

Special K is a separate project by other people. It is not affiliated with this
one, it is not bundled with it, and the file in this folder is configuration for
it rather than part of the add\-on. Nothing here changes what MGPU Bridge
supports.

If you find an arrangement that works better, a pull request is welcome.
