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
"""Presence-varying specimens: the cure for segmatch's enable-bit blindness.

WHY THIS EXISTS
---------------
segmatch solves, for every tag T,

    S(T) = (intersection of the bits of every segment where T=1)
           \\ (union of the bits of every segment where T=0)

A segment that does not mention T contributes to neither term.  So:

  * count0 == 0  ->  nothing is ever subtracted.  S(T) is every bit that
                     happens to be set in all the T=1 segments, which is the
                     tag's own bits plus every other bit that is always on.
                     segmatch marks the line `<const1>` and dbfixup drops it.
  * count1 == 0  ->  `<const0>`, emitted with no bits at all.

Neither is a sample-size problem.  A tag with count0 == 0 has **zero leverage at
any specimen count**: more specimens add more of the same support.  The only
cure is a specimen set in which the *presence* of the thing being measured
varies.  See tools/segmatch.cc, the `if (!count1)` / `if (!count0)` arms.

Three of prjxray's bit classes are structurally blind this way, and site enable
bits are the worst of them: a site that every specimen instantiates is
indistinguishable from silicon that is always on.  fuzzers/038-cfg is the
worked example -- it instantiated ICAP_X0Y1 in every specimen and ICAP_X0Y0 in
none, so both ICAP enable bits were invisible, for two different reasons.  On
zynq7 the resulting always-set bits were eventually noticed by the unknown-bit
oracle and hard-coded as ALWAYS_ON_PROP{1,2,3} -- three bits that are, set
identically, prjcombine's ICAP[1].ENABLE_TR.

WHAT THIS MODULE IS NOT
-----------------------
It is not a change to Segmaker, and it does not need one.  Presence tags are
ordinary tags; what makes them solvable is the specimen set, not the tagger.
If a fuzzer using this module seems to need a Segmaker change, the specimen
design is wrong.

USAGE
-----
In top.py, choosing what to instantiate:

    from prjxray import presence
    plan = presence.factorial(sites, presence.specimen_index())
    ...  instantiate site iff plan[site]

In generate.py, exposing the choice to the solver:

    presence.add_presence_tags(segmk, plan)

In the fuzzer Makefile, before segmatch, as a gate:

    python3 -m prjxray.presence $$(find -name 'segdata_*.txt')

which fails loudly if any tag reached the solver without two-sided support --
the check that would have caught 038-cfg in 2019.
"""

import os
import re
import sys

DEFAULT_FEATURE = "ENABLE_TR"


def specimen_index():
    """0-based index of the specimen being generated.

    utils/genheader.sh exports SEEDN as the specimen number with leading zeros
    stripped (`specimen_007` -> `7`), 1-based.  Falling back to the cwd keeps
    this usable from a hand-run generate step outside the make flow.
    """
    seedn = os.getenv("SEEDN")
    if seedn is None:
        m = re.search(r'specimen_0*(\d+)', os.getcwd())
        if not m:
            raise RuntimeError(
                "no SEEDN in the environment and cwd %r is not a specimen "
                "directory -- presence.specimen_index() needs one of them" %
                os.getcwd())
        seedn = m.group(1)
    n = int(seedn)
    assert n >= 1, "SEEDN is 1-based, got %r" % seedn
    return n - 1


def combinations(sites, valid=None):
    """Every presence combination over `sites`, in a fixed order.

    `valid` is an optional predicate on the {site: bool} dict.  It exists
    because presence is not always free: the hardware, or the vendor tool's
    view of it, can forbid combinations.  The worked example is 7-series ICAP,
    where Vivado refuses

        [Place 30-99] The only ICAP instance cannot be placed in the bottom
        ICAP site.  Please unplaced the instance, or lock it in the top site.

    so ICAP_X0Y0 is reachable only alongside ICAP_X0Y1 and the 2**2 factorial
    has to become a 3-point design.  Encoding that as a predicate keeps the
    constraint next to the fuzzer that discovered it and keeps the resulting
    plan deterministic, which a retry loop would not.

    Raises if the predicate leaves any site with only one reachable state --
    that site cannot be solved by ANY specimen set of this shape, and finding
    out at plan time is much cheaper than finding out from a `<const1>`.
    """
    sites = list(sites)
    k = len(sites)
    assert k > 0, "combinations() needs at least one site"
    out = []
    for combo in range(2**k):
        plan = {s: bool((combo >> j) & 1) for j, s in enumerate(sites)}
        if valid is None or valid(plan):
            out.append(plan)
    if not out:
        raise ValueError("the validity predicate rejects every combination")
    for s in sites:
        seen = {p[s] for p in out}
        if len(seen) < 2:
            raise ValueError(
                "site %s is %s in every reachable combination -- no specimen "
                "set of this shape can give segmatch two-sided support for it" %
                (s, "present" if seen == {True} else "absent"))
    return out


def factorial(sites, index, valid=None):
    """Deterministic presence pattern for one specimen.

    Returns {site: bool}, cycling through combinations(sites, valid) as `index`
    advances.  With N >= len(combinations(...)) specimens this gives

        count1 > 0 and count0 > 0 for every site, BY CONSTRUCTION,

    and no two sites share a pattern, so segmatch can separate them and
    dbfixup's remove_ambiguous_solutions has nothing to drop.  Random draws
    give the same properties only with high probability, and "high probability"
    is how 038-cfg's DCIRESET tag ends up `<const1>` on a short run.

    Independence is what matters, not uniformity: two sites whose presence is
    correlated across the specimen set solve to each other's bits as well as
    their own.
    """
    combos = combinations(sites, valid)
    return combos[index % len(combos)]


def factorial_specimens_needed(sites, valid=None):
    """Smallest N for which factorial() covers every reachable combination."""
    return len(combinations(sites, valid))


def random_presence(rng, sites, min_present=None, max_present=None):
    """Independent Bernoulli(1/2) presence, resampled to fit a size window.

    Use this when the number of sites makes a full factorial impractical, or
    when the design cannot tolerate the all-absent or all-present corner (a DRC
    that needs at least one instance, say).  The resampling makes the per-site
    marginal non-uniform when a window is given -- documented rather than
    hidden, because it does not affect solvability, only balance.

    Prefer factorial() when 2**len(sites) <= N.  It is the same idea with the
    probability removed.
    """
    sites = list(sites)
    lo = 0 if min_present is None else min_present
    hi = len(sites) if max_present is None else max_present
    assert lo <= hi, "empty presence window [%s, %s]" % (lo, hi)
    for _ in range(1000):
        pick = {s: bool(rng.randint(0, 1)) for s in sites}
        if lo <= sum(pick.values()) <= hi:
            return pick
    raise RuntimeError(
        "could not draw a presence pattern with %d..%d of %d sites present" %
        (lo, hi, len(sites)))


def presence_tag(site, feature=DEFAULT_FEATURE):
    """The tag name for a site's presence bit.

    Emitted as a TILE tag, not a site tag, and the reason is mechanical.
    Segmaker names a site tag `<TILE_TYPE>.<sitekey>.<name>`, where sitekey for
    anything outside its hard-coded table is the site name with the trailing
    `_X<n>Y<n>` removed (segmaker.py, `name_default`).  Two sites of the same
    type in one tile -- ICAP_X0Y0 and ICAP_X0Y1, which is precisely the case
    that matters -- therefore collapse onto ONE tag name, and the second
    silently overwrites the first in Segmaker.compile.

    Segmaker has a table for exactly this (SLICE_X0/X1, RAMB18_Y0/Y1,
    IOB_Y0/Y1) and the obvious repair is to add ICAP to it.  Do not: an entry
    there changes tag names for every fuzzer that touches the site type, and
    Segmaker is not this problem's owner.  A tile tag's name is passed through
    unchanged, so putting the site name in it is enough:

        CFG_CENTER_MID.ICAP_X0Y0.ENABLE_TR
        CFG_CENTER_MID.ICAP_X0Y1.ENABLE_TR

    which is one component shorter than a site tag would be, unambiguous, and
    lines up 1:1 with prjcombine's ICAP[0].ENABLE_TR / ICAP[1].ENABLE_TR.
    """
    return "%s.%s" % (site, feature)


def add_presence_tags(segmk, plan, feature=DEFAULT_FEATURE):
    """Expose a presence plan to the solver, one tag per site.

    `segmk.sites` is Segmaker's own site -> tile index, built in its
    constructor, so no second grid load is needed and a site name that is not
    in this device fails here rather than as an unused-tag assertion later.
    """
    for site, present in sorted(plan.items()):
        assert site in segmk.sites, "unknown site %s" % (site, )
        segmk.add_tile_tag(
            segmk.sites[site], presence_tag(site, feature), bool(present))


# --------------------------------------------------------------------------
# The gate: two-sided support, measured on the solver's own input
# --------------------------------------------------------------------------


def support_from_segdata(paths):
    """{tag: [count1, count0]} read from segmaker's segdata_*.txt files.

    This reads what segmatch reads, so it measures the real support rather than
    the support the fuzzer's author believed it was producing.
    """
    counts = {}
    for path in paths:
        with open(path) as f:
            for line in f:
                parts = line.split()
                if len(parts) != 3 or parts[0] != "tag":
                    continue
                tag, val = parts[1], int(parts[2])
                c = counts.setdefault(tag, [0, 0])
                c[0 if val else 1] += 1
    return counts


def blind_tags(counts, minimum=1):
    """Tags without `minimum` segments on both sides -- the unsolvable ones."""
    return {
        tag: c
        for tag, c in counts.items()
        if c[0] < minimum or c[1] < minimum
    }


def main():
    import argparse
    p = argparse.ArgumentParser(
        description="Report per-tag two-sided support in segmaker output. "
        "Exits 1 if any tag is structurally unsolvable.")
    p.add_argument("segdata", nargs="+", help="segdata_*.txt files")
    p.add_argument(
        "-m",
        "--minimum",
        type=int,
        default=1,
        help="minimum segments required on each side (default 1)")
    p.add_argument(
        "--only",
        default=None,
        help="regex; only tags matching it are required to have support")
    p.add_argument(
        "--warn",
        action="store_true",
        help="report but exit 0 -- for fuzzers with known-blind tags")
    args = p.parse_args()

    counts = support_from_segdata(args.segdata)
    if not counts:
        print("presence: no tags found in %d file(s)" % len(args.segdata))
        return 2

    bad = blind_tags(counts, args.minimum)
    if args.only is not None:
        rx = re.compile(args.only)
        bad = {t: c for t, c in bad.items() if rx.search(t)}

    print(
        "presence: %d tag(s) over %d segdata file(s), minimum support %d" %
        (len(counts), len(args.segdata), args.minimum))
    for tag in sorted(counts):
        c1, c0 = counts[tag]
        flag = ""
        if tag in bad:
            flag = "  <-- BLIND: %s" % (
                "count1=0, segmatch will emit <const0>" if c1 == 0 else
                "count0=0, segmatch will emit <const1>" if c0 == 0 else
                "under the minimum")
        print("  %-56s count1=%-4d count0=%-4d%s" % (tag, c1, c0, flag))

    if bad:
        print(
            "presence: %d tag(s) have no leverage at ANY specimen count -- "
            "vary the presence of what they measure, do not add specimens" %
            len(bad))
        return 0 if args.warn else 1
    print("presence: OK, every tag has two-sided support")
    return 0


if __name__ == "__main__":
    sys.exit(main())
