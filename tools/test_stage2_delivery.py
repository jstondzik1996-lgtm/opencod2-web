"""Stage 2 delivery: private asset access, versioned asset cache, overlay packs.

* serve_web_local --access-key: game archives / manifest need a cookie that a
  tester gets with ?access=<key>; the launcher page itself stays public.
* gen_assets_manifest adds a per-file version; the real cod2_fs.js (headless
  Chromium, OPFS) re-downloads an archive whose version changed even when the
  size is the same, and skips unchanged ones.
* build_overlay_pack builds a zz_ IWD and reports which base archive each
  replaced path came from.
Uses tiny synthetic archives only.
"""
import http.client
import json
import os
import shutil
import sys
import tempfile
import threading
import time
import unittest
import zipfile
from http.server import ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import build_overlay_pack as overlay  # noqa: E402
import gen_assets_manifest as manifest  # noqa: E402
import serve_web_local as web  # noqa: E402

try:
    from playwright.sync_api import sync_playwright
except ImportError:  # pragma: no cover
    sync_playwright = None


def make_iwd(path, files):
    with zipfile.ZipFile(path, 'w') as z:
        for name, data in files.items():
            z.writestr(name, data)


class Front:
    def __init__(self, webroot, access_key=None):
        self.old = web.CFG.copy()
        web.CFG.update(webroot=webroot, coop=False, master_port=1, relay_port=1, access_key=access_key)
        self.httpd = ThreadingHTTPServer(('127.0.0.1', 0), web.Handler)
        self.httpd.daemon_threads = True
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()
        self.port = self.httpd.server_port

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()
        web.CFG.clear()
        web.CFG.update(self.old)

    def req(self, method, path, cookie=None):
        conn = http.client.HTTPConnection('127.0.0.1', self.port, timeout=5)
        conn.request(method, path, headers={'Cookie': cookie} if cookie else {})
        r = conn.getresponse()
        body = r.read()
        headers = dict(r.getheaders())
        conn.close()
        return r.status, headers, body


class AccessKeyTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        os.makedirs(os.path.join(self.tmp, 'main'))
        make_iwd(os.path.join(self.tmp, 'main', 'iw_00.iwd'), {'a.txt': 'x'})
        with open(os.path.join(self.tmp, 'cod2.html'), 'w') as fh:
            fh.write('<html></html>')
        with open(os.path.join(self.tmp, 'assets.json'), 'w') as fh:
            fh.write('{"files":[]}')
        self.front = Front(self.tmp, access_key='tester-key-123456')

    def tearDown(self):
        self.front.close()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_assets_need_cookie(self):
        self.assertEqual(self.front.req('GET', '/cod2.html')[0], 200, 'launcher stays public')
        for path in ('/assets.json', '/main/iw_00.iwd', '/MAIN/iw_00.iwd', '/main%2fiw_00.iwd'):
            self.assertEqual(self.front.req('GET', path)[0], 403, path)
        self.assertEqual(self.front.req('HEAD', '/assets.json')[0], 403)
        self.assertEqual(self.front.req('GET', '/cod2.html?access=wrong-key')[0], 403)
        status, headers, _ = self.front.req('GET', '/cod2.html?access=tester-key-123456&x=1')
        self.assertEqual(status, 302)
        self.assertEqual(headers['Location'], '/cod2.html?x=1', 'key removed from the URL')
        cookie = headers['Set-Cookie'].split(';')[0]
        self.assertIn('HttpOnly', headers['Set-Cookie'])
        self.assertNotIn('tester-key', cookie, 'cookie holds a derived token, not the key')
        self.assertEqual(self.front.req('GET', '/main/iw_00.iwd', cookie=cookie)[0], 200)
        self.assertEqual(self.front.req('GET', '/assets.json', cookie=cookie)[0], 200)
        self.assertEqual(self.front.req('GET', '/main/iw_00.iwd', cookie='cod2_access=forged')[0], 403)

    def test_no_key_configured_keeps_old_behaviour(self):
        self.front.close()
        self.front = Front(self.tmp, access_key=None)
        self.assertEqual(self.front.req('GET', '/main/iw_00.iwd')[0], 200)


class OverlayTests(unittest.TestCase):
    def test_overlay_overrides_and_report(self):
        tmp = tempfile.mkdtemp()
        try:
            base = os.path.join(tmp, 'main')
            os.makedirs(base)
            make_iwd(os.path.join(base, 'iw_00.iwd'), {'sound/a.wav': 'old', 'images/b.iwi': 'old'})
            make_iwd(os.path.join(base, 'iw_01.iwd'), {'images/b.iwi': 'newer'})
            src = os.path.join(tmp, 'repl')
            os.makedirs(os.path.join(src, 'images'))
            os.makedirs(os.path.join(src, 'sound'))
            for rel, data in (('images/b.iwi', 'licensed'), ('sound/a.wav', 'licensed'),
                              ('sound/typo.wav', 'x')):
                with open(os.path.join(src, rel), 'w') as fh:
                    fh.write(data)
            out = os.path.join(base, 'zz_overlay_test.iwd')
            report = overlay.build(src, out, base)
            self.assertEqual(report['overrides'], {'images/b.iwi': 'iw_01.iwd', 'sound/a.wav': 'iw_00.iwd'})
            self.assertEqual(report['new'], ['sound/typo.wav'])
            with zipfile.ZipFile(out) as z:
                self.assertEqual(z.read('images/b.iwi'), b'licensed')
            # The overlay sorts after the base archives, so it has priority.
            self.assertEqual(sorted(os.listdir(base))[-1], 'zz_overlay_test.iwd')
            with self.assertRaises(SystemExit):
                overlay.main([src, '--name', 'Bad Name'])
            with self.assertRaises(SystemExit):
                overlay.main([src, '--name', 'ok', '-o', os.path.join(tmp, 'aa.iwd')])
        finally:
            shutil.rmtree(tmp, ignore_errors=True)


@unittest.skipIf(sync_playwright is None, 'playwright not installed')
class VersionedCacheBrowserTests(unittest.TestCase):
    def test_replaced_archive_with_same_size_is_downloaded_again(self):
        try:
            with sync_playwright() as p:
                p.chromium.launch().close()
        except Exception as exc:  # pragma: no cover
            self.skipTest('chromium unavailable: %s' % exc)
        tmp = tempfile.mkdtemp()
        front = None
        try:
            os.makedirs(os.path.join(tmp, 'main'))
            iwd_a = os.path.join(tmp, 'main', 'iw_00.iwd')
            iwd_b = os.path.join(tmp, 'main', 'iw_01.iwd')
            make_iwd(iwd_a, {'x.txt': 'AAAA'})
            make_iwd(iwd_b, {'y.txt': 'BBBB'})
            shutil.copy(os.path.join(REPO, 'src', 'web', 'cod2_fs.js'), tmp)
            with open(os.path.join(tmp, 'cod2.html'), 'w') as fh:
                fh.write('<html><script src="cod2_fs.js"></script></html>')

            def write_manifest():
                doc = {'baseUrl': '/', 'files': manifest.scan(tmp)}
                with open(os.path.join(tmp, 'assets.json'), 'w') as fh:
                    json.dump(doc, fh)
                return doc

            doc = write_manifest()
            self.assertTrue(all('version' in f for f in doc['files']))
            front = Front(tmp)
            url = 'http://127.0.0.1:%d/cod2.html' % front.port
            with sync_playwright() as p:
                browser = p.chromium.launch()
                page = browser.new_page()
                downloads = []
                page.on('request', lambda r: downloads.append(r.url.rsplit('/', 1)[-1])
                        if r.url.endswith('.iwd') else None)
                page.goto(url)
                ingest = ('async () => { await Cod2Fs.ingestRemote({manifestUrl: "assets.json", baseUrl: "/"}); '
                          'const f = await (await (await (await navigator.storage.getDirectory())'
                          '.getDirectoryHandle("cod2")).getDirectoryHandle("main")).getFileHandle("iw_00.iwd");'
                          'return await (await f.getFile()).text(); }')
                page.evaluate(ingest)
                self.assertEqual(sorted(downloads), ['iw_00.iwd', 'iw_01.iwd'])

                downloads.clear()
                page.evaluate(ingest)
                self.assertEqual(downloads, [], 'unchanged archives come from the cache')

                # Same size, different content and mtime -> new version.
                size = os.path.getsize(iwd_a)
                make_iwd(iwd_a, {'x.txt': 'ZZZZ'})
                self.assertEqual(os.path.getsize(iwd_a), size)
                os.utime(iwd_a, (time.time() + 100, time.time() + 100))
                write_manifest()
                downloads.clear()
                text = page.evaluate(ingest)
                self.assertEqual(downloads, ['iw_00.iwd'], 'only the replaced archive is fetched')
                self.assertIn('ZZZZ', text)
                browser.close()
        finally:
            if front:
                front.close()
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == '__main__':
    unittest.main()
