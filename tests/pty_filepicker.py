#!/usr/bin/env python3
"""Drives examples/demo in a real pty: focus the file picker, type a filter, choose the file, read the screen bytes.
No terminal emulator needed: the demo redraws changed cells, and the strings below only appear after the keys did their job.
Usage: tests/pty_filepicker.py   (after `make`); exit 0 = pass."""
import os, re, pty, select, struct, fcntl, termios, sys, time, tempfile, shutil

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
work = tempfile.mkdtemp(prefix="ltpty-")
for n in ("hello.txt", "other.md"):
    open(os.path.join(work, n), "w").close()
os.mkdir(os.path.join(work, "sub"))

pid, fd = pty.fork()
if pid == 0:
    os.chdir(work)
    os.environ.update(TERM="xterm-256color", LT_FOCUS="filepick")
    os.execv(os.path.join(ROOT, "examples", "demo"), ["demo"])
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 70, 120, 0, 0))

buf = b""
def pump(secs):
    global buf
    end = time.time() + secs
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.05)
        if r:
            try:
                buf += os.read(fd, 65536)
            except OSError:
                return

def plain():
    return re.sub(rb"\x1b\[[0-9;?]*[A-Za-z]", b"", buf)            # the demo colours cell by cell: drop the escapes, the text is left in order

def need(text, why):
    if text.encode() not in plain():
        print("FAIL:", why, "- missing", repr(text)); finish(1)

def finish(code):
    try:
        os.write(fd, b"\x1b"); pump(0.3); os.kill(pid, 15)
    except OSError:
        pass
    shutil.rmtree(work, ignore_errors=True)
    sys.exit(code)

pump(1.5)
if os.environ.get("DBG"): print(plain()[-1500:].decode("utf8","replace"))
need("hello.txt", "the start directory is listed")
need("nothing chosen yet", "the chosen-file panel starts empty")
buf = b""
os.write(fd, b"hello"); pump(0.6)
need("hello", "typed text reaches the filter row")
os.write(fd, b"\x1b[B"); pump(0.3)        # Down: ".." is row 0, hello.txt row 1 after the filter
os.write(fd, b"\r"); pump(0.8)
if os.environ.get("DBG"): print(plain()[-2500:].decode("utf8","replace"))
need(os.path.join(os.path.realpath(work), "hello.txt"), "Enter on a file shows its absolute path in the chosen-file panel")
print("ok: filepicker in the demo, driven through a pty")
finish(0)
