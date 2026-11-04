#!/bin/zsh
# gw2 (B2, jdrly/dxmt #67): part of bridge_manifest_test (run by tests/gw2/run.zsh). Builds the call
# manifest's checks (winemetal_thunks.h) against mutated copies of src/winemetal/winemetal_calls.h
# and expects every mutation to fail the build, and the unmutated copy to pass.
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
  cp src/winemetal/winemetal.h src/winemetal/winemetal_thunks.h $h/
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

(( failures == 0 )) || { print "$failures check(s) failed"; exit 1 }
print "bridge manifest rejects: OK"
