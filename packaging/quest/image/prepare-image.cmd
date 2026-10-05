@echo off
setlocal EnableExtensions
rem ===========================================================================
rem  TMFOXR container image: run ONCE inside a fresh WinlatorXR container,
rem  then export the container as an image. It copies the first-start program
rem  and the VR mod to C:\TMFOXR and adds the "TrackMania VR" shortcut. It
rem  does not install TrackMania: TMFOXR-Setup.exe downloads Nadeo's official
rem  installer when the image's user starts the shortcut for the first time.
rem ===========================================================================
set "SRC=%~dp0"

echo.
echo  Preparing the TrackMania VR container image ...
if exist "C:\TmForever\TmForever.exe" (
  echo.
  echo  ERROR: TrackMania is already installed in this container.
  echo  The image must not contain the game; use a fresh container.
  pause
  exit /b 1
)
xcopy /E /I /Y "%SRC%files\TMFOXR" "C:\TMFOXR" >nul
if errorlevel 1 (
  echo  ERROR: failed to copy the files to C:\TMFOXR.
  pause
  exit /b 1
)

set "DESKTOP=%USERPROFILE%\Desktop"
if not exist "%DESKTOP%" mkdir "%DESKTOP%"
> "%DESKTOP%\TrackMania VR.desktop" (
  echo [Desktop Entry]
  echo Name=TrackMania VR
  echo Exec=wine C:\\\\TMFOXR\\\\TMFOXR-Setup.exe
  echo Type=Application
  echo StartupWMClass=tmforever.exe
  echo.
  echo [Extra Data]
  echo screenSize=3456x1728
)

echo.
echo  ============================================================
echo   Done. Do NOT start "TrackMania VR" in this container.
echo   Close this window, then in WinlatorXR: Containers ^>
echo   this container's menu ^> Export as Image.
echo  ============================================================
echo.
pause
endlocal
exit /b 0
