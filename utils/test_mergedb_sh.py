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
import shutil
import subprocess
import sys
import tempfile
import unittest

XRAY_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MERGEDB = os.path.join(XRAY_DIR, 'utils', 'mergedb.sh')
FAMILY = 'artix7'


@unittest.skipUnless(shutil.which('flock'), 'mergedb.sh needs flock')
class TestMergedbSh(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = self.tmp.name
        self.family_dir = os.path.join(self.root, 'db', FAMILY)
        os.makedirs(self.family_dir)
        self.slow = os.path.join(self.root, 'parsedb-slow.sh')
        with open(self.slow, 'w') as f:
            f.write(
                '#!/bin/sh\nsleep "${PARSEDB_DELAY:-0}"\n'
                'exec "%s" "%s" "$@"\n' %
                (sys.executable, os.path.join(XRAY_DIR, 'utils', 'parsedb.py')))
        os.chmod(self.slow, 0o755)

    def tearDown(self):
        self.tmp.cleanup()

    def env(self, delay):
        return {
            'PATH':
            os.path.dirname(sys.executable) + os.pathsep +
            os.environ.get('PATH', '/usr/bin:/bin'),
            'PYTHONPATH': XRAY_DIR,
            'XRAY_DIR': XRAY_DIR,
            'XRAY_DATABASE': FAMILY,
            'XRAY_DATABASE_DIR': os.path.join(self.root, 'db'),
            'XRAY_PARSEDB': self.slow,
            'PARSEDB_DELAY': str(delay),
            'LOCKTIMEOUT': '120',
        }

    def write(self, name, lines):
        path = os.path.join(self.root, name)
        with open(path, 'w') as f:
            f.write(''.join(l + '\n' for l in lines))
        return path

    def db(self, name):
        path = os.path.join(self.family_dir, name)
        if not os.path.exists(path):
            return None
        with open(path) as f:
            return f.read().splitlines()

    def spawn(self, mode, fragment, delay, workdir):
        os.makedirs(workdir, exist_ok=True)
        return subprocess.Popen(
            ['bash', MERGEDB, mode, fragment],
            cwd=workdir,
            env=self.env(delay),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            universal_newlines=True)

    def merge(self, mode, fragment, delay=0):
        p = self.spawn(mode, fragment, delay, os.path.join(self.root, 'w0'))
        out, _ = p.communicate(timeout=300)
        return p.returncode, out

    def concurrent(self, mode, fragments, delay=1):
        procs = [
            self.spawn(
                mode, frag, delay, os.path.join(self.root, 'w%d' % i))
            for i, frag in enumerate(fragments)
        ]
        results = []
        for p in procs:
            out, _ = p.communicate(timeout=300)
            results.append((p.returncode, out))
        for rc, out in results:
            self.assertEqual(rc, 0, out[-2000:])

    def test_two_mask_writers_into_a_new_db_keep_both(self):
        a = self.write('a.db', ['bit 26_80'])
        b = self.write('b.db', ['bit 26_240'])
        self.concurrent('mask_dsp_l', [a, b])
        self.assertEqual(
            sorted(self.db('mask_dsp_l.db')), ['bit 26_240', 'bit 26_80'])

    def test_two_mask_writers_into_an_existing_db_keep_both(self):
        with open(os.path.join(self.family_dir, 'mask_dsp_l.db'), 'w') as f:
            f.write('bit 00_00\n')
        a = self.write('a.db', ['bit 26_80'])
        b = self.write('b.db', ['bit 26_240'])
        self.concurrent('mask_dsp_l', [a, b])
        self.assertEqual(
            sorted(self.db('mask_dsp_l.db')),
            ['bit 00_00', 'bit 26_240', 'bit 26_80'])

    def test_two_segbits_writers_into_a_new_db_keep_both(self):
        a = self.write('a.db', ['CFG_CENTER_MID.PROBE_A 26_2162'])
        b = self.write('b.db', ['CFG_CENTER_MID.PROBE_B 26_2163'])
        self.concurrent('cfg_center_mid', [a, b])
        self.assertEqual(
            self.db('segbits_cfg_center_mid.db'), [
                'CFG_CENTER_MID.PROBE_A 26_2162',
                'CFG_CENTER_MID.PROBE_B 26_2163'
            ])
        self.assertEqual(
            len(self.db('segbits_cfg_center_mid.origin_info.db')), 2)

    def test_refused_merge_leaves_segbits_and_origin_info_untouched(self):
        seg = os.path.join(self.family_dir, 'segbits_cfg_center_mid.db')
        org = os.path.join(
            self.family_dir, 'segbits_cfg_center_mid.origin_info.db')
        with open(seg, 'w') as f:
            f.write('CFG_CENTER_MID.A 26_2162\n')
        open(org, 'w').close()
        frag = self.write('in.db', ['CFG_CENTER_MID.B 26_2162'])
        rc, out = self.merge('cfg_center_mid', frag)
        self.assertNotEqual(rc, 0, out[-2000:])
        self.assertEqual(self.db('segbits_cfg_center_mid.db'),
                         ['CFG_CENTER_MID.A 26_2162'])
        self.assertEqual(self.db('segbits_cfg_center_mid.origin_info.db'), [])

    def test_refused_first_merge_creates_no_db(self):
        frag = self.write('in.db', ['CFG_CENTER_MID.PROBE_A 26_2162'])
        p = self.spawn(
            'cfg_center_mid', frag, 0, os.path.join(self.root, 'w,0'))
        out, _ = p.communicate(timeout=300)
        rc = p.returncode
        self.assertNotEqual(rc, 0, out[-2000:])
        self.assertIsNone(self.db('segbits_cfg_center_mid.db'))
        self.assertIsNone(self.db('segbits_cfg_center_mid.origin_info.db'))

    def test_a_single_merge_lands_with_its_origin(self):
        frag = self.write('in.db', ['CFG_CENTER_MID.PROBE_A 26_2162'])
        rc, out = self.merge('cfg_center_mid', frag)
        self.assertEqual(rc, 0, out[-2000:])
        self.assertEqual(
            self.db('segbits_cfg_center_mid.db'),
            ['CFG_CENTER_MID.PROBE_A 26_2162'])
        self.assertEqual(
            self.db('segbits_cfg_center_mid.origin_info.db'),
            ['CFG_CENTER_MID.PROBE_A origin:w0 26_2162'])


if __name__ == '__main__':
    unittest.main()
