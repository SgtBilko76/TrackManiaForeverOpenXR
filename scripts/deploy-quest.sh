#!/usr/bin/env bash
# Build the XrAPI (WinlatorXR) variant of TMFOXR on Linux and install it into a
# TrackMania folder on a Quest/Pico headset connected over adb. The folder must
# already contain the game with the prepackaged TrackMania ModLoader.
#
#   LLVM_MINGW_ROOT=~/.local/opt/llvm-mingw-... scripts/deploy-quest.sh [/sdcard/Download/TmForever]
set -euo pipefail

GAME_DIR="${1:-/sdcard/Download/TmForever}"
REPO="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$REPO/build-quest"

if [[ -z "${LLVM_MINGW_ROOT:-}" ]]; then
  LLVM_MINGW_ROOT="$(ls -d "$HOME"/.local/opt/llvm-mingw*/ 2>/dev/null | head -1)"
fi
[[ -n "$LLVM_MINGW_ROOT" ]] || { echo "Set LLVM_MINGW_ROOT to an llvm-mingw toolchain." >&2; exit 1; }
export LLVM_MINGW_ROOT

cmake -S "$REPO" -B "$BUILD" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$REPO/cmake/llvm-mingw-i686.cmake" \
  -DTMFOXR_VR_BACKEND=XrAPI -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$BUILD" --target d3d9

adb shell "test -f '$GAME_DIR/ModLoader.ini'" ||
  { echo "$GAME_DIR/ModLoader.ini not found on the headset." >&2; exit 1; }
adb push "$BUILD/TMFOXR.dll" "$REPO/TMFOXR.defaults.ini" "$GAME_DIR/"
adb shell "grep -q '^TMFOXR=' '$GAME_DIR/ModLoader.ini' || printf '\r\nTMFOXR=TMFOXR.dll\r\n' >> '$GAME_DIR/ModLoader.ini'"
echo "Installed TMFOXR (XrAPI) into $GAME_DIR."
