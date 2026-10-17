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

from io import StringIO
import os
import unittest
from unittest import mock

from prjxray import cross_tile_features
import utils.tile_window_census as census

TEST_DATA = os.path.join(os.path.dirname(__file__), 'test_data')
ARTIX7 = os.path.join(TEST_DATA, 'db')
KINTEX7 = os.path.join(TEST_DATA, 'db_kintex7')

FIXTURE_ALLOWED = {
    ('INT_R', 'IOB_COL_BANK_ACTIVE'): ('RIOB18', 'RIOB18_SING'),
    ('INT_R', 'IOB_COL_OBUF_CASCADE_Y1'): ('RIOB18', 'RIOB18_SING'),
}
FIXTURE_REFUSED = {
    ('INT_R', 'IOB_COL_OBUF_CASCADE_Y1'): ('CLBLM_R', ),
}


class TestTileWindowCensus(unittest.TestCase):
    def run_census(self, roots, complete=False):
        out = StringIO()
        rc = census.run(roots, complete, out)
        return rc, out.getvalue()

    def test_shipped_table_classifies_every_crossing(self):
        rc, out = self.run_census([ARTIX7, KINTEX7])
        self.assertEqual(rc, 0, out)
        self.assertIn(
            'cross db_kintex7 xc7k325t INT_R.IOB_COL_BANK_ACTIVE -> RIOB18: '
            '3 bit(s) allowed', out)
        self.assertIn(
            'cross db_kintex7 xc7k325t INT_R.IOB_COL_OBUF_CASCADE_Y1 -> '
            'CLBLM_R: 3 bit(s) refused', out)
        self.assertIn('0 unclassified cross-tile row(s)', out)

    def test_undeclared_crossing_is_red(self):
        with mock.patch.dict(cross_tile_features.ALLOWED, clear=True):
            rc, out = self.run_census([KINTEX7])
        self.assertEqual(rc, 1, out)
        self.assertIn(
            'INT_R.IOB_COL_BANK_ACTIVE -> RIOB18: 3 bit(s) UNCLASSIFIED', out)

    def test_undeclared_refusal_is_red(self):
        with mock.patch.dict(cross_tile_features.REFUSED, clear=True):
            rc, out = self.run_census([KINTEX7])
        self.assertEqual(rc, 1, out)
        self.assertIn(
            'INT_R.IOB_COL_OBUF_CASCADE_Y1 -> CLBLM_R: 3 bit(s) UNCLASSIFIED',
            out)

    def test_complete_census_with_exercised_rows_is_green(self):
        with mock.patch.object(cross_tile_features, 'ALLOWED',
                               FIXTURE_ALLOWED), mock.patch.object(
                                   cross_tile_features, 'REFUSED',
                                   FIXTURE_REFUSED):
            rc, out = self.run_census([ARTIX7, KINTEX7], complete=True)
        self.assertEqual(rc, 0, out)
        self.assertIn('0 stale declaration(s)', out)

    def test_complete_census_names_a_stale_row(self):
        refused = dict(FIXTURE_REFUSED)
        refused[('INT_R', 'IOB_COL_BANK_ACTIVE')] = ('CLBLL_R', )
        with mock.patch.object(cross_tile_features, 'ALLOWED',
                               FIXTURE_ALLOWED), mock.patch.object(
                                   cross_tile_features, 'REFUSED', refused):
            rc, out = self.run_census([ARTIX7, KINTEX7], complete=True)
        self.assertEqual(rc, 1, out)
        self.assertIn(
            'STALE refused row INT_R.IOB_COL_BANK_ACTIVE -> CLBLL_R', out)

    def test_no_fabric_is_not_a_pass(self):
        rc, out = self.run_census([TEST_DATA])
        self.assertEqual(rc, 2, out)


if __name__ == '__main__':
    unittest.main()
