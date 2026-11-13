"""gw2 (#200): count the exclusive-pair instructions in the ARM64 and ARM64EC code of ARM64X DLLs.

    python3 -I exclusive-pairs.py <llvm-readobj> <dll>...

Prints one line per DLL: its name, then load-exclusive and store-exclusive counts for the ARM64 and
the ARM64EC code, "<name> <arm64 ldxr> <arm64 stxr> <ec ldxr> <ec stxr>". The code ranges are the
CHPE code map that llvm-readobj --coff-load-config prints, so the x64 thunks of an ARM64X image
aren't read as ARM64 words. A DLL without an ARM64EC code map prints "<name> no-code-map".

An atomic built for ARMv8.0 is a load-exclusive/store-exclusive loop; built for a core with LSE
(every Apple core) it is one instruction (ldadd, cas, swp...). The patterns are ldxr/ldaxr and
stxr/stlxr for each size (b, h, w, x); the pair forms (ldxp, stxp) aren't counted.
"""

import struct
import subprocess
import sys

LOAD = ((0x3FFFFC00, 0x085F7C00), (0x3FFFFC00, 0x085FFC00))
STORE = ((0x3FE0FC00, 0x08007C00), (0x3FE0FC00, 0x0800FC00))


def sections(data):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    count, opt_size = struct.unpack_from("<H12xH", data, pe + 6)
    table = pe + 24 + opt_size
    out = []
    for i in range(count):
        vsize, rva, raw_size, raw_ptr = struct.unpack_from("<IIII", data, table + 40 * i + 8)
        out.append((rva, min(vsize, raw_size), raw_ptr))
    return out


def code_map(readobj, path):
    text = subprocess.run([readobj, "--coff-load-config", path], check=True, capture_output=True, text=True).stdout
    ranges, inside = [], False
    for line in text.splitlines():
        line = line.strip()
        if line == "CodeMap [":
            inside = True
        elif inside and line == "]":
            break  # the hybrid view repeats the same map
        elif inside:
            start, _, end, kind = line.split()
            ranges.append((int(start, 16), int(end, 16), kind))
    return ranges


def count(data, secs, start, end):
    loads = stores = 0
    for rva, size, ptr in secs:
        lo, hi = max(start, rva), min(end, rva + size)
        lo += -lo % 4  # instructions are 4-aligned
        hi -= hi % 4
        if hi <= lo:
            continue
        for (word,) in struct.iter_unpack("<I", data[ptr + lo - rva : ptr + hi - rva]):
            loads += any(word & m == v for m, v in LOAD)
            stores += any(word & m == v for m, v in STORE)
    return loads, stores


def main(readobj, paths):
    for path in paths:
        name = path.rsplit("/", 1)[-1]
        ranges = code_map(readobj, path)
        if not any(kind == "ARM64EC" for _, _, kind in ranges):
            print(name, "no-code-map")
            continue
        with open(path, "rb") as f:
            data = f.read()
        secs = sections(data)
        totals = {"ARM64": [0, 0], "ARM64EC": [0, 0]}
        for start, end, kind in ranges:
            if kind in totals:
                loads, stores = count(data, secs, start, end)
                totals[kind][0] += loads
                totals[kind][1] += stores
        print(name, *totals["ARM64"], *totals["ARM64EC"])


if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit("usage: exclusive-pairs.py <llvm-readobj> <dll>...")
    main(sys.argv[1], sys.argv[2:])
