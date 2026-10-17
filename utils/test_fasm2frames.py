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
# TODO: need better coverage for different tile types

import contextlib
from io import StringIO
import os
import os.path
import re
import shutil
import sys
import unittest
import tempfile
from unittest import mock

import prjxray
import prjxray.grid_types
import prjxray.tile_segbits
from prjxray import cross_tile_features
from prjxray import fasm_assembler
from prjxray.db import Database
from prjxray.roi import Roi
import utils.fasm2frames as fasm2frames
from prjxray.util import OpenSafeFile

from textx.exceptions import TextXSyntaxError


def frm2bits(txt):
    '''
    Convert output .frm file text to set of (frame addr, word #, bit index) tuples
    '''
    bits_out = set()
    for l in txt.split('\n'):
        l = l.strip()
        if not l:
            continue
        # 0x00020500 0x00000000,0x00000000,0x00000000,...
        addr, words = l.split(' ')
        addr = int(addr, 0)
        words = words.split(',')
        assert (101 == len(words))
        for wordi, word in enumerate(words):
            word = int(word, 0)
            for biti in range(32):
                val = word & (1 << biti)
                if val:
                    bits_out.add((addr, wordi, biti))
    return bits_out


def bitread2bits(txt):
    '''
    Convert .bits text file (ie bitread output) to set of (frame addr, word #, bit index) tuples
    '''
    bits_ref = set()
    for l in txt.split('\n'):
        l = l.strip()
        if not l:
            continue
        # bit_0002050b_004_14
        m = re.match(r'bit_(.{8})_(.{3})_(.{2})', l)
        addr = int(m.group(1), 16)
        word = int(m.group(2), 10)
        bit = int(m.group(3), 10)
        bits_ref.add((addr, word, bit))
    return bits_ref


class TestStringMethods(unittest.TestCase):
    def filename_test_data(self, fname):
        return os.path.join(os.path.dirname(__file__), 'test_data', fname)

    def get_test_data(self, fname):
        with OpenSafeFile(self.filename_test_data(fname)) as f:
            return f.read()

    def fasm2frames(self, fin_data, **kw):
        with tempfile.NamedTemporaryFile(suffix='.fasm') as fin:
            fin.write(fin_data.encode('utf-8'))
            fin.flush()

            fout = StringIO()
            fasm2frames.run(
                self.filename_test_data('db'), "xc7a200tffg1156-1", fin.name,
                fout, **kw)

            return fout.getvalue()

    def test_lut(self):
        '''Simple smoke test on just the LUTs'''
        self.fasm2frames(self.get_test_data('lut.fasm'))

    def bitread_frm_equals(self, frm_fn, bitread_fn):
        fout = self.fasm2frames(self.get_test_data(frm_fn))

        # Build a list of output used bits
        bits_out = frm2bits(fout)

        # Build a list of reference used bits
        bits_ref = bitread2bits(self.get_test_data(bitread_fn))

        # Now check for equivilence vs reference design
        self.assertEqual(len(bits_ref), len(bits_out))
        self.assertEqual(bits_ref, bits_out)

    def test_lut_int(self):
        self.bitread_frm_equals('lut_int.fasm', 'lut_int/design.bits')

    def test_ff_int(self):
        self.bitread_frm_equals('ff_int.fasm', 'ff_int/design.bits')

    @unittest.skip
    def test_ff_int_op1(self):
        '''Omitted key set to '''
        self.bitread_frm_equals('ff_int_op1.fasm', 'ff_int/design.bits')

    # Same check as above, but isolated test case
    def test_opkey_01_default(self):
        '''Optional key with binary omitted value should produce valid result'''
        self.fasm2frames("CLBLM_L_X10Y102.SLICEM_X0.SRUSEDMUX")

    @unittest.skip
    def test_opkey_01_1(self):
        self.fasm2frames("CLBLM_L_X10Y102.SLICEM_X0.SRUSEDMUX 1")

    @unittest.skip
    def test_opkey_enum(self):
        '''Optional key with enumerated value should produce syntax error'''
        try:
            # CLBLM_L.SLICEM_X0.AMUXFF.O6 !30_06 !30_07 !30_08 30_11
            self.fasm2frames("CLBLM_L_X10Y102.SLICEM_X0.AFFMUX.O6")
            self.fail("Expected syntax error")
        except TextXSyntaxError:
            pass

    def test_ff_int_0s(self):
        '''Explicit 0 entries'''
        self.bitread_frm_equals('ff_int_0s.fasm', 'ff_int/design.bits')

    def test_badkey(self):
        '''Bad key should throw syntax error'''
        try:
            self.fasm2frames("CLBLM_L_X10Y102.SLICEM_X0.SRUSEDMUX 2")
            self.fail("Expected syntax error")
        except TextXSyntaxError:
            pass

    @unittest.skip
    def test_dupkey(self):
        '''Duplicate key should throw syntax error'''
        try:
            self.fasm2frames(
                """\
CLBLM_L_X10Y102.SLICEM_X0.SRUSEDMUX 0
CLBLM_L_X10Y102.SLICEM_X0.SRUSEDMUX 1
""")
            self.fail("Expected syntax error")
        except TextXSyntaxError:
            pass

    @unittest.skip
    def test_sparse(self):
        '''Verify sparse equivalent to normal encoding'''
        frm_fn = 'lut_int.fasm'

        fout_sparse_txt = self.fasm2frames(
            self.get_test_data(frm_fn), sparse=True)
        bits_sparse = frm2bits(fout_sparse_txt)

        fout_full_txt = self.fasm2frames(
            self.get_test_data(frm_fn), sparse=False)
        bits_full = frm2bits(fout_full_txt)

        # Now check for equivilence vs reference design
        self.assertEquals(len(bits_sparse), len(bits_full))
        self.assertEquals(bits_sparse, bits_full)

        # Verify the full ROI is way bigger description
        # It will still be decent size though since even sparse occupies all columns in that area
        self.assertGreaterEqual(len(fout_full_txt), len(fout_sparse_txt) * 4)

    def test_stepdown_1(self):
        self.bitread_frm_equals(
            'iob/liob_stepdown.fasm', 'iob/liob_stepdown.bits')

    def test_stepdown_2(self):
        self.bitread_frm_equals(
            'iob/riob_stepdown.fasm', 'iob/riob_stepdown.bits')

    # LIOB18_X1Y10 and LIOI_X2Y10 are grid neighbours sharing baseaddr
    # 0x00421000, offset 18 (the frames of virtex7's LIOB18_X81Y10 and
    # LIOI_X82Y10, renamed so the X81 column's own glue rule stays out).
    # OBUF_HP_BANK_GLUE of IOB_Y1 is OLOGIC_Y1's OQUSED (30_41), OMUX.D1
    # (32_16) and OSERDES.DATA_RATE_TQ.BUF (33_61).
    HP_DRIVE = 'LIOB18_X1Y10.IOB_Y1.LVCMOS15_LVCMOS18.DRIVE.I12_I16_I2_I4_I6_I8\n'
    HP_OQUSED = (0x0042101e, 19, 9)
    HP_OMUX_D1 = (0x00421020, 18, 16)
    HP_TQ_BUF = (0x00421021, 19, 29)
    HP_TQ_DDR = (0x00421021, 19, 25)

    def test_hp_glue_pass_through(self):
        '''An HP output with no OLOGIC cell gets OBUF_HP_BANK_GLUE'''
        bits = frm2bits(self.fasm2frames(self.HP_DRIVE))
        for bit in (self.HP_OQUSED, self.HP_OMUX_D1, self.HP_TQ_BUF):
            self.assertIn(bit, bits)

        # ZINV_T1 is the one pass-through feature that sets none of the glue
        # bits, so here they can only come from the glue.
        bits = frm2bits(
            self.fasm2frames(
                self.HP_DRIVE + 'LIOI_X2Y10.OLOGIC_Y1.ZINV_T1\n'))
        for bit in (self.HP_OQUSED, self.HP_OMUX_D1, self.HP_TQ_BUF):
            self.assertIn(bit, bits)

    def test_hp_glue_oddr_on_tristate(self):
        '''An ODDR on T: the glue's DATA_RATE_TQ.BUF must not be injected'''
        bits = frm2bits(
            self.fasm2frames(
                self.HP_DRIVE + 'LIOI_X2Y10.OLOGIC_Y1.OQUSED\n'
                'LIOI_X2Y10.OLOGIC_Y1.OMUX.D1\n'
                'LIOI_X2Y10.OLOGIC_Y1.OSERDES.DATA_RATE_TQ.DDR\n'
                'LIOI_X2Y10.OLOGIC_Y1.OSERDES.TSRTYPE.SYNC\n'
                'LIOI_X2Y10.OLOGIC_Y1.ZINIT_TQ\n'
                'LIOI_X2Y10.OLOGIC_Y1.ZINV_T2\n'))
        self.assertIn(self.HP_TQ_DDR, bits)
        self.assertNotIn(self.HP_TQ_BUF, bits)

    def test_hp_glue_oddr_on_data(self):
        '''An ODDR on the data: the glue's OMUX.D1 must not be injected'''
        bits = frm2bits(
            self.fasm2frames(
                self.HP_DRIVE + 'LIOI_X2Y10.OLOGIC_Y1.ODDR_TDDR.IN_USE\n'
                'LIOI_X2Y10.OLOGIC_Y1.OQUSED\n'
                'LIOI_X2Y10.OLOGIC_Y1.OSERDES.DATA_RATE_OQ.DDR\n'
                'LIOI_X2Y10.OLOGIC_Y1.OSERDES.DATA_RATE_TQ.BUF\n'
                'LIOI_X2Y10.OLOGIC_Y1.ZINV_CLK\n'))
        self.assertIn(self.HP_OQUSED, bits)
        self.assertNotIn(self.HP_OMUX_D1, bits)


class TestExitContract(unittest.TestCase):
    def run_main(self, fasm_text):
        with tempfile.TemporaryDirectory() as d:
            fn_in = os.path.join(d, 'in.fasm')
            fn_out = os.path.join(d, 'out.frm')
            with open(fn_in, 'w') as f:
                f.write(fasm_text + '\n')

            argv = [
                'fasm2frames.py', '--db-root',
                os.path.join(os.path.dirname(__file__), 'test_data', 'db'),
                '--part', 'xc7a200tffg1156-1', fn_in, fn_out
            ]
            with mock.patch.object(sys, 'argv', argv):
                rc = fasm2frames.main()

            if not os.path.exists(fn_out):
                return rc, None
            return rc, os.path.getsize(fn_out)

    def test_success_leaves_output(self):
        rc, size = self.run_main('CLBLM_L_X10Y102.SLICEM_X0.SRUSEDMUX')
        self.assertEqual(rc, 0)
        self.assertIsNotNone(size)
        self.assertGreater(size, 0)

    def test_lookup_error_leaves_no_output(self):
        rc, size = self.run_main('CLBLM_L_X10Y102.SLICEM_X0.NO_SUCH_FEATURE')
        self.assertEqual(rc, 1)
        self.assertIsNone(size)


class TestTileWindow(unittest.TestCase):
    SING_Y1 = 'LIOB33_SING_X0Y0.IOB_Y1.SOMETHING.IN'

    def assembler(self):
        db = Database(
            os.path.join(os.path.dirname(__file__), 'test_data', 'db'),
            'xc7a200tffg1156-1')
        return fasm_assembler.FasmAssembler(db)

    def test_absent_sing_site_is_refused(self):
        assembler = self.assembler()
        with tempfile.NamedTemporaryFile('w', suffix='.fasm') as fin:
            fin.write(self.SING_Y1 + '\n')
            fin.flush()
            with self.assertRaises(fasm_assembler.FasmTileWindowViolation):
                assembler.parse_fasm_filename(fin.name)
        self.assertEqual(assembler.frames, {})
        self.assertEqual(assembler.frames_in_use, set())

    def test_refusal_is_not_suppressed_by_allow_missing_features(self):
        assembler = self.assembler()
        with tempfile.NamedTemporaryFile('w', suffix='.fasm') as fin:
            fin.write(self.SING_Y1 + '\n')
            fin.flush()
            with mock.patch.dict(os.environ,
                                 {'XRAY_ALLOW_MISSING_FEATURES': '1'}):
                with self.assertRaises(
                        fasm_assembler.FasmTileWindowViolation):
                    assembler.parse_fasm_filename(fin.name)

    def test_absent_sing_site_exits_nonzero(self):
        rc, size = TestExitContract().run_main(self.SING_Y1)
        self.assertEqual(rc, 1)
        self.assertIsNone(size)

    def test_word_past_the_tile(self):
        bits = prjxray.grid_types.Bits(
            base_address=0x00400000,
            frames=42,
            offset=99,
            words=2,
            alias=None)
        for word, refused in ((98, True), (99, False), (100, False),
                              (101, True)):
            bit = prjxray.tile_segbits.Bit(
                word_column=0x00400000 + 38,
                word_bit=word * 32 + 3,
                isset=True)
            if refused:
                with self.assertRaises(
                        fasm_assembler.FasmTileWindowViolation):
                    fasm_assembler.check_bit_in_tile_window(
                        'T', bits, 'CLB_IO_CLK', bit, 'line')
            else:
                fasm_assembler.check_bit_in_tile_window(
                    'T', bits, 'CLB_IO_CLK', bit, 'line')

    def test_clear_bit_outside_the_tile_is_not_refused(self):
        bits = prjxray.grid_types.Bits(
            base_address=0x00400000,
            frames=42,
            offset=99,
            words=2,
            alias=None)
        for word_column, word in ((0x00400000 + 38, 98),
                                  (0x00400000 + 38, 101),
                                  (0x00400000 + 42, 99)):
            fasm_assembler.check_bit_in_tile_window(
                'T', bits, 'CLB_IO_CLK',
                prjxray.tile_segbits.Bit(
                    word_column=word_column,
                    word_bit=word * 32 + 3,
                    isset=False), 'line')

    def test_frame_past_the_tile(self):
        bits = prjxray.grid_types.Bits(
            base_address=0x00400000,
            frames=42,
            offset=0,
            words=2,
            alias=None)
        for frame, refused in ((-1, True), (0, False), (41, False),
                               (42, True)):
            bit = prjxray.tile_segbits.Bit(
                word_column=0x00400000 + frame, word_bit=3, isset=True)
            if refused:
                with self.assertRaises(
                        fasm_assembler.FasmTileWindowViolation):
                    fasm_assembler.check_bit_in_tile_window(
                        'T', bits, 'CLB_IO_CLK', bit, 'line')
            else:
                fasm_assembler.check_bit_in_tile_window(
                    'T', bits, 'CLB_IO_CLK', bit, 'line')

    def test_no_bit_is_written_before_a_later_bit_is_refused(self):
        assembler = self.assembler()
        tile = 'CLBLM_L_X10Y102'
        gridinfo = assembler.grid.gridinfo_at_tilename(tile)
        block_type = prjxray.grid_types.BlockType.CLB_IO_CLK
        bits = gridinfo.bits[block_type]
        inside = prjxray.tile_segbits.Bit(
            word_column=bits.base_address,
            word_bit=bits.offset * 32,
            isset=True)
        outside = prjxray.tile_segbits.Bit(
            word_column=bits.base_address,
            word_bit=(bits.offset + bits.words) * 32,
            isset=True)
        segbits = mock.Mock()
        segbits.feature_to_bits.return_value = [(block_type, inside),
                                                (block_type, outside)]
        with mock.patch.object(assembler.grid, 'get_tile_segbits_at_tilename',
                               return_value=segbits):
            with self.assertRaises(fasm_assembler.FasmTileWindowViolation):
                assembler.enable_feature(tile, 'SLICEM_X0.PAIR', 0, 'line')
        self.assertEqual(assembler.frames, {})
        self.assertEqual(assembler.frames_in_use, set())


class TestKintex7HpBank(unittest.TestCase):
    DB = os.path.join(os.path.dirname(__file__), 'test_data', 'db_kintex7')
    PART = 'xc7k325tffg900-2'
    IO_COLUMN = 0x00442F80

    HP_OUTPUT = '\n'.join(
        (
            'RIOB18_X95Y1.IOB_Y0.LVCMOS15_LVCMOS18.DRIVE.'
            'I12_I16_I2_I4_I6_I8',
            'RIOB18_X95Y1.IOB_Y0.LVCMOS12_LVCMOS15_LVCMOS18.SLEW.SLOW',
            'RIOB18_X95Y1.IOB_Y0.PULLTYPE.NONE',
            'INT_R_X95Y1.IOB_COL_BANK_ACTIVE',
            'INT_R_X95Y1.IOB_COL_OBUF_CASCADE_Y1',
        ))

    IOB_COL_BITS = {
        (IO_COLUMN + 38, 2, 12),
        (IO_COLUMN + 38, 3, 0),
        (IO_COLUMN + 38, 3, 2),
        (IO_COLUMN + 30, 3, 9),
        (IO_COLUMN + 32, 2, 16),
        (IO_COLUMN + 33, 3, 29),
    }

    def run_fasm(self, text, sparse=True):
        with tempfile.NamedTemporaryFile('w', suffix='.fasm') as fin:
            fin.write(text + '\n')
            fin.flush()
            fout = StringIO()
            fasm2frames.run(self.DB, self.PART, fin.name, fout, sparse=sparse)
            return fout.getvalue()

    def test_sing_input_whose_clear_bit_is_outside_the_tile(self):
        frm = self.run_fasm('RIOB18_SING_X95Y0.IOB_Y0.LVCMOS12_LVCMOS15.IN')
        self.assertEqual(
            frm2bits(frm), {
                (self.IO_COLUMN + 38, 1, 30),
                (self.IO_COLUMN + 39, 1, 31),
            })

    def test_sing_input_glue_with_no_word_in_the_tile_is_not_injected(self):
        frm = self.run_fasm(
            'RIOB18_SING_X95Y49.IOB_Y1.LVCMOS12_LVCMOS15_LVCMOS18.IN')
        self.assertEqual(
            frm2bits(frm), {
                (self.IO_COLUMN + 38, 99, 0),
                (self.IO_COLUMN + 39, 99, 1),
            })

    def test_sing_input_glue_with_no_word_in_the_tile_is_reported(self):
        err = StringIO()
        with contextlib.redirect_stderr(err):
            self.run_fasm(
                'RIOB18_SING_X95Y49.IOB_Y1.LVCMOS12_LVCMOS15_LVCMOS18.IN')
        self.assertIn('WARNING: injected glue not written', err.getvalue())
        self.assertIn(
            'RIOB18_SING_X95Y49.IOB_Y1.IBUF_HP_BANK_GLUE', err.getvalue())
        self.assertIn('lands in no tile', err.getvalue())

    def with_glue_row(self, row):
        d = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, d)
        db = os.path.join(d, 'db')
        shutil.copytree(self.DB, db)
        segbits = os.path.join(db, 'segbits_riob18.db')
        with open(segbits) as f:
            lines = [
                l for l in f.read().splitlines()
                if not l.startswith('RIOB18.IOB_Y1.IBUF_HP_BANK_GLUE ')
            ]
        with open(segbits, 'w') as f:
            f.write('\n'.join(lines + [row]) + '\n')
        return db

    def test_injected_glue_landing_in_another_tile_is_refused(self):
        db = self.with_glue_row('RIOB18.IOB_Y1.IBUF_HP_BANK_GLUE 29_141')
        with tempfile.NamedTemporaryFile('w', suffix='.fasm') as fin:
            fin.write('RIOB18_X95Y47.IOB_Y1.LVCMOS12_LVCMOS15_LVCMOS18.IN\n')
            fin.flush()
            with self.assertRaisesRegex(fasm_assembler.FasmCrossTileWrite,
                                        'RIOB18_SING_X95Y49'):
                fasm2frames.run(db, self.PART, fin.name, StringIO(),
                                sparse=True)

    def test_injected_glue_inside_its_tile_is_written(self):
        db = self.with_glue_row('RIOB18.IOB_Y1.IBUF_HP_BANK_GLUE 29_109')
        with tempfile.NamedTemporaryFile('w', suffix='.fasm') as fin:
            fin.write('RIOB18_X95Y47.IOB_Y1.LVCMOS12_LVCMOS15_LVCMOS18.IN\n')
            fin.flush()
            fout = StringIO()
            fasm2frames.run(db, self.PART, fin.name, fout, sparse=True)
        self.assertIn((self.IO_COLUMN + 29, 98, 13), frm2bits(fout.getvalue()))

    def test_hp_output_column_glue_lands_in_the_iob_tile(self):
        bits = frm2bits(self.run_fasm(self.HP_OUTPUT))
        self.assertTrue(self.IOB_COL_BITS <= bits, self.IOB_COL_BITS - bits)

    def test_hp_output_exits_zero(self):
        with tempfile.TemporaryDirectory() as d:
            fn_in = os.path.join(d, 'in.fasm')
            fn_out = os.path.join(d, 'out.frm')
            with open(fn_in, 'w') as f:
                f.write(self.HP_OUTPUT + '\n')
            argv = [
                'fasm2frames.py', '--db-root', self.DB, '--part', self.PART,
                '--sparse', fn_in, fn_out
            ]
            with mock.patch.object(sys, 'argv', argv):
                self.assertEqual(fasm2frames.main(), 0)
            self.assertTrue(os.path.getsize(fn_out) > 0)

    def test_column_glue_alone_writes_the_host_tiles_frames(self):
        frm = self.run_fasm('INT_R_X95Y1.IOB_COL_BANK_ACTIVE')
        frames = {int(l.split(' ')[0], 0) for l in frm.splitlines() if l}
        self.assertEqual(
            frames, set(range(self.IO_COLUMN, self.IO_COLUMN + 42)))
        self.assertEqual(
            frm2bits(frm), {
                (self.IO_COLUMN + 38, 2, 12),
                (self.IO_COLUMN + 38, 3, 0),
                (self.IO_COLUMN + 38, 3, 2),
            })

    def test_column_glue_is_refused_without_its_declaration(self):
        with mock.patch.dict(cross_tile_features.ALLOWED, clear=True):
            with self.assertRaises(fasm_assembler.FasmTileWindowViolation):
                self.run_fasm(self.HP_OUTPUT)

    def test_column_glue_in_a_clb_column_is_refused(self):
        for feature in ('IOB_COL_BANK_ACTIVE', 'IOB_COL_OBUF_CASCADE_Y1'):
            with self.subTest(feature=feature):
                with self.assertRaises(
                        fasm_assembler.FasmTileWindowViolation):
                    self.run_fasm('INT_R_X93Y1.' + feature)

    def test_sing_absent_site_set_bit_is_still_refused(self):
        with self.assertRaises(fasm_assembler.FasmTileWindowViolation):
            self.run_fasm(
                'RIOB18_SING_X95Y0.IOB_Y1.LVCMOS12_LVCMOS15_LVCMOS18.IN')


class TestFeatureConflict(unittest.TestCase):
    INIT = 'CLBLM_L_X10Y102.SLICEM_X0.ALUT.INIT'

    def assemble(self, fasm_text):
        db = Database(
            os.path.join(os.path.dirname(__file__), 'test_data', 'db'),
            'xc7a200tffg1156-1')
        assembler = fasm_assembler.FasmAssembler(db)
        with tempfile.NamedTemporaryFile('w', suffix='.fasm') as fin:
            fin.write(fasm_text)
            fin.flush()
            assembler.parse_fasm_filename(fin.name)
        return assembler

    def test_two_inits_are_not_ored(self):
        with self.assertRaises(fasm_assembler.FasmFeatureConflict):
            self.assemble(
                self.INIT + "[3:0] = 4'b0101\n" + self.INIT +
                "[3:0] = 4'b1010\n")

    def test_zero_then_one_is_a_conflict(self):
        with self.assertRaises(fasm_assembler.FasmFeatureConflict):
            self.assemble(
                'CLBLM_L_X10Y102.SLICEM_X0.SRUSEDMUX = 0\n'
                'CLBLM_L_X10Y102.SLICEM_X0.SRUSEDMUX\n')

    def test_bit_and_range_overlap_is_a_conflict(self):
        with self.assertRaises(fasm_assembler.FasmFeatureConflict):
            self.assemble(
                self.INIT + "[3:0] = 4'b0000\n" + self.INIT + '[2]\n')

    def test_address_zero_and_bare_feature_are_one_address(self):
        with self.assertRaises(fasm_assembler.FasmFeatureConflict):
            self.assemble(self.INIT + '[0] = 0\n' + self.INIT + '\n')

    def test_repeat_with_same_value_is_accepted(self):
        self.assemble(
            self.INIT + "[3:0] = 4'b0101\n" + self.INIT +
            "[3:0] = 4'b0101\n")

    def test_disjoint_ranges_are_accepted(self):
        assembler = self.assemble(
            self.INIT + "[1:0] = 2'b01\n" + self.INIT + "[3:2] = 2'b10\n")
        self.assertEqual(
            assembler.feature_assignments['CLBLM_L_X10Y102.SLICEM_X0.ALUT.INIT']
            [:2], [0b1111, 0b1001])

    def test_conflict_exits_nonzero(self):
        rc, size = TestExitContract().run_main(
            self.INIT + "[3:0] = 4'b0101\n" + self.INIT + "[3:0] = 4'b1010")
        self.assertEqual(rc, 1)
        self.assertIsNone(size)

    def test_message_quotes_both_lines_at_full_width(self):
        with self.assertRaises(fasm_assembler.FasmFeatureConflict) as cm:
            self.assemble(
                self.INIT + "[7:0] = 8'b00000101\n" + self.INIT +
                "[7:0] = 8'h0A\n")
        message = str(cm.exception)
        self.assertIn('"' + self.INIT + "[7:0] = 8'b00000101\"", message)
        self.assertIn('"' + self.INIT + "[7:0] = 8'h0A\"", message)

    def test_message_names_each_earlier_line_that_conflicts(self):
        with self.assertRaises(fasm_assembler.FasmFeatureConflict) as cm:
            self.assemble(
                self.INIT + "[1:0] = 2'b01\n" + self.INIT +
                "[3:2] = 2'b00\n" + self.INIT + "[5:4] = 2'b10\n" +
                self.INIT + "[5:0] = 6'b000001\n")
        message = str(cm.exception)
        self.assertNotIn("[1:0] = 2'b01", message)
        self.assertNotIn("[3:2] = 2'b00", message)
        self.assertIn('"' + self.INIT + "[5:4] = 2'b10\"", message)
        self.assertIn('address(es) 5', message)


class TestRoiPrecedence(unittest.TestCase):
    def test_roi_violation_is_not_hidden_by_allowed_missing_features(self):
        db = Database(
            os.path.join(os.path.dirname(__file__), 'test_data', 'db'),
            'xc7a200tffg1156-1')
        assembler = fasm_assembler.FasmAssembler(db)
        assembler.mark_roi_frames(
            Roi(db=db, x1=0, x2=1, y1=130, y2=155), strict=True)

        with tempfile.NamedTemporaryFile('w', suffix='.fasm') as fin:
            fin.write(
                'LIOB33_X0Y1.IOB_Y0.NO_SUCH_FEATURE\n'
                'INT_L_X10Y102.BYP_ALT0.EE2END0\n')
            fin.flush()
            with mock.patch.dict(os.environ,
                                 {'XRAY_ALLOW_MISSING_FEATURES': '1'}):
                with self.assertRaises(fasm_assembler.FasmRoiViolation):
                    assembler.parse_fasm_filename(fin.name)


class TestRoiStrictSharedFrames(unittest.TestCase):
    def assembler(self):
        db = Database(
            os.path.join(os.path.dirname(__file__), 'test_data', 'db'),
            'xc7a200tffg1156-1')
        return db, fasm_assembler.FasmAssembler(db)

    def test_roi_sharing_frames_with_an_outside_tile_is_refused(self):
        db, assembler = self.assembler()
        roi = Roi(db=db, x1=30, x2=30, y1=49, y2=49)
        with self.assertRaises(fasm_assembler.FasmRoiViolation) as cm:
            assembler.mark_roi_frames(roi, strict=True)
        self.assertIn('INT_L_X10Y102', str(cm.exception))
        self.assertIn('HCLK_L_X31Y130', str(cm.exception))

    def test_roi_owning_its_frames_is_accepted(self):
        db, assembler = self.assembler()
        roi = Roi(db=db, x1=30, x2=31, y1=26, y2=49)
        assembler.mark_roi_frames(roi, strict=True)
        frames = assembler.get_frames(sparse=True)
        self.assertEqual(len(frames), 36)

    def test_permissive_roi_is_unchanged(self):
        db, assembler = self.assembler()
        roi = Roi(db=db, x1=30, x2=30, y1=49, y2=49)
        assembler.mark_roi_frames(roi)
        self.assertEqual(len(assembler.get_frames(sparse=True)), 36)


if __name__ == '__main__':
    unittest.main()
