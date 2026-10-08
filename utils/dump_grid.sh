#!/bin/bash
# Copyright (C) 2017-2020  The Project X-Ray Authors.
#
# Use of this source code is governed by a ISC-style
# license that can be found in the LICENSE file or at
# https://opensource.org/licenses/ISC
#
# SPDX-License-Identifier: ISC
#
# Dump the tile grid of one part, in the format 005-tilegrid reads.
#
# This is 005-tilegrid's own generate_tiles.tcl, run on its own: since it
# reads the grid off a device with nothing placed on it, it needs no ROI
# design, no fuzzer build and no settings file for the part -- which is the
# point, because a part that has no settings file yet is exactly the one whose
# grid you want to look at.  Feed the result to
# `utils/tilegrid_derive.py derive`, or diff it against the tiles.txt a
# fuzzer run produced.
#
# usage: utils/dump_grid.sh <part> [output directory]
#
# Source a settings file of the family first (it is what sets XRAY_VIVADO).
# XRAY_EXCLUDE_ROI_TILEGRID, the list of tiles to blank out, defaults to none.

set -e

part="${1:-${XRAY_PART}}"
out="${2:-.}"

if [ -z "${part}" ]; then
    echo "usage: $0 <part> [output directory]" >&2
    exit 1
fi
if [ -z "${XRAY_VIVADO}" ] || [ -z "${XRAY_FUZZERS_DIR}" ]; then
    echo "$0: source a settings file of the family first" >&2
    exit 1
fi

mkdir -p "${out}"
cd "${out}"

export XRAY_PART="${part}"
export XRAY_EXCLUDE_ROI_TILEGRID="${XRAY_EXCLUDE_ROI_TILEGRID:-}"
export FUZDIR="${XRAY_FUZZERS_DIR}/005-tilegrid"

"${XRAY_VIVADO}" -mode batch -source "${FUZDIR}/generate_tiles.tcl" \
    -nojournal -log dump_grid.log

echo "$0: wrote $(pwd)/tiles.txt and $(pwd)/pin_func.txt for ${part}"
