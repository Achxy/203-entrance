#!/bin/sh
set -eu
cd "$(dirname "$0")"
out=$(mktemp -d "${TMPDIR:-/tmp}/lights-runtime.XXXXXX")
trap 'rm -rf "$out"' EXIT HUP INT TERM
"${CC:-cc}" -std=c11 -Wall -Wextra -Wconversion -Werror -g \
  -fsanitize=address,undefined -I. lighting_runtime_queue.c tests/test_queue.c -o "$out/check"
"$out/check"
"${CC:-cc}" -std=c11 -Wall -Wextra -Wconversion -Werror -g \
  -fsanitize=address,undefined -I. lighting_runtime_recovery.c tests/test_recovery.c -o "$out/recovery"
"$out/recovery"
"${CC:-cc}" -std=c11 -Wall -Wextra -Wconversion -Werror -g \
  -fsanitize=address,undefined -I. lighting_runtime_recovery.c tests/test_boot.c -o "$out/boot"
"$out/boot"
"${CC:-cc}" -std=c11 -Wall -Wextra -Wconversion -Werror -g \
  -fsanitize=address,undefined -I. lighting_local_time.c tests/test_local_time.c -o "$out/local-time"
"$out/local-time"
for sockets in 20 16; do
  "${CC:-cc}" -std=c11 -D_DARWIN_C_SOURCE -D_DEFAULT_SOURCE -Wall -Wextra -Wconversion -Werror -g \
    -DCONFIG_LWIP_MAX_SOCKETS=$sockets -fsanitize=address,undefined \
    -Itests/local_time_idf_stubs -I. lighting_local_time.c lighting_local_time_idf.c \
    tests/test_local_time_idf.c -o "$out/local-time-idf-$sockets"
  "$out/local-time-idf-$sockets"
done
