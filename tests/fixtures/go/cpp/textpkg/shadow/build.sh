#!/bin/sh
# cpp-anything shadow: cgo links the port at `go build` time.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
lib="$here/../build/libcppan_shadow.so"
if [ ! -f "$lib" ]; then
  echo "shadow: $lib is missing; run: cpp-anything build <project>" >&2
  exit 1
fi
echo "shadow: ready ($lib)"
