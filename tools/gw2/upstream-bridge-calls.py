#!/usr/bin/env python3
"""gw2 (#299): the bridge calls upstream added between two of its commits, as manifest lines.

Upstream keeps two hand-written tables in src/winemetal/unix/winemetal_unix.c and numbers a call
by its position in them. My fork generates both tables from src/winemetal/winemetal_calls.h (ADR
0005), so upstream's edit to its tables can't be merged: I take my side there and append one
manifest line per new call. This prints those lines, and what to change in upstream's new thunk.

  tools/gw2/upstream-bridge-calls.py [OLD [NEW]] [--manifest FILE]
  tools/gw2/upstream-bridge-calls.py --self-test

  OLD  the upstream commit of my last sync (default: git merge-base origin/gw2-on-mac upstream/main)
  NEW  the upstream commit being synced (default: upstream/main)
  --manifest FILE  the manifest to number against (default: src/winemetal/winemetal_calls.h in
                   the working tree)
  --self-test      check the parsing and the numbering on built-in samples; needs no git

Run it from anywhere in the checkout. It reads git objects and the manifest and writes nothing.
Exit 0: lines printed, or nothing to add. Exit 1: upstream changed or removed an existing entry,
or its tables no longer parse, so the calls have to be read by hand.
"""
import re
import subprocess
import sys

TABLES = ("__wine_unix_call_funcs", "__wine_unix_call_wow64_funcs")
UNIX_C = "src/winemetal/unix/winemetal_unix.c"
MANIFEST = "src/winemetal/winemetal_calls.h"


class Unreadable(Exception):
    pass


def table(source, name):
    """The entries of one of upstream's tables: a handler's name, or None for NULL."""
    m = re.search(r"const void \*" + name + r"\[\] = \{(.*?)\n\};", source, re.S)
    if not m:
        raise Unreadable(f"no table {name}")
    entries = []
    for raw in m.group(1).split(","):
        item = re.sub(r"/\*.*?\*/|//[^\n]*", "", raw, flags=re.S).strip()
        if not item:
            continue
        if item == "NULL":
            entries.append(None)
        elif re.fullmatch(r"&\w+", item):
            entries.append(item[1:])
        else:
            raise Unreadable(f"{name}: an entry I can't read: {item[:60]!r}")
    return entries


def tables(source):
    both = [table(source, name) for name in TABLES]
    if len(both[0]) != len(both[1]):
        raise Unreadable("upstream's two tables differ in length")
    return both


def call_name(handler):
    """_MTLTexture_getBytes -> MTLTexture_getBytes, thunk_SM50Compile -> SM50Compile."""
    for prefix in ("thunk_", "_"):
        if handler.startswith(prefix):
            return handler[len(prefix):]
    return handler


def manifest(text):
    """(the next free number, handler -> number) of my manifest."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    # a reserved line's string may hold parentheses, "#277: was ... (#242)", so the number is read
    # up to its comma, and only a call's handlers through the closing parenthesis
    numbers = [int(n) for n in re.findall(r"\bWMT_(?:CALL|CALL_WOW64|RESERVED)\(\s*(\d+)\s*,", text)]
    handlers = {}
    for m in re.finditer(r"\bWMT_(?:CALL|CALL_WOW64)\(\s*(\d+)\s*,([^()]*)\)", text):
        handlers[m.group(2).split(",")[1].strip()] = int(m.group(1))
    if not numbers:
        raise Unreadable("no WMT_CALL lines in the manifest")
    return max(numbers) + 1, handlers


def lines_for(old_source, new_source, manifest_text, new_sha, old_sha):
    """The text to print for upstream's tables at two commits. Raises Unreadable."""
    old64, old32 = tables(old_source)
    new64, new32 = tables(new_source)
    next_number, known = manifest(manifest_text)
    if new64[: len(old64)] != old64 or new32[: len(old32)] != old32:
        first = next(
            (i for i in range(min(len(old64), len(new64))) if (old64[i], old32[i]) != (new64[i], new32[i])),
            min(len(old64), len(new64)),
        )
        raise Unreadable(
            f"upstream changed or removed entry {first} of its tables between {old_sha} and {new_sha}; "
            "that isn't an append"
        )
    if len(new64) == len(old64):
        return [f"/* upstream {new_sha}: no new bridge call since {old_sha} */"]
    lines, notes = [], []
    for index in range(len(old64), len(new64)):
        h64, h32 = new64[index], new32[index]
        if h64 is None:
            lines.append(f'WMT_RESERVED({next_number}, "upstream {new_sha}: NULL at its position {index}")')
            next_number += 1
            continue
        name = call_name(h64)
        if h64 in known:
            notes.append(f"# {name}: already in the manifest as {known[h64]}; no new line")
            continue
        if h32 == h64:
            lines.append(f"WMT_CALL({next_number}, {name}, {h64})")
        else:
            lines.append(f"WMT_CALL_WOW64({next_number}, {name}, {h64}, {h32 or 'NULL'})")
        notes.append(f"# {name}: upstream's thunk says UNIX_CALL({index}, ...); in my fork write UNIX_CALL({name}, ...)")
        next_number += 1
    return ([f"/* upstream {new_sha} */"] + lines if lines else []) + notes


def self_test():
    def source(funcs, wow64):
        body = lambda entries: "".join(f"    {e},\n" for e in entries)
        return (
            "const void *__wine_unix_call_funcs[] = {\n" + body(funcs) + "};\n\n#ifndef DXMT_NATIVE\n"
            "const void *__wine_unix_call_wow64_funcs[] = {\n" + body(wow64) + "};\n#endif\n"
        )

    old = source(["&_A", "NULL", "&thunk_S"], ["&_A", "NULL", "&thunk32_S"])
    new = source(
        ["&_A", "NULL", "&thunk_S", "&_B", "&thunk_T", "NULL", "&_A2"],
        ["&_A", "NULL", "&thunk32_S", "&_B", "&thunk32_T", "NULL", "&_A2"],
    )
    mine = '/* WMT_CALL(99, X, _X) in a comment */\nWMT_CALL(0, A, _A)\nWMT_RESERVED(1, "upstream")\n' \
           "WMT_CALL_WOW64(2, S,\n               thunk_S, thunk32_S)\nWMT_CALL(3, A2, _A2)\n"
    got = lines_for(old, new, mine, "new", "old")
    want = [
        "/* upstream new */",
        "WMT_CALL(4, B, _B)",
        "WMT_CALL_WOW64(5, T, thunk_T, thunk32_T)",
        'WMT_RESERVED(6, "upstream new: NULL at its position 5")',
        "# B: upstream's thunk says UNIX_CALL(3, ...); in my fork write UNIX_CALL(B, ...)",
        "# T: upstream's thunk says UNIX_CALL(4, ...); in my fork write UNIX_CALL(T, ...)",
        "# A2: already in the manifest as 3; no new line",
    ]
    assert got == want, got
    held = mine + 'WMT_RESERVED(4, "#277: was a call (#242)")\n'
    assert lines_for(old, new, held, "new", "old")[1] == "WMT_CALL(5, B, _B)", "a reserved line with ( in its string"
    assert lines_for(old, old, mine, "new", "old") == ["/* upstream new: no new bridge call since old */"]
    for bad in (source(["&_A"], ["&_A"]), source(["&_Z", "NULL", "&thunk_S"], ["&_Z", "NULL", "&thunk32_S"])):
        try:
            lines_for(old, bad, mine, "new", "old")
        except Unreadable as e:
            assert "isn't an append" in str(e), e
        else:
            raise AssertionError("a removal or a changed entry passed as an append")
    try:
        lines_for(old, source(["&_A", "NULL", "&thunk_S", "f(1)"], ["&_A", "NULL", "&thunk32_S", "&_B"]), mine, "n", "o")
    except Unreadable:
        pass
    else:
        raise AssertionError("an entry that isn't &name or NULL was read")
    print("upstream-bridge-calls: self-test ok")
    return 0


def git(*args):
    return subprocess.run(("git",) + args, check=True, capture_output=True, text=True).stdout


def main(argv):
    args, manifest_path = [], None
    it = iter(argv)
    for a in it:
        if a in ("-h", "--help"):
            print(__doc__)
            return 0
        if a == "--self-test":
            return self_test()
        if a == "--manifest":
            manifest_path = next(it)
        else:
            args.append(a)
    if len(args) > 2:
        print(__doc__, file=sys.stderr)
        return 2
    try:
        old = args[0] if args else git("merge-base", "origin/gw2-on-mac", "upstream/main").strip()
        new = args[1] if len(args) > 1 else "upstream/main"
        manifest_path = manifest_path or git("rev-parse", "--show-toplevel").strip() + "/" + MANIFEST
        out = lines_for(
            git("show", f"{old}:{UNIX_C}"),
            git("show", f"{new}:{UNIX_C}"),
            open(manifest_path).read(),
            git("rev-parse", "--short", new).strip(),
            git("rev-parse", "--short", old).strip(),
        )
    except subprocess.CalledProcessError as e:
        print(f"upstream-bridge-calls: {e.stderr.strip()}", file=sys.stderr)
        return 1
    except (Unreadable, OSError) as e:
        print(f"upstream-bridge-calls: {e}; read upstream's tables by hand", file=sys.stderr)
        return 1
    print("\n".join(out))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
