TMFOXR - TRACKMANIA FOREVER VR - WinlatorXR DAWN PACKAGE - BETA 0.1
====================================================================

TrackMania Nations Forever in VR, standalone on Meta Quest 3 through
WinlatorXR Dawn: races in stereo 3D with head tracking, menus on a screen
inside VR, the Quest controllers as a gamepad. You need your own copy of
TrackMania Nations Forever (free on Steam) and the TMLoader "prepackaged"
mod loader. No game files are included. On the WinlatorXR cats builds use
TMFOXR-Quest-Installer-Beta-0.1.zip instead (it has install.cmd).


WHAT IS IN HERE
---------------
  TMFOXR.dll            the VR mod (a TMLoader mod)
  TMFOXR.defaults.ini   its default settings
  ModLoader.ini         TMLoader's mod list with TMFOXR added
  TMFOXR-VR.wxrprofile.json
                        the tested shortcut settings, ready to import into
                        Dawn
  TMFOXR-licenses\      licenses


INSTALL
-------
1. Copy your TrackMania Nations Forever folder (Steam:
   steamapps/common/TrackMania Nations Forever) to the headset as
     /sdcard/Download/TmForever
   so that TmForever\TmForever.exe exists.

2. Extract the TMLoader "prepackaged" zip (https://tomashu.dev/software/tmloader/)
   into that folder, as its README says (it replaces TmForever.exe).

3. Extract everything from this zip INTO the same folder, so that you get
     TmForever\TMFOXR.dll
   ModLoader.ini is replaced by the default TMLoader list with the line
     TMFOXR=TMFOXR.dll
   added. If you had changed your mod list, keep yours and add that line.

4. In WinlatorXR Dawn (drive D: = /sdcard/Download) create a shortcut for
     D:\TmForever\TmForever.exe
   Copy TMFOXR-VR.wxrprofile.json to
     /sdcard/Download/Winlator/WxrProfiles/
   and import it in the shortcut's settings. It sets the screen size
   (3456x1728), the Turnip T26 driver (Turnip_Adreno_Driver_T26 by
   Mr_Purple_666; pick another Turnip if you do not have it) and Box64 with
   the Performance preset. Then set the DX wrapper by hand in the
   container settings: DXVK 2.4 with Async on.

5. Put the headset ON, then start the shortcut. The game switches to VR
   after a few seconds. The mod forces the game to the screen size, so
   TrackMania's own resolution setting does not matter.

6. Recommended: lower the graphics quality once. Start
   D:\TmForever\TmForeverLauncher.exe in WinlatorXR's desktop mode, open
   Settings and pick a lower quality. It raised races from about 23 to
   30-45 fps. Close the launcher before starting the game.

UPDATING: extract the new zip over the folder again.


CONTROLS
--------
  In races:
    Left stick     = steer (analog)
    Right trigger  = accelerate       Left trigger = brake / reverse
    A = respawn at the last checkpoint    B = restart the track
    Y = next camera (1, 2, 3)         Left menu button = pause menu
  In menus:
    Left stick     = move through the menu, one step at a time
    A = Enter      B = Esc / back
  Always:
    Left stick click, hold 1 s = recenter the view
    Right stick click          = WinlatorXR menu (VR keyboard for the
                                 player name and account settings)

Nothing needs to be bound in TrackMania.


TROUBLESHOOTING
---------------
  - The picture squeezes or looks tiny: the container screen size is not a
    2:1 size. Use 3456x1728 (or 2880x1440, 1920x1080).
  - Black screen or crash at start: WinlatorXR crashed with screens larger
    than about 3500 pixels wide; stay at 3456x1728 or below.
  - Low fps: lower the graphics quality (step 6) and charge the headset
    above 50% (it throttles when low).
  - Reports: send TmForever\TMFOXR.log.

TrackMania Nations Forever is (C) Nadeo / Ubisoft. TMLoader by tomashu.
TMFOXR is based on TrackManiaForeverOpenXR by jiink.
TMFOXR for Quest: https://github.com/SgtBilko76/TrackManiaForeverOpenXR
