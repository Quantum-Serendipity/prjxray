#!/usr/bin/env bash
set -u

X2B=$1
WANT=$2
got=$("$X2B" --version 2>&1 | head -1)
case $got in
	"xc7frames2bit version xc7dpr $WANT")
		echo "ok   version banner: $got"
		exit 0
		;;
esac
echo "FAIL version banner: got '$got', want 'xc7frames2bit version xc7dpr $WANT'"
exit 1
