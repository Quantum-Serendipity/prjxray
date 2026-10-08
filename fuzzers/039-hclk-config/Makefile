# Copyright (C) 2017-2020  The Project X-Ray Authors.
#
# Use of this source code is governed by a ISC-style
# license that can be found in the LICENSE file or at
# https://opensource.org/licenses/ISC
#
# SPDX-License-Identifier: ISC
N ?= 50

include ../fuzzer.mk

ifeq ($(XRAY_DATABASE),virtex7)
# virtex7 xc7vx485tffg1761-2 is HP-only: top.py targets HCLK_IOI sites and
# the segmaker writes 'segdata_hclk_ioi.txt' (tile type HCLK_IOI). On
# kintex7 the BUFR fuzzer historically only exercised HR sites
# (HCLK_IOI3 → segdata_hclk_ioi3.txt) and HP results were sed-derived; that
# direction is inverted for HP-only parts.
database: build/segbits_hclk_ioi.db

build/segbits_hclk_ioi.rdb: $(SPECIMENS_OK)
	${XRAY_SEGMATCH} -c 5 -o build/segbits_hclk_ioi.rdb \
		$(addsuffix /segdata_hclk_ioi.txt,$(SPECIMENS))

build/segbits_hclk_ioi.db: build/segbits_hclk_ioi.rdb
	${XRAY_DBFIXUP} --db-root build --zero-db bits.dbf \
		--seg-fn-in build/segbits_hclk_ioi.rdb \
		--seg-fn-out build/segbits_hclk_ioi.db
	${XRAY_MASKMERGE} build/mask_hclk_ioi.db \
		$(addsuffix /segdata_hclk_ioi.txt,$(SPECIMENS))
else
# kintex7 and zynq7 have both tile types on some devices. Each is solved
# from its own segdata. xc7z010/xc7z020 have no HCLK_IOI tiles, so that
# solve is skipped when the segdata was not produced.
ifneq (,$(filter $(XRAY_DATABASE),kintex7 zynq7))
database: build/segbits_hclk_ioi3.db build/segbits_hclk_ioi.db
else
database: build/segbits_hclk_ioi3.db
endif
endif

build/segbits_hclk_ioi3.rdb: $(SPECIMENS_OK)
	${XRAY_SEGMATCH} -c 5 -o build/segbits_hclk_ioi3.rdb \
		$(addsuffix /segdata_hclk_ioi3.txt,$(SPECIMENS))

build/segbits_hclk_ioi3.db: build/segbits_hclk_ioi3.rdb
	${XRAY_DBFIXUP} --db-root build --zero-db bits.dbf \
		--seg-fn-in build/segbits_hclk_ioi3.rdb \
		--seg-fn-out build/segbits_hclk_ioi3.db
	${XRAY_MASKMERGE} build/mask_hclk_ioi3.db \
		$(addsuffix /segdata_hclk_ioi3.txt,$(SPECIMENS))

# HP banks are measured, not renamed from the HR solve. A part with no
# HCLK_IOI tiles writes no segdata_hclk_ioi.txt; the solve is then a no-op.
ifneq (,$(filter $(XRAY_DATABASE),kintex7 zynq7))
build/segbits_hclk_ioi.rdb: $(SPECIMENS_OK)
	mkdir -p build
	files="$$(ls $(addsuffix /segdata_hclk_ioi.txt,$(SPECIMENS)) 2>/dev/null || true)"; \
	if [ -z "$$files" ]; then \
		echo "039: no HCLK_IOI segdata on this part"; \
		: > $@; \
	else \
		${XRAY_SEGMATCH} -c 5 -o $@ $$files; \
	fi

build/segbits_hclk_ioi.db: build/segbits_hclk_ioi.rdb
	if [ ! -s $< ]; then \
		: > $@; \
		: > build/mask_hclk_ioi.db; \
	else \
		${XRAY_DBFIXUP} --db-root build --zero-db bits.dbf \
			--seg-fn-in build/segbits_hclk_ioi.rdb \
			--seg-fn-out build/segbits_hclk_ioi.db; \
		${XRAY_MASKMERGE} build/mask_hclk_ioi.db \
			$(addsuffix /segdata_hclk_ioi.txt,$(SPECIMENS)); \
	fi
endif

pushdb: database
# On HP-only parts (virtex7 xc7vx485tffg1761-2) there is no HCLK_IOI3
# segdata, so skip the hclk_ioi3 merge to avoid a missing-file error.
ifneq ($(XRAY_DATABASE),virtex7)
	${XRAY_MERGEDB} hclk_ioi3 build/segbits_hclk_ioi3.db
	${XRAY_MERGEDB} mask_hclk_ioi3 build/mask_hclk_ioi3.db
endif
ifneq (,$(filter $(XRAY_DATABASE),kintex7 zynq7 virtex7))
	${XRAY_MERGEDB} hclk_ioi build/segbits_hclk_ioi.db
	${XRAY_MERGEDB} mask_hclk_ioi build/mask_hclk_ioi.db
endif

.PHONY: database pushdb
