"""End-to-end coverage for transparent tmux sessions."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_pty import Session


F2 = b"\x1bOQ"


@unittest.skipUnless(os.environ.get("INCOGNITO_TERMINAL_BINARY"), "native C feature")
@unittest.skipUnless(shutil.which("tmux"), "tmux is not installed")
class TmuxTests(unittest.TestCase):
    def tmux(self, environment, *arguments):
        return subprocess.run(
            [shutil.which("tmux"), "-L", "incognito-terminal", *arguments],
            env={**os.environ, **environment},
            text=True,
            capture_output=True,
        )

    def stop_tmux(self, environment):
        self.tmux(environment, "kill-server")

    def startup_file(self, home, shell, *, replace=True):
        name = Path(shell).name
        launch = "exec tmux" if replace else "tmux"
        if name == "zsh":
            path = Path(home, ".zshrc")
            path.write_text(
                f"if [[ -z $TMUX ]]; then {launch}; fi\n"
                "PS1='incognito-terminal> '\n"
            )
        else:
            path = Path(home, ".bashrc")
            path.write_text(
                f"if [[ -z $TMUX ]]; then {launch}; fi\n"
                "PS1='incognito-terminal> '\n"
            )

    def test_autostart_masks_and_protects_default_windows(self):
        for name in ("bash", "zsh"):
            shell = shutil.which(name)
            if not shell:
                continue
            with self.subTest(shell=name), tempfile.TemporaryDirectory() as home, \
                    tempfile.TemporaryDirectory() as socket_dir, \
                    tempfile.TemporaryDirectory() as config_dir:
                self.startup_file(home, shell)
                environment = {"HOME": home, "SHELL": shell, "TMUX_TMPDIR": socket_dir}
                session = Session(shell, clean=False, config_dir=config_dir,
                                  extra_env=environment)
                try:
                    session.send("echo -p ")
                    for character in "TMUX_SECRET":
                        session.send(character, duration=.008)
                    self.assertIn("echo -p ***********", session.text())
                    self.assertNotIn("TMUX_SECRET", session.text())
                    self.assertNotIn(b"TMUX_SECRET", session.raw)

                    session.send(F2)
                    session.wait_for("Screen privacy settings")
                    session.send(F2)

                    # CSI-u modified keys must reach the pane wrapper through
                    # tmux after the user chooses such a binding.
                    session.send(F2 + b"se\x15Ctrl+Shift+I\r")
                    session.wait_for("Shortcuts saved.")
                    session.send(F2, duration=.2)
                    session.send(b"\x15")
                    session.send("echo value ", duration=.1)
                    # tmux 3.4 cannot decode CSI-u from its parent terminal.
                    # Incognito pops that terminal mode for old tmux, so
                    # emulate the terminal's resulting xterm encoding.
                    modified_i = (b"\x1b[27;6;73~" if b"\x1b[<u" in session.raw
                                  else b"\x1b[105;6u")
                    session.send(modified_i + b"CSI_SECRET")
                    session.wait_for("echo value **********")
                    self.assertNotIn("CSI_SECRET", session.text())

                    session.send(b"\x03", duration=.2)
                    session.send(b"\x02c", duration=.5)
                    offset = len(session.raw)
                    session.send("echo -p WINDOW_SECRET", duration=.2)
                    session.wait_for("echo -p *************")
                    self.assertNotIn("WINDOW_SECRET", session.text())
                    self.assertNotIn(b"WINDOW_SECRET", session.raw[offset:])
                    windows = self.tmux(
                        environment, "list-windows", "-t", "incognito-terminal", "-F",
                        "#{pane_current_command}",
                    )
                    self.assertEqual(windows.returncode, 0, windows.stderr)
                    self.assertGreaterEqual(windows.stdout.count("incognito-terminal"), 2)
                    marker = self.tmux(
                        environment, "show-options", "-t", "incognito-terminal", "-qv",
                        "@incognito_protected",
                    )
                    self.assertEqual(marker.stdout.strip(), "1")
                    default = self.tmux(
                        environment, "show-options", "-gv", "default-command",
                    )
                    self.assertIn("INCOGNITO_TMUX_PANE=1", default.stdout)
                    features = self.tmux(
                        environment, "show-options", "-sv", "terminal-features",
                    )
                    self.assertIn("xterm-256color:extkeys", features.stdout)
                finally:
                    self.stop_tmux(environment)
                    self.assertEqual(session.close(), session.original_tty)

    def test_manual_launch_detach_reattach_and_return(self):
        shell = shutil.which("zsh") or shutil.which("bash")
        with tempfile.TemporaryDirectory() as socket_dir, \
                tempfile.TemporaryDirectory() as config_dir:
            environment = {"SHELL": shell, "TMUX_TMPDIR": socket_dir}
            session = Session(shell, config_dir=config_dir, extra_env=environment)
            try:
                session.send("tmux\r", duration=.6)
                self.assertIn("[incognito", session.text())
                session.send("echo -p RETURN_SECRET")
                session.wait_for("echo -p *************")

                session.send(b"\x03\x02d", duration=.6)
                session.wait_for("incognito-terminal>")
                self.assertIsNone(session.process.poll())

                session.send("tmux\r", duration=.6)
                self.assertIn("[incognito", session.text())
                session.send("exit\r", duration=.8)
                session.wait_for("incognito-terminal>")
                session.send("echo OUTER_RESUMED\r")
                session.wait_for("OUTER_RESUMED")
            finally:
                self.stop_tmux(environment)
                self.assertEqual(session.close(), session.original_tty)

    def test_unmarked_private_session_is_refused(self):
        shell = shutil.which("zsh") or shutil.which("bash")
        with tempfile.TemporaryDirectory() as socket_dir, \
                tempfile.TemporaryDirectory() as config_dir:
            environment = {"SHELL": shell, "TMUX_TMPDIR": socket_dir}
            session = Session(shell, config_dir=config_dir, extra_env=environment)
            try:
                created = self.tmux(
                    environment, "new-session", "-d", "-s", "incognito-terminal",
                )
                self.assertEqual(created.returncode, 0, created.stderr)
                session.send("tmux\r", duration=.5)
                session.wait_for("the private tmux session is not protected")
                self.assertIsNone(session.process.poll())

                session.send("echo -p REFUSAL_SECRET", duration=.2)
                session.wait_for("echo -p **************")
                self.assertNotIn("REFUSAL_SECRET", session.text())
            finally:
                self.stop_tmux(environment)
                self.assertEqual(session.close(), session.original_tty)

    def test_non_exec_autostart_returns_to_outer_shell(self):
        shell = shutil.which("bash") or shutil.which("zsh")
        with tempfile.TemporaryDirectory() as home, \
                tempfile.TemporaryDirectory() as socket_dir, \
                tempfile.TemporaryDirectory() as config_dir:
            self.startup_file(home, shell, replace=False)
            environment = {"HOME": home, "SHELL": shell, "TMUX_TMPDIR": socket_dir}
            session = Session(shell, clean=False, config_dir=config_dir,
                              extra_env=environment)
            try:
                session.send("echo -p AUTOSTART_SECRET", duration=.2)
                session.wait_for("echo -p ****************")
                self.assertNotIn("AUTOSTART_SECRET", session.text())

                session.send(b"\x03\x02d", duration=.7)
                session.wait_for("incognito-terminal>")
                session.send("echo -p OUTER_SECRET", duration=.2)
                session.wait_for("echo -p ************")
                self.assertNotIn("OUTER_SECRET", session.text())
            finally:
                self.stop_tmux(environment)
                self.assertEqual(session.close(), session.original_tty)

    def test_history_override_survives_shell_autostart(self):
        shell = shutil.which("zsh") or shutil.which("bash")
        with tempfile.TemporaryDirectory() as home, \
                tempfile.TemporaryDirectory() as socket_dir, \
                tempfile.TemporaryDirectory() as config_dir:
            self.startup_file(home, shell)
            environment = {"HOME": home, "SHELL": shell, "TMUX_TMPDIR": socket_dir}
            session = Session(shell, clean=False, history_mode="off", config_dir=config_dir,
                              extra_env=environment)
            try:
                session.send("echo -p OFF_SECRET")
                session.wait_for("echo -p **********")
                self.assertIsNone(session.process.poll())

                session.send(b"\x03\x02c", duration=.5)
                session.send(": NEW_WINDOW_HISTORY\r", duration=.2)
                offset = len(session.raw)
                session.send(b"\x1b[A", duration=.2)
                self.assertNotIn(b"NEW_WINDOW_HISTORY", session.raw[offset:])
            finally:
                self.stop_tmux(environment)
                self.assertEqual(session.close(), session.original_tty)

    def test_unused_manual_arm_does_not_survive_pane_layout_change(self):
        shell = shutil.which("zsh") or shutil.which("bash")
        with tempfile.TemporaryDirectory() as home, \
                tempfile.TemporaryDirectory() as socket_dir, \
                tempfile.TemporaryDirectory() as config_dir:
            self.startup_file(home, shell)
            environment = {"HOME": home, "SHELL": shell, "TMUX_TMPDIR": socket_dir}
            session = Session(shell, clean=False, config_dir=config_dir,
                              extra_env=environment)
            try:
                session.send(b"\x1bi")
                session.wait_for("Masked typing: On")
                session.send(b"\x02%", duration=.4)
                session.send("exit\r", duration=.5)
                session.send("VISIBLE_AFTER_CLOSE value", duration=.2)
                self.assertIn("VISIBLE_AFTER_CLOSE value", session.text())
                self.assertNotIn("******************* value", session.text())

                # A span that already contains sensitive input must remain
                # protected while the pane shrinks and expands.
                session.send(b"\x03", duration=.3)
                session.send(b"\x1bi")
                session.wait_for("Masked typing: On")
                session.send("STARTED_SECRET", duration=.2)
                session.wait_for("**************")
                session.send(b"\x02%", duration=.4)
                session.send("exit\r", duration=.5)
                self.assertIn("**************", session.text())
                self.assertNotIn("STARTED_SECRET", session.text())
                self.assertNotIn(b"STARTED_SECRET", session.raw)
            finally:
                self.stop_tmux(environment)
                self.assertEqual(session.close(), session.original_tty)


if __name__ == "__main__":
    unittest.main()
