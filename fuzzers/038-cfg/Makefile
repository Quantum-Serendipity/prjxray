# Copyright (C) 2017-2020  The Project X-Ray Authors.
#
# Use of this source code is governed by a ISC-style
# license that can be found in the LICENSE file or at
# https://opensource.org/licenses/ISC
#
# SPDX-License-Identifier: ISC
# N must be at least 2**(number of presence-varied sites) = 4, so that top.py's
# factorial presence plan covers every combination.  The `presence` gate below
# measures the result rather than trusting this comment.
N = 24
include ../fuzzer.mk

database: build/segbits_cfg.rdb
	${XRAY_DBFIXUP} --db-root build --zero-db bits.dbf \
                --seg-fn-in build/segbits_cfg.rdb \
                --seg-fn-out build/segbits_cfg.db \
                --unresolved-out build/segbits_cfg.unresolved.rdb
# Some of the bits in Zynq seem to be always set
# see https://github.com/SymbiFlow/prjxray/issues/746
#
# Those three bits are {26_2206, 26_2207, 27_2205}, and they are -- set
# identically -- prjcombine's ICAP[1].ENABLE_TR.  They were "always set" only
# because this fuzzer instantiated ICAP_X0Y1 in every specimen; it no longer
# does, and CFG_CENTER_MID.ICAP_X0Y1.ENABLE_TR is now solved by fuzzing on any
# family this runs on.  add_constant_bits.py is left in place because removing
# it changes a family that is not exercised here.
ifeq (${XRAY_DATABASE}, zynq7)
	python3 add_constant_bits.py build/segbits_cfg.db
endif


build/segbits_cfg.rdb: $(SPECIMENS_OK)
	python3 -m prjxray.presence --only 'ENABLE_TR$$' $$(find -name segdata_cfg_center_mid.txt)
	${XRAY_SEGMATCH} -c 15 -o build/segbits_cfg.rdb $$(find -name segdata_cfg_center_mid.txt)

pushdb:
	${XRAY_MERGEDB} cfg_center_mid build/segbits_cfg.db

.PHONY: database pushdb

