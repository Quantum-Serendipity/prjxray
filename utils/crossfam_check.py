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
import itertools
import os
import sys

from prjxray import util


VARIANT_SUFFIXES = ('_fuji2', '_pele1')


def frame_indices(bits):
    return frozenset(int(bit.lstrip('!').split('_')[0]) for bit in bits)


def tile_of(fname):
    assert fname.startswith('segbits_') and fname.endswith('.db'), fname
    return fname[len('segbits_'):-len('.db')]


def canonical_tile(tile):
    for suffix in VARIANT_SUFFIXES:
        if tile.endswith(suffix):
            return tile[:-len(suffix)]
    return tile


def read_segbits(fn, alias=None):
    entries = {}
    full = {}
    with util.OpenSafeFile(fn) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            tag, bits, mode, _ = util.parse_db_line(line)
            if bits is None:
                continue
            if alias is not None:
                prefix, _, rest = tag.partition('.')
                if prefix == alias[0]:
                    tag = '%s.%s' % (alias[1], rest)
            bitset = frozenset(bits)
            if tag in full:
                if full[tag] != bitset:
                    raise ValueError(
                        "%s: tag %s appears twice with different bits" %
                        (fn, tag))
                continue
            full[tag] = bitset
            entries[tag] = frame_indices(bits)
    return entries


def read_exceptions(fn):
    exceptions = set()
    with open(fn) as f:
        for number, line in enumerate(f, 1):
            line = line.split('#')[0].strip()
            if not line:
                continue
            fields = line.split(None, 4)
            if len(fields) < 5:
                raise ValueError(
                    "%s:%d: want '<file> <tag> <family> <family> <reason>'"
                    % (fn, number))
            fname, tag, fam_a, fam_b, _ = fields
            exceptions.add((fname, tag, frozenset((fam_a, fam_b))))
    return exceptions


def families_in(db_root):
    return sorted(
        name for name in os.listdir(db_root)
        if glob.glob(os.path.join(db_root, name, 'segbits_*.db')))


def run(
        db_root, families=None, exceptions=frozenset(), out=None,
        unused_out=None):
    if out is None:
        out = sys.stdout
    families = families or families_in(db_root)
    by_file = {}
    for family in families:
        for fn in glob.glob(os.path.join(db_root, family, 'segbits_*.db')):
            if fn.endswith('.origin_info.db'):
                continue
            tile = tile_of(os.path.basename(fn))
            key = 'segbits_%s.db' % canonical_tile(tile)
            per_family = by_file.setdefault(key, {})
            if family in per_family:
                raise ValueError(
                    "%s: %s and %s both map to %s" % (
                        family, os.path.basename(per_family[family][0]),
                        os.path.basename(fn), key))
            alias = None
            if canonical_tile(tile) != tile:
                alias = (tile.upper(), canonical_tile(tile).upper())
            per_family[family] = (fn, alias)

    files = pairs = excused = 0
    violations = []
    used = set()
    for fname in sorted(by_file):
        per_family = by_file[fname]
        if len(per_family) < 2:
            continue
        files += 1
        loaded = {
            fam: read_segbits(fn, alias)
            for fam, (fn, alias) in per_family.items()
        }
        for fam_a, fam_b in itertools.combinations(sorted(loaded), 2):
            a, b = loaded[fam_a], loaded[fam_b]
            for tag in sorted(set(a) & set(b)):
                pairs += 1
                if a[tag] == b[tag]:
                    continue
                key = (fname, tag, frozenset((fam_a, fam_b)))
                if key in exceptions:
                    used.add(key)
                    excused += 1
                    continue
                violations.append(
                    "%s %s/%s %s: %s=%s %s=%s" % (
                        fname, fam_a, fam_b, tag, fam_a, sorted(a[tag]),
                        fam_b, sorted(b[tag])))

    for violation in violations:
        print("VIOLATION %s" % violation, file=out)
    unused = sorted(
        (fname, tag, sorted(fams))
        for fname, tag, fams in set(exceptions) - used)
    for fname, tag, fams in unused:
        print(
            "UNUSED EXCEPTION %s %s %s %s" % (fname, tag, fams[0], fams[-1]),
            file=out)
    if unused_out is not None:
        unused_out.extend(unused)
    print(
        "crossfam: %d shared file(s), %d shared tag-pair(s), "
        "%d frame-set violation(s), %d excused" %
        (files, pairs, len(violations), excused),
        file=out)
    return len(violations), pairs - excused - len(violations)


def main():
    parser = argparse.ArgumentParser(
        description="Fail when a feature present in two families' copies of "
        "the same segbits_<tile>.db uses a different set of frame indices in "
        "each. Polarity and word offsets are ignored; only the frames are "
        "compared.",
        epilog="Exit status: 0 when every compared tag-pair agrees and at "
        "least --min-pairs of them agreed; 1 on any frame-set violation or "
        "unused exception; 2 "
        "when there is no verdict: a usage error, a missing --db-root, an "
        "unreadable or malformed input file, or fewer than --min-pairs "
        "agreeing tag-pairs.")
    parser.add_argument(
        '--db-root',
        required=True,
        help='directory holding the family directories (artix7, kintex7, ...)')
    parser.add_argument(
        '--family',
        action='append',
        help='restrict to these families (repeatable); default: every '
        'directory under --db-root holding segbits files')
    parser.add_argument(
        '--exceptions',
        help="file of '<file> <tag> <family> <family> <reason>' lines that "
        'may disagree; <file> and <tag> use the canonical tile name, without '
        'a family suffix such as %s; a line that excuses nothing is an error'
        % '/'.join(VARIANT_SUFFIXES))
    parser.add_argument(
        '--min-pairs',
        type=int,
        default=1,
        help='exit 2 instead of 0 when fewer shared tag-pairs than this '
        'agreed; excused pairs do not count (default: %(default)s)')
    args = parser.parse_args()
    if not os.path.isdir(args.db_root):
        parser.error('--db-root %s is not a directory' % args.db_root)
    if args.min_pairs < 0:
        parser.error('--min-pairs must not be negative')

    try:
        exceptions = read_exceptions(
            args.exceptions) if args.exceptions else frozenset()
        unused = []
        violations, agreed = run(
            args.db_root, args.family, exceptions, unused_out=unused)
    except (OSError, ValueError, AssertionError) as e:
        print("crossfam: no verdict, %s" % e, file=sys.stderr)
        sys.exit(2)
    if violations or unused:
        sys.exit(1)
    if agreed < args.min_pairs:
        print(
            "crossfam: no verdict, %d shared tag-pair(s) agreed, "
            "--min-pairs is %d" % (agreed, args.min_pairs),
            file=sys.stderr)
        sys.exit(2)
    sys.exit(0)


if __name__ == '__main__':
    main()
