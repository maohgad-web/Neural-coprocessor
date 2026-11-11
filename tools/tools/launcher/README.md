# MGPU Bridge Launcher (experimental)

The launcher helps install and configure the MGPU Bridge add-on: a library of game folders, an install check, a display scan with a test of the DLSS 5 card without running a game, and the record of the last launch.

## Download warning

The launcher is not code-signed yet. Your browser or Windows may warn when you download or first run it ("unknown publisher", "not commonly downloaded"). It is built by this repository's own workflow from the source in `tools/launcher`; the build log is on the Actions tab. If Windows SmartScreen shows the warning, choose "More info" and "Run anyway". Signing is planned.

## Get it

Open the repository's **Actions** tab, then **build**, then the latest successful run. Download `mgpu_bridge_launcher` under **Artifacts**: the launcher with the bridge files already in its `payload\` folder.

## Set it up

1. Unzip it anywhere.
2. Keep the `payload\` folder next to the launcher: **Copy files** copies from it.
3. Run `MGPU Bridge Launcher.exe`.

## Use it

The launcher has three pages: **Install** (the install steps), **Status** (the selected game) and **Options** (the settings the ReShade panel writes, for the next launch).

- **Add game**: the folder with the game's exe, where ReShade is installed. The launcher asks for the name to show; **Rename** changes it.
- **Save**: changes on the Status and Options pages are kept until you press **Save**, which writes them to that game's `mgpu.ini` (the page shows the file).
- **Copy files** (Status page): copies the bridge files from `payload\`. An existing `gpu1.ini` is never overwritten. An existing `mgpu.ini` is replaced by the one in the release after the launcher asks: your Dcomp settings are kept, and you can keep a backup of the old file. `nvngx_dlssnr.dll` is not included with the bridge and is not copied: the **Install** page says where to get it.
- **Display** (Status page): the add-on decides which card does the DLSS 5 work. The row shows Windows' main display: the picture lands on the card that drives it. **Scan display** reads the displays and cards again and runs the test below. An old `Display=` line in a game's `mgpu.ini` is removed when you press **Save**.
- **Diagnose** (Status page): started by **Scan display**. It measures DLSS-NR (`nvngx_dlssnr.dll` in the game's `mgpu` folder) at a resolution, with no game running. About ten seconds. The card is the one the add-on used on the last launch; before any launch, the only NVIDIA card that does not drive the main display. If that is not clear, run the game once first.
- **Install** page: the install steps. ReShade with add-on support ([download](https://reshade.me/downloads/ReShade_Setup_6.8.0_Addon.exe)). `nvngx_dlssnr.dll` from [RHI](https://github.com/RankFTW/RHI) or the RenoDX Discord, put in the game's `mgpu` folder. Other ReShade add-ons (`.addon64` / `.addon`) moved out of the game folder; the Status page lists the ones it finds.
- **Last launch**: what the add-on recorded on the last run: which card ran the game, which the bridge used, and whether the display was on the DLSS 5 card.
- **Language**: the flags under the library switch the launcher and the install guide between English, Português (Brasil), Español, 中文, 日本語 and 한국어.

The launcher never runs a game and never changes ReShade or the driver.
