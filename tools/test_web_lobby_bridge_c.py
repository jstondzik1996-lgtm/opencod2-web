"""Compile the real WASM lobby bridge with a tiny host stub (no game assets).

A clean browser may reconnect through Web_LobbyConnect after its original
boot-time +set flags have been consumed. Ensure the bridge applies our
project-local LAN setting before connect and keeps command injection blocked.
"""
import pathlib
import shutil
import subprocess
import tempfile
import unittest


REPO = pathlib.Path(__file__).resolve().parents[1]
SOURCE = REPO / 'src' / 'web' / 'web_lobby.c'
BRIDGE_TEST_C = r'''
#include <stdio.h>
#include <string.h>

extern int Web_LobbyConnect(const char *address);
static char commands[256];

void Cbuf_AddText(const char *text) {
    size_t avail = sizeof(commands) - strlen(commands) - 1;
    strncat(commands, text, avail);
}

static int check_rejected(const char *s) {
    char old[256];
    memcpy(old, commands, sizeof(old));
    return !Web_LobbyConnect(s) && !strcmp(old, commands);
}

int main(void) {
    if (!Web_LobbyConnect("127.0.0.1:28963")) return 1;
    if (strcmp(commands, "set net_lanauthorize 0\\nconnect 127.0.0.1:28963\\n")) {
        fprintf(stderr, "unexpected lobby command: %s\\n", commands);
        return 2;
    }
    if (!check_rejected("127.0.0.1:28963\\nquit")) return 3;
    if (!check_rejected("127.0.0.1:0")) return 4;
    if (!check_rejected("127.0.0.1:65536")) return 5;
    if (!check_rejected("127.0.0.1:28963;quit")) return 6;
    if (!check_rejected("127.0.0.1")) return 7;
    if (!check_rejected("127.0.0.1:")) return 8;
    if (!check_rejected("999.0.0.1:28963")) return 9;
    if (!check_rejected("127.0.0.1:28963 extra")) return 10;
    return 0;
}
'''


class WebLobbyBridgeCTest(unittest.TestCase):
    def test_real_bridge_sets_project_lan_flag_before_connect(self):
        cc = shutil.which('cc') or shutil.which('gcc') or shutil.which('clang')
        if not cc:
            self.skipTest('No host C compiler; GitHub Ubuntu CI has one')
        with tempfile.TemporaryDirectory(prefix='cod2-web-lobby-c-') as root:
            root = pathlib.Path(root)
            (root / 'emscripten.h').write_text('#define EMSCRIPTEN_KEEPALIVE\n')
            (root / 'test.c').write_text(BRIDGE_TEST_C)
            exe = root / 'web_lobby_test'
            build = subprocess.run(
                [cc, '-std=c99', '-Wall', '-Wextra', '-Werror',
                 '-I', str(root), str(SOURCE), str(root / 'test.c'),
                 '-o', str(exe)],
                capture_output=True, text=True, timeout=30,
            )
            self.assertEqual(build.returncode, 0, build.stderr)
            check = subprocess.run(
                [str(exe)], capture_output=True, text=True, timeout=10
            )
            self.assertEqual(check.returncode, 0, check.stderr)


if __name__ == '__main__':
    unittest.main()
