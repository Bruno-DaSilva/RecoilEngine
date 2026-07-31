#!/usr/bin/env python3
"""Whole-process census of calls on RenderDoc's unsupported list.

The first such call permanently disables capture for the process, so what
matters is not the per-frame count but (a) which call comes FIRST and (b) the
complete set that has to reach zero -- startup and loading included.

Reads `apitrace dump` on stdin.
"""
import collections
import re
import sys

CALL = re.compile(r"^(\d+) (gl[A-Za-z0-9_]+)\(")
PUSH_MSG = re.compile(r'"([^"]*)"\s*\)\s*$')

unsupported = set(open(sys.argv[1]).read().split())

frame = 0            # incremented on each swap; 0 == before the first swap
stack = []
total = 0
offending = 0
first = None
by_func = collections.Counter()
by_scope = collections.Counter()
by_phase = collections.Counter()
startup_funcs = collections.Counter()

for line in sys.stdin:
    m = CALL.match(line)
    if not m:
        continue
    num, fn = m.group(1), m.group(2)
    total += 1

    if fn == "glXSwapBuffers":
        frame += 1
        continue
    if fn == "glPushDebugGroup":
        g = PUSH_MSG.search(line)
        stack.append(g.group(1) if g else "?")
        continue
    if fn == "glPopDebugGroup":
        if stack:
            stack.pop()
        continue

    if fn in unsupported:
        offending += 1
        scope = "/".join(stack) if stack else "<no group>"
        by_func[fn] += 1
        by_scope[scope] += 1
        by_phase["startup (before first swap)" if frame == 0 else "after first swap"] += 1
        if frame == 0:
            startup_funcs[fn] += 1
        if first is None:
            first = (num, fn, frame, scope)

print("total GL calls in process : %d" % total)
print("swaps (frames)            : %d" % frame)
print("calls on RenderDoc's unsupported list: %d  (%.2f%% of all calls)"
      % (offending, 100.0 * offending / max(total, 1)))
print()
if first:
    print("FIRST offending call -- this is what kills capture:")
    print("  call #%s  %s   (frame %s, scope %s)" % first)
print()
print("phase split:")
for k, v in by_phase.most_common():
    print("  %-32s %10d" % (k, v))
print()
print("top offending functions (whole process):")
for k, v in by_func.most_common(20):
    print("  %-28s %10d" % (k, v))
print()
print("top offending functions BEFORE the first swap (startup only):")
for k, v in startup_funcs.most_common(15):
    print("  %-28s %10d" % (k, v))
print()
print("top scopes (whole process):")
for k, v in by_scope.most_common(12):
    print("  %10d  %s" % (v, k))
