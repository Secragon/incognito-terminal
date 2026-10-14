"""Native masking styles and Crazy mode through real terminal keystrokes."""
import json
import os
from pathlib import Path
import shlex
import shutil
import tempfile
import time
import unittest

from test_pty import Session

F2 = b'\x1bOQ'
FACES = {chr(n) for n in (0x1f600, 0x1f603, 0x1f604, 0x1f601, 0x1f606,
                           0x1f605, 0x1f602, 0x1f609, 0x1f60a, 0x1f642,
                           0x1f60d, 0x1f60e, 0x1f923, 0x263a)}


@unittest.skipUnless(os.environ.get('INCOGNITO_TERMINAL_BINARY'), 'native C feature')
class PrivacyStyleTests(unittest.TestCase):
    def assert_styled_text(self, session, phrase, *, description=False):
        for y, line in enumerate(session.screen.display):
            if phrase in line:
                start = line.index(phrase)
                cells = [session.screen.buffer[y][x] for x in range(start, start + len(phrase))]
                self.assertEqual({cell.italics for cell in cells}, {description})
                self.assertEqual({cell.bold for cell in cells}, {not description})
                if description:
                    self.assertEqual({cell.fg for cell in cells}, {'91b9bc'})
                return
        self.fail('Missing menu text: ' + phrase)

    def test_explained_submenus_selection_cancel_and_compact_layout(self):
        for name in ('bash', 'zsh'):
            with self.subTest(shell=name), tempfile.TemporaryDirectory() as directory:
                session = Session(shutil.which(name), config_dir=directory)
                try:
                    session.send('record --pass MENU_SECRET')
                    session.wait_for('record --pass ***********')
                    session.send(F2)
                    self.assertNotIn('cycles', session.text())
                    self.assert_styled_text(session, '[P] Privacy')
                    self.assert_styled_text(session, 'Control which text stays concealed.', description=True)
                    # Every home entry opens a submenu and explains its choices.
                    pages = ((b'p', '[O] On', 'Mask passwords; respect command exceptions.'),
                             (b'm', '[A] Asterisks', 'Green stars, matching the On indicator.'),
                             (b't', '-p', 'Flags that identify password arguments.'),
                             (b'h', '[N] Normal', "Use the shell's usual history settings."),
                             (b'x', 'Exceptions', 'Commands left visible in Privacy On.'))
                    for shortcut, option, explanation in pages:
                        session.send(shortcut)
                        session.wait_for(option)
                        self.assert_styled_text(session, explanation, description=True)
                        self.assertNotIn('MENU_SECRET', session.text())
                        session.send(b'\x03')
                        session.wait_for('[M] Privacy mode')
                    session.send(b'\r')  # Enter on Privacy opens rather than toggles.
                    session.wait_for('Privacy state')
                    self.assert_styled_text(session, '[O] On')
                    session.send(b'\x1b[B')  # Select Off, but do not apply it.
                    self.assertTrue(session.screen.display[0].endswith('Privacy: On  Menu (F2)'))
                    session.send(b'\x03p')
                    session.wait_for('Current: On')
                    session.resize(40, 13)
                    self.assertIn('Mask matching password values.', session.text())
                    session.send(b'f')
                    self.assertIn('Reveal all text and passwords.', session.text())
                    session.send(b'c')
                    self.assertIn('Hide input; show the prompt.', session.text())
                    session.resize(100, 24)
                    session.send(b'\r')
                    session.wait_for('Current: Crazy')
                    session.send(F2)
                    self.assertNotIn('record', session.text())
                    session.send(F2 + b'po\r' + F2)
                    session.wait_for('record --pass ***********')
                    self.assertFalse(Path(directory, 'incognito-terminal/config.json').exists())
                    session.send(b'\x03')
                finally:
                    self.assertEqual(session.close(), session.original_tty)

    def select_style(self, session, shortcut):
        session.send(F2 + b'm' + shortcut + b'\r')
        session.wait_for('Privacy mode saved.')
        session.send(F2)

    def test_styles_colors_stability_cursor_and_restart(self):
        for name in ('bash', 'zsh'):
            with self.subTest(shell=name), tempfile.TemporaryDirectory() as directory:
                session = Session(shutil.which(name), config_dir=directory)
                config = Path(directory, 'incognito-terminal/config.json')
                try:
                    session.send('record --pass ABCDE')
                    session.wait_for('record --pass *****')
                    row = session.screen.buffer[session.screen.cursor.y]
                    start = session.screen.cursor.x - 5
                    self.assertEqual({row[x].fg for x in range(start, start + 5)}, {'7fa58f'})
                    indicator_row = session.screen.buffer[0]
                    indicator = session.screen.display[0].index('On')
                    self.assertEqual(row[start].fg, indicator_row[indicator].fg)
                    for shortcut, style in ((b'o', 'obfuscation'), (b'e', 'emoji'), (b'a', 'asterisks')):
                        self.select_style(session, shortcut)
                        saved = json.loads(config.read_text())
                        self.assertEqual(saved['privacy_style'], style)
                        self.assertEqual(saved['history_mode'], 'normal')
                        self.assertEqual(len(saved['triggers']), 5)
                        self.assertEqual(len(saved['exceptions']), 97)
                        row = session.screen.buffer[session.screen.cursor.y]
                        self.assertEqual(session.screen.cursor.x, start + 5)
                        mask = ''.join(row[x].data for x in range(start, start + 5))
                        self.assertNotIn('ABCDE', session.text())
                        if style == 'asterisks':
                            self.assertEqual(mask, '*****')
                        elif style == 'obfuscation':
                            self.assertRegex(mask, r'^[a-zA-Z0-9!@#$%&+=?]{5}$')
                        else:
                            self.assertEqual([c for c in mask if c != '\ufe0e'][-1], '☺')
                            self.assertTrue(all(c in FACES or c == '\ufe0e' for c in mask))
                        before = session.text()
                        session.send(F2 + F2)  # Closing a menu redraws the same concealed cells.
                        self.assertEqual(session.text(), before)
                    session.send(b'\x03')
                    self.select_style(session, b'e')
                    session.send('record -p "A" tail')
                    session.wait_for('record -p "☺')
                    self.assertIn('" tail', session.text())
                    session.send(b'\x03')
                    session.send("record -p '密🔐码' tail")
                    session.read(.15)
                    self.assertNotIn('密', session.text())
                    self.assertNotIn('🔐', session.text())
                    self.assertIn('tail', session.text())
                    session.resize(42, 12)
                    session.resize(110, 28)
                    self.assertNotIn('密', session.text())
                    self.assertNotIn(b'ABCDE', session.raw)
                finally:
                    self.assertEqual(session.close(), session.original_tty)
                restarted = Session(shutil.which(name), config_dir=directory)
                try:
                    self.assertTrue(restarted.screen.display[0].endswith('Privacy: On  Menu (F2)'))
                    restarted.send('record --pass RESTART_SECRET')
                    restarted.read(.1)
                    self.assertNotIn('RESTART_SECRET', restarted.text())
                    self.assertTrue(any(c in FACES for c in restarted.text()))
                    self.assertNotIn(b'RESTART_SECRET', restarted.raw)
                finally:
                    self.assertEqual(restarted.close(), restarted.original_tty)

    def test_crazy_all_styles_output_exceptions_and_original_argv(self):
        for name in ('bash', 'zsh'):
            with self.subTest(shell=name), tempfile.TemporaryDirectory() as directory:
                session = Session(shutil.which(name), config_dir=directory)
                log = Path(directory, 'received.txt')
                try:
                    functions = Path(directory, 'functions.sh')
                    functions.write_text('record() { printf "%s\\n" "$@" >> ' +
                                         shlex.quote(str(log)) + '; printf "OUTPUT_FIXTURE\\n"; }\n')
                    session.send('. ' + shlex.quote(str(functions)) + '\r')
                    # Apply Crazy inside its submenu; the shell remains covered.
                    session.send(F2 + b'pc\r')
                    session.wait_for('Privacy: Crazy')
                    session.send(b'\x03')
                    self.assertIn('[M] Privacy mode', session.text())
                    session.send(F2)
                    expected = []
                    for shortcut, style in ((b'a', 'asterisks'), (b'o', 'obfuscation'), (b'e', 'emoji')):
                        self.select_style(session, shortcut)
                        offset = len(session.raw)
                        session.send('record --pass CRAZY_SECRET\r')
                        expected += ['--pass', 'CRAZY_SECRET']
                        deadline = time.monotonic() + 3
                        while not log.exists() or log.read_text().splitlines() != expected:
                            self.assertLess(time.monotonic(), deadline)
                            session.read(.02)
                        session.read(.15)
                        for raw in (b'CRAZY_SECRET', b'record'):
                            self.assertNotIn(raw, session.raw[offset:])
                        session.wait_for('OUTPUT_FIXTURE')
                        session.wait_for('incognito-terminal>')
                        self.assertTrue(session.screen.display[0].endswith('Privacy: Crazy  Menu (F2)'))
                        if style == 'asterisks':
                            self.assertIn('*****', session.text())
                        elif style == 'emoji':
                            self.assertTrue(any(c in FACES for c in session.text()))
                        offset = len(session.raw)
                        session.send('nmap -p 80')  # Type only: the actual scanner is never run.
                        session.read(.1)
                        self.assertNotIn(b'nmap', session.raw[offset:])
                        session.send(b'\x03')
                        session.resize(50, 14)
                        session.resize(100, 24)
                        self.assertNotIn('CRAZY_SECRET', session.text())
                    session.send(F2 + b'po\r' + F2)  # On restores non-sensitive text.
                    session.wait_for('Privacy: On')
                    session.wait_for('OUTPUT_FIXTURE')
                    session.send('nmap -p 80')
                    session.wait_for('nmap -p 80')
                    session.send(b'\x03')
                    self.select_style(session, b'a')
                    session.send('record --pass HIDE_AGAIN')
                    session.wait_for('record --pass **********')
                finally:
                    self.assertEqual(session.close(), session.original_tty)

    def test_style_cancel_validation_and_failed_save(self):
        for name in ('bash', 'zsh'):
            with self.subTest(shell=name), tempfile.TemporaryDirectory() as directory:
                session = Session(shutil.which(name), config_dir=directory)
                path = Path(directory, 'incognito-terminal/config.json')
                backup = path.with_suffix('.backup')
                try:
                    self.select_style(session, b'o')
                    original = path.read_bytes()
                    session.send(F2 + b'me\x03')
                    session.wait_for('[M] Privacy mode')
                    self.assertEqual(path.read_bytes(), original)
                    session.send(F2)
                    path.rename(backup)
                    path.mkdir()
                    session.send(F2 + b'me\r')
                    session.wait_for('Could not save the privacy mode.')
                    session.send(b'\x03')
                    session.wait_for('Obfuscation')
                    self.assertEqual(backup.read_bytes(), original)
                    path.rmdir()
                    backup.rename(path)
                    session.send(F2)
                finally:
                    if backup.exists():
                        if path.is_dir():
                            path.rmdir()
                        backup.rename(path)
                    self.assertEqual(session.close(), session.original_tty)

    def test_crazy_keeps_colored_multiline_prompts_and_padding_visible(self):
        for name in ('bash', 'zsh'):
            with self.subTest(shell=name), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                log = root / 'argv.txt'
                if name == 'bash':
                    prompt = "PS1='\\[\\e[32m\\]user@host\\[\\e[0m\\]\\nincognito-terminal> '\n"
                    # Prompt functions that assign PS1 must still work, including array hooks.
                    rc = "user_prompt() { " + prompt.strip() + "; }\nPROMPT_COMMAND=(user_prompt)\n"
                else:
                    rc = "PS1=$'%F{green}user@host%f\\nincognito-terminal> '\nRPROMPT='RIGHT_PROMPT'\n"
                rc += "PS2='continue> '\nrecord() { builtin printf '%s\\n' \"$@\" > " + \
                      shlex.quote(str(log)) + "; builtin printf 'OUTPUT_FIXTURE\\n'; }\n"
                (root / ('.bashrc' if name == 'bash' else '.zshrc')).write_text(rc)
                session = Session(shutil.which(name), config_dir=directory, clean=False,
                                  extra_env={'HOME': directory, 'ZDOTDIR': directory})
                try:
                    session.send(F2 + b'pc\r' + F2)
                    session.wait_for('user@host')
                    for shortcut, style in ((b'a', 'asterisks'), (b'o', 'obfuscation'), (b'e', 'emoji')):
                        self.select_style(session, shortcut)
                        log.unlink(missing_ok=True)
                        self.assertIn('user@host', session.text())
                        self.assertIn('incognito-terminal>', session.text())
                        # Idle prompts and the padding used by ZLE contain no decorative masks.
                        body = '\n'.join(session.screen.display[1:])
                        self.assertNotIn('*', body)
                        self.assertFalse(any(c in FACES for c in body))
                        if name == 'zsh':
                            self.assertIn('RIGHT_PROMPT', body)
                        offset = len(session.raw)
                        session.send('record ')
                        for char in 'TYPED_SECRET':
                            session.send(char, duration=.01)
                            self.assertNotIn('TYPED', session.text())
                            self.assertIn('user@host', session.text())
                        # Editing and wrapping must preserve both the real argument and the prompt.
                        session.send(b'\x7fZ')
                        payload = 'TYPED_SECREZ' + 'LONG_SECRET' * 12
                        session.send('LONG_SECRET' * 12)
                        self.assertNotIn('LONG_SECRET', session.text())
                        if style == 'asterisks':
                            self.assertEqual(session.text().count('*'), len('record') + len(payload))
                        self.assertNotIn(b'TYPED_SECRET', session.raw[offset:])
                        session.send('\r')
                        session.wait_for('OUTPUT_FIXTURE')
                        deadline = time.monotonic() + 3
                        while not log.exists() or log.read_text() != payload + '\n':
                            self.assertLess(time.monotonic(), deadline)
                            session.read(.02)
                        session.send(b'\x1b[A')
                        self.assertNotIn('TYPED_SECREZ', session.text())
                        session.send(b'\x03')
                        session.send('record "CONTINUATION_SECRET\r')
                        session.wait_for('continue>')
                        self.assertNotIn('CONTINUATION_SECRET', session.text())
                        session.send(b'\x03')
                        session.send(b'\x0c')
                        session.wait_for('user@host')
                finally:
                    self.assertEqual(session.close(), session.original_tty)
