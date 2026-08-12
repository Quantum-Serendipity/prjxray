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

# The lock must live on the SAME FILESYSTEM as the database it guards.
#
# The previous default was a hard-coded node-local "/tmp/segbits_$1.db.lock"
# while the database it protects lives under $XRAY_DATABASE_DIR, which on a
# distributed run is SHARED storage. Two workers on two nodes then take two
# different locks, both believe they hold it, and perform an unguarded
# read-modify-write of the same segbits file -- so merges are SILENTLY LOST.
# Nothing reports it: each worker's mergedb.sh exits 0.
#
# Note this was not a missing variable -- LOCKFILE and LOCKTIMEOUT already
# existed here. They were simply hard-coded, and a plain assignment also means
# an exported LOCKFILE from the environment was overwritten rather than
# honoured. Both are now overridable, with a default that sits beside the
# database rather than beside the process.
LOCKDIR="$XRAY_DATABASE_DIR/$XRAY_DATABASE"
LOCKFILE="${LOCKFILE:-$LOCKDIR/.segbits_$1.db.lock}"
LOCKTIMEOUT="${LOCKTIMEOUT:-30}" # seconds
LOCKID=222
mkdir -p "$LOCKDIR"

function finish {
    echo "Cleaning up temp files"
    rm -f "$tmp1"
    rm -f "$tmp2"
}
trap finish EXIT

db=$XRAY_DATABASE_DIR/$XRAY_DATABASE/segbits_$1.db

# --- Vivado version provenance stamp -----------------------------------------
#
# This is the ONLY layer that catches a PARTIALLY REBUILT database -- the case
# where some segbits came from one Vivado version and some from another,
# because three fuzzers were re-run last month under a different install.
#
# Environment gates cannot see that: they check the version of the process that
# is running now, not the version that produced the bytes already on disk. Only
# the artefacts can carry that, and upstream's do not -- segdata_*.txt,
# segbits_*.db and mask_*.db have no provenance field at all.
#
# Adopting an existing database that predates this patch: write the stamp by
# hand from that tree's known provenance, e.g.
#   echo v2017.2 > "$XRAY_FAMILY_DIR/.vivado-version"
# An absent stamp is treated as "unknown, adopt it", NOT as a mismatch --
# otherwise this patch would refuse to merge into every existing checkout.
#
# --- WHERE THE VERSION COMES FROM, and why it is derived HERE ---------------
#
# The first version of this patch read XRAY_VIVADO_VERSION_VERIFIED and did
# nothing at all when it was empty. It was ALWAYS empty. utils/genheader.sh
# exports it, but genheader.sh is sourced by generate.sh, and `pushdb` is a
# different make recipe in a different process; the variable does not cross
# that boundary. Measured in Unit 3.1: four merges, four `[ -n '' ]`, zero
# stamps written. The layer described two paragraphs up as "the ONLY layer that
# catches a PARTIALLY REBUILT database" had never once fired.
#
# The alternative fix was to export the variable from settings/<family>.sh,
# which is sourced once per campaign and would reach pushdb for free. It is
# rejected for two reasons, the second decisive:
#
#   1. Coverage. A settings export reaches only processes descended from a
#      shell that sourced settings. It does not reach a hand-run mergedb.sh --
#      which is how a database gets adopted or repaired -- and it does not
#      reach a distributed worker that assembled its own environment, which is
#      exactly the case the LOCKFILE fix above exists for. The stamp needs to
#      cover every path that can WRITE the database, and that set is defined by
#      this script, not by who sourced what.
#   2. It would silently disable the layer above it. genheader.sh's gate is
#      `if [ -z "${XRAY_VIVADO_VERSION_VERIFIED:-}" ]` -- it SKIPS its own
#      version check when the variable is already populated. Exporting it at
#      settings time turns a per-fuzzer re-check into a once-per-campaign
#      check, which is criticism 3 of the four that the genheader patch was
#      written to fix. Buying the stamp by breaking the gate above it is a bad
#      trade.
#
# So: honour an inherited value if there is one (free, and it is what
# genheader.sh exports on the paths that do reach here), otherwise derive it.
#
# Cost: `vivado.sh -version` measured at 0.24 s warm on this host, once per
# mergedb.sh process, against ~229 ${XRAY_MERGEDB} sites across the 42 fuzzers
# -- ~55 s over a five-family campaign that runs for tens of hours.
#
# If no version can be derived at all, say so on stderr rather than silently
# skipping the stamp: an unwritten stamp is how this guard spent its entire
# life inert. XRAY_VIVADO_STAMP_REQUIRED=1 turns that into a hard failure, for
# a CI that wants "the database is always attributed" to be an invariant.
_stamp="$XRAY_DATABASE_DIR/$XRAY_DATABASE/.vivado-version"
if [ -z "${XRAY_VIVADO_VERSION_VERIFIED:-}" ] && [ -n "${XRAY_VIVADO:-}" ]; then
	# `set -e` is on: a failing command substitution in an assignment would
	# abort the merge, so the failure is absorbed and reported below.
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

# if the DB exists acquire a lock
if [ -f $db ]; then
	eval "exec $LOCKID>$LOCKFILE"
	# the lock is automatically released on script exit
	flock --timeout $LOCKTIMEOUT $LOCKID
	if [ ! $? -eq 0 ]; then
		echo "Timeout while waiting for lock"
		finish
		exit 1
	fi
fi

# The mismatch check sits AFTER the lock, not before it. In the first version
# of this patch it was before, so between reading the stamp and taking the lock
# another worker could land a merge under a different version and this one
# would never see it -- a race in the check whose entire job is to stop two
# versions sharing a database.
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

ismask=false
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
		db=$XRAY_DATABASE_DIR/$XRAY_DATABASE/$1.db
		ismask=true
		cp "$2" "$tmp1" ;;

	*)
		echo "Invalid mode: $1"
		rm -f "$tmp1" "$tmp2"
		exit 1
esac

# --- prjcombine cross-validation merge gate ----------------------------------
#
# An INDEPENDENT oracle, in the merge path. prjcombine's 7-series bit database
# was reverse-engineered with ISE 14.7 + xdl/bitgen, not Vivado, so a solver
# that invents a coordinate is caught here by something that never saw the tool
# that produced it. Three checks:
#
#   [C] every bit coordinate in the incoming file is one prjcombine documents
#       for that tile class -- needs no feature-name table, so it covers every
#       mapped tile type immediately, and it is the one with teeth;
#   [R] every coordinate is inside the tile's own frames x words*32 box;
#   [F] for features with a hand-written prjcombine counterpart, the bit sets
#       are set-identical.
#
# A tile type outside prjcombine's coverage table prints `gate: SKIP` and exits
# 0 -- a NAMED, enumerable gap, not a silent pass. Anything the parser cannot
# account for is exit 2, never a warning. mask_* types fall into SKIP, so mask
# merges are unaffected.
#
# Position matters twice. It is after the tile-type-normalising `case`, so
# "$tmp1" already carries the final TILE_TYPE prefix that prjcombine is indexed
# by; and it is before `touch "$db"`, so under the `set -e` at the top of this
# script a rejection aborts BEFORE the database is touched at all. It also runs
# inside the flock taken above, which is released only on script exit, so no
# other worker can land a merge past it while it is thinking.
#
# --db-root is where the [R] check reads tilegrid.json from. It prefers the
# pinned full database and falls back to the workspace's own, which several
# fuzzers redirect to $FUZDIR/build/database for a staging merge; if neither
# carries a tilegrid the range check reports itself as NOT RUN rather than
# passing silently.
#
# Two ways off: XRAY_CROSSVALIDATE=0, and XC7DPR_ROOT unset. The second is what
# makes this safe to carry -- an ordinary prjxray checkout without this repo
# beside it behaves exactly as before.
#
# Measured cost: 0.55 s per merge (artix7 segbits_int_l.db, 1,598 coordinates,
# warm), against ~229 merge sites in a full five-family campaign.
if [ "${XRAY_CROSSVALIDATE:-1}" = 1 ] && [ -n "${XC7DPR_ROOT:-}" ] ; then
	python3 "$XC7DPR_ROOT/scripts/prjcombine-crossvalidate.py" \
		--gate "$1" "$tmp1" \
		--db-root "${PRJXRAY_DB_DIR:-$XRAY_DATABASE_DIR}" \
		--family "$XRAY_DATABASE"
fi
# --- end prjcombine cross-validation merge gate -------------------------------

touch "$db"
if $ismask ; then
    sort -u "$tmp1" "$db" | grep -v '<.*>' > "$tmp2" || true
else
    # tmp1 must be placed second to take precedence over old bad entries
    if ! $ismask ; then
	db_origin=$XRAY_DATABASE_DIR/$XRAY_DATABASE/segbits_$1.origin_info.db
        if [ ! -f $db_origin ] ; then
            touch "$db_origin"
        fi
        python3 ${XRAY_DIR}/utils/mergedb.py --out "$db_origin" "$db_origin" "$tmp1" --track_origin
    fi
    python3 ${XRAY_DIR}/utils/mergedb.py --out "$tmp2" "$db" "$tmp1"
fi
# Check aggregate db for consistency and make canonical
${XRAY_PARSEDB} --strict "$tmp2" "$db"

# Record the version that produced this merge. Written AFTER parsedb has
# accepted the result, and still under the lock taken above (flock is released
# on script exit), so the stamp never claims a merge that did not land.
if [ -n "${XRAY_VIVADO_VERSION_VERIFIED:-}" ]; then
	echo "$XRAY_VIVADO_VERSION_VERIFIED" > "$_stamp"
fi
