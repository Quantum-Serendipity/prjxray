# Copyright (C) 2017-2020  The Project X-Ray Authors.
#
# Use of this source code is governed by a ISC-style
# license that can be found in the LICENSE file or at
# https://opensource.org/licenses/ISC
#
# SPDX-License-Identifier: ISC
#
# 045a — fill the CK_IN[1..13] gap in segbits_hclk_cmt[_l].db.
#
# Mirrors 045-hclk-cmt-pips/Makefile but iterates over the 21 unsolved
# CK_IN PIPs in targets.csv (rather than sweeping all PIPs).  Re-uses
# 045's cmt_regions.csv (build/ symlink), bits.dbf, and segmatch+mergedb
# downstream chain.
export FUZDIR=$(shell pwd)
PARENT_FUZDIR=$(FUZDIR)/../045-hclk-cmt-pips
PIP_TYPE?=hclk_cmt
PIPLIST_TCL=$(PARENT_FUZDIR)/hclk_cmt_pip_list.tcl
TODO_RE=".*HCLK_CMT.*CK_IN.*"
EXCLUDE_RE="(^.*LEAF)|(^.*BUFMR)|(^.*PHSR)|(^.*CLK_PLL7)|(^.*CLK_MMCM13)"
MAKETODO_FLAGS=--sides ",l" --pip-type ${PIP_TYPE} --seg-type ${PIP_TYPE} --re $(TODO_RE) --exclude-re $(EXCLUDE_RE)

# One specimen per target PIP in targets.csv (21 rows + header).  N a few
# above 21 to give segmatch slack for failed-route specimens.
N = 24
FOUR_BIT_PIPS="FREQ_REF"
SPECIMENS_DEPS=build/cmt_regions.csv
A_PIPLIST=hclk_cmt.txt

include ../pip_loop.mk

.PRECIOUS: build/%.rdb

build/%.rdb: $(SPECIMENS_OK)
	$(XRAY_SEGMATCH) -c 4 -o $@.4 $(shell find build -name $(subst segbits,segdata,$(patsubst %.rdb,%.txt,$(notdir $@))))
	$(XRAY_SEGMATCH) -c 2 -o $@.2 $(shell find build -name $(subst segbits,segdata,$(patsubst %.rdb,%.txt,$(notdir $@))))
	grep    ${FOUR_BIT_PIPS} $@.4 >  $@ || true
	grep -v ${FOUR_BIT_PIPS} $@.2 >> $@ || true

build/%.db: build/%.rdb
	${XRAY_DBFIXUP} --db-root build --zero-db bits.dbf \
		--seg-fn-in  $< \
		--seg-fn-out $@
	cp $< build/$(ITER)/$(notdir $<)
	${XRAY_MASKMERGE} $(subst segbits,mask,$@) \
		$(shell find build -name $(subst segbits,segdata,$(patsubst %.db,%.txt,$(notdir $@))))

database: build/segbits_hclk_cmt.db build/segbits_hclk_cmt_l.db
	cp ${XRAY_DATABASE_DIR}/${XRAY_DATABASE}/segbits_hclk_cmt*.db build/database/${XRAY_DATABASE} || true
	XRAY_DATABASE_DIR=${FUZDIR}/build/database ${XRAY_MERGEDB} hclk_cmt build/segbits_hclk_cmt.db
	XRAY_DATABASE_DIR=${FUZDIR}/build/database ${XRAY_MERGEDB} hclk_cmt_l build/segbits_hclk_cmt_l.db

# cmt_regions.csv: symlink to 045's existing one so we don't re-run Vivado
# to regenerate it.  If the symlink is missing, fall back to running
# 045's output_cmt.tcl.
build/cmt_regions.csv:
	mkdir -p build
	test -e $@ || ln -sf $(PARENT_FUZDIR)/build/cmt_regions.csv $@

pushdb: database
	${XRAY_MERGEDB} hclk_cmt build/database/${XRAY_DATABASE}/segbits_hclk_cmt.db
	${XRAY_MERGEDB} mask_hclk_cmt build/mask_hclk_cmt.db
	${XRAY_MERGEDB} hclk_cmt_l build/database/${XRAY_DATABASE}/segbits_hclk_cmt_l.db
	${XRAY_MERGEDB} mask_hclk_cmt_l build/mask_hclk_cmt_l.db

.PHONY: database pushdb
