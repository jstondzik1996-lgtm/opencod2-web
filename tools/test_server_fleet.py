"""Tests for tools/run_server_fleet.py and concurrent Quick Play over HTTP.

The dedicated server is replaced by a tiny Python program that reads the same
command line (+set net_ip/net_port/sv_maxclients/sv_hostname) and answers the
UDP getstatus query, so supervision, the generated directory file, restarts
and concurrent matchmaking are tested end to end without game files.
"""
import http.client
import json
import os
import socket
import sys
import tempfile
import threading
import time
import unittest
from http.server import ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, 'master_proxy'))

import directory  # noqa: E402
import run_server_fleet as fleet  # noqa: E402
import server as master  # noqa: E402

FAKE_SERVER = r'''
import socket, sys
args = sys.argv[1:]
cfg = {}
i = 0
while i < len(args):
    if args[i] == '+set' and i + 2 < len(args):
        cfg[args[i + 1]] = args[i + 2]
        i += 3
    else:
        i += 1
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind((cfg['net_ip'], int(cfg['net_port'])))
while True:
    data, addr = s.recvfrom(2048)
    if data.startswith(b'\xff\xff\xff\xffgetstatus'):
        rules = '\\sv_maxclients\\%s\\mapname\\mp_toujane\\g_gametype\\dm\\sv_hostname\\%s\\lanauth\\%s' % (
            cfg['sv_maxclients'], cfg['sv_hostname'], cfg.get('net_lanauthorize'))
        s.sendto(b'\xff\xff\xff\xffstatusResponse\n' + rules.encode() + b'\n', addr)
'''


def free_base_port(count):
    for _ in range(50):
        socks = []
        try:
            first = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            first.bind(('127.0.0.1', 0))
            base = first.getsockname()[1]
            socks.append(first)
            for k in range(1, count):
                s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                s.bind(('127.0.0.1', base + k))
                socks.append(s)
            return base
        except OSError:
            continue
        finally:
            for s in socks:
                s.close()
    raise RuntimeError('no free port range')


class FleetTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.fake = os.path.join(self.tmp, 'fake_ded.py')
        with open(self.fake, 'w') as fh:
            fh.write(FAKE_SERVER)
        os.makedirs(os.path.join(self.tmp, 'root', 'main'))
        self.out = os.path.join(self.tmp, 'servers.json')
        self.base = free_base_port(2)

    def args(self, *extra):
        return fleet.parse_args([
            '--exe', '%s %s' % (sys.executable, self.fake), '--exe-split',
            '--root', os.path.join(self.tmp, 'root'), '--count', '2',
            '--base-port', str(self.base), '--max-players', '3',
            '--home-dir', os.path.join(self.tmp, 'homes'), '--servers-out', self.out,
            '--name', 'Test', *extra])

    def test_argument_guards(self):
        for bad in (['--bind', '8.8.8.8'], ['--bind', 'host'], ['--count', '0'],
                    ['--max-players', '99'], ['--set', 'a;b=1']):
            with self.assertRaises(SystemExit, msg=bad):
                with open(os.devnull, 'w') as devnull:
                    old = sys.stderr
                    sys.stderr = devnull
                    try:
                        self.args(*bad)
                    finally:
                        sys.stderr = old

    def test_fleet_runs_writes_directory_and_restarts(self):
        args = self.args()
        instances = [fleet.Instance(i, args) for i in range(args.count)]
        fleet.write_directory(args.servers_out, instances)
        with open(self.out) as fh:
            cfg = json.load(fh)
        self.assertEqual([s['port'] for s in cfg['servers']], [self.base, self.base + 1])
        self.assertTrue(all(s['host'] == '127.0.0.1' for s in cfg['servers']))
        self.assertIn('net_lanauthorize', instances[0].command())
        self.assertNotEqual(instances[0].home, instances[1].home, 'separate fs_homepath per instance')

        stop = threading.Event()
        worker = threading.Thread(target=fleet.supervise, args=(instances, args, stop), daemon=True)
        worker.start()
        try:
            d = directory.ServerDirectory(directory.load_config(self.out), response_window=0.5)
            deadline = time.time() + 15
            while time.time() < deadline:
                d.poll_once()
                listed = d.list_servers()
                if all(s['status'] == 'online' for s in listed):
                    break
                time.sleep(0.3)
            self.assertEqual([s['status'] for s in listed], ['online', 'online'])
            self.assertEqual(listed[0]['maxPlayers'], 3)

            first_pid = instances[0].proc.pid
            instances[0].proc.kill()
            deadline = time.time() + 15
            while time.time() < deadline:
                p = instances[0].proc
                if p and p.pid != first_pid and p.poll() is None:
                    break
                time.sleep(0.2)
            self.assertNotEqual(instances[0].proc.pid, first_pid, 'crashed instance restarted')
            self.assertEqual(instances[0].restarts, 1)
            deadline = time.time() + 15
            ok = False
            while time.time() < deadline and not ok:
                time.sleep(0.3)
                d.poll_once()
                ok = d.list_servers()[0]['status'] == 'online'
            self.assertTrue(ok, 'restarted instance answers again')
        finally:
            stop.set()
            worker.join(timeout=15)
        self.assertTrue(all(i.proc.poll() is not None for i in instances), 'stopped on shutdown')


class ConcurrentQuickPlayHttpTests(unittest.TestCase):
    def test_parallel_http_matchmake_never_overbooks(self):
        from test_server_directory import FakeCod2Server, entry
        a = FakeCod2Server(players=1, maxclients=4)
        b = FakeCod2Server(players=0, maxclients=4)
        d = directory.ServerDirectory(directory.parse_config(
            {'servers': [entry('a', a.port), entry('b', b.port)]}), response_window=0.5)
        d.poll_once()
        saved = (master.DIRECTORY, master.SIGNER, master._matchmake_limit)
        master.DIRECTORY, master.SIGNER = d, None
        master._matchmake_limit = master.RateLimiter(1000)
        httpd = ThreadingHTTPServer(('127.0.0.1', 0), master.Handler)
        httpd.daemon_threads = True
        threading.Thread(target=httpd.serve_forever, daemon=True).start()
        results = []
        lock = threading.Lock()

        def player():
            conn = http.client.HTTPConnection('127.0.0.1', httpd.server_port, timeout=10)
            conn.request('POST', '/matchmake', body=b'{}', headers={'Content-Type': 'application/json'})
            resp = conn.getresponse()
            body = json.loads(resp.read())
            conn.close()
            with lock:
                results.append((resp.status, body.get('id')))

        try:
            threads = [threading.Thread(target=player) for _ in range(20)]
            for t in threads:
                t.start()
            for t in threads:
                t.join()
        finally:
            httpd.shutdown()
            httpd.server_close()
            master.DIRECTORY, master.SIGNER, master._matchmake_limit = saved
            a.close()
            b.close()
        got = sorted(i for s, i in results if s == 200)
        self.assertEqual(got, ['a'] * 3 + ['b'] * 4, '7 free slots, fullest server first')
        self.assertEqual(sum(1 for s, _ in results if s == 503), 13)


if __name__ == '__main__':
    unittest.main()
