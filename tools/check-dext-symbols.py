#!/usr/bin/env python3
"""Resolve a dext's imports against a DriverKit dyld shared cache, the way dyld does.

A dext is linked two-level: every import names the library it must come from.
Checking "is this symbol exported by anything in the cache" is not enough, and
was how b40 passed the check and still died in dyld: ___cxa_atexit is in
libsystem_c, but the SDK's libc++ TBD re-exports it so the dext binds it to
libc++, and the target's libc++ does not re-export it. Export tries in the
cache carry re-export entries that `nm` on extracted dylibs never shows, so
this reads the tries out of the raw cache file instead.

    check-dext-symbols.py <dext executable> <dyld_shared_cache file> <arch>

Exit status: 0 when every non-weak import resolves, 1 otherwise.
"""

import os
import struct
import subprocess
import sys

LC_SEGMENT_64 = 0x19
LC_ID_DYLIB = 0xD
LC_LOAD_DYLIB = 0xC
LC_LAZY_LOAD_DYLIB = 0x20
LC_LOAD_WEAK_DYLIB = 0x80000018
LC_REEXPORT_DYLIB = 0x8000001F
LC_LOAD_UPWARD_DYLIB = 0x80000023
LC_DYLD_INFO = 0x22
LC_DYLD_INFO_ONLY = 0x80000022
LC_DYLD_EXPORTS_TRIE = 0x80000033
DEPENDENCY_COMMANDS = {LC_LOAD_DYLIB, LC_LAZY_LOAD_DYLIB, LC_LOAD_WEAK_DYLIB,
                       LC_REEXPORT_DYLIB, LC_LOAD_UPWARD_DYLIB}
EXPORT_SYMBOL_FLAGS_REEXPORT = 0x08
EXPORT_SYMBOL_FLAGS_STUB_AND_RESOLVER = 0x10


def read_uleb(buf, pos):
    result = shift = 0
    while True:
        byte = buf[pos]
        pos += 1
        result |= (byte & 0x7F) << shift
        shift += 7
        if not byte & 0x80:
            return result, pos


def read_cstr(buf, pos):
    end = buf.index(b"\0", pos)
    return buf[pos:end].decode("utf-8", "replace"), end + 1


class Image:
    def __init__(self, path):
        self.path = path
        self.deps = []          # install names, in load-command order (1-based ordinals)
        self.reexported = []    # install names of LC_REEXPORT_DYLIB dependencies
        self.exports = {}       # symbol -> None (defined here) or (ordinal, import name)


class Cache:
    """One dyld shared cache file: its mappings and the images inside it."""

    def __init__(self, path):
        self.buf = open(path, "rb").read()
        buf = self.buf
        if not buf.startswith(b"dyld_v1"):
            sys.exit(f"{path}: not a dyld shared cache")
        mapping_offset, mapping_count = struct.unpack_from("<II", buf, 0x10)
        self.mappings = [struct.unpack_from("<QQQ", buf, mapping_offset + 32 * i)
                         for i in range(mapping_count)]
        if mapping_offset > 0x18C:
            _, subcaches = struct.unpack_from("<II", buf, 0x188)
            if subcaches:
                sys.exit(f"{path}: split cache ({subcaches} subcaches) not supported")
        text_offset, text_count = struct.unpack_from("<QQ", buf, 0x88)
        self.images = {}
        for i in range(text_count):
            load_address, _, path_offset = struct.unpack_from("<QII", buf, text_offset + 32 * i + 16)
            image_path, _ = read_cstr(buf, path_offset)
            self.images[image_path] = self._parse_image(load_address, image_path)

    def offset_of(self, address):
        for addr, size, file_offset in self.mappings:
            if addr <= address < addr + size:
                return file_offset + (address - addr)
        raise ValueError(f"address {address:#x} is outside the cache mappings")

    def _parse_image(self, load_address, path):
        buf = self.buf
        image = Image(path)
        base = self.offset_of(load_address)
        magic, _, _, _, ncmds = struct.unpack_from("<IiiII", buf, base)
        if magic != 0xFEEDFACF:
            sys.exit(f"{path}: unexpected Mach-O magic {magic:#x}")
        pos = base + 32
        linkedit = None
        trie = None
        for _ in range(ncmds):
            cmd, cmdsize = struct.unpack_from("<II", buf, pos)
            if cmd == LC_SEGMENT_64:
                name = buf[pos + 8:pos + 24].rstrip(b"\0")
                if name == b"__LINKEDIT":
                    vmaddr, _, fileoff = struct.unpack_from("<QQQ", buf, pos + 24)
                    linkedit = (vmaddr, fileoff)
            elif cmd in DEPENDENCY_COMMANDS or cmd == LC_ID_DYLIB:
                name_offset = struct.unpack_from("<I", buf, pos + 8)[0]
                name, _ = read_cstr(buf, pos + name_offset)
                if cmd != LC_ID_DYLIB:
                    image.deps.append(name)
                    if cmd == LC_REEXPORT_DYLIB:
                        image.reexported.append(name)
            elif cmd in (LC_DYLD_INFO, LC_DYLD_INFO_ONLY):
                trie = struct.unpack_from("<II", buf, pos + 40)
            elif cmd == LC_DYLD_EXPORTS_TRIE:
                trie = struct.unpack_from("<II", buf, pos + 8)
            pos += cmdsize
        if trie and trie[1]:
            data_off, data_size = trie
            if linkedit:
                # Linkedit offsets are file offsets of the file holding __LINKEDIT.
                data_off = self.offset_of(linkedit[0] + (data_off - linkedit[1]))
            self._parse_trie(image, buf[data_off:data_off + data_size])
        return image

    @staticmethod
    def _parse_trie(image, trie):
        stack = [(0, "")]
        while stack:
            pos, prefix = stack.pop()
            terminal_size, pos = read_uleb(trie, pos)
            if terminal_size:
                flags, tpos = read_uleb(trie, pos)
                if flags & EXPORT_SYMBOL_FLAGS_REEXPORT:
                    ordinal, tpos = read_uleb(trie, tpos)
                    import_name, _ = read_cstr(trie, tpos)
                    image.exports[prefix] = (ordinal, import_name or prefix)
                else:
                    image.exports[prefix] = None
                pos += terminal_size
            child_count = trie[pos]
            pos += 1
            for _ in range(child_count):
                edge, pos = read_cstr(trie, pos)
                child, pos = read_uleb(trie, pos)
                stack.append((child, prefix + edge))

    def resolve(self, image_path, symbol, seen=None):
        """Which image defines `symbol` as seen through `image_path`, or None."""
        seen = seen or set()
        if (image_path, symbol) in seen:
            return None
        seen.add((image_path, symbol))
        image = self.images.get(image_path)
        if image is None:
            return None
        if symbol in image.exports:
            entry = image.exports[symbol]
            if entry is None:
                return image_path
            ordinal, import_name = entry
            if 1 <= ordinal <= len(image.deps):
                return self.resolve(image.deps[ordinal - 1], import_name, seen)
            return None
        for dep in image.reexported:
            found = self.resolve(dep, symbol, seen)
            if found:
                return found
        return None

    def defined_anywhere(self, symbol):
        return sorted(path for path, image in self.images.items()
                      if image.exports.get(symbol, 0) is None)


def run(*argv):
    return subprocess.run(argv, check=True, capture_output=True, text=True).stdout


def dext_imports(dext, arch):
    """(symbol, library label, weak) for every undefined symbol of the dext."""
    imports = []
    for line in run("nm", "-m", "-arch", arch, dext).splitlines():
        if "(undefined)" not in line:
            continue
        fields = line.split()
        weak = "weak" in fields
        symbol = fields[fields.index("external") + 1]
        label = line[line.rindex("(from ") + 6:].rstrip(")") if "(from " in line else ""
        imports.append((symbol, label, weak))
    return imports


def dext_libraries(dext, arch):
    """nm's '(from X)' label -> install name, from the dext's load commands."""
    libraries = {}
    for line in run("otool", "-arch", arch, "-L", dext).splitlines()[1:]:
        install_name = line.strip().split(" (compatibility")[0]
        leaf = os.path.basename(install_name)
        label = leaf[:-len(".dylib")] if leaf.endswith(".dylib") else leaf
        libraries[label] = install_name
    return libraries


def demangle(symbols):
    if not symbols:
        return {}
    out = run("c++filt", *symbols).splitlines()
    return dict(zip(symbols, out))


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    dext, cache_path, arch = sys.argv[1:]
    cache = Cache(cache_path)
    libraries = dext_libraries(dext, arch)
    imports = dext_imports(dext, arch)

    missing = []    # (symbol, install name, hint)
    weak_missing = []
    for symbol, label, weak in imports:
        install_name = libraries.get(label)
        if install_name is None:
            missing.append((symbol, label or "?", "no such library in the dext's load commands"))
            continue
        if install_name not in cache.images:
            missing.append((symbol, install_name, "library is not in this cache"))
            continue
        if cache.resolve(install_name, symbol):
            continue
        elsewhere = cache.defined_anywhere(symbol)
        hint = (f"exported only by {', '.join(os.path.basename(p) for p in elsewhere)}, "
                f"not re-exported by {os.path.basename(install_name)}" if elsewhere
                else "not exported by anything in this cache")
        (weak_missing if weak else missing).append((symbol, install_name, hint))

    names = demangle([m[0] for m in missing + weak_missing])
    print(f"dext:    {dext} ({arch})")
    print(f"runtime: {cache_path} ({len(cache.images)} images)")
    print(f"imports: {len(imports)}, missing: {len(missing)}"
          + (f", weak and missing (tolerated by dyld): {len(weak_missing)}" if weak_missing else ""))
    for title, entries in (("Missing", missing), ("Weak, missing", weak_missing)):
        if entries:
            print(f"\n{title}:")
            for symbol, install_name, hint in entries:
                print(f"  {symbol}  (from {os.path.basename(install_name)})")
                if names.get(symbol, symbol) != symbol:
                    print(f"      {names[symbol]}")
                print(f"      {hint}")
    sys.exit(1 if missing else 0)


if __name__ == "__main__":
    main()
