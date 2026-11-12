#!/usr/bin/env python3
# Boot xv6 under qemu, type some commands at the shell, show what came back.
#
#   ./test-p5.py p5test                 run the whole suite
#   ./test-p5.py "semtest" "philo order"   run these, in order
#
# Exit status is 0 only if every command printed its PASS line.
# CPUS=1 ./test-p5.py ...  runs on a single hart.

import os, subprocess, sys, time, select, re

CPUS = os.environ.get("CPUS", "3")
TIMEOUT = int(os.environ.get("TIMEOUT", "240"))

def main():
    cmds = sys.argv[1:] or ["p5test"]
    q = subprocess.Popen(
        ["make", "-s", "qemu", "CPUS=" + CPUS],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        bufsize=0)
    buf = b""
    def pump(until, limit):
        nonlocal buf
        end = time.time() + limit
        while time.time() < end:
            r, _, _ = select.select([q.stdout], [], [], 0.2)
            if r:
                d = os.read(q.stdout.fileno(), 4096)
                if not d:
                    return False
                buf += d
                sys.stdout.write(d.decode(errors="replace"))
                sys.stdout.flush()
                if until in buf:
                    return True
        return False

    ok = pump(b"init: starting sh", 60) and pump(b"$ ", 30)
    if not ok:
        print("\n*** xv6 did not boot")
        q.kill()
        return 2
    status = 0
    for c in cmds:
        buf = b""
        q.stdin.write(c.encode() + b"\n")
        q.stdin.flush()
        if not pump(b"\n$ ", TIMEOUT):
            print("\n*** timeout waiting for: " + c)
            status = 1
            break
        if b"PASS" not in buf or b"FAIL" in buf or b"panic" in buf:
            status = 1
    q.stdin.write(b"\x01x")
    q.stdin.flush()
    try:
        q.wait(timeout=5)
    except subprocess.TimeoutExpired:
        q.kill()
    print("\n=== %s ===" % ("PASSED" if status == 0 else "FAILED"))
    return status

sys.exit(main())
