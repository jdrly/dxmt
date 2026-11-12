# gw2 (#205): what a bridge thunk calls, in one target's build. It preprocesses winemetal_thunks.c
# with winemetal.dll's own command (compile_commands.json, without -marm64x, so an ARM64X build shows its
# ARM64EC half) and prints NSObject_retain's call. The call has to be <callee>(__wine_unixlib_handle,
# ...). With <llvm-objdump> it also disassembles #NSObject_retain, the ARM64EC code, from the build's
# unstripped winemetal.dll, which has to branch straight to #__wine_unix_call_arm64ec and make no
# indirect call.
#
#   python3 -I unix-call.py <build dir> <callee> [<llvm-objdump>]
import json
import os
import re
import shlex
import subprocess
import sys

build, callee = sys.argv[1], sys.argv[2]
objdump = sys.argv[3] if len(sys.argv) > 3 else None

# winemetal.dll's compile, not bridge_native_smoke's (the x86_64 build compiles the file for both)
entry = next(e for e in json.load(open(os.path.join(build, "compile_commands.json")))
             if e["file"].endswith("src/winemetal/winemetal_thunks.c") and "winemetal.dll.p/" in e["command"])
args = shlex.split(entry["command"])
cmd, skip = [], False
for a in args:
    if skip:
        skip = False
    elif a in ("-o", "-MQ", "-MF", "-MT"):
        skip = True
    elif a not in ("-c", "-MD", "-MMD", "-marm64x"):
        cmd.append(a)
out = subprocess.run(cmd + ["-E", "-P"], cwd=entry["directory"], check=True,
                     capture_output=True, text=True).stdout
calls = [line.strip() for line in out.splitlines() if "(WMTCall_NSObject_retain)" in line]
print(f"{cmd[0]}: {calls}")
if len(calls) != 1 or f"{callee}(__wine_unixlib_handle, (WMTCall_NSObject_retain), (&obj))" not in calls[0]:
    sys.exit(f"FAIL  NSObject_retain doesn't call {callee}")
print(f"ok    NSObject_retain calls {callee}")

if objdump:
    dll = os.path.join(build, "src/winemetal/winemetal.dll")
    dis = subprocess.run([objdump, "-d", "--disassemble-symbols=#NSObject_retain", dll],
                         check=True, capture_output=True, text=True).stdout
    body = dis[dis.find("<#NSObject_retain>:"):] if "<#NSObject_retain>:" in dis else ""
    print(body.rstrip())
    if not body:
        syms = subprocess.run([objdump, "-t", dll], capture_output=True, text=True).stdout
        print("\n".join(l for l in syms.splitlines() if "NSObject_retain" in l))
        sys.exit("FAIL  no #NSObject_retain in winemetal.dll's symbol table")
    if not re.search(r"\sbl?\s+0x[0-9a-f]+ <#__wine_unix_call_arm64ec>", body) or re.search(r"\sblr\s", body):
        sys.exit("FAIL  #NSObject_retain doesn't branch straight to #__wine_unix_call_arm64ec")
    print("ok    #NSObject_retain branches straight to #__wine_unix_call_arm64ec")
