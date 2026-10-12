import io
import os
import sys
import tempfile
import unittest
from unittest import mock

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
            violations, _ = crossfam_check.run(
                root, exceptions=exceptions, out=out)
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


class ExitStatusTest(unittest.TestCase):
    def exit_code(self, *args):
        argv = ['crossfam_check.py'] + list(args)
        with mock.patch.object(sys, 'argv', argv), \
                mock.patch.object(sys, 'stdout', io.StringIO()), \
                mock.patch.object(sys, 'stderr', io.StringIO()):
            with self.assertRaises(SystemExit) as cm:
                crossfam_check.main()
        return cm.exception.code

    def run_main(self, families, *args):
        with tempfile.TemporaryDirectory() as root:
            for family, files in families.items():
                write_family(root, family, files)
            return self.exit_code('--db-root', root, *args)

    def test_agreement_exits_0(self):
        self.assertEqual(
            self.run_main(
                {
                    'artix7': {
                        'segbits_x.db': 'X.A 1_1\n'
                    },
                    'kintex7': {
                        'segbits_x.db': 'X.A 1_1\n'
                    },
                }), 0)

    def test_violation_exits_1(self):
        self.assertEqual(
            self.run_main(
                {
                    'artix7': {
                        'segbits_x.db': 'X.A 1_1\n'
                    },
                    'kintex7': {
                        'segbits_x.db': 'X.A 2_1\n'
                    },
                }), 1)

    def test_single_family_exits_2(self):
        self.assertEqual(
            self.run_main({'artix7': {
                'segbits_x.db': 'X.A 1_1\n'
            }}), 2)

    def test_no_shared_tag_exits_2(self):
        self.assertEqual(
            self.run_main(
                {
                    'artix7': {
                        'segbits_x.db': 'X.A 1_1\n'
                    },
                    'kintex7': {
                        'segbits_x.db': 'X.B 1_1\n'
                    },
                }), 2)

    def test_empty_db_root_exits_2(self):
        self.assertEqual(self.run_main({}), 2)

    def test_under_min_pairs_exits_2(self):
        families = {
            'artix7': {
                'segbits_x.db': 'X.A 1_1\nX.B 2_1\n'
            },
            'kintex7': {
                'segbits_x.db': 'X.A 1_1\nX.B 2_1\n'
            },
        }
        self.assertEqual(self.run_main(families, '--min-pairs', '2'), 0)
        self.assertEqual(self.run_main(families, '--min-pairs', '3'), 2)

    def test_missing_db_root_exits_2(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(
                self.exit_code('--db-root', os.path.join(d, 'absent')), 2)

    def test_db_root_that_is_a_file_exits_2(self):
        with tempfile.NamedTemporaryFile() as f:
            self.assertEqual(self.exit_code('--db-root', f.name), 2)

    def test_negative_min_pairs_exits_2(self):
        families = {
            'artix7': {
                'segbits_x.db': 'X.A 1_1\n'
            },
            'kintex7': {
                'segbits_x.db': 'X.A 1_1\n'
            },
        }
        self.assertEqual(self.run_main(families, '--min-pairs', '-1'), 2)

    def test_excused_pairs_do_not_count_toward_min_pairs(self):
        families = {
            'artix7': {
                'segbits_x.db': 'X.A 1_1\nX.B 3_1\n'
            },
            'kintex7': {
                'segbits_x.db': 'X.A 2_1\nX.B 3_1\n'
            },
        }
        with tempfile.TemporaryDirectory() as d:
            fn = os.path.join(d, 'exceptions')
            with open(fn, 'w') as f:
                f.write('segbits_x.db X.A artix7 kintex7 known\n')
            self.assertEqual(
                self.run_main(
                    families, '--exceptions', fn, '--min-pairs', '1'), 0)
            self.assertEqual(
                self.run_main(
                    families, '--exceptions', fn, '--min-pairs', '2'), 2)

    def test_missing_exceptions_file_exits_2(self):
        families = {
            'artix7': {
                'segbits_x.db': 'X.A 1_1\n'
            },
            'kintex7': {
                'segbits_x.db': 'X.A 1_1\n'
            },
        }
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(
                self.run_main(
                    families, '--exceptions', os.path.join(d, 'absent')), 2)

    def test_malformed_exceptions_file_exits_2(self):
        families = {
            'artix7': {
                'segbits_x.db': 'X.A 1_1\n'
            },
            'kintex7': {
                'segbits_x.db': 'X.A 1_1\n'
            },
        }
        with tempfile.TemporaryDirectory() as d:
            fn = os.path.join(d, 'exceptions')
            with open(fn, 'w') as f:
                f.write('segbits_x.db X.A artix7\n')
            self.assertEqual(self.run_main(families, '--exceptions', fn), 2)

    @unittest.skipIf(
        hasattr(os, 'geteuid') and os.geteuid() == 0,
        'root can read a mode-000 file')
    def test_unreadable_segbits_exits_2(self):
        with tempfile.TemporaryDirectory() as root:
            write_family(root, 'artix7', {'segbits_x.db': 'X.A 1_1\n'})
            write_family(root, 'kintex7', {'segbits_x.db': 'X.A 1_1\n'})
            fn = os.path.join(root, 'kintex7', 'segbits_x.db')
            os.chmod(fn, 0)
            try:
                self.assertEqual(self.exit_code('--db-root', root), 2)
            finally:
                os.chmod(fn, 0o644)

    def test_excused_only_population_exits_2(self):
        families = {
            'artix7': {
                'segbits_x.db': 'X.A 1_1\n'
            },
            'kintex7': {
                'segbits_x.db': 'X.A 2_1\n'
            },
        }
        with tempfile.TemporaryDirectory() as d:
            fn = os.path.join(d, 'exceptions')
            with open(fn, 'w') as f:
                f.write('segbits_x.db X.A artix7 kintex7 known\n')
            self.assertEqual(self.run_main(families, '--exceptions', fn), 2)

    def test_violation_under_min_pairs_exits_1(self):
        families = {
            'artix7': {
                'segbits_x.db': 'X.A 1_1\n'
            },
            'kintex7': {
                'segbits_x.db': 'X.A 2_1\n'
            },
        }
        self.assertEqual(self.run_main(families, '--min-pairs', '5'), 1)

if __name__ == '__main__':
    unittest.main()
