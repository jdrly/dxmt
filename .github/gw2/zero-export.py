# gw2 (#182): check-mach9.zsh's negative fixture. Zero the address of one named export in a PE
# image's primary export table, in place, so the name stays and the export points nowhere.
#
#   python3 -I zero-export.py <dll> <name>
import struct
import sys

path, name = sys.argv[1], sys.argv[2].encode()
data = bytearray(open(path, "rb").read())
pe = struct.unpack_from("<I", data, 0x3C)[0]
sections = struct.unpack_from("<H", data, pe + 6)[0]
opt = pe + 24
magic = struct.unpack_from("<H", data, opt)[0]
ddir = opt + (112 if magic == 0x20B else 96)
export_rva = struct.unpack_from("<I", data, ddir)[0]
shdr = opt + struct.unpack_from("<H", data, pe + 20)[0]


def off(rva):
    for i in range(sections):
        s = shdr + 40 * i
        vsize, va, rawsize, raw = struct.unpack_from("<IIII", data, s + 8)
        if va <= rva < va + max(vsize, rawsize):
            return raw + rva - va
    sys.exit(f"RVA {rva:#x} is in no section")


e = off(export_rva)
nfuncs, nnames, funcs, names, ords = struct.unpack_from("<IIIII", data, e + 20)
for i in range(nnames):
    n = off(struct.unpack_from("<I", data, off(names) + 4 * i)[0])
    if bytes(data[n : data.index(0, n)]) == name:
        ordinal = struct.unpack_from("<H", data, off(ords) + 2 * i)[0]
        struct.pack_into("<I", data, off(funcs) + 4 * ordinal, 0)
        open(path, "wb").write(data)
        break
else:
    sys.exit(f"no export {name.decode()}")
