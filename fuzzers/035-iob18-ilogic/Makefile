# Copyright (C) 2017-2020  The Project X-Ray Authors.
#
# Use of this source code is governed by a ISC-style
# license that can be found in the LICENSE file or at
# https://opensource.org/licenses/ISC
#
# SPDX-License-Identifier: ISC
#
# IOB18 (HP-bank) ILOGIC fuzzer for Virtex-7.  The kintex7 035-iob-ilogic
# fuzzer targets IOB33/HR sites that do not exist on this part, so its LIOI/
# RIOI results were transplanted.  This variant places ISERDESE2/IDDR on real
# IOB18 sites on BOTH the L and R columns and measures the ILOGIC config bits
# (incl. ZINV_D) directly.
N := 30
include ../fuzzer.mk

database: build/segbits_iob18_ilogic.db

build/segbits_iob18_ilogic.rdb: $(SPECIMENS_OK)
	${XRAY_SEGMATCH} -c 6 -o build/segbits_iob18_ilogic.rdb2 $$(find -name segdata_*.txt)
	# ISERDES features belong to 035b-iob18-iserdes; DYN_* are solved there too.
	grep -v ".ISERDES." build/segbits_iob18_ilogic.rdb2 | grep -v "DYN_" >$@

build/segbits_iob18_ilogic.db: build/segbits_iob18_ilogic.rdb
	${XRAY_DBFIXUP} --db-root build --zero-db bits.dbf --groups tag_groups.txt --seg-fn-in $^ --seg-fn-out $@
	${XRAY_MASKMERGE} build/mask_iob18_ilogic.db $$(find -name segdata_*.txt)

pushdb:
	${XRAY_MERGEDB} lioi build/segbits_iob18_ilogic.db
	${XRAY_MERGEDB} lioi_tbytesrc build/segbits_iob18_ilogic.db
	${XRAY_MERGEDB} lioi_tbyteterm build/segbits_iob18_ilogic.db
	${XRAY_MERGEDB} rioi build/segbits_iob18_ilogic.db
	${XRAY_MERGEDB} rioi_tbytesrc build/segbits_iob18_ilogic.db
	${XRAY_MERGEDB} rioi_tbyteterm build/segbits_iob18_ilogic.db
	${XRAY_MERGEDB} mask_lioi build/mask_iob18_ilogic.db
	${XRAY_MERGEDB} mask_lioi_tbytesrc build/mask_iob18_ilogic.db
	${XRAY_MERGEDB} mask_lioi_tbyteterm build/mask_iob18_ilogic.db
	${XRAY_MERGEDB} mask_rioi build/mask_iob18_ilogic.db
	${XRAY_MERGEDB} mask_rioi_tbytesrc build/mask_iob18_ilogic.db
	${XRAY_MERGEDB} mask_rioi_tbyteterm build/mask_iob18_ilogic.db

.PHONY: database pushdb
