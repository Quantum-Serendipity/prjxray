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

if [ -z "${XRAY_VIVADO_VERSION:-}" ]; then
	echo "FATAL: fuzzer $(basename "$PWD"): XRAY_VIVADO_VERSION is unset;" \
	     "source settings/<family>.sh first. Refusing to produce database output." >&2
	exit 3
fi
if [ "${XRAY_VIVADO_VERSION_VERIFIED:-}" != "$XRAY_VIVADO_VERSION" ]; then
	_xray_v="$(${XRAY_VIVADO} -version 2>/dev/null | head -1 | cut -d' ' -f2)"
	if [ "$_xray_v" != "$XRAY_VIVADO_VERSION" ]; then
		echo "FATAL: fuzzer $(basename "$PWD") requires Vivado ${XRAY_VIVADO_VERSION}," \
		     "found '${_xray_v:-<none>}'. Refusing to produce database output." >&2
		exit 3
	fi
	export XRAY_VIVADO_VERSION_VERIFIED="$_xray_v"
fi
unset _xray_v

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

