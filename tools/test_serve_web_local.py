import http.client
from http.server import ThreadingHTTPServer
from pathlib import Path
import tempfile
import threading
import unittest

import serve_web_local as server


class KeepAliveHeadTests(unittest.TestCase):
    def test_head_probe_does_not_suppress_next_get_body(self):
        with tempfile.TemporaryDirectory() as temp:
            payload = b'{"files":[{"path":"main/iw_00.iwd","size":123}]}'
            (Path(temp) / 'assets.json').write_bytes(payload)
            old_cfg = server.CFG.copy()
            server.CFG.update(webroot=temp, coop=False)
            httpd = ThreadingHTTPServer(('127.0.0.1', 0), server.Handler)
            httpd.daemon_threads = True
            worker = threading.Thread(target=httpd.serve_forever, daemon=True)
            worker.start()
            conn = http.client.HTTPConnection('127.0.0.1', httpd.server_port, timeout=2)
            try:
                conn.request('HEAD', '/assets.json')
                head = conn.getresponse()
                self.assertEqual(head.status, 200)
                self.assertEqual(int(head.getheader('Content-Length')), len(payload))
                self.assertEqual(head.read(), b'')
                first_socket = conn.sock
                conn.request('GET', '/assets.json')
                get = conn.getresponse()
                self.assertEqual(get.status, 200)
                self.assertIs(conn.sock, first_socket)
                self.assertEqual(get.read(), payload)
            finally:
                conn.close()
                httpd.shutdown()
                httpd.server_close()
                worker.join(timeout=3)
                server.CFG.clear()
                server.CFG.update(old_cfg)


if __name__ == '__main__':
    unittest.main()
