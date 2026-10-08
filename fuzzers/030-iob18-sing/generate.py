#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# Copyright (C) 2017-2022  The Project X-Ray Authors.
#
# Use of this source code is governed by a ISC-style
# license that can be found in the LICENSE file or at
# https://opensource.org/licenses/ISC
#
# SPDX-License-Identifier: ISC
"""
SING-row sibling of 030-iob18.  Tags the basic IOB18 config (IN_USE / OUT / IN /
IN_ONLY / IN_DIFF / PULLTYPE / DRIVE / SLEW / STEPDOWN) for the single IOB18
site in each *_SING tile, per the OBUF / IBUF / unused type chosen in top.py.

Reuses 030-iob18's drive_opts / enum-zero convention so the merged feature
names (after process_rdb.py) match nextpnr, e.g.
LVCMOS15_LVCMOS18.DRIVE.I12_I16_I2_I4_I6_I8.  Single-ended only.
"""
from prjxray.segmaker import Segmaker
from prjxray import segmaker
from prjxray import verilog
import json

LVCMOS = ['LVCMOS12', 'LVCMOS15', 'LVCMOS18']


def bitfilter(frame, word):
    # SING tile frames differ from regular LIOB18; let segmatch find them.
    return True


def mk_drive_opt(iostandard, drive):
    if drive is None:
        drive = '_FIXED'
    return '{}.DRIVE.I{}'.format(iostandard, drive)


def main():
    print("Loading tags")
    segmk = Segmaker("design.bits")

    with open('params.jl', 'r') as f:
        design = json.load(f)

    for d in design:
        site = d['site']
        iostandard = verilog.unquote(d['IOSTANDARD'])
        typ = d['type']  # 'OBUF', 'IBUF', or None (unused)

        in_use = 1 if typ is not None else 0
        is_out = 1 if typ == 'OBUF' else 0
        is_in = 1 if typ == 'IBUF' else 0

        segmk.add_site_tag(site, 'INOUT', 0)
        segmk.add_site_tag(site, '{}.IN_USE'.format(iostandard), in_use)
        segmk.add_site_tag(site, '{}.OUT'.format(iostandard), is_out)
        segmk.add_site_tag(site, '{}.IN'.format(iostandard), is_in)
        segmk.add_site_tag(site, '{}.IN_ONLY'.format(iostandard), is_in)
        segmk.add_site_tag(site, '{}.IN_DIFF'.format(iostandard), 0)

        # PULLTYPE applies whenever the IOB is in use.
        if in_use:
            segmaker.add_site_group_zero(
                segmk, site, "PULLTYPE.",
                ("NONE", "KEEPER", "PULLDOWN", "PULLUP"), "PULLDOWN",
                verilog.unquote(d['PULLTYPE']))

        # DRIVE / SLEW / STEPDOWN only meaningful for output buffers.
        if typ == 'OBUF':
            drive_opts = set()
            for opt in LVCMOS:
                for drive_opt in ("2", "4", "6", "8", "12", "16"):
                    if drive_opt in ["12", "16"] and opt == "LVCMOS12":
                        continue
                    drive_opts.add(mk_drive_opt(opt, drive_opt))
            segmaker.add_site_group_zero(
                segmk, site, '', drive_opts, mk_drive_opt('LVCMOS25', '12'),
                mk_drive_opt(iostandard, d['DRIVE']))

            if d['SLEW']:
                for opt in ["SLOW", "FAST"]:
                    segmk.add_site_tag(
                        site, iostandard + ".SLEW." + opt,
                        opt == verilog.unquote(d['SLEW']))

            segmk.add_site_tag(site, '{}.STEPDOWN'.format(iostandard), 1)

    segmk.compile(bitfilter=bitfilter)
    segmk.write(allow_empty=True)


if __name__ == "__main__":
    main()
