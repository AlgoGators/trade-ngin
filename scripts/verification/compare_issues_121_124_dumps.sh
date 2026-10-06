#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 OLD_DUMP NEW_DUMP" >&2
  exit 64
fi

old_dump=$1
new_dump=$2

sha256sum "$old_dump" "$new_dump"
if ! cmp -s "$old_dump" "$new_dump"; then
  diff -u "$old_dump" "$new_dump"
  exit 1
fi

echo "zero differing cells"
