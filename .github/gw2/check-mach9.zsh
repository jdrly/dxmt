#!/bin/zsh
# gw2 (#182): the checks of my arm64 build for Mach9's engine that need no Wine. They read files
# only (llvm-readobj, otool, nm, lipo); nothing of the engine runs.
#
#   check-mach9.zsh <dist> <engine> <llvm-readobj> <symbols>
#
# <dist> is the build in the engine's layout (aarch64-windows, i386-windows, aarch64-unix and
# VERSION, as the arm64 job of .github/workflows/gw2.yml stages it). <engine> is the directory of
# Mach9's engine that holds lib/wine (the release tarball's wine/). <symbols> holds the unstripped
# copies of the ARM64X DLLs, in aarch64-windows (#199). Checked:
#
# - every DLL has the machine of the engine's DLL of the same name: ARM64X (an ARM64 image with an
#   ARM64EC view, so a game's x64 process loads its ARM64EC half natively) or i386, and Wine's
#   builtin marker
# - every export of the engine's DLL, in each view of an ARM64X image, is exported by mine
# - every name winemetal.dll imports from ntdll.dll is exported by the engine's ntdll.dll in the same
#   view, and so is __wine_unix_call_dispatcher, which winecrt0 looks up at the first unix call (#205)
# - winemetal.so is an arm64-only dylib, no newer macOS than the engine's own needs, and loads only
#   the system's libraries and @rpath/winemac.so and @rpath/ntdll.so
# - every symbol it imports from winemac.so and ntdll.so is exported by the engine's files, and so
#   is macdrv_functions, which it looks up at run time (the table's layout can't be read off a file;
#   docs/gw2/README.md says where it was checked)
# - d3d11.dll carries the version that VERSION names
# - every section of each ARM64X DLL is byte for byte its unstripped copy's, and the copy has
#   DWARF (.debug_info), so the symbols describe the code that runs
emulate -L zsh
setopt err_exit pipe_fail no_unset

(( $# == 4 )) || { print -u2 "usage: check-mach9.zsh <dist> <engine> <llvm-readobj> <symbols>"; exit 2 }
dist=${1:A} engine=${2:A} readobj=$3 symbols=${4:A} here=${0:A:h}
mine_dxmt=$dist engine_dxmt=$engine/lib/wine/dxgi/dxmt engine_unix=$engine/lib/wine/aarch64-unix
dlls=(d3d11 dxgi d3d10core winemetal)
integer failed=0

ok() { print -r -- "ok    $*" }
fail() { print -r -- "FAIL  $*"; failed+=1 }

# the PE machine field, as 4 hex digits
pe_machine() {
  local off=$(od -An -tu4 -j60 -N4 "$1" | tr -d ' ')
  od -An -tx2 -j$((off + 4)) -N2 "$1" | tr -d ' '
}
# llvm-readobj's name for the format: COFF-ARM64X only for an ARM64 image with ARM64EC metadata
pe_format() { "$readobj" --file-headers "$1" | sed -n 's/^Format: //p' | head -1 }
# "<view> <name>" for every named export with an address (RVA 0 is no export); an ARM64X image has
# a native view and an ec view. llvm-readobj prints Ordinal, Name, RVA for each.
pe_exports() {
  "$readobj" --coff-exports "$1" |
    awk '/^HybridObject/ { view = "ec" }
         $1 == "Name:" { name = (NF > 1) ? $2 : "" }
         $1 == "RVA:" && name != "" && $2 !~ /^0x0+$/ { print (view ? view : "native"), name; name = "" }' |
    sort -u
}
# "<view> <name>" for every name $1 imports from the DLL $2 (lower case). llvm-readobj prints
# "Symbol: <name> (<hint>)" for each, after the import's Name.
pe_imports() {
  "$readobj" --coff-imports "$1" |
    awk -v dll="$2" '/^HybridObject/ { view = "ec" }
         $1 == "Name:" { from = tolower($2) }
         $1 == "Symbol:" && from == dll && NF > 2 { print (view ? view : "native"), $2 }' |
    sort -u
}

for tree in aarch64-windows i386-windows; do
  for dll in $dlls; do
    mine=$mine_dxmt/$tree/$dll.dll theirs=$engine_dxmt/$tree/$dll.dll
    [[ -f $mine ]] || { fail "$tree/$dll.dll is missing"; continue }
    [[ -f $theirs ]] || { fail "the engine has no $tree/$dll.dll to compare with"; continue }

    m=$(pe_machine "$mine") t=$(pe_machine "$theirs") mf=$(pe_format "$mine") tf=$(pe_format "$theirs")
    if [[ $tree == aarch64-windows ]]; then want=aa64 wantf=COFF-ARM64X; else want=014c wantf=COFF-i386; fi
    if [[ $m == $t && $m == $want && $mf == $tf && $mf == $wantf ]]; then
      ok "$tree/$dll.dll is $mf (machine $m), as the engine's"
    else
      fail "$tree/$dll.dll is $mf (machine $m), the engine's is $tf (machine $t), expected $wantf"
    fi

    if [[ $(dd if="$mine" bs=1 skip=64 count=16 2>/dev/null) == "Wine builtin DLL" ]]; then
      ok "$tree/$dll.dll is marked as a Wine builtin"
    else
      fail "$tree/$dll.dll has no Wine builtin marker"
    fi

    missing=$(comm -23 <(pe_exports "$theirs") <(pe_exports "$mine"))
    extra=$(comm -13 <(pe_exports "$theirs") <(pe_exports "$mine") | wc -l | tr -d ' ')
    if [[ -z $missing ]]; then
      ok "$tree/$dll.dll exports all $(pe_exports "$theirs" | wc -l | tr -d ' ') of the engine's (view name), $extra more"
    else
      fail "$tree/$dll.dll lacks exports the engine's has:"$'\n'"      ${(pj:\n      :)${(f)missing}}"
    fi
  done

  # winemetal.dll's unix call setup (winecrt0) and its few CRT functions come from ntdll.dll
  mine=$mine_dxmt/$tree/winemetal.dll ntdll=$engine/lib/wine/$tree/ntdll.dll
  if [[ ! -f $ntdll ]]; then
    fail "the engine has no $tree/ntdll.dll to compare with"
  elif [[ -f $mine ]]; then
    imports=$(pe_imports "$mine" ntdll.dll) ntdll_exports=$(pe_exports "$ntdll")
    views=(${(u)${${(f)imports}%% *}})
    absent=$(comm -23 <(print -r -- "$imports") <(print -r -- "$ntdll_exports"))
    if [[ -z $imports ]]; then
      fail "$tree/winemetal.dll imports nothing from ntdll.dll"
    elif [[ -n $absent ]]; then
      fail "$tree/winemetal.dll imports from ntdll.dll what the engine's doesn't export:"$'\n'"      ${(pj:\n      :)${(f)absent}}"
    else
      ok "$tree/winemetal.dll imports ${#${(f)imports}} names from ntdll.dll (views: $views), all exported by the engine's"
    fi
    lookup=(${^views}" __wine_unix_call_dispatcher")
    if strings -a "$mine" | grep -x __wine_unix_call_dispatcher >/dev/null && (( ${#views} )) &&
      [[ -z $(comm -23 <(print -rl -- $lookup | sort) <(print -r -- "$ntdll_exports")) ]]; then
      ok "$tree/winemetal.dll looks up __wine_unix_call_dispatcher, and the engine's ntdll.dll exports it (views: $views)"
    else
      fail "__wine_unix_call_dispatcher: not looked up by $tree/winemetal.dll, or not exported by the engine's ntdll.dll in each of its views"
    fi
  fi
done

# the engine's tree links its winemetal.so to the one in lib/wine/aarch64-unix
so=$mine_dxmt/aarch64-unix/winemetal.so theirs_so=$engine_dxmt/aarch64-unix/winemetal.so
theirs_so=${theirs_so:A}
if [[ ! -f $so ]]; then
  fail "aarch64-unix/winemetal.so is missing"
else
  [[ $(lipo -archs "$so") == arm64 ]] && ok "winemetal.so is arm64 only" || fail "winemetal.so is $(lipo -archs "$so")"
  [[ $(otool -hv "$so" | awk 'NR == 4 { print $5 }') == DYLIB ]] && ok "winemetal.so is a dylib" ||
    fail "winemetal.so isn't a dylib: $(otool -hv "$so" | tail -1)"

  # LC_BUILD_VERSION's minos, or the older LC_VERSION_MIN_MACOSX's version
  minos() {
    otool -l "$1" | awk '$1 == "cmd" { c = $2 }
      (c == "LC_BUILD_VERSION" && $1 == "minos") || (c == "LC_VERSION_MIN_MACOSX" && $1 == "version") { print $2; exit }'
  }
  mine_os=$(minos "$so") theirs_os=$(minos "$theirs_so")
  if [[ -z $mine_os || -z $theirs_os ]]; then
    fail "winemetal.so's macOS floor unread (mine: ${mine_os:-none}, the engine's: ${theirs_os:-none})"
  elif [[ $(printf '%s\n' "$mine_os" "$theirs_os" | sort -V | tail -1) == "$theirs_os" ]]; then
    ok "winemetal.so needs macOS $mine_os, the engine's $theirs_os"
  else
    fail "winemetal.so needs macOS $mine_os, newer than the engine's $theirs_os"
  fi

  # every library it loads (otool -L: line 1 is the file, line 2 its own id)
  libs=("${(@f)$(otool -L "$so" | awk 'NR > 2 { print $1 }')}")
  bad=() wine=()
  for lib in $libs; do
    case $lib in
      @rpath/winemac.so | @rpath/ntdll.so) wine+=("$lib") ;;
      /System/Library/* | /usr/lib/*) ;;
      *) bad+=("$lib") ;;
    esac
  done
  (( ${#bad} == 0 )) && ok "winemetal.so loads only system libraries besides Wine's" ||
    fail "winemetal.so loads libraries the engine may not have: $bad"
  (( ${#wine} == 2 )) && ok "winemetal.so loads @rpath/winemac.so and @rpath/ntdll.so, as the engine's" ||
    fail "winemetal.so loads Wine's ${wine:-nothing}, not @rpath/winemac.so and @rpath/ntdll.so"
  print "      its rpaths: ${(j:, :)${(f)$(otool -l "$so" | awk '$1 == "cmd" { c = $2 } c == "LC_RPATH" && $1 == "path" { print $2 }')}:-none}"

  for lib in winemac ntdll; do
    exported=$(nm -gU "$engine_unix/$lib.so" | awk '{ print $3 }' | sort -u)
    imports=("${(@f)$(nm -m "$so" | awk -v lib="$lib" 'index($0, "(from " lib ")") || index($0, "(from @rpath/" lib ".so)") { print $(NF - 2) }' | sort -u)}")
    imports=(${imports:#})
    exported_list=("${(@f)exported}")
    absent=(${imports:|exported_list})
    if (( ${#absent} )); then
      fail "winemetal.so imports from $lib.so what the engine's doesn't export: $absent"
    else
      ok "winemetal.so imports ${#imports} symbols from $lib.so, all exported by the engine's"
    fi
  done
  if strings -a "$so" | grep -x macdrv_functions >/dev/null &&
    nm -gU "$engine_unix/winemac.so" | grep ' _macdrv_functions$' >/dev/null; then
    ok "winemetal.so looks up macdrv_functions, and the engine's winemac.so exports it"
  else
    fail "macdrv_functions: not looked up by winemetal.so, or not exported by the engine's winemac.so"
  fi
fi

version=$(head -1 "$dist/VERSION" 2>/dev/null || true)
# grep without -q, which would end the pipe early and fail it under pipe_fail
if [[ -n $version ]] && strings -a "$mine_dxmt/aarch64-windows/d3d11.dll" | grep -xF "$version" >/dev/null; then
  ok "d3d11.dll carries $version, VERSION's first line"
else
  fail "d3d11.dll doesn't carry VERSION's first line (${version:-no VERSION})"
fi

for dll in $dlls; do
  mine=$mine_dxmt/aarch64-windows/$dll.dll sym=$symbols/aarch64-windows/$dll.dll
  [[ -f $mine ]] || continue
  [[ -f $sym ]] || { fail "symbols: aarch64-windows/$dll.dll is missing"; continue }
  # grep without -q, as above
  if "$readobj" --sections "$sym" | grep -F 'Name: .debug_info ' >/dev/null; then
    ok "symbols: $dll.dll has DWARF"
  else
    fail "symbols: $dll.dll has no .debug_info section"
  fi
  # every section the shipped DLL has, at the same RVA with the same bytes; the copy has the debug
  # sections on top
  shipped=("${(@f)$(python3 -I "$here/pe-sections.py" "$mine")}")
  copy=("${(@f)$(python3 -I "$here/pe-sections.py" "$sym")}")
  differ=(${shipped:|copy})
  if (( ${#differ} == 0 )) && [[ ${(M)shipped:#.text *} ]]; then
    ok "symbols: $dll.dll's ${#shipped} sections are its unstripped copy's, byte for byte"
  else
    fail "symbols: $dll.dll's sections differ from its unstripped copy's (name, RVA, size, sha256):"$'\n'"      shipped  ${(pj:\n               :)differ}"$'\n'"      symbols  ${(pj:\n               :)copy}"
  fi
done

(( failed == 0 )) && print "all checks passed" || { print "$failed checks failed"; exit 1 }
