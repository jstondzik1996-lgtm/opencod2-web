"""Static safety/consistency checks for the optional CoD2 1.3 LAN QA server.cfg.

No original assets or running dedicated server required. This test verifies the
configuration text and registered native dvars; it does NOT prove real gameplay.
"""
import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
CFG = ROOT / "docs" / "server_configs" / "cod2_13_browser_baseline.cfg"

CVAR_RE = re.compile(r'^set\s+([A-Za-z_][A-Za-z_0-9]*)\s+"([^"]*)"\s*(?://.*)?$', re.I)


def get_settings():
    entries = []
    for number, line in enumerate(CFG.read_text(encoding="utf-8").splitlines(), 1):
        stripped = line.strip()
        if not stripped or stripped.startswith("//"):
            continue
        match = CVAR_RE.fullmatch(stripped)
        if match is None:
            raise AssertionError(f"Unsafe/invalid cfg syntax at line {number}: {stripped!r}")
        entries.append((match.group(1), match.group(2)))
    seen = set()
    for key, _ in entries:
        lower = key.lower()
        if lower in seen:
            raise AssertionError(f"Duplicate cfg dvar: {key}")
        seen.add(lower)
    return dict(entries)


class CoD2ServerCfgTest(unittest.TestCase):
    def test_parses_as_only_quoted_set_commands(self):
        self.assertGreaterEqual(len(get_settings()), 15)

    def test_private_loopback_baseline_not_internet(self):
        c = get_settings()
        self.assertEqual(c["dedicated"], "1")
        self.assertEqual(c["sv_maxclients"], "4")
        self.assertEqual(c["g_password"], "")
        self.assertEqual(c["rcon_password"], "")
        self.assertEqual(c["sv_allowDownload"], "0")
        self.assertEqual(c["sv_pure"], "0")
        for key in c:
            self.assertNotIn(key.lower(), {"sv_master1", "sv_master2", "sv_wwwdownload", "sv_wwwbaseurl", "net_ip", "net_port"})

    def test_original_low_risk_network_defaults(self):
        c = get_settings()
        self.assertEqual(c["sv_fps"], "20")
        self.assertEqual(c["sv_maxRate"], "25000")
        self.assertEqual(c["sv_floodProtect"], "1")
        self.assertEqual(c["g_antilag"], "1")
        self.assertEqual(c["g_logSync"], "0")
        self.assertEqual(c["g_gametype"], "dm")
        self.assertEqual(c["sv_mapRotation"], "gametype dm map mp_toujane")

    def test_critical_dvars_are_registered_in_original_source(self):
        source = (
            (ROOT / "src/PC/server_mp/sv_init_mp.c").read_text(encoding="utf-8")
            + (ROOT / "src/PC/game_mp/g_main_mp.c").read_text(encoding="utf-8")
        )
        for key in (
            "sv_maxclients", "sv_fps", "sv_maxRate", "sv_floodProtect",
            "sv_timeout", "sv_reconnectlimit", "sv_pure", "sv_allowDownload",
            "rcon_password", "sv_mapRotation", "g_gametype", "g_logSync",
            "g_antilag",
        ):
            with self.subTest(dvar=key):
                self.assertRegex(source, r'Dvar_Register(?:Int|Bool|String|String_mac|Bool_mac)\s*\(\s*"' + re.escape(key) + r'"')


if __name__ == "__main__":
    unittest.main()
