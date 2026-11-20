#!/bin/sh
# Builds the Node-API addon that loads libcppan_shadow.so. Run after `cpp-anything build`.
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
NODE="${NODE:-node}"
CXX="${CXX:-g++}"
BUILD=/mnt/data/Projects/OpenSource/cpp-anything/tests/fixtures/ts/cpp/textkit/build
OUT="$here/textkit_shadow.node"
NODE_PREFIX=$("$NODE" -p "require('path').dirname(process.execPath)")
INCLUDE="$NODE_PREFIX/../include/node"
if [ ! -f "$INCLUDE/node_api.h" ]; then
  INCLUDE="$NODE_PREFIX/include/node"
fi
if [ ! -f "$INCLUDE/node_api.h" ]; then
  echo "cpp-anything: node_api.h not found under $NODE_PREFIX; install node headers" >&2
  exit 1
fi

if [ "${CPPAN_USE_NODE_GYP:-0}" = "1" ] && command -v node-gyp >/dev/null 2>&1; then
  cd "$here"
  node-gyp rebuild --nodedir="$NODE_PREFIX/.." >/dev/null
  cp "$here/build/Release/textkit_shadow.node" "$OUT"
  echo "cpp-anything: built $OUT (node-gyp)"
  exit 0
fi

case "$(uname -s)" in
  Darwin)
    SHARED="-bundle -undefined dynamic_lookup"
    ;;
  *)
    SHARED="-shared"
    ;;
esac

"$CXX" -std=c++17 -O2 -fPIC $SHARED \
  -I"$INCLUDE" -DNODE_GYP_MODULE_NAME=textkit_shadow \
  "$here/addon.cc" -o "$OUT" \
  -L"$BUILD" -lcppan_shadow -Wl,-rpath,"$BUILD"
echo "cpp-anything: built $OUT"
