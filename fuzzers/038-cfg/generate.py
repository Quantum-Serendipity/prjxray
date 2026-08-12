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

from prjxray.segmaker import Segmaker
from prjxray import presence
from prjxray import verilog
from prjxray import segmaker


def bitfilter(frame, word):
    if frame < 26:
        return False
    return True


def run():

    segmk = Segmaker("design.bits")

    print("Loading tags")
    f = open('params.jl', 'r')
    design = json.load(f)

    # PRESENCE.  One tag per ICAP site saying whether this specimen
    # instantiated it.  Both sites are candidates and their presence varies
    # independently across the specimen set, which is what gives segmatch the
    # count0 > 0 it needs; see prjxray/presence.py for why nothing else does.
    icap_present = {
        p["site"]: bool(p["params"]["ENABLED"])
        for p in design if p["site_type"] == "ICAP"
    }
    presence.add_presence_tags(segmk, icap_present)

    # ICAP_WIDTH is a tile-level field (prjcombine MISC_CFG.ICAP_WIDTH) and its
    # prjxray tag name, CFG_CENTER_MID.ICAP.ICAP_WIDTH_*, is the same whichever
    # ICAP site it is added through -- Segmaker's sitekey for both is `ICAP`.
    # Emit it from exactly one instantiated site so the value cannot depend on
    # which of two identical writes lands last, and from none at all when no
    # ICAP is instantiated, since then the field has no meaning to record.
    width_carrier = min([s for s, on in icap_present.items() if on],
                        default=None)

    for p in design:
        ps = p["params"]
        if p["site_type"] == "ICAP":
            if p["site"] != width_carrier:
                continue
            param = verilog.unquote(ps["ICAP_WIDTH"])
            segmaker.add_site_group_zero(
                segmk, p["site"], "ICAP_WIDTH_", ["X32", "X8", "X16"], "X32",
                param)
        elif p["site_type"] in "BSCAN":
            param = str(ps["JTAG_CHAIN"])
            segmaker.add_site_group_zero(
                segmk, p["site"], "JTAG_CHAIN_", ["1", "2", "3", "4"], param,
                param)
        elif p["site_type"] in "CAPTURE":
            param = verilog.unquote(ps["ONESHOT"])
            segmk.add_site_tag(
                p["site"], "ONESHOT", True if param in "TRUE" else False)
        elif p["site_type"] in "STARTUP":
            param = verilog.unquote(ps["PROG_USR"])
            segmk.add_site_tag(
                p["site"], "PROG_USR", True if param in "TRUE" else False)
        elif p["site_type"] in "FRAME_ECC":
            param = verilog.unquote(ps["FARSRC"])
            segmaker.add_site_group_zero(
                segmk, p["site"], "FARSRC_", ["FAR", "EFAR"], param, param)
        elif p["site_type"] in ["USR_ACCESS", "DCIRESET"]:
            feature = "ENABLED"
            segmk.add_site_tag(
                p["site"], feature, True if ps["ENABLED"] else False)

    segmk.compile(bitfilter=bitfilter)
    segmk.write()


run()
