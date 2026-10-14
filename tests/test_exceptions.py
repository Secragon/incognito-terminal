"""Exercise native exception dialogs and masking in real Bash/Zsh sessions."""
import json
import os
from pathlib import Path
import shutil
import tempfile
import unittest

from test_pty import Session

F2 = b'\x1bOQ'
DEFAULT_EXCEPTIONS = (
    'ssh nmap nc docker psql redis-cli mkdir scp dirb ffuf feroxbuster masscan rustscan naabu dirbuster '
    'cp rmdir install ln ls mktemp realpath tar du df chown chgrp tree getfacl setfacl getfattr lsattr chattr pwd '
    'diff patch grep rg less more tee nl bat batcat '
    'lsof ps pgrep pkill pidwait pstree top htop renice vmstat mpstat pidstat uptime uname who '
    'ping ping6 ss netstat tcpdump dumpcap traceroute traceroute6 tracepath mtr arping hping3 sftp nc.openbsd nc.traditional netcat '
    'dpkg dpkg-query sysctl ldconfig ipcs findmnt lsblk lscpu lsns lslocks lsipc lspci '
    'cc gcc g++ c++ clang clang++ cpp type hash ulimit '
).split()


@unittest.skipUnless(os.environ.get('INCOGNITO_TERMINAL_BINARY'), 'native C feature')
class ExceptionTests(unittest.TestCase):
    def test_updates_merge_custom_lists_and_remember_removals(self):
        for name in ('bash', 'zsh'):
            shell = shutil.which(name)
            if not shell:
                continue
            with self.subTest(shell=name), tempfile.TemporaryDirectory() as directory:
                config = Path(directory, 'incognito-terminal/config.json')
                config.parent.mkdir()
                previous_exceptions = DEFAULT_EXCEPTIONS[:15]
                custom_exceptions = [n for n in previous_exceptions if n != 'nmap'] + ['custom-net']
                config.write_text(json.dumps({
                    'version': 6,
                    'history_mode': 'normal',
                    'triggers': [
                        {'flag': '-p', 'value_mode': 'separated'},
                        {'flag': '--password', 'value_mode': 'equals_or_separated'},
                        {'flag': '/p:', 'value_mode': 'attached'},
                        {'flag': '-Q', 'value_mode': 'attached'},
                    ],
                    'exceptions': custom_exceptions,
                    'defaults_seen': {
                        'triggers': ['-p', '--password', '/p:'],
                        'exceptions': previous_exceptions,
                    },
                }))
                session = Session(shell, config_dir=directory)
                try:
                    for command, display in (
                        ('record --pass UPDATE_SECRET', 'record --pass *************'),
                        ('record -QUPDATE_SECRET', 'record -Q*************'),
                        ('record -pUPDATE_VISIBLE', 'record -pUPDATE_VISIBLE'),
                        ('nmap -p 80', 'nmap -p **'),
                        ('cp -p source destination', 'cp -p source destination'),
                        ('custom-net --pass UPDATE_VISIBLE', 'custom-net --pass UPDATE_VISIBLE'),
                    ):
                        session.send(command)
                        session.wait_for(display)
                        session.send(b'\x03')
                    self.assertNotIn(b'UPDATE_SECRET', session.raw)
                    session.send(F2 + b't\x1b[Fe\r')  # Save the last trigger without changing it.
                    session.wait_for('Triggers saved.')
                    saved = json.loads(config.read_text())
                    self.assertEqual(saved['history_mode'], 'normal')
                    self.assertEqual([r['flag'] for r in saved['triggers']],
                                     ['-p', '--password', '/p:', '-Q', '--pass', '-P'])
                    self.assertEqual(saved['triggers'][0]['value_mode'], 'separated')
                    self.assertEqual(saved['exceptions'], custom_exceptions + DEFAULT_EXCEPTIONS[15:])
                    self.assertEqual(saved['defaults_seen']['triggers'],
                                     ['-p', '--password', '/p:', '--pass', '-P'])
                    self.assertEqual(saved['defaults_seen']['exceptions'], DEFAULT_EXCEPTIONS)
                    # Remove the newly introduced --pass and cp defaults.
                    session.send(b'\x1b[Ad')
                    session.wait_for('Triggers saved.')
                    session.send(F2 + F2 + b'x\x1b[H')
                    session.send(b'\x1b[B' * len(custom_exceptions), duration=0.2)
                    session.send(b'd')
                    session.wait_for('Exceptions saved.')
                    saved = json.loads(config.read_text())
                    self.assertNotIn('--pass', [r['flag'] for r in saved['triggers']])
                    self.assertNotIn('cp', saved['exceptions'])
                    self.assertIn('--pass', saved['defaults_seen']['triggers'])
                    self.assertIn('cp', saved['defaults_seen']['exceptions'])
                    session.send(F2)
                finally:
                    self.assertEqual(session.close(), session.original_tty)
                restarted = Session(shell, config_dir=directory)
                try:
                    for command, display in (
                        ('record --pass UPDATE_VISIBLE', 'record --pass UPDATE_VISIBLE'),
                        ('record -QUPDATE_SECRET', 'record -Q*************'),
                        ('cp -p source destination', 'cp -p ****** destination'),
                        ('nmap -p 80', 'nmap -p **'),
                        ('lsof -p 1234', 'lsof -p 1234'),
                    ):
                        restarted.send(command)
                        restarted.wait_for(display)
                        restarted.send(b'\x03')
                    self.assertNotIn(b'UPDATE_SECRET', restarted.raw)
                finally:
                    self.assertEqual(restarted.close(), restarted.original_tty)

    def test_legacy_defaults_with_added_uppercase_p_gain_pass(self):
        for name in ('bash', 'zsh'):
            shell = shutil.which(name)
            if not shell:
                continue
            with self.subTest(shell=name), tempfile.TemporaryDirectory() as directory:
                config = Path(directory, 'incognito-terminal/config.json')
                config.parent.mkdir()
                config.write_text(json.dumps({
                    'version': 2,
                    'history_mode': 'normal',
                    'triggers': [
                        {'flag': '-p', 'value_mode': 'attached_or_separated'},
                        {'flag': '--password', 'value_mode': 'equals_or_separated'},
                        {'flag': '/p:', 'value_mode': 'attached'},
                        {'flag': '-P', 'value_mode': 'attached_or_separated'},
                    ],
                }))
                session = Session(shell, config_dir=directory)
                try:
                    session.send(F2 + b't')
                    session.wait_for('--pass           After = or a space')
                    session.send(F2)
                    for option in ('--pass ', '--pass='):
                        session.send('record ' + option + 'UPGRADE_SECRET')
                        session.wait_for('record ' + option + '**************')
                        self.assertNotIn(b'UPGRADE_SECRET', session.raw)
                        session.send(b'\x03')
                    # A save from another menu persists the upgraded triggers.
                    session.send(F2 + b'xe\r')
                    session.wait_for('Exceptions saved.')
                    saved = json.loads(config.read_text())
                    self.assertEqual(saved['version'], 6)
                    self.assertEqual([r['flag'] for r in saved['triggers']],
                                     ['-p', '--password', '/p:', '-P', '--pass'])
                finally:
                    self.assertEqual(session.close(), session.original_tty)

    def test_default_exceptions_and_command_boundaries(self):
        for name in ('bash', 'zsh'):
            shell = shutil.which(name)
            if not shell:
                continue
            with self.subTest(shell=name):
                session = Session(shell, columns=120)
                try:
                    # The real scanner is never executed; these are typed fixtures.
                    for command, display in (
                        ('nmap -p 80,443', 'nmap -p 80,443'),
                        ('ssh -p 2222', 'ssh -p 2222'),
                        ('lsof -p 1234', 'lsof -p 1234'),
                        ('ping -p deadbeef example.test', 'ping -p deadbeef example.test'),
                        ('cp -p source destination', 'cp -p source destination'),
                        ('cp -P source destination', 'cp -P source destination'),
                        ('grep -P pattern file', 'grep -P pattern file'),
                        ('htop -p 1234', 'htop -p 1234'),
                        ('gcc -P -E source.c', 'gcc -P -E source.c'),
                        ('lsof -p 1234 | record --pass SECRET_FIXTURE', 'lsof -p 1234 | record --pass **************'),
                        ('cp -p source destination; record -P SECRET_FIXTURE', 'cp -p source destination; record -P **************'),
                        ('record cp --pass SECRET_FIXTURE', 'record cp --pass **************'),
                        ('cp-wrapper -p SECRET_FIXTURE', 'cp-wrapper -p **************'),
                        ('scp -P 2222 file host:/tmp', 'scp -P 2222 file host:/tmp'),
                        ('ffuf -p 0.1 -x http://127.0.0.1:8080', 'ffuf -p 0.1 -x http://127.0.0.1:8080'),
                        ('dirb http://example.test -p 127.0.0.1:8080', 'dirb http://example.test -p 127.0.0.1:8080'),
                        ('feroxbuster -P http://127.0.0.1:8080', 'feroxbuster -P http://127.0.0.1:8080'),
                        ('masscan -p80,443', 'masscan -p80,443'),
                        ('rustscan -p 80,443', 'rustscan -p 80,443'),
                        ('naabu -p 80,443', 'naabu -p 80,443'),
                        ('dirbuster -P -u http://example.test', 'dirbuster -P -u http://example.test'),
                        ('record -P SECRET_FIXTURE', 'record -P **************'),
                        ('ffuf -p 0.1; record -PSECRET_FIXTURE', 'ffuf -p 0.1; record -P**************'),
                        ('redis-cli --pass VISIBLE_FIXTURE', 'redis-cli --pass VISIBLE_FIXTURE'),
                        ('nmap -p 80; record --pass SECRET_FIXTURE', 'nmap -p 80; record --pass **************'),
                        ('nmap -p 80 | record -p SECRET_FIXTURE', 'nmap -p 80 | record -p **************'),
                        ('nmap -p 80 && record --pass="SECRET_FIXTURE"', 'nmap -p 80 && record --pass="**************"'),
                        ('record nmap --pass SECRET_FIXTURE', 'record nmap --pass **************'),
                        ('nmap-wrapper --pass SECRET_FIXTURE', 'nmap-wrapper --pass **************'),
                        ('sudo nmap -p 80', 'sudo nmap -p 80'),
                        ('/usr/bin/nmap -p 80', '/usr/bin/nmap -p 80'),
                    ):
                        session.send(command)
                        session.wait_for(display)
                        session.send(b'\x03')
                    self.assertNotIn(b'SECRET_FIXTURE', session.raw)
                    session.send(F2 + b'x')
                    session.wait_for('Exceptions')
                    for index, command in enumerate(DEFAULT_EXCEPTIONS):
                        if index:
                            session.send(b'\x1b[B', duration=0.02)
                        session.wait_for(command)
                    session.resize(80, 20)
                    session.wait_for('Exceptions')
                    self.assertTrue(session.screen.display[0].endswith('Privacy: On  Menu (F2)'))
                    session.resize(30, 8)
                    session.wait_for('Resize to at least')
                    session.resize(100, 24)
                    session.send(F2)
                finally:
                    self.assertEqual(session.close(), session.original_tty)

    def test_exception_add_edit_remove_clear_and_restart(self):
        for name in ('bash', 'zsh'):
            shell = shutil.which(name)
            if not shell:
                continue
            with self.subTest(shell=name), tempfile.TemporaryDirectory() as directory:
                config = Path(directory, 'incognito-terminal/config.json')
                session = Session(shell, config_dir=directory)
                try:
                    session.send('custom --pass CHANGE_SECRET')
                    session.wait_for('custom --pass *************')
                    session.send(F2 + b'xa')
                    session.wait_for('Add exception')
                    session.send(b'\x1b[200~custom\r\x03\x1b[201~')
                    self.assertFalse(config.exists())
                    session.send(b'\r')
                    session.wait_for('Exceptions saved.')
                    data = json.loads(config.read_text())
                    self.assertEqual(data['version'], 6)
                    self.assertEqual(data['exceptions'], DEFAULT_EXCEPTIONS + ['custom'])
                    self.assertIn('--pass', [rule['flag'] for rule in data['triggers']])
                    self.assertIn('-P', [rule['flag'] for rule in data['triggers']])
                    session.send(F2)
                    session.wait_for('custom --pass CHANGE_SECRET')

                    session.send(F2 + b'xe\x15custom2\r')
                    session.wait_for('Exceptions saved.')
                    self.assertEqual(json.loads(config.read_text())['exceptions'][-1], 'custom2')
                    session.send(F2)
                    session.wait_for('custom --pass *************')
                    session.send(b'\x03')
                    session.send('custom2 --pass VISIBLE_FIXTURE')
                    session.wait_for('custom2 --pass VISIBLE_FIXTURE')
                    session.send(b'\x03')

                    session.send(F2 + b'xa/bad/path\r')
                    session.wait_for('Enter a command name')
                    session.send(b'\x15nmap\r')
                    session.wait_for('already exists')
                    session.send(b'\x03')
                    original = config.read_bytes()
                    session.send(b'e\x15cancelled\x03')
                    self.assertEqual(config.read_bytes(), original)

                    for _ in range(len(DEFAULT_EXCEPTIONS) + 1):
                        session.send(b'd', duration=0.03)
                    session.wait_for('No exceptions.')
                    self.assertEqual(json.loads(config.read_text())['exceptions'], [])
                    session.send(b'e\r')  # An empty list must handle Edit/Enter safely.
                    session.send(F2)
                    session.send('nmap -p 80')
                    session.wait_for('nmap -p **')
                    session.send(b'\x03')
                finally:
                    self.assertEqual(session.close(), session.original_tty)
                restarted = Session(shell, config_dir=directory)
                try:
                    restarted.send('ssh -p 2222')
                    restarted.wait_for('ssh -p ****')
                    restarted.send(b'\x03')
                    restarted.send('record --pass RESTART_SECRET')
                    restarted.wait_for('record --pass **************')
                    self.assertNotIn(b'RESTART_SECRET', restarted.raw)
                finally:
                    self.assertEqual(restarted.close(), restarted.original_tty)
