# Copyright (C) 2017-2022  The Project X-Ray Authors.
#
# Use of this source code is governed by a ISC-style
# license that can be found in the LICENSE file or at
# https://opensource.org/licenses/ISC
#
# SPDX-License-Identifier: ISC
N := 40
SPECIMENS_DEPS := build/iobanks.txt
include ../fuzzer.mk

database: build/segbits_riob18.db build/segbits_hclk_ioi.db

build/iobanks.txt: write_io_banks.tcl
	mkdir -p build
	cd build/ && ${XRAY_VIVADO} -mode batch -source ../write_io_banks.tcl

build/segbits_riob18.rdb: $(SPECIMENS_OK)
	# Glob both L- and R-side segdata: top.py iterates IOB18 sites on both
	# sides of the chip, but kintex7's R-only Makefile only used the R-side
	# files. Virtex-7 xc7vx485tffg1761-2 has IOB18 banks on both sides, so
	# include segdata_liob18.txt too. IOB18 architecture is symmetric so
	# segmatch will derive consistent bit positions across L and R samples.
	${XRAY_SEGMATCH} -c 23 -o build/segbits_riob18.rdb $$(find -name 'segdata_[rl]iob18.txt')

build/segbits_riob18.db: build/segbits_riob18.rdb process_rdb.py bits.dbf
#   delete the Y1 LVDS tags because they are empty anyway and are missing the DRIVE tag which upsets process_rdb.py
	sed '/IOB18.IOB_Y1.LVDS/d' -i build/segbits_riob18.rdb
	python3 process_rdb.py build/segbits_riob18.rdb > build/segbits_riob18_processed.rdb
	${XRAY_DBFIXUP} --db-root build --zero-db bits.dbf --groups tag_groups.txt --seg-fn-in build/segbits_riob18_processed.rdb --seg-fn-out $@
	# TODO: This is a hack. See https://github.com/f4pga/prjxray/issues/2073
	# Use the prefix-less form 'IOB18.' so mergedb.sh's per-side sed (which
	# rewrites '^IOB18\.' to either 'RIOB18.' or 'LIOB18.') applies to this
	# manual entry as well. The previous hard-coded 'RIOB18.' would land
	# verbatim in segbits_liob18.db and bit2fasm would KeyError on the
	# missing 'LIOB18.IOB_Y0.SSTL12_SSTL135_SSTL15.IN'.
	echo 'IOB18.IOB_Y0.SSTL12_SSTL135_SSTL15.IN !38_126 39_127' >> $@
	sort -o $@ $@
	${XRAY_MASKMERGE} build/mask_riob18.db $$(find -name 'segdata_[rl]iob18.txt')

build/segbits_hclk_ioi.rdb: $(SPECIMENS_OK)
	${XRAY_SEGMATCH} -c 10 -o build/segbits_hclk_ioi.rdb $$(find -name segdata_hclk_ioi.txt)

build/segbits_hclk_ioi.db: build/segbits_hclk_ioi.rdb
	${XRAY_DBFIXUP} --db-root build --zero-db hclk_bits.dbf --seg-fn-in build/segbits_hclk_ioi.rdb --seg-fn-out $@

pushdb:
	${XRAY_MERGEDB} riob18 build/segbits_riob18.db
	${XRAY_MERGEDB} mask_riob18 build/mask_riob18.db
	# Mirror to L-side for parts with HP-bank IOB18 on both sides
	# (virtex7 xc7vx485tffg1761-2). The same .db content is correct for
	# LIOB18 because IOB18 tile architecture is symmetric.
	${XRAY_MERGEDB} liob18 build/segbits_riob18.db
	${XRAY_MERGEDB} mask_liob18 build/mask_riob18.db

	${XRAY_MERGEDB} hclk_ioi build/segbits_hclk_ioi.db

.PHONY: database pushdb

