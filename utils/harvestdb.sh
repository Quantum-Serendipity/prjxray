#!/usr/bin/env bash
# Copyright (C) 2026  The Project X-Ray Authors.
#
# Use of this source code is governed by a ISC-style
# license that can be found in the LICENSE file or at
# https://opensource.org/licenses/ISC
#
# SPDX-License-Identifier: ISC

set -euo pipefail

usage() {
    echo "usage: $0 <family-db-dir> <tile> <incoming segbits .db> <source-label>" >&2
    exit 2
}

[ $# -eq 4 ] || usage
family_dir=$1
tile=$2
incoming=$3
label=$4

[ -d "$family_dir" ] || { echo "$0: no such directory: $family_dir" >&2; exit 2; }
[ -s "$incoming" ] || { echo "$0: empty or missing: $incoming" >&2; exit 2; }
case "$label" in
    ""|*[[:space:],]*) echo "$0: source label must be one token with no comma: '$label'" >&2; exit 2 ;;
esac

XRAY_DIR=${XRAY_DIR:-$(cd "$(dirname "$0")/.." && pwd)}
export PYTHONPATH="${XRAY_DIR}${PYTHONPATH:+:$PYTHONPATH}"

db="$family_dir/segbits_$tile.db"
db_origin="$family_dir/segbits_$tile.origin_info.db"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir "$work/old" "$work/new"
cp "$incoming" "$work/incoming.db"
if [ -f "$db" ]; then cp "$db" "$work/old/"; else : > "$work/old/segbits_$tile.db"; fi
if [ -f "$db_origin" ]; then cp "$db_origin" "$work/old/"; else : > "$work/old/segbits_$tile.origin_info.db"; fi
new_db="$work/new/segbits_$tile.db"
new_origin="$work/new/segbits_$tile.origin_info.db"

MERGEDB_STRICT=1 python3 "$XRAY_DIR/utils/mergedb.py" --db-root "$family_dir" \
    --out "$new_db" "$work/old/segbits_$tile.db" "$work/incoming.db"
MERGEDB_STRICT=1 python3 "$XRAY_DIR/utils/mergedb.py" --db-root "$family_dir" \
    --track_origin --origin "$label" \
    --out "$new_origin" "$work/old/segbits_$tile.origin_info.db" "$work/incoming.db"
python3 "$XRAY_DIR/utils/parsedb.py" --db-root "$family_dir" --strict "$new_db"
python3 "$XRAY_DIR/utils/parsedb.py" --db-root "$family_dir" --strict "$new_origin"
python3 "$XRAY_DIR/utils/sort_db.py" "$new_db" "$new_origin" > /dev/null

cp "$new_db" "$db"
cp "$new_origin" "$db_origin"
echo "$0: merged $(wc -l < "$incoming") line(s) from $incoming into $db as origin:$label"
