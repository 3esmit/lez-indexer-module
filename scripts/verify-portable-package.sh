#!/usr/bin/env bash

set -euo pipefail

if (( $# != 2 )); then
  printf 'usage: %s <package.lgx> <expected-variant>\n' "$0" >&2
  exit 2
fi

archive="$1"
variant="$2"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
metadata="$repo_root/metadata.json"

test -f "$archive"
test -f "$metadata"

expected_name="$(jq -er '.name | strings | select(length > 0)' "$metadata")"
expected_version="$(jq -er '.version | strings | select(length > 0)' "$metadata")"
manifest="$(tar -xOf "$archive" manifest.json)"

actual_name="$(jq -er '.name | strings | select(length > 0)' <<<"$manifest")"
actual_version="$(jq -er '.version | strings | select(length > 0)' <<<"$manifest")"
test "$actual_name" = "$expected_name"
test "$actual_version" = "$expected_version"

main_path="$(jq -er --arg variant "$variant" '.main[$variant] | strings | select(length > 0)' <<<"$manifest")"
tar -tzf "$archive" | grep -Fx "variants/$variant/$main_path" >/dev/null

printf 'verified %s@%s portable %s package: ' "$actual_name" "$actual_version" "$variant"
sha256sum "$archive"
