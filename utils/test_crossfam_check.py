import io
import os
import tempfile
import unittest

from utils import crossfam_check


def write_family(root, family, files):
    os.makedirs(os.path.join(root, family))
    for name, text in files.items():
        with open(os.path.join(root, family, name), 'w') as f:
            f.write(text)


class CrossFamilyFrameSetTest(unittest.TestCase):
    def run_check(self, families, exceptions=frozenset()):
        with tempfile.TemporaryDirectory() as root:
            for family, files in families.items():
                write_family(root, family, files)
            out = io.StringIO()
            violations = crossfam_check.run(root, exceptions=exceptions, out=out)
            return violations, out.getvalue()

    def test_same_frames_pass(self):
        violations, _ = self.run_check(
            {
                'artix7': {
                    'segbits_monitor_bot.db': 'MONITOR_BOT.XADC.INIT_40[0] 28_1696\n'
                },
                'kintex7': {
                    'segbits_monitor_bot.db': 'MONITOR_BOT.XADC.INIT_40[0] 28_1696\n'
                },
            })
        self.assertEqual(violations, 0)

    def test_frame_shift_is_a_violation(self):
        violations, out = self.run_check(
            {
                'artix7': {
                    'segbits_monitor_bot.db': 'MONITOR_BOT.XADC.INIT_40[0] 20_1696\n'
                },
                'kintex7': {
                    'segbits_monitor_bot.db': 'MONITOR_BOT.XADC.INIT_40[0] 28_1696\n'
                },
            })
        self.assertEqual(violations, 1)
        self.assertIn('MONITOR_BOT.XADC.INIT_40[0]', out)

    def test_polarity_and_word_are_ignored(self):
        violations, _ = self.run_check(
            {
                'artix7': {
                    'segbits_cfg_center_mid.db': 'CFG_CENTER_MID.X 26_2199 !27_2200\n'
                },
                'zynq7': {
                    'segbits_cfg_center_mid.db': 'CFG_CENTER_MID.X !26_100 27_2201\n'
                },
            })
        self.assertEqual(violations, 0)

    def test_tag_in_one_family_only_is_not_compared(self):
        violations, _ = self.run_check(
            {
                'artix7': {
                    'segbits_cfg_center_mid.db': 'CFG_CENTER_MID.X 26_2199\n'
                },
                'zynq7': {
                    'segbits_cfg_center_mid.db': 'CFG_CENTER_MID.Y 28_2199\n'
                },
            })
        self.assertEqual(violations, 0)

    def test_origin_files_are_not_compared(self):
        violations, _ = self.run_check(
            {
                'artix7': {
                    'segbits_x.db': 'X.A 1_1\n',
                    'segbits_x.origin_info.db': 'X.A origin:f 1_1\n',
                },
                'zynq7': {
                    'segbits_x.db': 'X.A 1_1\n',
                    'segbits_x.origin_info.db': 'X.A origin:f 9_1\n',
                },
            })
        self.assertEqual(violations, 0)

    def test_exception_excuses_only_its_own_pair(self):
        families = {
            'artix7': {
                'segbits_x.db': 'X.A 1_1\n'
            },
            'kintex7': {
                'segbits_x.db': 'X.A 2_1\n'
            },
            'virtex7': {
                'segbits_x.db': 'X.A 2_1\n'
            },
        }
        excused = {('segbits_x.db', 'X.A', frozenset(('artix7', 'kintex7')))}
        violations, out = self.run_check(families, exceptions=excused)
        self.assertEqual(violations, 1)
        self.assertIn('artix7/virtex7', out)
        self.assertIn('1 excused', out)


if __name__ == '__main__':
    unittest.main()
