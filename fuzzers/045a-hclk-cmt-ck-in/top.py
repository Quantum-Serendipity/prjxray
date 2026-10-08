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
""" 045a — same MMCM/PLL/BUFR loadload design as 045, just driven with a
different SEED.  The per-specimen target PIP selection happens in
generate.tcl, which reads targets.csv and picks based on the SPECIMEN
env var.

We delegate to 045's top.py to avoid duplicating ~600 lines of CMT
loadload synthesis.  045's top.py reads
$FUZDIR/build/cmt_regions.csv at module level, so we temporarily
override FUZDIR for the duration of the import.
"""
import os
import sys
import runpy

# Make 045's helpers importable.
PARENT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                      '..', '045-hclk-cmt-pips')
sys.path.insert(0, PARENT)

# 045's top.py expects FUZDIR pointing at *its own* fuzzer dir so it can
# read build/cmt_regions.csv.  Save the caller's value and restore after.
caller_fuzdir = os.environ.get('FUZDIR')
os.environ['FUZDIR'] = PARENT
try:
    runpy.run_path(os.path.join(PARENT, 'top.py'), run_name='__main__')
finally:
    if caller_fuzdir is None:
        os.environ.pop('FUZDIR', None)
    else:
        os.environ['FUZDIR'] = caller_fuzdir
