"""Real-shell regression matrix for the native executable."""
import json
from pathlib import Path
import shlex
import shutil
import tempfile
import time
import unittest
import sys

from test_pty import Session


# Typed source, expected display, argument received by the shell command.
VALUES = (
    ('Q9xy', '****', 'Q9xy'),
    ("'Q9xy'", "'****'", 'Q9xy'),
    ('"Q9xy"', '"****"', 'Q9xy'),
    ("''", "''", ''),
    ('""', '""', ''),
    ("'two words'", "'*********'", 'two words'),
    ('"two words"', '"*********"', 'two words'),
    ("abc'dcb'", "***'***'", 'abcdcb'),
    ('abc"dcb"', '***"***"', 'abcdcb'),
    ('"abc\'def"', '"*******"', "abc'def"),
    ("'abc\"def'", "'*******'", 'abc"def'),
    (r"abc\'def", '********', "abc'def"),
    (r'abc\"def', '********', 'abc"def'),
    (r'ab\ cd', '******', 'ab cd'),
    (r'ab\\cd', '******', 'ab\\cd'),
    (r'"ab\" cd"', '"*******"', 'ab" cd'),
    (r"$'ab\' cd'", "*'*******'", "ab' cd"),
    ("'!;|&()<>'", "'********'", '!;|&()<>'),
    ("'abc'\"def\"ghi", "'***'\"***\"***", 'abcdefghi'),
    ("'密🔐码'", "'***'", '密🔐码'),
)
# Prefix, flag in argv, whether value is a separate argument.
PREFIXES = (
    ('-p', '-p', False), ('-p ', '-p', True),
    ('-p   ', '-p', True), ('-p\t', '-p', True),
    ('--password ', '--password', True),
    ('--password\t', '--password', True),
    ('--password=', '--password=', False), ('/p:', '/p:', False),
    ('--pass ', '--pass', True), ('--pass=', '--pass=', False),
    ('-P', '-P', False), ('-P ', '-P', True), ('-P   ', '-P', True), ('-P\t', '-P', True),
)


class AuditTests(unittest.TestCase):
    def test_real_shell_paste_matrix_and_exact_execution(self):
        for name in ('bash', 'zsh'):
            shell = shutil.which(name)
            if shell is None:
                continue
            with self.subTest(shell=name), tempfile.TemporaryDirectory(prefix='privacy-audit-') as folder:
                log = Path(folder, 'argv.jsonl')
                recorder = Path(folder, 'record.py')
                recorder.write_text('import json,sys\n'
                    f'with open({str(log)!r}, "a") as f: f.write(json.dumps(sys.argv[1:])+"\\n")\n')
                session = Session(shell, columns=120)
                try:
                    session.send(f'record() {{ {shlex.quote(sys.executable)} {shlex.quote(str(recorder))} "$@"; }}\r')
                    expected = []
                    # Exercise every value form across the supported trigger
                    # formats while checking both the display and real argv.
                    for prefix, flag, separate in (PREFIXES[1], PREFIXES[4], PREFIXES[6], PREFIXES[7], PREFIXES[8], PREFIXES[9], PREFIXES[10], PREFIXES[11]):
                        for typed, masked, value in VALUES:
                            with self.subTest(prefix=prefix, typed=typed):
                                command = 'record ' + prefix + typed + ' tail'
                                # The recorder file can be written before Bash/Zsh
                                # restores its prompt and bracketed-paste mode.
                                # Paste only when the active input row is ready.
                                deadline = time.monotonic() + 3
                                while session.screen.display[session.screen.cursor.y].strip() != 'incognito-terminal>':
                                    if time.monotonic() > deadline:
                                        self.fail('Shell did not return to its prompt')
                                    session.read(0.02)
                                session.read(0.02)
                                session.send('\x1b[200~' + command + '\x1b[201~', duration=0.015)
                                # Wide characters take two screen columns.
                                visible = masked if value != '密🔐码' else "'******'"
                                session.wait_for('record ' + prefix + visible + ' tail')
                                session.send('\r', duration=0.015)
                                expected.append(([flag, value] if separate else [flag + value]) + ['tail'])
                                deadline = time.monotonic() + 3
                                while (not log.exists() or len(log.read_text().splitlines()) < len(expected)):
                                    if time.monotonic() > deadline:
                                        self.fail('Command did not execute')
                                    session.read(0.02)
                                self.assertEqual([json.loads(line) for line in log.read_text().splitlines()], expected)
                finally:
                    self.assertEqual(session.close(), session.original_tty)

    def test_unfinished_quoted_multiline_input_stays_hidden(self):
        for name in ('bash', 'zsh'):
            shell = shutil.which(name)
            if shell is None:
                continue
            for quote in ("'", '"'):
                with self.subTest(shell=name, quote=quote):
                    session = Session(shell)
                    try:
                        session.send('echo -p ' + quote + 'FIRST_FIXTURE\r')
                        session.send('SECOND_FIXTURE')
                        self.assertIsNone(session.process.poll())
                        self.assertNotIn(b'FIRST_FIXTURE', session.raw)
                        self.assertNotIn(b'SECOND_FIXTURE', session.raw)
                    finally:
                        session.close()

    def test_backslash_continuation_before_separated_value(self):
        for name in ('bash', 'zsh'):
            shell = shutil.which(name)
            if shell is None:
                continue
            with self.subTest(shell=name), tempfile.TemporaryDirectory(prefix='privacy-continuation-') as folder:
                log = Path(folder, 'argv.jsonl')
                recorder = Path(folder, 'record.py')
                recorder.write_text('import json,sys\n'
                    f'with open({str(log)!r}, "a") as f: f.write(json.dumps(sys.argv[1:])+"\\n")\n')
                session = Session(shell)
                try:
                    session.send(f'record() {{ {shlex.quote(sys.executable)} {shlex.quote(str(recorder))} "$@"; }}\r')
                    cases = (
                        ('-p', 'CONT_P'),
                        ('--password', 'CONT_PASSWORD'),
                        ('--pass', 'CONT_PASS'),
                        ('-P', 'CONT_CAPITAL'),
                    )
                    expected = []
                    for flag, secret in cases:
                        session.wait_for('incognito-terminal>')
                        offset = len(session.raw)
                        session.send('record ' + flag + ' ' + '\\' + '\r')
                        session.send('    ' + secret + ' tail')
                        session.wait_for('    ' + '*' * len(secret) + ' tail')
                        self.assertNotIn(secret.encode(), session.raw[offset:])
                        session.send('\r')
                        expected.append([flag, secret, 'tail'])
                        deadline = time.monotonic() + 3
                        while (not log.exists() or len(log.read_text().splitlines()) < len(expected)):
                            if time.monotonic() > deadline:
                                self.fail('Continued command did not execute')
                            session.read(.02)
                        self.assertEqual([json.loads(line) for line in log.read_text().splitlines()], expected)

                    session.wait_for('incognito-terminal>')
                    offset = len(session.raw)
                    session.send('record -p ' + '\\' + '\r')
                    session.send('  ' + '\\' + '\r')
                    session.send('    REPEATED_CONT tail')
                    session.wait_for('    ************* tail')
                    self.assertNotIn(b'REPEATED_CONT', session.raw[offset:])
                    session.send('\r')
                    expected.append(['-p', 'REPEATED_CONT', 'tail'])
                    deadline = time.monotonic() + 3
                    while len(log.read_text().splitlines()) < len(expected):
                        if time.monotonic() > deadline:
                            self.fail('Repeated continuation did not execute')
                        session.read(.02)
                    self.assertEqual([json.loads(line) for line in log.read_text().splitlines()], expected)
                finally:
                    self.assertEqual(session.close(), session.original_tty)



if __name__ == '__main__':
    unittest.main()
