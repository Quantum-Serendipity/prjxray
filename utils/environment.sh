# dirs
# Copyright (C) 2017-2020  The Project X-Ray Authors.
#
# Use of this source code is governed by a ISC-style
# license that can be found in the LICENSE file or at
# https://opensource.org/licenses/ISC
#
# SPDX-License-Identifier: ISC
XRAY_ENV_PATH="${BASH_SOURCE[0]:-$0}"
while [ -h "$XRAY_ENV_PATH" ]; do # resolve $XRAY_ENV_PATH until the file is no longer a symlink
  XRAY_UTILS_DIR="$( cd -P "$( dirname "$XRAY_ENV_PATH" )" && pwd )"
  XRAY_ENV_PATH="$(readlink "$XRAY_ENV_PATH")"
  [[ $XRAY_ENV_PATH != /* ]] && XRAY_ENV_PATH="$XRAY_UTILS_DIR/$XRAY_ENV_PATH" # if $XRAY_ENV_PATH was a relative symlink, we need to resolve it relative to the path where the symlink file was located
done
export XRAY_UTILS_DIR="$( cd -P "$( dirname "$XRAY_ENV_PATH" )" && pwd )"
export XRAY_DIR="$( dirname "$XRAY_UTILS_DIR" )"
export XRAY_DATABASE_DIR="${XRAY_DIR}/database"
export XRAY_TOOLS_DIR="${XRAY_DIR}/build/tools"
export XRAY_FUZZERS_DIR="${XRAY_DIR}/fuzzers"
export XRAY_FAMILY_DIR="${XRAY_DATABASE_DIR}/${XRAY_DATABASE}"

if [ -e "${XRAY_DIR}/env/bin/activate" ]; then
  source "${XRAY_DIR}/env/bin/activate"
fi

# misc
# The I/O standard the fuzzers drive their ROI pins with.  Only virtex7 needs a
# different one (its HP banks are 1.8 V) and settings/virtex7.sh sets it before
# sourcing this file; every other family runs the high-range default, which is
# the same fallback the python fuzzers already use (fuzzers/039-hclk-config,
# fuzzers/039a-hclk-bufrclk-perfclk).  Without it the tcl fuzzers abort with
# "no such variable ::env(XRAY_IOSTANDARD)".
export XRAY_IOSTANDARD="${XRAY_IOSTANDARD:-LVCMOS33}"
export XRAY_PART_YAML="${XRAY_DATABASE_DIR}/${XRAY_DATABASE}/${XRAY_PART}/part.yaml"
source $XRAY_UTILS_DIR/environment.python.sh

# Set environment to default output and overwrite localisation settings
export LC_ALL=C

# tools
export XRAY_GENHEADER="${XRAY_UTILS_DIR}/genheader.sh"
export XRAY_BITREAD="${XRAY_TOOLS_DIR}/bitread --part_file ${XRAY_PART_YAML}"
export XRAY_MERGEDB="bash ${XRAY_UTILS_DIR}/mergedb.sh"
export XRAY_DBFIXUP="python3 ${XRAY_UTILS_DIR}/dbfixup.py"
export XRAY_MASKMERGE="bash ${XRAY_UTILS_DIR}/maskmerge.sh"
export XRAY_SEGMATCH="${XRAY_TOOLS_DIR}/segmatch"
export XRAY_SEGPRINT="python3 ${XRAY_UTILS_DIR}/segprint.py"
export XRAY_BIT2FASM="python3 ${XRAY_UTILS_DIR}/bit2fasm.py"
export XRAY_FASM2FRAMES="python3 ${XRAY_UTILS_DIR}/fasm2frames.py"
export XRAY_BITTOOL="${XRAY_TOOLS_DIR}/bittool"
export XRAY_BLOCKWIDTH="python3 ${XRAY_UTILS_DIR}/blockwidth.py"
export XRAY_PARSEDB="python3 ${XRAY_UTILS_DIR}/parsedb.py"
export XRAY_TCL_REFORMAT="${XRAY_UTILS_DIR}/tcl-reformat.sh"
export XRAY_VIVADO="${XRAY_UTILS_DIR}/vivado.sh"

# Verify an approved version is in use.  XRAY_VIVADO_VERSION lets a lab select
# a different release without editing this file; the database is generated from
# whatever Vivado reports here, so it is worth stating explicitly.
export XRAY_VIVADO_SETTINGS="${XRAY_VIVADO_SETTINGS:-/tools/Xilinx/Vivado/2024.1/settings64.sh}"
export XRAY_VIVADO_VERSION="${XRAY_VIVADO_VERSION:-v2024.1}"
if [ "$(${XRAY_VIVADO} -h |grep Vivado |cut -d\  -f 2)" != "${XRAY_VIVADO_VERSION}" ] ; then
    echo "Requires Vivado ${XRAY_VIVADO_VERSION#v}; set XRAY_VIVADO_VERSION to use another."
    # Can't exit since sourced script
    # Trash a key environment variable to preclude use
    export XRAY_DIR="/bad/vivado/version"
    return
fi
