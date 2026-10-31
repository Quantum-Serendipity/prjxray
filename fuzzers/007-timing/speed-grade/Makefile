# Copyright (C) 2017-2026  The Project X-Ray Authors.
#
# Use of this source code is governed by a ISC-style
# license that can be found in the LICENSE file or at
# https://opensource.org/licenses/ISC
#
# SPDX-License-Identifier: ISC
all: build/regrade.ok

clean:
	rm -rf build

build/dump.ok:
	mkdir -p build
	cd build && ${XRAY_VIVADO} -mode batch -source ../dump_speed.tcl
	touch build/dump.ok

build/compare.ok: build/dump.ok
	python3 regrade.py --db-family ${XRAY_FAMILY_DIR} --dump build --compare-only
	touch build/compare.ok

build/regrade.ok: build/dump.ok
	python3 regrade.py --db-family ${XRAY_FAMILY_DIR} --dump build $(if $(SPEED_SDFS),--sdfs $(SPEED_SDFS))
	touch build/regrade.ok
