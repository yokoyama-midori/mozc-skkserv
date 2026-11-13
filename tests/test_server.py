# Copyright (c) 2026 mozc-skkserv contributors. BSD-3-Clause (see LICENSE).
#
# Tests of mozc_skkserv.exe over the skkserv protocol. The test starts the
# servers itself on free local ports.
#
#   python tests/test_server.py [path of mozc_skkserv.exe]
#
# The executable is the argument, or the environment variable
# MOZC_SKKSERV_EXE, or dist\mozc_skkserv.exe (so that a test runner such as
# "python -m unittest discover tests" works too). The tests check the
# protocol, the limits and the EUC-JP output. They do not check which
# candidates Mozc returns, because they depend on the Mozc version.
import os
import socket
import subprocess
import sys
import time
import unittest

EXE = os.environ.get('MOZC_SKKSERV_EXE') or os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    'dist', 'mozc_skkserv.exe')
SERVERS = {}  # name -> (port, process)


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def start(name, *flags):
    port = free_port()
    process = subprocess.Popen(
        [EXE, f'--port={port}', *flags],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    SERVERS[name] = (port, process)


def wait_ready(port, timeout=60):
    end = time.time() + timeout
    while time.time() < end:
        try:
            with socket.create_connection(('127.0.0.1', port), timeout=1):
                return
        except OSError:
            time.sleep(0.3)
    raise RuntimeError(f'the server on port {port} did not start')


def connect(name):
    return socket.create_connection(('127.0.0.1', SERVERS[name][0]), timeout=30)


def read_line(s):
    data = b''
    while not data.endswith(b'\n'):
        chunk = s.recv(65536)
        if not chunk:
            break
        data += chunk
    return data


def lookup(name, request):
    """Sends '1<request> ' and returns the raw response."""
    with connect(name) as s:
        s.sendall(b'1' + request + b' ')
        data = read_line(s)
        s.sendall(b'0')
    return data


def candidates(response, encoding):
    if response == b'4\n':
        return []
    assert response.startswith(b'1/') and response.endswith(b'/\n'), response
    return [p.decode(encoding) for p in response[2:-2].split(b'/')]


# The characters that the server accepts as other forms of the same EUC-JP cell.
ALIASES = str.maketrans({'～': '〜', '∥': '‖', '－': '−', '￠': '¢',
                         '￡': '£', '￢': '¬'})


def is_jisx0208_euc(b):
    """ASCII, half-width katakana (8E xx) and JIS X 0208 rows 1-8 and 16-84."""
    i = 0
    while i < len(b):
        x = b[i]
        if 0x20 <= x <= 0x7E:
            i += 1
            continue
        if i + 1 >= len(b):
            return False
        t = b[i + 1]
        if x == 0x8E:
            if not 0xA1 <= t <= 0xDF:
                return False
        elif 0xA1 <= x <= 0xFE:
            row = x - 0xA0
            if not (0xA1 <= t <= 0xFE and (1 <= row <= 8 or 16 <= row <= 84)):
                return False
        else:
            return False
        i += 2
    return True


READINGS = ['かんじる', 'きょうはいいてんきですね', 'やま', 'から', 'まる',
            'きごう', 'さんかく', 'ゐ', 'ぱーせんと', 'ろーますう']


class Protocol(unittest.TestCase):
    def test_version_and_host(self):
        with connect('utf8') as s:
            s.sendall(b'2')
            self.assertTrue(s.recv(100).startswith(b'mozc-skkserv/'))
            s.sendall(b'3')
            self.assertTrue(s.recv(200))
            s.sendall(b'0')

    def test_completion_is_not_supported(self):
        with connect('utf8') as s:
            s.sendall('4かん '.encode())
            self.assertEqual(s.recv(100), b'4\n')
            s.sendall(b'0')

    def test_lookup_found(self):
        response = lookup('utf8', 'かんじる'.encode())
        self.assertTrue(response.startswith(b'1/'))
        self.assertTrue(candidates(response, 'utf-8'))

    def test_request_split_into_packets(self):
        with connect('utf8') as s:
            s.sendall('1かん'.encode())
            time.sleep(0.3)
            s.sendall('じる '.encode())
            self.assertTrue(read_line(s).startswith(b'1/'))
            s.sendall(b'0')

    def test_not_found(self):
        cases = {
            'katakana': 'カタカナ'.encode(),
            'ascii': b'abc',
            'kanji': '漢字'.encode(),
            'okurigana': 'おくr'.encode(),
            'too long (81 characters)': ('あ' * 81).encode(),
            'truncated sequence': b'\xe3\x81',
            'overlong (C0 80)': b'\xc0\x80',
            'overlong (E0 81 81)': b'\xe0\x81\x81',
            'surrogate U+D800': b'\xed\xa0\x80',
            'out of range U+110000': b'\xf4\x90\x80\x80',
            'five-byte form': b'\xf8\x88\x80\x80\x80',
            'continuation byte': b'\x80',
            '0xFF': b'\xff',
            'invalid tail': 'か'.encode() + b'\xe3',
        }
        for label, request in cases.items():
            with self.subTest(label):
                self.assertEqual(lookup('utf8', request), b'4\n')

    def test_longest_reading_is_accepted(self):
        self.assertTrue(lookup('utf8', ('あ' * 80).encode()).startswith(b'1/'))


class Limits(unittest.TestCase):
    def test_request_without_a_space_is_cut(self):
        with connect('utf8') as s:
            s.sendall(b'1' + b'x' * 6000)
            try:
                closed = s.recv(10) == b''
            except ConnectionResetError:
                closed = True
            self.assertTrue(closed)
        # The server keeps working.
        self.assertTrue(lookup('utf8', 'やま'.encode()).startswith(b'1/'))

    def test_long_reading_is_not_found(self):
        with connect('utf8') as s:
            s.sendall(b'1' + b'x' * 4000)
            time.sleep(0.3)
            s.sendall(b' ')
            self.assertEqual(read_line(s), b'4\n')

    def test_connection_limit(self):
        # A server of its own: connections of other tests may still be counted
        # by the server for a moment after they are closed.
        sockets = [connect('limit') for _ in range(17)]
        try:
            time.sleep(0.5)
            closed = 0
            for s in sockets:
                s.settimeout(0.5)
                try:
                    if s.recv(1, socket.MSG_PEEK) == b'':
                        closed += 1
                except socket.timeout:
                    pass
                except OSError:
                    closed += 1
            self.assertEqual(closed, 1)
        finally:
            for s in sockets:
                s.close()
        # The server counts the closed connections down asynchronously.
        end = time.time() + 10
        while True:
            try:
                if lookup('limit', 'かわ'.encode()).startswith(b'1/'):
                    break
            except OSError:
                pass  # the connection was closed because of the limit
            self.assertLess(time.time(), end)
            time.sleep(0.2)

    def test_max_candidates(self):
        for name, unlimited, encoding, limit in (
                ('utf8_max3', 'utf8', 'utf-8', 3),
                ('euc_max4', 'euc', 'euc_jp', 4)):
            for reading in ('かんじる', 'から', 'だっしゅ'):
                with self.subTest(name=name, reading=reading):
                    request = reading.encode(encoding)
                    total = len(candidates(lookup(unlimited, request), encoding))
                    response = lookup(name, request)
                    self.assertEqual(len(candidates(response, encoding)),
                                     min(limit, total))


class EucJp(unittest.TestCase):
    def test_euc_output_matches_utf8_output(self):
        for reading in READINGS:
            with self.subTest(reading):
                full = candidates(lookup('utf8', reading.encode()), 'utf-8')
                expected = []
                seen = set()
                for c in full:
                    try:
                        b = c.translate(ALIASES).encode('euc_jp')
                    except UnicodeEncodeError:
                        continue
                    if is_jisx0208_euc(b) and b not in seen:
                        seen.add(b)
                        expected.append(b)
                response = lookup('euc', reading.encode('euc_jp'))
                got = [] if response == b'4\n' else response[2:-2].split(b'/')
                self.assertEqual(got, expected)

    def test_utf8_request_to_euc_server(self):
        self.assertEqual(lookup('euc', 'かんじる'.encode()), b'4\n')

    def test_truncated_euc_request(self):
        self.assertEqual(lookup('euc', 'かんじる'.encode('euc_jp')[:3]), b'4\n')

    def test_euc_unencodable_reading(self):
        self.assertEqual(lookup('euc', 'カタカナ'.encode('euc_jp')), b'4\n')


def stop_servers():
    for _, process in SERVERS.values():
        process.kill()
    for _, process in SERVERS.values():
        process.wait()


def setUpModule():
    if not os.path.exists(EXE):
        raise unittest.SkipTest(f'The server was not found: {EXE}')
    # tearDownModule() is not called if setUpModule() fails.
    unittest.addModuleCleanup(stop_servers)
    start('utf8', '--max_candidates=1000')
    start('limit')
    start('utf8_max3', '--max_candidates=3')
    start('euc', '--encoding=euc-jp', '--max_candidates=1000')
    start('euc_max4', '--encoding=euc-jp', '--max_candidates=4')
    for port, _ in SERVERS.values():
        wait_ready(port)


if __name__ == '__main__':
    if len(sys.argv) > 1:
        EXE = sys.argv.pop(1)
    EXE = os.path.abspath(EXE)
    if not os.path.exists(EXE):
        sys.exit(f'The server was not found: {EXE}')
    unittest.main(verbosity=2)
