"""gw2 (#322): the switch table in docs/gw2/README.md names exactly the switches the sources read.

  .github/gw2/switch-table.py [ROOT]      check the checkout at ROOT (default: the current directory)
  .github/gw2/switch-table.py --self-test

A switch the sources read is a string literal "DXMT_..." anywhere under src/: a sw::Switch
declaration, an env::getEnvVar or getenv read, a name constant, a helper such as
ThreadPrioritySwitch. The table is the one under "## Switches": a row's first cell is the name in
backticks. The two sets must be equal, apart from NOT_MINE below.

It also compares the Default column with the fallback of every sw::Switch<bool> and
sw::Switch<uint32_t> declared with a literal: true is "on", false is "off", 0 is "off" or "0",
another number is that number. And every row has five cells, the last one Status (#320), which
has to start with one of STATUSES in backticks.

Exit 0 when they agree, 1 with one line per difference. It reads files and builds nothing.
"""
import os
import re
import sys

# Names in src/ that are not rows of my table, each with the reason.
NOT_MINE = {
    "DXMT_CAPTURE_EXECUTABLE": "upstream's (docs/DEVELOPMENT.md)",
    "DXMT_CAPTURE_FRAME": "upstream's",
    "DXMT_CONFIG": "upstream's",
    "DXMT_CONFIG_FILE": "upstream's",
    "DXMT_ENABLE_NVEXT": "upstream's",
    "DXMT_LOG_LEVEL": "upstream's",
    "DXMT_LOG_PATH": "upstream's",
    "DXMT_METALFX_SPATIAL_SWAPCHAIN": "upstream's; my fork only declares it (#60)",
    "DXMT_SHADER_CACHE": "upstream's",
    "DXMT_SHADER_CACHE_PATH": "upstream's",
    "DXMT_USE_DEFAULT_METAL_CACHE": "upstream's",
    "DXMT_FOO": "the example in dxmt_gw2_switches.hpp's comments",
}

# The words the README's Status column starts with, as #320 defined them.
STATUSES = ("default", "experiment", "diagnostic", "test", "settled")

LITERAL = re.compile(r'"(DXMT_[A-Z0-9_]+)"')
DECLARED = re.compile(r'sw::Switch<(bool|uint32_t)>\s+\w+\s*\{\s*"(DXMT_[A-Z0-9_]+)"\s*,\s*(true|false|\d+)\s*,')
ROW = re.compile(r"^\| `(DXMT_[A-Z0-9_]+)` \|")
STATUS = re.compile(r"`(%s)`" % "|".join(STATUSES))


def source_names(texts):
    names, declared = set(), {}
    for text in texts:
        names.update(LITERAL.findall(text))
        for kind, name, value in DECLARED.findall(text):
            declared[name] = value
    return names, declared


def table_rows(readme):
    """name -> the row's cells, and the names that have more than one row."""
    rows, twice, inside = {}, [], False
    for line in readme.splitlines():
        if line.startswith("## "):
            inside = line.strip() == "## Switches"
            continue
        m = ROW.match(line) if inside else None
        if not m:
            continue
        if m.group(1) in rows:
            twice.append(m.group(1))
        else:
            rows[m.group(1)] = [cell.strip() for cell in line.strip().split("|")[1:-1]]
    return rows, twice


def default_matches(value, cell):
    cell = cell.strip("` ")
    if value == "true":
        return cell == "on"
    if value == "false":
        return cell == "off"
    if value == "0":
        return cell in ("off", "0")
    return cell == value


def differences(texts, readme):
    names, declared = source_names(texts)
    rows, twice = table_rows(readme)
    out = []
    if not rows:
        return ['no rows under "## Switches" in the README']
    for name in sorted(names - set(NOT_MINE) - set(rows)):
        out.append(f"{name}: the sources read it, the table has no row")
    for name in sorted(set(rows) - names):
        out.append(f"{name}: the table has a row, nothing under src/ reads it")
    for name in sorted(set(rows) & set(NOT_MINE)):
        out.append(f"{name}: in the table and in NOT_MINE ({NOT_MINE[name]}); take it out of one")
    for name in sorted(set(twice)):
        out.append(f"{name}: two rows in the table")
    for name, value in sorted(declared.items()):
        if len(rows.get(name, [])) == 5 and not default_matches(value, rows[name][1]):
            out.append(f"{name}: declared with {value}, the table's Default says {rows[name][1]!r}")
    for name, cells in sorted(rows.items()):
        if len(cells) != 5:
            out.append(f"{name}: {len(cells)} cells, the table has 5")
        elif not STATUS.match(cells[4]):
            out.append(f"{name}: the Status cell {cells[4]!r} doesn't start with one of "
                       + ", ".join(f"`{s}`" for s in STATUSES))
    return out


def self_test():
    src = [
        'sw::Switch<bool> kA{"DXMT_A", true, sw::legacy::notZero};\n'
        'sw::Switch<uint32_t> kN{\n    "DXMT_N", 250, sw::legacy::strtoul32};\n'
        'sw::Switch<uint32_t> kZ{"DXMT_Z", 0, sw::legacy::strtoul32};\n'
        'auto v = env::getEnvVar("DXMT_RAW"); const char *c = getenv("DXMT_LOG_LEVEL");\n'
        'Log("DXMT_UNREAD=1 is only a log text");\n'
        '// `sw::Switch<bool> kFoo{"DXMT_FOO", true, sw::legacy::notZero};`\n'
    ]
    head = "# x\n\n## Switches\n\n| Variable | Default | What it does | Off restores | Status |\n|---|---|---|---|---|\n"
    good = head + "| `DXMT_A` | on | a | b | `default` (#1) |\n| `DXMT_N` | 250 | a | b | `experiment`: not decided |\n" \
                  "| `DXMT_Z` | off | a | b | `diagnostic` |\n| `DXMT_RAW` | `x` | a | b | `settled`: off won (#2) |\n" \
                  "\n## Other\n\n| `DXMT_ELSEWHERE` | on | not a switch row | | |\n"
    assert differences(src, good) == [], differences(src, good)
    bad = head + "| `DXMT_A` | off | a | b | `test` |\n| `DXMT_N` | 100 | a | b | `default` |\n" \
                 "| `DXMT_Z` | off | a | b | `default` |\n| `DXMT_Z` | off | a | b | `default` |\n" \
                 "| `DXMT_GONE` | on | a | b | `default` |\n| `DXMT_LOG_LEVEL` | info | a | b | `default` |\n" \
                 "| `DXMT_UNREAD` | on | a | b | `default` |\n"
    got = differences(src, bad)
    want = [
        "DXMT_RAW: the sources read it, the table has no row",
        "DXMT_GONE: the table has a row, nothing under src/ reads it",
        "DXMT_UNREAD: the table has a row, nothing under src/ reads it",
        "DXMT_LOG_LEVEL: in the table and in NOT_MINE (upstream's); take it out of one",
        "DXMT_Z: two rows in the table",
        "DXMT_A: declared with true, the table's Default says 'off'",
        "DXMT_N: declared with 250, the table's Default says '100'",
    ]
    assert got == want, got
    statuses = head + "| `DXMT_A` | on | a | b | |\n| `DXMT_N` | 250 | a | b | `testing` |\n" \
                      "| `DXMT_Z` | on | `default` |\n| `DXMT_RAW` | `x` | a | b | default |\n"
    got = differences(src, statuses)
    tail = " doesn't start with one of `default`, `experiment`, `diagnostic`, `test`, `settled`"
    want = [
        "DXMT_A: the Status cell ''" + tail,
        "DXMT_N: the Status cell '`testing`'" + tail,
        "DXMT_RAW: the Status cell 'default'" + tail,
        "DXMT_Z: 3 cells, the table has 5",
    ]
    assert got == want, got
    assert differences(src, "# no table\n") == ['no rows under "## Switches" in the README']
    print("switch-table: self-test ok")
    return 0


def main(argv):
    if argv[:1] == ["--self-test"]:
        return self_test()
    if argv[:1] in (["-h"], ["--help"]):
        print(__doc__)
        return 0
    root = argv[0] if argv else "."
    texts = []
    for folder, _, files in os.walk(os.path.join(root, "src")):
        for name in files:
            with open(os.path.join(folder, name), encoding="utf-8", errors="replace") as f:
                texts.append(f.read())
    with open(os.path.join(root, "docs/gw2/README.md"), encoding="utf-8") as f:
        readme = f.read()
    out = differences(texts, readme)
    for line in out:
        print(f"switch-table: {line}")
    if out:
        print(f"switch-table: {len(out)} difference(s) between src/ and docs/gw2/README.md's switch table")
        return 1
    rows, _ = table_rows(readme)
    print(f"switch-table: {len(rows)} switches, src/ and docs/gw2/README.md agree")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
