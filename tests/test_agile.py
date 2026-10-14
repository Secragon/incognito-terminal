"""Agile privacy controls exercised through real Bash/Zsh PTYs.

The default Alt bindings use legacy terminal bytes. CSI-u fixtures separately
exercise terminals with exact modifier and key-release reporting.
"""
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
ARM = b'\x1bi'
REVEAL = b'\x1bu'
ENHANCED_REVEAL = b'\x1b[117;3u'
RELEASE = b'\x1b[117;3:3u'
CAPABILITIES = b'\x1b[?3u'


@unittest.skipUnless(os.environ.get('INCOGNITO_TERMINAL_BINARY'), 'native C feature')
class AgileTests(unittest.TestCase):
    def test_manual_values_quotes_spaces_paste_and_original_argv(self):
        for name in ('bash', 'zsh'):
            with self.subTest(shell=name), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                log = root / 'argv.txt'
                functions = root / 'functions.sh'
                functions.write_text('record() { builtin printf "%s\\n" "$@" > ' +
                                     shlex.quote(str(log)) + '; }\n')
                s = Session(shutil.which(name), config_dir=directory)
                try:
                    s.send('. ' + shlex.quote(str(functions)) + '\r')
                    for value, hidden, argv in [
                        ('BARE_SECRET tail', '*********** tail', 'BARE_SECRET\ntail\n'),
                        ("   'QUOTE SECRET' tail", "   '************' tail", 'QUOTE SECRET\ntail\n'),
                        ('  "DOUBLE SECRET" tail', '  "*************" tail', 'DOUBLE SECRET\ntail\n'),
                        ("abc'd ef'ghi tail", "***'****'*** tail", 'abcd efghi\ntail\n'),
                        ('密🔐码 tail', '****** tail', '密🔐码\ntail\n'),
                    ]:
                        log.unlink(missing_ok=True)
                        s.wait_for('incognito-terminal>')
                        s.send('record ')
                        offset = len(s.raw)
                        # Fragment a shortcut at every byte and paste the value in one batch.
                        for byte in ARM:
                            s.send(bytes([byte]), duration=.002)
                        s.wait_for('Masked typing: On')
                        s.send(b'\x1b[200~' + value.encode() + b'\x1b[201~')
                        s.wait_for('record ' + hidden)
                        self.assertNotIn(value.split(' tail')[0].strip().encode(), s.raw[offset:])
                        s.send('\r')
                        deadline = time.monotonic() + 3
                        while not log.exists():
                            self.assertLess(time.monotonic(), deadline)
                            s.read(.02)
                        self.assertEqual(log.read_text(), argv)
                    s.send('record ' + ARM.decode() + 'NOTICE_SECRET tail')
                    s.wait_for('record ************* tail')
                    s.read(2.15)
                    self.assertNotIn('Masked typing: On', s.text())
                    self.assertNotIn('NOTICE_SECRET', s.text())
                finally:
                    self.assertEqual(s.close(), s.original_tty)

    def test_manual_styles_edit_wrap_redraw_recall_and_privacy_off(self):
        for name in ('bash', 'zsh'):
            with self.subTest(shell=name), tempfile.TemporaryDirectory() as directory:
                s = Session(shutil.which(name), config_dir=directory)
                try:
                    s.send('record() { :; }\r')
                    for shortcut in (b'a', b'o', b'e'):
                        s.send(F2 + b'm' + shortcut + b'\r' + F2)
                        s.send('record ' + ARM.decode() + 'MANUAL_SECRET')
                        self.assertNotIn('MANUAL_SECRET', s.text())
                        s.send(b'\x7fZ')
                        s.send('LONG_SECRET' * 15 + ' tail')
                        self.assertNotIn('LONG_SECRET', s.text())
                        s.send(b'\x0c')
                        self.assertNotIn('LONG_SECRET', s.text())
                        s.resize(45, 16)
                        s.resize(100, 24)
                        self.assertNotIn('LONG_SECRET', s.text())
                        s.send('\r')
                        s.send(b'\x1b[A')
                        self.assertNotIn('MANUAL_SECREZ', s.text())
                        self.assertNotIn('LONG_SECRET', s.text())
                        s.send(b'\x03')
                    s.send(F2 + b'pf\r' + F2)
                    s.send('record ' + ARM.decode() + 'OFF_SECRET tail')
                    self.assertNotIn('OFF_SECRET', s.text())
                    self.assertIn('record ', s.text())
                    self.assertIn(' tail', s.text())
                finally:
                    self.assertEqual(s.close(), s.original_tty)

    def test_reveal_release_focus_loss_and_legacy_auto_hide(self):
        for name in ('bash', 'zsh'):
            with self.subTest(shell=name):
                s = Session(shutil.which(name))
                try:
                    s.send('record --pass RULE_SECRET')
                    s.wait_for('record --pass ***********')
                    s.send(REVEAL)
                    s.wait_for('RULE_SECRET')
                    s.wait_for('Reveal: hold the key; release auto-hides.')
                    # Legacy key-repeat events extend reveal while held.
                    for _ in range(8):
                        s.send(REVEAL, duration=.1)
                        self.assertIn('RULE_SECRET', s.text())
                    s.read(.3)
                    self.assertNotIn('RULE_SECRET', s.text())
                    s.send(CAPABILITIES + ENHANCED_REVEAL)
                    s.wait_for('RULE_SECRET')
                    # Releasing U still hides text if Alt was released first.
                    s.send(b'\x1b[117;1:3u')
                    self.assertNotIn('RULE_SECRET', s.text())
                    s.send(ENHANCED_REVEAL + b'\x1b[117;3:2u')
                    s.wait_for('RULE_SECRET')
                    s.send(b'\x1b[O')
                    self.assertNotIn('RULE_SECRET', s.text())
                    s.send(ENHANCED_REVEAL + F2 + F2)
                    self.assertNotIn('RULE_SECRET', s.text())
                    # Ordinary Tab and Ctrl-U remain shell keys.
                    s.send(b'\x15')
                    s.send('record ' + ARM.decode() + 'MANUAL_SECRET')
                    s.send(ENHANCED_REVEAL)
                    s.wait_for('MANUAL_SECRET')
                    s.send(RELEASE)
                    self.assertNotIn('MANUAL_SECRET', s.text())
                finally:
                    self.assertEqual(s.close(), s.original_tty)

    def test_manual_continuations_middle_of_line_and_unicode_prefix(self):
        for name in ('bash', 'zsh'):
            with self.subTest(shell=name):
                s = Session(shutil.which(name))
                try:
                    s.send('record \\\r')
                    s.send(ARM + b'CONT_SECRET tail')
                    s.wait_for('*********** tail')
                    self.assertNotIn('CONT_SECRET', s.text())
                    s.send(b'\x03')
                    s.send('record VISIBLE')
                    s.send(b'\x01' + b'\x1b[C' * 7 + ARM + b'MID_SECRET ')
                    s.wait_for('record ********** VISIBLE')
                    s.send(b'\x03')
                    s.send('record 密🔐 ')
                    s.send(ARM + b'UNICODE_SECRET tail')
                    s.wait_for('record 密🔐 ************** tail')
                    # Enhanced ordinary Ctrl-U must still clear the line.
                    s.send(b'\x1b[117;5u')
                    s.send('record ' + ARM.decode() + 'AFTER_CLEAR tail')
                    s.wait_for('record *********** tail')
                finally:
                    self.assertEqual(s.close(), s.original_tty)

    def test_toggles_shortcut_edits_preservation_failures_and_restart(self):
        for name in ('bash', 'zsh'):
            with self.subTest(shell=name), tempfile.TemporaryDirectory() as directory:
                path = Path(directory, 'incognito-terminal/config.json')
                s = Session(shutil.which(name), config_dir=directory)
                try:
                    s.send(F2 + b't ')
                    s.wait_for('Triggers disabled.')
                    initial = json.loads(path.read_text())
                    self.assertFalse(initial['triggers_enabled'])
                    self.assertEqual(len(initial['triggers']), 5)
                    s.send(F2)
                    s.send('record -p UNMASKED')
                    s.wait_for('record -p UNMASKED')
                    s.send(b'\x15record ' + ARM + b'MANUAL_SECRET tail')
                    s.wait_for('record ************* tail')
                    s.send(b'\x03')
                    s.send(F2 + b't ' + b'\x03x ')
                    s.wait_for('Exclusions disabled.')
                    s.send(F2)
                    s.send('nmap -p 80')
                    s.wait_for('nmap -p **')
                    s.send(b'\x03')
                    s.send(F2 + b'se\x15Alt+I\r')
                    s.wait_for('Shortcuts saved.')
                    s.send(b'\x1b[Be\x15Ctrl+Shift+R\r')
                    s.wait_for('Shortcuts saved.')
                    saved = json.loads(path.read_text())
                    self.assertEqual(saved['shortcuts'], {'mask': 'Alt+I', 'reveal': 'Ctrl+Shift+R'})
                    self.assertEqual(saved['triggers'], initial['triggers'])
                    self.assertEqual(saved['exceptions'], initial['exceptions'])
                    self.assertFalse(saved['exclusions_enabled'])
                    s.send(b'e\x15Alt+I\r')
                    s.wait_for('already assigned')
                    self.assertEqual(json.loads(path.read_text()), saved)
                    s.send(b'\x03e\x15F2\r')
                    s.wait_for('F2 is reserved')
                    self.assertEqual(json.loads(path.read_text()), saved)
                    s.send(b'\x03e\x15Ctrl+M\r')
                    s.wait_for('Ctrl+M is Enter and cannot be assigned.')
                    self.assertEqual(json.loads(path.read_text()), saved)
                    s.send(b'\x03')
                    backup = path.with_suffix('.backup')
                    path.rename(backup)
                    path.mkdir()
                    try:
                        s.send(b'e\x15Alt+U\r')
                        s.wait_for('Could not save the shortcuts.')
                        self.assertEqual(json.loads(backup.read_text()), saved)
                    finally:
                        path.rmdir()
                        backup.rename(path)
                    s.send(F2)
                    s.send('record \x1biCUSTOM_SECRET tail')
                    s.wait_for('record ************* tail')
                    s.send(CAPABILITIES + b'\x1b[114;6u')
                    s.wait_for('CUSTOM_SECRET')
                    s.send(b'\x1b[114;6:3u')
                    self.assertNotIn('CUSTOM_SECRET', s.text())
                finally:
                    self.assertEqual(s.close(), s.original_tty)
                restarted = Session(shutil.which(name), config_dir=directory)
                try:
                    restarted.send('record \x1biRESTART_SECRET tail')
                    restarted.wait_for('record ************** tail')
                    self.assertFalse(json.loads(path.read_text())['exclusions_enabled'])
                finally:
                    self.assertEqual(restarted.close(), restarted.original_tty)
