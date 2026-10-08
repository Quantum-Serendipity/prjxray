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
import json
import os
import random
random.seed(int(os.getenv("SEED"), 16))
from prjxray.db import Database
from prjxray import util
from prjxray.lut_maker import LutMaker

# Use the IOSTANDARD set by the part's settings script
# (LVCMOS33 on HR banks, LVCMOS18 on HP banks). Falls back to LVCMOS33
# to preserve the historical kintex7 behaviour when the env var is unset.
IOSTANDARD = os.getenv('XRAY_IOSTANDARD', 'LVCMOS33')


def iostandard_for(site_type):
    # HP banks (IOB18) are 1.8 V. One XRAY_IOSTANDARD cannot describe a
    # part that has both HR and HP tiles; the historical default is the
    # HR value and Vivado rejects it on an HP bank.
    if site_type.startswith('IOB18'):
        return 'LVCMOS18'
    return IOSTANDARD


def gen_sites():
    xy_fun = util.create_xy_fun('BUFR_')
    db = Database(util.get_db_root(), util.get_part())
    grid = db.grid()
    # MMCME2_ADV site per clock region: an MMCM feeding a bank's BUFIOs must sit
    # in that bank's own region (IOCLK_PLL is a region-local path).  Only the
    # MMCM's CLKOUT0..3 reach BUFIO -- a PLLE2's outputs are unroutable to BUFIO
    # sites (measured), which is also why 047 registers only MMCM outputs as
    # BUFIO-capable sources.
    mmcm_by_region = {}
    for tn in grid.tiles():
        gi = grid.gridinfo_at_loc(grid.loc_of_tilename(tn))
        for st, sty in gi.sites.items():
            if sty == 'MMCME2_ADV' and gi.clock_region is not None:
                mmcm_by_region[str(gi.clock_region)] = st

    for tile_name in sorted(grid.tiles()):
        loc = grid.loc_of_tilename(tile_name)
        gridinfo = grid.gridinfo_at_loc(loc)
        sites = []

        xs = []
        ys = []
        bufio_sites = []
        bufio_xy = util.create_xy_fun('BUFIO_')
        for site, site_type in gridinfo.sites.items():
            if site_type == 'BUFR':
                x, y = xy_fun(site)
                xs.append(x)
                ys.append(y)

                sites.append((site, x, y))
            elif site_type == 'BUFIO':
                bx, by = bufio_xy(site)
                bufio_sites.append((site, bx, by))

        if not sites:
            continue

        # variable still named 'ioi3' but the check below accepts either
        # IOI3 (HR-bank, kintex7) or IOI (HP-bank, virtex7 xc7vx485tffg1761-2).
        ioi3 = grid.gridinfo_at_loc((loc.grid_x, loc.grid_y - 1))

        if 'IOI3' not in ioi3.tile_type and 'IOI' not in ioi3.tile_type:
            continue

        if ioi3.tile_type.startswith('R'):
            dx = 1
        else:
            assert ioi3.tile_type.startswith('L')
            dx = -1

        iobs = []
        iobs_s = []
        ilogics = []
        iostd_of = {}

        for dy in (-1, -3, 2, 4):
            iob = grid.gridinfo_at_loc((loc.grid_x + dx, loc.grid_y + dy))

            for site, site_type in iob.sites.items():
                # IOB33M = HR-bank (kintex7); IOB18M = HP-bank (virtex7).
                if site_type in ('IOB33M', 'IOB18M', 'IOB33S', 'IOB18S'):
                    iostd_of[site] = iostandard_for(site_type)
                if site_type in ('IOB33M', 'IOB18M'):
                    iobs.append(site)
                elif site_type in ('IOB33S', 'IOB18S'):
                    iobs_s.append(site)

            ioi = grid.gridinfo_at_loc((loc.grid_x, loc.grid_y + dy))
            for site, site_type in sorted(ioi.sites.items()):
                # One ILOGIC per dy, the first in site-name order. HR tiles
                # only have ILOGICE3, so this is the historical choice there.
                if site_type in ('ILOGICE3', 'ILOGICE2'):
                    ilogics.append(site)
                    break

        # No clock-capable IOB in this tile's neighbourhood: the specimen
        # has nothing legal to drive the BUFR with. xc7z045's bottom
        # HCLK_IOI3 sits on the unbonded region and is in that state.
        if not iobs:
            continue

        mmcm_site = mmcm_by_region.get(str(gridinfo.clock_region))
        yield tile_name, min(xs), min(ys), sorted(sites), sorted(iobs), sorted(bufio_sites), ilogics, mmcm_site, sorted(iobs_s), iostd_of


def main():

    params_list = []
    num_clocks = 0
    num_outs = 0
    outputs = []
    luts = LutMaker()
    for tile_name, x_min, y_min, sites, iobs, bufio_sites, ilogics, mmcm_site, iobs_s, iostd_of in gen_sites():
        outs_used = 0
        ioclks = []
        for iob in iobs:
            ioclk = 'clk_{}'.format(iob)
            ioclks.append(ioclk)
            idx = num_clocks
            num_clocks += 1
            outputs.append(
                '''
        wire {ioclk};

        (* KEEP, DONT_TOUCH, LOC="{site}" *)
        IBUF #(
            .IOSTANDARD("{iostandard}")
            ) ibuf_{site} (
                .I(clks[{idx}]),
                .O({ioclk})
                );'''.format(
                    ioclk=ioclk,
                    site=iob,
                    idx=idx,
                    iostandard=iostd_of[iob],
                ))

        for site, x, y in sites:
            params = {}
            params['tile'] = tile_name
            params['site'] = site
            params['IN_USE'] = random.randint(0, 1)
            params['x'] = x - x_min
            params['y'] = y - y_min

            if params['IN_USE']:
                params['BUFR_DIVIDE'] = random.choice(
                    (
                        '"BYPASS"',
                        '1',
                        '2',
                        '3',
                        '4',
                        '5',
                        '6',
                        '7',
                        '8',
                    ))
                params['I'] = random.choice(ioclks)

                if params['BUFR_DIVIDE'] == '"BYPASS"':
                    params['CE'] = '1'
                    params['CLR'] = '0'
                else:
                    params['CE'] = luts.get_next_output_net()
                    params['CLR'] = luts.get_next_output_net()

                params['consumed'] = 0
                if outs_used < len(iobs_s) and random.randint(0, 1):
                    params['consumed'] = 1
                    params['obuf_site'] = iobs_s[outs_used]
                    params['iostd'] = iostd_of[iobs_s[outs_used]]
                    params['out_idx'] = num_outs
                    num_outs += 1
                    outs_used += 1
                if params['consumed']:
                    outputs.append(
                        '''
    wire {site}_o;
    (* KEEP, DONT_TOUCH, LOC = "{site}" *)
    BUFR #(
        .BUFR_DIVIDE({BUFR_DIVIDE})
        ) buf_{site} (
            .CE({CE}),
            .CLR({CLR}),
            .I({I}),
            .O({site}_o)
        );
    wire {site}_q;
    (* KEEP, DONT_TOUCH *)
    ODDR #(.DDR_CLK_EDGE("SAME_EDGE")) oddr_{site} (
            .C({site}_o), .CE(1'b1), .D1(1'b1), .D2(1'b0), .R(1'b0), .S(1'b0), .Q({site}_q));
    (* KEEP, DONT_TOUCH, LOC = "{obuf_site}" *)
    OBUF #(.IOSTANDARD("{iostd}")) obuf_{site} (.I({site}_q), .O(outs[{out_idx}]));
                        '''.format(**params))
                else:
                    outputs.append(
                        '''
    (* KEEP, DONT_TOUCH, LOC = "{site}" *)
    BUFR #(
        .BUFR_DIVIDE({BUFR_DIVIDE})
        ) buf_{site} (
            .CE({CE}),
            .CLR({CLR}),
            .I({I})
        );
                        '''.format(**params))

            params_list.append(params)

        # --- BUFIO half (extension v4, fully LOC'd) ---
        rec = {}
        rec['tile'] = tile_name
        rec['kind'] = 'BUFIO_TILE'
        rec['bufio_sites'] = [site for site, _, _ in bufio_sites]
        rec['bufio_insts'] = []
        rec['bufio_src'] = {}
        rec['bufio_loc'] = {}
        bys = sorted(by for _, _, by in bufio_sites)
        by_min = bys[0] if bys else 0
        clkout_for_rel_y = {0: 0, 1: 1, 2: 2, 3: 3}
        mmcm_declared = False
        mmcm_name = 'mmcm_{}'.format(tile_name)
        for k, (iob, ioclk) in enumerate(zip(iobs, ioclks)):
            if k >= len(ilogics):
                break
            # A clock-capable IOB feeds ONE dedicated BUFIO of its bank: the k-th
            # clock-capable IOB (by y) of the tile's neighbourhood drives the k-th
            # BUFIO site (by y).  Measured on every HCLK_IOI3 of xc7a100t (24/24,
            # bijective); a CCIO-fed BUFIO LOC'd anywhere else fails placement.
            bufio_by_y = sorted((st for st, _, _ in bufio_sites), key=lambda st: int(st.split('Y')[-1]))
            iobs_by_y = sorted(iobs, key=lambda st: int(st.split('Y')[-1]))
            rank = iobs_by_y.index(iob)
            if rank >= len(bufio_by_y):
                continue
            bsite = bufio_by_y[rank]
            state = random.choice(('unused', 'CCIO', 'MMCM')) if mmcm_site else random.choice(('unused', 'CCIO'))
            if state == 'unused':
                continue
            inst = 'bufio_{}'.format(iob)
            rec['bufio_insts'].append(inst)
            rec['bufio_src'][inst] = state
            rec['bufio_loc'][inst] = bsite
            if state == 'MMCM':
                if not mmcm_declared:
                    mmcm_declared = True
                    outputs.append(
                        '''
    wire {pll}_fb, {pll}_out0, {pll}_out1, {pll}_out2, {pll}_out3;
    (* KEEP, DONT_TOUCH, LOC = "{mmcm_site}" *)
    MMCME2_BASE #(
        .CLKIN1_PERIOD(10.0), .CLKFBOUT_MULT_F(8.0), .DIVCLK_DIVIDE(1),
        .CLKOUT0_DIVIDE_F(8.0), .CLKOUT1_DIVIDE(8), .CLKOUT2_DIVIDE(8), .CLKOUT3_DIVIDE(8)
    ) {pll} (
        .CLKIN1({clkin}), .CLKFBIN({pll}_fb), .CLKFBOUT({pll}_fb),
        .CLKOUT0({pll}_out0), .CLKOUT1({pll}_out1), .CLKOUT2({pll}_out2), .CLKOUT3({pll}_out3),
        .RST(1'b0), .PWRDWN(1'b0)
    );'''.format(pll=mmcm_name, clkin=ioclks[0], mmcm_site=mmcm_site))
                rel_y = int(bsite.split('Y')[-1]) - by_min
                bufio_i = '{}_out{}'.format(mmcm_name, clkout_for_rel_y[rel_y])
            else:
                bufio_i = ioclk
            outputs.append(
                '''
    wire {inst}_o;
    (* KEEP, DONT_TOUCH, LOC = "{bsite}" *)
    BUFIO {inst} (
        .I({bufio_i}),
        .O({inst}_o)
    );
    (* KEEP, DONT_TOUCH, LOC = "{ilogic}" *)
    ISERDESE2 #(
        .DATA_RATE("SDR"),
        .DATA_WIDTH(4),
        .INTERFACE_TYPE("OVERSAMPLE"),
        .IOBDELAY("NONE"),
        .NUM_CE(2),
        .SERDES_MODE("MASTER")
    ) iserdes_{inst} (
        .CLK({inst}_o),
        .CLKB(),
        .CLKDIV(),
        .D(1'b0),
        .DDLY(),
        .OFB(),
        .OCLKB(),
        .RST(),
        .SHIFTIN1(),
        .SHIFTIN2()
    );
                '''.format(inst=inst, bufio_i=bufio_i, bsite=bsite, ilogic=ilogics[k]))
        params_list.append(rec)

    # An output port with no buffer makes Vivado's IO placer fail
    # (one unplaced port and no site left for it). Emit the port only
    # when an OBUF actually drives it. The text matches the historical
    # header whenever that port exists.
    if num_outs == 0:
        ports = "input [%d:0] clks" % (num_clocks - 1)
    else:
        ports = "input [%d:0] clks, output [%d:0] outs" % (
            num_clocks - 1, num_outs - 1)
    print("\nmodule top(%s);\n    " % ports)

    print("""
    (* KEEP, DONT_TOUCH *)
    LUT6 dummy (
        );""")

    for l in luts.create_wires_and_luts():
        print(l)

    for l in outputs:
        print(l)

    print("endmodule")

    with open('params.json', 'w') as f:
        json.dump(params_list, f, indent=2)


if __name__ == '__main__':
    main()
