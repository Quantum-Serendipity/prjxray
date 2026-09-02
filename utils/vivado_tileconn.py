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
Generate tileconn.json for one part using only Vivado TCL -- no RapidWright.

This is a standalone, trimmed reimplementation of the prjxray tileconn
pipeline (fuzzers/072-ordered_wires + fuzzers/074-dump_all), producing
output identical to prjxray-db's tileconn.json (same pair-selection
algorithm, same sorting, same xjson.pprint formatting), assuming the same
part and Vivado version.

Pipeline:

1. Dump the tile grid (name, type, GRID_POINT_X/Y) -- one small Vivado run.
2. Dump every node's wire list -- N parallel Vivado runs (like
   fuzzers/074-dump_all/run_fuzzer.py).
3. Process nodes with the algorithm ported verbatim from
   generate_grid.py:
     - 2-wire nodes: record the pair unconditionally.
     - >=3-wire nodes: keep a wire pair only if is_connected() proves a
       direct connection (next_wire_in_dimension -> node_tree ->
       only_wire cascade).
   Nodes where next_wire_in_dimension() cannot decide some pair need the
   node_tree (ordered wires).  Those node names are collected instead of
   dumping ordered wires for the whole device up front.
4. Dump ordered wires ONLY for the collected nodes (per-node equivalent of
   fuzzers/072-ordered_wires/job.tcl: get_wires -from/-to <pip> -of
   <node>), build their node trees with the logic ported verbatim from
   create_node_tree.py, and finish processing the deferred nodes.
5. Sort wire_pairs naturally and entries by (tile_types, grid_deltas)
   exactly as generate_grid.py main() does, then write with
   prjxray.xjson.pprint -- the same normalization utils/sort_db.py
   applies to prjxray-db files.

Each phase caches its dump files in --workdir and is skipped when the
output already exists, so an interrupted run resumes where it left off.

Usage:
    export XRAY_VIVADO=vivado   # or pass --vivado
    python3 vivado_tileconn.py --part xc7a50tfgg484-1 \
        --workdir build_tileconn --output tileconn.json --jobs 4

Note: use the full part name prjxray used (XRAY_PART, e.g.
xc7a50tfgg484-1) and, for byte-identical output, the same Vivado version
prjxray-db was generated with -- node/wire sets can differ slightly
between Vivado releases.
"""

import argparse
import datetime
import itertools
import json
import multiprocessing
import os
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from prjxray.xjson import extract_numbers, pprint  # noqa: E402

# =========================================================================
# Vivado TCL templates
# =========================================================================

OPEN_DESIGN_TCL = """\
create_project -force -part {part} {name} {name}
set_property design_mode PinPlanning [current_fileset]
open_io_design -name io_1
set_param messaging.disableStorage 1
"""

DUMP_TILES_TCL = OPEN_DESIGN_TCL + """\
set fp [open "{out}" w]
foreach tile [get_tiles] {{
    puts $fp "$tile,[get_property TYPE $tile],[get_property GRID_POINT_X $tile],[get_property GRID_POINT_Y $tile]"
}}
close $fp
"""

COUNT_NODES_TCL = OPEN_DESIGN_TCL + """\
set fp [open "{out}" w]
puts $fp [llength [get_nodes]]
close $fp
"""

DUMP_NODES_TCL = OPEN_DESIGN_TCL + """\
set start [expr int([lindex $argv 0])]
set stop [expr int([lindex $argv 1])]
set fp [open "{out}" w]
set nodes [get_nodes]
for {{ set j $start }} {{ $j < $stop }} {{ incr j }} {{
    set node [lindex $nodes $j]
    puts $fp "$node [get_wires -of_objects $node]"
}}
close $fp
"""

# Per-node equivalent of fuzzers/072-ordered_wires/job.tcl.  072 iterates
# pips: for pip P and node N in [get_nodes -downhill -of P], the downhill
# line is "P N [get_wires -from P -of N]".  N in the downhill set of P is
# the same relation as P in the uphill pip set of N, so iterating the
# node's pips produces the same lines.
DUMP_ORDERED_TCL = OPEN_DESIGN_TCL + """\
set fp_in [open "{node_list}" r]
set node_names [split [string trim [read $fp_in]] "\\n"]
close $fp_in

set down_fp [open "{down_out}" w]
set up_fp [open "{up_out}" w]
foreach node_name $node_names {{
    set node [get_nodes $node_name]
    foreach pip [get_pips -uphill -of_object $node] {{
        puts $down_fp "$pip $node [get_wires -from $pip -of_object $node]"
    }}
    foreach pip [get_pips -downhill -of_object $node] {{
        puts $up_fp "$pip $node [get_wires -to $pip -of_object $node]"
    }}
}}
close $down_fp
close $up_fp
"""


def run_vivado(vivado, workdir, tcl_name, tcl_text, tclargs=()):
    tcl_path = os.path.join(workdir, tcl_name)
    with open(tcl_path, 'w') as f:
        f.write(tcl_text)

    cmd = [vivado, '-mode', 'batch', '-source', os.path.basename(tcl_path)]
    if tclargs:
        cmd.append('-tclargs')
        cmd.extend(str(arg) for arg in tclargs)

    print(
        '{} Running: {}'.format(datetime.datetime.now(), ' '.join(cmd)),
        flush=True)
    subprocess.check_call(cmd, cwd=workdir, stdout=subprocess.DEVNULL)


def _run_vivado_star(args):
    return run_vivado(*args[0], **args[1])


def run_vivado_pool(jobs, invocations):
    """ Run a list of (args, kwargs) run_vivado invocations in parallel. """
    if jobs <= 1 or len(invocations) <= 1:
        for args, kwargs in invocations:
            run_vivado(*args, **kwargs)
    else:
        with multiprocessing.Pool(processes=jobs) as pool:
            pool.map(_run_vivado_star, invocations)


# =========================================================================
# Pair-selection algorithm, ported verbatim from
# fuzzers/074-dump_all/generate_grid.py.  Do not "improve" these
# functions; output identity depends on them matching prjxray exactly.
# =========================================================================


def next_wire_in_dimension(
        wire1, tile1, wire2, tile2, tiles, x_wires, y_wires, wire_map,
        wires_in_node):
    """ next_wire_in_dimension returns true if tile1 and tile2 are in the same
  row and column, and must be adjcent.
  """
    tile1_info = tiles[tile1]
    tile2_info = tiles[tile2]

    tile1_x = tile1_info['grid_x']
    tile2_x = tile2_info['grid_x']
    tile1_y = tile1_info['grid_y']
    tile2_y = tile2_info['grid_y']

    # All wires are in the same row or column or if the each wire lies in its own
    # row or column.
    if len(y_wires) == 1 or len(x_wires) == len(wires_in_node) or abs(
            tile1_y - tile2_y) == 0:
        ordered_wires = sorted(x_wires.keys())

        idx1 = ordered_wires.index(tile1_x)
        idx2 = ordered_wires.index(tile2_x)

        if len(x_wires[tile1_x]) == 1 and len(x_wires[tile2_x]) == 1:
            return abs(idx1 - idx2) == 1

    if len(x_wires) == 1 or len(y_wires) == len(wires_in_node) or abs(
            tile1_x - tile2_x) == 0:
        ordered_wires = sorted(y_wires.keys())

        idx1 = ordered_wires.index(tile1_y)
        idx2 = ordered_wires.index(tile2_y)

        if len(y_wires[tile1_y]) == 1 and len(y_wires[tile2_y]) == 1:
            return abs(idx1 - idx2) == 1

    return None


def only_wire(tile1, tile2, tiles, x_wires, y_wires):
    """ only_wire returns true if tile1 and tile2 only have 1 wire in their respective x or y dimension.
  """
    tile1_info = tiles[tile1]
    tile2_info = tiles[tile2]

    tile1_x = tile1_info['grid_x']
    tile2_x = tile2_info['grid_x']

    tiles_x_adjacent = abs(tile1_x - tile2_x) == 1
    if tiles_x_adjacent and len(x_wires[tile1_x]) == 1 and len(
            x_wires[tile2_x]) == 1:
        return True

    tile1_y = tile1_info['grid_y']
    tile2_y = tile2_info['grid_y']

    tiles_y_adjacent = abs(tile1_y - tile2_y) == 1
    if tiles_y_adjacent and len(y_wires[tile1_y]) == 1 and len(
            y_wires[tile2_y]) == 1:
        return True

    return None


def is_directly_connected(node, node_tree, wire1, wire2):
    if 'wires' in node_tree:
        node_tree_wires = node_tree['wires']
    else:
        if len(node_tree['edges']) == 1 and len(node_tree['joins']) == 0:
            node_tree_wires = node_tree['edges'][0]
        else:
            return None

    if wire1 not in node_tree_wires:
        return None
    if wire2 not in node_tree_wires:
        return None

    # Is there than edge that has wire1 next to wire2?
    for edge in node_tree['edges']:
        idx1 = None
        idx2 = None
        try:
            idx1 = edge.index(wire1)
        except ValueError:
            pass

        try:
            idx2 = edge.index(wire2)
        except ValueError:
            pass

        if idx1 is not None and idx2 is not None:
            return abs(idx1 - idx2) == 1

        if idx1 is not None and (idx1 != 0 and idx1 != len(edge) - 1):
            return False

        if idx2 is not None and (idx2 != 0 and idx2 != len(edge) - 1):
            return False

    # Is there a join of nodes between wire1 and wire2?
    if wire1 in node_tree['joins']:
        return wire2 in node_tree['joins'][wire1]

    if wire2 in node_tree['joins']:
        assert wire1 not in node_tree['joins'][wire2]

    return None


def is_connected(
        wire1, tile1, wire2, tile2, node, wires_in_tiles, wire_map, node_tree,
        tiles, x_wires, y_wires, wires_in_node):
    """ Check if two wires are directly connected. """

    next_wire_in_dim = next_wire_in_dimension(
        wire1, tile1, wire2, tile2, tiles, x_wires, y_wires, wire_map,
        wires_in_node)
    if next_wire_in_dim is not None:
        return next_wire_in_dim

    connected = is_directly_connected(
        node['node'], node_tree[node['node']], wire1, wire2)
    if connected is not None:
        return connected

    is_only_wire = only_wire(tile1, tile2, tiles, x_wires, y_wires)
    if is_only_wire is not None:
        return is_only_wire

    # The node_tree didn't specify these wires, and the wires are not
    # unambiguously connected.
    return False


def compute_wire_dicts(wires, wire_map, tiles):
    """ Compute the wires_in_tiles/x_wires/y_wires dicts of process_node. """
    wires_in_tiles = {}
    x_wires = {}
    y_wires = {}
    for wire in wires:
        wire_info = wire_map[wire]

        if wire_info['tile'] not in wires_in_tiles:
            wires_in_tiles[wire_info['tile']] = []
        wires_in_tiles[wire_info['tile']].append(wire)

        grid_x = tiles[wire_info['tile']]['grid_x']
        if grid_x not in x_wires:
            x_wires[grid_x] = []
        x_wires[grid_x].append(wire)

        grid_y = tiles[wire_info['tile']]['grid_y']
        if grid_y not in y_wires:
            y_wires[grid_y] = []
        y_wires[grid_y].append(wire)

    return wires_in_tiles, x_wires, y_wires


def process_node(tileconn, node, wire_map, node_tree, tiles):
    """ Ported from generate_grid.py process_node.

    tileconn here is the flat {(tile_types, grid_deltas): set(wire_pairs)}
    accumulator (see update_tile_conn); the result after flattening is
    identical to prjxray's append-then-flatten_tile_conn, which only
    deduplicates into exactly this keyed set structure.
    """
    wires = [wire['wire'] for wire in node['wires']]

    wires_in_tiles, x_wires, y_wires = compute_wire_dicts(
        wires, wire_map, tiles)

    if len(wires) == 2:
        wire1 = wires[0]
        wire_info1 = wire_map[wire1]
        wire2 = wires[1]
        wire_info2 = wire_map[wire2]
        update_tile_conn(tileconn, wire1, wire_info1, wire2, wire_info2, tiles)
        return

    for idx, wire1 in enumerate(wires):
        wire_info1 = wire_map[wire1]
        for wire2 in wires[idx + 1:]:
            wire_info2 = wire_map[wire2]

            if not is_connected(wire1, wire_info1['tile'], wire2,
                                wire_info2['tile'], node, wires_in_tiles,
                                wire_map, node_tree, tiles, x_wires, y_wires,
                                wires):
                continue

            update_tile_conn(
                tileconn, wire1, wire_info1, wire2, wire_info2, tiles)


def update_tile_conn(tileconn, wirename1, wire1, wirename2, wire2, tiles):
    # Ensure that (wire1, wire2) is sorted, so we can easy check if a connection
    # already exists.

    tile1 = tiles[wire1['tile']]
    tile2 = tiles[wire2['tile']]
    if ((wire1['type'], wire1['shortname'], tile1['grid_x'], tile1['grid_y']) >
        (wire2['type'], wire2['shortname'], tile2['grid_x'], tile2['grid_y'])):
        wire1, tile1, wire2, tile2 = wire2, tile2, wire1, tile1

    key = (
        (tile1['type'], tile2['type']),
        (
            tile2['grid_x'] - tile1['grid_x'],
            tile2['grid_y'] - tile1['grid_y'],
        ),
    )
    if key not in tileconn:
        tileconn[key] = set()
    tileconn[key].add((wire1['shortname'], wire2['shortname']))


# =========================================================================
# Node tree construction, ported verbatim from
# fuzzers/074-dump_all/create_node_tree.py.
# =========================================================================


def generate_edges(graph, root, graph_nodes):
    """ Starting from root, generate an edge in dir and insert into graph.

  If the tree forks, simply insert a joins to indicate the split.

  """
    edge = [root]
    prev_root = None

    while True:
        outbound_edges = graph_nodes[root]
        outbound_edges -= set((prev_root, ))
        if len(outbound_edges) > 1:
            graph['edges'].append(edge)
            if root not in graph['joins']:
                graph['joins'][root] = set()
            graph['joins'][root] |= outbound_edges

            for element in graph_nodes[root]:
                if element not in graph['joins']:
                    graph['joins'][element] = set()
                graph['joins'][element].add(root)

            break
        else:
            if len(outbound_edges) == 0:
                graph['edges'].append(edge)
                break

            next_root = tuple(outbound_edges)[0]
            edge.append(next_root)
            prev_root, root = root, next_root


def create_ordered_wires_for_node(node, wires_in_node, downhill, uphill):
    if len(wires_in_node) <= 2:
        return {'edges': [wires_in_node], 'joins': {}}

    downhill = set(tuple(l) for l in downhill)
    uphill = set(tuple(l) for l in uphill)

    roots = set()
    all_wires = set()

    for wire in downhill:
        if len(wire) > 0:
            roots |= set((wire[0], wire[-1]))
            all_wires |= set(wire)

    for wire in uphill:
        if len(wire) > 0:
            roots |= set((wire[0], wire[-1]))
            all_wires |= set(wire)

    assert len(wires_in_node) >= len(all_wires), (
        len(wires_in_node), len(all_wires))

    if len(all_wires) <= 2:
        return {'edges': tuple(all_wires), 'joins': {}}

    graph_nodes = dict((wire, set()) for wire in all_wires)

    for wire in all_wires:
        for down in downhill:
            try:
                idx = down.index(wire)
                if idx + 1 < len(down):
                    graph_nodes[wire].add(down[idx + 1])
                if idx - 1 >= 0:
                    graph_nodes[wire].add(down[idx - 1])
            except ValueError:
                continue

        for up in uphill:
            try:
                idx = up.index(wire)
                if idx + 1 < len(up):
                    graph_nodes[wire].add(up[idx + 1])
                if idx - 1 >= 0:
                    graph_nodes[wire].add(up[idx - 1])
            except ValueError:
                continue

    graph = {'edges': [], 'joins': {}}

    while len(roots) > 0:
        root = roots.pop()

        if len(graph_nodes[root]) > 0:
            generate_edges(graph, root, graph_nodes)

    # Dedup identical edges.
    final_edges = set()

    for edge in graph['edges']:
        edge1 = tuple(edge)
        edge2 = tuple(edge[::-1])

        if edge1 > edge2:
            final_edges.add((edge2, edge1))
        else:
            final_edges.add((edge1, edge2))

    edges = [edge[0] for edge in final_edges]

    element_index = {}
    for edge in edges:
        for idx, element in enumerate(edge):
            if element not in element_index:
                element_index[element] = []
            element_index[element].append((idx, edge))

    new_edges = []
    for edge in edges:
        starts = element_index[edge[0]]
        ends = element_index[edge[-1]]

        found_any = False
        for start in starts:
            start_idx, other_edge = start
            if other_edge is edge:
                continue

            for end in ends:
                if other_edge is not end[1]:
                    continue

                found_any = True
                end_idx, _ = end
                # check if the interior elements are the same.
                if start_idx > end_idx:
                    step = -1
                else:
                    step = 1

                other_edge_slice = slice(
                    start_idx, end_idx + step if end_idx + step >= 0 else None,
                    step)
                if edge != other_edge[other_edge_slice]:
                    new_edges.append(edge)

        if not found_any:
            new_edges.append(edge)

    output = {
        'edges':
        new_edges,
        'joins':
        dict((key, tuple(value)) for key, value in graph['joins'].items()),
        'wires':
        wires_in_node,
    }

    return output


# =========================================================================
# Driver
# =========================================================================


def parse_node_line(line):
    """ Parse '<node> <wire> <wire> ...' into a raw-node dict. """
    parts = line.split()
    return {
        'node': parts[0],
        'wires': [{
            'wire': wire
        } for wire in parts[1:]],
    }


def wire_map_for_node(node, tiles):
    """ Build the wire -> {tile, type, shortname} map for one node's wires.

    generate_grid.py builds this map for the whole device from the tile
    dumps; only entries for the node's own wires are ever read by
    process_node, and wire names are 'TILE/WIRE', so per-node
    reconstruction from the tile table is equivalent.
    """
    wire_map = {}
    for wire in node['wires']:
        wire_name = wire['wire']
        tile, _, shortname = wire_name.partition('/')
        wire_map[wire_name] = {
            'tile': tile,
            'type': tiles[tile]['type'],
            'shortname': shortname,
        }
    return wire_map


def needs_node_tree(node, wire_map, tiles):
    """ True if process_node would consult node_tree for this node.

    is_connected() reads node_tree only for pairs where
    next_wire_in_dimension() returns None, and only nodes with >= 3 wires
    reach the pair loop at all.
    """
    wires = [wire['wire'] for wire in node['wires']]
    if len(wires) <= 2:
        return False

    _, x_wires, y_wires = compute_wire_dicts(wires, wire_map, tiles)

    for wire1, wire2 in itertools.combinations(wires, 2):
        info1 = wire_map[wire1]
        info2 = wire_map[wire2]
        if next_wire_in_dimension(wire1, info1['tile'], wire2, info2['tile'],
                                  tiles, x_wires, y_wires, wire_map,
                                  wires) is None:
            return True

    return False


def read_ordered_wires(fname):
    """ Read 'pip node wire wire ...' lines into {node: [wire lists]}. """
    by_node = {}
    with open(fname) as f:
        for line in f:
            parts = line.split()
            if len(parts) < 2:
                continue
            node = parts[1]
            by_node.setdefault(node, []).append(parts[2:])
    return by_node


def main():
    parser = argparse.ArgumentParser(
        description='Generate prjxray-identical tileconn.json via Vivado TCL.'
    )
    parser.add_argument(
        '--part', required=True, help='Full part name, e.g. xc7a50tfgg484-1')
    parser.add_argument('--output', default='tileconn.json')
    parser.add_argument('--workdir', default='build_tileconn')
    parser.add_argument(
        '--vivado', default=os.getenv('XRAY_VIVADO', 'vivado'))
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument(
        '--block_size',
        type=int,
        default=100000,
        help='Nodes per Vivado instance in the node dump')
    args = parser.parse_args()

    os.makedirs(args.workdir, exist_ok=True)

    def workpath(name):
        return os.path.join(args.workdir, name)

    # ---- Phase 1: tile grid --------------------------------------------
    tiles_csv = workpath('tiles.csv')
    if not os.path.exists(tiles_csv):
        run_vivado(
            args.vivado, args.workdir, 'dump_tiles.tcl',
            DUMP_TILES_TCL.format(
                part=args.part, name='prj_tiles', out='tiles.csv'))

    tiles = {}
    with open(tiles_csv) as f:
        for line in f:
            name, tile_type, grid_x, grid_y = line.strip().split(',')
            tiles[name] = {
                'type': tile_type,
                'grid_x': int(grid_x),
                'grid_y': int(grid_y),
            }
    print('{} {} tiles'.format(datetime.datetime.now(), len(tiles)))

    # ---- Phase 2: node -> wires dump -----------------------------------
    count_file = workpath('nb_nodes.txt')
    if not os.path.exists(count_file):
        run_vivado(
            args.vivado, args.workdir, 'count_nodes.tcl',
            COUNT_NODES_TCL.format(
                part=args.part, name='prj_count', out='nb_nodes.txt'))
    with open(count_file) as f:
        node_count = int(f.read().strip())
    print('{} {} nodes'.format(datetime.datetime.now(), node_count))

    node_files = []
    invocations = []
    for block, start in enumerate(range(0, node_count, args.block_size)):
        stop = min(start + args.block_size, node_count)
        out = 'nodes_{}.txt'.format(block)
        node_files.append(workpath(out))
        if not os.path.exists(workpath(out)):
            invocations.append(
                (
                    (
                        args.vivado, args.workdir,
                        'dump_nodes_{}.tcl'.format(block),
                        DUMP_NODES_TCL.format(
                            part=args.part,
                            name='prj_nodes_{}'.format(block),
                            out=out)), {
                                'tclargs': (start, stop)
                            }))
    run_vivado_pool(args.jobs, invocations)

    # ---- Phase 3: process nodes, defer the ambiguous ones ---------------
    tileconn = {}
    deferred = []
    total = 0
    for node_file in node_files:
        with open(node_file) as f:
            for line in f:
                if not line.strip():
                    continue
                total += 1
                node = parse_node_line(line)
                if len(node['wires']) < 2:
                    continue

                wire_map = wire_map_for_node(node, tiles)
                if needs_node_tree(node, wire_map, tiles):
                    deferred.append(node)
                else:
                    process_node(tileconn, node, wire_map, {}, tiles)

    print(
        '{} {} nodes processed, {} deferred (need ordered wires)'.format(
            datetime.datetime.now(), total, len(deferred)))

    # ---- Phase 4: on-demand ordered wires for deferred nodes ------------
    if deferred:
        down_file = workpath('downhill_wires.txt')
        up_file = workpath('uphill_wires.txt')
        if not (os.path.exists(down_file) and os.path.exists(up_file)):
            node_list = workpath('nodes_needed.txt')
            with open(node_list, 'w') as f:
                for node in deferred:
                    print(node['node'], file=f)
            run_vivado(
                args.vivado, args.workdir, 'dump_ordered.tcl',
                DUMP_ORDERED_TCL.format(
                    part=args.part,
                    name='prj_ordered',
                    node_list='nodes_needed.txt',
                    down_out='downhill_wires.txt',
                    up_out='uphill_wires.txt'))

        downhill = read_ordered_wires(down_file)
        uphill = read_ordered_wires(up_file)

        node_tree = {}
        for node in deferred:
            node_tree[node['node']] = create_ordered_wires_for_node(
                node['node'],
                tuple(wire['wire'] for wire in node['wires']),
                downhill.get(node['node'], ()), uphill.get(node['node'], ()))

        for node in deferred:
            wire_map = wire_map_for_node(node, tiles)
            process_node(tileconn, node, wire_map, node_tree, tiles)

    # ---- Phase 5: sort and write, matching generate_grid.py + sort_db ---
    entries = []
    for (tile_types, grid_deltas), wire_pairs in tileconn.items():
        entries.append(
            {
                'tile_types': list(tile_types),
                'grid_deltas': list(grid_deltas),
                'wire_pairs': tuple(
                    sorted(
                        wire_pairs,
                        key=lambda x: tuple(extract_numbers(s) for s in x))),
            })

    entries = sorted(
        entries, key=lambda x: (x['tile_types'], x['grid_deltas']))

    with open(args.output, 'w') as f:
        pprint(f, entries)

    print(
        '{} Wrote {} entries to {}'.format(
            datetime.datetime.now(), len(entries), args.output))


if __name__ == '__main__':
    main()
