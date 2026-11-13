"""gw2 (#248): find calls from one patchable entry of an ARM64X DLL into another.

    python3 -I patchable-calls.py <llvm-readobj> <dll>

On ARM64EC a DXMT_HOTPATCHABLE function (hybrid_patchable) has an x64 entry that a hook can patch,
and a call to it goes through that entry even from inside the module. When the ARM64EC code of one
entry reaches another entry, a hook on both runs twice for one call: Present calling Present1 ran
the Steam overlay's Present hook twice a frame.

The image's CHPE redirection metadata (llvm-readobj --coff-load-config) maps each x64 entry, a
patchable function's or an export's, to its ARM64EC body. From each body the script follows every direct call and tail call (bl, b) through the
ARM64EC code and looks for an address of an x64 entry (adrp, then add), which is how ARM64EC code
calls one: lld's $hybpatch_thunk for the callee loads its x64 entry and branches there through the
emulator's dispatcher. Indirect calls aren't followed. Functions end at the next function start: a
.pdata entry, a call target or a body, and at their .pdata length where they have one.

Prints "<dll> <entries> <crossings>", then one line per crossing: the entry, the entry it reaches,
and the functions in between. Names come from the image's symbol table through llvm-nm and
llvm-cxxfilt from llvm-readobj's directory, RVAs where there are none.
"""

import os
import re
import struct
import subprocess
import sys


def image(data):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    count, opt_size = struct.unpack_from("<H12xH", data, pe + 6)
    opt = pe + 24
    base = struct.unpack_from("<Q", data, opt + 24)[0]
    exc_rva, exc_size = struct.unpack_from("<II", data, opt + 112 + 3 * 8)  # the exception directory
    secs = []
    for i in range(count):
        vsize, rva, raw_size, raw_ptr = struct.unpack_from("<IIII", data, opt + opt_size + 40 * i + 8)
        secs.append((rva, min(vsize, raw_size), raw_ptr))
    return base, (exc_rva, exc_size), secs


def reader(data, secs):
    def read(rva, size):
        for start, length, ptr in secs:
            if start <= rva and rva + size <= start + length:
                return data[ptr + rva - start : ptr + rva - start + size]
        return None

    return read


def load_config(readobj, path):
    text = subprocess.run([readobj, "--coff-load-config", path], check=True, capture_output=True, text=True).stdout
    text = text.split("HybridObject")[0]  # the hybrid view repeats the map
    ranges = [(int(s, 16), int(e, 16), k) for s, e, k in re.findall(r"(0x[0-9A-F]+) - (0x[0-9A-F]+)  (\S+)", text)]
    redirect = {}
    block = re.search(r"RedirectionMetadata \[(.*?)\]", text, re.S)
    for src, dst in re.findall(r"(0x[0-9A-F]+) -> (0x[0-9A-F]+)", block.group(1) if block else ""):
        redirect[int(src, 16)] = int(dst, 16)
    return ranges, redirect


def names(readobj, path, base):
    tools = os.path.dirname(readobj)
    nm = subprocess.run([os.path.join(tools, "llvm-nm"), path], capture_output=True, text=True).stdout
    by_rva = {}
    for line in nm.splitlines():
        m = re.match(r"([0-9a-fA-F]+) \S (\S+)$", line)
        if m:
            by_rva.setdefault(int(m.group(1), 16) - base, []).append(m.group(2))
    # one name per address: the plain one before #name, name$hp_target and EXP+ names
    chosen = {rva: sorted(n, key=lambda s: (s.startswith(("#", "EXP+")), "$" in s, len(s)))[0] for rva, n in by_rva.items()}
    filt = os.path.join(tools, "llvm-cxxfilt")
    if chosen and os.path.exists(filt):
        keys = list(chosen)
        plain = [chosen[k].lstrip("#").split("$")[0] for k in keys]
        out = subprocess.run([filt], input="\n".join(plain), capture_output=True, text=True).stdout.splitlines()
        if len(out) == len(keys):
            chosen.update(zip(keys, out))
    return chosen


def main(readobj, path):
    with open(path, "rb") as f:
        data = f.read()
    base, (exc_rva, exc_size), secs = image(data)
    read = reader(data, secs)
    ranges, redirect = load_config(readobj, path)
    ec = [(s, e) for s, e, k in ranges if k == "ARM64EC"]
    name = path.rsplit("/", 1)[-1]
    if not ec:
        print(name, "no-code-map")
        return
    in_ec = lambda rva: any(s <= rva < e for s, e in ec)

    # the ARM64EC code, a word per address
    words = {}
    for s, e in ec:
        s += -s % 4
        for i, (w,) in enumerate(struct.iter_unpack("<I", read(s, (e - s) & ~3) or b"")):
            words[s + 4 * i] = w

    # .pdata: ARM64 RUNTIME_FUNCTIONs, packed (length in bits 2-12) or with .xdata (bits 0-17)
    length = {}
    raw = read(exc_rva, exc_size) or b""
    for begin, unwind in struct.iter_unpack("<II", raw[: len(raw) // 8 * 8]):
        if not in_ec(begin):
            continue
        if unwind & 3:
            length[begin] = ((unwind >> 2) & 0x7FF) * 4
        else:
            header = read(unwind, 4)
            if header:
                length[begin] = (struct.unpack("<I", header)[0] & 0x3FFFF) * 4

    def branch(addr, w):
        # bl and b: imm26, words from here
        if w & 0x7C000000 == 0x14000000:
            off = (w & 0x3FFFFFF) - ((w & 0x2000000) << 1)
            return addr + 4 * off, bool(w & 0x80000000)
        return None, False

    starts = set(length) | set(redirect.values())
    for addr, w in words.items():
        target, link = branch(addr, w)
        if link and target in words:
            starts.add(target)

    def segments():
        order = sorted(starts)
        seg = {}
        for i, s in enumerate(order):
            end = min([e for b, e in ec if b <= s < e] + order[i + 1 : i + 2])
            if s in length:
                end = min(end, s + length[s])
            seg[s] = end
        return seg

    # a b out of its function is a tail call: its target starts a function too
    while True:
        seg = segments()
        added = False
        for s, end in seg.items():
            for addr in range(s, end, 4):
                target, link = branch(addr, words.get(addr, 0))
                if target is not None and not link and target in words and not (s <= target < end) and target not in starts:
                    starts.add(target)
                    added = True
        if not added:
            break

    calls, refs = {}, {}
    for s, end in seg.items():
        out, hit, page = set(), set(), {}
        for addr in range(s, end, 4):
            w = words.get(addr)
            if w is None:
                break
            target, _ = branch(addr, w)
            if target is not None and target in starts:
                out.add(target)
            elif w & 0x9F000000 == 0x90000000:  # adrp
                imm = ((w >> 3) & 0x1FFFFC) | ((w >> 29) & 3)
                imm -= (imm & 0x100000) << 1
                page[w & 31] = ((base + addr) & ~0xFFF) + (imm << 12) - base
            elif w & 0xFFC00000 == 0x91000000:  # add x, x, #imm (64-bit, no shift)
                rn, rd = (w >> 5) & 31, w & 31
                if rn in page:
                    ref = page[rn] + ((w >> 10) & 0xFFF)
                    if ref in redirect:
                        hit.add(ref)
                page.pop(rd, None)
        calls[s], refs[s] = out, hit

    label = names(readobj, path, base)
    show = lambda rva: label.get(rva, f"RVA {rva:#x}")
    crossings = []
    for entry, body in sorted(redirect.items()):
        parent, todo = {body: None}, [body]
        while todo:
            fn = todo.pop()
            for ref in sorted(refs.get(fn, ())):
                chain, at = [], parent[fn]  # what lies between the body and fn, which loads the entry
                while at is not None and at != body:
                    chain.append(show(at))
                    at = parent[at]
                crossings.append((show(entry), show(ref), chain[::-1]))
            for nxt in calls.get(fn, ()):
                if nxt not in parent:
                    parent[nxt] = fn
                    todo.append(nxt)
    print(name, len(redirect), len(crossings))
    for src, dst, via in crossings:
        print(f"{src} -> {dst}" + (f" (via {', '.join(via)})" if via else ""))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit("usage: patchable-calls.py <llvm-readobj> <dll>")
    main(sys.argv[1], sys.argv[2])
