#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="${1:-$(date +%Y%m%d)}"
ARCHIVE="$ROOT/brohome-control-hub-$VERSION.tar.gz"

cd "$ROOT"
python3 scripts/check_public_tree.py

if ! git diff --quiet || ! git diff --cached --quiet; then
  echo "Commit or stash changes before building a reproducible release." >&2
  exit 1
fi

git archive --format=tar.gz --prefix="brohome-control-hub-$VERSION/" -o "$ARCHIVE" HEAD
sha256sum "$ARCHIVE" >"$ARCHIVE.sha256"
echo "Created: $ARCHIVE"
echo "Checksum: $ARCHIVE.sha256"
