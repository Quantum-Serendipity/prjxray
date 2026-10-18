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

import os
import subprocess
import tempfile
import unittest

GENHEADER = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'genheader.sh')


class TestGenheaderVersionGate(unittest.TestCase):
    def source(self, stub_version, env_version, verified=None):
        with tempfile.TemporaryDirectory() as d:
            stub = os.path.join(d, 'vivado')
            with open(stub, 'w') as f:
                if stub_version is None:
                    f.write('#!/bin/sh\nexit 1\n')
                else:
                    f.write(
                        '#!/bin/sh\nprintf "Vivado %s (64-bit)\\n"\n' %
                        stub_version)
            os.chmod(stub, 0o755)
            env = {
                'PATH': os.environ.get('PATH', '/usr/bin:/bin'),
                'XRAY_DATABASE': 'artix7',
                'XRAY_VIVADO': stub,
            }
            if env_version is not None:
                env['XRAY_VIVADO_VERSION'] = env_version
            if verified is not None:
                env['XRAY_VIVADO_VERSION_VERIFIED'] = verified
            fz = os.path.join(d, 'fz')
            os.mkdir(fz)
            p = subprocess.run(
                [
                    'bash', '-c', 'source "$0" "$1" >/dev/null 2>&1 || exit $?;'
                    ' echo "VERIFIED=$XRAY_VIVADO_VERSION_VERIFIED"', GENHEADER,
                    os.path.join(fz, 'specimen_001')
                ],
                cwd=fz,
                env=env,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                universal_newlines=True,
                timeout=60)
            return p.returncode, p.stdout.strip()

    def test_matching_version_passes_and_is_cached(self):
        self.assertEqual(
            self.source('v2024.1', 'v2024.1'), (0, 'VERIFIED=v2024.1'))

    def test_other_family_default_passes(self):
        self.assertEqual(
            self.source('v2020.1.1', 'v2020.1.1'), (0, 'VERIFIED=v2020.1.1'))

    def test_mismatch_is_refused(self):
        self.assertEqual(self.source('v2017.2', 'v2024.1')[0], 3)

    def test_unset_version_is_refused(self):
        self.assertEqual(self.source('v2024.1', None)[0], 3)

    def test_stale_cache_is_rechecked(self):
        self.assertEqual(
            self.source('v2017.2', 'v2024.1', verified='v2017.2')[0], 3)

    def test_cache_for_the_required_version_is_honoured(self):
        self.assertEqual(
            self.source(None, 'v2024.1', verified='v2024.1'),
            (0, 'VERIFIED=v2024.1'))


if __name__ == '__main__':
    unittest.main()
