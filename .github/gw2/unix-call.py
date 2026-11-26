# gw2 (#205): what a bridge thunk calls, in one target's build. It preprocesses winemetal_thunks.c
# with winemetal.dll's own command (compile_commands.json, without -marm64x, so an ARM64X build shows its
# ARM64EC half) and prints NSObject_retain's call. The call has to be <callee>(__wine_unixlib_handle,
# ...). With <llvm-objdump> it also disassembles #NSObject_retain, the ARM64EC code, from the build's
# unstripped winemetal.dll, which has to make no indirect call and branch straight to an address that
# is winecrt0's ARM64EC __wine_unix_call_arm64ec: llvm-nm names that address so, the image's CHPE code
# map puts it in an ARM64EC range (a native function of the same name would be in an ARM64 one), and
# the routine ends in winecrt0's br x16. llvm-nm and llvm-readobj are taken from llvm-objdump's
# directory.
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
    if re.search(r"\sblr\s", body):
        sys.exit("FAIL  #NSObject_retain makes an indirect call")
    targets = re.findall(r"\sbl?\s+0x([0-9a-f]+) <", body)
    if len(targets) != 1:
        sys.exit(f"FAIL  #NSObject_retain has {len(targets)} direct branches, expected the one call")
    target = int(targets[0], 16)

    # the names at that address (llvm-nm prints virtual addresses for an image)
    tools = os.path.dirname(objdump)
    nm = subprocess.run([os.path.join(tools, "llvm-nm"), dll], check=True, capture_output=True, text=True).stdout
    names = sorted({m.group(2) for m in (re.match(r"([0-9a-fA-F]+) \S (\S+)$", l) for l in nm.splitlines())
                    if m and int(m.group(1), 16) == target})
    print(f"      the call's target {target:#x}: {names}")
    if not {"__wine_unix_call_arm64ec", "#__wine_unix_call_arm64ec"} & set(names):
        sys.exit("FAIL  the call's target isn't __wine_unix_call_arm64ec")

    # its kind of code, from the CHPE code map (RVA ranges; the hybrid view repeats the map)
    readobj = os.path.join(tools, "llvm-readobj")
    headers = subprocess.run([readobj, "--file-headers", dll], check=True, capture_output=True, text=True).stdout
    rva = target - int(re.search(r"ImageBase: (0x[0-9A-Fa-f]+)", headers).group(1), 16)
    config = subprocess.run([readobj, "--coff-load-config", dll], check=True, capture_output=True, text=True).stdout
    config = config.split("HybridObject")[0]
    kinds = [k for s, e, k in re.findall(r"(0x[0-9A-Fa-f]+) - (0x[0-9A-Fa-f]+)  (\S+)", config)
             if int(s, 16) <= rva < int(e, 16)]
    print(f"      RVA {rva:#x} in the code map: {kinds}")
    if kinds != ["ARM64EC"]:
        sys.exit("FAIL  the call's target isn't ARM64EC code")

    # winecrt0's routine: adrp/ldr x16 of its dispatcher pointer, then br x16
    stub = subprocess.run([objdump, "-d", f"--start-address={target:#x}", f"--stop-address={target + 12:#x}", dll],
                          check=True, capture_output=True, text=True).stdout
    print("\n".join(l for l in stub.splitlines() if re.match(r"\s*[0-9a-f]+:", l)))
    if not re.search(r"\sbr\s+x16\b", stub):
        sys.exit("FAIL  the call's target doesn't end in winecrt0's br x16")
    print("ok    #NSObject_retain branches straight to winecrt0's ARM64EC __wine_unix_call_arm64ec")
