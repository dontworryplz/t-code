#!/usr/bin/env python3
"""How often an idle micro writes to its terminal, shown and hidden.

Every write is work for t-code too (VTE parses it, the minimap and the indent guides may redraw),
times the number of open editors. Runs micro with t-code's config (micro/) in a pseudo-terminal,
with a control pipe like t-code's, and counts the writes per second while the tab counts as shown,
after "visible 0" (hidden) and after "visible 1" again. An idle hidden editor should write nothing.

  tools/micro-redraws.py [file] [micro flags...]    (default: src/project/run.c, -autosave 0 as t-code runs it)
"""
import os
import pty
import select
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "src", "project", "run.c")
    flags = sys.argv[2:] or ["-autosave", "0"]
    work = tempfile.mkdtemp(prefix="micro-redraws-")
    fifo = os.path.join(work, "control")
    copy = os.path.join(work, os.path.basename(path))  # micro may save: never the real file
    with open(path, "rb") as src, open(copy, "wb") as dst:
        dst.write(src.read())
    os.mkfifo(fifo)
    control = os.open(fifo, os.O_RDWR | os.O_NONBLOCK)

    pid, fd = pty.fork()
    if pid == 0:
        os.environ["TERM"] = "xterm-256color"
        os.environ["TCODE_CONTROL"] = fifo
        os.execvp("micro", ["micro", "-config-dir", os.path.join(ROOT, "micro")] + flags + [copy])

    def writes_per_second(seconds):
        counts = []
        for _ in range(seconds):
            n, end = 0, time.time() + 1
            while time.time() < end:
                if select.select([fd], [], [], 0.05)[0]:
                    os.read(fd, 65536)
                    n += 1
            counts.append(n)
        return counts

    writes_per_second(2)  # startup
    print("shown  ", writes_per_second(4))
    os.write(control, b"visible 0\n")
    writes_per_second(1)
    print("hidden ", writes_per_second(5))
    os.write(control, b"visible 1\n")
    print("shown  ", writes_per_second(4))
    os.kill(pid, 9)
    os.waitpid(pid, 0)
    for name in os.listdir(work):
        os.unlink(os.path.join(work, name))
    os.rmdir(work)


if __name__ == "__main__":
    main()
