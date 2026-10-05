TMFOXR - TRACKMANIA FOREVER VR (Meta Quest / WinlatorXR cats) - INSTALLER - BETA 0.2
=====================================================================================

This package installs TMFOXR onto an EXISTING TrackMania Nations Forever
folder inside a WinlatorXR container on Meta Quest 3. It does NOT include
the game - use your own copy (free on Steam) with the TMLoader
"prepackaged" mod loader.

This is the package for the WinlatorXR "cats" builds (cats-27). It is
UNTESTED on cats so far (TMFOXR was tested on WinlatorXR Dawn); please
report how it goes. On Dawn use TMFOXR-Quest-Dawn-Beta-0.2.zip instead: it
is extracted into the game folder and brings an importable shortcut profile.

EASIEST: TrackMania-VR-Beta-0.2.tzst is a ready container image. Import it
in WinlatorXR (Containers > Import) and start its "TrackMania VR" shortcut:
it downloads TrackMania from Nadeo and sets everything up. This package is
for setting TMFOXR up in your own container instead.

Package contents:
  install.cmd          - the installer (run inside the container)
  README_INSTALL.txt   - this file
  QUEST_BETA.md        - full guide: controls, settings, known issues
  files\               - TMFOXR, its settings, a launcher and the licenses

--------------------------------------------------------------------------
WHAT THE INSTALLER DOES vs. MANUAL STEPS (WinlatorXR UI only)
--------------------------------------------------------------------------
install.cmd copies TMFOXR into your game folder, adds it to TMLoader's
ModLoader.ini and adds a WinlatorXR shortcut. It CANNOT configure the
container itself - do those steps by hand (below).

--------------------------------------------------------------------------
SETUP - do these in order
--------------------------------------------------------------------------
0) Use WinlatorXR cats-27 (package com.winlator.cmod).

1) Copy your TrackMania Nations Forever folder to the headset as
   Download/TmForever (so D:\TmForever\TmForever.exe exists), and extract
   the TMLoader "prepackaged" zip (https://tomashu.dev/software/tmloader/)
   into it.

2) Create a container in WinlatorXR and set:
     - Screen size : 3456x1728 (2880x1440 or 1920x1080 also work; keep a
                     2:1 size, and not wider than about 3500 pixels)
     - Drive  D:   : /sdcard/Download
     - Wine        : proton-9.0-x86_64, Box64 with the Performance preset
     - DX wrapper  : DXVK 2.4, Async on
     - Graphics driver: Turnip
     - XR settings : CPU level at the highest value

3) Copy this whole installer folder to D: (e.g. D:\TMFOXR-Setup), start the
   container and run install.cmd INSIDE it (file manager, or a one-shot
   shortcut: wine C:\windows\system32\cmd.exe  /c D:\TMFOXR-Setup\install.cmd).
   Accept D:\TmForever (or type your folder, without spaces) and the
   container number shown in WinlatorXR.

4) Recommended: start D:\TmForever\TmForeverLauncher.exe once, open
   Settings and pick a lower graphics quality. Close the launcher
   afterwards.

5) Close the window, open WinlatorXR's Shortcuts tab and start TMFOXR-VR
   with the headset on. The main menu appears on WinlatorXR's flat screen;
   VR switches on when a race starts.

--------------------------------------------------------------------------
CONTROLS
--------------------------------------------------------------------------
  In races:
    Left stick     = steer (analog)
    Right trigger  = accelerate       Left trigger = brake / reverse
    A = respawn at the last checkpoint    B = restart the track
    Y = next camera (1, 2, 3)         Left menu button = pause menu
  In menus:
    Left stick = move through the menu      A = Enter      B = Esc / back
  Always:
    Left stick click, hold 1 s = recenter the view
    Right stick click          = WinlatorXR menu (VR keyboard)

--------------------------------------------------------------------------
TROUBLESHOOTING
--------------------------------------------------------------------------
  - The picture squeezes or looks tiny: the screen size is not 2:1.
  - Black screen or crash at start: use a screen size up to 3456x1728.
  - Low fps: lower the graphics quality (step 4); charge above 50%.
  - Reports: send D:\TmForever\TMFOXR.log.
