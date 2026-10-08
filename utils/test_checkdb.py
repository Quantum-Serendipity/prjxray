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
"""Alias declarations for checkdb's strict duplicate detection.

checkdb's --alias-file groups go into parsedb, whose strict mode rejects two
features claiming one bit set unless the database declares them aliases.  The
database does declare them: the HP-bank IOB input spellings nextpnr-xilinx
emits name the same bits.  What the file declares is a set of pairs, because
detection only ever has two tags in hand, and every pair sharing a bit set has
to be declared.  A group of three names is therefore worth exactly its three
pairs -- no less (a declared group has to work) and no more (it must not stand
in for a pair it does not contain).
"""

import os
import tempfile
import unittest

from utils import checkdb
from utils import parsedb

# One bit set, under three of the HP-bank input spellings the alias file
# declares.  Real ones, so the test reads like the case it protects.
BIT_SET = '39_01 38_126 39_127'
SPELLINGS = (
    'LIOB18.IOB_Y0.LVCMOS12_LVCMOS15.IN',
    'LIOB18.IOB_Y0.LVCMOS12_LVCMOS15_LVCMOS18.IN',
    'LIOB18.IOB_Y0.LVCMOS12_LVCMOS15.IN_ONLY',
)


def temp_file(text):
    """Write text to a temporary file and return its path."""
    handle, path = tempfile.mkstemp()
    with os.fdopen(handle, 'w') as f:
        f.write(text)
    return path


def alias_file(*groups):
    """The declared aliases of the groups, as they would be written."""
    path = temp_file(''.join('%s\n' % ' '.join(g) for g in groups))
    return path, checkdb.read_alias_file(path)


class TestReadAliasFile(unittest.TestCase):
    def test_a_line_declares_every_pair_of_its_names(self):
        path, aliases = alias_file(('A', 'B', 'C'), ('D', 'E'))
        self.addCleanup(os.unlink, path)
        self.assertEqual(
            aliases, {
                frozenset(('A', 'B')),
                frozenset(('A', 'C')),
                frozenset(('B', 'C')),
                frozenset(('D', 'E')),
            })

    def test_comments_and_blank_lines_are_ignored(self):
        path = temp_file('# a comment\n\nA B  # the same bits\n   \n')
        self.addCleanup(os.unlink, path)
        self.assertEqual(
            checkdb.read_alias_file(path), {frozenset(('A', 'B'))})

    def test_a_group_of_one_is_rejected(self):
        """A feature sharing its bits with nothing is not an alias."""
        path = temp_file('A\n')
        self.addCleanup(os.unlink, path)
        with self.assertRaises(AssertionError):
            checkdb.read_alias_file(path)


class TestStrictDuplicates(unittest.TestCase):
    """One file per case, its tags all claiming the same bit set."""

    def accepts(self, tags, aliases):
        db = temp_file(''.join('%s %s\n' % (tag, BIT_SET) for tag in tags))
        self.addCleanup(os.unlink, db)
        parsedb.run(db, strict=True, aliases=aliases)

    def rejects(self, tags, aliases):
        db = temp_file(''.join('%s %s\n' % (tag, BIT_SET) for tag in tags))
        self.addCleanup(os.unlink, db)
        with self.assertRaises(AssertionError):
            parsedb.run(db, strict=True, aliases=aliases)

    def test_a_declared_pair_is_accepted(self):
        path, aliases = alias_file(SPELLINGS[:2])
        self.addCleanup(os.unlink, path)
        self.accepts(SPELLINGS[:2], aliases)

    def test_a_declared_three_name_group_is_accepted(self):
        path, aliases = alias_file(SPELLINGS)
        self.addCleanup(os.unlink, path)
        self.accepts(SPELLINGS, aliases)

    def test_overlapping_groups_do_not_cover_what_they_leave_out(self):
        """A B and B C between them still do not say A C."""
        path, aliases = alias_file(SPELLINGS[:2], SPELLINGS[1:])
        self.addCleanup(os.unlink, path)
        self.rejects(SPELLINGS, aliases)

    def test_without_an_alias_file_every_duplicate_is_rejected(self):
        self.rejects(SPELLINGS[:2], None)

    def test_a_duplicate_tag_is_rejected_even_when_declared(self):
        """The same feature twice is malformed however it is declared."""
        path, aliases = alias_file(SPELLINGS[:2])
        self.addCleanup(os.unlink, path)
        self.rejects((SPELLINGS[0], SPELLINGS[0]), aliases)


if __name__ == '__main__':
    unittest.main()
