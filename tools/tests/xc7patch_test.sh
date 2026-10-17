#!/usr/bin/env bash
set -u

X2B=$1
PATCH=$2
PART=$3
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
failures=0

check() {
	if [[ $2 == 0 ]]; then
		echo "ok   $1"
	else
		echo "FAIL $1"
		failures=$((failures + 1))
	fi
}

payload() {
	local off
	off=$(LC_ALL=C grep -obUaP '\xaa\x99\x55\x66' "$1" | head -1 | cut -d: -f1)
	[[ -n $off ]] && tail -c +$((off + 1)) "$1"
}

{
	printf '0x%08x ' 0x15a7
	for w in $(seq 1 100); do printf '0x00000001,'; done
	printf '0x00000001\n'
} > "$tmp/one.frm"

for variant in plain crc rbcrc; do
	case $variant in
	plain) flags=() ;;
	crc) flags=(--emit_crc) ;;
	rbcrc) flags=(--emit_crc --rbcrc correct_and_continue) ;;
	esac
	"$X2B" --part_file "$PART" --frm_file "$tmp/one.frm" \
		--output_file "$tmp/$variant.bit" "${flags[@]}" \
		> "$tmp/$variant-x2b.log" 2>&1
	check "$variant: base written" $?
	"$PATCH" --part_file "$PART" --bitstream_file "$tmp/$variant.bit" \
		--output_file "$tmp/$variant-patched.bit" \
		> "$tmp/$variant-patch.log" 2>&1
	check "$variant: xc7patch rc 0" $?
	cmp -s <(payload "$tmp/$variant.bit") \
		<(payload "$tmp/$variant-patched.bit")
	check "$variant: patching with no delta reproduces the payload" $?
done

"$PATCH" --part_file "$PART" --bitstream_file "$tmp/plain.bit" \
	--output_file "$tmp/no/such/dir/out.bit" > "$tmp/nodir.log" 2>&1
rc=$?
[[ $rc -ne 0 ]]
check "unwritable output: rc $rc" $?

"$PATCH" --part_file "$PART" --bitstream_file "$tmp/plain.bit" \
	--output_file /dev/full > "$tmp/devfull.log" 2>&1
rc=$?
[[ $rc -eq 1 ]]
check "output to a full device: rc $rc" $?

"$X2B" --part_file "$PART" --frm_file "$tmp/one.frm" --compress \
	--output_file "$tmp/compressed.bit" > "$tmp/compressed-x2b.log" 2>&1
check "compressed: base written" $?
"$PATCH" --part_file "$PART" --bitstream_file "$tmp/compressed.bit" \
	--output_file "$tmp/compressed-patched.bit" > "$tmp/compressed.log" 2>&1
rc=$?
[[ $rc -eq 1 ]] && grep -qF "writes MFWR" "$tmp/compressed.log"
check "compressed base: refused, rc $rc" $?

cp "$tmp/plain.bit" "$tmp/cor0.bit"
off=$(LC_ALL=C grep -obUaP '\x30\x01\x20\x01' "$tmp/cor0.bit" | head -1 | cut -d: -f1)
[[ -n $off ]]
check "cor0: the base writes COR0" $?
if [[ -n $off ]]; then
	printf '\x02\x42\x3f\xe5' |
		dd of="$tmp/cor0.bit" bs=1 seek=$((off + 4)) conv=notrunc 2>/dev/null
fi
cmp -s "$tmp/plain.bit" "$tmp/cor0.bit"
[[ $? -eq 1 ]]
check "cor0: the rewrite changed the base" $?
"$PATCH" --part_file "$PART" --bitstream_file "$tmp/cor0.bit" \
	--output_file "$tmp/cor0-patched.bit" > "$tmp/cor0.log" 2>&1
rc=$?
[[ $rc -eq 1 ]] && grep -qF "would change COR0" "$tmp/cor0.log"
check "rewritten COR0: refused, rc $rc" $?

[[ $failures -eq 0 ]]
