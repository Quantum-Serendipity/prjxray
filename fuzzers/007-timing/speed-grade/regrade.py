#!/usr/bin/env python3
# Copyright (C) 2017-2026  The Project X-Ray Authors.
#
# Use of this source code is governed by a ISC-style
# license that can be found in the LICENSE file or at
# https://opensource.org/licenses/ISC
#
# SPDX-License-Identifier: ISC
import argparse
import collections
import glob
import json
import os
import re
import shutil
import sys

MAGIC = 0.6875
NO_INDEX = '65535'
SITE_RE = re.compile(r'^(.+)_X([0-9]+)Y([0-9]+)$')
MODEL_COLS = (
    'NAME', 'SPEED_INDEX', 'TYPE', 'FAST_MIN', 'FAST_MAX', 'SLOW_MIN',
    'SLOW_MAX', 'DRIVE', 'IN_CAP', 'CAP', 'RES', 'WIRE_RES', 'WIRE_CAP',
    'FORWARD', 'REVERSE', 'DRIVER')
MODEL_ALIAS = {
    '_BSW_INT_LONG_MUX': '_BSW_INT_HLONG_MUX',
    '_BSW_LONG_TLREVERSE': '_BSW_LONG_NONTLFORWARD',
}


def tcl_double(x):
    return repr(float(x))


def load_part(path):
    out = {}
    with open(path) as f:
        for line in f:
            k, _, v = line.rstrip('\n').partition('\t')
            out[k] = v
    return out


def load_models(path):
    by_name = {}
    by_index = {}
    with open(path) as f:
        for line in f:
            fields = line.rstrip('\n').split('\t')
            assert len(fields) == len(MODEL_COLS), line
            m = {
                k: (None if v == '-' else v)
                for k, v in zip(MODEL_COLS, fields)
            }
            by_name[m['NAME']] = m
            idx = m['SPEED_INDEX']
            if idx != NO_INDEX:
                assert idx not in by_index, ('duplicate SPEED_INDEX', idx)
                by_index[idx] = m
    return by_name, by_index


def model_by_name(by_name, name):
    return by_name[MODEL_ALIAS.get(name, name)]


def delay4(m):
    return [m['FAST_MIN'], m['FAST_MAX'], m['SLOW_MIN'], m['SLOW_MAX']]


def resolve_pip(by_name, by_index, idx):
    sm = by_index[idx]
    fwd = model_by_name(by_name, sm['FORWARD'])
    rev = model_by_name(by_name, sm['REVERSE'])
    is_pt = int(fwd['TYPE'] == 'pass_transistor')
    out = {'is_pass_transistor': is_pt}
    for key, m in (('src_to_dst', fwd), ('dst_to_src', rev)):
        if is_pt:
            out[key] = {'delay': None, 'in_cap': None, 'res': m['RES']}
            continue
        res = None
        if m['TYPE'] in ('buffer_switch', 'buffer'):
            res = tcl_double(MAGIC * float(m['DRIVE']))
        in_cap = m['IN_CAP'] if m['TYPE'] == 'buffer_switch' else None
        out[key] = {'delay': delay4(m), 'in_cap': in_cap, 'res': res}
    return out


def resolve_site_pin(by_name, by_index, idx):
    sm = by_index[idx]
    if sm['DRIVER'] is not None:
        drv = model_by_name(by_name, sm['DRIVER'])
        return {
            'delay': delay4(drv),
            'res': tcl_double(MAGIC * float(drv['DRIVE']))
        }
    return {'delay': delay4(sm), 'cap': sm['CAP']}


def resolve_wire(by_index, idx):
    sm = by_index[idx]
    cap, res = sm['WIRE_CAP'], sm['WIRE_RES']
    if cap != '0.000' or res != '0.000':
        return {'cap': cap, 'res': res}
    return None


def load_indices(path):
    pips = collections.defaultdict(lambda: collections.defaultdict(set))
    wires = collections.defaultdict(lambda: collections.defaultdict(set))
    sites = collections.defaultdict(list)
    with open(path) as f:
        for line in f:
            fields = line.rstrip('\n').split('\t')
            kind, ttype, tile = fields[0], fields[1], fields[2]
            if kind == 'P' or kind == 'W':
                name, idx = fields[3], fields[4]
                assert name.startswith(tile + '/'), line
                local = name[len(tile) + 1:]
                (pips if kind == 'P' else wires)[ttype][local].add(idx)
            elif kind == 'S':
                site, _stype, pin, idx = fields[3:7]
                sites[(ttype, tile)].append((site, pin, idx))
            else:
                raise ValueError(line)

    site_pins = collections.defaultdict(lambda: collections.defaultdict(set))
    for (ttype, tile), rows in sites.items():
        coords = {}
        for site, _pin, _idx in rows:
            m = SITE_RE.match(site)
            coords[site] = (m.group(1), int(m.group(2)), int(m.group(3)))
        origin = {}
        for prefix, x, y in coords.values():
            ox, oy = origin.get(prefix, (x, y))
            origin[prefix] = (min(ox, x), min(oy, y))
        for site, pin, idx in rows:
            prefix, x, y = coords[site]
            x0, y0 = origin[prefix]
            assert pin.startswith(site + '/'), pin
            key = (prefix, 'X{}Y{}'.format(x - x0, y - y0), pin[len(site) + 1:])
            site_pins[ttype][key].add(idx)
    return pips, wires, site_pins


class Census:
    def __init__(self):
        self.c = collections.Counter()

    def __getitem__(self, k):
        return self.c[k]

    def add(self, k, n=1):
        self.c[k] += n


def single(idxs, census, what):
    real = {i for i in idxs if i != NO_INDEX}
    if len(real) > 1:
        census.add(what + '-index-conflict')
        return None
    if not real:
        census.add(what + '-no-index')
        return None
    return next(iter(real))


def regrade_tile(tj, by_name, by_index, pips, wires, site_pins, census):
    ttype = tj['tile_type']
    over = {'tile_type': ttype, 'pips': {}, 'wires': {}, 'sites': []}
    diffs = collections.Counter()

    for name, pd in sorted(tj['pips'].items()):
        census.add('pip')
        if name not in pips[ttype]:
            census.add('pip-absent-from-part')
            continue
        idx = single(pips[ttype][name], census, 'pip')
        if idx is None:
            continue
        new = resolve_pip(by_name, by_index, idx)
        over['pips'][name] = new
        census.add('pip-regraded')
        for k in ('src_to_dst', 'dst_to_src'):
            for f in ('delay', 'in_cap', 'res'):
                if pd.get(k, {}).get(f) != new[k][f]:
                    diffs['pip.' + k + '.' + f] += 1
        if pd.get('is_pass_transistor') != new['is_pass_transistor']:
            diffs['pip.is_pass_transistor'] += 1

    for name, wd in sorted(tj['wires'].items()):
        census.add('wire')
        if name not in wires[ttype]:
            census.add('wire-absent-from-part')
            continue
        idx = single(wires[ttype][name], census, 'wire')
        if idx is None:
            continue
        new = resolve_wire(by_index, idx)
        over['wires'][name] = new
        census.add('wire-regraded')
        if wd != new:
            diffs['wire'] += 1

    for site in tj['sites']:
        osite = {'name': site['name'], 'prefix': site['prefix'], 'site_pins': {}}
        for pin, pdata in sorted(site['site_pins'].items()):
            if pdata is None:
                continue
            census.add('site_pin')
            key = (site['prefix'], site['name'], pin)
            if key not in site_pins[ttype]:
                census.add('site_pin-absent-from-part')
                continue
            idx = single(site_pins[ttype][key], census, 'site_pin')
            if idx is None:
                continue
            new = resolve_site_pin(by_name, by_index, idx)
            osite['site_pins'][pin] = new
            census.add('site_pin-regraded')
            old = {k: pdata.get(k) for k in ('delay', 'res', 'cap') if k in pdata}
            if old != new:
                diffs['site_pin'] += 1
        over['sites'].append(osite)
    return over, diffs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--db-family', required=True)
    ap.add_argument('--dump', required=True)
    ap.add_argument('--out')
    ap.add_argument('--sdfs')
    ap.add_argument('--compare-only', action='store_true')
    args = ap.parse_args()

    part = load_part(os.path.join(args.dump, 'part.txt'))
    by_name, by_index = load_models(os.path.join(args.dump, 'models.tsv'))
    pips, wires, site_pins = load_indices(
        os.path.join(args.dump, 'indices.tsv'))
    part_types = set(pips) | set(wires) | set(site_pins)

    grid_path = os.path.join(args.db_family, part['device'], 'tilegrid.json')
    if not os.path.exists(grid_path):
        print('regrade: no tilegrid at {}'.format(grid_path), file=sys.stderr)
        return 1
    with open(grid_path) as f:
        grid_types = {t['type'] for t in json.load(f).values()}
    missing = sorted(grid_types - part_types)
    if missing:
        print('regrade: tile types on {} absent from the dump: {}'.format(
            part['device'], ', '.join(missing)), file=sys.stderr)
        return 1

    census = Census()
    diffs_all = collections.Counter()
    covered = []
    skipped = []
    overlays = {}
    for path in sorted(glob.glob(os.path.join(args.db_family, 'tile_type_*.json'))):
        with open(path) as f:
            tj = json.load(f)
        if tj['tile_type'] not in part_types:
            skipped.append(tj['tile_type'])
            continue
        over, diffs = regrade_tile(
            tj, by_name, by_index, pips, wires, site_pins, census)
        diffs_all.update(diffs)
        covered.append(tj['tile_type'])
        overlays[tj['tile_type']] = over

    print('part {} vivado {} speed {} speed_level_id {}'.format(
        part['part'], part['vivado'], part['speed'], part['speed_level_id']))
    print('tile types regraded {} not on this part {}'.format(
        len(covered), len(skipped)))
    for k in sorted(census.c):
        print('census {} {}'.format(k, census[k]))
    for k in sorted(diffs_all):
        print('differs-from-db {} {}'.format(k, diffs_all[k]))
    gaps = sorted(k for k in census.c if k.endswith(('-absent-from-part', '-index-conflict', '-no-index')))
    if gaps:
        print('regrade: incomplete: {}'.format(', '.join(gaps)), file=sys.stderr)
        return 1
    if args.compare_only:
        return 0

    out = args.out or os.path.join(
        args.db_family, 'speed', part['device'] + part['speed'])
    os.makedirs(out, exist_ok=True)
    for ttype, over in overlays.items():
        with open(os.path.join(out, 'tile_type_{}.json'.format(ttype)), 'w') as f:
            json.dump(over, f, indent=2, sort_keys=True)
            f.write('\n')
    prov = dict(part)
    prov['tile_types'] = sorted(covered)
    prov['census'] = dict(sorted(census.c.items()))
    if args.sdfs:
        sdfs = sorted(glob.glob(os.path.join(args.sdfs, '*.sdf')))
        if not sdfs:
            print('regrade: no SDF in {}'.format(args.sdfs), file=sys.stderr)
            return 1
        os.makedirs(os.path.join(out, 'timings'), exist_ok=True)
        for sdf in sdfs:
            shutil.copyfile(sdf, os.path.join(out, 'timings', os.path.basename(sdf)))
        prov['timings'] = [os.path.basename(x) for x in sdfs]
    with open(os.path.join(out, 'speed.json'), 'w') as f:
        json.dump(prov, f, indent=2, sort_keys=True)
        f.write('\n')
    print('regrade: wrote {}'.format(out))
    return 0


if __name__ == '__main__':
    sys.exit(main())
