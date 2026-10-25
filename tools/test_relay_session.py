"""Runs the real relay_handler (tools/game_proxy/relay.py) with a fake
WebSocket object and a real UDP echo server.

Covers: forwarding both ways in order, the bounded drop-oldest send queue for
a slow browser, the upstream packet-rate limit, the per-address session limit
(including X-Forwarded-For only from a loopback peer) and cleanup. The
websockets library itself is stubbed, so the WebSocket protocol is not tested
here.
"""
import asyncio
import importlib
import os
import socket
import sys
import threading
import types
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))


class Closed(Exception):
    pass


def load_relay():
    stub = types.ModuleType('websockets')
    stub.server = types.ModuleType('websockets.server')
    stub.exceptions = types.ModuleType('websockets.exceptions')
    stub.exceptions.ConnectionClosed = Closed
    saved = {k: sys.modules.get(k) for k in ('websockets', 'websockets.server', 'websockets.exceptions', 'relay')}
    sys.modules['websockets'] = stub
    sys.modules['websockets.server'] = stub.server
    sys.modules['websockets.exceptions'] = stub.exceptions
    sys.modules.pop('relay', None)
    sys.path.insert(0, os.path.join(HERE, 'game_proxy'))
    old_secret = os.environ.pop('COD2_SESSION_SECRET', None)
    try:
        relay = importlib.import_module('relay')
    finally:
        if old_secret is not None:
            os.environ['COD2_SESSION_SECRET'] = old_secret
    return relay, saved


class Echo:
    """UDP server that echoes every datagram, optionally N times."""

    def __init__(self, copies=1):
        self.copies = copies
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(('127.0.0.1', 0))
        self.sock.settimeout(0.2)
        self.port = self.sock.getsockname()[1]
        self.received = []
        self._stop = False
        self.t = threading.Thread(target=self.run, daemon=True)
        self.t.start()

    def run(self):
        while not self._stop:
            try:
                data, addr = self.sock.recvfrom(20000)
            except socket.timeout:
                continue
            except OSError:
                return
            self.received.append(data)
            for i in range(self.copies):
                self.sock.sendto(data + b'#%d' % i, addr)

    def close(self):
        self._stop = True
        self.sock.close()
        self.t.join(timeout=2)


class FakeWS:
    def __init__(self, port, messages, peer='127.0.0.1', xff=None, send_delay=0.0, hold=0.3):
        headers = {}
        if xff:
            headers['X-Forwarded-For'] = xff
        self.request = types.SimpleNamespace(path='/?host=127.0.0.1&port=%d' % port, headers=headers)
        self.remote_address = (peer, 50000)
        self._messages = list(messages)
        self.sent = []
        self.closed = None
        self.send_delay = send_delay
        self.hold = hold

    def __aiter__(self):
        return self

    async def __anext__(self):
        if self._messages:
            await asyncio.sleep(0)
            return self._messages.pop(0)
        await asyncio.sleep(self.hold)   # keep the session open for replies
        raise StopAsyncIteration

    async def send(self, data):
        if self.send_delay:
            await asyncio.sleep(self.send_delay)
        self.sent.append(data)

    async def close(self, code, reason):
        self.closed = (code, reason)


class RelaySessionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.relay, cls.saved = load_relay()
        cls.relay.ALLOW_ANY = True
        cls.relay.SIGNER = None

    @classmethod
    def tearDownClass(cls):
        for k, v in cls.saved.items():
            if v is None:
                sys.modules.pop(k, None)
            else:
                sys.modules[k] = v

    def setUp(self):
        self.echo = Echo()
        self.relay._conns = self.relay.ConnLimiter(4)

    def tearDown(self):
        self.echo.close()

    def run_ws(self, *wss):
        async def go():
            await asyncio.gather(*(self.relay.relay_handler(ws) for ws in wss))
        asyncio.run(go())

    def test_forwards_in_order(self):
        msgs = [b'\xff\xff\xff\xffpacket%03d' % i for i in range(50)]
        ws = FakeWS(self.echo.port, msgs)
        self.run_ws(ws)
        self.assertEqual(self.echo.received, msgs)
        self.assertEqual(ws.sent, [m + b'#0' for m in msgs])
        self.assertEqual(self.relay._conns._active, {}, 'session slot released')

    def test_text_and_oversized_frames_ignored(self):
        ws = FakeWS(self.echo.port, ['text', b'x' * 20000, b'ok'])
        self.run_ws(ws)
        self.assertEqual(self.echo.received, [b'ok'])

    def test_slow_browser_drops_oldest_not_memory(self):
        self.echo.close()
        self.echo = Echo(copies=400)
        old = self.relay.SEND_QUEUE
        self.relay.SEND_QUEUE = 32
        try:
            ws = FakeWS(self.echo.port, [b'burst'], send_delay=0.002, hold=0.8)
            self.run_ws(ws)
        finally:
            self.relay.SEND_QUEUE = old
        nums = [int(x.split(b'#')[1]) for x in ws.sent]
        self.assertLess(len(ws.sent), 400, 'some datagrams dropped')
        self.assertGreater(len(ws.sent), 20)
        self.assertEqual(nums, sorted(nums), 'order preserved')
        # The kernel may also drop part of a 400-datagram burst before the
        # relay sees it, so only require that the tail of the burst (newest
        # data) is what reached the slow browser, not the head.
        self.assertGreater(nums[-1], 200, 'newest datagrams kept, oldest dropped')
        gaps = sum(1 for a, b in zip(nums, nums[1:]) if b - a > 1)
        self.assertGreater(gaps, 0)

    def test_upstream_rate_limit(self):
        old = self.relay.UP_PPS
        self.relay.UP_PPS = 20.0
        try:
            ws = FakeWS(self.echo.port, [b'p%d' % i for i in range(200)])
            self.run_ws(ws)
        finally:
            self.relay.UP_PPS = old
        self.assertLessEqual(len(self.echo.received), 30)
        self.assertGreaterEqual(len(self.echo.received), 20)

    def test_session_limit_per_address(self):
        wss = [FakeWS(self.echo.port, [b'hi'], xff='203.0.113.7', hold=0.5) for _ in range(6)]
        self.run_ws(*wss)
        refused = [w for w in wss if w.closed and w.closed[0] == 4429]
        self.assertEqual(len(refused), 2)
        # A different browser behind the same proxy is not affected.
        other = FakeWS(self.echo.port, [b'hi'], xff='198.51.100.9')
        self.run_ws(other)
        self.assertIsNone(other.closed)

    def test_forwarded_header_only_trusted_from_loopback(self):
        ws = FakeWS(self.echo.port, [], peer='192.0.2.50', xff='1.2.3.4')
        self.assertEqual(self.relay.client_ip(ws, ws.request.headers), '192.0.2.50')
        ws = FakeWS(self.echo.port, [], peer='127.0.0.1', xff='1.2.3.4, 10.0.0.1')
        self.assertEqual(self.relay.client_ip(ws, ws.request.headers), '1.2.3.4')


if __name__ == '__main__':
    unittest.main()
