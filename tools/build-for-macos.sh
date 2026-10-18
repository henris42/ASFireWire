#!/usr/bin/env bash
#
# build-for-macos.sh - Build and package ASFW for an older macOS release.
#
# project.yml targets Tahoe. This derives a spec with lower deployment targets
# (build/specs/project.<os>.yml), builds it with build.sh, packages the app the
# way the Release workflow does, and restores ASFW.xcodeproj from project.yml.
#
#   tools/build-for-macos.sh monterey            # -> $DIST_DIR/monterey/
#   DIST_DIR=/tmp/out tools/build-for-macos.sh sequoia
#   ASFW_DYLD_CACHE=~/dk12/dyld tools/build-for-macos.sh monterey   # + symbol check
#
# Below macOS 14, ASFW.app is the lite variant of the same app: no Metal
# analyzer, meters, DV capture or MCP; same bundle, entitlements and dext, plus
# the headless activate/deactivate/status commands install.sh uses.

set -euo pipefail

usage() {
  echo "Usage: $0 <tahoe|sequoia|sonoma|ventura|monterey> [build.sh options...]" >&2
  exit 2
}

[[ $# -ge 1 ]] || usage
OS_NAME="$1"; shift

# macOS marketing version -> DriverKit (Darwin) version.
case "$OS_NAME" in
  tahoe)    MACOS="26.0"; DRIVERKIT="25.0" ;;
  sequoia)  MACOS="15.0"; DRIVERKIT="24.0" ;;
  sonoma)   MACOS="14.0"; DRIVERKIT="23.0" ;;
  ventura)  MACOS="13.0"; DRIVERKIT="22.0" ;;
  monterey) MACOS="12.0"; DRIVERKIT="21.0" ;;
  *) usage ;;
esac

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

DIST_DIR="${DIST_DIR:-$HOME/dist}"
SPEC="build/specs/project.${OS_NAME}.yml"
# Below macOS 14 the app is built "lite": the same app without the Metal audio
# analyzer, meters, DV capture and the MCP server, with the loader's headless
# --activate/--deactivate/--status for install.sh (tools/derive-spec.py).
APP_KIND=full
[[ "${MACOS%%.*}" -ge 14 ]] || APP_KIND=lite

mkdir -p "$(dirname "$SPEC")"
# The dext refuses to start unless the controller has an "asfw-armed" property
# (OpenCore DeviceProperties); ASFW_ARMING=0 builds without that gate.
ARMING="${ASFW_ARMING:-1}"

python3 -I tools/derive-spec.py "$MACOS" "$DRIVERKIT" "$APP_KIND" "$ARMING" "$SPEC"

# Leave the committed project matching project.yml whatever happens below.
trap 'xcodegen generate --spec project.yml --quiet || echo "warning: could not restore ASFW.xcodeproj" >&2' EXIT

echo "== Building for macOS ${MACOS} / DriverKit ${DRIVERKIT} (app: ${APP_KIND}, arming: ${ARMING})"
./build.sh --config Release --spec "$SPEC" "$@"

APP="build/DerivedData/Build/Products/Release/ASFW.app"
DEXT_BIN="$APP/Contents/Library/SystemExtensions/net.asfw.driver.dext/net.asfw.driver"
[[ -d "$APP" ]] || { echo "error: $APP not found" >&2; exit 1; }

# Refuse to package a product whose stamped minimums disagree with the request.
for arch in $(lipo -archs "$DEXT_BIN"); do
  minos=$(vtool -arch "$arch" -show-build "$DEXT_BIN" | awk '/minos/ { print $2; exit }')
  [[ "$minos" == "$DRIVERKIT" ]] || { echo "error: dext $arch minos $minos, expected $DRIVERKIT" >&2; exit 1; }
done
app_min=$(/usr/libexec/PlistBuddy -c 'Print :LSMinimumSystemVersion' "$APP/Contents/Info.plist")
[[ "$app_min" == "$MACOS" ]] || { echo "error: app LSMinimumSystemVersion $app_min, expected $MACOS" >&2; exit 1; }

# install.sh signs the dext with ASFWDriver.entitlements; DriverKit lets the dext
# open only the PCI devices that entitlement lists, so it must match the personality.
DEXT_PLIST="${DEXT_BIN%/*}/Info.plist"
match=$(/usr/libexec/PlistBuddy -c 'Print :IOKitPersonalities:ASFWDriverService:IOPCIPrimaryMatch' "$DEXT_PLIST")
entitled=$(/usr/libexec/PlistBuddy -c 'Print :com.apple.developer.driverkit.transport.pci:0:IOPCIPrimaryMatch' ASFWDriver/ASFWDriver.entitlements)
[[ "$match" == "$entitled" ]] || { echo "error: IOPCIPrimaryMatch '$match' != entitlement '$entitled'" >&2; exit 1; }

# Arm at match time as well: with IOPropertyMatch the personality only matches a
# controller that carries asfw-armed = <01>, so an unarmed machine never launches
# the dext at all and Apple's driver takes the card without a server timeout.
# The Start() gate stays as a second line. install.sh signs the dext on the target.
if [[ "$ARMING" == 1 ]]; then
  python3 - "$DEXT_PLIST" <<'PLIST'
import plistlib, sys
path = sys.argv[1]
armed = {"asfw-armed": b"\x01"}
with open(path, "rb") as f:
    plist = plistlib.load(f)
plist["IOKitPersonalities"]["ASFWDriverService"]["IOPropertyMatch"] = armed
with open(path, "wb") as f:
    plistlib.dump(plist, f)
with open(path, "rb") as f:
    assert plistlib.load(f)["IOKitPersonalities"]["ASFWDriverService"]["IOPropertyMatch"] == armed
PLIST
fi

# With a copy of the target's DriverKit dyld cache directory, refuse to package a
# dext that would die in dyld there (see tools/check-dext-symbols.sh).
if [[ -n "${ASFW_DYLD_CACHE:-}" ]]; then
  for arch in $(lipo -archs "$DEXT_BIN"); do
    tools/check-dext-symbols.sh "$DEXT_BIN" "$ASFW_DYLD_CACHE" "$arch" \
      || { echo "error: $arch dext has imports the runtime in $ASFW_DYLD_CACHE lacks" >&2; exit 1; }
  done
fi

VERSION=$(head -n 1 VERSION.txt | tr -d '[:space:]')
BUILD=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$APP/Contents/Info.plist")
COMMIT=$(git rev-parse --short HEAD)
DIRTY=""; git diff --quiet HEAD -- . ':!project.yml' ':!ASFWDriver/Info.plist' || DIRTY="-dirty"
NAME="ASFW-${VERSION}-${OS_NAME}-b${BUILD}"

OUT="$DIST_DIR/$OS_NAME"
STAGE="$OUT/$NAME"
rm -rf "$STAGE" "$OUT/$NAME.zip"
mkdir -p "$STAGE/Entitlements"
ditto --rsrc --extattr --acl "$APP" "$STAGE/ASFW.app"
install -m 0755 install.sh "$STAGE/install.sh"
install -m 0644 INSTALL.md "$STAGE/INSTALL.md"
install -m 0644 ASFW/App.entitlements "$STAGE/Entitlements/App.entitlements"
install -m 0644 ASFWDriver/ASFWDriver.entitlements "$STAGE/Entitlements/ASFWDriver.entitlements"
AGENT_BIN="build/DerivedData/Build/Products/Release/asfw-config-agent"
[[ -f "$AGENT_BIN" ]] || { echo "error: $AGENT_BIN not built" >&2; exit 1; }
install -m 0755 "$AGENT_BIN" "$STAGE/asfw-config-agent"
install -m 0644 ASFWConfigAgent/ConfigAgent.entitlements "$STAGE/Entitlements/ConfigAgent.entitlements"
# IOPCIDevice refuses a dext the match on a controller marked "built-in" unless
# transport.pci also lists com.apple.developer.driverkit.builtin as a string.
# AppleACPIPlatform marks every PCI device that has an ACPI node, which VMware's
# DSDT gives to all of them. Only install.sh's ad-hoc signature uses this copy.
# The string must come BEFORE the match dictionary: IOPCIFamily stops scanning
# the array at the first dictionary that matches (verified on macOS 12, b46).
STAGED_ENT="$STAGE/Entitlements/ASFWDriver.entitlements"
/usr/libexec/PlistBuddy \
  -c 'Add :com.apple.developer.driverkit.transport.pci:0 string com.apple.developer.driverkit.builtin' \
  "$STAGED_ENT"
[[ "$(/usr/libexec/PlistBuddy -c 'Print :com.apple.developer.driverkit.transport.pci:0' "$STAGED_ENT")" == com.apple.developer.driverkit.builtin \
   && "$(/usr/libexec/PlistBuddy -c 'Print :com.apple.developer.driverkit.transport.pci:1:IOPCIPrimaryMatch' "$STAGED_ENT")" == "$match" ]] \
  || { echo "error: packaged entitlement order is not (builtin, match dict)" >&2; exit 1; }

# Local installs are ad-hoc signed, so app and dext share no Team ID, and the
# App Sandbox then refuses the app's DriverKit user client (sandboxd:
# deny iokit-open-user-client IOUserUserClient; b62 report §6) even with
# userclient-access naming the dext. Two measures, either sufficient: the
# packaged app entitlements drop the sandbox (verified on the target), and the
# packaged dext entitlements allow any user client (xnu's fallback when the
# client's Team ID check fails). Signed builds from Xcode keep both as they are.
/usr/libexec/PlistBuddy -c 'Delete :com.apple.security.app-sandbox' "$STAGE/Entitlements/App.entitlements"
/usr/libexec/PlistBuddy -c 'Add :com.apple.developer.driverkit.allow-any-userclient-access bool true' "$STAGED_ENT"
[[ "$(/usr/libexec/PlistBuddy -c 'Print :com.apple.security.app-sandbox' "$STAGE/Entitlements/App.entitlements" 2>&1)" == *"Does Not Exist"* ]] \
  || { echo "error: app-sandbox still in the packaged app entitlements" >&2; exit 1; }

{
  echo "name:        $NAME"
  echo "built:       $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
  echo "commit:      $(git rev-parse HEAD)${DIRTY}"
  echo "branch:      $(git rev-parse --abbrev-ref HEAD)"
  echo "macOS min:   $MACOS"
  echo "DriverKit:   $DRIVERKIT"
  echo "app:         $APP_KIND"
  echo "arming:      $([[ "$ARMING" == 1 ]] && echo "required (asfw-armed = <01> on the controller; IOPropertyMatch + Start gate)" || echo off)"
  echo "entitlements: transport.pci = (builtin, {0x11c1, 0x104c}) - string first; allow-any-userclient-access; app without app-sandbox"
  echo "dma:         slabs are scattered pages translated per page run (no kmem_alloc_contig); AR buffers 4 KiB; contiguous requests capped at 64 KiB"
  echo "dma mapping: cached/coherent on x86_64 (kIOMemoryMapCacheModeDefault), cache-inhibited on arm64"
  echo "dma reach:   32-bit (OHCI); every prepared buffer is probed through PerformOperation and Start fails with instructions if the kernel bounces it (pages above 4 GiB without an IOMMU)"
  echo "agent:       asfw-config-agent (bundle-less login agent for remembered device settings)"
  echo "start:       30 s watchdog on its own queue ends the process if Start() is stuck in a kernel RPC (IOKit then falls back to AppleFWOHCI)"
  echo "interrupts:  MSI-X -> MSI -> INTx"
  echo "symbols:     $([[ -n "${ASFW_DYLD_CACHE:-}" ]] && echo "checked against $ASFW_DYLD_CACHE" || echo "not checked against a target cache")"
  echo "archs:       $(lipo -archs "$DEXT_BIN")"
  echo "xcode:       $(xcodebuild -version | tr '\n' ' ')"
  echo "driverkit sdk: $(xcrun --sdk driverkit --show-sdk-version)"
} > "$STAGE/BUILDINFO.txt"

(cd "$OUT" && ditto -c -k --keepParent "$NAME" "$NAME.zip" && rm -rf "$NAME")
# Only the newest build per OS is kept; superseded ones only confuse testing.
find "$OUT" -maxdepth 1 -name 'ASFW-*.zip' ! -name "$NAME.zip" -print -delete | sed 's|^|== Pruned |'
(cd "$OUT" && shasum -a 256 "$NAME.zip" > SHA256SUMS)

echo "== Packaged $OUT/$NAME.zip (${COMMIT}${DIRTY})"
