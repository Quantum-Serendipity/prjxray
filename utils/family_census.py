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
import glob
import os
import re
import sys

from prjxray import util

TILE_SUFFIX_RE = re.compile(r'_X\d+Y\d+$')


def families_in(db_root):
    return sorted(
        name for name in os.listdir(db_root)
        if glob.glob(os.path.join(db_root, name, 'segbits_*.db')))


def segbits_path(db_root, family, tile_type):
    return os.path.join(
        db_root, family, 'segbits_%s.db' % tile_type.lower())


def load_segbits(path, cache):
    if path not in cache:
        entries = {}
        if os.path.exists(path):
            with util.OpenSafeFile(path) as f:
                for line in f:
                    line = line.strip()
                    if not line:
                        continue
                    tag, bits, mode, _ = util.parse_db_line(line)
                    if bits is None:
                        continue
                    entries.setdefault(tag, set()).update(
                        b for b in bits if not b.startswith('!'))
        cache[path] = entries
    return cache[path]


def check_tag(db_root, family, tag, cache):
    tile_type = tag.split('.', 1)[0]
    entries = load_segbits(segbits_path(db_root, family, tile_type), cache)
    if tag not in entries:
        return ['%s: %s is absent' % (family, tag)]
    problems = []
    mine = entries[tag]
    for other, bits in sorted(entries.items()):
        if other == tag:
            continue
        shared = mine & bits
        if shared and bits <= mine:
            problems.append(
                '%s: %s claims %s, which %s also sets' %
                (family, other, ' '.join(sorted(shared)), tag))
    return problems


def required_features(db_root, family):
    out = []
    for fn in sorted(glob.glob(os.path.join(db_root, family, '*',
                                            'required_features.fasm'))):
        part = os.path.basename(os.path.dirname(fn))
        with open(fn) as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                tile, _, feature = line.partition('.')
                if not feature:
                    raise ValueError('%s: malformed line %r' % (fn, line))
                tile_type = TILE_SUFFIX_RE.sub('', tile)
                out.append((part, line, '%s.%s' % (tile_type, feature)))
    return out


def run(db_root, families, require, out=None):
    if out is None:
        out = sys.stdout
    cache = {}
    problems = []
    checked = 0
    for family in families:
        for tag in require:
            checked += 1
            got = check_tag(db_root, family, tag, cache)
            print(
                '%-9s %-48s %s' % (family, tag, 'MISSING' if any(
                    p.endswith('is absent') for p in got) else
                                    'OVERLAP' if got else 'ok'),
                file=out)
            problems += got
        for part, line, tag in required_features(db_root, family):
            checked += 1
            got = check_tag(db_root, family, tag, cache)
            print(
                '%-9s %-48s %s (required by %s)' %
                (family, line, 'UNRESOLVED' if got else 'ok', part),
                file=out)
            problems += ['%s (required_features.fasm of %s)' % (p, part)
                         for p in got]
    for p in problems:
        print('PROBLEM %s' % p, file=out)
    print(
        'family-census: %d family(ies), %d check(s), %d problem(s)' %
        (len(families), checked, len(problems)),
        file=out)
    return problems, checked


def main(argv=None):
    parser = argparse.ArgumentParser(
        description='Check, per family, that each --require tag is in the '
        'segbits file of its tile type, that no other tag of that file sets '
        'only bits it sets, and that every line of every part\'s '
        'required_features.fasm resolves the same way.',
        epilog='Exit status: 0 when every check passes; 1 on any problem; 2 '
        'when there is no verdict (no family found, or an unreadable or '
        'malformed input).')
    parser.add_argument('--db-root', required=True)
    parser.add_argument('--family', action='append')
    parser.add_argument(
        '--require',
        action='append',
        default=[],
        help='<TILE_TYPE>.<feature> that every family must carry')
    args = parser.parse_args(argv)
    try:
        families = args.family or families_in(args.db_root)
        if not families:
            print(
                'family-census: no verdict, no family under %s' %
                args.db_root,
                file=sys.stderr)
            return 2
        problems, checked = run(args.db_root, families, args.require)
    except (OSError, ValueError, AssertionError) as e:
        print('family-census: no verdict, %s' % e, file=sys.stderr)
        return 2
    if not checked:
        print('family-census: no verdict, nothing checked', file=sys.stderr)
        return 2
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
