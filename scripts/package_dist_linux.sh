#!/usr/bin/env bash
set -euo pipefail

# Package a Linux x64 build for local distribution.
#
# Produces dist/duckdb-salesforce-<version>-linux-x64/ plus a .tar.gz archive.
# Run `make release` first.

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

EXT="build/release/extension/salesforce/salesforce.duckdb_extension"
if [ ! -f "$EXT" ]; then
    echo "ERROR: extension not found at $EXT" >&2
    echo "Run make release first." >&2
    exit 1
fi

# Version precedence: explicit arg ($1) > RELEASE_VERSION env > a v-prefixed
# GITHUB_REF_NAME (tag push) > community descriptor > "unknown". The explicit
# sources let a workflow_dispatch on main (GITHUB_REF_NAME=main) still package
# the intended release version instead of falling back to the descriptor.
VERSION="${1:-${RELEASE_VERSION:-}}"
VERSION="${VERSION#v}" # tolerate a v-prefixed explicit value
if [ -z "$VERSION" ] && [ -n "${GITHUB_REF_NAME:-}" ]; then
    case "$GITHUB_REF_NAME" in
        v*) VERSION="${GITHUB_REF_NAME#v}" ;;
    esac
fi
if [ -z "$VERSION" ] && [ -f docs/community/description.yml ]; then
    VERSION="$(awk -F: '/^[[:space:]]+version:/ {gsub(/^[ \t]+|[ \t]+$/, "", $2); print $2; exit}' docs/community/description.yml)"
fi
if [ -z "$VERSION" ]; then
    VERSION="unknown"
fi

STAGE="dist/duckdb-salesforce-$VERSION-linux-x64"
ARCHIVE="$STAGE.tar.gz"

rm -rf "$STAGE" "$ARCHIVE"
mkdir -p "$STAGE"

cp "$EXT" "$STAGE/salesforce.duckdb_extension"
sed \
    -e "s/@@VERSION@@/$VERSION/g" \
    -e "s/@@PLATFORM@@/Linux x64/g" \
    scripts/dist_README.template.txt > "$STAGE/README.txt"

tar -czf "$ARCHIVE" -C "$(dirname "$STAGE")" "$(basename "$STAGE")"

echo
echo "--- packaged ---"
echo "Stage dir: $STAGE"
echo "Archive:   $ARCHIVE"
ls -lh "$ARCHIVE"
echo
echo "SHA-256:"
sha256sum "$ARCHIVE" | awk '{print $1}'
