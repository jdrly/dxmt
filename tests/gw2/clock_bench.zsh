#!/bin/zsh
# gw2 (#28): runs tests/gw2/clock_bench.cpp headless under the engine, to choose gw2::nowNs's clock.
#
#   tests/gw2/clock_bench.zsh <engine> <scratch dir> [calls] [runs]
#
# <engine> is never run itself: the script makes an APFS clone of it in <scratch dir>/engine (once)
# and a throwaway prefix in <scratch dir>/prefix. winemac is disabled, so no window opens, and
# WineDbg's crash dialog is off. Only that prefix's wineserver is stopped. Build with the fork's
# toolchain (x86_64-w64-mingw32-g++, Homebrew). Prints clock_bench.exe's table once per run.

emulate -L zsh
setopt err_exit pipe_fail no_unset

(( $# >= 2 )) || { sed -n '2,10p' "${0:A}" | sed 's/^# \{0,1\}//'; exit 2 }
SRC=${0:A:h}
ENGINE=${1:A}
T=${2:A}
calls=${3:-1000000}
runs=${4:-3}
[[ -x $ENGINE/bin/wine ]] || { print -u2 "clock_bench.zsh: no bin/wine in $ENGINE"; exit 2 }

mkdir -p "$T"
export PATH="/opt/homebrew/bin:$PATH"
x86_64-w64-mingw32-g++ -O2 -std=c++20 -static "$SRC/clock_bench.cpp" -o "$T/clock_bench.exe"

[[ -x $T/engine/bin/wine ]] || cp -c -R "$ENGINE" "$T/engine"
export WINEPREFIX="$T/prefix" WINEDEBUG=-all WINEDLLOVERRIDES="winemac.drv=d;winemenubuilder.exe=d;mscoree,mshtml=d"
W="$T/engine/bin"
stop() { "$W/wineserver" -k 2>/dev/null || true }
trap stop EXIT
if [[ ! -f $WINEPREFIX/system.reg ]]; then
  "$W/wine" wineboot -i >/dev/null 2>&1
  "$W/wine" reg add 'HKCU\Software\Wine\WineDbg' /v ShowCrashDialog /t REG_DWORD /d 0 /f >/dev/null 2>&1
  "$W/wineserver" -w
fi
for r in {1..$runs}; do
  print -r -- "=== run $r"
  "$W/wine" "$T/clock_bench.exe" $calls 2>&1 | tr -d '\r'
done
