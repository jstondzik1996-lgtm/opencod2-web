"""Browser test of the launcher lobby against the real local stack.

Runs the real launcher shell (src/web/shell_nzp.html), lobby.js, cod2_fs.js,
serve_web_local.py, the master proxy with a project directory and fake UDP
CoD2 servers in headless Chromium. Only the WASM engine is replaced by a stub
(cod2.js) that records the callMain arguments and Web_LobbyConnect calls, so
this proves the lobby -> matchmake -> "+connect" path and the full-server
retry, NOT that the game connects. Skips when Playwright/Chromium is missing.
"""
import json
import os
import shutil
import sys
import tempfile
import threading
import unittest
from http.server import ThreadingHTTPServer

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'master_proxy'))

import directory  # noqa: E402
import server as master  # noqa: E402
import sessions  # noqa: E402
import serve_web_local as web  # noqa: E402
from test_server_directory import FakeCod2Server, entry, free_udp_port  # noqa: E402

try:
    from playwright.sync_api import sync_playwright
except ImportError:  # pragma: no cover
    sync_playwright = None

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

ENGINE_STUB = r"""
(function () {
  var files = {};
  window.FS = {
    mkdir: function (p) { files[p] = files[p] || {dir: true}; },
    analyzePath: function (p) { return {exists: !!files[p]}; },
    writeFile: function (p, d) { files[p] = {size: d.length}; }
  };
  window.__engine = {args: null, connects: []};
  Module.callMain = function (args) {
    window.__engine.args = args.slice();
    document.getElementById('canvas').style.display = 'block';
  };
  Module.ccall = function (name, ret, types, args) {
    if (name === 'Web_LobbyConnect') { window.__engine.connects.push(args[0]); return 1; }
    return 0;
  };
  setTimeout(function () { Module.onRuntimeInitialized(); }, 0);
})();
"""


@unittest.skipIf(sync_playwright is None, 'playwright not installed')
class LobbyBrowserTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            with sync_playwright() as p:
                p.chromium.launch().close()
        except Exception as exc:  # pragma: no cover
            raise unittest.SkipTest('chromium unavailable: %s' % exc)

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        webroot = os.path.join(self.tmp, 'web')
        os.makedirs(webroot)
        with open(os.path.join(REPO, 'src', 'web', 'shell_nzp.html'), encoding='utf-8') as fh:
            shell = fh.read().replace('{{{ SCRIPT }}}', '<script src="cod2.js"></script>')
        with open(os.path.join(webroot, 'cod2.html'), 'w', encoding='utf-8') as fh:
            fh.write(shell)
        with open(os.path.join(webroot, 'cod2.js'), 'w', encoding='utf-8') as fh:
            fh.write(ENGINE_STUB)
        for name in ('cod2_fs.js', 'favicon.png', 'apple-touch-icon.png'):
            shutil.copy(os.path.join(REPO, 'src', 'web', name), webroot)
        self.game = os.path.join(self.tmp, 'game')
        os.makedirs(os.path.join(self.game, 'main'))
        with open(os.path.join(self.game, 'main', 'iw_test.iwd'), 'wb') as fh:
            fh.write(b'PK\x05\x06' + b'\0' * 18)

        # A is nearly full (1 slot), B is empty: Quick Play must pick A first.
        self.a = FakeCod2Server(players=3, maxclients=4)
        self.b = FakeCod2Server(players=0, maxclients=4)
        self.dead_port = free_udp_port()
        self.dir = directory.ServerDirectory(directory.parse_config({'servers': [
            entry('a', self.a.port), entry('b', self.b.port), entry('dead', self.dead_port)]}),
            response_window=0.5)
        self.dir.poll_once()
        self.old_dir = master.DIRECTORY
        self.old_signer = master.SIGNER
        master.DIRECTORY = self.dir
        master.SIGNER = None
        self.master = ThreadingHTTPServer(('127.0.0.1', 0), master.Handler)
        self.master.daemon_threads = True
        threading.Thread(target=self.master.serve_forever, daemon=True).start()
        self.old_cfg = web.CFG.copy()
        web.CFG.update(webroot=webroot, coop=False, master_port=self.master.server_port, relay_port=1)
        self.front = ThreadingHTTPServer(('127.0.0.1', 0), web.Handler)
        self.front.daemon_threads = True
        threading.Thread(target=self.front.serve_forever, daemon=True).start()
        self.url = 'http://127.0.0.1:%d/cod2.html' % self.front.server_port

    def tearDown(self):
        for s in (self.front, self.master):
            s.shutdown()
            s.server_close()
        master.DIRECTORY = self.old_dir
        master.SIGNER = self.old_signer
        web.CFG.clear()
        web.CFG.update(self.old_cfg)
        self.a.close()
        self.b.close()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_quick_play_boots_with_connect_and_retries_full_server(self):
        self._quick_play_flow(sessions_on=False)

    def test_quick_play_with_player_sessions(self):
        master.SIGNER = sessions.Signer('browser-test-secret-0123456789')
        self._quick_play_flow(sessions_on=True)

    def test_nick_required(self):
        with sync_playwright() as p:
            browser = p.chromium.launch()
            page = browser.new_page()
            page.goto(self.url)
            page.wait_for_selector('#lobby .lobby-row', timeout=20000)
            page.fill('#lobbyNick', '^1')
            page.click('#lobbyQuick')
            page.wait_for_function('document.querySelector("#lobby .lobby-status").textContent.length > 0')
            self.assertIn('nick', page.inner_text('#lobby .lobby-status').lower())
            self.assertEqual(self.dir.list_servers()[0]['reserved'], 0, 'no reservation without a nick')
            browser.close()

    def _quick_play_flow(self, sessions_on):
        with sync_playwright() as p:
            browser = p.chromium.launch()
            page = browser.new_page()
            errors = []
            page.on('pageerror', lambda e: errors.append(str(e)))
            page.goto(self.url)
            page.wait_for_function('!document.getElementById("beginBtn").disabled', timeout=20000)
            page.wait_for_selector('#lobby .lobby-row', timeout=10000)

            self.assertEqual(page.title(), 'WWII Browser Multiplayer')
            self.assertIn('nie jest powiązany z Activision', page.inner_text('#fanNotice'))
            rows = page.locator('#lobby .lobby-row')
            self.assertEqual(rows.count(), 3)
            self.assertIn('3/4', rows.nth(0).inner_text())
            self.assertIn('offline', rows.nth(2).inner_text())
            self.assertTrue(page.locator('#lobby .lobby-row').nth(2).locator('button').is_disabled())
            if os.environ.get('COD2_LOBBY_SCREENSHOT'):
                page.set_viewport_size({'width': 1280, 'height': 900})
                page.screenshot(path=os.environ['COD2_LOBBY_SCREENSHOT'])

            page.fill('#lobbyNick', '^2Sgt ^7Kowal ski')
            # Use the no-File-System-Access fallback so the test can supply a folder.
            page.evaluate('Cod2Fs.hasFsAccess = function () { return false; }')
            with page.expect_file_chooser() as fc:
                page.click('#lobbyQuick')
            fc.value.set_files(self.game)
            page.wait_for_function('window.__engine.args !== null', timeout=10000)
            args = page.evaluate('window.__engine.args')
            a_addr = '127.0.0.1:%d' % self.a.port
            b_addr = '127.0.0.1:%d' % self.b.port
            # Regression: project-owned LAN-mode connection must be explicit
            # BEFORE +connect, or a clean browser tries CD-key authorization.
            # No game key is ever injected into command-line arguments.
            self.assertEqual(args[-8:], ['+set', 'net_lanauthorize', '0',
                                         '+set', 'name', 'Sgt_Kowal_ski',
                                         '+connect', a_addr])
            self.assertNotIn('codkey', ' '.join(args).lower())
            self.assertEqual(self.dir.list_servers()[0]['reserved'], 1)
            tickets = page.evaluate('window.COD2_RELAY_TICKETS || {}')
            relay_session = page.evaluate('window.COD2_RELAY_SESSION || ""')
            if sessions_on:
                t = master.SIGNER.verify(tickets[a_addr], 'ticket')
                self.assertEqual((t['srv'], t['nick']), (a_addr, 'Sgt_Kowal_ski'))
                self.assertEqual(master.SIGNER.verify(relay_session, 'session')['pid'], t['pid'])
                key = page.evaluate('localStorage.getItem("cod2web.playerKey")')
                self.assertEqual(t['pid'], sessions.player_id(key), 'persistent guest identity')
            else:
                self.assertEqual((tickets, relay_session), ({}, ''))
            self.assertEqual(page.evaluate('localStorage.getItem("cod2web.nick")'), 'Sgt_Kowal_ski')

            # Server A refuses as full: the lobby must exclude it and go to B
            # through the narrow engine export, without restarting the engine.
            page.evaluate('window.cod2LobbyOnLine("EXE_SERVERISFULL")')
            page.wait_for_function('window.__engine.connects.length === 1', timeout=5000)
            self.assertEqual(page.evaluate('window.__engine.connects'), [b_addr])
            if sessions_on:
                tickets = page.evaluate('window.COD2_RELAY_TICKETS')
                self.assertEqual(master.SIGNER.verify(tickets[b_addr], 'ticket')['srv'], b_addr)
            reserved = {s['id']: s['reserved'] for s in self.dir.list_servers()}
            self.assertEqual(reserved, {'a': 0, 'b': 1, 'dead': 0}, 'A reservation released')
            self.assertIn('Łączenie', page.inner_text('#lobby .lobby-status'))
            self.assertEqual(errors, [])
            browser.close()

    def test_lobby_hidden_without_project_directory(self):
        master.DIRECTORY = None
        with sync_playwright() as p:
            browser = p.chromium.launch()
            page = browser.new_page()
            page.route('**/play/api/masterlist/**', lambda r: r.fulfill(status=200, body='{"servers":[]}'))
            page.goto(self.url)
            page.wait_for_function('!document.getElementById("beginBtn").disabled', timeout=20000)
            page.wait_for_timeout(800)
            self.assertFalse(page.is_visible('#lobby'))
            browser.close()


if __name__ == '__main__':
    unittest.main()
