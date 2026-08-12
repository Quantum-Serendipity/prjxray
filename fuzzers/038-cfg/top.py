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
import io
import os
import random
import re
random.seed(int(os.getenv("SEED"), 16))
from prjxray import util
from prjxray import lut_maker
from prjxray import presence
from prjxray import verilog
from prjxray.db import Database

SITE_TYPES = [
    'BSCAN', 'CAPTURE', 'ICAP', 'USR_ACCESS', 'STARTUP', 'FRAME_ECC',
    'DCIRESET'
]


def gen_sites():
    db = Database(util.get_db_root(), util.get_part())
    grid = db.grid()
    for tile_name in sorted(grid.tiles()):
        loc = grid.loc_of_tilename(tile_name)
        gridinfo = grid.gridinfo_at_loc(loc)

        for site_name, site_type in gridinfo.sites.items():
            # This used to read
            #     if site_name not in 'ICAP_X0Y0':
            # -- Python's SUBSTRING operator against a string literal, where
            # `!=` was meant.  For the site names this device actually has the
            # two spellings agree exactly (only 'ICAP_X0Y0' is a substring of
            # 'ICAP_X0Y0'), so replacing `not in` with `!=` fixes the reading
            # and changes NOTHING: either way ICAP_X0Y0 was dropped from every
            # specimen, and ICAP_X0Y1 -- the only ICAP left -- was instantiated
            # in every specimen because `icap_already_on` was never set.  Both
            # ICAP sites were invisible to segmatch, for two different reasons:
            # count1 == 0 for one, count0 == 0 for the other.
            #
            # The cure is to drop the exclusion outright and vary the presence
            # of each ICAP site independently across the specimen set.  See
            # prjxray/presence.py.
            if site_type in SITE_TYPES:
                yield site_name, site_type


def write_csv_params(params):
    pinstr = 'tile,site,\n'
    for vals in params:
        pinstr += ','.join(map(str, vals)) + '\n'

    open('params.csv', 'w').write(pinstr)


def icap_placement_legal(plan):
    """Which ICAP presence combinations 7-series will actually build.

    Measured, not assumed.  A specimen holding only ICAP_X0Y0 fails placement:

        ERROR: [Place 30-99] Placer failed with error: 'The only ICAP instance
        cannot be placed in the bottom ICAP site.  Please unplaced the
        instance, or lock it in the top site.'

    ICAP_X0Y0 is the bottom site and is reachable only alongside ICAP_X0Y1.
    That leaves three combinations rather than four -- {}, {X0Y1}, {X0Y0,X0Y1}
    -- which is still two-sided support for both sites, and with different
    patterns, so both remain separable:

        ICAP_X0Y0   present in 1 of 3, absent in 2 of 3
        ICAP_X0Y1   present in 2 of 3, absent in 1 of 3
    """
    return plan.get('ICAP_X0Y1', True) or not plan.get('ICAP_X0Y0', False)


def generate_params():
    bscan_already_on = False
    tile_params = []
    sites = sorted(gen_sites())

    # The presence-varying half of the specimen.  A factorial over the ICAP
    # sites, indexed by specimen number, so that each site is instantiated in
    # some specimens and not others and the two sites' patterns differ --
    # count1 > 0 and count0 > 0 by construction rather than by luck.
    icap_sites = [s for s, t in sites if t == 'ICAP']
    icap_present = presence.factorial(
        icap_sites, presence.specimen_index(), valid=icap_placement_legal)

    # ICAP_WIDTH is a property of the TILE, not of an ICAP site: prjcombine
    # models it as MISC_CFG.ICAP_WIDTH, one 6-bit field for the whole CFG tile,
    # and prjxray's own db has one CFG_CENTER_MID.ICAP.ICAP_WIDTH_* per value
    # rather than one per site.  Two instantiated ICAPE2s with different widths
    # would therefore be a self-contradictory specimen, so the width is drawn
    # once and shared.
    icap_width = verilog.quote(random.choice(["X32", "X8", "X16"]))

    for loci, (site, site_type) in enumerate(sites):
        p = {}
        # `site_type in "ICAP"` etc. below were substring tests too.  They are
        # accidentally correct for the seven site types this fuzzer handles and
        # are spelled `==` here so the next reader does not have to re-derive
        # that, and so the one that was NOT accidentally correct does not look
        # like the house style.
        if site_type == "ICAP":
            p["ENABLED"] = int(icap_present[site])
            p["ICAP_WIDTH"] = icap_width
        elif site_type == "BSCAN" and not bscan_already_on:
            p["JTAG_CHAIN"] = random.randint(1, 4)
            bscan_already_on = True
        elif site_type == "CAPTURE":
            p["ONESHOT"] = verilog.quote(random.choice(["TRUE", "FALSE"]))
        elif site_type == "STARTUP":
            p["PROG_USR"] = verilog.quote(random.choice(["TRUE", "FALSE"]))
        elif site_type == "FRAME_ECC":
            p["FARSRC"] = verilog.quote(random.choice(["FAR", "EFAR"]))
        elif site_type in [
                "DCIRESET", "USR_ACCESS"
        ]:  #The primitives from these sites have no parameters
            p["ENABLED"] = random.randint(0, 1)
        else:
            continue
        p["LOC"] = verilog.quote(site)
        tile_params.append(
            {
                "site": site,
                "site_type": site_type,
                "module": "mod_{}".format(site_type),
                "params": p
            })
    return tile_params


def generate_netlist(params):
    DUTN = len(params)
    DIN_N = DUTN * 32
    DOUT_N = DUTN * 32

    string_output = io.StringIO()
    any_bscan = False
    any_icap = False
    usr_access_on = False
    capture_on = False
    startup_on = False
    frame_ecc_on = False
    dcireset_on = False
    luts = lut_maker.LutMaker()
    verilog.top_harness(DIN_N, DOUT_N)
    print(
        '''
module roi(input clk, input [%d:0] din, output [%d:0] dout);''' %
        (DIN_N - 1, DOUT_N - 1))
    for loci, param in enumerate(params):
        ports = {
            'din': 'din[{} +: 8]'.format(8 * loci),
            'dout': 'dout[{} +: 8]'.format(8 * loci),
            'clk': 'clk'
        }
        if param["site_type"] == "BSCAN":
            ports = {
                'din':
                '{{din[{} +: 7],{}}}'.format(
                    8 * loci + 1, luts.get_next_output_net()),
                'dout':
                '{{dout[{} +: 7],{}}}'.format(
                    8 * loci + 1, luts.get_next_input_net()),
                'clk':
                'clk'
            }
            any_bscan = True
        elif param["site_type"] == "ICAP":
            # The presence-varying arm.  Same shape as DCIRESET/USR_ACCESS
            # below, which is the one place prjxray already had this pattern
            # and the one CFG enable bit it ever solved.
            if not param["params"]["ENABLED"]:
                continue
            any_icap = True
        elif param["site_type"] in ["CAPTURE"]:
            capture_on = True
        elif param["site_type"] in ["STARTUP"]:
            startup_on = True
        elif param["site_type"] in ["FRAME_ECC"]:
            frame_ecc_on = True
        elif param["site_type"] in ["USR_ACCESS", "DCIRESET"]:
            if not param["params"]["ENABLED"]:
                continue
            if param["site_type"] in ["DCIRESET"]:
                dcireset_on = True
            else:
                usr_access_on = True
        else:
            continue
        verilog.instance(
            param["module"],
            "inst_{}".format(param["site"]),
            ports,
            param["params"],
            string_buffer=string_output)

    #Generate LUTs
    for l in luts.create_wires_and_luts():
        print(l)
    print(string_output.getvalue())

    print(
        '''
endmodule

// ---------------------------------------------------------------------''')
    if any_icap:
        print(
            '''
module mod_ICAP (input [7:0] din, output [7:0] dout, input clk);
    parameter ICAP_WIDTH = "X32";
    parameter LOC = "ICAP_X0Y0";
    // Declared, unused: verilog.instance() passes every entry of params, and
    // ENABLED is what decides whether this module is instantiated at all.
    parameter ENABLED = 1;

    wire [23:0] icap_out;
    (* KEEP, DONT_TOUCH, LOC=LOC *)
    ICAPE2 #(
    .ICAP_WIDTH(ICAP_WIDTH),
    .SIM_CFG_FILE_NAME("NONE")
    )
    ICAPE2_inst (
    .O({icap_out, dout}),
    .CLK(clk),
    .CSIB(),
    .I({24'd0, din}),
    .RDWRB()
    );
endmodule
''')

    if capture_on:
        print(
            '''
module mod_CAPTURE (input [7:0] din, output [7:0] dout, input clk);
    parameter ONESHOT ="TRUE";
    parameter LOC = "ICAP_X0Y0";
    (* KEEP, DONT_TOUCH, LOC=LOC *)
    CAPTUREE2 #(
    .ONESHOT(ONESHOT) // Specifies the procedure for performing single readback per CAP trigger.
    )
    CAPTUREE2_inst (
    .CAP(1'b0),
    .CLK(clk)
    );
endmodule
''')

    if usr_access_on:
        print(
            '''
module mod_USR_ACCESS (input [7:0] din, output [7:0] dout, input clk);
    parameter ENABLED = 1;
    parameter LOC = "USR_ACCESS_X0Y0";

    wire [23:0] usr_access_wire;

    (* KEEP, DONT_TOUCH, LOC=LOC *)
    USR_ACCESSE2 USR_ACCESSE2_inst (
    .CFGCLK(),
    .DATA({usr_access_wire, dout}),
    .DATAVALID()
    );
endmodule
''')

    if any_bscan:
        print(
            '''
module mod_BSCAN (input [7:0] din, output [7:0] dout, input clk);
    parameter JTAG_CHAIN  = 1;
    parameter LOC = "BSCAN_X0Y0";

    (* KEEP, DONT_TOUCH, LOC=LOC *)
    BSCANE2 #(
    .JTAG_CHAIN(JTAG_CHAIN)
    )
    dut (
    .CAPTURE(),
    .DRCK(),
    .RESET(),
    .RUNTEST(),
    .SEL(),
    .SHIFT(),
    .TCK(),
    .TDI(dout[0]),
    .TMS(),
    .UPDATE(),
    .TDO(din[0])
    );
endmodule
''')

    if startup_on:
        print(
            '''
module mod_STARTUP (input [7:0] din, output [7:0] dout, input clk);
    parameter LOC = "STARTUP_X0Y0";
    parameter PROG_USR = "FALSE";

    (* KEEP, DONT_TOUCH, LOC=LOC *)
    STARTUPE2 #(
    .PROG_USR(PROG_USR), // Activate program event security feature. Requires encrypted bitstreams.
    .SIM_CCLK_FREQ(0.0) // Set the Configuration Clock Frequency(ns) for simulation.
    )
    STARTUPE2_inst (
    .CFGCLK(),
    .CFGMCLK(),
    .EOS(),
    .PREQ(dout[0]),
    .CLK(clk),
    .GSR(),
    .GTS(),
    .KEYCLEARB(),
    .PACK(),
    .USRCCLKO(),
    .USRCCLKTS(),
    .USRDONEO(),
    .USRDONETS()
    );
endmodule
''')

    if frame_ecc_on:
        print(
            '''
module mod_FRAME_ECC (input [7:0] din, output [7:0] dout, input clk);
    parameter LOC = "FRAME_ECC_X0Y0";
    parameter FARSRC = "EFAR";

    wire [25:0] far_wire;
    assign dout[7:0] = far_wire[7:0];
    (* KEEP, DONT_TOUCH, LOC=LOC *)
    FRAME_ECCE2 #(
    .FARSRC(FARSRC),
    .FRAME_RBT_IN_FILENAME("NONE")
    )
    FRAME_ECCE2_inst (
    .CRCERROR(),
    .ECCERROR(),
    .ECCERRORSINGLE(),
    .FAR(far_wire),
    .SYNBIT(),
    .SYNDROME(),
    .SYNDROMEVALID(),
    .SYNWORD()
    );
endmodule
''')

    if dcireset_on:
        print(
            '''
module mod_DCIRESET (input [7:0] din, output [7:0] dout, input clk);
    parameter LOC = "FRAME_ECC_X0Y0";
    parameter ENABLED = 1;

    (* KEEP, DONT_TOUCH, LOC=LOC *)
    DCIRESET DCIRESET_inst (
    .LOCKED(dout[0]),
    .RST(dout[1])
);
endmodule
''')


def run():
    params = generate_params()
    generate_netlist(params)
    with open('params.jl', 'w') as f:
        json.dump(params, f, indent=2)


if __name__ == '__main__':
    run()
