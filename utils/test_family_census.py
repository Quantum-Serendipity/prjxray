#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# Copyright (C) 2026  The Project X-Ray Authors.
#
# Use of this source code is governed by a ISC-style
# license that can be found in the LICENSE file or at
# https://opensource.org/licenses/ISC
#
# SPDX-License-Identifier: ISC

import contextlib
import io
import os
import tempfile
import unittest

from utils import family_census

X0 = 'CFG_CENTER_MID.ICAP_X0Y0.ENABLE_TR 26_2205 27_2206 27_2207\n'
X1 = 'CFG_CENTER_MID.ICAP_X0Y1.ENABLE_TR 26_2206 26_2207 27_2205\n'
ALWAYS = (
    'CFG_CENTER_MID.ALWAYS_ON_PROP1 26_2206\n'
    'CFG_CENTER_MID.ALWAYS_ON_PROP2 26_2207\n'
    'CFG_CENTER_MID.ALWAYS_ON_PROP3 27_2205\n')
WIDTH = 'CFG_CENTER_MID.ICAP.ICAP_WIDTH_X8 !26_2199 26_2203 26_2204\n'
REQUIRE = [
    '--require', 'CFG_CENTER_MID.ICAP_X0Y0.ENABLE_TR', '--require',
    'CFG_CENTER_MID.ICAP_X0Y1.ENABLE_TR'
]


class TestFamilyCensus(unittest.TestCase):
    def census(self, families, *args):
        with tempfile.TemporaryDirectory() as root:
            for family, files in families.items():
                for name, text in files.items():
                    path = os.path.join(root, family, name)
                    os.makedirs(os.path.dirname(path), exist_ok=True)
                    with open(path, 'w') as f:
                        f.write(text)
            out = io.StringIO()
            with contextlib.redirect_stdout(out), \
                    contextlib.redirect_stderr(io.StringIO()):
                rc = family_census.main(['--db-root', root] + list(args))
            return rc, out.getvalue()

    def test_every_family_carrying_both_sites_passes(self):
        rc, out = self.census(
            {
                'artix7': {
                    'segbits_cfg_center_mid.db': WIDTH + X0 + X1
                },
                'kintex7': {
                    'segbits_cfg_center_mid.db': WIDTH + X0 + X1
                },
            }, *REQUIRE)
        self.assertEqual(rc, 0, out)
        self.assertIn('2 family(ies), 4 check(s), 0 problem(s)', out)

    def test_a_family_without_the_rows_fails(self):
        rc, out = self.census(
            {
                'artix7': {
                    'segbits_cfg_center_mid.db': WIDTH + X0 + X1
                },
                'kintex7': {
                    'segbits_cfg_center_mid.db': WIDTH
                },
            }, *REQUIRE)
        self.assertEqual(rc, 1, out)
        self.assertIn(
            'kintex7: CFG_CENTER_MID.ICAP_X0Y0.ENABLE_TR is absent', out)

    def test_two_names_for_one_bit_set_fail(self):
        rc, out = self.census(
            {
                'zynq7': {
                    'segbits_cfg_center_mid.db': ALWAYS + WIDTH + X0 + X1
                },
            }, *REQUIRE)
        self.assertEqual(rc, 1, out)
        self.assertIn(
            'CFG_CENTER_MID.ALWAYS_ON_PROP1 claims 26_2206, which '
            'CFG_CENTER_MID.ICAP_X0Y1.ENABLE_TR also sets', out)

    def test_required_features_must_resolve(self):
        rc, out = self.census(
            {
                'zynq7': {
                    'segbits_cfg_center_mid.db':
                    WIDTH + X0 + X1,
                    'xc7z010clg400-1/required_features.fasm':
                    'CFG_CENTER_MID_X67Y32.ALWAYS_ON_PROP1\n',
                },
            })
        self.assertEqual(rc, 1, out)
        self.assertIn('ALWAYS_ON_PROP1 is absent', out)
        rc, out = self.census(
            {
                'zynq7': {
                    'segbits_cfg_center_mid.db':
                    WIDTH + X0 + X1,
                    'xc7z010clg400-1/required_features.fasm':
                    'CFG_CENTER_MID_X67Y32.ICAP_X0Y1.ENABLE_TR\n',
                },
            })
        self.assertEqual(rc, 0, out)

    def test_nothing_to_check_is_no_verdict(self):
        rc, _ = self.census(
            {'artix7': {
                'segbits_cfg_center_mid.db': WIDTH
            }})
        self.assertEqual(rc, 2)

    def test_empty_root_is_no_verdict(self):
        rc, _ = self.census({}, *REQUIRE)
        self.assertEqual(rc, 2)


if __name__ == '__main__':
    unittest.main()
