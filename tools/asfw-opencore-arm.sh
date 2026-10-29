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
#   sudo ./asfw-opencore-arm.sh --disarm            remove asfw-armed (and asfw-dma-exclude) from every path
#   options:
#     --esp diskXsY      use this EFI partition (default: the ESP on the boot disk)
#     --exclude RANGES   also set asfw-dma-exclude = "first-last[,first-last]" (hex bus addresses the
#                        driver must not use for DMA, e.g. a passthrough bridge window in a VM)
#     --trace            add dk=0x3 to boot-args: the kernel prints the driver's start-up steps on
#                        the console and serial port (DriverKit stays enabled: bit 0x1)
#     --no-trace         remove the dk= boot-arg again
#     --vtd              also set Kernel → Quirks → DisableIoMapper = false (experimental: with AppleVTD
#                        active the armed boot froze on one Z390 machine, b170)
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

MODE=arm; ESP=""; VTD=0; DRY=0; EXCLUDE=""; TRACE=""
while [ $# -gt 0 ]; do
    case "$1" in
        --arm) MODE=arm ;;
        --disarm) MODE=disarm ;;
        --report) MODE=report ;;
        --esp) ESP="${2:-}"; shift ;;
        --exclude) EXCLUDE="${2:-}"; shift ;;
        --trace) TRACE=on ;;
        --no-trace) TRACE=off ;;
        --vtd) VTD=1 ;;
        --dry-run) DRY=1 ;;
        -h|--help) sed -n '2,27p' "$0"; exit 0 ;;
        *) echo "unknown option: $1 (see --help)"; exit 2 ;;
    esac
    shift
done
if [ -n "$EXCLUDE" ] && ! printf '%s' "$EXCLUDE" | grep -Eq '^ *(0[xX])?[0-9a-fA-F]{1,16}-(0[xX])?[0-9a-fA-F]{1,16}( *[, ] *(0[xX])?[0-9a-fA-F]{1,16}-(0[xX])?[0-9a-fA-F]{1,16})* *$'; then
    echo "--exclude wants first-last[,first-last] in hex, e.g. 0x90300000-0x903fffff"; exit 2
fi
if [ "$MODE" != report ] && [ "$DRY" = 0 ] && [ "$(id -u)" != 0 ]; then
    echo "Run with sudo (it mounts the EFI partition and edits config.plist)."; exit 2
fi

OCGUID=7C436110-AB2A-4BBB-A880-FE41995C9F82

# ------------------------------------------------------------ controllers
# Walk the IODeviceTree plane. Every node with class-code <10000c00> (serial bus,
# FireWire, OHCI) is a target; its path is PciRoot(<_UID of the PNP0A03/08 host
# bridge>) followed by Pci(dev,func) for each PCI device from the root port down,
# taken from the node names (RP05@1C → 0x1C,0x0; XHC@14,3 → 0x14,0x3).
bold "FireWire OHCI controllers"
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
if [ -z "$PATHS" ]; then bad "no FireWire OHCI (class 0x0C0010) found in the I/O Registry"; exit 1; fi
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
iomapper=$(plutil -extract Kernel.Quirks.DisableIoMapper raw -o - "$C" 2>/dev/null)
mem=$(( $(sysctl -n hw.memsize) / 1073741824 ))
vtd_live=$(ioreg -c AppleVTD 2>/dev/null | grep -c '<class AppleVTD')
while IFS=$'\t' read -r p n ids; do
    [ "$(armed_count "$p")" -gt 0 ] && ok "armed:   $p" || info "unarmed: $p"
    ex=$(plutil -extract "DeviceProperties.Add.$p.asfw-dma-exclude" raw -o - "$C" 2>/dev/null)
    [ -n "$ex" ] && info "         asfw-dma-exclude = $ex"
done <<< "$PATHS"
bootargs=$(plutil -extract "NVRAM.Add.$OCGUID.boot-args" raw -o - "$C" 2>/dev/null)
info "boot-args: ${bootargs:-<none>}"
if [ "$mem" -gt 4 ]; then
    if [ "$vtd_live" -gt 0 ]; then
        warn "$mem GiB RAM with AppleVTD active: an armed boot froze with VT-d on one machine (b170)."
        info "If it freezes, boot another OpenCore and run --disarm, or set DisableIoMapper=true."
    else
        ok "$mem GiB RAM, no IOMMU: the driver keeps every DMA page below 4 GiB itself (from b173)."
        info "If it logs that too few pages lie below 4 GiB, add the boot-arg maxmem=2048."
    fi
fi
plutil -extract ACPI.Delete xml1 -o - "$C" 2>/dev/null | grep -qi 'DMAR' && warn "config deletes an ACPI DMAR table: VT-d will not be available"

[ "$MODE" = report ] && exit 0

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
    if [ "$MODE" = arm ] && [ -n "$EXCLUDE" ]; then
        plutil -remove "DeviceProperties.Add.$p.asfw-dma-exclude" "$TMP" 2>/dev/null
        plutil -insert "DeviceProperties.Add.$p.asfw-dma-exclude" -string "$EXCLUDE" "$TMP" \
            && { ok "exclude $p  $EXCLUDE"; changed=1; }
    fi
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
if [ -n "$TRACE" ]; then
    old=$(plutil -extract "NVRAM.Add.$OCGUID.boot-args" raw -o - "$TMP" 2>/dev/null)
    new=$(printf '%s' "$old" | tr ' ' '\n' | grep -v '^dk=' | tr '\n' ' ' | sed 's/  */ /g; s/^ //; s/ $//')
    [ "$TRACE" = on ] && new="${new:+$new }dk=0x3"
    if [ "$new" != "$old" ]; then
        plutil -replace "NVRAM.Add.$OCGUID.boot-args" -string "$new" "$TMP" && { ok "boot-args → $new"; changed=1; }
        plutil -extract "NVRAM.Delete.$OCGUID" xml1 -o - "$TMP" 2>/dev/null | grep -q '<string>boot-args</string>' \
            || warn "NVRAM → Delete does not list boot-args: OpenCore may keep the old value in NVRAM"
    fi
fi
plutil -lint "$TMP" >/dev/null || { bad "edited config does not lint; nothing written"; rm -f "$TMP"; exit 1; }

if [ "$changed" = 0 ]; then ok "nothing to change"; rm -f "$TMP"; exit 0; fi
if [ "$DRY" = 1 ]; then
    bold "dry run — would write:"; diff <(plutil -convert xml1 -o - "$C") <(plutil -convert xml1 -o - "$TMP") | sed 's/^/        /'
    rm -f "$TMP"; exit 0
fi
bak="$C.bak-$(date +%Y%m%d-%H%M%S)-asfw-$MODE"
cp "$C" "$bak" && cp "$TMP" "$C" && rm -f "$TMP" || { bad "write failed; config unchanged (backup: $bak)"; exit 1; }
ok "written; backup $(basename "$bak")"
bold "Takes effect at the next boot. If that boot fails, boot another OpenCore (e.g. the installer stick) and run --disarm, or restore the backup."
