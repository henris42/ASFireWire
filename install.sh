#!/bin/zsh
#
# ASFireWire – local installer for a release bundle.
#
# The release ZIP deliberately contains an unsigned ASFW.app. This script runs on
# the installer's Mac, ad-hoc signs the dext first and the app second, then installs
# the result in /Applications. No Apple Developer account or provisioning profile
# is required for this experimental local-install path.
#
# Every run converges on "installed": it checks each step and only does what is
# missing, so running it again after an approval prompt or a reboot just carries
# on where the last run stopped.

emulate -L zsh
setopt ERR_EXIT NO_UNSET PIPE_FAIL NO_NOMATCH

SCRIPT_DIR="${0:A:h}"
APP_PATH="${SCRIPT_DIR}/ASFW.app"
INSTALL_DIR="${ASFW_INSTALL_DIR:-/Applications}"
INSTALLED_APP="${INSTALL_DIR}/ASFW.app"
DEXT_ID="net.asfw.driver"
MODE=install
STEP=0

ENTITLEMENT_DIR="${SCRIPT_DIR}/Entitlements"
if [[ ! -d "$ENTITLEMENT_DIR" && -d "${SCRIPT_DIR}/ASFW" && -d "${SCRIPT_DIR}/ASFWDriver" ]]; then
  # Also allow the script to be exercised directly from the source checkout.
  ENTITLEMENT_DIR="${SCRIPT_DIR}"
  APP_ENTITLEMENTS="${SCRIPT_DIR}/ASFW/App.entitlements"
  DEXT_ENTITLEMENTS="${SCRIPT_DIR}/ASFWDriver/ASFWDriver.entitlements"
else
  APP_ENTITLEMENTS="${ENTITLEMENT_DIR}/App.entitlements"
  DEXT_ENTITLEMENTS="${ENTITLEMENT_DIR}/ASFWDriver.entitlements"
fi

usage() {
  cat <<'EOF'
Usage: ./install.sh [--status | --uninstall]

Install or update ASFW.app from this folder. Safe to run again at any point:
  1. Check that SIP is disabled.
  2. Enable system-extension developer mode, if it is off.
  3. Sign and copy the app to /Applications, unless that build is already there.
  4. Activate the driver (headless on macOS 12/13; via the app's Install button
     on the full app).
  5. Report the driver state and the one thing left to do, if anything:
     approve it in System Preferences, or reboot, then run this again.

Options:
  --status     Only report: developer mode, installed build, driver state.
  --uninstall  Deactivate the driver and remove /Applications/ASFW.app.

Environment override for advanced users:
  ASFW_INSTALL_DIR        Destination directory (default: /Applications).

Read INSTALL.md first: arming the controller, what each NEXT: line means,
and how to recover a Mac that hangs.
EOF
}

info() {
  print -r -- "[ASFW] $*"
}

step() {
  STEP=$((STEP + 1))
  print -r -- ""
  print -r -- "[ASFW] Step ${STEP}: $*"
}

error() {
  print -u2 -r -- "[ASFW] ERROR: $*"
}

fail() {
  error "$*"
  exit 1
}

require_command() {
  command -v "$1" >/dev/null 2>&1 || fail "Required macOS command '$1' was not found. Install or update Xcode, then try again."
}

run_with_privilege() {
  if [[ -w "$INSTALL_DIR" ]]; then
    "$@"
  else
    info "Administrator permission is required for $INSTALL_DIR. macOS will ask for your password."
    sudo "$@"
  fi
}

read_plist_value() {
  local plist_path="$1"
  local plist_key="$2"
  /usr/libexec/PlistBuddy -c "Print :${plist_key}" "$plist_path" 2>/dev/null || true
}

# The loader build (macOS 12/13) activates from the command line; the full app
# only through its window.
is_loader_app() {
  local min_os
  min_os="$(read_plist_value "$1/Contents/Info.plist" "LSMinimumSystemVersion")"
  [[ -n "$min_os" && "${min_os%%.*}" -lt 14 ]]
}

developer_mode_on() {
  systemextensionsctl developer 2>/dev/null | grep -qi 'is on'
}

# The driver's lines from `systemextensionsctl list` (one per staged version).
driver_lines() {
  systemextensionsctl list 2>/dev/null | grep -F "$DEXT_ID" || true
}

# Turns the driver's system-extension state into the next action, and returns
# 0 when nothing is left to do.
report_driver_state() {
  local lines
  lines="$(driver_lines)"
  print -r -- ""
  if [[ -z "$lines" ]]; then
    info "Driver state: not installed as a system extension."
    return 1
  fi
  info "Driver state (systemextensionsctl list):"
  print -r -- "$lines" | sed 's/^/    /'
  if print -r -- "$lines" | grep -q 'waiting to uninstall on reboot'; then
    info "NEXT: reboot, then run ./install.sh again. An old build is removed at reboot."
    return 1
  fi
  if print -r -- "$lines" | grep -q 'activated waiting for user'; then
    info "NEXT: allow the extension in System Preferences > Security & Privacy > General,"
    info "      then run ./install.sh again."
    return 1
  fi
  if print -r -- "$lines" | grep -q 'activated enabled'; then
    info "The driver is activated and enabled."
    return 0
  fi
  info "NEXT: unusual state; reboot and run ./install.sh again, or ./install.sh --uninstall."
  return 1
}

while (( $# > 0 )); do
  case "$1" in
    --status) MODE=status; shift ;;
    --uninstall) MODE=uninstall; shift ;;
    --yes) shift ;; # accepted for compatibility; replacing the app no longer asks
    -h|--help) usage; exit 0 ;;
    *) usage >&2; fail "Unknown option: $1" ;;
  esac
done

require_command csrutil
require_command systemextensionsctl
require_command codesign
require_command xattr
require_command ditto
require_command find
require_command open
[[ -x /usr/libexec/PlistBuddy ]] || fail "Required macOS command '/usr/libexec/PlistBuddy' was not found."

if [[ "$MODE" == status ]]; then
  developer_mode_on && info "Developer mode: on" || info "Developer mode: off"
  if [[ -d "$INSTALLED_APP" ]]; then
    info "Installed app: ${INSTALLED_APP} (build $(read_plist_value "$INSTALLED_APP/Contents/Info.plist" CFBundleVersion))"
    if is_loader_app "$INSTALLED_APP"; then
      "$INSTALLED_APP/Contents/MacOS/ASFW" --status || true
    fi
  else
    info "Installed app: none"
  fi
  report_driver_state || true
  exit 0
fi

if [[ "$MODE" == uninstall ]]; then
  step "Deactivate the driver"
  if [[ -d "$INSTALLED_APP" ]] && is_loader_app "$INSTALLED_APP"; then
    "$INSTALLED_APP/Contents/MacOS/ASFW" --deactivate || true
  fi
  if [[ -n "$(driver_lines)" ]]; then
    # With SIP disabled systemextensionsctl can remove it directly.
    systemextensionsctl uninstall - "$DEXT_ID" || true
  fi
  step "Remove ${INSTALLED_APP}"
  if [[ -e "$INSTALLED_APP" ]]; then
    run_with_privilege rm -rf "$INSTALLED_APP"
    info "Removed."
  else
    info "Not installed."
  fi
  if [[ -n "$(driver_lines)" ]]; then
    report_driver_state || true
    info "Some state remains until the next reboot."
  else
    info "Uninstalled."
  fi
  exit 0
fi

[[ -d "$APP_PATH" ]] || fail "ASFW.app was not found next to this script. Extract the complete release ZIP and run ./install.sh from the extracted folder."
[[ -f "$APP_ENTITLEMENTS" ]] || fail "The release bundle is incomplete: missing the app entitlement file."
[[ -f "$DEXT_ENTITLEMENTS" ]] || fail "The release bundle is incomplete: missing the DriverKit entitlement file."

DEXT_PATH="$(find "$APP_PATH/Contents/Library/SystemExtensions" -maxdepth 1 -type d -name '*.dext' -print -quit 2>/dev/null || true)"
[[ -n "$DEXT_PATH" ]] || fail "The release bundle does not contain a DriverKit extension under ASFW.app/Contents/Library/SystemExtensions."

APP_INFO_PLIST="${APP_PATH}/Contents/Info.plist"
# A .dext is itself the bundle; unlike the enclosing app, its Info.plist is at
# the bundle root rather than under a nested Contents directory.
DEXT_INFO_PLIST="${DEXT_PATH}/Info.plist"
[[ -f "$APP_INFO_PLIST" && -f "$DEXT_INFO_PLIST" ]] || fail "The app or dext is missing its Info.plist; the release bundle may be damaged."

APP_BUNDLE_ID="$(read_plist_value "$APP_INFO_PLIST" "CFBundleIdentifier")"
DEXT_BUNDLE_ID="$(read_plist_value "$DEXT_INFO_PLIST" "CFBundleIdentifier")"
NEW_BUILD="$(read_plist_value "$APP_INFO_PLIST" "CFBundleVersion")"
[[ -n "$APP_BUNDLE_ID" && -n "$DEXT_BUNDLE_ID" ]] || fail "Could not read the app and dext bundle identifiers."

step "Check macOS security settings"
SIP_STATUS="$(csrutil status 2>&1 || true)"
if ! print -r -- "$SIP_STATUS" | grep -qi 'disabled'; then
  error "System Integrity Protection is not disabled."
  print -u2 -r -- "Boot into macOS Recovery, open Terminal, run 'csrutil disable', reboot, and run this installer again."
  exit 1
fi
info "SIP is disabled, so this experimental DriverKit installation can continue."

step "System-extension developer mode"
if developer_mode_on; then
  info "Already on."
else
  systemextensionsctl developer on \
    || fail "Could not enable system-extension developer mode. Run 'systemextensionsctl developer on' in Terminal and try again."
  info "Enabled."
fi

step "Install build ${NEW_BUILD} in ${INSTALL_DIR}"
[[ -d "$INSTALL_DIR" ]] || fail "Install destination does not exist: ${INSTALL_DIR}"
INSTALLED_BUILD=""
if [[ -d "$INSTALLED_APP" ]]; then
  INSTALLED_BUILD="$(read_plist_value "$INSTALLED_APP/Contents/Info.plist" "CFBundleVersion")"
fi
if [[ "$INSTALLED_BUILD" == "$NEW_BUILD" ]] && codesign --verify --deep --strict "$INSTALLED_APP" 2>/dev/null; then
  info "Build ${NEW_BUILD} is already installed and correctly signed; not copying again."
else
  [[ -n "$INSTALLED_BUILD" ]] && info "Replacing installed build ${INSTALLED_BUILD}."

  if xattr -lr "$APP_PATH" 2>/dev/null | grep -q 'com.apple.quarantine'; then
    info "Removing the download quarantine flag."
    xattr -dr com.apple.quarantine "$APP_PATH" || fail "Could not remove the quarantine flag from ASFW.app."
  fi

  info "Ad-hoc signing the dext first: ${DEXT_BUNDLE_ID}"
  codesign --force --timestamp=none --sign - \
    --entitlements "$DEXT_ENTITLEMENTS" "$DEXT_PATH" \
    || fail "The DriverKit extension could not be ad-hoc signed."
  info "Ad-hoc signing the app second: ${APP_BUNDLE_ID}"
  codesign --force --timestamp=none --sign - \
    --entitlements "$APP_ENTITLEMENTS" "$APP_PATH" \
    || fail "The app could not be ad-hoc signed."

  codesign --verify --deep --strict --verbose=2 "$APP_PATH" \
    || fail "codesign verification failed for the signed ASFW.app."
  codesign -d --entitlements - --xml "$APP_PATH" 2>&1 | grep -q 'system-extension.install' \
    || fail "The signed app is missing com.apple.developer.system-extension.install."
  codesign -d --entitlements - --xml "$DEXT_PATH" 2>&1 | grep -q 'com.apple.developer.driverkit' \
    || fail "The signed dext is missing its DriverKit entitlements."

  if [[ -e "$INSTALLED_APP" ]]; then
    run_with_privilege rm -rf "$INSTALLED_APP"
  fi
  run_with_privilege ditto --rsrc --extattr --acl "$APP_PATH" "$INSTALLED_APP" \
    || fail "Could not copy the signed app to ${INSTALL_DIR}."
  codesign --verify --deep --strict "$INSTALLED_APP" \
    || fail "The installed app failed signature verification after copying."
  info "Installed."
fi

step "Activate the driver"
if is_loader_app "$INSTALLED_APP"; then
  # Needs the logged-in user's session. Over ssh, run as that user with:
  #   sudo launchctl asuser "$(id -u <user>)" /Applications/ASFW.app/Contents/MacOS/ASFW --activate
  if ! "$INSTALLED_APP/Contents/MacOS/ASFW" --activate; then
    info "Activation did not complete; see the state below."
  fi
else
  open "$INSTALLED_APP" || info "Open ${INSTALLED_APP} manually."
  info "Press Install in the app to activate the system extension, then run ./install.sh --status."
fi

if report_driver_state; then
  print -r -- ""
  info "Installation complete: build ${NEW_BUILD}."
  info "Keep SIP disabled while using this experimental DriverKit build."
fi
