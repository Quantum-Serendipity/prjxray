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


build/segbits_cfg.rdb: $(SPECIMENS_OK)
	python3 -m prjxray.presence --only 'ENABLE_TR$$' $$(find -name segdata_cfg_center_mid.txt)
	${XRAY_SEGMATCH} -c 15 -o build/segbits_cfg.rdb $$(find -name segdata_cfg_center_mid.txt)

pushdb:
	${XRAY_MERGEDB} cfg_center_mid build/segbits_cfg.db

.PHONY: database pushdb

