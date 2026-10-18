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

from prjxray import presence

TWO_SIDED = [
    {
        'CFG_CENTER_MID.ICAP_X0Y0.ENABLE_TR': 1,
        'CFG_CENTER_MID.ICAP_X0Y1.ENABLE_TR': 0,
        'CFG_CENTER_MID.DCIRESET.ENABLED': 1,
    },
    {
        'CFG_CENTER_MID.ICAP_X0Y0.ENABLE_TR': 0,
        'CFG_CENTER_MID.ICAP_X0Y1.ENABLE_TR': 1,
        'CFG_CENTER_MID.DCIRESET.ENABLED': 1,
    },
]


class TestPresenceGate(unittest.TestCase):
    def gate(self, specimens, *flags):
        with tempfile.TemporaryDirectory() as d:
            paths = []
            for i, tags in enumerate(specimens):
                path = os.path.join(d, 'segdata_%d.txt' % i)
                with open(path, 'w') as f:
                    print('seg 00020000_068', file=f)
                    for tag, val in sorted(tags.items()):
                        print('tag %s %d' % (tag, val), file=f)
                paths.append(path)
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                rc = presence.main(list(flags) + paths)
            return rc, out.getvalue()

    def test_two_sided_filter_passes(self):
        rc, out = self.gate(TWO_SIDED, '--only', 'ENABLE_TR$')
        self.assertEqual(rc, 0, out)
        self.assertIn('all 2 tag(s) matching', out)
        self.assertNotIn('every tag', out)

    def test_filter_matching_nothing_is_refused(self):
        dci_only = [{
            'CFG_CENTER_MID.DCIRESET.ENABLED': 1
        }, {
            'CFG_CENTER_MID.DCIRESET.ENABLED': 0
        }]
        rc, out = self.gate(dci_only, '--only', 'ENABLE_TR$')
        self.assertEqual(rc, 2, out)
        self.assertNotIn('OK', out)
        self.assertIn('matches none', out)

    def test_one_sided_tag_in_filter_fails(self):
        blind = [dict(s) for s in TWO_SIDED]
        for s in blind:
            s['CFG_CENTER_MID.ICAP_X0Y1.ENABLE_TR'] = 1
        rc, out = self.gate(blind, '--only', 'ENABLE_TR$')
        self.assertEqual(rc, 1, out)
        self.assertIn('count0=0, segmatch will emit <const1>', out)

    def test_one_sided_tag_under_warn_reports_and_passes(self):
        blind = [dict(s) for s in TWO_SIDED]
        for s in blind:
            s['CFG_CENTER_MID.ICAP_X0Y0.ENABLE_TR'] = 0
        rc, out = self.gate(blind, '--warn')
        self.assertEqual(rc, 0, out)
        self.assertIn('count1=0, segmatch will emit <const0>', out)
        self.assertNotIn('OK', out)

    def test_blind_tag_outside_filter_is_not_graded(self):
        rc, out = self.gate(TWO_SIDED, '--only', 'ENABLE_TR$')
        self.assertEqual(rc, 0, out)
        rc, out = self.gate(TWO_SIDED)
        self.assertEqual(rc, 1, out)
        self.assertIn('DCIRESET.ENABLED', out)

    def test_no_tags_is_refused(self):
        rc, out = self.gate([{}, {}])
        self.assertEqual(rc, 2, out)


if __name__ == '__main__':
    unittest.main()
