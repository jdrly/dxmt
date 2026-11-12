#!/bin/zsh
# gw2 (#199): name the functions at addresses in my arm64 build, from the symbols of the same commit
# (the artifact dxmt-arm64-symbols-<commit>, or dxmt-arm64-symbols-<sha>.tar.xz on arm64-builds,
# unpacked; VERSION in both names the commit).
#
#   symbolize.zsh <symbols dir> <dll> <rva>...
#
# <dll> is d3d11.dll, dxgi.dll, d3d10core.dll, winemetal.dll (aarch64-windows) or winemetal.so
# (aarch64-unix). An RVA is a sampled address minus the file's base, the start of its first region in
# vmmap, written as 0x-prefixed hex. For each RVA it prints the function, its source line and the
# functions inlined there, through llvm-symbolizer: $LLVM_SYMBOLIZER, or the one on PATH (llvm-mingw
# has one).
emulate -L zsh
setopt err_exit pipe_fail no_unset

(( $# >= 3 )) || { print -u2 "usage: symbolize.zsh <symbols dir> <dll> <rva>..."; exit 2 }
dir=$1 dll=$2
shift 2
case $dll in
  *.so) file=$dir/aarch64-unix/$dll ;;
  *) file=$dir/aarch64-windows/$dll ;;
esac
[[ -f $file ]] || { print -u2 "no $file"; exit 2 }
exec "${LLVM_SYMBOLIZER:-llvm-symbolizer}" --obj="$file" --relative-address --pretty-print --print-address "$@"
