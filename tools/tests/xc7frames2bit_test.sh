#!/usr/bin/env bash
set -u

X2B=$1
PART=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
failures=0

frm() {
	local out=$1 a w
	shift
	: > "$out"
	for a in "$@"; do
		printf '0x%08x ' "$a" >> "$out"
		for w in $(seq 1 100); do printf '0x00000001,' >> "$out"; done
		printf '0x00000001\n' >> "$out"
	done
}

run() {
	local name=$1 want=$2 rc
	shift 2
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
run rowend-default-pad refuse "${partial[@]}"
run rowend-pad3-noverify refuse "${partial[@]}" --pad_frames 3 --noverify
run rowend-pad4-noverify refuse "${partial[@]}" --pad_frames 4 --noverify

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

[[ $failures -eq 0 ]]
