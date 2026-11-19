#!/bin/sh
# Host test for the BMS protocol code. Needs the cJSON sources that
# `idf.py build` downloads into managed_components/.
set -e
cd "$(dirname "$0")/.."
cjson=$(dirname "$(find managed_components -name cJSON.c -path '*espressif__cjson*' | head -1)")
[ -f "$cjson/cJSON.c" ] || { echo "cJSON not found: run 'idf.py build' first"; exit 1; }
out=${TMPDIR:-/tmp}/test_bms_proto
# -iquote: main/features/features.h must not shadow glibc <features.h>
cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined \
   -iquote main/features -I "$cjson" \
   test/test_bms_proto.c main/features/bms_proto.c "$cjson/cJSON.c" -lm -o "$out"
"$out" test/fixtures/*.json   # an unmatched glob is skipped by the test
