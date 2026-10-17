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

ALLOWED = {
    ('INT_L', 'IOB_COL_BANK_ACTIVE'): ('LIOB18', 'LIOB18_SING'),
    ('INT_L', 'IOB_COL_OBUF_CASCADE_Y1'): ('LIOB18', 'LIOB18_SING'),
    ('INT_R', 'IOB_COL_BANK_ACTIVE'): ('RIOB18', 'RIOB18_SING'),
    ('INT_R', 'IOB_COL_OBUF_CASCADE_Y1'): ('RIOB18', 'RIOB18_SING'),
}

REFUSED = {
    ('INT_L', 'IOB_COL_BANK_ACTIVE'): ('LIOB33', 'LIOB33_SING'),
    ('INT_L', 'IOB_COL_OBUF_CASCADE_Y1'): (
        'CLBLL_L', 'CLBLM_L', 'LIOB33', 'LIOB33_SING'),
    ('INT_R', 'IOB_COL_OBUF_CASCADE_Y1'): (
        'CLBLL_R', 'CLBLM_R', 'GTX_COMMON', 'PCIE_BOT'),
    ('GTX_COMMON', 'GTXE2_COMMON.ENABLE_DRP'): ('HCLK_R', ),
    ('LIOB18_SING', 'IOB_Y0.LVCMOS12.DRIVE.I2_I4_I6_I8'): ('LIOB18', ),
    ('LIOB18_SING', 'IOB_Y0.LVCMOS15_LVCMOS18.DRIVE.I12_I16_I2_I4_I6_I8'): (
        'LIOB18', ),
    ('LIOB18_SING', 'IOB_Y0.LVDS.DRIVE.I_FIXED'): ('LIOB18', ),
    ('LIOB18_SING', 'IOB_Y0.OBUF_HP_BANK_GLUE'): ('LIOB18', ),
    ('LIOB18_SING', 'IOB_Y0.SSTL12.DRIVE.I_FIXED'): ('LIOB18', ),
    ('LIOB18_SING', 'IOB_Y0.SSTL135.DRIVE.I_FIXED'): ('LIOB18', ),
    ('LIOB18_SING', 'IOB_Y0.SSTL15.DRIVE.I_FIXED'): ('LIOB18', ),
    ('RIOB18_SING', 'IOB_Y0.LVCMOS12.DRIVE.I2_I4_I6_I8'): ('RIOB18', ),
    ('RIOB18_SING', 'IOB_Y0.LVCMOS15_LVCMOS18.DRIVE.I12_I16_I2_I4_I6_I8'): (
        'RIOB18', ),
    ('RIOB18_SING', 'IOB_Y0.LVDS.DRIVE.I_FIXED'): ('RIOB18', ),
    ('RIOB18_SING', 'IOB_Y0.SSTL12.DRIVE.I_FIXED'): ('RIOB18', ),
    ('RIOB18_SING', 'IOB_Y0.SSTL135.DRIVE.I_FIXED'): ('RIOB18', ),
    ('RIOB18_SING', 'IOB_Y0.SSTL15.DRIVE.I_FIXED'): ('RIOB18', ),
}
