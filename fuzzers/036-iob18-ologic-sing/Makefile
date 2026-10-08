# Copyright (C) 2017-2022  The Project X-Ray Authors.
#
# Use of this source code is governed by a ISC-style
# license that can be found in the LICENSE file or at
# https://opensource.org/licenses/ISC
#
# SPDX-License-Identifier: ISC

# SING-row sibling of 036-iob18-ologic.  Generates OLOGIC stimulus
# only on IOB18 sites within *_SING tiles, then bit-diffs into
# segbits suitable for merging as lioi_sing / rioi_sing.  Without
# this, fasm2frames emits "invalid word address" warnings when
# nextpnr asks for OLOGIC features on SING-row LED IOBs (VC707
# counter25 build hits this for led[?] @ LIOI_SING_X82Y51).

# N=50: the sibling 036-iob18-ologic uses N=40 to resolve the OLOGIC_Y0
# enum features (OMUX/OQUSED/OSERDES.DATA_RATE_TQ); SING needs at least as
# many specimens or dbfixup drops them as unresolved/duplicate.
N := 50
include ../fuzzer.mk

database: build/segbits_rioi_sing.db

build/segbits_rioi_sing.rdb: $(SPECIMENS_OK)
	${XRAY_SEGMATCH} -c 7 -o build/segbits_rioi_sing.rdb $$(find -name segdata_*)

build/segbits_rioi_sing.db: build/segbits_rioi_sing.rdb
	${XRAY_DBFIXUP} --db-root build --zero-db bits.dbf --seg-fn-in $^ --seg-fn-out $@
	${XRAY_MASKMERGE} build/mask_rioi_sing.db $$(find -name segdata_*)

pushdb:
	${XRAY_MERGEDB} rioi_sing build/segbits_rioi_sing.db
	${XRAY_MERGEDB} lioi_sing build/segbits_rioi_sing.db
	${XRAY_MERGEDB} mask_rioi_sing build/mask_rioi_sing.db
	${XRAY_MERGEDB} mask_lioi_sing build/mask_rioi_sing.db

.PHONY: database pushdb
