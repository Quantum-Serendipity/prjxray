# Copyright (C) 2017-2022  The Project X-Ray Authors.
#
# Use of this source code is governed by a ISC-style
# license that can be found in the LICENSE file or at
# https://opensource.org/licenses/ISC
#
# SPDX-License-Identifier: ISC
#
# SING-row IOB18 basic-config fuzzer (DRIVE/SLEW/PULLTYPE/STEPDOWN) for the
# single IOB18 site in each *_SING tile.  Replaces the hand-copied (and
# mis-addressed) LIOB18_SING entries with real measurements.
#
# Unlike regular IOB18 (where L+R share the 'iob18' segbits db and segmaker
# emits a normalised IOB18. prefix), SING tiles have SEPARATE liob18_sing /
# riob18_sing dbs, so segmaker emits LIOB18_SING. / RIOB18_SING. and the two
# sides must be solved independently.  process_rdb.py still re-emits the
# IOB18. prefix, which the per-side mergedb cases rewrite to LIOB18_SING. /
# RIOB18_SING.
N := 30
include ../fuzzer.mk

database: build/segbits_liob18_sing.db build/segbits_riob18_sing.db

# Per-side: segmatch -> drop unresolved (<.. candidates>, which process_rdb
# cannot parse) -> process_rdb enum-merge -> dbfixup.
build/segbits_liob18_sing.db: $(SPECIMENS_OK)
	${XRAY_SEGMATCH} -o build/raw_l.rdb $$(find -name 'segdata_liob18_sing.txt')
	python3 process_rdb.py build/raw_l.rdb > build/proc_l.rdb
	${XRAY_DBFIXUP} --db-root build --zero-db bits.dbf --seg-fn-in build/proc_l.rdb --seg-fn-out $@

build/segbits_riob18_sing.db: $(SPECIMENS_OK)
	${XRAY_SEGMATCH} -o build/raw_r.rdb $$(find -name 'segdata_riob18_sing.txt')
	python3 process_rdb.py build/raw_r.rdb > build/proc_r.rdb
	${XRAY_DBFIXUP} --db-root build --zero-db bits.dbf --seg-fn-in build/proc_r.rdb --seg-fn-out $@

pushdb:
	${XRAY_MERGEDB} liob18_sing build/segbits_liob18_sing.db
	${XRAY_MERGEDB} riob18_sing build/segbits_riob18_sing.db

.PHONY: database pushdb
