"""Tests for the project server directory, Quick Play and their HTTP routes.

Fake dedicated servers are real UDP sockets that answer the connectionless
``getstatus`` query like a CoD2 server, so the poller, parser, matchmaker and
HTTP layer are exercised end to end without game assets.
"""
import http.client
import json
import os
import socket
import sys
import threading
import time
import unittest
from http.server import ThreadingHTTPServer

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'master_proxy'))

import directory  # noqa: E402
import server as master  # noqa: E402
import serve_web_local as web  # noqa: E402


class FakeCod2Server:
    """Answers getstatus with configurable map, players and sv_maxclients."""

    def __init__(self, players=0, maxclients=4, mapname='mp_toujane', gametype='dm',
                 hostname='^1Fake', password=False):
        self.players = players
        self.maxclients = maxclients
        self.mapname = mapname
        self.gametype = gametype
        self.hostname = hostname
        self.password = password
        self.queries = 0
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(('127.0.0.1', 0))
        self.sock.settimeout(0.2)
        self.port = self.sock.getsockname()[1]
        self._stop = False
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def _run(self):
        while not self._stop:
            try:
                data, addr = self.sock.recvfrom(2048)
            except socket.timeout:
                continue
            except OSError:
                return
            if data.startswith(b'\xff\xff\xff\xffgetstatus'):
                self.queries += 1
                rules = ('\\sv_maxclients\\%d\\mapname\\%s\\g_gametype\\%s\\sv_hostname\\%s\\pswrd\\%d'
                         % (self.maxclients, self.mapname, self.gametype, self.hostname,
                            1 if self.password else 0))
                lines = ['%d %d "Player%d"' % (i * 3, 40 + i, i) for i in range(self.players)]
                body = 'statusResponse\n' + rules + '\n' + ''.join(l + '\n' for l in lines)
                self.sock.sendto(b'\xff\xff\xff\xff' + body.encode('latin-1'), addr)

    def close(self):
        self._stop = True
        self.sock.close()
        self._thread.join(timeout=2)


def free_udp_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(('127.0.0.1', 0))
    port = s.getsockname()[1]
    s.close()
    return port


class Clock:
    def __init__(self):
        self.now = 1000.0

    def __call__(self):
        return self.now


def entry(sid, port, max_players=4, **kw):
    return dict(id=sid, name=sid.upper(), host='127.0.0.1', port=port,
                maxPlayers=max_players, region=kw.pop('region', 'eu'), mode=kw.pop('mode', 'dm'), **kw)


class ParseTests(unittest.TestCase):
    def test_parse_status(self):
        data = (b'\xff\xff\xff\xffstatusResponse\n\\sv_maxclients\\32\\mapname\\mp_carentan'
                b'\\g_gametype\\tdm\n5 48 "Alpha"\n0 999 "B \\"q\\""\n')
        parsed = directory.parse_status(data)
        self.assertEqual(parsed['rules']['mapname'], 'mp_carentan')
        self.assertEqual(parsed['rules']['sv_maxclients'], '32')
        self.assertEqual(parsed['players'][0], {'score': 5, 'ping': 48, 'name': 'Alpha'})
        self.assertEqual(len(parsed['players']), 2)

    def test_rejects_other_packets(self):
        self.assertIsNone(directory.parse_status(b'\xff\xff\xff\xffinfoResponse\n\\a\\b'))

    def test_config_validation(self):
        good = directory.parse_config({'servers': [entry('a', 28961)]})
        self.assertEqual(good[0].port, 28961)
        bad = [
            {'servers': [entry('a b', 1)]},
            {'servers': [dict(entry('a', 28961), host='example.com')]},
            {'servers': [entry('a', 0)]},
            {'servers': [entry('a', 28961), entry('a', 28962)]},
            {'servers': [entry('a', 28961), entry('b', 28961)]},
            {'servers': [entry('a', 28961, max_players=99)]},
            {'nope': []},
        ]
        for cfg in bad:
            with self.assertRaises(directory.ConfigError, msg=cfg):
                directory.parse_config(cfg)


class DirectoryTests(unittest.TestCase):
    def setUp(self):
        self.fakes = []
        self.clock = Clock()

    def tearDown(self):
        for f in self.fakes:
            f.close()

    def fake(self, **kw):
        f = FakeCod2Server(**kw)
        self.fakes.append(f)
        return f

    def make(self, entries, **kw):
        d = directory.ServerDirectory(directory.parse_config({'servers': entries}),
                                      response_window=0.5, clock=self.clock, **kw)
        d.poll_once()
        return d

    def test_status_offline_and_capacity(self):
        a = self.fake(players=2, maxclients=3, hostname='^2Colour ^7Name')
        dead = free_udp_port()
        d = self.make([entry('a', a.port, max_players=32), entry('dead', dead)])
        listed = {s['id']: s for s in d.list_servers()}
        self.assertEqual(listed['a']['status'], 'online')
        self.assertEqual(listed['a']['players'], 2)
        self.assertEqual(listed['a']['maxPlayers'], 3, 'server sv_maxclients caps the config')
        self.assertEqual(listed['a']['free'], 1)
        self.assertEqual(listed['a']['map'], 'mp_toujane')
        self.assertLess(listed['a']['ping_ms'], 999)
        self.assertEqual(listed['dead']['status'], 'offline')
        self.assertEqual(listed['dead']['free'], 0)
        ml = d.masterlist()
        self.assertEqual([s['port'] for s in ml['servers']], [a.port])
        self.assertEqual(ml['servers'][0]['clients'], 2)
        self.assertEqual(len(ml['servers'][0]['playerinfo']), 2)
        self.assertEqual(ml['source'], 'project')

    def test_goes_stale(self):
        a = self.fake()
        d = self.make([entry('a', a.port)], stale_after=10)
        self.assertEqual(d.list_servers()[0]['status'], 'online')
        a.close()
        self.fakes.remove(a)
        self.clock.now += 11
        d.poll_once()
        self.assertEqual(d.list_servers()[0]['status'], 'offline')
        self.assertIsNone(d.matchmake())

    def test_stale_directory_is_not_advertised_if_poller_stalls(self):
        # Simulate the background polling thread not running: list, master and
        # Quick Play must all reject an expired heartbeat without poll_once().
        a = self.fake(players=1)
        d = self.make([entry('a', a.port)], stale_after=10)
        self.assertEqual(d.list_servers()[0]['status'], 'online')
        self.assertEqual(len(d.masterlist()['servers']), 1)

        self.clock.now += 11
        stale = d.list_servers()[0]
        self.assertEqual(stale['status'], 'offline')
        self.assertEqual(stale['free'], 0)
        self.assertEqual(d.masterlist()['servers'], [])
        self.assertIsNone(d.matchmake(), 'Quick Play must not allocate a stale server')

        # A fresh getstatus response on the next sweep restores availability.
        d.poll_once()
        self.assertEqual(d.list_servers()[0]['status'], 'online')
        self.assertIsNotNone(d.matchmake())

    def test_quick_play_fills_then_overflows(self):
        a = self.fake(players=2, maxclients=4)
        b = self.fake(players=0, maxclients=4)
        d = self.make([entry('a', a.port), entry('b', b.port)])
        picks = [d.matchmake() for _ in range(4)]
        self.assertEqual([p['id'] for p in picks], ['a', 'a', 'b', 'b'],
                         'fill the busier server, overflow to the next')
        self.assertTrue(all(p['reservation'] for p in picks))
        self.assertEqual(picks[2]['connect'], '127.0.0.1:%d' % b.port)

    def test_reservations_prevent_overbooking_and_expire(self):
        a = self.fake(players=3, maxclients=4)
        d = self.make([entry('a', a.port)], reservation_ttl=30)
        first = d.matchmake()
        self.assertEqual(first['id'], 'a')
        self.assertIsNone(d.matchmake(), 'the last slot is reserved')
        self.assertTrue(d.release(first['reservation']))
        self.assertIsNotNone(d.matchmake())
        self.assertIsNone(d.matchmake())
        self.clock.now += 31
        self.assertIsNotNone(d.matchmake(), 'expired reservations free the slot')

    def test_concurrent_matchmake_never_exceeds_capacity(self):
        a = self.fake(players=0, maxclients=4)
        b = self.fake(players=0, maxclients=4)
        d = self.make([entry('a', a.port), entry('b', b.port)])
        results = []
        lock = threading.Lock()

        def worker():
            r = d.matchmake()
            with lock:
                results.append(r)

        threads = [threading.Thread(target=worker) for _ in range(12)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        got = [r['id'] for r in results if r]
        self.assertEqual(sorted(got), ['a'] * 4 + ['b'] * 4)
        self.assertEqual(sum(1 for r in results if r is None), 4)

    def test_filters(self):
        full = self.fake(players=4, maxclients=4)
        locked = self.fake(players=0, password=True)
        priv = self.fake(players=0)
        tdm = self.fake(players=0, gametype='tdm')
        us = self.fake(players=0)
        d = self.make([entry('full', full.port), entry('locked', locked.port),
                       entry('priv', priv.port, private=True),
                       entry('tdm', tdm.port, mode='tdm'),
                       entry('us', us.port, region='us')])
        self.assertNotIn('priv', [s['id'] for s in d.list_servers()])
        self.assertEqual(d.matchmake(mode='tdm')['id'], 'tdm')
        self.assertEqual(d.matchmake(region='us')['id'], 'us')
        self.assertIsNone(d.matchmake(mode='dm', region='eu'))
        self.assertEqual(d.matchmake(exclude=['tdm'])['id'], 'us')
        self.assertTrue(d.allows('127.0.0.1', priv.port))
        self.assertFalse(d.allows('127.0.0.1', 1))


class HttpRouteTests(unittest.TestCase):
    def setUp(self):
        self.a = FakeCod2Server(players=1, maxclients=2)
        self.d = directory.ServerDirectory(
            directory.parse_config({'servers': [entry('a', self.a.port)]}), response_window=0.5)
        self.d.poll_once()
        self.old_dir = master.DIRECTORY
        self.old_limit = master._matchmake_limit
        master.DIRECTORY = self.d
        master._matchmake_limit = master.RateLimiter(5)
        self.httpd = ThreadingHTTPServer(('127.0.0.1', 0), master.Handler)
        self.httpd.daemon_threads = True
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def tearDown(self):
        self.httpd.shutdown()
        self.httpd.server_close()
        master.DIRECTORY = self.old_dir
        master._matchmake_limit = self.old_limit
        self.a.close()

    def req(self, method, path, body=None, port=None, raw=None):
        conn = http.client.HTTPConnection('127.0.0.1', port or self.httpd.server_port, timeout=5)
        payload = raw if raw is not None else (json.dumps(body).encode() if body is not None else None)
        headers = {'Content-Type': 'application/json'} if payload is not None else {}
        conn.request(method, path, body=payload, headers=headers)
        resp = conn.getresponse()
        data = resp.read()
        conn.close()
        ctype = resp.getheader('Content-Type') or ''
        return resp.status, (json.loads(data) if data and 'json' in ctype else data)

    def test_routes(self):
        status, data = self.req('GET', '/servers')
        self.assertEqual(status, 200)
        self.assertEqual(data['servers'][0]['players'], 1)
        status, data = self.req('GET', '/masterlist/cod2/1.3')
        self.assertEqual(status, 200)
        self.assertEqual(data['servers'][0]['port'], self.a.port)
        status, data = self.req('GET', '/external/masterlist/cod2/1.3')
        self.assertEqual(status, 404, 'external list is off by default')
        status, data = self.req('POST', '/matchmake', {'mode': 'dm'})
        self.assertEqual(status, 200)
        self.assertEqual(data['connect'], '127.0.0.1:%d' % self.a.port)
        status, data2 = self.req('POST', '/matchmake', {})
        self.assertEqual(status, 503)
        self.assertEqual(data2['error'], 'no_free_server')
        status, rel = self.req('POST', '/matchmake/release', {'reservation': data['reservation']})
        self.assertEqual((status, rel['released']), (200, True))
        status, _ = self.req('POST', '/matchmake', raw=b'{not json')
        self.assertEqual(status, 400)
        status, health = self.req('GET', '/health')
        self.assertEqual(health['directory']['online'], 1)

    def test_rate_limit(self):
        codes = [self.req('POST', '/matchmake/release', {'reservation': 'x'})[0] for _ in range(3)]
        self.assertEqual(codes, [200, 200, 200], 'release is not rate limited')
        codes = [self.req('POST', '/matchmake', {'exclude': ['a']})[0] for _ in range(7)]
        self.assertEqual(codes[:5], [503] * 5)
        self.assertEqual(codes[5:], [429, 429])

    def test_local_web_server_forwards_post(self):
        old_cfg = web.CFG.copy()
        web.CFG.update(webroot=os.path.dirname(os.path.abspath(__file__)), coop=False,
                       master_port=self.httpd.server_port, relay_port=1)
        front = ThreadingHTTPServer(('127.0.0.1', 0), web.Handler)
        front.daemon_threads = True
        threading.Thread(target=front.serve_forever, daemon=True).start()
        try:
            status, data = self.req('POST', '/play/api/matchmake', {}, port=front.server_port)
            self.assertEqual(status, 200)
            self.assertEqual(data['id'], 'a')
            status, data = self.req('GET', '/play/api/servers', port=front.server_port)
            self.assertEqual(data['servers'][0]['reserved'], 1)
            status, _ = self.req('POST', '/cod2.html', {}, port=front.server_port)
            self.assertEqual(status, 404)
        finally:
            front.shutdown()
            front.server_close()
            web.CFG.clear()
            web.CFG.update(old_cfg)



class RelayAllowListTests(unittest.TestCase):
    """The relay must follow the project directory, including servers that
    come online after its last refresh. websockets is stubbed: only the
    allowlist logic is imported, no WebSocket traffic is tested here."""

    def test_allowlist_follows_directory_and_refreshes_on_miss(self):
        import importlib
        import types
        stub = types.ModuleType('websockets')
        stub.server = types.ModuleType('websockets.server')
        stub.exceptions = types.ModuleType('websockets.exceptions')
        saved = {k: sys.modules.get(k) for k in ('websockets', 'websockets.server', 'relay')}
        sys.modules['websockets'] = stub
        sys.modules['websockets.server'] = stub.server
        sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'game_proxy'))
        a = FakeCod2Server()
        late_port = free_udp_port()
        d = directory.ServerDirectory(directory.parse_config(
            {'servers': [entry('a', a.port), entry('late', late_port)]}), response_window=0.5)
        d.poll_once()
        old_dir = master.DIRECTORY
        master.DIRECTORY = d
        httpd = ThreadingHTTPServer(('127.0.0.1', 0), master.Handler)
        httpd.daemon_threads = True
        threading.Thread(target=httpd.serve_forever, daemon=True).start()
        late = None
        try:
            relay = importlib.import_module('relay')
            relay.MASTERLIST_URL = 'http://127.0.0.1:%d/masterlist/cod2/1.3' % httpd.server_port
            relay.ALLOWLIST_MISS_REFRESH = 0.0
            relay.ALLOW_ANY = False
            allow = relay.AllowList()
            allow._refresh()
            self.assertTrue(allow.allows('127.0.0.1', a.port))
            self.assertFalse(allow.allows('127.0.0.1', late_port), 'offline server refused')
            self.assertFalse(allow.allows('8.8.8.8', 53), 'not an open proxy')
            late = FakeCod2Server()
            late.sock.close()
            late.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            late.sock.bind(('127.0.0.1', late_port))
            late.sock.settimeout(0.2)
            late._thread = threading.Thread(target=late._run, daemon=True)
            late._thread.start()
            d.poll_once()
            self.assertTrue(allow.allows('127.0.0.1', late_port), 'refreshed on miss')
        finally:
            httpd.shutdown()
            httpd.server_close()
            master.DIRECTORY = old_dir
            a.close()
            if late:
                late.close()
            for k, v in saved.items():
                if v is None:
                    sys.modules.pop(k, None)
                else:
                    sys.modules[k] = v


if __name__ == '__main__':
    unittest.main()
