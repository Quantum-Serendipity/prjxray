#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# Copyright (C) 2017-2022  The Project X-Ray Authors.
#
# Use of this source code is governed by a ISC-style
# license that can be found in the LICENSE file or at
# https://opensource.org/licenses/ISC
"""
Fallback for SING IOB DRIVE/STEPDOWN: the *_SING tiles have only ~28 IOB18
sites on the whole device, too few for segmatch to isolate the multi-bit DRIVE
enum (the OUT bit stays <N candidates>).  The SING IOB18 is the *same* IOB18
primitive as the regular LIOB18/RIOB18 tiles, and the SING fuzz that DID
resolve (IN/SLEW/PULLTYPE, and the DRIVE candidate bits 38_32/38_34/39_23/39_55)
matches the regular tile bit-for-bit.  So derive the unresolved SING features
from the REAL virtex7 LIOB18/RIOB18 measurements (origin 030-iob18) rather than
leave them hand-copied/absent.

Emits, on stdout, the requested feature group(s) for the *_SING tile type,
copied from the same feature on the regular tile.  Intended to be appended to
the per-side SING db before pushdb, for ONLY the features the SING fuzz could
not solve (default: DRIVE and STEPDOWN).
"""
import argparse


def load(fn):
    d = {}
    for line in open(fn):
        line = line.strip()
        if not line:
            continue
        tag, _, bits = line.partition(' ')
        d[tag] = bits
    return d


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--src-db', required=True, help='segbits_liob18.db (real)')
    ap.add_argument('--src-prefix', default='LIOB18')
    ap.add_argument('--dst-prefix', default='LIOB18_SING')
    ap.add_argument(
        '--groups', nargs='+', default=['DRIVE', 'STEPDOWN'],
        help='feature groups to derive (substring match after the site)')
    args = ap.parse_args()

    src = load(args.src_db)
    for tag, bits in sorted(src.items()):
        if not tag.startswith(args.src_prefix + '.'):
            continue
        # tag = PREFIX.IOB_Yn.<iostds>.<group>[.<enum>]
        rest = tag[len(args.src_prefix) + 1:]
        parts = rest.split('.')
        if len(parts) < 3:
            continue
        group = parts[2]
        if group not in args.groups:
            continue
        print('{}.{} {}'.format(args.dst_prefix, rest, bits))


if __name__ == '__main__':
    main()
