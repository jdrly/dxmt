#!/bin/zsh
# gw2 (B2, jdrly/dxmt #67): part of bridge_manifest_test (run by tests/gw2/run.zsh). Builds the call
# manifest's checks (winemetal_call_numbers.h) against mutated copies of src/winemetal/winemetal_calls.h
# and expects every mutation to fail the build, and the unmutated copy to pass.
# gw2 (B3, #76): then the same for the PE thunks (winemetal_thunks.c, built natively): a thunk that
# passes a literal call number, or a name the manifest doesn't have, must fail to build. And no
# bridge call in src/winemetal/*.c may bypass the naming macros (a raw WINE_UNIX_CALL).
#
#   tests/gw2/bridge_manifest_reject.zsh <scratch dir>     (from the checkout's root)
emulate -L zsh
setopt err_exit pipe_fail no_unset

dir=${1:?usage: bridge_manifest_reject.zsh <scratch dir>}
manifest=src/winemetal/winemetal_calls.h
mkdir -p $dir
print '#include "winemetal_thunks.h"' >$dir/tu.c
failures=0

# check <name> <expect: pass|fail> <sed script, applied to the manifest>
check() {
  local name=$1 expect=$2 script=$3 h=$dir/$1 got
  rm -rf $h && mkdir -p $h
  cp src/winemetal/winemetal.h src/winemetal/winemetal_thunks.h src/winemetal/winemetal_call_numbers.h $h/
  sed -e "$script" $manifest >$h/winemetal_calls.h
  if [[ $expect == fail ]] && cmp -s $manifest $h/winemetal_calls.h; then
    print "FAIL $name: the mutation changed nothing"; (( ++failures )); return
  fi
  if clang -std=c11 -fsyntax-only -I$h $dir/tu.c >$h/build.log 2>&1; then got=pass; else got=fail; fi
  if [[ $got == $expect ]]; then
    print "ok   $name: the build ${got}ed"
  else
    print "FAIL $name: expected the build to $expect, it ${got}ed ($h/build.log)"; (( ++failures ))
  fi
}

check unchanged pass ''
check duplicate-number fail 's/^WMT_CALL(1, NSObject_release,/WMT_CALL(0, NSObject_release,/'
check gap fail '/^WMT_CALL(5, /d'
check out-of-order fail '/^WMT_CALL(2, /{h;d;}; /^WMT_CALL(3, /G'
check duplicate-name fail 's/^WMT_CALL(1, NSObject_release, _NSObject_release)/WMT_CALL(1, NSObject_retain, _NSObject_release)/'
check reserved-without-reason fail 's/^WMT_RESERVED(83, .*/WMT_RESERVED(83)/'
# every line still at its own number, but MetalView_visibility now 146: only the fixed anchor fails
check anchor-moved fail 's/^WMT_CALL(147, MetalView_visibility,/WMT_CALL(147, MetalView_visibilityX,/; s/^WMT_CALL(146, MTLCommandBuffer_presentDrawableWithDeadline,/WMT_CALL(146, MetalView_visibility,/'

# check_thunks <name> <expect: pass|fail> <sed script, applied to winemetal_thunks.c>
check_thunks() {
  local name=$1 expect=$2 script=$3 h=$dir/$1 got
  rm -rf $h && mkdir -p $h
  sed -e "$script" src/winemetal/winemetal_thunks.c >$h/winemetal_thunks.c
  if [[ $expect == fail ]] && cmp -s src/winemetal/winemetal_thunks.c $h/winemetal_thunks.c; then
    print "FAIL $name: the mutation changed nothing"; (( ++failures )); return
  fi
  # nativemetal first: its wineunixlib.h (the native dispatcher), as the native build has it
  if clang -std=gnu17 -fsyntax-only -DDXMT_NATIVE=1 -Isrc/nativemetal -Isrc/winemetal $h/winemetal_thunks.c \
      >$h/build.log 2>&1; then got=pass; else got=fail; fi
  if [[ $got == $expect ]]; then
    print "ok   $name: the build ${got}ed"
  else
    print "FAIL $name: expected the build to $expect, it ${got}ed ($h/build.log)"; (( ++failures ))
  fi
}

check_thunks thunks-unchanged pass ''
check_thunks thunk-literal-number fail 's/UNIX_CALL(NSObject_release, &obj)/UNIX_CALL(1, \&obj)/'
check_thunks thunk-unknown-name fail 's/UNIX_CALL(NSObject_release, &obj)/UNIX_CALL(NSObject_releas, \&obj)/'
check_thunks thunk-reserved-name fail 's/UNIX_CALL(NSObject_release, &obj)/UNIX_CALL(reserved_83, \&obj)/'

# every bridge call in the PE sources goes through a naming macro: WINE_UNIX_CALL only in a #define
raw=$(grep -nE '\bWINE_UNIX_CALL\(' src/winemetal/*.c | grep -vE '^[^:]+:[0-9]+:\s*(#\s*define |NTSTATUS status = WINE_UNIX_CALL\(WMTCall_##call)' || true)
if [[ -n $raw ]]; then
  print "FAIL raw-unix-call: a bridge call bypasses UNIX_CALL / UNIX_CALL_STATUS:"; print -r -- $raw; (( ++failures ))
else
  print "ok   raw-unix-call: every bridge call in src/winemetal/*.c is named"
fi

(( failures == 0 )) || { print "$failures check(s) failed"; exit 1 }
print "bridge manifest rejects: OK"
