#!/usr/bin/env python3
#
# Copyright (C) 2017-2020  The Project X-Ray Authors.
#
# Use of this source code is governed by a ISC-style
# license that can be found in the LICENSE file or at
# https://opensource.org/licenses/ISC
#
# SPDX-License-Identifier: ISC
"""
Generate a prjxray-style tileconn.json using only RapidWright APIs.

tileconn.json describes how wires of neighboring tiles are stitched
together into nodes.  Each entry relates a *pair of tile types* at a given
relative grid offset, and lists the wire name pairs that belong to the same
node whenever those two tile types appear at that offset:

    [
        {
            "grid_deltas": [<dx>, <dy>],
            "tile_types": ["<TILE_TYPE1>", "<TILE_TYPE2>"],
            "wire_pairs": [["<wire in tile1>", "<wire in tile2>"], ...]
        },
        ...
    ]

RapidWright already models the full device routing graph (Tile, Wire,
Node), so instead of parsing raw Vivado dumps like
fuzzers/074-dump_all/generate_grid.py does, we can simply walk every node
in the device and record which wire pairs cross tile boundaries.

prjxray stores only the "directly connected" wire pairs of a node (a
spanning tree), not the full clique of all its wires.  This script
approximates that with a minimum spanning tree over the node's wires using
manhattan grid distance as the edge weight, which strongly prefers pairs
in adjacent tiles.  The resulting file reconstructs exactly the same nodes
as the prjxray database, although the particular spanning pairs chosen for
multi-tile nodes may differ from the ones prjxray picked.

Coordinate note: RapidWright's Tile.getColumn()/getRow() match Vivado's
GRID_POINT_X/GRID_POINT_Y tile properties, which are exactly the
grid_x/grid_y used by prjxray's tilegrid.json, so grid_deltas are computed
the same way.

Usage:
    pip install rapidwright
    python3 rapidwright_tileconn.py --part xc7a35t --output tileconn.json

Add --verify to additionally check that every generated (tile pair, wire
pair) rule holds at *every* occurrence of that tile-type pattern in the
grid, i.e. that the file is safe to apply purely by pattern matching.
Add --drop-unsound to remove the rules that fail that check (e.g.
fabric-edge "bounce" shortcuts the MST picked up) and rewrite the output
file with only the sound ones.
"""

import argparse
import json


def wire_sort_key(wire):
    tile = wire.getTile()
    return (
        str(tile.getTileTypeEnum().name()),
        str(wire.getWireName()),
        tile.getColumn(),
        tile.getRow(),
    )


def spanning_wire_pairs(node):
    """Yield a minimal set of wire pairs connecting all wires of a node.

    Kruskal's MST with manhattan grid distance as the weight, so wire
    pairs in the same or adjacent tiles are preferred over long-distance
    pairs, mirroring prjxray's "directly connected" pairs.
    """
    wires = list(node.getAllWiresInNode())
    if len(wires) < 2:
        return

    edges = []
    for i, wire1 in enumerate(wires):
        tile1 = wire1.getTile()
        for j in range(i + 1, len(wires)):
            tile2 = wires[j].getTile()
            dist = abs(tile1.getColumn() - tile2.getColumn()) + \
                abs(tile1.getRow() - tile2.getRow())
            edges.append((dist, i, j))
    edges.sort()

    parent = list(range(len(wires)))

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    for _, i, j in edges:
        root_i, root_j = find(i), find(j)
        if root_i != root_j:
            parent[root_i] = root_j
            yield wires[i], wires[j]


def add_pair(tileconn, wire1, wire2):
    # Canonical ordering, same as update_tile_conn() in
    # fuzzers/074-dump_all/generate_grid.py.
    if wire_sort_key(wire1) > wire_sort_key(wire2):
        wire1, wire2 = wire2, wire1

    tile1 = wire1.getTile()
    tile2 = wire2.getTile()
    key = (
        str(tile1.getTileTypeEnum().name()),
        str(tile2.getTileTypeEnum().name()),
        tile2.getColumn() - tile1.getColumn(),
        tile2.getRow() - tile1.getRow(),
    )
    tileconn.setdefault(key, set()).add(
        (str(wire1.getWireName()), str(wire2.getWireName())))


def generate_tileconn(device):
    from com.xilinx.rapidwright.device import Node

    tileconn = {}
    tiles = list(device.getAllTiles())
    for tile_idx, tile in enumerate(tiles):
        if tile_idx % 1000 == 0:
            print('tile {}/{}'.format(tile_idx, len(tiles)))

        for wire_idx in range(tile.getWireCount()):
            node = Node.getNode(tile, wire_idx)
            if node is None:
                continue

            # A node is visible from every wire it contains; only process
            # it from its base wire so each node is handled exactly once.
            if node.getTile() != tile or node.getWireIndex() != wire_idx:
                continue

            for wire1, wire2 in spanning_wire_pairs(node):
                add_pair(tileconn, wire1, wire2)

    entries = []
    for (type1, type2, dx, dy), pairs in sorted(tileconn.items()):
        entries.append(
            {
                'grid_deltas': [dx, dy],
                'tile_types': [type1, type2],
                'wire_pairs': sorted(list(pair) for pair in pairs),
            })

    return entries


def load_tileconn(path):
    """ Load an already-generated tileconn.json.

    Returns entries in the exact form generate_tileconn() returns them:
    a list of {'grid_deltas': [dx, dy], 'tile_types': [type1, type2],
    'wire_pairs': [[wire1, wire2], ...]} dicts, sorted by
    (type1, type2, dx, dy) with wire_pairs sorted, so a loaded file can be
    passed to verify_tileconn() (or re-dumped) interchangeably with
    freshly generated entries.
    """
    with open(path) as f:
        entries = json.load(f)

    for entry in entries:
        entry['wire_pairs'] = sorted(
            list(pair) for pair in entry['wire_pairs'])

    entries.sort(
        key=lambda entry: (
            tuple(entry['tile_types']), tuple(entry['grid_deltas'])))

    return entries


def verify_tileconn(device, entries, max_report=20):
    """Check that every rule holds at every matching grid location.

    tileconn.json is applied by pattern matching over the whole grid, so a
    (tile_types, grid_deltas, wire_pair) rule is only sound if the two
    wires are on the same node for *every* occurrence of the pattern.

    The unit of failure is the RULE, not the grid location: one unsound
    rule fails at all its instances, so reporting per location floods the
    output while telling you nothing new.  For each rule this aggregates
    over its instances, classifying each instance as:

      ok        - both wires are on the same node.
      different - the wires are on two DIFFERENT real nodes: applying the
                  rule here would merge two unrelated nets.  Never OK.
      half-dead - exactly one wire has a node (Node.getNode returned null
                  for the other): applying the rule here would glue a dead
                  stub wire onto a real node.  Not OK either; prjxray's
                  check_nodes.py asserts reconstructed nodes contain no
                  wires the real node lacks.
      both-dead - NEITHER wire has a node here: applying the rule invents
                  a phantom node out of wires that are unconnected at this
                  location.  This is benign and unavoidable in the
                  tileconn representation -- the same-type tile simply has
                  less connectivity at this grid location (fabric edge,
                  hole, clock row), and prjxray's own database creates the
                  same phantoms when pattern-applied, invisible to its
                  check_nodes.py which only walks real nodes.

    A rule is unsound if it has any 'different' or 'half-dead' instances.
    Rules whose only anomalies are 'both-dead' instances are counted and
    reported separately, not as unsound.

    Returns the list of unsound rules as (entry_index, (wire1, wire2))
    tuples, suitable for drop_unsound_rules().  Its length is the number
    of unsound rules.
    """
    from com.xilinx.rapidwright.device import Node

    tiles_by_type = {}
    for tile in device.getAllTiles():
        tiles_by_type.setdefault(
            str(tile.getTileTypeEnum().name()), []).append(tile)

    total_rules = 0
    unsound = []
    phantom_only_rules = 0

    for entry_idx, entry in enumerate(entries):
        type1, type2 = entry['tile_types']
        dx, dy = entry['grid_deltas']

        # Find all grid locations matching this entry's pattern once.
        instances = []
        for tile1 in tiles_by_type[type1]:
            row = tile1.getRow() + dy
            col = tile1.getColumn() + dx
            if not (0 <= row < device.getRows()
                    and 0 <= col < device.getColumns()):
                continue

            tile2 = device.getTile(row, col)
            if tile2 is not None and str(
                    tile2.getTileTypeEnum().name()) == type2:
                instances.append((tile1, tile2))

        for wire1, wire2 in entry['wire_pairs']:
            total_rules += 1
            ok = different = half_dead = both_dead = 0
            example = None

            for tile1, tile2 in instances:
                idx1 = tile1.getWireIndex(wire1)
                idx2 = tile2.getWireIndex(wire2)
                node1 = Node.getNode(tile1, idx1) if idx1 is not None else None
                node2 = Node.getNode(tile2, idx2) if idx2 is not None else None

                if node1 is None and node2 is None:
                    both_dead += 1
                elif node1 is None or node2 is None:
                    half_dead += 1
                    if example is None:
                        example = (tile1.getName(), tile2.getName())
                elif node1.equals(node2):
                    ok += 1
                else:
                    different += 1
                    if example is None:
                        example = (tile1.getName(), tile2.getName())

            if different or half_dead:
                unsound.append((entry_idx, (wire1, wire2)))
                if len(unsound) <= max_report:
                    print(
                        'UNSOUND: ({}, {}, {}) {} <-> {}: '
                        'ok {}, different {}, half-dead {}, both-dead {} '
                        'of {} instances, e.g. {} <-> {}'.format(
                            type1, type2, entry['grid_deltas'], wire1,
                            wire2, ok, different, half_dead, both_dead,
                            len(instances), example[0], example[1]))
            elif both_dead:
                phantom_only_rules += 1

    if len(unsound) > max_report:
        print('... {} more unsound rules not shown'.format(
            len(unsound) - max_report))
    print(
        '{} of {} rules unsound, {} more benign '
        '(phantom nodes over dead wires only)'.format(
            len(unsound), total_rules, phantom_only_rules))

    return unsound


def drop_unsound_rules(entries, unsound):
    """ Return entries with the given unsound wire pairs removed.

    unsound is the list returned by verify_tileconn().  Entries left with
    no wire pairs are removed entirely.  The node reconstruction survives
    dropping a rule as long as the affected nodes are still spanned by
    their remaining (sound) pairs; compare the result against a reference
    database with compare_tileconn.py to confirm.
    """
    unsound_by_entry = {}
    for entry_idx, pair in unsound:
        unsound_by_entry.setdefault(entry_idx, set()).add(tuple(pair))

    filtered = []
    for entry_idx, entry in enumerate(entries):
        bad_pairs = unsound_by_entry.get(entry_idx, set())
        wire_pairs = [
            pair for pair in entry['wire_pairs']
            if tuple(pair) not in bad_pairs
        ]
        if wire_pairs:
            filtered.append(dict(entry, wire_pairs=wire_pairs))

    return filtered


def main():
    parser = argparse.ArgumentParser(
        description='Generate a prjxray-style tileconn.json via RapidWright.'
    )
    parser.add_argument(
        '--part', default='xc7a35t', help='Part or device name')
    parser.add_argument('--output', default='tileconn.json')
    parser.add_argument(
        '--verify',
        action='store_true',
        help='Re-check every rule at every grid location (slow); '
        'exit 1 if unsound rules are found')
    parser.add_argument(
        '--drop-unsound',
        action='store_true',
        help='Verify (implied), remove unsound rules and rewrite the '
        'output file with only the sound ones')
    args = parser.parse_args()

    import rapidwright  # noqa: F401 -- starts the JVM
    from com.xilinx.rapidwright.device import Device

    device = Device.getDevice(args.part)
    print('Loaded device {} ({} rows x {} cols)'.format(
        device.getName(), device.getRows(), device.getColumns()))

    entries = generate_tileconn(device)
    print('{} tile type/delta entries'.format(len(entries)))

    with open(args.output, 'w') as f:
        json.dump(entries, f, indent=4, sort_keys=True)
        f.write('\n')

    if args.verify or args.drop_unsound:
        unsound = verify_tileconn(device, entries)

        if unsound and args.drop_unsound:
            entries = drop_unsound_rules(entries, unsound)
            print(
                'dropped {} unsound rules, {} entries remain'.format(
                    len(unsound), len(entries)))
            with open(args.output, 'w') as f:
                json.dump(entries, f, indent=4, sort_keys=True)
                f.write('\n')
        elif unsound:
            exit(1)


if __name__ == '__main__':
    main()
