#!/usr/bin/env bash
# Pin and build the Rust reference. Nothing here is benchmarked; the output is
# the oracle the C++ implementation is compared against.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TAG="0.13.0"
COMMIT="2b4cbaa0eecf7b507a612632fe00289b1b358c15"
DEST="$ROOT/upstream/xsv"
REPO="https://github.com/BurntSushi/xsv.git"

mkdir -p "$ROOT/upstream"

if [ ! -d "$DEST/.git" ]; then
  git clone --quiet "$REPO" "$DEST"
fi

git -C "$DEST" fetch --quiet --tags origin
git -C "$DEST" checkout --quiet --detach "$COMMIT"
ACTUAL="$(git -C "$DEST" rev-parse HEAD)"
if [ "$ACTUAL" != "$COMMIT" ]; then
  echo "error: expected $COMMIT, checked out $ACTUAL" >&2
  exit 1
fi

# The 2018 lockfile may need resolution on a modern cargo. Record both the
# shipped lockfile hash and the resolved one so the resolved set is auditable.
SHIPPED_LOCK_SHA="$(sha256sum "$DEST/Cargo.lock" | cut -d' ' -f1)"
(
  cd "$DEST"
  CARGO_NET_RETRY=10 cargo build --release
)
cp "$DEST/Cargo.lock" "$ROOT/upstream/Cargo.lock.resolved"

BIN="$DEST/target/release/xsv"
if [ ! -x "$BIN" ]; then
  echo "error: reference binary was not produced" >&2
  exit 1
fi

VERSION="$("$BIN" --version)"
if [ "$VERSION" != "$TAG" ]; then
  echo "error: reference version is '$VERSION', expected '$TAG'" >&2
  exit 1
fi

BIN_SHA="$(sha256sum "$BIN" | cut -d' ' -f1)"
RESOLVED_LOCK_SHA="$(sha256sum "$ROOT/upstream/Cargo.lock.resolved" | cut -d' ' -f1)"
RUSTC_VERSION="$(rustc --version)"
BUILT_AT="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

cat > "$ROOT/upstream/PIN.txt" <<EOF
tag=$TAG
commit=$COMMIT
cargo_lock_sha256=$SHIPPED_LOCK_SHA
resolved_lock_sha256=$RESOLVED_LOCK_SHA
rustc=$RUSTC_VERSION
binary=$BIN
binary_sha256=$BIN_SHA
built_at=$BUILT_AT
EOF

echo "reference: $BIN"
echo "version:   $VERSION"
echo "sha256:    $BIN_SHA"
echo "pin:       $ROOT/upstream/PIN.txt"
