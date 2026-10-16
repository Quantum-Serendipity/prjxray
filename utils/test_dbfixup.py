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
    def run_dbfixup(self, segmatch, groups, strict):
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
                    strict=strict)
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


if __name__ == '__main__':
    unittest.main()
