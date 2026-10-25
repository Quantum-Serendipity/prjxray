#!/usr/bin/env bash
set -u

X2B=$1
PART=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
failures=0

DATA=$(dirname "$PART")

frm() {
	local out=$1 a
	shift
	frm_fill "$out" 0x00000001 "$@"
}

frm_fill() {
	local out=$1 fill=$2 a w
	shift 2
	: > "$out"
	for a in "$@"; do
		printf '0x%08x ' "$a" >> "$out"
		for w in $(seq 1 100); do printf '%s,' "$fill" >> "$out"; done
		printf '%s\n' "$fill" >> "$out"
	done
}

run() {
	local name=$1 want=$2 rc
	shift 2
	rm -f "$tmp/out.bit"
	"$X2B" --part_file "$PART" --output_file "$tmp/out.bit" "$@" \
		> "$tmp/$name.log" 2>&1
	rc=$?
	if [[ $want == accept && $rc -ne 0 ]] ||
	   [[ $want == refuse && ( $rc -eq 0 || $rc -gt 1 ) ]]; then
		echo "FAIL $name: wanted $want, rc $rc"
		cat "$tmp/$name.log"
		failures=$((failures + 1))
	else
		echo "ok   $name: $want, rc $rc"
	fi
}

frm "$tmp/rowend.frm" 0x15a7 0x15a8 0x15a9
partial=(--partial_bitstream --prepass off --frm_file "$tmp/rowend.frm")

run rowend-pad2 accept "${partial[@]}" --pad_frames 2
run rowend-pad1 accept "${partial[@]}" --pad_frames 1
run rowend-pad0 refuse "${partial[@]}" --pad_frames 0
run rowend-default-pad accept "${partial[@]}" --raw
cp "$tmp/out.bit" "$tmp/rowend-default.bit" 2>/dev/null
run rowend-pad2-raw accept "${partial[@]}" --raw --pad_frames 2
if cmp -s "$tmp/rowend-default.bit" "$tmp/out.bit"; then
	echo "ok   rowend-default-pad: same bytes as --pad_frames 2"
else
	echo "FAIL rowend-default-pad: differs from --pad_frames 2"
	failures=$((failures + 1))
fi
run rowend-pad3-noverify refuse "${partial[@]}" --pad_frames 3 --noverify
run rowend-pad4-noverify refuse "${partial[@]}" --pad_frames 4 --noverify

frm "$tmp/midrow.frm" 0x15a0 0x15a1
frm "$tmp/midrow-static.frm" 0x15a0 0x15a1 0x15a2
midrow=(--partial_bitstream --raw --prepass off --frm_file "$tmp/midrow.frm"
	--static_frames "$tmp/midrow-static.frm")
for p in 0 2 7; do
	run midrow-pad$p accept "${midrow[@]}" --pad_frames $p
	cp "$tmp/out.bit" "$tmp/midrow-pad$p.bit" 2>/dev/null
done
for p in 2 7; do
	if cmp -s "$tmp/midrow-pad0.bit" "$tmp/midrow-pad$p.bit"; then
		echo "ok   midrow-pad$p: same bytes as pad 0"
	else
		echo "FAIL midrow-pad$p: differs from pad 0 on a block with no row end"
		failures=$((failures + 1))
	fi
done

midrow_bit=(--partial_bitstream --prepass off --frm_file "$tmp/midrow.frm"
	--static_frames "$tmp/midrow-static.frm" --pad_frames 0)
run midrow-bit accept "${midrow_bit[@]}"
cp "$tmp/out.bit" "$tmp/midrow.bit" 2>/dev/null
run midrow-raw accept "${midrow_bit[@]}" --raw
cp "$tmp/out.bit" "$tmp/midrow.raw" 2>/dev/null
idcode=$(sed -n 's/^idcode: *0x\([0-9a-fA-F]*\)$/\1/p' "$PART")
idcode_hex=$(printf '30018001%08x' "0x$idcode")
if [[ -s $tmp/midrow.raw && -s $tmp/midrow.bit ]] &&
   tail -c "$(stat -c %s "$tmp/midrow.raw")" "$tmp/midrow.bit" |
	cmp -s - "$tmp/midrow.raw"; then
	echo "ok   midrow-raw: a byte-exact suffix of the .bit"
else
	echo "FAIL midrow-raw: not a byte-exact suffix of the .bit"
	failures=$((failures + 1))
fi
for f in midrow.bit midrow.raw; do
	if od -An -tx1 -v "$tmp/$f" 2>/dev/null | tr -d ' \n' |
		grep -qF "$idcode_hex"; then
		echo "ok   $f: writes IDCODE 0x$idcode"
	else
		echo "FAIL $f: no IDCODE write of 0x$idcode"
		failures=$((failures + 1))
	fi
done

full=(--frm_file "$tmp/rowend.frm")
run full accept "${full[@]}"
run full-verify accept "${full[@]}" --verify
run full-raw refuse "${full[@]}" --raw
run full-expected-frames refuse "${full[@]}" --expected_frames 3
run full-static-frames refuse "${full[@]}" --static_frames "$tmp/rowend.frm"
run full-prepass refuse "${full[@]}" --prepass off
run full-pad-frames refuse "${full[@]}" --pad_frames 2
run full-pad-frames-default-value refuse "${full[@]}" --pad_frames 0
run full-trailing-nops refuse "${full[@]}" --trailing_nops 16
run full-crc refuse "${full[@]}" --crc compute
run full-db-root refuse "${full[@]}" --db_root "$tmp"
run full-tilegrid refuse "${full[@]}" --tilegrid "$tmp/rowend.frm"

"$X2B" --helpfull > "$tmp/help.txt" 2>&1
for phrase in "are written, and nothing else" "flushed by nothing"; do
	if tr -s ' \n' ' ' < "$tmp/help.txt" | grep -qF "$phrase"; then
		echo "FAIL help: still says '$phrase'"
		failures=$((failures + 1))
	else
		echo "ok   help: does not say '$phrase'"
	fi
done
if tr -s ' \n' ' ' < "$tmp/help.txt" | grep -qF "plus one FLUSH FRAME"; then
	echo "ok   help: names the flush frame"
else
	echo "FAIL help: --partial_bitstream does not name the flush frame"
	failures=$((failures + 1))
fi

logged() {
	local name=$1 phrase=$2
	if tr -s ' \n' ' ' < "$tmp/$name.log" | grep -qF "$phrase"; then
		echo "ok   $name: says '$phrase'"
	else
		echo "FAIL $name: does not say '$phrase'"
		cat "$tmp/$name.log"
		failures=$((failures + 1))
	fi
}

run full-devfull-noverify refuse "${full[@]}" --noverify \
	--output_file /dev/full
run midrow-raw-devfull-noverify refuse "${midrow[@]}" --noverify \
	--output_file /dev/full

frm_fill "$tmp/rm.frm" 0xffffffff 0x0 0x1 0x2
frm_fill "$tmp/static.frm" 0x00000010 0x0 0x1 0x2 0x3
frm_fill "$tmp/static-short.frm" 0x00000010 0x0 0x2 0x3
prepass=(--partial_bitstream --frm_file "$tmp/rm.frm"
	--tilegrid "$DATA/prepass_tilegrid.json" --db_root "$DATA/prepass_db")
run prepass-on-nostatic refuse "${prepass[@]}" --prepass on
logged prepass-on-nostatic "needs --static_frames"
run prepass-on accept "${prepass[@]}" --prepass on \
	--static_frames "$tmp/static.frm"
logged prepass-on "independent replay OK, 3 frames committed 2 time(s) each"
run prepass-on-static-missing-frame refuse "${prepass[@]}" --prepass on \
	--static_frames "$tmp/static-short.frm"
logged prepass-on-static-missing-frame "needs the static's own frame"

sed 's/"baseaddr": "0x00000200"/"baseaddr": "0x00060000"/' \
	"$DATA/prepass_tilegrid.json" > "$tmp/foreign_tilegrid.json"
if cmp -s "$DATA/prepass_tilegrid.json" "$tmp/foreign_tilegrid.json"; then
	echo "FAIL foreign-tilegrid: the fixture edit changed nothing"
	failures=$((failures + 1))
fi
run prepass-foreign-tilegrid refuse --partial_bitstream \
	--frm_file "$tmp/rm.frm" --prepass on --static_frames "$tmp/static.frm" \
	--tilegrid "$tmp/foreign_tilegrid.json" --db_root "$DATA/prepass_db"
logged prepass-foreign-tilegrid "describe different devices"

fam=$tmp/db/fam
mkdir -p "$fam/mapping" "$fam/parta" "$fam/fabrica" "$fam/fabricb"
cp "$DATA"/prepass_db/*.db "$fam/"
cp "$PART" "$fam/parta/part.yaml"
cp "$DATA/prepass_tilegrid.json" "$fam/fabrica/tilegrid.json"
cp "$DATA/prepass_tilegrid.json" "$fam/fabricb/tilegrid.json"
printf 'parta:\n  device: deva\npartb:\n  device: devb\n' \
	> "$fam/mapping/parts.yaml"
printf 'deva:\n  fabric: fabrica\ndevb:\n  fabric: fabricb\n' \
	> "$fam/mapping/devices.yaml"
byname=(--partial_bitstream --frm_file "$tmp/rm.frm" --prepass on
	--static_frames "$tmp/static.frm" --part_file "$fam/parta/part.yaml")
run prepass-part-name-matches accept "${byname[@]}" --part_name parta
logged prepass-part-name-matches "fabrica/tilegrid.json"
run prepass-part-name-other-fabric refuse "${byname[@]}" --part_name partb
logged prepass-part-name-other-fabric "resolves to fabric fabricb"
mkdir -p "$fam/unlisted"
cp "$PART" "$fam/unlisted/part.yaml"
run prepass-part-file-unlisted accept "${byname[@]}" --part_name parta \
	--part_file "$fam/unlisted/part.yaml"
logged prepass-part-file-unlisted "was not cross-checked against --part_file"
logged prepass-part-file-unlisted "does not resolve in"
if grep -qF "not cross-checked" "$tmp/prepass-part-name-matches.log"; then
	echo "FAIL prepass-part-name-matches: says it did not cross-check"
	failures=$((failures + 1))
else
	echo "ok   prepass-part-name-matches: no cross-check note"
fi
logged prepass-on "checked only for columns outside --part_file's frame space"

run full-uncompressed accept "${full[@]}"
cp "$tmp/out.bit" "$tmp/uncompressed.bit"
run full-compress accept "${full[@]}" --compress
logged full-compress "independent replay OK"
if [[ -f $tmp/out.bit && -f $tmp/uncompressed.bit ]] &&
   [[ $(stat -c %s "$tmp/out.bit") -lt $(stat -c %s "$tmp/uncompressed.bit") ]]; then
	echo "ok   full-compress: shorter than the uncompressed stream"
else
	echo "FAIL full-compress: not shorter than the uncompressed stream"
	failures=$((failures + 1))
fi
run compress-partial refuse "${partial[@]}" --compress
run compress-ultrascale refuse "${full[@]}" --compress \
	--architecture UltraScale
{
	printf '0x%08x ' 0x15a7
	for w in $(seq 2 123); do printf '0x00000001,'; done
	printf '0x00000001\n'
} > "$tmp/us.frm"
run ultrascale-verify accept --architecture UltraScale --frm_file "$tmp/us.frm"
logged ultrascale-verify "the independent replay models Series7 only"

[[ $failures -eq 0 ]]
