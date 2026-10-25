import tempfile
import unittest
from pathlib import Path
import zipfile

from build_web_mp_pack import iter_kept_members, should_keep


class PackPrecedenceTests(unittest.TestCase):
    def test_patch_and_base_precedence_match_engine(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            sources = []
            for name, entries in [
                ('iw_07.iwd', {'maps/mp/gametypes/_spawnlogic.gsc': b'old', 'images/shared.iwi': b'base'}),
                ('iw_15.iwd', {'maps/mp/gametypes/_spawnlogic.gsc': b'1.3'}),
                ('localized_english_iw00.iwd', {'images/shared.iwi': b'localized', 'localizedstrings/mp.str': b'old strings'}),
                ('localized_english_iw11.iwd', {'localizedstrings/mp.str': b'new strings'}),
            ]:
                path = root / name
                with zipfile.ZipFile(path, 'w') as archive:
                    for member, data in entries.items():
                        archive.writestr(member, data)
                sources.append(str(path))
            expected = {'maps/mp/gametypes/_spawnlogic.gsc': b'1.3', 'images/shared.iwi': b'base', 'localizedstrings/mp.str': b'new strings'}
            self.assertEqual(dict(iter_kept_members(sources)), expected)
            self.assertEqual(dict(iter_kept_members(reversed(sources))), expected)

    def test_mp_map_and_shared_skybox_survive_campaign_filter(self):
        self.assertTrue(should_keep('maps/mp/mp_toujane.d3dbsp'))
        self.assertTrue(should_keep('images/rhine_ft.iwi'))
        self.assertTrue(should_keep('materials/toujane_brick2'))
        self.assertTrue(should_keep('xmodel/toujane_door_narrow05'))
        self.assertTrue(should_keep('soundaliases/mp_toujane.csv'))
        self.assertFalse(should_keep('maps/toujane.d3dbsp'))
        self.assertFalse(should_keep('sound/voiceovers/us/mission.wav'))


if __name__ == '__main__':
    unittest.main()
