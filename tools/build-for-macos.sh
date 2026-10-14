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
#
# The full app needs macOS 14. Below that, ASFW.app is ASFWLoader instead: the
# same bundle, entitlements and dext, with only activate/deactivate/status.

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
# The full app needs macOS 14; older releases get ASFWLoader in its place.
APP_KIND=full
[[ "${MACOS%%.*}" -ge 14 ]] || APP_KIND=loader

mkdir -p "$(dirname "$SPEC")"
python3 - "$SPEC" "$MACOS" "$DRIVERKIT" "$APP_KIND" <<'PY'
import re, sys
out, macos, driverkit, app_kind = sys.argv[1:]
s = open("project.yml").read()
s, n_mac = re.subn(r'(MACOSX_DEPLOYMENT_TARGET: )"[0-9.]+"', rf'\g<1>"{macos}"', s)
s, n_dk = re.subn(r'(DRIVERKIT_DEPLOYMENT_TARGET: )"[0-9.]+"', rf'\g<1>"{driverkit}"', s)
if n_mac == 0 or n_dk == 0:
    sys.exit("project.yml: deployment target keys not found")
if app_kind == "loader":
    # Only the installer pieces of the app, which need nothing past macOS 12.
    s, n_src = re.subn(r'(\n  ASFW:\n    type: application\n(?:.*\n)*?    sources:\n)(?:      #.*\n)?      - path: ASFW\n',
                       r'\1      - path: ASFWLoader\n'
                       r'      - path: ASFW/DriverInstallManager.swift\n'
                       r'      - path: ASFW/DriverExtensionRegistry.swift\n'
                       r'      - path: ASFW/Assets.xcassets\n'
                       r'      - path: ASFW/fwicons.icon\n', s, count=1)
    if n_src != 1:
        sys.exit("project.yml: ASFW app target sources not found")
    # The MCP SDK requires macOS 13 and the loader does not use it.
    s, n_pkg = re.subn(r'\n *- package: swift-sdk\n *product: MCP(?=\n)', '', s)
    if n_pkg == 0:
        sys.exit("project.yml: MCP package dependency not found")
open(out, "w").write(s)
PY

# Leave the committed project matching project.yml whatever happens below.
trap 'xcodegen generate --spec project.yml --quiet || echo "warning: could not restore ASFW.xcodeproj" >&2' EXIT

echo "== Building for macOS ${MACOS} / DriverKit ${DRIVERKIT} (app: ${APP_KIND})"
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
install -m 0644 ASFW/App.entitlements "$STAGE/Entitlements/App.entitlements"
install -m 0644 ASFWDriver/ASFWDriver.entitlements "$STAGE/Entitlements/ASFWDriver.entitlements"

{
  echo "name:        $NAME"
  echo "built:       $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
  echo "commit:      $(git rev-parse HEAD)${DIRTY}"
  echo "branch:      $(git rev-parse --abbrev-ref HEAD)"
  echo "macOS min:   $MACOS"
  echo "DriverKit:   $DRIVERKIT"
  echo "app:         $APP_KIND"
  echo "archs:       $(lipo -archs "$DEXT_BIN")"
  echo "xcode:       $(xcodebuild -version | tr '\n' ' ')"
  echo "driverkit sdk: $(xcrun --sdk driverkit --show-sdk-version)"
} > "$STAGE/BUILDINFO.txt"

(cd "$OUT" && ditto -c -k --keepParent "$NAME" "$NAME.zip" && rm -rf "$NAME")
(cd "$OUT" && { grep -v "  $NAME.zip\$" SHA256SUMS 2>/dev/null || true; shasum -a 256 "$NAME.zip"; } > SHA256SUMS.new && mv SHA256SUMS.new SHA256SUMS)

echo "== Packaged $OUT/$NAME.zip (${COMMIT}${DIRTY})"
