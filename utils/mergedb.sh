#!/bin/bash
# Copyright (C) 2017-2020  The Project X-Ray Authors.
#
# Use of this source code is governed by a ISC-style
# license that can be found in the LICENSE file or at
# https://opensource.org/licenses/ISC
#
# SPDX-License-Identifier: ISC

# $1: DB type
# $2: filename to merge in

set -ex
test $# = 2
test -e "$2"

tmp1=`mktemp -p .`
tmp2=`mktemp -p .`
tmp3=`mktemp -p .`
tmp4=`mktemp -p .`

LOCKDIR="$XRAY_DATABASE_DIR/$XRAY_DATABASE"
mkdir -p "$LOCKDIR"

db=$LOCKDIR/segbits_$1.db
db_origin=$LOCKDIR/segbits_$1.origin_info.db
ismask=false
if [[ "$1" == mask_* ]]; then
	db=$LOCKDIR/$1.db
	ismask=true
fi

LOCKFILE="${LOCKFILE:-$LOCKDIR/.$(basename "$db").lock}"
LOCKTIMEOUT="${LOCKTIMEOUT:-30}"
LOCKID=222

created=()
landed=false
function finish {
    echo "Cleaning up temp files"
    rm -f "$tmp1" "$tmp2" "$tmp3" "$tmp4"
    if ! $landed ; then
        for f in "${created[@]}"; do
            rm -f "$f"
        done
    fi
}
trap finish EXIT

_stamp="$XRAY_DATABASE_DIR/$XRAY_DATABASE/.vivado-version"
if [ -z "${XRAY_VIVADO_VERSION_VERIFIED:-}" ] && [ -n "${XRAY_VIVADO:-}" ]; then
	XRAY_VIVADO_VERSION_VERIFIED="$(${XRAY_VIVADO} -version 2>/dev/null \
		| head -1 | cut -d' ' -f2)" || XRAY_VIVADO_VERSION_VERIFIED=""
fi
if [ -z "${XRAY_VIVADO_VERSION_VERIFIED:-}" ]; then
	echo "WARNING: mergedb.sh cannot determine a Vivado version" \
	     "(XRAY_VIVADO='${XRAY_VIVADO:-<unset>}'); the .vivado-version stamp" \
	     "will NOT be written and this merge will not be attributed." >&2
	if [ "${XRAY_VIVADO_STAMP_REQUIRED:-0}" = 1 ]; then
		echo "FATAL: XRAY_VIVADO_STAMP_REQUIRED=1 and no version is available." >&2
		exit 5
	fi
fi

eval "exec $LOCKID>\"\$LOCKFILE\""
if ! flock --timeout "$LOCKTIMEOUT" $LOCKID; then
	echo "Timeout while waiting for lock on $LOCKFILE" >&2
	exit 1
fi

if [ -n "${XRAY_VIVADO_VERSION_VERIFIED:-}" ] && [ -e "$_stamp" ] \
   && [ "$(cat "$_stamp")" != "$XRAY_VIVADO_VERSION_VERIFIED" ]; then
	echo "FATAL: database at $XRAY_DATABASE_DIR/$XRAY_DATABASE was built with" \
	     "$(cat "$_stamp"), this process is $XRAY_VIVADO_VERSION_VERIFIED." \
	     "Refusing to merge: mixing versions corrupts the database silently." >&2
	exit 4
fi

# Check existing DB
if [ -f $db ] ; then
    ${XRAY_PARSEDB} --strict "$db"
fi

# Check new DB
${XRAY_PARSEDB} --strict "$2"

# Fuzzers verify L/R data is equivilent
# However, expand back to L/R to make downstream tools not depend on this
# in case we later find exceptions

case "$1" in
	clbll_l)
		sed < "$2" > "$tmp1" \
			-e 's/^CLB\.SLICE_X0\./CLBLL_L.SLICEL_X0./' \
			-e 's/^CLB\.SLICE_X1\./CLBLL_L.SLICEL_X1./' ;;
	clbll_r)
		sed < "$2" > "$tmp1" \
			-e 's/^CLB\.SLICE_X0\./CLBLL_R.SLICEL_X0./' \
			-e 's/^CLB\.SLICE_X1\./CLBLL_R.SLICEL_X1./' ;;
	clblm_l)
		sed < "$2" > "$tmp1" \
			-e 's/^CLB\.SLICE_X0\./CLBLM_L.SLICEM_X0./' \
			-e 's/^CLB\.SLICE_X1\./CLBLM_L.SLICEL_X1./' ;;
	clblm_r)
		sed < "$2" > "$tmp1" \
			-e 's/^CLB\.SLICE_X0\./CLBLM_R.SLICEM_X0./' \
			-e 's/^CLB\.SLICE_X1\./CLBLM_R.SLICEL_X1./' ;;

	dsp_l)
		sed < "$2" > "$tmp1" -e 's/^DSP\./DSP_L./' ;;
	dsp_r)
		sed < "$2" > "$tmp1" -e 's/^DSP\./DSP_R./' ;;

	bram_l)
		sed < "$2" > "$tmp1" -e 's/^BRAM\./BRAM_L./' ;;
	bram_r)
		sed < "$2" > "$tmp1" -e 's/^BRAM\./BRAM_R./' ;;

	bram_l.block_ram)
		sed < "$2" > "$tmp1" -e 's/^BRAM\./BRAM_L./' ;;
	bram_r.block_ram)
		sed < "$2" > "$tmp1" -e 's/^BRAM\./BRAM_R./' ;;

	int_l)
		sed < "$2" > "$tmp1" -e 's/^INT\./INT_L./' ;;
	int_r)
		sed < "$2" > "$tmp1" -e 's/^INT\./INT_R./' ;;

	hclk_l)
		sed < "$2" > "$tmp1" -e 's/^HCLK\./HCLK_L./' ;;
	hclk_r)
		sed < "$2" > "$tmp1" -e 's/^HCLK\./HCLK_R./' ;;

	clk_hrow_bot_r)
		sed < "$2" > "$tmp1" -e 's/^CLK_HROW\./CLK_HROW_BOT_R./' ;;
	clk_hrow_top_r)
		sed < "$2" > "$tmp1" -e 's/^CLK_HROW\./CLK_HROW_TOP_R./' ;;

	clk_bufg_bot_r)
		sed < "$2" > "$tmp1" -e 's/^CLK_BUFG\./CLK_BUFG_BOT_R./' ;;
	clk_bufg_top_r)
		sed < "$2" > "$tmp1" -e 's/^CLK_BUFG\./CLK_BUFG_TOP_R./' ;;

	hclk_cmt)
		cp "$2" "$tmp1" ;;
	hclk_cmt_l)
		sed < "$2" > "$tmp1" -e 's/^HCLK_CMT\./HCLK_CMT_L./' ;;

	clk_bufg_rebuf)
		cp "$2" "$tmp1" ;;

	liob33)
		sed < "$2" > "$tmp1" -e 's/^IOB33\./LIOB33./' ;;

	riob33)
		sed < "$2" > "$tmp1" -e 's/^IOB33\./RIOB33./' ;;

	riob18)
		sed < "$2" > "$tmp1" -e 's/^IOB18\./RIOB18./' ;;

	liob18)
		sed < "$2" > "$tmp1" -e 's/^IOB18\./LIOB18./' ;;

	riob18_sing)
		sed < "$2" > "$tmp1" -e 's/^IOB18\./RIOB18_SING./' ;;

	liob18_sing)
		sed < "$2" > "$tmp1" -e 's/^IOB18\./LIOB18_SING./' ;;

	mask_liob18)
		sed < "$2" > "$tmp1" -e 's/^IOB18\./LIOB18./' ;;

	lioi3)
		sed < "$2" > "$tmp1" -e 's/^IOI3\./LIOI3./' ;;

	lioi3_tbytesrc)
		sed < "$2" > "$tmp1" -e 's/^IOI3\./LIOI3_TBYTESRC./' ;;

	lioi3_tbyteterm)
		sed < "$2" > "$tmp1" -e 's/^IOI3\./LIOI3_TBYTETERM./' ;;

	rioi3)
		sed < "$2" > "$tmp1" -e 's/^IOI3\./RIOI3./' ;;

	rioi)
		sed < "$2" > "$tmp1" -e 's/^IOI\./RIOI./' ;;

	rioi3_tbytesrc)
		sed < "$2" > "$tmp1" -e 's/^IOI3\./RIOI3_TBYTESRC./' ;;

	rioi_tbytesrc)
		sed < "$2" > "$tmp1" -e 's/^IOI\./RIOI_TBYTESRC./' ;;

	rioi3_tbyteterm)
		sed < "$2" > "$tmp1" -e 's/^IOI3\./RIOI3_TBYTETERM./' ;;

	rioi_tbyteterm)
		sed < "$2" > "$tmp1" -e 's/^IOI\./RIOI_TBYTETERM./' ;;

	lioi)
		sed < "$2" > "$tmp1" -e 's/^IOI\./LIOI./' ;;

	lioi_tbytesrc)
		sed < "$2" > "$tmp1" -e 's/^IOI\./LIOI_TBYTESRC./' ;;

	lioi_tbyteterm)
		sed < "$2" > "$tmp1" -e 's/^IOI\./LIOI_TBYTETERM./' ;;

	lioi_sing)
		# 036-iob18-ologic-sing fuzzer emits LIOI_SING. / RIOI_SING.
		# entries in one file; for the lioi_sing target keep only
		# the LIOI_SING.* rows and pass them through verbatim.
		grep '^LIOI_SING\.' "$2" > "$tmp1" || true ;;

	rioi_sing)
		grep '^RIOI_SING\.' "$2" > "$tmp1" || true ;;

	cmt_top_r_upper_t)
		sed < "$2" > "$tmp1" -e 's/^CMT_UPPER_T\./CMT_TOP_R_UPPER_T./' ;;

	cmt_top_l_upper_t)
		sed < "$2" > "$tmp1" -e 's/^CMT_UPPER_T\./CMT_TOP_L_UPPER_T./' ;;

	cmt_top_r_lower_b)
		sed < "$2" > "$tmp1" -e 's/^CMT_LOWER_B\./CMT_TOP_R_LOWER_B./' ;;

	cmt_top_l_lower_b)
		sed < "$2" > "$tmp1" -e 's/^CMT_LOWER_B\./CMT_TOP_L_LOWER_B./' ;;

	cfg_center_mid)
		cp "$2" "$tmp1" ;;

	hclk_ioi3)
		cp "$2" "$tmp1" ;;

	hclk_ioi)
		cp "$2" "$tmp1" ;;

	pcie_bot)
		cp "$2" "$tmp1" ;;

	pcie_int_interface_l)
		sed < "$2" > "$tmp1" -e 's/^PCIE_INT_INTERFACE\./PCIE_INT_INTERFACE_L./' ;;

	pcie_int_interface_r)
		sed < "$2" > "$tmp1" -e 's/^PCIE_INT_INTERFACE\./PCIE_INT_INTERFACE_R./' ;;

	gtp_common)
		cp "$2" "$tmp1" ;;

	gtp_common_mid_left)
		sed < "$2" > "$tmp1" -e 's/^GTP_COMMON\./GTP_COMMON_MID_LEFT./' ;;

	gtp_common_mid_right)
		sed < "$2" > "$tmp1" -e 's/^GTP_COMMON\./GTP_COMMON_MID_RIGHT./' ;;

	gtp_channel_0)
		sed < "$2" > "$tmp1" -e 's/^GTP_CHANNEL\./GTP_CHANNEL_0./' ;;

	gtp_channel_1)
		sed < "$2" > "$tmp1" -e 's/^GTP_CHANNEL\./GTP_CHANNEL_1./' ;;

	gtp_channel_2)
		sed < "$2" > "$tmp1" -e 's/^GTP_CHANNEL\./GTP_CHANNEL_2./' ;;

	gtp_channel_3)
		sed < "$2" > "$tmp1" -e 's/^GTP_CHANNEL\./GTP_CHANNEL_3./' ;;

	gtp_channel_0_mid_left)
		sed < "$2" > "$tmp1" -e 's/^GTP_CHANNEL\./GTP_CHANNEL_0_MID_LEFT./' ;;

	gtp_channel_1_mid_left)
		sed < "$2" > "$tmp1" -e 's/^GTP_CHANNEL\./GTP_CHANNEL_1_MID_LEFT./' ;;

	gtp_channel_2_mid_left)
		sed < "$2" > "$tmp1" -e 's/^GTP_CHANNEL\./GTP_CHANNEL_2_MID_LEFT./' ;;

	gtp_channel_3_mid_left)
		sed < "$2" > "$tmp1" -e 's/^GTP_CHANNEL\./GTP_CHANNEL_3_MID_LEFT./' ;;

	gtp_channel_0_mid_right)
		sed < "$2" > "$tmp1" -e 's/^GTP_CHANNEL\./GTP_CHANNEL_0_MID_RIGHT./' ;;

	gtp_channel_1_mid_right)
		sed < "$2" > "$tmp1" -e 's/^GTP_CHANNEL\./GTP_CHANNEL_1_MID_RIGHT./' ;;

	gtp_channel_2_mid_right)
		sed < "$2" > "$tmp1" -e 's/^GTP_CHANNEL\./GTP_CHANNEL_2_MID_RIGHT./' ;;

	gtp_channel_3_mid_right)
		sed < "$2" > "$tmp1" -e 's/^GTP_CHANNEL\./GTP_CHANNEL_3_MID_RIGHT./' ;;

	gtp_int_interface_l)
		sed < "$2" > "$tmp1" -e 's/^GTP_INT_INTERFACE\.GTPE2_INT/GTP_INT_INTERFACE_L\.GTPE2_INT_LEFT/' ;;

	gtp_int_interface_r)
		sed < "$2" > "$tmp1" -e 's/^GTP_INT_INTERFACE\.GTPE2_INT/GTP_INT_INTERFACE_R\.GTPE2_INT_R/' ;;

	gtp_int_interface)
		cp "$2" "$tmp1" ;;

	gtx_common)
		cp "$2" "$tmp1" ;;

	gtx_common_mid_left)
		sed < "$2" > "$tmp1" -e 's/^GTX_COMMON\./GTX_COMMON_MID_LEFT./' ;;

	gtx_common_mid_right)
		sed < "$2" > "$tmp1" -e 's/^GTX_COMMON\./GTX_COMMON_MID_RIGHT./' ;;

	gtx_channel_0)
		sed < "$2" > "$tmp1" -e 's/^GTX_CHANNEL\./GTX_CHANNEL_0./' ;;

	gtx_channel_1)
		sed < "$2" > "$tmp1" -e 's/^GTX_CHANNEL\./GTX_CHANNEL_1./' ;;

	gtx_channel_2)
		sed < "$2" > "$tmp1" -e 's/^GTX_CHANNEL\./GTX_CHANNEL_2./' ;;

	gtx_channel_3)
		sed < "$2" > "$tmp1" -e 's/^GTX_CHANNEL\./GTX_CHANNEL_3./' ;;

	gtx_channel_0_mid_left)
		sed < "$2" > "$tmp1" -e 's/^GTX_CHANNEL\./GTX_CHANNEL_0_MID_LEFT./' ;;

	gtx_channel_1_mid_left)
		sed < "$2" > "$tmp1" -e 's/^GTX_CHANNEL\./GTX_CHANNEL_1_MID_LEFT./' ;;

	gtx_channel_2_mid_left)
		sed < "$2" > "$tmp1" -e 's/^GTX_CHANNEL\./GTX_CHANNEL_2_MID_LEFT./' ;;

	gtx_channel_3_mid_left)
		sed < "$2" > "$tmp1" -e 's/^GTX_CHANNEL\./GTX_CHANNEL_3_MID_LEFT./' ;;

	gtx_channel_0_mid_right)
		sed < "$2" > "$tmp1" -e 's/^GTX_CHANNEL\./GTX_CHANNEL_0_MID_RIGHT./' ;;

	gtx_channel_1_mid_right)
		sed < "$2" > "$tmp1" -e 's/^GTX_CHANNEL\./GTX_CHANNEL_1_MID_RIGHT./' ;;

	gtx_channel_2_mid_right)
		sed < "$2" > "$tmp1" -e 's/^GTX_CHANNEL\./GTX_CHANNEL_2_MID_RIGHT./' ;;

	gtx_channel_3_mid_right)
		sed < "$2" > "$tmp1" -e 's/^GTX_CHANNEL\./GTX_CHANNEL_3_MID_RIGHT./' ;;

	gtx_int_interface_l)
		sed < "$2" > "$tmp1" -e 's/^GTX_INT_INTERFACE\.GTXE2_INT/GTX_INT_INTERFACE_L\.GTXE2_INT_LEFT/' ;;

	gtx_int_interface_r)
		sed < "$2" > "$tmp1" -e 's/^GTX_INT_INTERFACE\.GTXE2_INT/GTX_INT_INTERFACE_R\.GTXE2_INT_R/' ;;

	gtx_int_interface)
		cp "$2" "$tmp1" ;;

	mask_*)
		cp "$2" "$tmp1" ;;

	*)
		echo "Invalid mode: $1"
		rm -f "$tmp1" "$tmp2"
		exit 1
esac

if [ "${XRAY_CROSSVALIDATE:-1}" = 1 ] && [ -n "${XC7DPR_ROOT:-}" ] ; then
	python3 "$XC7DPR_ROOT/scripts/prjcombine-crossvalidate.py" \
		--gate "$1" "$tmp1" \
		--db-root "${PRJXRAY_DB_DIR:-$XRAY_DATABASE_DIR}" \
		--family "$XRAY_DATABASE"
fi

[ -e "$db" ] || created+=("$db")
touch "$db"
if $ismask ; then
    sort -u "$tmp1" "$db" | grep -v '<.*>' > "$tmp2" || true
else
    if [ ! -f "$db_origin" ] ; then
        created+=("$db_origin")
        touch "$db_origin"
    fi
    # tmp1 must be placed second to take precedence over old bad entries
    python3 ${XRAY_DIR}/utils/mergedb.py --out "$tmp3" "$db_origin" "$tmp1" --track_origin
    python3 ${XRAY_DIR}/utils/mergedb.py --out "$tmp2" "$db" "$tmp1"
fi
# Check aggregate db for consistency and make canonical
${XRAY_PARSEDB} --strict "$tmp2" "$tmp4"
cat "$tmp4" > "$db"
if ! $ismask ; then
    cat "$tmp3" > "$db_origin"
fi
landed=true

if [ "${XRAY_SORTDB:-1}" = 1 ]; then
	if $ismask ; then
		PYTHONPATH="${XRAY_DIR}${PYTHONPATH:+:$PYTHONPATH}" \
			python3 ${XRAY_DIR}/utils/sort_db.py "$db"
	else
		PYTHONPATH="${XRAY_DIR}${PYTHONPATH:+:$PYTHONPATH}" \
			python3 ${XRAY_DIR}/utils/sort_db.py "$db" "$db_origin"
	fi
fi

if [ -n "${XRAY_VIVADO_VERSION_VERIFIED:-}" ]; then
	echo "$XRAY_VIVADO_VERSION_VERIFIED" > "$_stamp"
fi
