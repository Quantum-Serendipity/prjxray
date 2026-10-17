#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# Copyright (C) 2026  The Project X-Ray Authors.
#
# Use of this source code is governed by a ISC-style
# license that can be found in the LICENSE file or at
# https://opensource.org/licenses/ISC
#
# SPDX-License-Identifier: ISC

import argparse
import collections
import os
import sys

import yaml

from prjxray import bitstream
from prjxray import cross_tile_features
from prjxray.db import Database
from prjxray.tile_segbits_alias import TileSegbitsAlias


def fabric_parts(db_root):
    if not os.path.isfile(os.path.join(db_root, 'mapping', 'parts.yaml')):
        return []
    with open(os.path.join(db_root, 'mapping', 'parts.yaml')) as f:
        parts = yaml.safe_load(f)
    with open(os.path.join(db_root, 'mapping', 'devices.yaml')) as f:
        devices = yaml.safe_load(f)

    chosen = {}
    for part in sorted(parts):
        device = parts[part]['device']
        if device not in devices:
            continue
        fabric = devices[device]['fabric']
        if not os.path.isfile(os.path.join(db_root, fabric, 'tilegrid.json')):
            continue
        chosen.setdefault(fabric, part)
    return sorted(chosen.items())


def tile_features(segbits):
    if isinstance(segbits, TileSegbitsAlias):
        names = [
            segbits.map_feature_from_segbits(name)
            for block in segbits.tile_segbits.segbits.values()
            for name in block
        ]
    else:
        names = [name for block in segbits.segbits.values() for name in block]

    for name in sorted(set(names)):
        yield name.split('.', 1)[1]


def in_window(bits, bit):
    word = bit.word_bit // bitstream.WORD_SIZE_BITS
    return (
        bits.base_address <= bit.word_column < bits.base_address + bits.frames
        and bits.offset <= word < bits.offset + bits.words)


def feature_bits(segbits, gridinfo, feature):
    sidx = feature.rfind('[')
    if sidx != -1 and feature.endswith(']'):
        return list(
            segbits.feature_to_bits(
                gridinfo.bits, '{}.{}'.format(
                    gridinfo.tile_type, feature[:sidx]),
                int(feature[sidx + 1:-1])))
    return list(
        segbits.feature_to_bits(
            gridinfo.bits, '{}.{}'.format(gridinfo.tile_type, feature)))


def census_fabric(db_root, part):
    db = Database(db_root, part)
    grid = db.grid()

    groups = collections.defaultdict(list)
    for tile in sorted(grid.tiles()):
        gridinfo = grid.gridinfo_at_tilename(tile)
        if not gridinfo.bits:
            continue
        if db.tile_types.get(gridinfo.tile_type) is None:
            continue
        key = (
            gridinfo.tile_type,
            tuple(
                sorted(
                    (bt.value, bits.frames, bits.offset, bits.words,
                     bits.alias is not None)
                    for bt, bits in gridinfo.bits.items())))
        groups[key].append(tile)

    crossings = collections.Counter()
    orphans = collections.Counter()
    for key, tiles in sorted(groups.items()):
        segbits = grid.get_tile_segbits_at_tilename(tiles[0])
        outside = []
        gridinfo = grid.gridinfo_at_tilename(tiles[0])
        for feature in tile_features(segbits):
            try:
                bits = feature_bits(segbits, gridinfo, feature)
            except KeyError:
                continue
            if any(bit.isset and not in_window(gridinfo.bits[bt], bit)
                   for bt, bit in bits):
                outside.append(feature)

        for tile in tiles:
            gridinfo = grid.gridinfo_at_tilename(tile)
            segbits = grid.get_tile_segbits_at_tilename(tile)
            for feature in outside:
                for bt, bit in feature_bits(segbits, gridinfo, feature):
                    if not bit.isset or in_window(gridinfo.bits[bt], bit):
                        continue
                    hosts = frozenset(
                        grid.gridinfo_at_tilename(host).tile_type
                        for host in grid.tiles_containing(
                            bt, bit.word_column, bit.word_bit //
                            bitstream.WORD_SIZE_BITS) if host != tile)
                    if hosts:
                        crossings[(gridinfo.tile_type, feature, hosts)] += 1
                    else:
                        orphans[(gridinfo.tile_type, feature)] += 1

    return crossings, orphans


def classify(tile_type, feature, host_types):
    allowed = host_types & set(
        cross_tile_features.ALLOWED.get((tile_type, feature), ()))
    if allowed:
        return 'allowed', allowed
    refused = host_types & set(
        cross_tile_features.REFUSED.get((tile_type, feature), ()))
    if refused:
        return 'refused', refused
    return 'UNCLASSIFIED', frozenset()


def declared_rows():
    for table, verdict in ((cross_tile_features.ALLOWED, 'allowed'),
                           (cross_tile_features.REFUSED, 'refused')):
        for (tile_type, feature), hosts in table.items():
            for host_type in hosts:
                yield tile_type, feature, host_type, verdict


def run(db_roots, complete, out):
    unclassified = 0
    observed = set()
    fabrics = 0
    for db_root in db_roots:
        family = os.path.basename(os.path.normpath(db_root))
        for fabric, part in fabric_parts(db_root):
            fabrics += 1
            crossings, orphans = census_fabric(db_root, part)
            for (tile_type, feature, host_types), count in sorted(
                    crossings.items(), key=lambda kv: (kv[0][0], kv[0][1],
                                                       sorted(kv[0][2]))):
                verdict, matched = classify(tile_type, feature, host_types)
                for host_type in matched:
                    observed.add((tile_type, feature, host_type))
                if verdict == 'UNCLASSIFIED':
                    unclassified += 1
                print(
                    'cross {} {} {}.{} -> {}: {} bit(s) {}'.format(
                        family, fabric, tile_type, feature, '|'.join(
                            sorted(host_types)), count, verdict),
                    file=out)
            print(
                'fabric {} {} ({}): {} cross-tile row(s), {} feature(s) with '
                'a set bit in no tile'.format(
                    family, fabric, part, len(crossings), len(orphans)),
                file=out)

    if fabrics == 0:
        print('tile_window_census: no fabric found', file=out)
        return 2

    stale = 0
    if complete:
        for tile_type, feature, host_type, verdict in sorted(declared_rows()):
            if (tile_type, feature, host_type) not in observed:
                stale += 1
                print(
                    'STALE {} row {}.{} -> {} crosses into that host on no '
                    'fabric censused'.format(
                        verdict, tile_type, feature, host_type),
                    file=out)

    print(
        'tile_window_census: {} fabric(s), {} unclassified cross-tile '
        'row(s), {} stale declaration(s)'.format(fabrics, unclassified, stale),
        file=out)
    return 1 if unclassified or stale else 0


def main():
    parser = argparse.ArgumentParser(
        description='Census every segbits feature whose set bits resolve '
        'outside its own tile window, and compare each crossing with '
        'prjxray.cross_tile_features.')
    parser.add_argument(
        '--db-root',
        action='append',
        required=True,
        help='A family directory of prjxray-db; repeat for each family.')
    parser.add_argument(
        '--complete',
        action='store_true',
        help='The --db-root set is every family the table covers: a '
        'declared row that no fabric exercises is reported as stale.')
    args = parser.parse_args()
    return run(args.db_root, args.complete, sys.stdout)


if __name__ == '__main__':
    sys.exit(main())
