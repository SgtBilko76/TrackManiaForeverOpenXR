@echo off
setlocal EnableExtensions
rem ===========================================================================
rem  TMFOXR (TrackMania Forever VR) - WinlatorXR installer for Meta Quest
rem  Run this INSIDE your WinlatorXR container (wine cmd /c install.cmd).
rem  It installs TMFOXR onto your EXISTING TrackMania Nations Forever folder
rem  with the TMLoader "prepackaged" mod loader. It does not contain the game.
rem ===========================================================================
set "SRC=%~dp0"

echo.
echo  TMFOXR - TrackMania Forever VR for WinlatorXR
echo  ---------------------------------------------
echo.
rem TMFOXR_GAME_DIR and TMFOXR_CONTAINER skip the questions (unattended installs).
set "GAME=D:\TmForever"
if defined TMFOXR_GAME_DIR (set "GAME=%TMFOXR_GAME_DIR%") else set /p "GAME=Path to your TrackMania folder [%GAME%]: "
if "%GAME%"=="" set "GAME=D:\TmForever"
if "%GAME:~-1%"=="\" set "GAME=%GAME:~0,-1%"
if not exist "%GAME%\TmForever.exe" (
  echo.
  echo  ERROR: "%GAME%\TmForever.exe" was not found.
  echo  Copy the complete TrackMania Nations Forever folder there first,
  echo  then run this installer again.
  echo.
  pause
  exit /b 1
)
if not exist "%GAME%\ModLoader.ini" (
  echo.
  echo  ERROR: "%GAME%\ModLoader.ini" was not found.
  echo  Extract the TMLoader "prepackaged" zip into the game folder first
  echo  ^(https://tomashu.dev/software/tmloader/^), then run this installer again.
  echo.
  pause
  exit /b 1
)
set "CONTAINER=1"
if defined TMFOXR_CONTAINER (set "CONTAINER=%TMFOXR_CONTAINER%") else set /p "CONTAINER=WinlatorXR container number for the shortcut [%CONTAINER%]: "
if "%CONTAINER%"=="" set "CONTAINER=1"

echo.
echo  Installing TMFOXR into "%GAME%" ...
xcopy /E /I /Y "%SRC%files\TmForever" "%GAME%" >nul
if errorlevel 1 (
  echo  ERROR: failed to copy the TMFOXR files.
  pause
  exit /b 1
)

echo  Adding TMFOXR to ModLoader.ini ...
findstr /C:"TMFOXR=" "%GAME%\ModLoader.ini" >nul
if errorlevel 1 (
  >> "%GAME%\ModLoader.ini" echo.
  >> "%GAME%\ModLoader.ini" echo TMFOXR=TMFOXR.dll
)

echo  Adding the WinlatorXR shortcut ...
set "DESKTOP=%USERPROFILE%\Desktop"
if not exist "%DESKTOP%" mkdir "%DESKTOP%"
> "%DESKTOP%\TMFOXR-VR.desktop" (
  echo [Desktop Entry]
  echo Name=TMFOXR-VR
  echo Exec=wine C:\\\\windows\\\\system32\\\\cmd.exe
  echo Type=Application
  echo StartupWMClass=TmForever.exe
  echo.
  echo [Extra Data]
  echo container_id=%CONTAINER%
  echo screenSize=3456x1728
  echo execArgs=/c %GAME%\TMFOXR-VR.bat
)

echo.
echo  ============================================================
echo   TMFOXR installed. REMAINING STEPS in the WinlatorXR UI:
echo.
echo   1^) Container %CONTAINER% settings:
echo        - Screen size : 3456x1728
echo        - DX wrapper  : DXVK 2.4, Async on
echo        - Box64 preset: Performance
echo        - Drive D:    : /sdcard/Download
echo        - XR settings : CPU level at the highest value
echo   2^) Recommended: lower the graphics quality once in
echo        %GAME%\TmForeverLauncher.exe ^(Settings^).
echo   3^) Close this window and start the TMFOXR-VR shortcut
echo        with the headset on.
echo.
echo   See README_INSTALL.txt for the full walkthrough.
echo  ============================================================
echo.
if not defined TMFOXR_NO_PAUSE pause
endlocal
exit /b 0
