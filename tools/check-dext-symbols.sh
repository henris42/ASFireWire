#!/usr/bin/env bash
#
# check-dext-symbols.sh - List dext imports a DriverKit runtime does not provide.
#
# DriverKit headers carry almost no availability markup, so a dext built with a
# new SDK for an old deployment target can compile cleanly and still die in dyld
# ("Symbol missing"), which names only the first missing symbol. This resolves
# all of them at once against the target's own DriverKit shared cache, per
# library the way dyld does (see check-dext-symbols.py).
#
# On the target, copy the cache directory somewhere this Mac can read:
#   macOS 12:   /System/DriverKit/System/Library/dyld/
#   macOS 13+:  /System/Volumes/Preboot/Cryptexes/OS/System/DriverKit/System/Library/dyld/
#
#   tools/check-dext-symbols.sh <dext binary or .dext or .app or .zip> <cache dir or file> [arch]
#
# Exit status: 0 when every import resolves, 1 when some are missing.

set -euo pipefail

[[ $# -ge 2 ]] || { sed -n '3,16p' "$0" >&2; exit 2; }
INPUT="$1"; CACHE="$2"; ARCH="${3:-x86_64}"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="${ROOT}/build/dext-symbols"
mkdir -p "$WORK"

# Locate the dext executable inside whatever was given.
if [[ "$INPUT" == *.zip ]]; then
  rm -rf "$WORK/zip" && mkdir -p "$WORK/zip"
  ditto -x -k "$INPUT" "$WORK/zip"
  INPUT="$WORK/zip"
fi
if [[ -d "$INPUT" ]]; then
  DEXT=$(find "$INPUT" -path '*.dext/*' -type f -perm +111 -name 'net.asfw.driver' | head -n 1)
else
  DEXT="$INPUT"
fi
[[ -n "$DEXT" && -f "$DEXT" ]] || { echo "error: no dext executable found in $1" >&2; exit 2; }

# An x86_64 dext loads from the x86_64h cache on machines that have one.
if [[ -d "$CACHE" ]]; then
  CACHE_FILE="$CACHE/dyld_shared_cache_${ARCH}h"
  [[ -f "$CACHE_FILE" ]] || CACHE_FILE="$CACHE/dyld_shared_cache_${ARCH}"
  [[ -f "$CACHE_FILE" || "$ARCH" != arm64 ]] || CACHE_FILE="$CACHE/dyld_shared_cache_arm64e"
else
  CACHE_FILE="$CACHE"
fi
[[ -f "$CACHE_FILE" ]] || { echo "error: no dyld_shared_cache for $ARCH in $CACHE" >&2; exit 2; }

exec python3 -I "$ROOT/tools/check-dext-symbols.py" "$DEXT" "$CACHE_FILE" "$ARCH"
