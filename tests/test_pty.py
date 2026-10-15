"""Real Bash/Zsh sessions, with a second PTY representing the user's terminal.

All passwords here are disposable fixtures. Check the visible screen after
individual keypresses and check argv in a file written by the real child.
"""

import errno
import fcntl
import json
import os
from pathlib import Path
import pty
import select
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time
import unittest

import pyte


ROOT = Path(__file__).resolve().parents[1]


class TerminalScreen(pyte.Screen):
    """Accept private DSR queries emitted by tmux.

    pyte 0.8.2 dispatches DEC private status reports with a keyword that its
    base Screen method does not accept.  The tests do not emulate responses,
    but they must still be able to observe the resulting display.
    """

    def report_device_status(self, mode, **_kwargs):
        return super().report_device_status(mode)


def set_window_size(fd, lines, columns):
    """Set a PTY size on Python versions before termios.tcsetwinsize()."""
    if hasattr(termios, "tcsetwinsize"):
        termios.tcsetwinsize(fd, (lines, columns))
        return
    fcntl.ioctl(fd, termios.TIOCSWINSZ,
                struct.pack("HHHH", lines, columns, 0, 0))


class Session:
    def __init__(self, shell, columns=100, lines=24, config_dir=None, *,
                 clean=True, history_mode=None, extra_env=None):
        self.master, self.slave = pty.openpty()
        set_window_size(self.slave, lines, columns)
        self.original_tty = termios.tcgetattr(self.slave)
        env = os.environ.copy()
        env.pop("INCOGNITO_TERMINAL", None)
        # The test runner itself may be inside tmux.  Do not let that host
        # session disable the integration under test or redirect tmux clients
        # to the runner's server.
        env.pop("TMUX", None)
        env.pop("TMUX_PANE", None)
        env["TERM"] = "xterm-256color"
        # Ubuntu's global zshrc runs compinit. Hosted runners can contain
        # completion directories that trigger an interactive security prompt,
        # so keep that unrelated system configuration out of PTY tests.
        env["skip_global_compinit"] = "1"
        # Normal-mode tests must not write commands to the user's history file.
        env["HISTFILE"] = "/dev/null"
        self.config_temp = tempfile.TemporaryDirectory(prefix="privacy-test-config-") if config_dir is None else None
        env["XDG_CONFIG_HOME"] = self.config_temp.name if self.config_temp else str(config_dir)
        env.update(extra_env or {})
        binary = os.environ.get(
            "INCOGNITO_TERMINAL_BINARY",
            str(ROOT / "native" / "build" / "incognito-terminal"),
        )
        if not os.access(binary, os.X_OK):
            raise RuntimeError(f"Build the native executable before testing: {binary}")
        args = [binary]
        if clean:
            args.append("--clean")
        if history_mode:
            args += ["--history", history_mode]
        args += ["--", shell]
        self.process = subprocess.Popen(
            args,
            stdin=self.slave, stdout=self.slave, stderr=self.slave,
            cwd=ROOT, env=env, start_new_session=True,
        )
        self.screen = TerminalScreen(columns, lines)
        self.stream = pyte.ByteStream(self.screen)
        self.raw = bytearray()
        try:
            self.wait_for("incognito-terminal>")
        except Exception:
            self.close()
            raise

    def text(self):
        return "\n".join(line.rstrip() for line in self.screen.display)

    def read(self, duration=0.04):
        deadline = time.monotonic() + duration
        while time.monotonic() < deadline:
            ready, _, _ = select.select([self.master], [], [], max(0, deadline - time.monotonic()))
            if not ready:
                break
            try:
                chunk = os.read(self.master, 65536)
            except OSError as error:
                if error.errno == errno.EIO:
                    break
                raise
            if not chunk:
                break
            self.raw.extend(chunk)
            self.stream.feed(chunk)

    def send(self, data, duration=0.05):
        if isinstance(data, str):
            data = data.encode()
        while data:
            data = data[os.write(self.master, data):]
        self.read(duration)

    def wait_for(self, text, timeout=5):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.read(0.05)
            if text in self.text():
                return
            if self.process.poll() is not None:
                break
        raise AssertionError(f"Did not see {text!r}. Screen:\n{self.text()}")

    def resize(self, columns, lines):
        set_window_size(self.slave, lines, columns)
        self.screen.resize(lines=lines, columns=columns)
        os.kill(self.process.pid, signal.SIGWINCH)
        self.read(0.15)

    def close(self):
        if self.process.poll() is None:
            self.send(b"\x03", 0.05)
            self.send("exit\r", 0.15)
            try:
                self.process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.process.terminate()
                self.process.wait(timeout=2)
        restored = termios.tcgetattr(self.slave)
        self.read(0.01)
        os.close(self.master)
        os.close(self.slave)
        if self.config_temp:
            self.config_temp.cleanup()
        for marker in (b"ERROR: AddressSanitizer", b"ERROR: LeakSanitizer", b"runtime error:"):
            if marker in self.raw:
                raise AssertionError("Native sanitizer reported an error")
        return restored


class PtyTests(unittest.TestCase):
    def test_version_help_and_stale_tmux_marker(self):
        binary = os.environ.get(
            "INCOGNITO_TERMINAL_BINARY",
            str(ROOT / "native" / "build" / "incognito-terminal"),
        )
        version = subprocess.check_output([binary, "--version"], text=True)
        self.assertEqual(version, "incognito-terminal 1.3.3\n")
        help_text = subprocess.check_output([binary, "--help"], text=True)
        self.assertTrue(help_text.startswith("Incognito Terminal 1.3.3\n\nKeep passwords"))
        for section in (
            "Usage:",
            "Options:",
            "History modes:",
            "Session controls:",
            "Examples:",
            "More help: man incognito-terminal",
        ):
            self.assertIn(section, help_text)
        self.assertNotIn("native C", version + help_text)

        shell = shutil.which("zsh") or shutil.which("bash")
        session = Session(
            shell,
            extra_env={
                "INCOGNITO_TERMINAL": "1",
                "TMUX": "/tmp/tmux-stale/default,999,0",
            },
        )
        try:
            session.send("echo STALE_MARKER_ACCEPTED\r")
            session.wait_for("STALE_MARKER_ACCEPTED")
            session.send(f"{binary} --clean -- {shell}\r")
            session.wait_for("already inside a protected session")
            self.assertIsNone(session.process.poll())
        finally:
            self.assertEqual(session.close(), session.original_tty)

    def test_bash_and_zsh_masking_and_execution(self):
        for name in ("bash", "zsh"):
            shell = shutil.which(name)
            if shell is None:
                continue
            with self.subTest(shell=name), tempfile.TemporaryDirectory(prefix="incognito-terminal-test-") as folder:
                log = Path(folder, "argv.jsonl")
                recorder = Path(folder, "record.py")
                recorder.write_text(
                    "import json,sys\n"
                    f"with open({str(log)!r}, 'a') as f: f.write(json.dumps(sys.argv[1:])+'\\n')\n"
                    "print('EXECUTED')\n"
                )
                session = Session(shell)
                try:
                    session.send(f'record() {{ {sys.executable} {recorder} "$@"; }}\r')
                    session.send("record -p ")
                    for i, char in enumerate("V9secretX", 1):
                        session.send(char)
                        self.assertIn("record -p " + "*" * i, session.text())
                        self.assertNotIn("V9", session.text())
                        self.assertFalse(log.exists(), "typing must not execute the command")
                    session.send(b"\x7f")
                    session.send("Z next\r")
                    session.wait_for("EXECUTED")
                    self.assertEqual([json.loads(line) for line in log.read_text().splitlines()],
                                     [["-p", "V9secretZ", "next"]])

                    # Recalling history must remain masked and not execute yet.
                    session.send(b"\x1b[A")
                    self.assertNotIn("V9secretZ", session.text())
                    session.send(b"\x03")
                    self.assertEqual(len(log.read_text().splitlines()), 1)

                    # Entire quoted command arrives as one bracketed paste.
                    session.send(b"\x1b[200~record -p 'quoted fake secret' after\x1b[201~")
                    self.assertIn("-p '******************' after", session.text())
                    self.assertNotIn("quoted fake secret", session.text())
                    session.send("\r")
                    session.read(0.1)
                    self.assertEqual(json.loads(log.read_text().splitlines()[-1]),
                                     ["-p", "quoted fake secret", "after"])

                    session.send("record -pATTACHED_SECRET tail\r")
                    session.read(0.1)
                    self.assertEqual(json.loads(log.read_text().splitlines()[-1]),
                                     ["-pATTACHED_SECRET", "tail"])
                    self.assertEqual(len(log.read_text().splitlines()), 3)
                    self.assertNotIn(b"ATTACHED_SECRET", session.raw)
                    self.assertNotIn(b"quoted fake secret", session.raw)
                    self.assertNotIn(b"V9secret", session.raw)

                    cases = (
                        ("--password ", "LONG_SECRET", ["--password", "LONG_SECRET", "tail"]),
                        ("--password=", "EQUAL_SECRET", ["--password=EQUAL_SECRET", "tail"]),
                        ("/p:", "COLON_SECRET", ["/p:COLON_SECRET", "tail"]),
                        ("--password ", "'long fake value'", ["--password", "long fake value", "tail"]),
                        ("--password=", "'equal fake value'", ["--password=equal fake value", "tail"]),
                        ("/p:", "'colon fake value'", ["/p:colon fake value", "tail"]),
                        ("-p ", "abcdef'ghi'jkl", ["-p", "abcdefghijkl", "tail"]),
                        ("-p ", r"abc\'def", ["-p", "abc'def", "tail"]),
                        ("-p ", '"abc\'def"', ["-p", "abc'def", "tail"]),
                        ("-p ", "'abc\"def'", ["-p", 'abc"def', "tail"]),
                        ("--password ", '"double quoted value"', ["--password", "double quoted value", "tail"]),
                        ("--password=", r'"abc\"def"', ['--password=abc"def', "tail"]),
                        ("/p:", '"abc\'def"', ["/p:abc'def", "tail"]),
                    )
                    displays = {
                        "'long fake value'": "'***************'",
                        "'equal fake value'": "'****************'",
                        "'colon fake value'": "'****************'",
                        "abcdef'ghi'jkl": "******'***'***",
                        r"abc\'def": "********",
                        '"abc\'def"': '"*******"',
                        "'abc\"def'": "'*******'",
                        '"double quoted value"': '"*******************"',
                        r'"abc\"def"': '"********"',
                    }
                    expected = [json.loads(line) for line in log.read_text().splitlines()]
                    for prefix, secret, argv in cases:
                        with self.subTest(prefix=prefix):
                            masked = displays.get(secret, "*" * len(secret))
                            session.send("record " + prefix)
                            for length, char in enumerate(secret, 1):
                                session.send(char, duration=0.015)
                                self.assertIn("record " + prefix + masked[:length], session.text())
                            self.assertEqual(len(log.read_text().splitlines()), len(expected))
                            session.send(" tail")
                            self.assertIn("record " + prefix + masked + " tail", session.text())
                            session.send("\r")
                            session.read(0.1)
                            expected.append(argv)
                            self.assertEqual([json.loads(line) for line in log.read_text().splitlines()], expected)
                            self.assertNotIn(secret.encode(), session.raw)
                finally:
                    restored = session.close()
                self.assertEqual(restored, session.original_tty)

    def test_wrapping_resize_and_ctrl_c(self):
        for name in ("bash", "zsh"):
            shell = shutil.which(name)
            if shell is None:
                continue
            with self.subTest(shell=name):
                session = Session(shell, columns=40, lines=10)
                try:
                    session.send("echo -p ")
                    for char in "Q" * 85:
                        session.send(char, duration=0.008)
                        self.assertNotIn("Q", session.text())
                    session.resize(32, 12)
                    self.assertNotIn("Q", session.text())
                    session.send(b"\x03")
                    session.send("printf 'ALIVE\\n'\r")
                    session.wait_for("ALIVE")
                    self.assertNotIn(b"Q", session.raw)
                finally:
                    session.close()

    def test_exit_status_and_terminal_restoration(self):
        session = Session(shutil.which("bash"))
        session.send("exit 7\r", duration=0.15)
        session.process.wait(timeout=3)
        self.assertEqual(session.process.returncode, 7)
        self.assertEqual(session.close(), session.original_tty)


if __name__ == "__main__":
    unittest.main()
