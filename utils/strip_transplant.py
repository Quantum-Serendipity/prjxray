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
"""
Remove segbits whose recorded provenance is a transplant (a fuzzer that
cannot run on this part, or a foreign-database copy) so the slot can be
re-filled by a real measurement.

For a given <db-type> (e.g. rioi) it edits, in lock-step:
    segbits_<db-type>.db                (TAG bit bit ...)
    segbits_<db-type>.origin_info.db    (TAG origin:<fuzzer> bit bit ...)
A tag is dropped iff its origin_info origin EXACTLY equals one of the
--origins values.  Tags whose origin is a comma-joined chain (i.e. a
transplant later partly overwritten by a real fuzzer) are kept and
reported, since the chain needs the real re-fuzz merged first.

Use --apply to write; default is a dry run.
"""
import argparse
import os
import sys


def load_origins(fn):
    """tag -> origin (from a *.origin_info.db)."""
    origins = {}
    if not os.path.exists(fn):
        return origins
    for line in open(fn):
        parts = line.split()
        if len(parts) < 2:
            continue
        tag = parts[0]
        # The origin token may sit right after the tag (most fuzzers) or at
        # the end of the line (hand-edited entries); search all fields.
        origin = None
        for p in parts[1:]:
            if p.startswith('origin:'):
                origin = p[len('origin:'):]
                break
        origins[tag] = origin
    return origins


def strip_file(fn, drop_tags):
    """Rewrite fn keeping only lines whose first field is not in drop_tags.
    Returns (kept, removed)."""
    kept, removed = [], []
    for line in open(fn):
        tag = line.split(' ', 1)[0].strip() if line.strip() else None
        if tag in drop_tags:
            removed.append(line.rstrip('\n'))
        elif line.strip():
            kept.append(line.rstrip('\n'))
    return kept, removed


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--db-root', required=True)
    ap.add_argument('--db-type', required=True, help='e.g. rioi, lioi_tbytesrc')
    ap.add_argument(
        '--origins', required=True, nargs='+',
        help='origin strings to drop (exact match), e.g. 035-iob-ilogic')
    ap.add_argument('--apply', action='store_true', help='write changes')
    args = ap.parse_args()

    seg = os.path.join(args.db_root, 'segbits_%s.db' % args.db_type)
    org = os.path.join(args.db_root, 'segbits_%s.origin_info.db' % args.db_type)
    if not os.path.exists(org):
        print('  %s: no origin_info db, skipping' % args.db_type)
        return 0

    origins = load_origins(org)
    bad = set(args.origins)
    drop = {t for t, o in origins.items() if o in bad}
    chained = {
        t: o
        for t, o in origins.items()
        if o and ',' in o and any(b in o.split(',') for b in bad)
    }

    print('[%s] drop %d tags with origin in %s' % (args.db_type, len(drop), sorted(bad)))
    if chained:
        print('  NOTE %d chained-origin tags kept (need real re-fuzz merged '
              'first): %s' % (len(chained), sorted(chained)[:4]))

    for fn in (seg, org):
        if not os.path.exists(fn):
            continue
        kept, removed = strip_file(fn, drop)
        print('  %s: %d kept, %d removed' % (os.path.basename(fn), len(kept), len(removed)))
        if args.apply:
            with open(fn, 'w') as f:
                f.write('\n'.join(sorted(kept)) + ('\n' if kept else ''))
    if not args.apply:
        print('  (dry run; pass --apply to write)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
