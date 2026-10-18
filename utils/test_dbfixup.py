#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# Copyright (C) 2017-2020  The Project X-Ray Authors.
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

from utils import dbfixup

GROUPS = '''\
IOI3.ILOGIC_Y0.IFF.SRTYPE.ASYNC IOI3.ILOGIC_Y0.IFF.SRTYPE.SYNC

IOI3.ILOGIC_Y0.IFF.DDR_CLK_EDGE.OPPOSITE_EDGE \
IOI3.ILOGIC_Y0.IFF.DDR_CLK_EDGE.SAME_EDGE \
IOI3.ILOGIC_Y0.IFF.DDR_CLK_EDGE.SAME_EDGE_PIPELINED
'''

SEGMATCH = '''\
IOI3.ILOGIC_Y0.IFF.SRTYPE.SYNC 29_67
IOI3.ILOGIC_Y0.IFF.SRTYPE.ASYNC <0 candidates>
IOI3.ILOGIC_Y0.IFF.DDR_CLK_EDGE.OPPOSITE_EDGE 26_99
IOI3.ILOGIC_Y0.IFF.DDR_CLK_EDGE.SAME_EDGE 27_98
IOI3.ILOGIC_Y0.IFF.DDR_CLK_EDGE.SAME_EDGE_PIPELINED <0 candidates>
'''

WANT = [
    'IOI3.ILOGIC_Y0.IFF.DDR_CLK_EDGE.OPPOSITE_EDGE !27_98 26_99',
    'IOI3.ILOGIC_Y0.IFF.DDR_CLK_EDGE.SAME_EDGE !26_99 27_98',
    'IOI3.ILOGIC_Y0.IFF.DDR_CLK_EDGE.SAME_EDGE_PIPELINED !26_99 !27_98',
    'IOI3.ILOGIC_Y0.IFF.SRTYPE.ASYNC !29_67',
    'IOI3.ILOGIC_Y0.IFF.SRTYPE.SYNC 29_67',
]


class TestGroupedZeroMember(unittest.TestCase):
    def run_dbfixup(self, segmatch, groups, strict, unresolved=None):
        with tempfile.TemporaryDirectory() as d:
            paths = {}
            for name, text in (('in.rdb', segmatch), ('bits.dbf', ''),
                               ('tag_groups.txt', groups)):
                paths[name] = os.path.join(d, name)
                with open(paths[name], 'w') as f:
                    f.write(text)
            out = os.path.join(d, 'out.db')
            with contextlib.redirect_stdout(io.StringIO()):
                dbfixup.run(
                    d,
                    zero_db_fn=paths['bits.dbf'],
                    seg_fn_in=paths['in.rdb'],
                    seg_fn_out=out,
                    groups_fn_in=paths['tag_groups.txt'],
                    strict=strict,
                    unresolved_out=os.path.join(d, 'unres.rdb')
                    if unresolved is not None else None)
            if unresolved is not None:
                with open(os.path.join(d, 'unres.rdb')) as f:
                    unresolved.extend(f.read().splitlines())
            with open(out) as f:
                return f.read().splitlines()

    def test_zero_member_is_cleared_against_its_group(self):
        self.assertEqual(self.run_dbfixup(SEGMATCH, GROUPS, False), WANT)

    def test_zero_member_resolves_under_strict(self):
        self.assertEqual(self.run_dbfixup(SEGMATCH, GROUPS, True), WANT)

    def test_ungrouped_zero_member_is_still_dropped(self):
        self.assertEqual(
            self.run_dbfixup(SEGMATCH, '', False),
            [
                'IOI3.ILOGIC_Y0.IFF.DDR_CLK_EDGE.OPPOSITE_EDGE 26_99',
                'IOI3.ILOGIC_Y0.IFF.DDR_CLK_EDGE.SAME_EDGE 27_98',
                'IOI3.ILOGIC_Y0.IFF.SRTYPE.SYNC 29_67',
            ])


GUARD_GROUPS = '''\
IOI3.ILOGIC_Y0.IFF.ZINV_D.A IOI3.ILOGIC_Y0.IFF.ZINV_D.B \
IOI3.ILOGIC_Y0.IFF.ZINV_D.K
'''

GUARD_SEGMATCH = '''\
IOI3.ILOGIC_Y0.IFF.ZINV_D.A 1_2
IOI3.ILOGIC_Y0.IFF.ZINV_D.B 3_4
IOI3.ILOGIC_Y0.IFF.ZINV_D.K <const1> 5_6 7_8
IOI3.ILOGIC_Y0.IFF.UNGROUPED.Z <0 candidates>
'''


class TestGroupTagsGuard(TestGroupedZeroMember):
    def test_unsolved_group_member_is_not_shipped_as_all_zero(self):
        unresolved = []
        out = self.run_dbfixup(
            GUARD_SEGMATCH, GUARD_GROUPS, False, unresolved=unresolved)
        self.assertEqual(
            out, [
                'IOI3.ILOGIC_Y0.IFF.ZINV_D.A !03_04 01_02',
                'IOI3.ILOGIC_Y0.IFF.ZINV_D.B !01_02 03_04',
            ])
        self.assertEqual(
            unresolved, [
                'IOI3.ILOGIC_Y0.IFF.UNGROUPED.Z <0 candidates>',
                'IOI3.ILOGIC_Y0.IFF.ZINV_D.K <const1> 5_6 7_8',
            ])

    def test_group_tags_passes_unsolved_lines_through_verbatim(self):
        _, lines = dbfixup.group_tags(
            {
                'X.A 1_2', 'X.B 3_4', 'X.K <const1> 5_6 7_8',
                'U.Z <0 candidates>'
            }, [{'X.A', 'X.B', 'X.K'}], [{(1, 2), (3, 4)}])
        self.assertIn('X.K <const1> 5_6 7_8', lines)
        self.assertIn('U.Z <0 candidates>', lines)
        self.assertNotIn('X.K !01_02 !03_04', lines)
        self.assertNotIn('U.Z', lines)


class TestCounts(unittest.TestCase):
    def quiet(self, fn, *args, **kwargs):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            ret = fn(*args, **kwargs)
        return ret, buf.getvalue()

    def test_duplicate_solutions_count_the_dropped_lines(self):
        (drops, kept), log = self.quiet(
            dbfixup.remove_ambiguous_solutions,
            'f', ['A 01_01', 'B 01_02', 'C 01_03', 'D 01_03', 'E 01_04'],
            strict=False,
            verbose=False)
        self.assertEqual(drops, 2)
        self.assertEqual(kept, {'A 01_01', 'B 01_02', 'E 01_04'})
        self.assertIn('dropped 2 duplicate solutions', log)

    def test_no_duplicate_reports_no_drop(self):
        (drops, kept), log = self.quiet(
            dbfixup.remove_ambiguous_solutions,
            'f', ['A 01_01', 'B 01_02', 'C 01_03'],
            strict=False,
            verbose=False)
        self.assertEqual(drops, 0)
        self.assertEqual(log, '')

    def test_strict_refuses_an_always_line(self):
        with self.assertRaises(AssertionError):
            self.quiet(
                dbfixup.add_zero_bits,
                'f', ['X.Y always', 'Z.W 01_01'], {},
                strict=True)

    def test_non_strict_drops_an_always_line_with_its_reason(self):
        unresolved = []
        (_, lines), _ = self.quiet(
            dbfixup.add_zero_bits,
            'f', ['X.Y always', 'Z.W 01_01'], {},
            strict=False,
            unresolved=unresolved)
        self.assertEqual(lines, {'Z.W 01_01'})
        self.assertEqual(unresolved, [('f', 'always', 'X.Y always')])

    def test_mode_reason(self):
        self.assertEqual(dbfixup.mode_reason('always'), 'always')
        self.assertEqual(dbfixup.mode_reason('<const1>'), 'const1')
        self.assertEqual(dbfixup.mode_reason('<const0>'), 'const0')
        self.assertEqual(dbfixup.mode_reason('<3 candidates>'), 'candidates')
        self.assertEqual(dbfixup.mode_reason('<m1 2>'), 'under-m1')


if __name__ == '__main__':
    unittest.main()
