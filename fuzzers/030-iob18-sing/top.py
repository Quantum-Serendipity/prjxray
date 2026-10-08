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
SING-row IOB18 basic-config fuzzer design.  For each IOB18 site that lives in a
*_SING tile, randomly instantiate OBUF / IBUF / nothing.  The mix of driven,
input-only and unused sites gives segmaker the 0-reference it needs to resolve
the OUT / DRIVE / IN_USE bits (an all-output design leaves them const-1).

DRIVE / SLEW / PULLTYPE are applied to the ports by generate.tcl's loc_pins
from params.csv; the per-site type is recorded in params.jl for generate.py.
"""
import json
import os
import random
random.seed(int(os.getenv("SEED"), 16))
from prjxray import util
from prjxray import verilog
from prjxray.db import Database

# HP banks: low-voltage standards only.
IOSTANDARDS = ['LVCMOS12', 'LVCMOS15', 'LVCMOS18']
PULLS = ["NONE", "KEEPER", "PULLDOWN", "PULLUP"]
SLEWS = ['FAST', 'SLOW']
# OBUF (drive out), IBUF (input only) and unused, weighted toward variety.
TYPES = ['OBUF', 'OBUF', 'IBUF', 'IBUF', None, None]


def drives_for(iostandard):
    if iostandard == 'LVCMOS12':
        return [2, 4, 6, 8]
    return [2, 4, 6, 8, 12, 16]


def gen_sites():
    db = Database(util.get_db_root(), util.get_part())
    grid = db.grid()
    for tile_name in sorted(grid.tiles()):
        gridinfo = grid.gridinfo_at_loc(grid.loc_of_tilename(tile_name))
        if not gridinfo.tile_type.endswith("_SING"):
            continue
        for site_name, site_type in gridinfo.sites.items():
            if site_type == 'IOB18':
                yield tile_name, site_name


def main():
    # One IOSTANDARD per specimen: all SING IOBs share it so a bank never sees
    # conflicting VCCO (DRC BIVC-1).  Variety across the N specimens gives
    # process_rdb the LVCMOS12/15/18 set it merges into the DRIVE enum names.
    iostandard = random.choice(IOSTANDARDS)
    params = []
    for tile, site in gen_sites():
        params.append(
            {
                'tile': tile,
                'site': site,
                'type': random.choice(TYPES),
                'IOSTANDARD': verilog.quote(iostandard),
                'DRIVE': random.choice(drives_for(iostandard)),
                'SLEW': verilog.quote(random.choice(SLEWS)),
                'PULLTYPE': verilog.quote(random.choice(PULLS)),
            })

    # params.csv for generate.tcl's loc_pins (tile,site,pin,iostd,drive,slew,pull)
    lines = ['tile,site,pin,iostandard,drive,slew,pulltype']
    n_in = n_out = 0
    for p in params:
        if p['type'] == 'OBUF':
            p['pin'] = 'do[{}]'.format(n_out)
            n_out += 1
        elif p['type'] == 'IBUF':
            p['pin'] = 'di[{}]'.format(n_in)
            n_in += 1
        else:
            p['pin'] = None
            continue
        lines.append(
            ','.join(
                map(
                    str, (
                        p['tile'], p['site'], p['pin'],
                        verilog.unquote(p['IOSTANDARD']), p['DRIVE'],
                        verilog.unquote(p['SLEW']),
                        verilog.unquote(p['PULLTYPE'])))))
    open('params.csv', 'w').write('\n'.join(lines) + '\n')

    with open('params.jl', 'w') as f:
        json.dump(params, f, indent=2)

    # Verilog: one OBUF / IBUF per used site, LOC'd; unused sites get nothing.
    # OBUFs are driven by a DONT_TOUCH LUT source (no external clock port, which
    # would be an unplaceable terminal); DRIVE/SLEW/PULLTYPE come from loc_pins.
    print('module top(output wire [{no}:0] do, input wire [{ni}:0] di);'.format(
        no=max(n_out - 1, 0), ni=max(n_in - 1, 0)))
    print('    wire src;')
    print('    (* KEEP, DONT_TOUCH *) '
          'LUT6 #(.INIT(64\'hAAAAAAAAAAAAAAAA)) srclut '
          '(.O(src), .I0(1\'b0), .I1(1\'b0), .I2(1\'b0), '
          '.I3(1\'b0), .I4(1\'b0), .I5(1\'b0));')
    for idx, p in enumerate(params):
        if p['type'] == 'OBUF':
            print(
                '''
    (* KEEP, DONT_TOUCH, LOC = "{site}" *)
    OBUF obuf_{idx} (.I(src), .O({pin}));'''.format(
                    site=p['site'], idx=idx, pin=p['pin']))
        elif p['type'] == 'IBUF':
            print(
                '''
    wire ib_{idx};
    (* KEEP, DONT_TOUCH, LOC = "{site}" *)
    IBUF ibuf_{idx} (.I({pin}), .O(ib_{idx}));'''.format(
                    site=p['site'], idx=idx, pin=p['pin']))
    print('endmodule')


if __name__ == '__main__':
    main()
