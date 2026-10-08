#!/bin/bash
# Copyright (C) 2017-2021  The Project X-Ray Authors.
#
# Use of this source code is governed by a ISC-style
# license that can be found in the LICENSE file or at
# https://opensource.org/licenses/ISC
#
# SPDX-License-Identifier: ISC
export XRAY_DATABASE="spartan7"
export XRAY_PART="xc7s25csga324-1"
export XRAY_ROI_FRAMES="0x00000000:0xffffffff"

# The fabric geometry below is per device, not per family: xc7s25 is a
# different die from xc7s50, and xc7s75/xc7s100 are a third one.  Selecting it
# from XRAY_PART keeps the default part (xc7s25csga324-1, the Arty S7-25)
# runnable while leaving the other devices reachable with XRAY_PART=...
case "${XRAY_PART}" in
xc7s25*)
    # xc7s25: SLICE X0..X41 over Y0..Y49 and X0..X33 over Y50..Y99 (the
    # top-right clock region is narrower); 3 BRAM and 2 DSP columns.
    export XRAY_ROI_TILEGRID="SLICE_X0Y0:SLICE_X41Y49 SLICE_X0Y50:SLICE_X33Y99 RAMB18_X0Y0:RAMB18_X1Y39 RAMB36_X0Y0:RAMB36_X1Y19 RAMB18_X2Y0:RAMB18_X2Y19 RAMB36_X2Y0:RAMB36_X2Y9 DSP48_X0Y0:DSP48_X1Y39"
    export XRAY_EXCLUDE_ROI_TILEGRID=""
    # See fuzzers/005-tilegrid/generate_full.py: the two IOI3 tiles whose frame
    # address sits one frame above the rest of their column.
    export XRAY_IOI3_TILES="LIOI3_X0Y9 RIOI3_X31Y9"
    # Most of clock region X0Y1.
    export XRAY_ROI="SLICE_X0Y50:SLICE_X15Y99 RAMB18_X0Y20:RAMB18_X0Y39 RAMB36_X0Y10:RAMB36_X0Y19 DSP48_X0Y20:DSP48_X0Y39 IOB_X0Y50:IOB_X0Y99"
    export XRAY_ROI_GRID_X1="10"
    export XRAY_ROI_GRID_X2="50"
    export XRAY_ROI_GRID_Y1="0"
    export XRAY_ROI_GRID_Y2="51"
    ;;
xc7s75*|xc7s100*)
    # xc7s75 and xc7s100 are the same die: SLICE X0..X85 over Y0..Y199,
    # 3 BRAM and 2 DSP columns spanning the full height.
    export XRAY_ROI_TILEGRID="SLICE_X0Y0:SLICE_X85Y199 RAMB18_X0Y0:RAMB18_X2Y79 RAMB36_X0Y0:RAMB36_X2Y39 DSP48_X0Y0:DSP48_X1Y79"
    export XRAY_EXCLUDE_ROI_TILEGRID=""
    export XRAY_IOI3_TILES="LIOI3_X0Y9 RIOI3_X43Y9"
    export XRAY_ROI="SLICE_X0Y150:SLICE_X35Y199 RAMB18_X0Y60:RAMB18_X0Y79 RAMB36_X0Y30:RAMB36_X0Y39 DSP48_X0Y60:DSP48_X0Y79 IOB_X0Y150:IOB_X0Y199"
    export XRAY_ROI_GRID_X1="10"
    export XRAY_ROI_GRID_X2="58"
    export XRAY_ROI_GRID_Y1="0"
    export XRAY_ROI_GRID_Y2="51"
    ;;
*)
    # xc7s50 (and the xc7s6/xc7s15 proxies, which have no fabric of their own).
    # All CLB's in part, all BRAM's in part, all DSP's in part.
    # tcl queries IOB => don't bother adding
    export XRAY_ROI_TILEGRID="SLICE_X0Y0:SLICE_X65Y99 SLICE_X0Y100:SLICE_X57Y149 RAMB18_X0Y0:RAMB18_X1Y59 RAMB36_X0Y0:RAMB36_X1Y29 RAMB18_X2Y0:RAMB18_X2Y39 RAMB36_X2Y0:RAMB36_X2Y19 DSP48_X0Y0:DSP48_X1Y59"
    export XRAY_EXCLUDE_ROI_TILEGRID=""
    # This is used by fuzzers/005-tilegrid/generate_full.py
    # (special handling for frame addresses of certain IOIs -- see the script for details).
    # This needs to be changed for any new device!
    # If you have a FASM mismatch or unknown bits in IOIs, CHECK THIS FIRST.
    export XRAY_IOI3_TILES="LIOI3_X0Y9 RIOI3_X43Y9"
    # These settings must remain in sync
    export XRAY_ROI="SLICE_X0Y100:SLICE_X35Y149 RAMB18_X0Y40:RAMB18_X0Y59 RAMB36_X0Y20:RAMB36_X0Y29 DSP48_X0Y40:DSP48_X0Y59 IOB_X0Y100:IOB_X0Y149"
    # Most of CMT X0Y2.
    export XRAY_ROI_GRID_X1="10"
    export XRAY_ROI_GRID_X2="58"
    # Include VBRK / VTERM
    export XRAY_ROI_GRID_Y1="0"
    export XRAY_ROI_GRID_Y2="51"
    ;;
esac

# clock pin
export XRAY_PIN_00="F14"
# data pins
export XRAY_PIN_01="F13"
export XRAY_PIN_02="E13"
export XRAY_PIN_03="H15"
export XRAY_PIN_04="G15"
export XRAY_PIN_05="K16"
export XRAY_PIN_06="J16"

source $(dirname ${BASH_SOURCE[0]:-$0})/../utils/environment.sh

eval $(python3 ${XRAY_UTILS_DIR}/create_environment.py)
ENV_RET=$?
if [[ $ENV_RET != 0 ]] ; then
	return $ENV_RET
fi
eval $env
