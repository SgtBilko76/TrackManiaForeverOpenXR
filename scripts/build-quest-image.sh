#!/usr/bin/env bash
# Build the TrackMania VR container image for WinlatorXR (.tzst) from a
# container exported with WinlatorXR's "Export as Image".
#
#   scripts/build-quest-image.sh <exported.tzst> [out.tzst]
#
# The image ships Wine, TMFOXR, TMFOXR-Setup.exe and the tested settings, but
# no game files: the "TrackMania VR" shortcut runs TMFOXR-Setup.exe, which
# downloads Nadeo's official installer on the first start. Anything of the
# game, a player profile, logs or a cached installer left in the exported
# container is removed here, and the build fails if traces remain.
set -euo pipefail

BASE="${1:?usage: build-quest-image.sh <exported.tzst> [out.tzst]}"
REPO="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${2:-$REPO/release/TrackMania-VR.tzst}"
BUILD="$REPO/build-quest"
IMG="$REPO/packaging/quest/image"

cmake --build "$BUILD" --target d3d9 tmfoxr_setup >/dev/null

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
zstd -dc "$BASE" | tar -x -C "$WORK"
ROOTS=("$WORK"/xuser-*)
[[ ${#ROOTS[@]} -eq 1 && -f "${ROOTS[0]}/.container" ]] || { echo "Not a WinlatorXR container image: $BASE" >&2; exit 1; }
X="${ROOTS[0]}"
C="$X/.wine/drive_c"
U="$C/users/xuser"

# Game, profile, logs, caches and shortcuts of the exported container.
rm -rf "${C:?}/TmForever" "${U:?}/Documents/TrackMania" "${U:?}/Documents/TmForever" \
       "${C:?}/ProgramData/TrackMania" "${C:?}/ProgramData/TmForever" "${X:?}/.wine/.wineserver"
find "$U/AppData/Local/Microsoft/Windows/INetCache" -iname 'tmnationsforever_setup*' -delete 2>/dev/null || true
rm -f "$C"/windows/syswow64/dxwrapper-*.log "$C"/windows/system32/dxwrapper-*.log \
      "$X/.local/share/applications/wine-extension-gbx.desktop" \
      "$X/.local/share/applications/wine-protocol-tmtp.desktop" \
      "$X/.fex-emu/AppConfig/TmForever.exe.json"
find "$U/Desktop" "$C/users/Public/Desktop" -maxdepth 1 -type f -delete 2>/dev/null || true
find "$X/.local/share/icons" -name '*TmForever*' -delete 2>/dev/null || true

# First-start program, the mod and the shortcuts.
crlf() { sed 's/\r$//; s/$/\r/' "$1" > "$2"; }
mkdir -p "$C/TMFOXR/TMFOXR-licenses" "$U/Desktop"
install -m 755 "$BUILD/TMFOXR-Setup.exe" "$C/TMFOXR/TMFOXR-Setup.exe"
install -m 755 "$BUILD/TMFOXR.dll" "$C/TMFOXR/d3d9.dll"
crlf "$REPO/TMFOXR.defaults.ini" "$C/TMFOXR/TMFOXR.defaults.ini"
crlf "$REPO/LICENSE.md" "$C/TMFOXR/TMFOXR-licenses/LICENSE.md"
crlf "$REPO/packaging/quest/THIRD_PARTY_NOTICES.md" "$C/TMFOXR/TMFOXR-licenses/THIRD_PARTY_NOTICES.md"
install -m 644 "$IMG/TrackMania VR.desktop" "$U/Desktop/TrackMania VR.desktop"
install -m 644 "$IMG/TrackMania VR.lnk" "$U/Desktop/TrackMania VR.lnk"

# Tested settings: screen 3456x1728, the bundled Turnip T26 driver, DXVK
# 2.4-24-gplasync with async, Box64 Performance. WinlatorXR reinstalls the
# DX wrapper on import, so only the configuration matters here.
python3 - "$X/.container" <<'EOF'
import json, re, sys
path = sys.argv[1]
c = json.load(open(path))
c["name"] = "TrackMania VR"
c["screenSize"] = "3456x1728"
c["graphicsDriver"] = "wrapper"
c["graphicsDriverConfig"] = re.sub(r"^version=[^;]*", "version=Turnip_Adreno_Driver_T26_(@Mr_Purple_666)",
                                   c.get("graphicsDriverConfig", "version=;blacklistedExtensions=;maxDeviceMemory=0;adrenotoolsTurnip=1;frameSync=Never"))
c["dxwrapper"] = "dxvk"
config = c.get("dxwrapperConfig", "version=,framerate=0,maxDeviceMemory=0,async=1,asyncCache=1")
config = re.sub(r"version=[^,]*", "version=2.4-24-gplasync", config, count=1)
config = re.sub(r"async=\d", "async=1", config)
c["dxwrapperConfig"] = config
c["box64Preset"] = "PERFORMANCE"
c["lc_all"] = "en_US.UTF-8"
json.dump(c, open(path, "w"), separators=(", ", ": "))
EOF

# Nothing of the game or the player may remain.
LEFT="$(cd "$WORK" && find . | grep -iE 'tmnation|nadeo|/TmForever/|Documents/TrackMania|/Profiles' || true)"
[[ -z "$LEFT" ]] || { echo "Game or profile traces remain:" >&2; echo "$LEFT" >&2; exit 1; }

mkdir -p "$(dirname "$OUT")"
rm -f "$OUT"
(cd "$WORK" && tar --owner=0 --group=0 --numeric-owner --sort=name -cf - "$(basename "$X")") | zstd -19 -T0 -q -o "$OUT"
ls -la "$OUT"
sha256sum "$OUT"
