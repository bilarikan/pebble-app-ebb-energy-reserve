#!/bin/sh
# Replays a parsed watch log through the engine, optionally with model.h constants changed.
#
#   tools/replay/run.sh DATADIR "DD HH:MM" RESERVE [M_NAME=value ...]
#
# The constants are changed in a throwaway copy of the sources, so the repository is not
# touched. See replay.c for what is printed and docs/DEVELOPMENT.md for the workflow.
set -e
here="$(cd "$(dirname "$0")" && pwd)"
src="$here/../../src/c"
[ $# -ge 3 ] || { echo "usage: $0 DATADIR \"DD HH:MM\" RESERVE [M_NAME=value ...]"; exit 1; }
data="$1"; start="$2"; reserve="$3"; shift 3

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
cp "$src/engine.c" "$src/engine.h" "$src/model.h" "$src/shared.h" "$tmp/"
for kv in "$@"; do
  k="${kv%%=*}"; v="${kv#*=}"
  grep -q "^#define $k " "$tmp/model.h" || { echo "no constant $k in model.h"; exit 1; }
  sed -E "s|^(#define $k +)[^ /]+|\1$v|" "$tmp/model.h" > "$tmp/model.h.new" && mv "$tmp/model.h.new" "$tmp/model.h"
done
cc -std=c11 -O1 -Wall -I"$tmp" -o "$tmp/replay" "$here/replay.c" "$tmp/engine.c"
"$tmp/replay" "$data" "$start" "$reserve"
