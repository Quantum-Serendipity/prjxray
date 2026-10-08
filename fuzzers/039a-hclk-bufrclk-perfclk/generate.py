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
#
# Segmaker.  The HCLK_L / HCLK_R enable-buffer tag is read from the leaf
# pip routed in THAT tile in THAT specimen,
#
#   HCLK_L_*/HCLK_L.HCLK_CK_BUFRCLKn->>HCLK_LEAF_CLK_B_*
#
# and never from the BUFR site or from BUFR_Yn.IN_USE, which is 039's
# predicate.  Two negative specimens (see README) establish that a BUFR
# which is placed, or consumed inside the IOI column, leaves the enable
# at zero: what sets it is the clock crossing the spine into the fabric.
# Specimens whose BUFR feeds an ODDR stay in the population as honest
# zeroes rather than being excluded.
#
# The PERFCLK and CLK_PERF tags are read from the same pip dump.  045
# skips the PHSR family through its EXCLUDE_RE, so those positions have
# to be tagged explicitly.
import os
import re
import sys

from prjxray.db import Database
from prjxray.segmaker import Segmaker
from prjxray import util


HCLK_CMT_PERFCLK = [
    ("HCLK_CMT_MUX_PHSR_PERFCLK0", "HCLK_CMT_MUX_MMCM_MUXED0"),
    ("HCLK_CMT_MUX_PHSR_PERFCLK1", "HCLK_CMT_MUX_MMCM_MUXED0"),
    ("HCLK_CMT_MUX_PHSR_PERFCLK0", "HCLK_CMT_MUX_MMCM_MUXED1"),
    ("HCLK_CMT_MUX_PHSR_PERFCLK1", "HCLK_CMT_MUX_MMCM_MUXED1"),
    ("HCLK_CMT_MUX_PHSR_PERFCLK2", "HCLK_CMT_MUX_MMCM_MUXED2"),
    ("HCLK_CMT_MUX_PHSR_PERFCLK3", "HCLK_CMT_MUX_MMCM_MUXED2"),
    ("HCLK_CMT_MUX_PHSR_PERFCLK2", "HCLK_CMT_MUX_MMCM_MUXED3"),
    ("HCLK_CMT_MUX_PHSR_PERFCLK3", "HCLK_CMT_MUX_MMCM_MUXED3"),
]

CMT_CLKOUTS = [
    "CMT_LR_LOWER_B_MMCM_CLKFBOUT",
    "CMT_LR_LOWER_B_MMCM_CLKOUT0",
    "CMT_LR_LOWER_B_MMCM_CLKOUT1",
    "CMT_LR_LOWER_B_MMCM_CLKOUT2",
    "CMT_LR_LOWER_B_MMCM_CLKOUT3",
]

LEAF_DST_PREFIX = "HCLK_LEAF_CLK_B_"


def parse_pips(path):
    """Return {tile: set((src, dst))} from generate.tcl's design_pips.txt."""
    used = {}
    if not os.path.exists(path):
        return used
    rx = re.compile(r"^([^/]+)/[^.]+(?:\.)(.+?)->>?(.+)$")
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            m = rx.match(line)
            if not m:
                continue
            tile, src, dst = m.group(1), m.group(2), m.group(3)
            used.setdefault(tile, set()).add((src, dst))
    return used


def cmt_perf_pairs(side):
    dests = ["CMT_{}_LOWER_B_CLK_PERF{}".format(side, n) for n in range(4)]
    return [(d, s) for s in CMT_CLKOUTS for d in dests]


def leaf_pip_present(tile_used, n):
    """True iff this tile routed HCLK_CK_BUFRCLKn onto a leaf clock."""
    src_want = "HCLK_CK_BUFRCLK{}".format(n)
    for src, dst in tile_used:
        routed_to_leaf = (
            src == src_want and dst.startswith(LEAF_DST_PREFIX)
        )
        if routed_to_leaf:
            return True
    return False


def main():
    segmk = Segmaker("design.bits")
    used = parse_pips("design_pips.txt")

    db = Database(util.get_db_root(), util.get_part())
    grid = db.grid()

    n_en_one = 0
    n_en_tiles = 0
    for tile_name in grid.tiles():
        loc = grid.loc_of_tilename(tile_name)
        gi = grid.gridinfo_at_loc(loc)
        ttype = gi.tile_type
        tile_used = used.get(tile_name, set())

        if ttype in ("HCLK_L", "HCLK_R"):
            any_one = False
            for n in range(4):
                tag = "ENABLE_BUFFER.HCLK_CK_BUFRCLK{}".format(n)
                val = 1 if leaf_pip_present(tile_used, n) else 0
                segmk.add_tile_tag(tile_name, tag, val)
                n_en_one += val
                any_one = any_one or bool(val)
            n_en_tiles += int(any_one)

        elif ttype in ("HCLK_CMT", "HCLK_CMT_L"):
            for dst, src in HCLK_CMT_PERFCLK:
                tag = "{}.{}".format(dst, src)
                val = 1 if (src, dst) in tile_used else 0
                segmk.add_tile_tag(tile_name, tag, val)

        elif ttype == "CMT_TOP_R_LOWER_B":
            for dst, src in cmt_perf_pairs("R"):
                tag = "{}.{}".format(dst, src)
                val = 1 if (src, dst) in tile_used else 0
                segmk.add_tile_tag(tile_name, tag, val)

        elif ttype == "CMT_TOP_L_LOWER_B":
            for dst, src in cmt_perf_pairs("L"):
                tag = "{}.{}".format(dst, src)
                val = 1 if (src, dst) in tile_used else 0
                segmk.add_tile_tag(tile_name, tag, val)

    print("tiles_with_pips={} enable_ones={} enable_tiles={}".format(
        len(used), n_en_one, n_en_tiles), file=sys.stderr)

    def bitfilter(frame, bit):
        return True

    segmk.compile(bitfilter=bitfilter)
    segmk.write(allow_empty=True)


if __name__ == "__main__":
    main()
