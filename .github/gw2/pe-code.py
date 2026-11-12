# gw2 (#199): the code sections of a PE image, one line each: name, RVA, virtual size and the
# sha256 of the section's bytes up to its virtual size. check-mach9.zsh compares a shipped DLL's
# lines with those of its unstripped copy, so the symbols describe the code that runs.
#
#   python3 -I pe-code.py <dll>
import hashlib
import struct
import sys

data = open(sys.argv[1], "rb").read()
pe = struct.unpack_from("<I", data, 0x3C)[0]
sections = struct.unpack_from("<H", data, pe + 6)[0]
shdr = pe + 24 + struct.unpack_from("<H", data, pe + 20)[0]
for i in range(sections):
    s = shdr + 40 * i
    name = data[s : s + 8].rstrip(b"\0").decode()
    vsize, va, rawsize, raw = struct.unpack_from("<IIII", data, s + 8)
    flags = struct.unpack_from("<I", data, s + 36)[0]
    if flags & 0x20:  # IMAGE_SCN_CNT_CODE
        body = data[raw : raw + min(vsize, rawsize)]
        print(name, hex(va), vsize, hashlib.sha256(body).hexdigest())
