#!/usr/bin/env bash
# Build the Quest (WinlatorXR) release packages of TMFOXR:
#   TMFOXR-Quest-Dawn-<version>.zip       extracted into the TmForever folder,
#                                         with an importable shortcut profile
#   TMFOXR-Quest-Installer-<version>.zip  install.cmd package for the cats builds
#
#   LLVM_MINGW_ROOT=~/.local/opt/llvm-mingw-... \
#     scripts/package-quest-release.sh Beta-0.1 path/to/TMFOXR.wxrprofile.json [out-dir]
#
# The profile is exported from WinlatorXR Dawn (shortcut settings) with the
# tested container settings.
set -euo pipefail

VERSION="${1:?usage: package-quest-release.sh <version> <profile.wxrprofile.json> [out-dir]}"
PROFILE="${2:?usage: package-quest-release.sh <version> <profile.wxrprofile.json> [out-dir]}"
REPO="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${3:-$REPO/release}"
BUILD="$REPO/build-quest"
PKG="$REPO/packaging/quest"

if [[ -z "${LLVM_MINGW_ROOT:-}" ]]; then
  LLVM_MINGW_ROOT="$(ls -d "$HOME"/.local/opt/llvm-mingw*/ 2>/dev/null | head -1)"
fi
[[ -n "$LLVM_MINGW_ROOT" ]] || { echo "Set LLVM_MINGW_ROOT to an llvm-mingw toolchain." >&2; exit 1; }
export LLVM_MINGW_ROOT

cmake -S "$REPO" -B "$BUILD" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$REPO/cmake/llvm-mingw-i686.cmake" \
  -DTMFOXR_VR_BACKEND=XrAPI -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$BUILD" --target d3d9

# Windows text files get CRLF line endings.
crlf() { sed 's/\r$//; s/$/\r/' "$1" > "$2"; }

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$OUT"

# Files that go into the game folder, shared by both packages.
game_files() {
  local dir="$1"
  mkdir -p "$dir/TMFOXR-licenses"
  cp "$BUILD/TMFOXR.dll" "$dir/"
  crlf "$REPO/TMFOXR.defaults.ini" "$dir/TMFOXR.defaults.ini"
  crlf "$REPO/LICENSE.md" "$dir/TMFOXR-licenses/LICENSE.md"
  crlf "$PKG/THIRD_PARTY_NOTICES.md" "$dir/TMFOXR-licenses/THIRD_PARTY_NOTICES.md"
}

# Dawn: extracted straight into the TmForever folder.
DAWN="$STAGE/dawn"
game_files "$DAWN"
crlf "$PKG/ModLoader.ini" "$DAWN/ModLoader.ini"
crlf "$PKG/README_DAWN.txt" "$DAWN/README_DAWN.txt"
cp "$PROFILE" "$DAWN/TMFOXR.wxrprofile.json"
DAWN_ZIP="$OUT/TMFOXR-Quest-Dawn-$VERSION.zip"
rm -f "$DAWN_ZIP"
(cd "$DAWN" && zip -qrX "$DAWN_ZIP" .)

# cats: an installer folder copied to D: and run inside the container.
NAME="TMFOXR-Quest-Installer-$VERSION"
INST="$STAGE/$NAME"
game_files "$INST/files/TmForever"
crlf "$PKG/TMFOXR-VR.bat" "$INST/files/TmForever/TMFOXR-VR.bat"
crlf "$PKG/install.cmd" "$INST/install.cmd"
crlf "$PKG/README_INSTALL.txt" "$INST/README_INSTALL.txt"
cp "$REPO/docs/QUEST_BETA.md" "$INST/QUEST_BETA.md"
INST_ZIP="$OUT/$NAME.zip"
rm -f "$INST_ZIP"
(cd "$STAGE" && zip -qrX "$INST_ZIP" "$NAME")

echo "Built:"
(cd "$OUT" && sha256sum "$(basename "$DAWN_ZIP")" "$(basename "$INST_ZIP")")
