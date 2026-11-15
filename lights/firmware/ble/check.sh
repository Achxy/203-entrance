#!/bin/sh
set -eu
cd "$(dirname "$0")"
out=$(mktemp -d "${TMPDIR:-/tmp}/lights-ble.XXXXXX")
trap 'rm -rf "$out"' EXIT HUP INT TERM
"${CC:-cc}" -std=c11 -Wall -Wextra -Wconversion -Werror -g \
  -fsanitize=address,undefined -I. lighting_ble_frame.c tests/test_frame.c -o "$out/check"
"$out/check"
