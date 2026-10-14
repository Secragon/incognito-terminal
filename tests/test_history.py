"""History policy tests use disposable homes, config, and history files only."""
import os
from pathlib import Path
import shlex
import shutil
import signal
import tempfile
import unittest
from test_pty import Session


F2 = b'\x1bOQ'


def shell_home(root, shell):
    home = root / 'home'
    home.mkdir()
    history = home / 'history'
    history.write_text('echo EXISTING_HISTORY_FIXTURE\n')
    started = root / 'startup-count'
    rc = f'''HISTFILE={shlex.quote(str(history))}
HISTSIZE=2000
PS1='incognito-terminal> '
printf x >> {shlex.quote(str(started))}
'''
    env = {'HOME': str(home), 'ZDOTDIR': str(home)}
    if shell == 'bash':
        rc += "PROMPT_COMMAND='builtin history -a'\n"
        path = home / '.bashrc'
    else:
        (home / '.zshenv').write_text('export PRIVACY_ENV_FIXTURE=loaded\n')
        rc += 'SAVEHIST=2000\nsetopt SHARE_HISTORY\n'
        path = home / '.zshrc'
    path.write_text(rc)
    return env, history, path, started


class HistoryTests(unittest.TestCase):
    def test_normal_session_off_recall_and_history_files_with_user_rc(self):
        for name in ('bash', 'zsh'):
            shell = shutil.which(name)
            if shell is None:
                continue
            for mode in ('normal', 'session', 'off'):
                with self.subTest(shell=name, mode=mode), tempfile.TemporaryDirectory(prefix="privacy history '") as folder:
                    root = Path(folder)
                    env, history, rc, started = shell_home(root, name)
                    old_history, old_rc = history.read_bytes(), rc.read_bytes()
                    dump = root / 'history-dump'
                    session = Session(shell, clean=False, history_mode=mode, extra_env=env)
                    try:
                        self.assertEqual(started.read_text(), 'x', 'user startup must run once')
                        session.send(': FIRST_HISTORY_FIXTURE\r')
                        offset = len(session.raw)
                        session.send(b'\x1b[A')
                        self.assertEqual(b'FIRST_HISTORY_FIXTURE' in session.raw[offset:], mode != 'off')
                        session.send(b'\x03')
                        command = 'builtin history' if name == 'bash' else 'builtin fc -l 1'
                        session.send(command + ' > ' + shlex.quote(str(dump)) + '\r')
                        self.assertEqual('FIRST_HISTORY_FIXTURE' in dump.read_text(), mode != 'off')
                        # Repeat, including a cancelled input, to catch Zsh's last-entry behavior.
                        session.send(': SECOND_HISTORY_FIXTURE\r')
                        offset = len(session.raw)
                        session.send(b'\x1b[A')
                        self.assertEqual(b'SECOND_HISTORY_FIXTURE' in session.raw[offset:], mode != 'off')
                        session.send(b'\x03')
                        if mode != 'normal':
                            self.assertEqual(history.read_bytes(), old_history)
                        session.send(F2 + b'h')
                        self.assertIn('Active: ' + {'normal': 'Normal', 'session': 'Session only', 'off': 'Off'}[mode], session.text())
                        session.send(F2)
                    finally:
                        self.assertEqual(session.close(), session.original_tty)
                    self.assertEqual(rc.read_bytes(), old_rc)
                    if mode == 'normal':
                        self.assertIn('FIRST_HISTORY_FIXTURE', history.read_text())
                        self.assertIn('EXISTING_HISTORY_FIXTURE', history.read_text())
                    else:
                        self.assertEqual(history.read_bytes(), old_history)

    def test_saved_mode_applies_on_restart_and_does_not_change_current_history(self):
        for name in ('bash', 'zsh'):
            shell = shutil.which(name)
            if shell is None:
                continue
            with self.subTest(shell=name), tempfile.TemporaryDirectory() as directory:
                session = Session(shell, config_dir=directory)
                try:
                    session.send(F2 + b'ho\r')
                    session.wait_for('Saved. Applies to new sessions.')
                    session.send(F2)
                    session.send(': STILL_SESSION_FIXTURE\r')
                    offset = len(session.raw)
                    session.send(b'\x1b[A')
                    self.assertIn(b'STILL_SESSION_FIXTURE', session.raw[offset:])
                    session.send(b'\x03')
                finally:
                    session.close()
                restarted = Session(shell, config_dir=directory)
                try:
                    restarted.send(': OFF_RESTART_FIXTURE\r')
                    offset = len(restarted.raw)
                    restarted.send(b'\x1b[A')
                    self.assertNotIn(b'OFF_RESTART_FIXTURE', restarted.raw[offset:])
                    restarted.send(b'\x03')
                    restarted.send(F2 + b'h')
                    restarted.wait_for('Active: Off')
                    restarted.send(F2)
                finally:
                    restarted.close()

    def test_protected_modes_do_not_save_on_wrapper_termination(self):
        for name in ('bash', 'zsh'):
            shell = shutil.which(name)
            if shell is None:
                continue
            for mode in ('session', 'off'):
                with self.subTest(shell=name, mode=mode), tempfile.TemporaryDirectory() as directory:
                    env, history, _, _ = shell_home(Path(directory), name)
                    original = history.read_bytes()
                    session = Session(shell, clean=False, history_mode=mode, extra_env=env)
                    try:
                        session.send(': TERMINATION_FIXTURE\r')
                        os.kill(session.process.pid, signal.SIGTERM)
                        session.process.wait(timeout=3)
                    finally:
                        session.close()
                    self.assertEqual(history.read_bytes(), original)

    def test_readonly_conflicting_history_setting_fails_before_session(self):
        shell = shutil.which('bash')
        if shell is None:
            self.skipTest('Bash unavailable')
        with tempfile.TemporaryDirectory() as directory:
            env, history, rc, _ = shell_home(Path(directory), 'bash')
            rc.write_text(rc.read_text() + 'readonly HISTFILE\n')
            original = history.read_bytes()
            with self.assertRaisesRegex(AssertionError, 'History setup failed'):
                Session(shell, clean=False, history_mode='off', extra_env=env)
            self.assertEqual(history.read_bytes(), original)

    def test_startup_cursor_query_is_answered_before_history_confirmation(self):
        shell = shutil.which('bash')
        if shell is None:
            self.skipTest('Bash unavailable')
        with tempfile.TemporaryDirectory() as directory:
            env, _, rc, _ = shell_home(Path(directory), 'bash')
            rc.write_text(rc.read_text() + """
printf '\\033[6n'
IFS= builtin read -r -t 2 -d R _fixture_cursor || exit 91
[[ $_fixture_cursor == $'\\e['* ]] || exit 92
""")
            session = Session(shell, clean=False, history_mode='off', extra_env=env)
            try:
                session.send(': QUERY_READY_FIXTURE\r')
                self.assertIsNone(session.process.poll())
            finally:
                session.close()

    def test_zshenv_redirected_zdotdir_is_preserved(self):
        shell = shutil.which('zsh')
        if shell is None:
            self.skipTest('Zsh unavailable')
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            env, history, rc, started = shell_home(root, 'zsh')
            original = history.read_bytes()
            target = root / "dot dir's"
            target.mkdir()
            rc.rename(target / '.zshrc')
            Path(env['HOME'], '.zshenv').write_text('export ZDOTDIR=' + shlex.quote(str(target)) + '\n')
            dump = root / 'actual-zdotdir'
            session = Session(shell, clean=False, history_mode='session', extra_env=env)
            try:
                self.assertEqual(started.read_text(), 'x')
                session.send('print -r -- "$ZDOTDIR" > ' + shlex.quote(str(dump)) + '\r')
                self.assertEqual(dump.read_text().strip(), str(target))
            finally:
                session.close()
            self.assertEqual(history.read_bytes(), original)


if __name__ == '__main__':
    unittest.main()
