# Copyright (C) 2017-2020  The Project X-Ray Authors
#
# Use of this source code is governed by a ISC-style
# license that can be found in the LICENSE file or at
# https://opensource.org/licenses/ISC
#
# SPDX-License-Identifier: ISC
source "$::env(XRAY_DIR)/utils/utils.tcl"
# The cfg fuzzer sweeps BSCANE2 JTAG_CHAIN values to locate config bits; some
# values fail DRC PDRC-2 (BSCAN site vs JTAG_CHAIN mismatch) under newer Vivado,
# which aborts write_bitstream. We only need the resulting config bits, not a
# JTAG-valid design, so disable that DRC check for this fuzzer.
set_property IS_ENABLED 0 [get_drc_checks {PDRC-2}]
generate_top
