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

from __future__ import print_function

import fasm
import argparse
import json
import os
import os.path
import csv

from collections import defaultdict

from prjxray import fasm_assembler, util
from prjxray.db import Database
from prjxray.roi import Roi
from prjxray.util import OpenSafeFile

import sys


def eprint(*args, **kwargs):
    print(*args, file=sys.stderr, **kwargs)


class FASMSyntaxError(SyntaxError):
    pass


def dump_frames_verbose(frames):
    print()
    print("Frames: %d" % len(frames))
    for addr in sorted(frames.keys()):
        words = frames[addr]
        print(
            '0x%08X ' % addr + ', '.join(['0x%08X' % w for w in words]) +
            '...')


def dump_frames_sparse(frames):
    print()
    print("Frames: %d" % len(frames))
    for addr in sorted(frames.keys()):
        words = frames[addr]

        # Skip frames without filled words
        for w in words:
            if w:
                break
        else:
            continue

        print('Frame @ 0x%08X' % addr)
        for i, w in enumerate(words):
            if w:
                print('  % 3d: 0x%08X' % (i, w))


def dump_frm(f, frames):
    '''Write a .frm file given a list of frames, each containing a list of 101 32 bit words'''
    for addr in sorted(frames.keys()):
        words = frames[addr]
        f.write(
            '0x%08X ' % addr + ','.join(['0x%08X' % w for w in words]) + '\n')


def find_pudc_b(db):
    """ Find PUDC_B pin func in grid, and return the tile and site prefix.

    The PUDC_B pin is a special 7-series pin that controls unused pin pullup.

    If the PUDC_B is unused, it is configured as an input with a PULLUP.

    """
    grid = db.grid()

    pudc_b_tile_site = None
    for tile in grid.tiles():
        gridinfo = grid.gridinfo_at_tilename(tile)

        for site, pin_function in gridinfo.pin_functions.items():
            if 'PUDC_B' in pin_function:
                assert pudc_b_tile_site == None, (
                    pudc_b_tile_site, (tile, site))
                iob_y = int(site[-1]) % 2

                pudc_b_tile_site = (tile, 'IOB_Y{}'.format(iob_y))

    return pudc_b_tile_site


def get_iob_sites(db, tile_name):
    """
    Yields prjxray site names for given IOB tile name
    """
    grid = db.grid()
    gridinfo = grid.gridinfo_at_tilename(tile_name)

    for site in gridinfo.sites:
        site_y = int(site[-1]) % 2
        yield "IOB_Y{}".format(site_y)


def run(
        db_root,
        part,
        filename_in,
        f_out,
        sparse=False,
        roi=None,
        roi_strict=False,
        debug=False,
        emit_pudc_b_pullup=False):
    if roi_strict and not roi:
        raise ValueError('--roi-strict requires --roi')
    if roi_strict and not sparse:
        # Without --sparse, get_frames() zero-fills EVERY frame on the part
        # (frames_init()), so the ROI never bounds anything and the output is
        # a full-device image whatever the ROI says. Refuse rather than emit
        # 9k frames under a flag whose whole purpose is to bound them.
        raise ValueError('--roi-strict requires --sparse')
    db = Database(db_root, part)
    assembler = fasm_assembler.FasmAssembler(db)

    set_features = set()

    def feature_callback(feature):
        set_features.add(feature)

    assembler.set_feature_callback(feature_callback)

    # Build mapping of tile to IO bank
    tile_to_bank = {}
    bank_to_tile = defaultdict(lambda: set())

    if part is not None:
        with OpenSafeFile(os.path.join(db_root, part, "package_pins.csv"), "r") as fp:
            reader = csv.DictReader(fp)
            package_pins = [l for l in reader]

        with OpenSafeFile(os.path.join(db_root, part, "part.json"), "r") as fp:
            part_data = json.load(fp)

        # The IO-bank "anchor" tile prefix differs per family. kintex7/artix7
        # use HR-bank IOLOGIC and therefore HCLK_IOI3. virtex7 HP-only parts
        # (e.g. xc7vx485tffg1761-2) use HCLK_IOI. Probe the grid for whichever
        # one actually exists; if neither, skip this bank-anchor altogether
        # rather than queueing a KeyError for downstream lookups.
        for bank, loc in part_data["iobanks"].items():
            for prefix in ("HCLK_IOI3_", "HCLK_IOI_"):
                tile = prefix + loc
                try:
                    db.grid().gridinfo_at_tilename(tile)
                except KeyError:
                    continue
                bank_to_tile[bank].add(tile)
                tile_to_bank[tile] = bank
                break

        for pin in package_pins:
            bank_to_tile[pin["bank"]].add(pin["tile"])
            tile_to_bank[pin["tile"]] = pin["bank"]

    # Resolve PUDC_B site unconditionally — we need the tile name later in the
    # HP-bank glue walk to skip it (Vivado does not set IBUF_HP_BANK_GLUE on
    # the PUDC_B tile even though our heuristic would).
    pudc_b_tile_site = find_pudc_b(db) if part is not None else None

    if emit_pudc_b_pullup:
        pudc_b_in_use = False

        def check_for_pudc_b(set_feature):
            feature_callback(set_feature)
            parts = set_feature.feature.split('.')

            if parts[0] == pudc_b_tile_site[0] and parts[
                    1] == pudc_b_tile_site[1]:
                nonlocal pudc_b_in_use
                pudc_b_in_use = True

        if pudc_b_tile_site is not None:
            assembler.set_feature_callback(check_for_pudc_b)

    extra_features = []
    if roi:
        with OpenSafeFile(roi) as f:
            roi_j = json.load(f)
        x1 = roi_j['info']['GRID_X_MIN']
        x2 = roi_j['info']['GRID_X_MAX']
        y1 = roi_j['info']['GRID_Y_MIN']
        y2 = roi_j['info']['GRID_Y_MAX']

        assembler.mark_roi_frames(
            Roi(db=db, x1=x1, x2=x2, y1=y1, y2=y2), strict=roi_strict)

        if 'required_features' in roi_j:
            extra_features = list(
                fasm.parse_fasm_string('\n'.join(roi_j['required_features'])))

    # Get required extra features for the part
    required_features = db.get_required_fasm_features(part)
    extra_features += list(
        fasm.parse_fasm_string('\n'.join(required_features)))

    assembler.parse_fasm_filename(filename_in, extra_features=extra_features)

    if emit_pudc_b_pullup and not pudc_b_in_use and pudc_b_tile_site is not None:
        # Enable IN-only and PULLUP on PUDC_B IOB.
        #
        # The set of IOSTANDARD aliases differs per family because HR-bank
        # (IOB33) silicon supports more standards than HP-bank (IOB18).
        # Pick the right alias by sniffing the tile name; the chosen alias
        # has to match a segbits entry exactly or fasm2frames will error.
        missing_features = []
        is_hp = pudc_b_tile_site[0].startswith('LIOB18') or \
                pudc_b_tile_site[0].startswith('RIOB18')
        if is_hp:
            # HP-bank (virtex7 xc7vx485tffg1761-2) — Vivado emits this exact
            # feature set on the PUDC_B IOB; the bits flow from segbits
            # entries on LVCMOS12_LVCMOS15.IN, LVCMOS12_LVCMOS15_LVCMOS18.IN,
            # the long IN_ONLY alias, two SLEW.SLOW aliases, the STEPDOWN
            # alias, PULLTYPE.PULLUP on Y0, and Y1's default-state SLEW/
            # STEPDOWN/PULLDOWN trio. Cross-referenced with a Vivado-built
            # rst_to_led reference bitstream.
            pudc_template = (
                "{tile}.{site}.LVCMOS12_LVCMOS15.IN\n"
                "{tile}.{site}.LVCMOS12_LVCMOS15_LVCMOS18.IN\n"
                "{tile}.{site}.LVCMOS12_LVCMOS15_LVCMOS18_LVCMOS25_LVCMOS33_LVDS_25_LVTTL_SSTL135_SSTL15_TMDS_33.IN_ONLY\n"
                "{tile}.{site}.LVCMOS12_LVCMOS15_LVCMOS18_LVCMOS25_LVCMOS33_LVTTL_SSTL135_SSTL15.SLEW.SLOW\n"
                "{tile}.{site}.LVCMOS12_LVCMOS15_LVCMOS18.SLEW.SLOW\n"
                "{tile}.{site}.LVCMOS12_LVCMOS15_LVCMOS18_SSTL135_SSTL15.STEPDOWN\n"
                "{tile}.{site}.PULLTYPE.PULLUP\n"
                "{tile}.{partner}.LVCMOS12_LVCMOS15_LVCMOS18.SLEW.SLOW\n"
                "{tile}.{partner}.LVCMOS12_LVCMOS15_LVCMOS18_SSTL135_SSTL15.STEPDOWN\n"
                "{tile}.{partner}.PULLTYPE.PULLDOWN\n"
            )
            partner = 'IOB_Y1' if pudc_b_tile_site[1] == 'IOB_Y0' else 'IOB_Y0'
            for line in fasm.parse_fasm_string(pudc_template.format(
                    tile=pudc_b_tile_site[0],
                    site=pudc_b_tile_site[1],
                    partner=partner)):
                assembler.add_fasm_line(line, missing_features)
        else:
            # HR-bank (artix7/kintex7-HR/zynq7/spartan7) — original 3-line
            # template kept intact. TODO from the original code still
            # applies to K70T: it is known to be wrong but we have no
            # better data here.
            for line in fasm.parse_fasm_string("""
{tile}.{site}.LVCMOS12_LVCMOS15_LVCMOS18_LVCMOS25_LVCMOS33_LVTTL_SSTL135_SSTL15.IN_ONLY
{tile}.{site}.LVCMOS25_LVCMOS33_LVTTL.IN
{tile}.{site}.PULLTYPE.PULLUP
""".format(
                    tile=pudc_b_tile_site[0],
                    site=pudc_b_tile_site[1],
            )):
                assembler.add_fasm_line(line, missing_features)

        if missing_features:
            raise fasm_assembler.FasmLookupError('\n'.join(missing_features))

    if part is not None:
        # Make a set of all used IOB tiles and sites. Look for the "STEPDOWN"
        # feature. If one is set for an IOB then set it for all other IOBs of
        # the same bank.
        stepdown_tags = defaultdict(lambda: set())
        stepdown_banks = set()
        used_iob_sites = set()

        for set_feature in set_features:
            if set_feature.value == 0:
                continue

            feature = set_feature.feature
            parts = feature.split(".")
            if len(parts) >= 3:
                tile, site, tag = feature.split(".", maxsplit=2)
                if "IOB33" in tile:
                    used_iob_sites.add((
                        tile,
                        site,
                    ))

                # Store STEPDOWN related tags.
                if "STEPDOWN" in tag:
                    bank = tile_to_bank[tile]
                    stepdown_banks.add(bank)
                    stepdown_tags[bank].add(tag)

        # Set the feature for unused IOBs, loop over all banks which were
        # observed to have the STEPDOWN feature set.
        missing_features = []

        for bank in stepdown_banks:
            for tile in bank_to_tile[bank]:

                # This is an IOB33 tile. Set the STEPDOWN feature in it but
                # only if it is unused.
                if "IOB33" in tile:
                    for site in get_iob_sites(db, tile):

                        if (tile, site) in used_iob_sites:
                            continue

                        for tag in stepdown_tags[bank]:
                            feature = "{}.{}.{}".format(tile, site, tag)
                            for line in fasm.parse_fasm_string(feature):
                                assembler.add_fasm_line(line, missing_features)

                # On HR-bank parts the bank-anchor tile is HCLK_IOI3 and gets a
                # STEPDOWN feature; on virtex7 HP-only it is HCLK_IOI which has
                # the same STEPDOWN feature in our segbits.
                if "HCLK_IOI3" in tile or tile.startswith("HCLK_IOI_"):
                    feature = "{}.STEPDOWN".format(tile)
                    for line in fasm.parse_fasm_string(feature):
                        assembler.add_fasm_line(line, missing_features)

        if missing_features:
            raise fasm_assembler.FasmLookupError('\n'.join(missing_features))

        # HP-bank (LIOB18/RIOB18) "bank-glue" auto-injection.
        # The HP-class output buffer (OUTBUF_DCIEN) needs three driver-enable
        # bits, and the HP-class input buffer (INBUF_DCIEN) needs one input-
        # enable bit. Vivado emits these implicitly for every used HP-bank
        # IOB; on HR-bank IOB33 the equivalent bits are factory-default so
        # were never captured in upstream prjxray. For round-trip parity on
        # virtex7 xc7vx485tffg1761-2 (HP-only) we inject the right tag
        # whenever any IOB feature is set on an HP-bank tile, mirroring the
        # STEPDOWN walk above.
        glue_missing = []
        liob_in_features = defaultdict(lambda: {'in': False, 'out': False, 'diff_in': False})
        for set_feature in set_features:
            if set_feature.value == 0:
                continue
            feature = set_feature.feature
            parts = feature.split(".")
            if len(parts) < 3:
                continue
            tile = parts[0]
            site = parts[1]
            tag  = ".".join(parts[2:])
            if not (tile.startswith("LIOB18") or tile.startswith("RIOB18")):
                continue
            if site not in ("IOB_Y0", "IOB_Y1"):
                continue
            # Direction heuristic. Pure ".IN" / ".IN_ONLY" indicates IBUF.
            # ".DRIVE." is the only reliable OBUF marker — output drive
            # strength is meaningless for inputs. ".SLEW." and ".OUT" appear
            # on IBUF tiles too as bank-wide defaults (Vivado emits
            # SLEW.SLOW on every unused IOB), so they would mis-classify
            # IBUFs as IOBUFs and inject spurious OBUF_HP_BANK_GLUE bits.
            # ".IN_DIFF" indicates an IBUFDS (LVDS differential input).
            if "IN_DIFF" in tag:
                liob_in_features[(tile, site)]['diff_in'] = True
            elif "IN_ONLY" in tag or tag.endswith(".IN") or "IBUFDISABLE" in tag:
                liob_in_features[(tile, site)]['in'] = True
            if ".DRIVE." in tag:
                liob_in_features[(tile, site)]['out'] = True

        # Track whether the col-32 LIOB18 column has any Y1 OBUFs anywhere.
        # When it does, Vivado lights a 6-bit "DCI cascade + bank active"
        # pattern in INT_L_X32Y49 — the bottom-most INT_L tile of the X32
        # routing spine that serves the LIOB18 column. The same indicator
        # exists for the X110 (R-side IOB) column too, mirrored at
        # INT_R_X110Y49 — we'd add that when the first RIOB18 test design
        # asks for it.
        any_liob18_y1_obuf = False
        # PUDC_B's tile already gets its own IN/IN_ONLY/PULLUP/SLEW features
        # injected above; do NOT also fire the IBUF_HP_BANK_GLUE rule on it.
        # Empirically (Vivado-built rst_to_led ref) the PUDC_B tile does not
        # carry the bank-glue bit because the pullup is a "virtual" tie,
        # not a real IBUF placement.
        pudc_b_tile_name = pudc_b_tile_site[0] if pudc_b_tile_site else None
        for (tile, site), kind in liob_in_features.items():
            if tile == pudc_b_tile_name:
                continue
            # Both IOB_Y0 (master) and IOB_Y1 (slave) sites have HP_BANK_GLUE
            # patterns now. The Y1 OBUF rule was derived from the counter
            # design (LIOB18 tiles with two driving OBUFs) — Vivado emits an
            # extra 3 bits at rel 30_041 / 32_016 / 33_061 when Y1 has a
            # ".DRIVE." feature in addition to Y0.
            if kind['in']:
                feature = "{}.{}.IBUF_HP_BANK_GLUE".format(tile, site)
                for line in fasm.parse_fasm_string(feature):
                    assembler.add_fasm_line(line, glue_missing)
            if kind['out']:
                feature = "{}.{}.OBUF_HP_BANK_GLUE".format(tile, site)
                for line in fasm.parse_fasm_string(feature):
                    assembler.add_fasm_line(line, glue_missing)
            if kind['diff_in']:
                # IBUFDS — empirically Vivado lights a 7-bit pattern in the
                # IOB_Y0 (M-site) slot of the differential pair. Y1 is the
                # negative-half slave and reuses the silicon defaults, so
                # no separate Y1 rule is needed.
                feature = "{}.IOB_Y0.IBUFDS_BANK_GLUE".format(tile)
                for line in fasm.parse_fasm_string(feature):
                    assembler.add_fasm_line(line, glue_missing)
            if kind['out'] and site == 'IOB_Y1' and tile.startswith('LIOB18_X81'):
                any_liob18_y1_obuf = True

        if any_liob18_y1_obuf:
            for tag in ('INT_L_X32Y49.IOB_COL_OBUF_CASCADE_Y1',
                        'INT_L_X32Y49.IOB_COL_BANK_ACTIVE'):
                for line in fasm.parse_fasm_string(tag):
                    assembler.add_fasm_line(line, glue_missing)

        # GFAN0 T-tie root glue. When the design has an OBUF whose T
        # input must be tied to GND through general routing (e.g. an
        # IBUF→OBUF passthrough where no SLICE FF Q drives the OBUF),
        # Vivado emits INT_L_X62Y73.GFAN0.GND_WIRE / .IMUX_L33.GFAN0
        # FASM features AND lights one extra silicon bit at
        # INT_L_X62Y83 minor 26 bit 18 — the "this GFAN tie route is
        # in use" marker for the col-62 routing spine. Only the c=62
        # column has been observed needing this glue (the counter
        # designs use FF-driven OBUFs and route GFAN0 through other
        # columns without triggering an analogous bit). The rule fires
        # only when an X62-column GFAN0.GND_WIRE feature appears.
        x62_gfan_tile = None
        for set_feature in set_features:
            if set_feature.value == 0:
                continue
            feat = set_feature.feature
            if not feat.startswith('INT_L_X62'):
                continue
            if 'GFAN0.GND_WIRE' in feat:
                # The mirror tile (Y+10) gets the glue bit. For
                # INT_L_X62Y73 → INT_L_X62Y83.
                tile = feat.split('.')[0]
                import re as _re
                m = _re.match(r'(INT_L_X(\d+))Y(\d+)', tile)
                if m:
                    x62_gfan_tile = 'INT_L_X{}Y{}'.format(m.group(2), int(m.group(3)) + 10)
                break

        if x62_gfan_tile:
            feature = '{}.GFAN_TIE_ROOT_GLUE'.format(x62_gfan_tile)
            for line in fasm.parse_fasm_string(feature):
                assembler.add_fasm_line(line, glue_missing)

        # HCLK_L per-BUFRCLK-channel "active" markers. Empirically Vivado
        # sets one extra bit per HCLK_L tile per BUFR channel in use, at
        # minor 1 bit 28+ch. Only BUFRCLK3 is observed today (the
        # counter_sw_bufr design); when a kintex7 → virtex7 comparison
        # design that uses BUFRCLK0/1/2 is built we can add those analogues
        # to segbits_hclk_l.db with bits 01_28 / 01_29 / 01_30 respectively.
        hclkl_bufrclk_seen = {}  # (hclkl_tile) -> set of channel-ids in use
        for set_feature in set_features:
            if set_feature.value == 0:
                continue
            feat = set_feature.feature
            parts = feat.split('.')
            if len(parts) < 3:
                continue
            if not parts[0].startswith('HCLK_L'):
                continue
            # HCLK_L_X..Y...HCLK_LEAF_CLK_B_TOP/BOTL[0-5].HCLK_CK_BUFRCLK[0-3]
            tag = '.'.join(parts[2:])
            if 'HCLK_CK_BUFRCLK' not in tag:
                continue
            ch = tag.rsplit('HCLK_CK_BUFRCLK', 1)[1]
            if ch.isdigit():
                hclkl_bufrclk_seen.setdefault(parts[0], set()).add(int(ch))

        for hclkl_tile, channels in hclkl_bufrclk_seen.items():
            for ch in channels:
                if ch != 3:
                    # Only BUFRCLK3 has segbits data so far; skip rest
                    # silently so the partial rule doesn't error.
                    continue
                feature = '{}.HCLK_LEAF_BUFRCLK{}_ACTIVE'.format(hclkl_tile, ch)
                for line in fasm.parse_fasm_string(feature):
                    assembler.add_fasm_line(line, glue_missing)

        # glue_missing entries indicate the segbits don't carry the rule yet
        # (e.g. partially-built database); fall through silently rather than
        # erroring, so this auto-injection is a no-op on databases that
        # haven't been bank-glue-annotated.

    frames = assembler.get_frames(sparse=sparse)

    if debug:
        dump_frames_sparse(frames)

    dump_frm(f_out, frames)


def main():
    parser = argparse.ArgumentParser(
        description=
        'Convert FPGA configuration description ("FPGA assembly") into binary frame equivalent'
    )

    util.db_root_arg(parser)
    util.part_arg(parser)
    parser.add_argument(
        '--sparse', action='store_true', help="Don't zero fill all frames")
    parser.add_argument(
        '--roi',
        help="ROI design.json file defining which tiles are within the ROI. "
        "PERMISSIVE: the ROI's frames are added to the emitted set, and any "
        "tile a FASM feature touches adds its frames too, so the result is a "
        "UNION of the two. Correct when the device model has already been "
        "filtered to the region. See --roi-strict for the full-device case.")
    parser.add_argument(
        '--roi-strict',
        dest='roi_strict',
        action='store_true',
        help="Treat --roi as a FILTER rather than a union: a FASM feature "
        "naming a tile outside the ROI is a hard error (non-zero exit) "
        "identifying the feature and its tile, and the emitted frame set is "
        "asserted to be exactly the ROI's. Requires --roi and --sparse. Use "
        "this whenever the device model is the full device, where one stray "
        "feature pulls that tile's whole frame column into a 'partial' "
        "bitstream and writing it destroys the static logic there.")
    parser.add_argument(
        '--emit_pudc_b_pullup',
        help="Emit an IBUF and PULLUP on the PUDC_B pin if unused",
        action='store_true')
    parser.add_argument(
        '--debug', action='store_true', help="Print debug dump")
    parser.add_argument('fn_in', help='Input FPGA assembly (.fasm) file')
    parser.add_argument(
        'fn_out',
        default=None,
        nargs='?',
        help='Output FPGA frame (.frm) file (default: stdout)')

    args = parser.parse_args()

    f_out = open(args.fn_out, 'w') if args.fn_out else sys.stdout

    def discard_output():
        # open(..., 'w') truncated fn_out before run() was ever entered, so any
        # exit that is not a clean return leaves a zero-length .frm on disk.
        # xc7frames2bit reads one at rc 0: measured 2026-09-01 on
        # xc7vx485tffg1761-2, a 0-byte .frm produced a 20,273,632-byte .bit of
        # which 861 bytes are non-zero (sync word, header, commands, CRC) and
        # the tool reported "--verify round-trip OK, 50148 frames, ECC
        # included". A failed run must leave nothing the next stage can consume.
        if not args.fn_out:
            return
        try:
            f_out.close()
        except OSError:
            pass
        try:
            os.remove(args.fn_out)
        except OSError:
            pass

    try:
        run(
            db_root=args.db_root,
            part=args.part,
            filename_in=args.fn_in,
            f_out=f_out,
            sparse=args.sparse,
            roi=args.roi,
            roi_strict=args.roi_strict,
            debug=args.debug,
            emit_pudc_b_pullup=args.emit_pudc_b_pullup)
        # Closed inside the try so that a write that fails only at flush time --
        # ENOSPC is the realistic one -- is a failed run and gets the same
        # treatment as any other, rather than a rc 0 beside a truncated file.
        if args.fn_out:
            f_out.close()
    except (fasm_assembler.FasmRoiViolation, fasm_assembler.FasmLookupError,
            fasm_assembler.FasmInconsistentBits, ValueError) as e:
        # Exit 1 with the diagnostic on stderr rather than a traceback: this
        # is bad input, not a crash, and the calling flow needs a status code
        # it can branch on.
        #
        # FasmLookupError and FasmInconsistentBits are named here because they
        # were not: both subclass Exception (fasm_assembler.py:17,21), so the
        # pair (FasmRoiViolation, ValueError) let them escape as an uncaught
        # traceback -- past the cleanup below, which is why the 0-byte .frm
        # described in discard_output() was reachable at all. FasmLookupError
        # is what every virtex7 CFG_CENTER_MID feature raises today.
        discard_output()
        eprint('ERROR: %s' % e)
        return 1
    except BaseException:
        # Anything else -- a genuine crash, KeyboardInterrupt, a class added to
        # fasm_assembler after this line was written. The traceback is still
        # the report, so it is re-raised unchanged; what must not survive is
        # the artefact. Naming exception classes one at a time is how the
        # previous version of this handler went stale.
        discard_output()
        raise
    return 0


if __name__ == '__main__':
    sys.exit(main())
