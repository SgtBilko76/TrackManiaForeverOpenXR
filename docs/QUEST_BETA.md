# TMFOXR on Meta Quest (WinlatorXR) — Beta 0.1

TrackMania Nations Forever in VR, standalone on a Meta Quest 3 through
[WinlatorXR](https://winlatorxr.github.io/) (Wine + Box64 + DXVK). No PC is
needed while playing. The mod is loaded by
[TMLoader](https://tomashu.dev/software/tmloader/) and talks to WinlatorXR
through its [XrAPI](https://winlatorxr.github.io/xrapi.html).

Bring your own copy of TrackMania Nations Forever (free on Steam). No game
files are included.

## What you get

- Races in stereo 3D (both eyes every frame), with head tracking and the
  cockpit camera. 4x antialiasing.
- The main menu on WinlatorXR's flat screen, which is sharper than a screen
  inside VR.
- VR switches on when a race starts. The pause, medal and finish screens
  stay in VR, so there is no 2D fallback while playing. VR switches off
  about two seconds after you are back in the main menu.
- The Quest controllers as a gamepad, and nothing has to be bound in
  TrackMania.
- The game follows the WinlatorXR screen size. The mod forces TrackMania's
  window, backbuffer and display mode to it, so the game's own resolution
  setting does not matter.

## Requirements

- Meta Quest 3
- WinlatorXR Dawn (tested), or cats-27 (untested)
- TrackMania Nations Forever and the TMLoader "prepackaged" zip

## Container image (easiest)

`TrackMania-VR.tzst` is a ready WinlatorXR container with TMFOXR and the
tested settings. It contains no game files.

1. In WinlatorXR, go to Containers → menu → **Import** and pick
   `TrackMania-VR.tzst`.
2. Start the **TrackMania VR** shortcut. On the first start it downloads
   TrackMania Nations Forever from Nadeo's official server (530 MB, checked by
   SHA-256), installs it to `C:\TmForever` and starts it. Later starts go
   straight to the game.

The image doesn't need TMLoader: TMFOXR loads as the game's `d3d9.dll`.
Offline, put `tmnationsforever_setup.exe` into the headset's `Download`
folder before the first start, and it is used instead of the download.

The image is built with `scripts/build-quest-image.sh <exported.tzst>` from a
container exported with WinlatorXR's "Export as Image". The script removes
any game files, profile or cached installer and fails if traces remain.

## Downloads

- `TMFOXR-Quest-Dawn-Beta-0.1.zip`, for WinlatorXR Dawn:
  1. Extract it into the game folder.
  2. Import the included shortcut profile.
  3. Follow `README_DAWN.txt`.
- `TMFOXR-Quest-Installer-Beta-0.1.zip`, for WinlatorXR cats:
  1. Run `install.cmd` inside the container.
  2. Follow `README_INSTALL.txt`.

## Tested setup

| Setting | Value |
|---|---|
| WinlatorXR | com.winlator.cmod dawn-33 |
| Wine | proton-9.0-x86_64 |
| Box64 | 0.4.2, Performance preset |
| DX wrapper | DXVK 2.4, async on |
| Graphics driver | Turnip (wrapper) |
| Screen size | 3456x1728 (1728x1728 per eye) |
| Game folder | `/sdcard/Download/TmForever` = `D:\TmForever` |

Use a 2:1 screen size: 3456x1728, 2880x1440 or 1920x1080. The mod requests
a square 104.5° field of view per eye from WinlatorXR, like the Halo mod,
and each eye is half the screen width.

WinlatorXR crashed with screens wider than about 3500 pixels.

## Controls

| Quest controller | In races | In menus |
|---|---|---|
| Left stick | Steer (analog) | Move through the menu, one step at a time |
| Right trigger | Accelerate | – |
| Left trigger | Brake / reverse | – |
| A | Respawn at the last checkpoint | Enter |
| B | Restart the track | Esc / back |
| Y | Next camera (1, 2, 3) | – |
| Left menu button | Pause menu | – |
| Left stick click, hold 1 s | Recenter the view | Recenter the view |
| Right stick click | WinlatorXR menu | WinlatorXR menu |

For text input (player name, account settings), use WinlatorXR's VR
keyboard: press the right stick and choose Keyboard.

The race keys are TrackMania's default keyboard keys, injected into its
DirectInput keyboard. During races, keys that WinlatorXR's own controller
mapping types are ignored.

## Performance

Races run at about 30–45 fps and menus at about 70. The limit is
TrackMania's main thread under Box64. It uses one core fully, while the GPU
is almost idle. What helps:

- Lower TrackMania's graphics quality once in `TmForeverLauncher.exe`
  (Settings). It raised races from about 23 to 30–45 fps.
- Use Box64's Performance preset and DXVK with async on.
- The mod keeps the game's main thread on the Quest's four fastest cores.

The log (`TMFOXR.log` in the game folder) shows the frame rate and where
the frame time goes every 600 frames.

## Files beside TMFOXR.dll

These are optional, and all are re-read while the game runs unless noted.

| File | Content |
|---|---|
| `TMFOXR-menuvr.txt` | `1` shows the menus outside a race on a screen inside VR instead of WinlatorXR's flat screen. |
| `TMFOXR-menu.txt` | `<metres> <degrees> [curved 0/1]`: distance, width and shape of that VR menu screen. Default: 2.5 m, as wide as a 75° arc, flat. |
| `TMFOXR-fov.txt` | Field of view per eye in degrees (default 104.5); `0` uses the headset's own FOV. |
| `TMFOXR-msaa.txt` | Antialiasing samples: 0, 2, 4 (default) or 8. Read at start. |
| `TMFOXR-affinity.txt` | CPU mask for the game's main thread; `0` leaves it unpinned. Read at start. |
| `TMFOXR-profile.txt` | Starts a sampling profiler of the main thread. Diagnostic only: it once hung the game at start. |

## Known issues

- Short stutters the first time new objects or effects appear, while
  shaders are compiled. DXVK async reduces them.
- Only TrackMania Nations Forever (Stadium) was tested. United Forever is
  untested.
- The cats-27 installer is untested.
- Play with the battery above 50%; the Quest throttles when it is low.
