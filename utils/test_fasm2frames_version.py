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
from io import StringIO
import sys
import types
import unittest
from unittest import mock

import utils.fasm2frames as fasm2frames


def run_version(modules):
    out = StringIO()
    with mock.patch.dict(sys.modules, modules), \
            mock.patch.object(sys, 'argv', ['fasm2frames', '--version']), \
            contextlib.redirect_stdout(out):
        try:
            fasm2frames.main()
        except SystemExit as e:
            return e.code, out.getvalue()
    return None, out.getvalue()


class TestFasm2framesVersion(unittest.TestCase):
    def test_reports_the_stamped_revision(self):
        rev = types.ModuleType('prjxray.xc7dpr_revision')
        rev.REVISION = '0123456789abcdef0123456789abcdef01234567'
        code, out = run_version({'prjxray.xc7dpr_revision': rev})
        self.assertEqual(code, 0)
        self.assertEqual(
            out.strip(),
            'fasm2frames xc7dpr 0123456789abcdef0123456789abcdef01234567')

    def test_reports_unknown_without_a_stamp(self):
        code, out = run_version({'prjxray.xc7dpr_revision': None})
        self.assertEqual(code, 0)
        self.assertEqual(out.strip(), 'fasm2frames xc7dpr unknown')


if __name__ == '__main__':
    unittest.main()
