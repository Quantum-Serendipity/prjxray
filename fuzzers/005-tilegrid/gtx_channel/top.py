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
import random
random.seed(int(os.getenv("SEED"), 16))
from prjxray import util
from prjxray.db import Database


def gen_sites():
    db = Database(util.get_db_root(), util.get_part())
    grid = db.grid()
    for tile_name in sorted(grid.tiles()):
        loc = grid.loc_of_tilename(tile_name)
        gridinfo = grid.gridinfo_at_loc(loc)

        for site_name, site_type in gridinfo.sites.items():
            if site_type in ['GTXE2_CHANNEL']:
                # XRAY_GTX_SITES restricts the fuzz to a comma-separated list of
                # site names.  Needed on virtex7: the xc7vx485t-ffg1761 package
                # bonds only about half its GTX quads, and an UNBONDED quad
                # cannot be placed, so fuzzing every site fails the whole run --
                # which is why 005-tilegrid skips GTX for virtex7 entirely.
                # Restricting to a bonded quad measures the tiles that a real
                # design actually uses (this board's SGMII sits on
                # GTXE2_CHANNEL_X1Y* / GTXE2_COMMON_X1Y0) and leaves the
                # unbonded ones legitimately unmeasured.
                want = os.getenv("XRAY_GTX_SITES")
                if want and site_name not in want.split(','):
                    continue
                yield tile_name, site_name


def write_params(params):
    pinstr = 'tile,val,site\n'
    for tile, (site, val) in sorted(params.items()):
        pinstr += '%s,%s,%s\n' % (tile, val, site)
    open('params.csv', 'w').write(pinstr)


def run():
    print('''
module top(input wire in, output wire out);
    ''')

    params = {}

    sites = list(gen_sites())
    for (tile_name, site_name), isone in zip(sites,
                                             util.gen_fuzz_states(len(sites))):
        params[tile_name] = (site_name, isone)

        print(
            '''
    (* KEEP, DONT_TOUCH, LOC = "{}" *)
   GTXE2_CHANNEL #(
        .ALIGN_MCOMMA_DET("{}")
    ) gtxe2_channel_{} ();'''.format(site_name, "TRUE" if isone else "FALSE", site_name))

    print("endmodule")
    write_params(params)


if __name__ == '__main__':
    run()
