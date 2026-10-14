#!/usr/bin/env bash
#
# check-dext-symbols.sh - List dext imports a DriverKit runtime does not export.
#
# DriverKit headers carry almost no availability markup, so a dext built with a
# new SDK for an old deployment target can compile cleanly and still die in dyld
# ("Symbol missing"), which names only the first missing symbol. This checks all
# of them at once against the target's own DriverKit shared cache.
#
# On the target, copy the cache directory somewhere this Mac can read:
#   macOS 12:   /System/DriverKit/System/Library/dyld/
#   macOS 13+:  /System/Volumes/Preboot/Cryptexes/OS/System/DriverKit/System/Library/dyld/
#
#   tools/check-dext-symbols.sh <dext binary or .dext or .app or .zip> <cache dir or file> [arch]
#
# Exit status: 0 when every import resolves, 1 when some are missing.

set -euo pipefail

[[ $# -ge 2 ]] || { sed -n '3,15p' "$0" >&2; exit 2; }
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

if [[ -d "$CACHE" ]]; then
  CACHE_FILE="$CACHE/dyld_shared_cache_${ARCH}h"
  [[ -f "$CACHE_FILE" ]] || CACHE_FILE="$CACHE/dyld_shared_cache_${ARCH}"
else
  CACHE_FILE="$CACHE"
fi
[[ -f "$CACHE_FILE" ]] || { echo "error: no dyld_shared_cache_${ARCH}[h] in $CACHE" >&2; exit 2; }

# Xcode's extractor turns the cache back into ordinary Mach-O dylibs.
EXTRACTOR_BUNDLE=$(find "$(xcode-select -p)/Platforms" -name dsc_extractor.bundle -path '*/usr/lib/*' | head -n 1)
[[ -n "$EXTRACTOR_BUNDLE" ]] || { echo "error: dsc_extractor.bundle not found in Xcode" >&2; exit 2; }
EXTRACTOR="$WORK/dsc_extract"
if [[ ! -x "$EXTRACTOR" ]]; then
  cat > "$WORK/dsc_extract.c" <<'C'
#include <dlfcn.h>
#include <stdio.h>
typedef int (*extract_fn)(const char *cache, const char *dst, void (^progress)(unsigned, unsigned));
int main(int argc, char **argv) {
    if (argc != 4) { fprintf(stderr, "usage: dsc_extract bundle cache dst\n"); return 2; }
    void *h = dlopen(argv[1], RTLD_NOW);
    if (!h) { fprintf(stderr, "%s\n", dlerror()); return 1; }
    extract_fn fn = (extract_fn)dlsym(h, "dyld_shared_cache_extract_dylibs_progress");
    if (!fn) { fprintf(stderr, "extractor entry point missing\n"); return 1; }
    return fn(argv[2], argv[3], ^(unsigned c, unsigned t) { (void)c; (void)t; });
}
C
  xcrun clang -fblocks -o "$EXTRACTOR" "$WORK/dsc_extract.c"
fi

CACHE_ID=$(shasum "$CACHE_FILE" | cut -c1-12)
LIBS="$WORK/cache-$CACHE_ID"
if [[ ! -d "$LIBS" ]]; then
  "$EXTRACTOR" "$EXTRACTOR_BUNDLE" "$CACHE_FILE" "$LIBS.tmp" || { echo "error: extraction failed" >&2; exit 1; }
  mv "$LIBS.tmp" "$LIBS"
fi

# Every symbol the runtime exports, then every import of the dext.
find "$LIBS" -type f | while read -r lib; do
  # nm -m, not -gU: libSystem exports memcpy and friends as indirect symbols.
  nm -m -arch "$ARCH" "$lib" 2>/dev/null \
    | awk '!/\(undefined\)/ { for (i = 1; i < NF; i++) if ($i == "external") { print $(i + 1); break } }'
done | sort -u > "$WORK/exports.txt"
nm -u -arch "$ARCH" "$DEXT" | sort -u > "$WORK/imports.txt"

comm -23 "$WORK/imports.txt" "$WORK/exports.txt" > "$WORK/missing.txt"

echo "dext:    $DEXT ($ARCH, minos $(vtool -arch "$ARCH" -show-build "$DEXT" | awk '/minos/ { print $2; exit }'))"
echo "runtime: $CACHE_FILE ($(wc -l < "$WORK/exports.txt" | tr -d ' ') exports)"
echo "imports: $(wc -l < "$WORK/imports.txt" | tr -d ' '), missing: $(wc -l < "$WORK/missing.txt" | tr -d ' ')"
if [[ -s "$WORK/missing.txt" ]]; then
  echo
  while read -r sym; do
    printf '%s\n    %s\n' "$sym" "$(c++filt "$sym")"
  done < "$WORK/missing.txt"
  exit 1
fi
