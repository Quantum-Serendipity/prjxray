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
'''
Check:
-Individual files are valid
-No overlap between any tile

TODO:
Can we use prjxray?
Relies on 074, which is too far into the process
'''

from prjxray import util
from prjxray import db as prjxraydb
import os
import utils.parsedb as parsedb
#from prjxray import db as prjxraydb
import glob


def gen_tile_bits(tile_segbits, tile_bits):
    '''
    For given tile and corresponding db_file structure yield
    (absolute address, absolute FDRI bit offset, tag)

    For each tag bit in the corresponding block_type entry, calculate absolute address and bit offsets
    '''

    for block_type in tile_segbits:
        assert block_type in tile_bits, "block type %s is not present in current tile" % block_type

        block = tile_bits[block_type]

        baseaddr = block.base_address
        bitbase = 32 * block.offset
        frames = block.frames

        for tag in tile_segbits[block_type]:
            for bit in tile_segbits[block_type][tag]:
                # 31_06
                word_column = bit.word_column
                word_bit = bit.word_bit
                assert word_column <= frames, "ERROR: bit out of bound --> tag: %s; word_column = %s; frames = %s" % (
                    tag, word_column, frames)
                yield word_column + baseaddr, word_bit + bitbase, tag


def make_tile_mask(tile_segbits, tile_name, tile_bits):
    '''
    Return dict
    key: (address, bit index)
    val: sample description of where it came from (there may be multiple, only one)
    '''

    # FIXME: fix mask files https://github.com/SymbiFlow/prjxray/issues/301
    # in the meantime build them on the fly
    # We may want this to build them anyway

    ret = dict()
    for absaddr, bitaddr, tag in gen_tile_bits(tile_segbits, tile_bits):
        name = "%s.%s" % (tile_name, tag)
        ret.setdefault((absaddr, bitaddr), name)
    return ret


def mask_bit(masks, addr, bitaddr):
    '''
    Return (bytearray, byte index, bit mask) addressing bitaddr inside the
    mask holding addr, allocating that mask or growing it as needed
    '''
    mask = masks.get(addr)
    size = bitaddr // 8 + 1
    mask_is_missing = mask is None
    if mask_is_missing:
        mask = masks[addr] = bytearray(size)
    else:
        mask_is_too_small = len(mask) < size
        if mask_is_too_small:
            mask.extend(bytes(size - len(mask)))
    return mask, bitaddr // 8, 1 << (bitaddr & 7)


def collision_owners(grid, tile_segbits, checked_tiles, keys):
    '''
    Name the tile that already uses each colliding bit

    Only called on the collision path, where the run is about to fail, so the
    tiles are re-masked here instead of being kept in memory.  Every checked
    tile is re-masked once and asked for all the keys still unclaimed, and the
    scan stops once they are all claimed, so a database with many colliding
    bits costs one pass over the checked tiles rather than one pass per
    collision.
    '''
    unclaimed = set(keys)
    owners = dict()
    for tile_name in checked_tiles:
        all_keys_claimed = len(unclaimed) == 0
        if all_keys_claimed:
            break
        tile_info = grid.gridinfo_at_tilename(tile_name)
        mtile = make_tile_mask(
            tile_segbits[tile_info.tile_type], tile_name, tile_info.bits)
        claimed = unclaimed.intersection(mtile)
        for key in claimed:
            owners[key] = mtile[key]
        unclaimed -= claimed
    return owners


def read_alias_file(path):
    '''
    Read alias groups: each line names the features that deliberately share one
    bit set, so strict duplicate detection accepts them (one group per line,
    whitespace separated, # starts a comment)

    Each line is expanded into every pair of its names, because that is the
    form detection asks in: two tags claiming one bit set at a time.  A line
    reads as "these all name the same thing", which is exactly the set of
    pairs, and it keeps a three-name group no weaker than the pairs it
    implies.
    '''
    aliases = set()
    with open(path) as f:
        for line in f:
            line = line.split('#')[0].strip()
            if not line:
                continue
            names = line.split()
            assert len(
                names) > 1, "alias group of one in %s: %s" % (path, line)
            for index, name in enumerate(names):
                for other in names[index + 1:]:
                    aliases.add(frozenset((name, other)))
    return aliases


def parsedb_all(db_root, verbose=False, aliases=None):
    '''Verify .db files are individually valid'''

    files = 0
    for bit_fn in glob.glob('%s/segbits_*.db' % db_root):
        # Don't parse db files with fuzzer origin information
        if "origin_info" in bit_fn:
            continue
        verbose and print("Checking %s" % bit_fn)
        parsedb.run(
            bit_fn, fnout=None, strict=True, verbose=verbose, aliases=aliases)
        files += 1
    print("segbits_*.db: %d okay" % files)

    files = 0
    for bit_fn in glob.glob('%s/mask_*.db' % db_root):
        verbose and print("Checking %s" % bit_fn)
        parsedb.run(
            bit_fn, fnout=None, strict=True, verbose=verbose, aliases=aliases)
        files += 1
    print("mask_*.db: %d okay" % files)


def check_tile_overlap(db, verbose=False):
    '''
    Verifies that no two tiles use the same bit

    Assume .db files are individually valid
    Create a mask for all the bits the tile type uses
    For each tile, create bitmasks over the entire bitstream for current part
    Throw an exception if two tiles share an address

    Occupancy is one bit per bitstream bit -- a bytearray per address, grown
    on demand -- rather than a dict entry per bit.  A dict entry carries a
    formatted "tile.tag" name, about 250 bytes per bit, which the larger
    fabrics turn into tens of GiB: more than a CI runner has, so those devices
    could not be checked at all.  The names are only needed to describe a
    collision, which is a failure path, and are recovered there from the tiles
    already checked.
    '''
    masks = dict()
    tiles_type_done = dict()
    tile_segbits = dict()
    checked_tiles = []
    grid = db.grid()
    tiles_checked = 0
    bits_used = 0

    for tile_name in grid.tiles():
        tile_info = grid.gridinfo_at_tilename(tile_name)
        tile_type = tile_info.tile_type
        tile_bits = tile_info.bits

        if tile_type not in tiles_type_done:
            segbits = db.get_tile_segbits(tile_type).segbits
            tile_segbits[tile_type] = segbits

            # If segbits has zero length the tile_type is marked True in order to be skipped
            if len(segbits) == 0:
                tiles_type_done[tile_type] = True
            else:
                tiles_type_done[tile_type] = False

        if tiles_type_done[tile_type]:
            continue

        mtile_keys = set()
        for absaddr, bitaddr, tag in gen_tile_bits(tile_segbits[tile_type],
                                                   tile_bits):
            mtile_keys.add((absaddr, bitaddr))
        verbose and print(
            "Checking %s, type %s, bits: %s" %
            (tile_name, tile_type, len(mtile_keys)))
        if len(mtile_keys) == 0:
            continue

        collisions = set()
        for addr, bitaddr in mtile_keys:
            mask, byte, bit = mask_bit(masks, addr, bitaddr)
            bit_is_taken = (mask[byte] & bit) != 0
            if bit_is_taken:
                collisions.add((addr, bitaddr))
            else:
                mask[byte] |= bit
                bits_used += 1

        if collisions:
            print("ERROR: %s collisions" % len(collisions))
            owners = collision_owners(
                grid, tile_segbits, checked_tiles, collisions)
            mtile = make_tile_mask(
                tile_segbits[tile_type], tile_name, tile_bits)
            for ck in sorted(collisions):
                addr, bitaddr = ck
                word, bit = util.addr_bit2word(bitaddr)
                print(
                    "  %s: had %s, got %s" % (
                        util.addr2str(addr, word, bit),
                        owners.get(ck, "unknown"), mtile[ck]))
            raise ValueError("%s collisions" % len(collisions))
        checked_tiles.append(tile_name)
        tiles_checked += 1
    print("Checked %s tiles, %s bits" % (tiles_checked, bits_used))


def run(db_root, part, verbose=False, aliases=None):
    # Start by running a basic check on db files
    print("Checking individual .db...")
    parsedb_all(db_root, verbose=verbose, aliases=aliases)

    # Now load and verify tile consistency
    db = prjxraydb.Database(db_root, part)
    db._read_tilegrid()
    '''
    these don't load properly without .json files
    See: https://github.com/SymbiFlow/prjxray/issues/303
    db._read_tile_types()
    print(db.tile_types.keys())
    '''

    verbose and print("")

    print("Checking aggregate dir...")
    check_tile_overlap(db, verbose=verbose)


def main():
    import argparse

    parser = argparse.ArgumentParser(
        description="Parse a db repository, checking for consistency")

    util.db_root_arg(parser)
    util.part_arg(parser)
    parser.add_argument('--verbose', action='store_true', help='')
    parser.add_argument(
        '--alias-file',
        help='file of alias groups (see read_alias_file): features in one '
        'group may share a bit set')
    args = parser.parse_args()

    aliases = read_alias_file(args.alias_file) if args.alias_file else None
    run(args.db_root, args.part, verbose=args.verbose, aliases=aliases)


if __name__ == '__main__':
    main()
