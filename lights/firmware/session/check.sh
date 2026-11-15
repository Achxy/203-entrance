#!/bin/sh
set -eu
cd "$(dirname "$0")"
out=$(mktemp -d "${TMPDIR:-/tmp}/lights-session.XXXXXX")
trap 'rm -rf "$out"' EXIT HUP INT TERM
"${CC:-cc}" -std=c11 -Wall -Wextra -Wconversion -Werror -g \
  -fsanitize=address,undefined lighting_session_wire.c test_wire.c -o "$out/check"
"$out/check"
