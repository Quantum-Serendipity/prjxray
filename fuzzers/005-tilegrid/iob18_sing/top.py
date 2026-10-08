#!/usr/bin/env python3
# Copyright (C) 2017-2020  The Project X-Ray Authors.
#
# Use of this source code is governed by a ISC-style
# license that can be found in the LICENSE file or at
# https://opensource.org/licenses/ISC
#
# SPDX-License-Identifier: ISC
#
# 005-tilegrid sub-fuzzer for SING-row IOB18 tiles.  The existing
# iob18 sub-fuzzer filters to `IOB18S` (the main IOB of a diff pair)
# and ioi18 explicitly skips `_SING` tile types, so the SING-row
# IOB18 sites (one per LIOB18_SING / RIOB18_SING tile) never get
# their tilegrid frame addresses resolved by the existing fuzzers.
# This sub-fuzzer addresses that gap (task #17): place an IBUF on
# every SING-row IOB18 site, then bit-diff produces a tdb that
# add_tdb.py merges into tilegrid.json.
import os
import random
random.seed(int(os.getenv("SEED"), 16))
from prjxray import util
from prjxray.db import Database


def gen_sites():
    '''SING-row IOB18 singleton sites: tile_type ends in _SING, site
    type is plain IOB18 (no S/M suffix, no diff-pair partner).'''
    db = Database(util.get_db_root(), util.get_part())
    grid = db.grid()
    for tile_name in sorted(grid.tiles()):
        loc = grid.loc_of_tilename(tile_name)
        gridinfo = grid.gridinfo_at_loc(loc)

        if not gridinfo.tile_type.endswith("_SING"):
            continue
        # Skip non-IOB SING tiles (LIOI_SING etc.); only LIOB18_SING
        # / RIOB18_SING expose an IOB18 site.
        for site_name, site_type in gridinfo.sites.items():
            if site_type == 'IOB18':
                yield tile_name, site_name


def write_params(params):
    pinstr = 'tile,val,site,pin\n'
    for tile, (site, val, pin) in sorted(params.items()):
        pinstr += '%s,%s,%s,%s\n' % (tile, val, site, pin)
    open('params.csv', 'w').write(pinstr)


def run():
    sites = list(gen_sites())
    print(
        '''
`define N_DI {}

module top(input wire [`N_DI-1:0] di);
    wire [`N_DI-1:0] di_buf;
    '''.format(len(sites)))

    params = {}
    print('''
        (* KEEP, DONT_TOUCH *)
        LUT6 dummy_lut();''')

    for idx, ((tile_name, site_name), isone) in enumerate(zip(
            sites, util.gen_fuzz_states(len(sites)))):
        params[tile_name] = (site_name, isone, "di[%u]" % idx)
        print(
            '''
    (* KEEP, DONT_TOUCH *)
    IBUF #(
    ) ibuf_{site_name} (
        .I(di[{idx}]),
        .O(di_buf[{idx}])
        );'''.format(site_name=site_name, idx=idx))

        if isone:
            print(
                '''
    (* KEEP, DONT_TOUCH *)
    PULLUP #(
    ) pullup_{site_name} (
        .O(di[{idx}])
        );'''.format(site_name=site_name, idx=idx))

    print("endmodule")
    write_params(params)


if __name__ == '__main__':
    run()
