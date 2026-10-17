#!/usr/bin/env python3
"""Job-control tests that drive ysh through a pty.

Usage: python3 tests/test_interactive.py ./ysh
"""
import os
import pty
import re
import select
import signal
import sys
import time

YSH = os.path.realpath(sys.argv[1] if len(sys.argv) > 1 else "./ysh")
CTRL_C, CTRL_Z, CTRL_D = "\x03", "\x1a", "\x04"
PROMPT = re.compile(r"ysh:[^\n]*\$ $")


class Session:
    def __init__(self):
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.environ["HOME"] = "/tmp"
            os.chdir("/tmp")
            os.execv(YSH, [YSH])
        self.buf = ""
        self.expect_prompt()

    def read(self, timeout):
        end = time.time() + timeout
        while time.time() < end:
            r, _, _ = select.select([self.fd], [], [], max(0, end - time.time()))
            if not r:
                break
            try:
                data = os.read(self.fd, 4096).decode(errors="replace")
            except OSError:
                break
            if not data:
                break
            self.buf += data
            return True
        return False

    def expect(self, pattern, timeout=5):
        rx = re.compile(pattern) if isinstance(pattern, str) else pattern
        end = time.time() + timeout
        while True:
            m = rx.search(self.buf)
            if m:
                seen, self.buf = self.buf[: m.end()], self.buf[m.end():]
                return seen
            if time.time() > end or not self.read(end - time.time()):
                if time.time() > end:
                    raise AssertionError(f"timed out waiting for {rx.pattern!r}; got {self.buf!r}")

    def expect_prompt(self, timeout=5):
        return self.expect(PROMPT, timeout)

    def send(self, text):
        os.write(self.fd, text.encode())

    def run(self, line, timeout=5):
        self.send(line + "\n")
        out = self.expect_prompt(timeout)
        out = out.replace("\r\n", "\n")
        out = out[out.find("\n") + 1:] if "\n" in out else out  # drop the echoed command
        return PROMPT.sub("", out)

    def close(self):
        try:
            os.kill(self.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        os.waitpid(self.pid, 0)


tests = []


def test(fn):
    tests.append(fn)
    return fn


@test
def runs_commands():
    s = Session()
    out = s.run("echo hi | tr a-z A-Z")
    assert "HI" in out, out
    s.close()


@test
def ctrl_c_at_prompt_clears_the_line():
    s = Session()
    s.send("half typed command")
    s.send(CTRL_C)
    s.expect_prompt()
    assert "ok" in s.run("echo ok")
    s.close()


@test
def ctrl_c_stops_foreground_job_not_shell():
    s = Session()
    s.send("sleep 30\n")
    time.sleep(0.5)
    s.send(CTRL_C)
    s.expect_prompt()
    assert "130" in s.run("echo $?")  # 128 + SIGINT(2)
    s.close()


@test
def ctrl_c_stops_whole_pipeline():
    s = Session()
    s.send("sleep 30 | sleep 30 | sleep 30\n")
    time.sleep(0.5)
    s.send(CTRL_C)
    s.expect_prompt()
    assert s.run("jobs").strip() == "", "no jobs should be left"
    s.close()


@test
def ctrl_z_then_jobs_bg_fg():
    s = Session()
    s.send("sleep 30\n")
    time.sleep(0.5)
    s.send(CTRL_Z)
    stopped = s.expect_prompt()
    assert re.search(r"\[1\]\+\s+Stopped\s+sleep 30", stopped), stopped

    assert re.search(r"\[1\]\+\s+Stopped\s+sleep 30", s.run("jobs"))
    assert "[1]+ sleep 30 &" in s.run("bg %1")
    assert re.search(r"\[1\]\+\s+Running\s+sleep 30 &", s.run("jobs"))

    s.send("fg\n")
    s.expect("sleep 30")
    time.sleep(0.3)
    s.send(CTRL_C)
    s.expect_prompt()
    assert "130" in s.run("echo $?")
    s.close()


@test
def background_job_reports_done():
    s = Session()
    out = s.run("sleep 0.3 &")
    assert re.search(r"\[1\] \d+", out), out
    time.sleep(0.6)
    out = s.run("")  # pressing Enter shows finished jobs, like bash
    assert re.search(r"\[1\]\+\s+Done\s+sleep 0.3", out), out
    s.close()


@test
def foreground_program_can_read_the_terminal():
    s = Session()
    s.send("cat\n")
    time.sleep(0.3)
    s.send("typed into cat\n")
    s.expect("typed into cat.*\n.*typed into cat", timeout=5)  # the terminal echo, then cat's copy
    s.send(CTRL_D)
    s.expect_prompt()
    s.close()


@test
def exit_ends_the_shell_with_code():
    s = Session()
    s.send("exit 5\n")
    _, status = os.waitpid(s.pid, 0)
    assert os.WIFEXITED(status) and os.WEXITSTATUS(status) == 5, status


@test
def ctrl_d_ends_the_shell():
    s = Session()
    s.send(CTRL_D)
    _, status = os.waitpid(s.pid, 0)
    assert os.WIFEXITED(status), status


failed = 0
print("Interactive tests (real terminal)")
for fn in tests:
    try:
        fn()
        print(f"  pass  {fn.__name__.replace('_', ' ')}")
    except Exception as e:  # noqa: BLE001
        failed += 1
        print(f"  FAIL  {fn.__name__.replace('_', ' ')}: {e}")
print(f"{len(tests) - failed}/{len(tests)} passed")
sys.exit(1 if failed else 0)
