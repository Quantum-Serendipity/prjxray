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

hex_offset() {
	od -An -v -tx1 "$1" | tr -d ' \n' | awk -v p="$2" '{
		s = $0; base = 0
		while ((i = index(s, p)) > 0) {
			if ((base + i - 1) % 2 == 0) { print (base + i - 1) / 2; break }
			s = substr(s, i + 1); base += i
		}
	}'
}

payload() {
	local off
	off=$(hex_offset "$1" aa995566)
	[[ -n $off ]] && tail -c +$((off + 1)) "$1"
}

same_payload() {
	local a b
	a=$(payload "$1" | od -An -v -tx1 | tr -d ' \n')
	b=$(payload "$2" | od -An -v -tx1 | tr -d ' \n')
	[[ -n $a && $a == "$b" ]]
}

one_frame() {
	printf '0x%08x ' 0x15a7
	for w in $(seq 2 "$1"); do printf '0x00000001,'; done
	printf '0x00000001\n'
}
one_frame 101 > "$tmp/one.frm"
one_frame 123 > "$tmp/one-us.frm"

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
	same_payload "$tmp/$variant.bit" "$tmp/$variant-patched.bit"
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
off=$(hex_offset "$tmp/cor0.bit" 30012001)
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

"$X2B" --architecture UltraScale --part_file "$PART" --frm_file "$tmp/one-us.frm" \
	--output_file "$tmp/us.bit" > "$tmp/us-x2b.log" 2>&1
check "ultrascale: base written" $?
"$PATCH" --architecture UltraScale --part_file "$PART" \
	--bitstream_file "$tmp/us.bit" --output_file "$tmp/us-patched.bit" \
	> "$tmp/us-patch.log" 2>&1
check "ultrascale: xc7patch rc 0" $?
same_payload "$tmp/us.bit" "$tmp/us-patched.bit"
check "ultrascale: patching with no delta reproduces the payload" $?

cp "$tmp/us.bit" "$tmp/us-cor1.bit"
off=$(hex_offset "$tmp/us-cor1.bit" 3001c001)
[[ -n $off ]]
check "ultrascale cor1: the base writes COR1" $?
if [[ -n $off ]]; then
	printf '\x00\x00\x00\x00' |
		dd of="$tmp/us-cor1.bit" bs=1 seek=$((off + 4)) conv=notrunc 2>/dev/null
fi
cmp -s "$tmp/us.bit" "$tmp/us-cor1.bit"
[[ $? -eq 1 ]]
check "ultrascale cor1: the rewrite changed the base" $?
"$PATCH" --architecture UltraScale --part_file "$PART" \
	--bitstream_file "$tmp/us-cor1.bit" --output_file "$tmp/us-cor1-patched.bit" \
	> "$tmp/us-cor1.log" 2>&1
rc=$?
[[ $rc -eq 1 ]] && grep -qF "would change COR1" "$tmp/us-cor1.log"
check "ultrascale rewritten COR1: refused, rc $rc" $?

[[ $failures -eq 0 ]]
