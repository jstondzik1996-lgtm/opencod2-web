"""Tests for player sessions, join tickets, bans and their enforcement.

Covers tools/master_proxy/sessions.py, the /session /matchmake /join routes
with sessions on, and the relay's ticket check (websockets stubbed: only the
admission logic is exercised, not WebSocket traffic).
"""
import http.client
import importlib
import json
import os
import sys
import tempfile
import threading
import types
import unittest
from http.server import ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, 'master_proxy'))

import directory  # noqa: E402
import server as master  # noqa: E402
import sessions  # noqa: E402
from test_server_directory import FakeCod2Server, entry  # noqa: E402

SECRET = 'test-secret-0123456789abcdef'
KEY = 'k' * 32


class Clock:
    def __init__(self):
        self.now = 1_700_000_000.0

    def __call__(self):
        return self.now


class TokenTests(unittest.TestCase):
    def setUp(self):
        self.clock = Clock()
        self.s = sessions.Signer(SECRET, clock=self.clock)

    def test_nick_cleaning(self):
        self.assertEqual(sessions.clean_nick('^1Pan ^7Kapral'), 'Pan_Kapral')
        self.assertEqual(sessions.clean_nick('a\\b"c;d+e'), 'abcde', 'no userinfo/command injection')
        self.assertEqual(sessions.clean_nick('x' * 30), 'x' * 16)
        self.assertIsNone(sessions.clean_nick('a'))
        self.assertIsNone(sessions.clean_nick('^1'))
        self.assertIsNone(sessions.clean_nick(None))

    def test_session_and_ticket_roundtrip(self):
        sess = self.s.session(KEY, 'Gracz')
        self.assertEqual(sess['pid'], sessions.player_id(KEY))
        self.assertEqual(sessions.player_id(KEY), sessions.player_id(KEY), 'stable id per browser key')
        payload = self.s.verify(sess['token'], 'session')
        self.assertEqual(payload['nick'], 'Gracz')
        ticket = self.s.ticket(payload, '127.0.0.1:28961')
        t = self.s.verify(ticket, 'ticket')
        self.assertEqual((t['srv'], t['pid']), ('127.0.0.1:28961', sess['pid']))
        with self.assertRaises(sessions.TokenError):
            self.s.verify(ticket, 'session')
        with self.assertRaises(sessions.TokenError):
            self.s.verify(sess['token'], 'ticket')

    def test_rejections(self):
        sess = self.s.session(KEY, 'Gracz')['token']
        body, sig = sess.split('.')
        forged = sessions._b64e(sessions._b64d(body).replace(b'Gracz', b'Admin')) + '.' + sig
        for bad in (forged, sess + 'x', 'nope', '', None, 'a.b.c',
                    sessions.Signer('other-secret-0123456789').session(KEY, 'Gracz')['token']):
            with self.assertRaises(sessions.TokenError, msg=bad):
                self.s.verify(bad, 'session')
        self.clock.now += sessions.SESSION_TTL + 1
        with self.assertRaises(sessions.TokenError):
            self.s.verify(sess, 'session')

    def test_short_secret_refused(self):
        with self.assertRaises(ValueError):
            sessions.Signer('short')

    def test_banlist_reload(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, 'bans.txt')
            bans = sessions.BanList(path)
            pid = sessions.player_id(KEY)
            self.assertFalse(bans.banned(pid), 'missing file = no bans')
            with open(path, 'w') as fh:
                fh.write('# comment\n%s  # cheater\nnot-a-pid\n' % pid.upper())
            self.assertTrue(bans.banned(pid))
            os.utime(path, (1, 1))
            with open(path, 'w') as fh:
                fh.write('')
            os.utime(path, (2, 2))
            self.assertFalse(bans.banned(pid))


class RouteTests(unittest.TestCase):
    def setUp(self):
        self.a = FakeCod2Server(players=0, maxclients=2)
        self.b = FakeCod2Server(players=2, maxclients=2)
        self.d = directory.ServerDirectory(directory.parse_config({'servers': [
            entry('a', self.a.port), entry('b', self.b.port)]}), response_window=0.5)
        self.d.poll_once()
        self.tmp = tempfile.TemporaryDirectory()
        self.bans_path = os.path.join(self.tmp.name, 'bans.txt')
        self.saved = (master.DIRECTORY, master.SIGNER, master.BANS,
                      master._matchmake_limit, master._session_limit)
        master.DIRECTORY = self.d
        master.SIGNER = sessions.Signer(SECRET)
        master.BANS = sessions.BanList(self.bans_path)
        master._matchmake_limit = master.RateLimiter(100)
        master._session_limit = master.RateLimiter(100)
        self.httpd = ThreadingHTTPServer(('127.0.0.1', 0), master.Handler)
        self.httpd.daemon_threads = True
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def tearDown(self):
        self.httpd.shutdown()
        self.httpd.server_close()
        (master.DIRECTORY, master.SIGNER, master.BANS,
         master._matchmake_limit, master._session_limit) = self.saved
        self.a.close()
        self.b.close()
        self.tmp.cleanup()

    def post(self, path, body):
        conn = http.client.HTTPConnection('127.0.0.1', self.httpd.server_port, timeout=5)
        conn.request('POST', path, body=json.dumps(body).encode(), headers={'Content-Type': 'application/json'})
        resp = conn.getresponse()
        data = json.loads(resp.read() or b'{}')
        conn.close()
        return resp.status, data

    def test_session_route(self):
        status, data = self.post('/session', {'nick': '^3Sgt ^7Kowalski', 'playerKey': KEY})
        self.assertEqual(status, 200)
        self.assertEqual(data['nick'], 'Sgt_Kowalski')
        self.assertEqual(data['pid'], sessions.player_id(KEY))
        self.assertEqual(self.post('/session', {'nick': 'x', 'playerKey': KEY})[0], 400)
        self.assertEqual(self.post('/session', {'nick': 'Gracz', 'playerKey': 'short'})[0], 400)
        with open(self.bans_path, 'w') as fh:
            fh.write(sessions.player_id(KEY) + '\n')
        self.assertEqual(self.post('/session', {'nick': 'Gracz', 'playerKey': KEY}), (403, {'error': 'banned'}))

    def test_matchmake_and_join_need_session_and_return_tickets(self):
        status, data = self.post('/matchmake', {})
        self.assertEqual((status, data['error']), (401, 'session_required'))
        token = self.post('/session', {'nick': 'Gracz', 'playerKey': KEY})[1]['token']
        status, pick = self.post('/matchmake', {'session': token})
        self.assertEqual((status, pick['id']), (200, 'a'))
        t = master.SIGNER.verify(pick['ticket'], 'ticket')
        self.assertEqual((t['srv'], t['nick']), (pick['connect'], 'Gracz'))
        status, joined = self.post('/join', {'session': token, 'serverId': 'a'})
        self.assertEqual(status, 200)
        self.assertEqual(master.SIGNER.verify(joined['ticket'], 'ticket')['srv'], joined['connect'])
        self.assertEqual(self.post('/join', {'session': token, 'serverId': 'a'}), (409, {'error': 'full'}),
                         'two reservations fill a 2-slot server')
        self.assertEqual(self.post('/join', {'session': token, 'serverId': 'b'}), (409, {'error': 'full'}))
        self.assertEqual(self.post('/join', {'session': token, 'serverId': 'zz'}), (404, {'error': 'unknown'}))
        with open(self.bans_path, 'w') as fh:
            fh.write(sessions.player_id(KEY) + '\n')
        self.assertEqual(self.post('/matchmake', {'session': token}), (403, {'error': 'banned'}))


class RelayTicketTests(unittest.TestCase):
    def setUp(self):
        stub = types.ModuleType('websockets')
        stub.server = types.ModuleType('websockets.server')
        stub.exceptions = types.ModuleType('websockets.exceptions')
        self.saved_modules = {k: sys.modules.get(k) for k in ('websockets', 'websockets.server', 'relay')}
        sys.modules['websockets'] = stub
        sys.modules['websockets.server'] = stub.server
        sys.modules.pop('relay', None)
        sys.path.insert(0, os.path.join(HERE, 'game_proxy'))
        old = os.environ.get('COD2_SESSION_SECRET')
        os.environ['COD2_SESSION_SECRET'] = SECRET
        try:
            self.relay = importlib.import_module('relay')
        finally:
            if old is None:
                del os.environ['COD2_SESSION_SECRET']
            else:
                os.environ['COD2_SESSION_SECRET'] = old
        self.relay.ALLOW_ANY = True
        self.tmp = tempfile.TemporaryDirectory()
        self.relay.BANS = sessions.BanList(os.path.join(self.tmp.name, 'bans.txt'))
        self.signer = sessions.Signer(SECRET)
        self.session = self.signer.session(KEY, 'Gracz')

    def tearDown(self):
        self.tmp.cleanup()
        for k, v in self.saved_modules.items():
            if v is None:
                sys.modules.pop(k, None)
            else:
                sys.modules[k] = v

    def path(self, **q):
        from urllib.parse import urlencode
        return '/?' + urlencode(dict(host='127.0.0.1', port=28961, **q))

    def test_ticket_rules(self):
        payload = self.signer.verify(self.session['token'], 'session')
        good = self.signer.ticket(payload, '127.0.0.1:28961')
        other = self.signer.ticket(payload, '127.0.0.1:28962')
        self.assertEqual(self.relay.parse_target(self.path(ticket=good), None), ('127.0.0.1', 28961))
        self.assertEqual(self.relay.parse_target(self.path(session=self.session['token']), None),
                         ('127.0.0.1', 28961), 'session fallback for reconnects')
        for bad in (dict(), dict(ticket=other), dict(ticket='junk'),
                    dict(session=good), dict(ticket=self.session['token'])):
            with self.assertRaises(ValueError, msg=bad):
                self.relay.parse_target(self.path(**bad), None)
        with open(self.relay.BANS.path, 'w') as fh:
            fh.write(self.session['pid'] + '\n')
        with self.assertRaises(ValueError):
            self.relay.parse_target(self.path(ticket=good), None)


if __name__ == '__main__':
    unittest.main()
