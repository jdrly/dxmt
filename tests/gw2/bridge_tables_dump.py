#!/usr/bin/env python3
# gw2 (B2): dumps the bridge's two call tables from a built winemetal.so, entry by entry.
#
#   tests/gw2/bridge_tables_dump.py <build>/x64/src/winemetal/unix/winemetal.so <count> > tables.txt
#
# <count> is the tables' length (WMTCall_Count): Mach-O symbols carry no size, and trailing NULL
# entries look like the padding after them.
#
# One line per call number: the number, the 64-bit handler and the wow64 handler, each the local
# symbol its table entry points at, or NULL. Dump the library before and after a change to the
# manifest (src/winemetal/winemetal_calls.h) and diff the two files: that is B2's parity artifact.
# It needs an unstripped library (the build directory's, not the staged install's) and macOS's nm
# and dyld_info. It reads the binary only; it runs nothing.
import bisect
import subprocess
import sys

TABLES = ("___wine_unix_call_funcs", "___wine_unix_call_wow64_funcs")


def run(*args):
  return subprocess.run(args, check=True, capture_output=True, text=True).stdout


def main(path, count):
  # nm -m: "<addr> (<segment>,<section>) [non-]external <name>"
  symbols, text = {}, []
  for line in run("nm", "-m", "-n", path).splitlines():
    parts = line.split()
    if len(parts) < 4 or not parts[1].startswith("(__"):  # undefined, absolute, indirect
      continue
    addr, section, name = int(parts[0], 16), parts[1], parts[-1]
    symbols.setdefault(name, (addr, section))
    if section == "(__TEXT,__text)":
      text.append((addr, name))
  text.sort()
  text_addrs = [a for a, _ in text]

  def text_symbol(target):
    i = bisect.bisect_right(text_addrs, target) - 1
    if i < 0 or text_addrs[i] != target:
      return "?0x%x" % target
    return text[i][1]

  # The table's extent: from its symbol to the next symbol in the same section.
  extents = {}
  for table in TABLES:
    if table not in symbols:
      sys.exit("%s: no %s (a stripped library?)" % (path, table))
    start, section = symbols[table]
    end = min((a for a, s in symbols.values() if s == section and a > start), default=None)
    if end is None:
      sys.exit("%s: can't size %s" % (path, table))
    if end - start < count * 8:
      sys.exit("%s: %s holds fewer than %d entries" % (path, table, count))
    extents[table] = (start, start + count * 8)

  # dyld_info -fixups: "<segment> <section> 0x<addr> rebase 0x<target>"; a NULL entry has none
  fixups = {}
  for line in run("dyld_info", "-fixups", path).splitlines():
    parts = line.split()
    if len(parts) == 5 and parts[3] == "rebase":
      fixups[int(parts[2], 16)] = int(parts[4], 16)

  columns = []
  for table in TABLES:
    start, end = extents[table]
    columns.append(
      [text_symbol(fixups[a]) if a in fixups else "NULL" for a in range(start, end, 8)]
    )
  print("# %s and %s, %d entries each" % (TABLES[0][1:], TABLES[1][1:], count))
  for i in range(count):
    print("%3d %s %s" % (i, columns[0][i], columns[1][i]))


if __name__ == "__main__":
  if len(sys.argv) != 3:
    sys.exit("usage: %s <winemetal.so> <count>" % sys.argv[0])
  main(sys.argv[1], int(sys.argv[2]))
