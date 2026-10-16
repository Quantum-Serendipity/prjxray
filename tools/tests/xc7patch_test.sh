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

[[ $failures -eq 0 ]]
