#!/usr/bin/env bash
# Build Release artifacts for PScript (Linux x64).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

CONFIG="${1:-Release}"
BUILD_DIR="${BUILD_DIR:-build}"

parse_version() {
  local base pre
  base="$(sed -n 's/^project(PScript VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt | head -1)"
  pre="$(sed -n 's/^set(PSCRIPT_VERSION_PRERELEASE "\([^"]*\)".*/\1/p' CMakeLists.txt | head -1)"
  if [[ -z "$pre" ]]; then echo "$base"; else echo "${base}-${pre}"; fi
}

VERSION="$(parse_version)"
PLATFORM="linux-x64"
DIST_NAME="pscript-${VERSION}-${PLATFORM}"
DIST_ROOT="$ROOT/dist/$DIST_NAME"

cmake -S "$ROOT" -B "$ROOT/$BUILD_DIR"
cmake --build "$ROOT/$BUILD_DIR" --config "$CONFIG" --target pscript pscript_lsp 2>/dev/null \
  || cmake --build "$ROOT/$BUILD_DIR" --target pscript pscript_lsp

PSCRIPT_BIN="$ROOT/$BUILD_DIR/pscript"
LSP_BIN="$ROOT/$BUILD_DIR/pscript_lsp"
[[ -x "$PSCRIPT_BIN" ]] || PSCRIPT_BIN="$ROOT/$BUILD_DIR/Release/pscript"
[[ -x "$LSP_BIN" ]] || LSP_BIN="$ROOT/$BUILD_DIR/Release/pscript_lsp"
[[ -x "$PSCRIPT_BIN" ]] || { echo "missing pscript binary"; exit 1; }

rm -rf "$DIST_ROOT"
mkdir -p "$DIST_ROOT/bin" "$DIST_ROOT/docs"
cp "$PSCRIPT_BIN" "$LSP_BIN" "$DIST_ROOT/bin/"
cp LICENSE README.md CHANGELOG.md "$DIST_ROOT/"
cp -r examples "$DIST_ROOT/"
for f in docs/COMPATIBILITY.md docs/INSTALLATION.md docs/GETTING_STARTED.md docs/TROUBLESHOOTING.md \
  docs/MIGRATION.md docs/package-ecosystem.md docs/LANGUAGE_SPEC.md docs/language/developer-cli.md \
  docs/language/stdlib.md docs/language/editor-integration.md \
  docs/release/RELEASE_NOTES-1.0.0.md; do
  [[ -f "$f" ]] || continue
  dest="$DIST_ROOT/docs/${f#docs/}"
  mkdir -p "$(dirname "$dest")"
  cp "$f" "$dest"
done

ver_out="$("$DIST_ROOT/bin/pscript" --version | tr -d '\r\n')"
[[ "$ver_out" == "$VERSION" ]] || { echo "version mismatch: $ver_out vs $VERSION"; exit 1; }

UTC="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
cat > "$DIST_ROOT/RELEASE_METADATA.json" <<EOF
{
  "name": "pscript",
  "version": "$VERSION",
  "platform": "$PLATFORM",
  "configuration": "$CONFIG",
  "jit_backend": "x64-native",
  "built_utc": "$UTC"
}
EOF

( cd "$DIST_ROOT" && find . -type f ! -name 'SHA256SUMS.txt' -print0 | sort -z | xargs -0 sha256sum ) > "$DIST_ROOT/SHA256SUMS.txt"

mkdir -p "$ROOT/dist"
( cd "$ROOT/dist" && zip -rq "${DIST_NAME}.zip" "$DIST_NAME" )
sha256sum "$ROOT/dist/${DIST_NAME}.zip" | tee "$ROOT/dist/${DIST_NAME}.zip.sha256"

echo "Release package: $DIST_ROOT"
