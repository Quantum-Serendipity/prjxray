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
"""The cross-family frame-index invariant, as a gate.

THE DEFECT THIS REMOVES
=======================
prjxray owns exactly one structural check over an assembled database --
check_tile_overlap (utils/checkdb.py:95), "no two tiles use the same bit". That
check cannot fire on an address-translation error that stays inside one tile's
own address block. The motivating case is the disputed XADC data: two
independent datasets disagree on every shared address by a constant +8 frame
shift, and both frame sets land inside MONITOR_BOT's frames=30 window, so no
other tile's bits are touched. utils/parsedb.py:16 --strict passes both:
each file is individually well-formed. A database containing the wrong one of
the two is indistinguishable, by every check that exists, from a correct one.

The invariant that does discriminate it, and which nothing in the tree states:

    for a feature tag present in segbits_<tile>.db in two families, the SET OF
    FRAME INDICES that feature occupies is the same in both.

Polarity and word offsets are discarded on purpose. Families do legitimately
differ there -- bit membership and polarity differ on a small number of tags
(CLBLM_*.SLICEM_X0.DOUTMUX.MC31, CMT_*_MMCM_CLKFBIN.*, RIOB18.IOB_Y* LVDS) --
so a gate at exact-bit granularity would fire on known-good corpus content and
be turned off within a week. Frame-index granularity is the level at which the
measured disagreement count is zero, which is what makes it usable as a hard
gate rather than an advisory.

WHAT IS PINNED AND WHAT IS ONLY REPORTED
========================================
The INVARIANT is pinned exactly: frame-index disagreements must be 0. The
POPULATION -- how many shared tag-pairs, files and families the sweep found --
is data, floored loosely (--min-pairs). Those two are not interchangeable,
because the corpus moves under both. MEASURED 2026-09-01 over the segbits
corpus (464 files, `*/segbits_*.db`, origin_info twins included), rows counted
as non-empty lines: prjxray-db b45557ed 1,062,116; upstream/master ef46e80
(2026-08-13) 1,062,160; upstream/master b2228e7 (2026-08-18) 1,062,176. Sixteen
rows in five days, and our pin is not on that line at all. The shared tag-pair
population moves with it -- 982,263 at design time on d17fea26, 982,267 here on
b45557ed, the same invariant over four more comparisons. A gate that pinned the
population would turn a corpus that merely grew red and report it as "the
checker is wrong". The floor exists only so that a sweep which read almost
nothing cannot be mistaken for a sweep which found nothing wrong: below it the
verdict is a refusal (2), not a pass.

WHAT THIS CANNOT SEE
====================
A tile type present in only one family has no cross-family partner and this
check is silent on every tag in it -- structurally, not by omission. So is a
tag that exists in only one family of an otherwise-shared file. Both
populations are counted and printed on every run (the `coverage` block),
because a blind spot that is not quantified reads as coverage. Measured on
b45557ed: 22,028 of 130,162 distinct (file, tag) keys have no partner, and six
segbits files are 0 bytes. It is also silent on an error that is CONSISTENTLY
wrong in every family: this is a cross-check, not an oracle.

Usage:
  utils/crossfam_check.py --db-root <prjxray-db> [--exceptions <f.json|f.yaml>]
  utils/crossfam_check.py --self-test [--db-root <prjxray-db>]

Exit 0 when the invariant holds, 1 on a violation, 2 when no verdict is
available (unreadable root, <2 families, no shared files, malformed db line,
population under the floor).
"""
import argparse
import json
import os
import re
import shutil
import sys
import tempfile
import time

# Bit token as written in a segbits_*.db line: optional '!' polarity, then
# <frame>_<word>. Anchored at both ends, unlike prjxray/util.py:305, whose
# re.match has no trailing $ and would accept '30_06junk'. Widening this to
# match that laxity would let a corrupt token parse to a plausible frame.
BIT_RE = re.compile(r'^(!?)([0-9]+)_([0-9]+)$')

# A tag whose value is `always` or `<N candidates>` names no frame. Same set
# prjxray/util.py:298 special-cases.
UNRESOLVED = object()

# The population floor, NOT a pinned population. MEASURED on prjxray-db
# b45557ed, 2026-09-01: 982,267 shared tag-pairs over 48 shared basenames in 5
# families. This is ~79% of that. Losing one family takes the pair count to
# roughly 589 k (the two 36,864-line block_ram dbs alone go from C(5,2)=10 to
# C(4,2)=6 family pairs), which this catches; dropping the largest non-BRAM
# shared tile type costs about 3.7%, which it deliberately does not.
DEFAULT_MIN_PAIRS = 780000

# MEASURED same run: 48 basenames appear in >=2 families, of 75 present.
DEFAULT_MIN_SHARED_FILES = 40


def parse_line(line):
    """-> (tag, bits) with bits a frozenset of raw tokens, or UNRESOLVED.

    A local parser, not prjxray.util.parse_db_line. That function imports yaml
    at prjxray/util.py:22, and this gate's whole claim is that it runs on a
    bare ubuntu-latest in seconds with nothing installed; taking a PyYAML
    dependency to read plain ASCII would give that away. The cost of a second
    parser is that it can drift from the canonical one, so self-test arm
    `selftest_parser_agrees_with_prjxray_util` runs both over real corpus lines
    whenever prjxray.util is importable, and is reported SKIP -- never a pass
    -- when it is not.

    Raises ValueError on anything it cannot account for. Refusing is the point:
    a line silently dropped is a tag this gate then reports as absent from a
    family, which is the same output shape as a real coverage gap.
    """
    parts = line.split()
    if not parts:
        raise ValueError('empty line')
    tag = parts[0]
    if tag == 'bit':
        raise ValueError('mask db line in a segbits db: %s' % line.strip())
    rest = parts[1:]
    if not rest:
        raise ValueError('tag with no value: %s' % line.strip())
    # origin:<fuzzer> stamping lives in the parallel *.origin_info.db files,
    # but parse it anyway: a segbits db that ever carries one must not be read
    # with the origin token mistaken for a bit.
    if rest[0].startswith('origin:'):
        rest = rest[1:]
        if not rest:
            raise ValueError('origin stamp with no value: %s' % line.strip())
    joined = ' '.join(rest)
    if joined == 'always' or '<' in joined:
        return tag, UNRESOLVED
    for tok in rest:
        if not BIT_RE.match(tok):
            raise ValueError('invalid bit %r in: %s' % (tok, line.strip()))
    return tag, frozenset(rest)


def load_db(path):
    """-> ({tag: bits}, redundant_line_count).

    A repeated tag whose bit SET is identical is a redundant line, not a
    contradiction: 366 of them ship today across 10 virtex7 files
    (virtex7/segbits_bram_l.db:1-2 is one -- `26_32 !26_33 26_35` and
    `!26_33 26_32 26_35`, the same three bits in two token orders, which
    prjxray/util.py:326's sorted writer would have collapsed). They are counted
    and reported, not refused; refusing them would make this gate unusable on
    the corpus it exists to guard. utils/parsedb.py:32 does refuse them, which
    is why db-check-virtex7 cannot currently pass -- a separate defect.

    A repeated tag with DIFFERENT bits is a family disagreeing with itself, so
    there is no value to compare against another family. That is a refusal.
    mergedb.py:65 draws the line in the same place.
    """
    out = {}
    redundant = 0
    with open(path) as f:
        for n, line in enumerate(f, 1):
            if not line.strip():
                continue
            try:
                tag, bits = parse_line(line)
            except ValueError as exc:
                raise ValueError('%s:%d: %s' % (path, n, exc))
            if tag in out:
                if out[tag] == bits:
                    redundant += 1
                    continue
                raise ValueError(
                    '%s:%d: tag %s stated twice with different bits' %
                    (path, n, tag))
            out[tag] = bits
    return out, redundant


def frames_of(bits):
    return frozenset(int(BIT_RE.match(b).group(2)) for b in bits)


def footprint_of(bits):
    return frozenset(
        (int(m.group(2)), int(m.group(3)))
        for m in (BIT_RE.match(b) for b in bits))


def discover(db_root):
    """-> {family: {basename: path}} over segbits_*.db, origin_info excluded.

    A family is a subdirectory that actually contains segbits files, not any
    subdirectory: .git and .github are directories under a prjxray-db checkout
    and neither is a device family.
    """
    fams = {}
    for name in sorted(os.listdir(db_root)):
        d = os.path.join(db_root, name)
        if name.startswith('.') or not os.path.isdir(d):
            continue
        files = {}
        for fn in sorted(os.listdir(d)):
            if not fn.startswith('segbits_') or not fn.endswith('.db'):
                continue
            # The .origin_info.db twin holds `origin:<fuzzer>` provenance, not
            # bits; checkdb.py:80 skips it by the same substring test.
            if fn.endswith('.origin_info.db'):
                continue
            files[fn] = os.path.join(d, fn)
        if files:
            fams[name] = files
    return fams


def tile_type_of(basename):
    """segbits_bram_l.block_ram.db -> bram_l.

    Named separately because the file count and the tile-type count are
    different quantities and the coverage figure needs the second: BRAM_L ships
    as two files (the tile db and its .block_ram block-type db), so counting
    files would report one tile type twice.
    """
    stem = basename[len('segbits_'):-len('.db')]
    return stem.split('.', 1)[0]


def load_exceptions(path):
    """-> set of (file, tag, famA, famB) with famA < famB.

    The design (p2-5-artefact-pipeline-design.md section 2.1) names this file
    database/crossfam_exceptions.yaml. JSON is accepted too and is what the
    self-test uses, because reading YAML needs PyYAML and this gate otherwise
    needs nothing. A .yaml path with no PyYAML present is a refusal, never a
    silently-empty allow-list.
    """
    if path.endswith(('.yaml', '.yml')):
        try:
            import yaml
        except ImportError:
            raise ValueError(
                '%s needs PyYAML, which is not installed; convert the '
                'allow-list to .json or install PyYAML' % path)
        with open(path) as f:
            doc = yaml.safe_load(f)
    else:
        with open(path) as f:
            doc = json.load(f)
    if doc is None:
        doc = []
    if not isinstance(doc, list):
        raise ValueError('%s: want a list of entries, got %s' %
                         (path, type(doc).__name__))
    out = set()
    for i, e in enumerate(doc):
        if not isinstance(e, dict):
            raise ValueError('%s: entry %d is not a mapping' % (path, i))
        missing = [k for k in ('file', 'tag', 'famA', 'famB', 'reason')
                   if not e.get(k)]
        if missing:
            # An exception with no reason is an unattributable suppression: the
            # next reader cannot tell whether the disagreement was adjudicated
            # or merely annoying. Refuse rather than honour it.
            raise ValueError('%s: entry %d missing %s' %
                             (path, i, ', '.join(missing)))
        a, b = sorted((e['famA'], e['famB']))
        out.add((e['file'], e['tag'], a, b))
    return out


def sweep(db_root, exceptions=frozenset()):
    """-> report dict. Raises ValueError on anything that forbids a verdict."""
    t0 = time.time()
    fams = discover(db_root)
    if len(fams) < 2:
        raise ValueError(
            'need >=2 families under %s, found %d (%s)' %
            (db_root, len(fams), ', '.join(sorted(fams)) or 'none'))

    by_file = {}
    for fam, files in fams.items():
        for base in files:
            by_file.setdefault(base, []).append(fam)

    shared = sorted(b for b, f in by_file.items() if len(f) >= 2)
    single = sorted(b for b, f in by_file.items() if len(f) == 1)
    if not shared:
        raise ValueError('no segbits basename appears in >=2 families under %s'
                         % db_root)

    rep = {
        'db_root': os.path.abspath(db_root),
        'families': sorted(fams),
        'files_total': len(by_file),
        'files_shared': len(shared),
        'files_single_family': len(single),
        'single_family_files': single,
        'pairs': 0,
        'unresolved_pairs': 0,
        'frame_violations': [],
        'footprint_disagree': 0,
        'exact_disagree': 0,
        'exceptions_used': set(),
        'exceptions_suppressed': 0,
        'tags_total': 0,
        'tags_single_family': 0,
        'tags_single_family_in_shared_file': 0,
        'redundant_lines': 0,
        'empty_files': [],
    }

    for base in sorted(by_file):
        fam_list = sorted(by_file[base])
        dbs = {}
        for f in fam_list:
            dbs[f], red = load_db(fams[f][base])
            rep['redundant_lines'] += red
            # A zero-tag db states no knowledge, and it reaches every count in
            # this report as an ordinary file -- virtex7/segbits_mask_liob18.db
            # is 0 bytes on disk today. Name it, or "75 files" reads as 75
            # files' worth of evidence.
            if not dbs[f]:
                rep['empty_files'].append('%s/%s' % (f, base))

        # Coverage accounting runs over EVERY file, shared or not: a tag in a
        # single-family file is exactly as invisible to the invariant as a
        # single-family tag inside a shared file, and reporting only the first
        # would understate the blind spot.
        seen = {}
        for f in fam_list:
            for tag in dbs[f]:
                seen[tag] = seen.get(tag, 0) + 1
        rep['tags_total'] += len(seen)
        lonely = sum(1 for c in seen.values() if c == 1)
        rep['tags_single_family'] += lonely
        if len(fam_list) >= 2:
            rep['tags_single_family_in_shared_file'] += lonely

        if len(fam_list) < 2:
            continue

        for i in range(len(fam_list)):
            for j in range(i + 1, len(fam_list)):
                fa, fb = fam_list[i], fam_list[j]
                da, db_ = dbs[fa], dbs[fb]
                if len(da) > len(db_):
                    da, db_, fa, fb = db_, da, fb, fa
                for tag, bits_a in da.items():
                    bits_b = db_.get(tag)
                    if bits_b is None:
                        continue
                    if bits_a is UNRESOLVED or bits_b is UNRESOLVED:
                        # `always` / `<N candidates>` names no frame. Counting
                        # it as agreement would inflate the population with
                        # comparisons the invariant never made.
                        rep['unresolved_pairs'] += 1
                        continue
                    rep['pairs'] += 1
                    if bits_a != bits_b:
                        rep['exact_disagree'] += 1
                        if footprint_of(bits_a) != footprint_of(bits_b):
                            rep['footprint_disagree'] += 1
                        fr_a, fr_b = frames_of(bits_a), frames_of(bits_b)
                        if fr_a != fr_b:
                            key = (base, tag) + tuple(sorted((fa, fb)))
                            if key in exceptions:
                                rep['exceptions_used'].add(key)
                                rep['exceptions_suppressed'] += 1
                                continue
                            lo, hi = sorted((fa, fb))
                            rep['frame_violations'].append(
                                (base, tag, lo, hi,
                                 sorted(frames_of(dbs[lo][tag])),
                                 sorted(frames_of(dbs[hi][tag]))))
        del dbs

    # An allow-list entry that matches nothing is a suppression with no
    # subject: the corpus moved out from under it and the key now pre-excuses
    # whatever disagreement lands there next. Report it as a violation, not a
    # warning.
    rep['exceptions_stale'] = sorted(set(exceptions) - rep['exceptions_used'])
    rep['exceptions_used'] = sorted(rep['exceptions_used'])
    rep['seconds'] = round(time.time() - t0, 2)
    return rep


def emit(rep, min_pairs, min_shared_files):
    """-> exit code. Population printed as data, invariant as a verdict."""
    print('crossfam-check: %s' % rep['db_root'])
    print('  families            : %d  (%s)' %
          (len(rep['families']), ' '.join(rep['families'])))
    print('  segbits files       : %d total, %d shared by >=2 families, '
          '%d single-family' %
          (rep['files_total'], rep['files_shared'],
           rep['files_single_family']))
    print('  shared tag-pairs    : %d   (unresolved, not compared: %d)' %
          (rep['pairs'], rep['unresolved_pairs']))
    print('    exact bit-set disagree     : %d' % rep['exact_disagree'])
    print('    address-footprint disagree : %d' % rep['footprint_disagree'])
    print('    frame-index-set disagree   : %d' % len(rep['frame_violations']))
    if rep['exceptions_suppressed']:
        print('    ...of which allow-listed   : %d' %
              rep['exceptions_suppressed'])
    if rep['empty_files']:
        print('  empty db files      : %d  (%s)' %
              (len(rep['empty_files']), ' '.join(rep['empty_files'])))
    if rep['redundant_lines']:
        print('  redundant lines     : %d  (tag restated with the same bit '
              'set; not a disagreement, see load_db)' % rep['redundant_lines'])
    print('  elapsed             : %.2f s' % rep['seconds'])

    # THE BLIND SPOT, PRINTED ON EVERY RUN. Ratios, not bare zeroes: "0
    # violations" and "0 violations over N comparisons of M/T tags" are both
    # green and they say different things.
    covered = rep['tags_total'] - rep['tags_single_family']
    pct = 100.0 * covered / rep['tags_total'] if rep['tags_total'] else 0.0
    print('  coverage            : %d/%d distinct (file,tag) keys are present '
          'in >=2 families (%.1f%%)' % (covered, rep['tags_total'], pct))
    print('                        %d single-family files carry no '
          'cross-family partner at all' % rep['files_single_family'])
    print('                        %d further tags are single-family inside a '
          'shared file' % rep['tags_single_family_in_shared_file'])
    if rep['files_single_family']:
        # STEMS, NOT VALIDATED TILE TYPES. tile_type_of() strips the block-type
        # suffix so BRAM_L's two files count once, but it cannot know that
        # segbits_mask_liob18.db names no tile at all. The number the coverage
        # claim needs is "tile types with no cross-family partner"; the number
        # printed is "distinct basename stems", and they are not the same noun.
        types = sorted({tile_type_of(b) for b in rep['single_family_files']})
        print('                        %d single-family basename stems: %s' %
              (len(types), ' '.join(types)))

    for base, tag, fa, fb, ba, bb in rep['frame_violations'][:200]:
        print('  VIOLATION %s %s/%s %s: %s=%s %s=%s' %
              (base, fa, fb, tag, fa, ba, fb, bb))
    if len(rep['frame_violations']) > 200:
        print('  ... %d further violations suppressed' %
              (len(rep['frame_violations']) - 200))
    for key in rep['exceptions_stale']:
        print('  STALE-EXCEPTION %s %s %s/%s matched nothing' % key)

    # Order matters. A population under the floor means the sweep read almost
    # nothing, so "0 violations" carries no information -- refuse (2) before
    # reporting a pass on it. A real violation is still a 1 even from a thin
    # corpus, because a found defect is a verdict.
    if rep['frame_violations'] or rep['exceptions_stale']:
        print('crossfam-check: FAIL (%d frame-index violations, %d stale '
              'exceptions)' %
              (len(rep['frame_violations']), len(rep['exceptions_stale'])))
        return 1
    if rep['pairs'] < min_pairs or rep['files_shared'] < min_shared_files:
        print('crossfam-check: REFUSED -- population under floor: pairs %d '
              '(floor %d), shared files %d (floor %d). 0 violations over a '
              'corpus this thin is not a pass.' %
              (rep['pairs'], min_pairs, rep['files_shared'], min_shared_files),
              file=sys.stderr)
        return 2
    print('crossfam-check: PASS  frame-index-set disagreements = 0 over '
          '%d shared tag-pairs' % rep['pairs'])
    return 0


# ---------------------------------------------------------------------------
#  self-test
# ---------------------------------------------------------------------------
# The synthetic corpus. Two families, one shared file, one tag apiece; each arm
# overrides what it needs. Frames are 2-digit and words 2-digit throughout so
# that a shift in one is never confusable with a shift in the other.
BASE = {
    'famA': {
        'segbits_shared.db': [
            'TILE.FEAT.ON 30_06 30_07',
            'TILE.FEAT.OFF !31_10 31_11',
        ],
    },
    'famB': {
        'segbits_shared.db': [
            'TILE.FEAT.ON 30_06 30_07',
            'TILE.FEAT.OFF !31_10 31_11',
        ],
    },
}


def _write_corpus(root, corpus):
    for fam, files in corpus.items():
        d = os.path.join(root, fam)
        os.makedirs(d, exist_ok=True)
        for base, lines in files.items():
            with open(os.path.join(d, base), 'w') as f:
                f.write('\n'.join(lines) + '\n')
    return root


def _variant(**over):
    """BASE with famB's shared file replaced, or extra families/files added."""
    c = {f: {b: list(l) for b, l in files.items()}
         for f, files in BASE.items()}
    for fam, files in over.items():
        c.setdefault(fam, {})
        for base, lines in files.items():
            if lines is None:
                c[fam].pop(base, None)
            else:
                c[fam][base] = list(lines)
        if not c[fam]:
            c.pop(fam)
    return c


def _mirror(db_root, dest):
    """Symlink mirror of a real prjxray-db, so one file can be mutated cheaply.

    The mirror is the negative control's subject too, not the original tree:
    otherwise the positive arm would be testing the mirror mechanism as much as
    the checker.
    """
    fams = discover(db_root)
    for fam, files in fams.items():
        d = os.path.join(dest, fam)
        os.makedirs(d, exist_ok=True)
        for base, path in files.items():
            os.symlink(os.path.abspath(path), os.path.join(d, base))
    return fams


def _seed_shift(mirror_root, fam, base, real_path, shift, axis):
    """Replace one mirrored file with a copy whose every bit moves by `shift`.

    axis='frame' reproduces the XADC defect shape -- a constant frame offset
    inside the tile's own address window, which utils/checkdb.py:95 cannot see.
    axis='word' moves the same distance along the axis the invariant discards,
    and must NOT be reported as a frame violation.
    """
    out = []
    touched = []
    with open(real_path) as f:
        for line in f:
            if not line.strip():
                continue
            tag, bits = parse_line(line)
            if bits is UNRESOLVED or touched:
                out.append(line.rstrip('\n'))
                continue
            moved = []
            for b in sorted(bits):
                m = BIT_RE.match(b)
                fr, wd = int(m.group(2)), int(m.group(3))
                if axis == 'frame':
                    fr += shift
                else:
                    wd += shift
                moved.append('%s%d_%02d' % (m.group(1), fr, wd))
            out.append(' '.join([tag] + sorted(moved)))
            touched.append(tag)
    p = os.path.join(mirror_root, fam, base)
    os.unlink(p)
    with open(p, 'w') as f:
        f.write('\n'.join(out) + '\n')
    return touched[0]


def self_test(db_root=None):
    cases = []

    def case(name, want_rc, corpus, exceptions=None):
        cases.append((name, want_rc, corpus, exceptions))

    case('clean_identical', 0, _variant())
    # THE CRITICAL ARM: the XADC shape. Same tag, every frame +8, words
    # untouched. Nothing else in the tree fires on this.
    case('frame_shift_plus_8', 1, _variant(
        famB={'segbits_shared.db': ['TILE.FEAT.ON 38_06 38_07',
                                    'TILE.FEAT.OFF !31_10 31_11']}))
    # A frame SET difference that is not a shift: one family carries an extra
    # frame. Set inequality, not offset detection, is what is being asserted.
    case('extra_frame', 1, _variant(
        famB={'segbits_shared.db': ['TILE.FEAT.ON 28_06 30_06 30_07',
                                    'TILE.FEAT.OFF !31_10 31_11']}))
    # THE TWO GRANULARITY GUARDS. Both are real corpus shapes -- 76 exact-level
    # and 74 footprint-level disagreements over prjxray-db b45557ed, measured
    # by this script -- and both must stay green, or the gate fires on
    # known-good content and gets switched off.
    case('polarity_only_is_clean', 0, _variant(
        famB={'segbits_shared.db': ['TILE.FEAT.ON !30_06 30_07',
                                    'TILE.FEAT.OFF !31_10 31_11']}))
    case('word_only_is_clean', 0, _variant(
        famB={'segbits_shared.db': ['TILE.FEAT.ON 30_06 30_09',
                                    'TILE.FEAT.OFF !31_10 31_11']}))
    # VACUITY TRAP A: no basename in two families. Zero violations over zero
    # comparisons must refuse, not pass.
    case('no_shared_file_is_refused', 2, _variant(
        famB={'segbits_shared.db': None,
              'segbits_lonely.db': ['TILE.FEAT.ON 30_06']}))
    # VACUITY TRAP B: one family. Same reasoning, one level up.
    case('one_family_is_refused', 2,
         _variant(famB={'segbits_shared.db': None}))
    # VACUITY TRAP C: a corpus that clears every structural precondition but is
    # far too small to have exercised the invariant. Guarded by the floor,
    # which the arms above must therefore run with disabled.
    case('under_population_floor_is_refused', 2, _variant())
    # A malformed line is a refusal, never a silent skip: a dropped line is a
    # tag this gate then reports as absent, which is the same output shape as a
    # genuine coverage gap.
    case('malformed_bit_is_refused', 2, _variant(
        famB={'segbits_shared.db': ['TILE.FEAT.ON 30_06 3006',
                                    'TILE.FEAT.OFF !31_10 31_11']}))
    # A tag restated with the same bits in a different token order is what 366
    # shipped virtex7 lines look like; it must not stop the sweep.
    case('duplicate_tag_same_bits_is_clean', 0, _variant(
        famB={'segbits_shared.db': ['TILE.FEAT.ON 30_06 30_07',
                                    'TILE.FEAT.ON 30_07 30_06',
                                    'TILE.FEAT.OFF !31_10 31_11']}))
    # A tag restated with DIFFERENT bits leaves the family with no single value
    # to compare, so there is no verdict to give.
    case('duplicate_tag_different_bits_is_refused', 2, _variant(
        famB={'segbits_shared.db': ['TILE.FEAT.ON 30_06 30_07',
                                    'TILE.FEAT.ON 30_06 30_08',
                                    'TILE.FEAT.OFF !31_10 31_11']}))
    # `always` / `<N candidates>` name no frame; they are excluded from the
    # population rather than counted as agreement.
    case('unresolved_is_not_a_violation', 0, _variant(
        famB={'segbits_shared.db': ['TILE.FEAT.ON always',
                                    'TILE.FEAT.OFF !31_10 31_11']}))
    # The allow-list, both directions.
    case('exception_suppresses', 0, _variant(
        famB={'segbits_shared.db': ['TILE.FEAT.ON 38_06 38_07',
                                    'TILE.FEAT.OFF !31_10 31_11']}),
         exceptions=[{'file': 'segbits_shared.db', 'tag': 'TILE.FEAT.ON',
                      'famA': 'famA', 'famB': 'famB', 'reason': 'self-test'}])
    case('exception_without_reason_is_refused', 2, _variant(),
         exceptions=[{'file': 'segbits_shared.db', 'tag': 'TILE.FEAT.ON',
                      'famA': 'famA', 'famB': 'famB'}])
    case('stale_exception_is_a_violation', 1, _variant(),
         exceptions=[{'file': 'segbits_shared.db', 'tag': 'TILE.NOPE',
                      'famA': 'famA', 'famB': 'famB', 'reason': 'self-test'}])
    # A third family, to prove the pairing is over every unordered pair rather
    # than over one designated reference family.
    case('third_family_disagrees_with_both', 1, _variant(
        famC={'segbits_shared.db': ['TILE.FEAT.ON 38_06 38_07',
                                    'TILE.FEAT.OFF !31_10 31_11']}))

    npass = nfail = 0
    d = tempfile.mkdtemp(prefix='crossfam-selftest.')
    try:
        for name, want, corpus, exc in cases:
            root = _write_corpus(os.path.join(d, name), corpus)
            floor = DEFAULT_MIN_PAIRS if name.endswith('_floor_is_refused') \
                else 0
            files_floor = DEFAULT_MIN_SHARED_FILES if floor else 0
            try:
                ex = frozenset()
                if exc is not None:
                    p = os.path.join(root, 'exceptions.json')
                    with open(p, 'w') as f:
                        json.dump(exc, f)
                    ex = load_exceptions(p)
                rep = sweep(root, ex)
                rc = emit_quiet(rep, floor, files_floor)
            except ValueError:
                rc = 2
            good = (rc == want)
            print('  %s selftest_%s: want rc=%d got rc=%d' %
                  ('PASS' if good else 'FAIL', name, want, rc))
            npass, nfail = npass + good, nfail + (not good)

        npass, nfail = _self_test_real(db_root, d, npass, nfail)
    finally:
        shutil.rmtree(d, ignore_errors=True)
    print('crossfam-check --self-test: %d PASS, %d FAIL' % (npass, nfail))
    return 0 if nfail == 0 else 1


def emit_quiet(rep, min_pairs, min_shared_files):
    """emit() with output discarded: the self-test asserts on rc, not text."""
    out, err = sys.stdout, sys.stderr
    sys.stdout = sys.stderr = open(os.devnull, 'w')
    try:
        return emit(rep, min_pairs, min_shared_files)
    finally:
        sys.stdout.close()
        sys.stdout, sys.stderr = out, err


def _self_test_real(db_root, workdir, npass, nfail):
    """The arms that use the real corpus. SKIP is never counted as a pass.

    A checker that has only ever run on a two-line synthetic corpus has not
    been shown to survive the real one: real files carry multi-dot basenames,
    36,864-line block_ram dbs, six 0-byte files, 366 redundant duplicate lines
    and five families at once. They carry no `always` or `<N candidates>` line
    at all -- measured, 0 hits over all 232 segbits files against 44 in
    artix7/ppips_int_l.db -- so that branch is fed synthetically below.
    """
    db_root = db_root or os.environ.get('XRAY_DB_ROOT') or \
        os.path.expanduser('~/Repos/xc7dpr-forks/prjxray-db')
    if not os.path.isdir(db_root) or len(discover(db_root)) < 2:
        print('  SKIP selftest_real_corpus_*: no prjxray-db at %r '
              '-- NOT COUNTED AS A PASS' % db_root)
        print('  SKIP selftest_parser_agrees_with_prjxray_util: '
              'needs the same db -- NOT COUNTED AS A PASS')
        return npass, nfail

    mirror = os.path.join(workdir, 'mirror')
    fams = _mirror(db_root, mirror)
    rep = sweep(mirror)
    good = (not rep['frame_violations'] and rep['pairs'] >= DEFAULT_MIN_PAIRS)
    print('  %s selftest_real_corpus_negative: %d violations over %d pairs '
          '(want 0 violations, >=%d pairs)' %
          ('PASS' if good else 'FAIL', len(rep['frame_violations']),
           rep['pairs'], DEFAULT_MIN_PAIRS))
    npass, nfail = npass + good, nfail + (not good)

    # Pick a file that really is shared, and a family that really has it,
    # rather than hardcoding a name that a future corpus may drop.
    by_file = {}
    for fam, files in fams.items():
        for base in files:
            by_file.setdefault(base, []).append(fam)
    shared = sorted(b for b, f in by_file.items()
                    if len(f) >= 2 and not b.endswith('.block_ram.db'))
    victim_base = shared[0]
    victim_fam = sorted(by_file[victim_base])[0]
    real_path = fams[victim_fam][victim_base]

    for axis, shift, want_hit in (('frame', 8, True), ('word', 8, False)):
        m2 = os.path.join(workdir, 'mirror_%s' % axis)
        _mirror(db_root, m2)
        tag = _seed_shift(m2, victim_fam, victim_base, real_path, shift, axis)
        r2 = sweep(m2)
        hits = [v for v in r2['frame_violations'] if v[1] == tag]
        # THE WORD ARM'S VACUITY TRAP. "0 frame violations" is also what a
        # mutation that never landed produces, so the arm additionally demands
        # that the footprint counter MOVED: the seeded bits did change, and the
        # invariant declined to call it a violation because it is not one.
        moved = r2['footprint_disagree'] > rep['footprint_disagree']
        good = bool(hits) == want_hit and \
            (len(r2['frame_violations']) > 0) == want_hit and moved
        print('  %s selftest_real_corpus_seeded_%s_shift: %s %s %s+%d -> '
              '%d frame violations (%d naming the seeded tag), '
              'footprint disagreements %d -> %d, want %s' %
              ('PASS' if good else 'FAIL', axis, victim_fam, victim_base,
               axis, shift, len(r2['frame_violations']), len(hits),
               rep['footprint_disagree'], r2['footprint_disagree'],
               'a hit' if want_hit else 'none'))
        if hits:
            b, t, fa, fb, ba, bb = hits[0]
            print('        seeded violation as reported: %s %s/%s %s: '
                  '%s=%s %s=%s' % (b, fa, fb, t, fa, ba[:6], fb, bb[:6]))
        npass, nfail = npass + good, nfail + (not good)
        shutil.rmtree(m2, ignore_errors=True)

    # THE ARM THAT TIES THE LOCAL PARSER TO THE CANONICAL ONE. parse_line() is
    # a second implementation of prjxray/util.py:278; if it drifted, every arm
    # above would be measuring a fiction. Run over every line of a whole real
    # family whenever prjxray.util imports -- it needs PyYAML, which this gate
    # does not.
    try:
        sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
        from prjxray import util as xutil
    except Exception as exc:                                   # noqa: BLE001
        print('  SKIP selftest_parser_agrees_with_prjxray_util: %s '
              '-- NOT COUNTED AS A PASS' % exc)
        return npass, nfail
    n = mismatch = 0
    # The whole victim family, not one file: 232 corpus files carry zero
    # `always` and zero `<N candidates>` lines, so the UNRESOLVED branch is
    # unreachable from real data and is fed synthetically below instead.
    corpus = sorted(fams[victim_fam].values())
    synthetic = ['INT_L.BYP_BOUNCE5.BYP_ALT5 always',
                 'INT_L.FOO.BAR <0 candidates>',
                 'INT_L.FOO.BAZ <const0>']
    for line in _iter_lines(corpus, synthetic):
        n += 1
        tag_a, bits_a = parse_line(line)
        tag_b, bits_b, _mode, _origin = xutil.parse_db_line(line.strip())
        ours = None if bits_a is UNRESOLVED else set(bits_a)
        theirs = None if bits_b is None else set(bits_b)
        if tag_a != tag_b or ours != theirs:
            mismatch += 1
            print('        parser disagreement: %r ours=(%r,%r) '
                  'prjxray=(%r,%r)' % (line.strip(), tag_a, ours, tag_b,
                                       theirs))
    good = (n > 0 and mismatch == 0)
    print('  %s selftest_parser_agrees_with_prjxray_util: %d lines over %d '
          '%s files + %d synthetic mode lines, %d disagreements '
          '(want >0 lines, 0 disagreements)' %
          ('PASS' if good else 'FAIL', n, len(corpus), victim_fam,
           len(synthetic), mismatch))
    return npass + good, nfail + (not good)


def _iter_lines(paths, extra):
    for p in paths:
        with open(p) as f:
            for line in f:
                if line.strip():
                    yield line
    for line in extra:
        yield line


def main():
    p = argparse.ArgumentParser(
        description='Assert the cross-family frame-index invariant over a '
                    'prjxray-db checkout')
    p.add_argument('--db-root', help='prjxray-db checkout (the directory '
                                     'holding artix7/, kintex7/, ...)')
    p.add_argument('--exceptions', help='allow-list, .json or .yaml; entries '
                                        'need file, tag, famA, famB, reason')
    p.add_argument('--min-pairs', type=int, default=DEFAULT_MIN_PAIRS,
                   help='refuse a verdict under this many shared tag-pairs '
                        '(default %(default)d; the population is data, only '
                        'the 0-violation invariant is pinned)')
    p.add_argument('--min-shared-files', type=int,
                   default=DEFAULT_MIN_SHARED_FILES, help=argparse.SUPPRESS)
    p.add_argument('--json', help='write the report here')
    p.add_argument('--self-test', action='store_true')
    args = p.parse_args()

    if args.self_test:
        return self_test(args.db_root)

    db_root = args.db_root or os.environ.get('XRAY_DB_ROOT')
    if not db_root:
        print('crossfam-check: --db-root or XRAY_DB_ROOT is required',
              file=sys.stderr)
        return 2
    if not os.path.isdir(db_root):
        print('crossfam-check: no such directory: %s' % db_root,
              file=sys.stderr)
        return 2
    try:
        exceptions = load_exceptions(args.exceptions) if args.exceptions \
            else frozenset()
        rep = sweep(db_root, exceptions)
    except (ValueError, OSError) as exc:
        print('crossfam-check: %s' % exc, file=sys.stderr)
        return 2
    rc = emit(rep, args.min_pairs, args.min_shared_files)
    if args.json:
        with open(args.json, 'w') as f:
            json.dump({k: v for k, v in rep.items()
                       if k != 'single_family_files'},
                      f, indent=1, sort_keys=True)
    return rc


if __name__ == '__main__':
    sys.exit(main())
