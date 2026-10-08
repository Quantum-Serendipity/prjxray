# Copyright (C) 2017-2020  The Project X-Ray Authors.
#
# Use of this source code is governed by a ISC-style
# license that can be found in the LICENSE file or at
# https://opensource.org/licenses/ISC
#
# SPDX-License-Identifier: ISC
#
# IOB18 (HP-bank) ISERDES fuzzer for Virtex-7.  The kintex7 035b-iob-iserdes
# fuzzer targets IOB33/HR sites that do not exist on this part, so its RIOI
# ISERDES results were transplanted.  This variant places ISERDESE2 on real
# IOB18 sites on BOTH the L and R columns and measures the ISERDES config bits
# directly, merging to lioi and rioi (segmaker emits the IOI. prefix, which
# mergedb rewrites per side).
N := 40
include ../fuzzer.mk

database: build/segbits_iob18_iserdes.db

build/segbits_iob18_iserdes.rdb: $(SPECIMENS_OK)
	${XRAY_SEGMATCH} -c 20 -m 1 -M 1 -o build/segbits_iob18_iserdes.rdb2 $$(find -name segdata_*.txt)
	# Keep only ISERDES-related solutions (the rest belong to 035-iob18-ilogic).
	grep -e ".ISERDES." build/segbits_iob18_iserdes.rdb2 > $@
	grep -e ".IDDR.IN_USE." build/segbits_iob18_iserdes.rdb2 >> $@ || true

build/segbits_iob18_iserdes.db: build/segbits_iob18_iserdes.rdb
	${XRAY_DBFIXUP} --db-root build --zero-db bits.dbf --groups tag_groups.txt --seg-fn-in $^ --seg-fn-out $@
	${XRAY_MASKMERGE} build/mask_iob18_iserdes.db $$(find -name segdata_*.txt)

pushdb:
	${XRAY_MERGEDB} lioi build/segbits_iob18_iserdes.db
	${XRAY_MERGEDB} lioi_tbytesrc build/segbits_iob18_iserdes.db
	${XRAY_MERGEDB} lioi_tbyteterm build/segbits_iob18_iserdes.db
	${XRAY_MERGEDB} rioi build/segbits_iob18_iserdes.db
	${XRAY_MERGEDB} rioi_tbytesrc build/segbits_iob18_iserdes.db
	${XRAY_MERGEDB} rioi_tbyteterm build/segbits_iob18_iserdes.db
	${XRAY_MERGEDB} mask_lioi build/mask_iob18_iserdes.db
	${XRAY_MERGEDB} mask_lioi_tbytesrc build/mask_iob18_iserdes.db
	${XRAY_MERGEDB} mask_lioi_tbyteterm build/mask_iob18_iserdes.db
	${XRAY_MERGEDB} mask_rioi build/mask_iob18_iserdes.db
	${XRAY_MERGEDB} mask_rioi_tbytesrc build/mask_iob18_iserdes.db
	${XRAY_MERGEDB} mask_rioi_tbyteterm build/mask_iob18_iserdes.db

.PHONY: database pushdb
