#!/bin/bash
#
# asfw-opencore-arm.sh — arm (or disarm) ASFireWire in an OpenCore config.plist.
#
# ASFW's controller personality only matches a FireWire OHCI that carries
# `asfw-armed = <01>`. On an OpenCore machine that property comes from
# config.plist → DeviceProperties → Add → <PCI path of the OHCI>. This script finds
# every FireWire OHCI in the I/O Registry, builds its OpenCore device path, and adds
# or removes the property in the booted OpenCore's config.plist. It always makes a
# dated backup next to the config first.
#
#   sudo ./asfw-opencore-arm.sh --report            show controllers, paths and config state; change nothing
#   sudo ./asfw-opencore-arm.sh                     arm every FireWire OHCI found (same as --arm)
#   sudo ./asfw-opencore-arm.sh --disarm            remove asfw-armed (and asfw-log-level) from every path,
#                                                   also from armed paths that have no controller now
#   sudo ./asfw-opencore-arm.sh --follow            arm a controller whose PCI path has moved: only when a
#                                                   controller is unarmed and the config has an armed path with
#                                                   no controller at it. The old path stays armed. Exit status
#                                                   10: armed, reboot; 11: it moved and the write failed
#   sudo ./asfw-opencore-arm.sh --install-boot-check   run --follow at every boot (LaunchDaemon net.asfw.rearm):
#                                                   the PCI path changes when cards or NVMe drives are added
#                                                   or moved. Logs to /var/log/asfw/rearm.log and tells the
#                                                   logged-in user when one more reboot is needed
#   sudo ./asfw-opencore-arm.sh --remove-boot-check    remove it again
#   options:
#     --esp diskXsY      use this EFI partition (default: the ESP on the boot disk)
#     --vtd              also set Kernel → Quirks → DisableIoMapper = false. Needed with more than
#                        4 GiB of RAM (and VT-d on in the BIOS); without it, boot with maxmem=2048
#     --log-level SPEC   set asfw-log-level, the driver's log level from the first start step on:
#                        off, error, warn (the default), info, debug or trace, optionally followed
#                        by per-subsystem levels, e.g. "debug" or "info irm=debug isoch=trace".
#                        The app and the login agent can change the level later without a reboot.
#     --no-log-level     remove asfw-log-level again
#     --boot-trace [MS]  set asfw-boot-trace: the driver publishes every start step (and, in this
#                        mode, every register write, kernel call and poll) in the I/O Registry and
#                        pauses MS ms (default 100; 0 = no pause) after each, and the LaunchDaemon
#                        net.asfw.boot-trace writes them to /var/log/asfw/boot-trace-<boot>.log as they
#                        happen. After a hang, the file's last line is the last step that completed.
#     --no-boot-trace    remove asfw-boot-trace again
#     --manual-start     set asfw-start = manual: the driver claims the controller at boot and leaves
#                        it alone until `asfw-config-agent --bring-up`
#     --auto-start       remove asfw-start again
#     --path OCPATH      act on this OpenCore device path instead of the controllers found in the
#                        I/O Registry (repeatable), e.g. arm a slot while the card is out:
#                        --path 'PciRoot(0x0)/Pci(0x1C,0x4)/Pci(0x0,0x0)/Pci(0x0,0x0)'
#                        (--report prints the path of every controller present)
#     --keep-aspm        do not set pci-aspm-default = 0 on the controller's PCIe bridge. By default
#                        arming disables ASPM on the XIO2213B's link: its erratum #2 (TI SCPZ007A) hangs
#                        the system when the link leaves L1. IOPCIFamily applies the property to the
#                        bridge and its root port at enumeration; --report shows the current value.
#     --dry-run          show what would change, write nothing
#
# Takes effect at the next boot. To undo by hand, copy the backup over config.plist,
# or delete the asfw-armed key; the ESP is FAT and readable from any OS.

set -uo pipefail

bold() { printf '\033[1m%s\033[0m\n' "$*"; }
ok()   { printf '  \033[32mok\033[0m    %s\n' "$*"; }
warn() { printf '  \033[33mwarn\033[0m  %s\n' "$*"; }
bad()  { printf '  \033[31mFAIL\033[0m  %s\n' "$*"; }
info() { printf '        %s\n' "$*"; }
# plget KEYPATH FILE: the raw value at KEYPATH, or nothing. On macOS 12 plutil prints a
# missing key's error on stdout, so its exit status decides, not its output.
plget() { local v; v=$(plutil -extract "$1" raw -o - "$2" 2>/dev/null) || return 0; printf '%s' "$v"; }

REARM_LABEL=net.asfw.rearm
REARM_PLIST=/Library/LaunchDaemons/$REARM_LABEL.plist
REARM_SCRIPT=/usr/local/libexec/asfw-opencore-arm.sh
REARM_LOG=${ASFW_REARM_LOG:-/var/log/asfw/rearm.log}

MODE=arm; FOLLOW=0; ESP=""; VTD=0; DRY=0; LOGLEVEL=""; NO_LOGLEVEL=0; BOOTTRACE=""; NO_BOOTTRACE=0; START=""; NO_START=0; GIVEN=""; KEEP_ASPM=0
while [ $# -gt 0 ]; do
    case "$1" in
        --arm) MODE=arm ;;
        --disarm) MODE=disarm ;;
        --report) MODE=report ;;
        --follow) MODE=arm; FOLLOW=1 ;;
        --boot-check) MODE=bootcheck ;;
        --install-boot-check) MODE=installcheck ;;
        --remove-boot-check) MODE=removecheck ;;
        --esp) ESP="${2:-}"; shift ;;
        --log-level) LOGLEVEL="${2:-}"; shift ;;
        --no-log-level) NO_LOGLEVEL=1 ;;
        --boot-trace) if [[ "${2:-}" =~ ^[0-9]+$ ]]; then BOOTTRACE="$2"; shift; else BOOTTRACE=100; fi ;;
        --no-boot-trace) NO_BOOTTRACE=1 ;;
        --manual-start) START=manual ;;
        --auto-start) NO_START=1 ;;
        --path)
            if ! [[ "${2:-}" =~ ^PciRoot\(0x[0-9A-Fa-f]+\)(/Pci\(0x[0-9A-Fa-f]+,0x[0-9A-Fa-f]+\))+$ ]]; then
                echo "--path wants an OpenCore device path like PciRoot(0x0)/Pci(0x1C,0x4)/Pci(0x0,0x0)"; exit 2
            fi
            GIVEN+="$2"$'\t'"given"$'\t'$'\n'; shift ;;
        --vtd) VTD=1 ;;
        --keep-aspm) KEEP_ASPM=1 ;;
        --dry-run) DRY=1 ;;
        -h|--help) sed -n '2,55p' "$0"; exit 0 ;;
        *) echo "unknown option: $1 (see --help)"; exit 2 ;;
    esac
    shift
done
LEVELS='off|error|warn|info|debug|trace'
if [ -n "$LOGLEVEL" ] && ! grep -Eqi "^ *($LEVELS)( *[, ] *[a-z]+=($LEVELS))* *\$" <<< "$LOGLEVEL"; then
    echo "--log-level wants a level ($LEVELS), optionally followed by subsystem=level pairs, e.g. \"info irm=debug\""; exit 2
fi
if [ "$MODE" != report ] && [ "$DRY" = 0 ] && [ "$(id -u)" != 0 ]; then
    echo "Run with sudo (it mounts the EFI partition and edits config.plist)."; exit 2
fi

# ------------------------------------------------------------ boot check
# The LaunchDaemon runs --boot-check, which runs --follow and reports. A notification
# needs the logged-in user's session, so it waits for one (ten minutes at most): the
# console belongs to root and then to _windowserver until somebody has logged in, and
# a notification posted before that user's Notification Centre runs is lost.
if [ "$MODE" = installcheck ]; then
    self=$(cd "$(dirname "$0")" && pwd)/$(basename "$0")
    install -d -o root -g wheel -m 0755 "$(dirname "$REARM_SCRIPT")" /var/log/asfw || exit 1
    [ "$self" -ef "$REARM_SCRIPT" ] || install -o root -g wheel -m 0755 "$self" "$REARM_SCRIPT" || exit 1
    cat > "$REARM_PLIST" <<PLIST || exit 1
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>$REARM_LABEL</string>
  <key>ProgramArguments</key>
  <array>
    <string>/bin/bash</string>
    <string>$REARM_SCRIPT</string>
    <string>--boot-check</string>
  </array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><false/>
</dict>
</plist>
PLIST
    chown root:wheel "$REARM_PLIST"; chmod 0644 "$REARM_PLIST"
    ok "installed $REARM_PLIST and $REARM_SCRIPT"
    info "From the next boot on, a controller whose PCI path moved is armed again by itself"
    info "(log: $REARM_LOG). --remove-boot-check removes it."
    exit 0
fi
if [ "$MODE" = removecheck ]; then
    launchctl bootout "system/$REARM_LABEL" 2>/dev/null
    rm -f "$REARM_PLIST" "$REARM_SCRIPT" && ok "removed $REARM_PLIST and $REARM_SCRIPT"
    exit 0
fi
if [ "$MODE" = bootcheck ]; then
    mkdir -p "$(dirname "$REARM_LOG")"
    out=$(/bin/bash "$0" --follow 2>&1); rc=$?
    { date '+%F %T'; printf '%s\n' "$out" | sed -E $'s/\x1b\\[[0-9;]*m//g'; echo "exit status $rc"; } >> "$REARM_LOG"
    case "$rc" in
        10) msg="The FireWire controller moved to a new PCI path and is armed again. Restart once more to start ASFireWire." ;;
        11) msg="The FireWire controller moved to a new PCI path, and arming it again failed. See $REARM_LOG." ;;
        *) exit 0 ;;
    esac
    uid=0
    for _ in $(seq 120); do
        uid=$(id -u "$(stat -f %Su /dev/console)" 2>/dev/null || echo 0)
        [ "$uid" -ge 500 ] && pgrep -u "$uid" -x NotificationCenter >/dev/null && break
        uid=0; sleep 5
    done
    if [ "$uid" -ge 500 ]; then
        sleep 5
        launchctl asuser "$uid" sudo -u "#$uid" \
            osascript -e "display notification \"$msg\" with title \"ASFireWire\"" 2>/dev/null
        echo "notified uid $uid" >> "$REARM_LOG"
    else
        echo "nobody logged in within ten minutes; not notified" >> "$REARM_LOG"
    fi
    exit 0
fi

OCGUID=7C436110-AB2A-4BBB-A880-FE41995C9F82

# ------------------------------------------------------------ controllers
# Walk the IODeviceTree plane. Every node with class-code <10000c00> (serial bus,
# FireWire, OHCI) is a target; its path is PciRoot(<_UID of the PNP0A03/08 host
# bridge>) followed by Pci(dev,func) for each PCI device from the root port down,
# taken from the node names (RP05@1C → 0x1C,0x0; XHC@14,3 → 0x14,0x3).
bold "FireWire OHCI controllers"
if [ -n "$GIVEN" ]; then
    # The card need not be present: the paths come from the command line.
    PATHS=$(printf '%s' "$GIVEN" | sort -u)
else
PATHS=$(ioreg -p IODeviceTree -w0 -l | perl -e '
    my @stack; my $cur;
    while (<STDIN>) {
        if (/^([ |]*)\+-o (\S+)\s+<class (\w+)/) {
            my $depth = length($1);
            pop @stack while @stack && $stack[-1]{depth} >= $depth;
            $cur = { depth => $depth, name => $2, class => $3, props => {} };
            push @stack, $cur;
            next;
        }
        next unless $cur;
        if (/"([^"]+)" = (.*)$/) { $cur->{props}{$1} = $2; }
        if (/"class-code" = <10000c00>/) {
            my @chain = @stack; my @parts; my $root;
            for my $n (reverse @chain) {
                if (($n->{props}{compatible} // "") =~ /PNP0A0[38]/ || ($n->{props}{name} // "") =~ /PNP0A0[38]/) { $root = $n; last; }
                if ($n->{class} eq "IOPCIDevice" && $n->{name} =~ /@([0-9A-Fa-f]+)(?:,([0-9A-Fa-f]+))?$/) {
                    unshift @parts, sprintf("Pci(0x%X,0x%X)", hex($1), hex($2 // "0"));
                }
            }
            my $uid = 0;
            if ($root && defined $root->{props}{_UID}) { ($uid) = $root->{props}{_UID} =~ /(\d+)/; $uid //= 0; }
            my $ids = ($cur->{props}{"vendor-id"} // "") . " " . ($cur->{props}{"device-id"} // "");
            print "PciRoot(0x" . sprintf("%X", $uid) . ")/" . join("/", @parts) . "\t$cur->{name}\t$ids\n" if @parts;
        }
    }')
fi
if [ -z "$PATHS" ]; then bad "no FireWire OHCI (class 0x0C0010) found in the I/O Registry (pass --path to arm an empty slot)"; exit 1; fi
while IFS=$'\t' read -r p n ids; do ok "$p   ($n)"; done <<< "$PATHS"

# ------------------------------------------------------------ config.plist
if [ -z "$ESP" ]; then
    store=$(diskutil info / 2>/dev/null | awk -F': *' '/APFS Physical Store/{print $2; exit}')
    disk=${store%s*}
    ESP=$(diskutil list "$disk" 2>/dev/null | awk '/EFI/ {print $NF; exit}')
    [ -z "$ESP" ] && { bad "could not find the EFI partition of the boot disk ($disk); pass --esp diskXsY"; exit 1; }
fi
diskutil info "$ESP" >/dev/null 2>&1 || { bad "no such disk: $ESP (diskutil list shows the EFI partitions)"; exit 1; }
bold "OpenCore config on $ESP"
mp=$(diskutil info "$ESP" | awk -F': *' '/Mount Point/{print $2}')
MOUNTED_HERE=0
if [ -z "$mp" ]; then
    if [ "$MODE" = report ] || [ "$DRY" = 1 ]; then diskutil mount readOnly "$ESP" >/dev/null || sudo diskutil mount readOnly "$ESP" >/dev/null
    else diskutil mount "$ESP" >/dev/null; fi
    mp=$(diskutil info "$ESP" | awk -F': *' '/Mount Point/{print $2}'); MOUNTED_HERE=1
fi
cleanup() { [ "$MOUNTED_HERE" = 1 ] && { sync; diskutil unmount "$mp" >/dev/null 2>&1; }; }
trap cleanup EXIT
C="$mp/EFI/OC/config.plist"
[ -f "$C" ] || { bad "no $C — is this an OpenCore ESP? (pass --esp)"; exit 1; }
info "$C"

armed_count() { plutil -extract "DeviceProperties.Add.$1" xml1 -o - "$C" 2>/dev/null | grep -c '<key>asfw-armed</key>'; }
ver=$(nvram 4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102:opencore-version 2>/dev/null | awk '{print $2}')
info "running OpenCore: ${ver:-unknown}"
iomapper=$(plget Kernel.Quirks.DisableIoMapper "$C")
mem=$(( $(sysctl -n hw.memsize) / 1073741824 ))
vtd_live=$(ioreg -c AppleVTD 2>/dev/null | grep -c '<class AppleVTD')
while IFS=$'\t' read -r p n ids; do
    [ "$(armed_count "$p")" -gt 0 ] && ok "armed:   $p" || info "unarmed: $p"
    level=$(plget "DeviceProperties.Add.$p.asfw-log-level" "$C")
    [ -n "$level" ] && info "         asfw-log-level = $level"
    trace=$(plget "DeviceProperties.Add.$p.asfw-boot-trace" "$C")
    [ -n "$trace" ] && info "         asfw-boot-trace = $trace ms per step (lockstep trace to /var/log/asfw)"
    start=$(plget "DeviceProperties.Add.$p.asfw-start" "$C")
    [ -n "$start" ] && info "         asfw-start = $start"
    bridge="${p%/*}"
    aspm=$(plget "DeviceProperties.Add.$bridge.pci-aspm-default" "$C")
    if [ -n "$aspm" ]; then info "         bridge $bridge: pci-aspm-default = $aspm (ASPM off; XIO2213B erratum #2)"
    else warn "bridge $bridge: no pci-aspm-default in the config; L1 stays as the firmware left it (XIO2213B erratum #2: hang on L1 exit)"; fi
done <<< "$PATHS"
[ -f "$REARM_PLIST" ] && info "boot check installed ($REARM_PLIST): a controller whose path moved is armed again at boot"
# Paths in the config that carry asfw-* properties but are not in the list above:
# a card that was pulled, a slot armed with --path, or where a controller was before
# the PCI layout changed.
OTHERS=$(plutil -extract DeviceProperties.Add xml1 -o - "$C" 2>/dev/null | perl -e '
    my $depth = 0; my $cur = ""; my %paths;
    while (<STDIN>) {
        if ($depth == 1 && m{<key>(PciRoot[^<]*)</key>}) { $cur = $1; $paths{$cur} = ""; }
        if ($depth == 2 && $cur ne "" && m{<key>(asfw-[^<]+)</key>}) { $paths{$cur} .= " $1"; }
        $depth += () = /<dict>/g; $depth -= () = m{</dict>}g;
    }
    for (sort keys %paths) { print "$_\t$paths{$_}\n" if $paths{$_} ne ""; }' |
while IFS=$'\t' read -r p props; do
    grep -qF "$p"$'\t' <<< "$PATHS" || printf '%s\t%s\n' "$p" "$props"
done)
STALE_ARMED=""; UNARMED=""
while IFS=$'\t' read -r p props; do
    [ -z "$p" ] && continue
    warn "no controller at $p; config has:$props"
    [[ "$props " == *" asfw-armed "* ]] && STALE_ARMED+="$p"$'\n'
done <<< "$OTHERS"
while IFS=$'\t' read -r p n ids; do
    [ "$(armed_count "$p")" -gt 0 ] || UNARMED+="$p"$'\n'
done <<< "$PATHS"
if [ -n "$UNARMED" ] && [ -n "$STALE_ARMED" ]; then
    warn "an unarmed controller and an armed path without one: the PCI path has probably moved (--follow arms the new path)"
fi
bootargs=$(plget "NVRAM.Add.$OCGUID.boot-args" "$C")
info "boot-args: ${bootargs:-<none>}"
if [ "$mem" -gt 4 ]; then
    if [ "$vtd_live" -gt 0 ]; then
        ok "$mem GiB RAM, VT-d active (AppleVTD): the controller's DMA is remapped below 4 GiB."
    else
        warn "$mem GiB RAM and no IOMMU: the driver will refuse to start (its DMA would be bounced)."
        info "Run with --vtd (and turn VT-d on in the BIOS), or add the boot-arg maxmem=2048."
    fi
fi
acpi_delete=$(plutil -extract ACPI.Delete xml1 -o - "$C" 2>/dev/null || true)
grep -qi 'DMAR' <<< "$acpi_delete" && warn "config deletes an ACPI DMAR table: VT-d will not be available"

[ "$MODE" = report ] && exit 0
if [ "$FOLLOW" = 1 ]; then
    [ -z "$UNARMED" ] && { ok "every controller present is armed"; exit 0; }
    if [ -z "$STALE_ARMED" ]; then
        info "an unarmed controller, and no armed path without a controller: not a move, nothing done"; exit 0
    fi
fi
# --disarm means every path, also the armed ones no controller sits at now.
if [ "$MODE" = disarm ] && [ -z "$GIVEN" ] && [ -n "$STALE_ARMED" ]; then
    PATHS+=$'\n'$(sed -e '/^$/d' -e $'s/$/\tgone\t/' <<< "$STALE_ARMED")
fi
# In the change section a failure after a detected move has its own exit status.
give_up() { [ "$FOLLOW" = 1 ] && exit 11; exit 1; }

# ------------------------------------------------------------ change
TMP=$(mktemp /tmp/asfw-oc.XXXXXX); cp "$C" "$TMP"
changed=0
while IFS=$'\t' read -r p n ids; do
    has=$(armed_count "$p")
    if [ "$MODE" = arm ] && [ "$has" = 0 ]; then
        if plutil -extract "DeviceProperties.Add.$p" xml1 -o - "$TMP" >/dev/null 2>&1; then
            plutil -insert "DeviceProperties.Add.$p.asfw-armed" -data AQ== "$TMP"
        else
            plutil -insert "DeviceProperties.Add.$p" -xml '<dict><key>asfw-armed</key><data>AQ==</data></dict>' "$TMP"
        fi && { ok "arm     $p"; changed=1; }
    fi
    level=$(plget "DeviceProperties.Add.$p.asfw-log-level" "$TMP")
    if [ "$MODE" = arm ] && [ -n "$LOGLEVEL" ] && [ "$LOGLEVEL" != "$level" ]; then
        [ -n "$level" ] && plutil -remove "DeviceProperties.Add.$p.asfw-log-level" "$TMP"
        plutil -insert "DeviceProperties.Add.$p.asfw-log-level" -string "$LOGLEVEL" "$TMP" \
            && { ok "log level  $p  $LOGLEVEL"; changed=1; }
    fi
    if { [ "$MODE" = disarm ] || [ "$NO_LOGLEVEL" = 1 ]; } && [ -n "$level" ]; then
        plutil -remove "DeviceProperties.Add.$p.asfw-log-level" "$TMP" && { ok "remove asfw-log-level  $p"; changed=1; }
    fi
    trace=$(plget "DeviceProperties.Add.$p.asfw-boot-trace" "$TMP")
    if [ "$MODE" = arm ] && [ -n "$BOOTTRACE" ] && [ "$BOOTTRACE" != "$trace" ]; then
        [ -n "$trace" ] && plutil -remove "DeviceProperties.Add.$p.asfw-boot-trace" "$TMP"
        plutil -insert "DeviceProperties.Add.$p.asfw-boot-trace" -integer "$BOOTTRACE" "$TMP" \
            && { ok "boot trace  $p  $BOOTTRACE ms per step"; changed=1; }
    fi
    if { [ "$MODE" = disarm ] || [ "$NO_BOOTTRACE" = 1 ]; } && [ -n "$trace" ]; then
        plutil -remove "DeviceProperties.Add.$p.asfw-boot-trace" "$TMP" && { ok "remove asfw-boot-trace  $p"; changed=1; }
    fi
    start=$(plget "DeviceProperties.Add.$p.asfw-start" "$TMP")
    if [ "$MODE" = arm ] && [ -n "$START" ] && [ "$START" != "$start" ]; then
        [ -n "$start" ] && plutil -remove "DeviceProperties.Add.$p.asfw-start" "$TMP"
        plutil -insert "DeviceProperties.Add.$p.asfw-start" -string "$START" "$TMP" \
            && { ok "start  $p  $START"; changed=1; }
    fi
    if { [ "$MODE" = disarm ] || [ "$NO_START" = 1 ]; } && [ -n "$start" ]; then
        plutil -remove "DeviceProperties.Add.$p.asfw-start" "$TMP" && { ok "remove asfw-start  $p"; changed=1; }
    fi
    # ASPM off on the controller's PCIe bridge (the path one level up): XIO2213B
    # erratum #2. Removed on disarm only if it is the only key there.
    bridge="${p%/*}"
    aspm=$(plget "DeviceProperties.Add.$bridge.pci-aspm-default" "$TMP")
    if [ "$MODE" = arm ] && [ "$KEEP_ASPM" = 0 ] && [ "$aspm" != "AAAAAA==" ] && [ "$aspm" != "0" ]; then
        [ -n "$aspm" ] && plutil -remove "DeviceProperties.Add.$bridge.pci-aspm-default" "$TMP"
        if plutil -extract "DeviceProperties.Add.$bridge" xml1 -o - "$TMP" >/dev/null 2>&1; then
            plutil -insert "DeviceProperties.Add.$bridge.pci-aspm-default" -data AAAAAA== "$TMP"
        else
            plutil -insert "DeviceProperties.Add.$bridge" -xml '<dict><key>pci-aspm-default</key><data>AAAAAA==</data></dict>' "$TMP"
        fi && { ok "ASPM off  $bridge  (pci-aspm-default = 0)"; changed=1; }
    fi
    if [ "$MODE" = disarm ] && [ -n "$aspm" ]; then
        plutil -remove "DeviceProperties.Add.$bridge.pci-aspm-default" "$TMP" && { ok "remove pci-aspm-default  $bridge"; changed=1; }
        [ "$(plutil -extract "DeviceProperties.Add.$bridge" xml1 -o - "$TMP" | grep -c '<key>')" = 0 ] && plutil -remove "DeviceProperties.Add.$bridge" "$TMP"
    fi
    # The DMA exclusion property is gone from the driver (b178); old configs lose it on disarm.
    if [ "$MODE" = disarm ] && plutil -extract "DeviceProperties.Add.$p.asfw-dma-exclude" raw -o - "$TMP" >/dev/null 2>&1; then
        plutil -remove "DeviceProperties.Add.$p.asfw-dma-exclude" "$TMP" && { ok "remove asfw-dma-exclude  $p"; changed=1; }
    fi
    if [ "$MODE" = disarm ] && [ "$has" -gt 0 ]; then
        plutil -remove "DeviceProperties.Add.$p.asfw-armed" "$TMP" && { ok "disarm  $p"; changed=1; }
        # drop the now-empty path entry so the config stays tidy
        [ "$(plutil -extract "DeviceProperties.Add.$p" xml1 -o - "$TMP" | grep -c '<key>')" = 0 ] && plutil -remove "DeviceProperties.Add.$p" "$TMP"
    fi
done <<< "$PATHS"
if [ "$VTD" = 1 ] && [ "$iomapper" != false ]; then
    plutil -replace Kernel.Quirks.DisableIoMapper -bool NO "$TMP" && { ok "DisableIoMapper → false"; changed=1; }
fi
plutil -lint "$TMP" >/dev/null || { bad "edited config does not lint; nothing written"; rm -f "$TMP"; give_up; }

if [ "$changed" = 0 ]; then ok "nothing to change"; rm -f "$TMP"; exit 0; fi
if [ "$DRY" = 1 ]; then
    bold "dry run — would write:"; diff <(plutil -convert xml1 -o - "$C") <(plutil -convert xml1 -o - "$TMP") | sed 's/^/        /'
    rm -f "$TMP"; exit 0
fi
bak="$C.bak-$(date +%Y%m%d-%H%M%S)-asfw-$MODE"
cp "$C" "$bak" && cp "$TMP" "$C" && rm -f "$TMP" || { bad "write failed; config unchanged (backup: $bak)"; give_up; }
ok "written; backup $(basename "$bak")"
bold "Takes effect at the next boot. If that boot fails, boot another OpenCore (e.g. the installer stick) and run --disarm, or restore the backup."
[ "$FOLLOW" = 1 ] && exit 10
exit 0
