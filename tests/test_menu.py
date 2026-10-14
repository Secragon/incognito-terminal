import json
from pathlib import Path
import shlex
import shutil
import tempfile
import unittest
from test_pty import Session


F2 = b'\x1bOQ'


class MenuTests(unittest.TestCase):
    def test_real_shell_custom_colon_trigger_has_no_space(self):
        for name in ('bash', 'zsh'):
            shell = shutil.which(name)
            if shell is None:
                continue
            with self.subTest(shell=name), tempfile.TemporaryDirectory() as directory:
                session = Session(shell, config_dir=directory)
                try:
                    session.send(F2 + b'ta/key:\r')
                    session.wait_for('Triggers saved.')
                    data = json.loads(Path(directory, 'incognito-terminal/config.json').read_text())
                    self.assertEqual(data['triggers'][-1], {'flag': '/key:', 'value_mode': 'attached'})
                    session.send(F2)
                    session.send('echo /key:COLON_SECRET')
                    session.wait_for('echo /key:************')
                    self.assertNotIn(b'COLON_SECRET', session.raw)
                    session.send(b'\x03')
                    session.send('echo /key: VISIBLE_FIXTURE')
                    session.wait_for('echo /key: VISIBLE_FIXTURE')
                    session.send(b'\x03')
                finally:
                    session.close()





    def test_bash_zsh_privacy_navigation_validation_and_save_failure(self):
        for name in ('bash', 'zsh'):
            shell = shutil.which(name)
            if shell is None:
                continue
            with self.subTest(shell=name), tempfile.TemporaryDirectory() as directory:
                session = Session(shell, config_dir=directory)
                path = Path(directory, 'incognito-terminal/config.json')
                backup = path.with_suffix('.backup')
                try:
                    session.send('echo -p MENU_SECRET')
                    session.wait_for('echo -p ***********')
                    session.send(F2 + b'pf\r')
                    self.assertEqual(session.screen.display[0], 'Privacy: Off  Menu (F2)'.rjust(100))
                    self.assertNotIn('MENU_SECRET', session.text())
                    session.send(F2)
                    session.wait_for('echo -p MENU_SECRET')
                    session.send(F2 + b'po\r' + F2)
                    session.wait_for('echo -p ***********')
                    self.assertEqual(session.screen.display[0], 'Privacy: On  Menu (F2)'.rjust(100))
                    session.send(b'\x03')

                    # Open Triggers by selecting its home row, then paste data.
                    session.send(F2 + b'\x1b[B' * 2 + b'\r' +
                                 b'a\x1b[200~-Z\r\x03\x1b[201~')
                    session.wait_for('Add trigger')
                    self.assertFalse(path.exists(), 'pasted controls must not save the editor')
                    session.send(b'\r')
                    session.wait_for('Triggers saved.')
                    original = path.read_bytes()
                    self.assertEqual(json.loads(original)['triggers'][-1]['flag'], '-Z')

                    session.send(b'e\x15invalid\r')
                    session.wait_for('Use flags')
                    self.assertEqual(path.read_bytes(), original)
                    session.send(b'\x03a-Z\r')
                    session.wait_for('already exists')
                    self.assertEqual(path.read_bytes(), original)
                    session.send(b'\x03')

                    # A directory at config.json guarantees failure even as root.
                    path.rename(backup)
                    path.mkdir()
                    session.send(b'e\x15-X\r')
                    session.wait_for('Could not save')
                    session.send(b'\x03')
                    self.assertEqual(backup.read_bytes(), original)
                    path.rmdir()
                    backup.rename(path)
                    session.send(F2)
                    session.send('echo -Z SAVE_FAILURE_SECRET')
                    session.wait_for('echo -Z *******************')
                    session.send(b'\x03')
                finally:
                    if backup.exists():
                        if path.is_dir():
                            path.rmdir()
                        backup.rename(path)
                    self.assertEqual(session.close(), session.original_tty)

    def test_bash_zsh_menu_add_edit_remove_and_restart(self):
        for name in ('bash', 'zsh'):
            shell = shutil.which(name)
            if shell is None:
                continue
            with self.subTest(shell=name), tempfile.TemporaryDirectory() as directory:
                log = Path(directory, 'received.txt')
                session = Session(shell, config_dir=directory)
                try:
                    self.assertEqual(session.screen.display[0], 'Privacy: On  Menu (F2)'.rjust(100))
                    session.send(f'record() {{ printf "%s\\n" "$@" > {shlex.quote(str(log))}; }}\r')
                    # Preserve a half-typed command while working in the menu.
                    session.send('record -Q ')
                    session.send(F2)
                    session.wait_for('[T] Triggers')
                    session.send('ta-Q\r')
                    session.wait_for('Triggers saved.')
                    path = Path(directory, 'incognito-terminal/config.json')
                    self.assertIn({'flag': '-Q', 'value_mode': 'attached_or_separated'}, json.loads(path.read_text())['triggers'])
                    session.resize(80, 20)
                    self.assertTrue(session.screen.display[0].endswith('Privacy: On  Menu (F2)'))
                    session.send(F2)
                    session.send('NEW_SECRET')
                    session.wait_for('record -Q **********')
                    self.assertFalse(log.exists())
                    session.send('\r')
                    for _ in range(50):
                        if log.exists():
                            break
                        session.read(0.02)
                    self.assertEqual(log.read_text().splitlines(), ['-Q', 'NEW_SECRET'])
                    self.assertNotIn(b'NEW_SECRET', session.raw)
                    # Edit the selected custom trigger, applied immediately.
                    session.send(F2 + b'te\x15-X\r')
                    session.wait_for('Triggers saved.')
                    self.assertEqual([r['flag'] for r in json.loads(path.read_text())['triggers']], ['-p', '--password', '/p:', '--pass', '-P', '-X'])
                    session.send(F2)
                finally:
                    self.assertEqual(session.close(), session.original_tty)
                restarted = Session(shell, config_dir=directory)
                try:
                    restarted.send('echo -X RESTART_SECRET')
                    restarted.wait_for('echo -X **************')
                    self.assertNotIn(b'RESTART_SECRET', restarted.raw)
                    restarted.send(b'\x03')
                    restarted.send('echo -Q VISIBLE_FIXTURE')
                    restarted.wait_for('echo -Q VISIBLE_FIXTURE')
                    restarted.send(b'\x03')
                finally:
                    restarted.close()


if __name__ == '__main__':
    unittest.main()
