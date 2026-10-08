# Copyright (C) 2017-2020  The Project X-Ray Authors.
#
# Use of this source code is governed by a ISC-style
# license that can be found in the LICENSE file or at
# https://opensource.org/licenses/ISC
#
# SPDX-License-Identifier: ISC
export XRAY_DATABASE="virtex7"
export XRAY_PART="xc7vx485tffg1761-2"
export XRAY_ROI_FRAMES="0x00000000:0xffffffff"

# Virtex-7 "VX" parts are high-performance-bank only: their general purpose I/O
# cannot drive 3.3V. LVCMOS18 is valid on both HP and HR banks, so it is used as
# the default I/O standard for all virtex7 fuzzers (see ${XRAY_IOSTANDARD}).
export XRAY_IOSTANDARD="LVCMOS18"

# Virtex-7 synthesis only checks out a license under Vivado 2020.1 on this setup
# (2017.2's license manager refuses xc7vx485t). Override the install path and the
# version gate for this family. Both are overridable from the environment.
export XRAY_VIVADO_SETTINGS="${XRAY_VIVADO_SETTINGS:-/opt/Xilinx/Vivado/2020.1/settings64.sh}"
export XRAY_VIVADO_VERSION="${XRAY_VIVADO_VERSION:-v2020.1.1}"

# NOTE: The tilegrid/ROI coordinates below are device-specific (xc7vx485t) and
# MUST be verified against Vivado before running the full database build. They
# are seeded from the equivalently-sized kintex7 xc7k325t layout as a starting
# point. To regenerate them for this part, inspect the part in Vivado (or the
# output of fuzzers/005-tilegrid) and update the SLICE/DSP48/RAMB/IOB extents.
export XRAY_ROI_TILEGRID="SLICE_X0Y0:SLICE_X153Y349 DSP48_X0Y0:DSP48_X5Y139 RAMB18_X0Y0:RAMB18_X5Y139 RAMB36_X0Y0:RAMB36_X4Y69"

export XRAY_EXCLUDE_ROI_TILEGRID=""

# The IOI tiles whose frame address sits one frame higher than the rest
# (handled specially by 005-tilegrid/generate_full.py propagate_IOI_Y9).
# On Virtex-7 these are the HP IOI (LIOI/RIOI) tiles at Y10, verified from the
# generated tilegrid (cf. Kintex-7's LIOI3_X0Y9).
export XRAY_IOI3_TILES="LIOI_X82Y10 RIOI_X311Y10"

# Compact fuzzing region for the placement-based main fuzzers (010-100). The
# SLICE block must densely cover adjacent CLBLL and CLBLM columns: with only the
# CLBLM columns here (X2,3,6,7,10,11 -> 6 cols x 50 x 4 = 1200 LUTs) the 2000-LUT
# fuzzer design cannot fit, so the placer is forced to also use the CLBLL columns
# (X0,1,4,5,8,9). A too-large region lets the placer pack everything into CLBLM
# (which produced no CLBLL segdata -> segmatch error). DSP48/RAMB/IOB ranges
# cover those resources for the DSP/BRAM/IOB fuzzers.
# Positioned at Y50-99 (clock region 1), OFF the device bottom edge: edge tiles
# at Y0 can't exercise features like BRAM36 ECC/cascade (RAM_EXTENSION), which
# fails segmaker's all-tags-used check (027-bram36-config). RAMB36 here is at
# Y10-19 so cascade is exercisable. Same CLBLL/CLBLM columns as before.
export XRAY_ROI="SLICE_X0Y50:SLICE_X11Y99 DSP48_X0Y10:DSP48_X5Y29 RAMB18_X0Y20:RAMB18_X5Y39 RAMB36_X0Y10:RAMB36_X5Y19 IOB_X0Y50:IOB_X0Y99"
# Grid (tile) coordinates of the same physical region as XRAY_ROI above, used by
# fuzzers that go through util.get_roi()/roi_xy() (e.g. 018-clb-ram). On
# xc7vx485t the XRAY_ROI SLICE block (X0-11, Y0-49) maps to grid_x 5-19,
# grid_y 313-363; bounds are exclusive on the high end.
export XRAY_ROI_GRID_X1="5"
export XRAY_ROI_GRID_X2="20"
export XRAY_ROI_GRID_Y1="261"
export XRAY_ROI_GRID_Y2="312"

source $(dirname ${BASH_SOURCE[0]})/../utils/environment.sh

env=$(python3 ${XRAY_UTILS_DIR}/create_environment.py)
ENV_RET=$?
if [[ $ENV_RET != 0 ]] ; then
	return $ENV_RET
fi
eval $env
