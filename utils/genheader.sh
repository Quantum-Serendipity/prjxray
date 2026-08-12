# header for fuzzer generate.sh scripts
# Copyright (C) 2017-2020  The Project X-Ray Authors.
#
# Use of this source code is governed by a ISC-style
# license that can be found in the LICENSE file or at
# https://opensource.org/licenses/ISC
#
# SPDX-License-Identifier: ISC

if [ -z "$XRAY_DATABASE" ]; then
	echo "No XRAY environment found. Make sure to source the settings file first!"
	exit 1
fi

# --- Vivado version gate -----------------------------------------------------
#
# Every one of the 42 fuzzers sources this file, so one gate here covers all of
# them. It exists because utils/environment.sh's gate is SOFT in four ways:
#
#   1. it prints to stdout, not stderr, so under `make -j --output-sync` the
#      message is buried in the build log;
#   2. it does not exit -- it poisons XRAY_DIR="/bad/vivado/version", so what
#      the operator actually sees is a wall of "No such file or directory";
#   3. it runs ONCE, when settings/<family>.sh is sourced. Nothing re-checks
#      for the rest of a 20-hour campaign, and on a cluster each worker sources
#      its own environment;
#   4. its default is a PATH assertion (/opt/Xilinx/Vivado/2017.2/settings64.sh),
#      not a VERSION assertion. Anything installed at that path passes it.
#
# This matters more than an ordinary version check because NOTHING STAMPS THE
# VIVADO VERSION INTO DATABASE OUTPUT. segdata_*.txt, segbits_*.db and
# mask_*.db carry no provenance, so a database half-fuzzed on two versions is
# silently wrong and no existing check catches it. The versions do genuinely
# disagree: across 2017.2 -> 2019.1, JTAG_CHAIN stops being emitted, WA8USED
# gains a bit, and MMCM entries differ.
#
# `exit` from a sourced file exits the calling generate.sh, which propagates
# through make and stops the build. That is the desired behaviour: a version
# mismatch must ABORT the campaign, not degrade it.
#
# Cost: `vivado -version` measured at 0.72 s warm, once per fuzzer, and cached
# for the rest of the process tree via the exported variable below.
: "${XRAY_VIVADO_REQUIRED_VERSION:=v2017.2}"
if [ -z "${XRAY_VIVADO_VERSION_VERIFIED:-}" ]; then
	_xray_v="$(${XRAY_VIVADO} -version 2>/dev/null | head -1 | cut -d' ' -f2)"
	if [ "$_xray_v" != "$XRAY_VIVADO_REQUIRED_VERSION" ]; then
		echo "FATAL: fuzzer $(basename "$PWD") requires Vivado ${XRAY_VIVADO_REQUIRED_VERSION}," \
		     "found '${_xray_v:-<none>}'. Refusing to produce database output." >&2
		exit 3
	fi
	export XRAY_VIVADO_VERSION_VERIFIED="$_xray_v"
fi
unset _xray_v
# --- end Vivado version gate -------------------------------------------------

set -ex

export FUZDIR=$PWD

# for some reason on sourced script set -e doesn't work
# Scripts may have additional arguments, but first is reserved for build directory
test $# -ge 1 || exit 1
test ! -e "$SPECDIR"
export SPECDIR=$1

mkdir -p "$SPECDIR"
cd "$SPECDIR"

export SEED="$(echo $SPECDIR | md5sum | cut -c1-8)"
export SEEDN="$(basename $(pwd) |sed s/specimen_0*//)"

function seed_vh () {
    echo '`define SEED 32'"'h${SEED}" > setseed.vh
}

