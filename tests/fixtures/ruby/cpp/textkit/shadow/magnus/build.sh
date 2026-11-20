#!/bin/sh
# Build the magnus transport for textkit; needs cargo and Ruby headers (ruby-dev).
set -eu
cd "$(dirname "$0")"
cargo build --release
cp "target/release/libtextkit_shadow_ext.so" "../textkit_shadow_ext.so"
echo "built ../textkit_shadow_ext.so: the Ruby shadow now uses it instead of Fiddle"
