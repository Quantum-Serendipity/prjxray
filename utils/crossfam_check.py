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


def frame_indices(bits):
    return frozenset(int(bit.lstrip('!').split('_')[0]) for bit in bits)


def read_segbits(fn):
    entries = {}
    with util.OpenSafeFile(fn) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            tag, bits, mode, _ = util.parse_db_line(line)
            if bits is None:
                continue
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


def run(db_root, families=None, exceptions=frozenset(), out=sys.stdout):
    families = families or families_in(db_root)
    by_file = {}
    for family in families:
        for fn in glob.glob(os.path.join(db_root, family, 'segbits_*.db')):
            if fn.endswith('.origin_info.db'):
                continue
            by_file.setdefault(os.path.basename(fn), {})[family] = fn

    files = pairs = excused = 0
    violations = []
    for fname in sorted(by_file):
        per_family = by_file[fname]
        if len(per_family) < 2:
            continue
        files += 1
        loaded = {fam: read_segbits(fn) for fam, fn in per_family.items()}
        for fam_a, fam_b in itertools.combinations(sorted(loaded), 2):
            a, b = loaded[fam_a], loaded[fam_b]
            for tag in sorted(set(a) & set(b)):
                pairs += 1
                if a[tag] == b[tag]:
                    continue
                if (fname, tag, frozenset((fam_a, fam_b))) in exceptions:
                    excused += 1
                    continue
                violations.append(
                    "%s %s/%s %s: %s=%s %s=%s" % (
                        fname, fam_a, fam_b, tag, fam_a, sorted(a[tag]),
                        fam_b, sorted(b[tag])))

    for violation in violations:
        print("VIOLATION %s" % violation, file=out)
    print(
        "crossfam: %d shared file(s), %d shared tag-pair(s), "
        "%d frame-set violation(s), %d excused" %
        (files, pairs, len(violations), excused),
        file=out)
    return len(violations)


def main():
    parser = argparse.ArgumentParser(
        description="Fail when a feature present in two families' copies of "
        "the same segbits_<tile>.db uses a different set of frame indices in "
        "each. Polarity and word offsets are ignored; only the frames are "
        "compared.")
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
        'may disagree')
    args = parser.parse_args()

    exceptions = read_exceptions(
        args.exceptions) if args.exceptions else frozenset()
    violations = run(args.db_root, args.family, exceptions)
    sys.exit(1 if violations else 0)


if __name__ == '__main__':
    main()
