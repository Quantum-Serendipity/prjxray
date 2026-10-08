# Copyright (C) 2017-2020  The Project X-Ray Authors.
#
# Use of this source code is governed by a ISC-style
# license that can be found in the LICENSE file or at
# https://opensource.org/licenses/ISC
#
# SPDX-License-Identifier: ISC
N ?= 24
# On Virtex-7 a few IOI18 tiles (e.g. LIOI_X82Y10, RIOI_X311Y10) place the
# toggled bit one frame above the Kintex-7 offset, so a fixed DFRAME mis-aligns
# their base address. Use AUTO_FRAME (round the base down to the 0x80 boundary)
# for virtex7 instead; it is equivalent to DFRAME:20 for the aligned tiles.
ifeq ($(XRAY_DATABASE),virtex7)
GENERATE_ARGS?="--oneval 1 --design params.csv --auto-frame --dword 3"
else
GENERATE_ARGS?="--oneval 1 --design params.csv --dframe 20 --dword 3"
endif
include ../fuzzaddr/common.mk
