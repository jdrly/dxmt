#!/usr/bin/env python3
"""gw2 (#230, for #223): count a WindowServer `sample` call graph the way #223's samples were read.

  ws_sample_count.py <sample.txt>

One line, for present_cursor.zsh's RESULT line:
  frameinfo cb   samples under frame_info_callback in the com.apple.coreanimation.frameinfo.* threads;
            swapwait   of them kern_SwapWait
            mutex      of them a mutex wait (psynch_mutexwait, a firstfit lock wait, an unfair lock or
                       ulock wait) under setFrameInfoCallback:'s block
  cursor cb      samples under cursor_callback in the com.apple.coreanimation.cursor.* threads;
         set     of them set_cursor_surface
  main           in ws_main_thread: finish_update, of it kern_SwapBegin and kern_SwapEnd,
                 update_detached_layer, copy_surface (the copy), CompositorMetal::composite
  n              the main thread's sample count, the sample's length in samples
Counts are inclusive per thread: a symbol counts on every call-graph line naming it that has no
ancestor line naming it too, so recursion isn't counted twice. On my Mac (#223) Metal 3 had frameinfo
cb 2 or 3 in 600 and Metal 4 11 to 53.
"""
import re
import sys

LINE = re.compile(r"^(?P<lead>[ +!:|]*)(?P<count>\d+) (?P<rest>.*)$")
MUTEX = re.compile(r"psynch_mutexwait|_pthread_mutex_firstfit_lock_wait|os_unfair_lock_lock_with_options|__ulock_wait")
THREAD = re.compile(r"^Thread_\d+(?::\s*(?P<name>.*))?$")


def parse(path):
    """[(thread name, its sample count, [(count, text, ancestor texts)])]"""
    threads = []
    in_graph = False
    stack = []
    cur = None
    with open(path, encoding="utf-8", errors="replace") as f:
        for raw in f:
            line = raw.rstrip("\n")
            if line.startswith("Call graph:"):
                in_graph = True
                continue
            if not in_graph:
                continue
            if line.startswith(("Total number in stack", "Binary Images:", "Sort by top")):
                break
            m = LINE.match(line)
            if not m:
                continue
            depth = len(m.group("lead"))
            rest = m.group("rest")
            t = THREAD.match(rest)
            if t and depth <= 4:
                cur = [t.group("name") or "", int(m.group("count")), []]
                threads.append(cur)
                stack = []
                continue
            if cur is None:
                continue
            while stack and stack[-1][0] >= depth:
                stack.pop()
            cur[2].append((int(m.group("count")), rest, [s[1] for s in stack]))
            stack.append((depth, rest))
    return threads


def count(lines, sym, under=None):
    """sym: a substring or a compiled regex; under: a substring an ancestor must contain"""
    hit = (lambda t: sym.search(t) is not None) if hasattr(sym, "search") else (lambda t: sym in t)
    total = 0
    for c, text, anc in lines:
        if not hit(text) or any(hit(a) for a in anc):
            continue
        if under and not any(under in a for a in anc):
            continue
        total += c
    return total


def main():
    if len(sys.argv) != 2:
        print(__doc__.strip().splitlines()[2].strip(), file=sys.stderr)
        return 2
    threads = parse(sys.argv[1])
    if not threads:
        print("ws=unreadable (no call graph)")
        return 1
    fi = [t for t in threads if t[0].startswith("com.apple.coreanimation.frameinfo")]
    cu = [t for t in threads if t[0].startswith("com.apple.coreanimation.cursor")]
    mains = [t for t in threads if t[0] == "ws_main_thread"]
    m = mains[0] if mains else ["", 0, []]
    fields = [
        f"frameinfo cb={sum(count(t[2], 'frame_info_callback') for t in fi)}"
        f" swapwait={sum(count(t[2], 'kern_SwapWait', 'frame_info_callback') for t in fi)}"
        f" mutex={sum(count(t[2], MUTEX, 'setFrameInfoCallback') for t in fi)}"
        + ("" if fi else " (no frame-info thread)"),
        f"cursor cb={sum(count(t[2], 'cursor_callback') for t in cu)}"
        f" set={sum(count(t[2], 'set_cursor_surface', 'cursor_callback') for t in cu)}",
        f"main finish_update={count(m[2], 'finish_update')} swapbegin={count(m[2], 'kern_SwapBegin')}"
        f" swapend={count(m[2], 'kern_SwapEnd')} detached={count(m[2], 'update_detached_layer')}"
        f" copy={count(m[2], 'copy_surface')} composite={count(m[2], 'CompositorMetal::composite')}",
        f"n={m[1]}",
    ]
    print(" | ".join(fields))
    return 0


if __name__ == "__main__":
    sys.exit(main())
