#!/usr/bin/env bash
# Regenerate app icons for all platforms from the canonical branding image.
#
# Source (tracked):  ./1776logo.png
# Outputs updated:
#   - Qt GUI resources: src/qt/res/icons/bitcoin*.png + .ico + .icns
#   - Linux/packaging:  share/pixmaps/pivx*.png + pivx.ico + bitcoin*.png + bitcoin.icns
#
# Requires either:
#   - ImageMagick `convert`, or
#   - macOS `sips` (and `iconutil` for .icns is NOT required; we use png_to_icns.py)
#
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SRC_ICON="${ROOT_DIR}/src/qt/res/images/1776logo.png"
if [[ ! -f "$SRC_ICON" ]]; then
  # Backwards-compatible fallback for a repo-root source icon.
  SRC_ICON="${ROOT_DIR}/1776logo.png"
fi

if [[ ! -f "$SRC_ICON" ]]; then
  echo "ERROR: missing source icon: $SRC_ICON" >&2
  exit 1
fi

need() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "ERROR: missing required tool: $1" >&2
    exit 1
  }
}

resize_png() {
  local src="$1"
  local dst="$2"
  local size="$3"

  mkdir -p "$(dirname "$dst")"

  if command -v convert >/dev/null 2>&1; then
    convert "$src" -resize "${size}x${size}" "$dst"
    return
  fi

  if command -v sips >/dev/null 2>&1; then
    sips -z "$size" "$size" "$src" --out "$dst" >/dev/null
    return
  fi

  echo "ERROR: neither 'convert' (ImageMagick) nor 'sips' is available." >&2
  exit 1
}

echo "[ICON] Updating Qt PNG resources..."
for f in bitcoin.png bitcoin_testnet.png bitcoin_regtest.png; do
  resize_png "$SRC_ICON" "$ROOT_DIR/src/qt/res/icons/$f" 1024
done

echo "[ICON] Updating share/pixmaps PNGs..."
# Keep existing filenames used by Linux desktop integration (Icon=pivx128).
resize_png "$SRC_ICON" "$ROOT_DIR/share/pixmaps/pivx1024.png" 1024
for size in 16 24 32 48 64 128 256 512; do
  resize_png "$SRC_ICON" "$ROOT_DIR/share/pixmaps/pivx${size}.png" "$size"
done

# Also keep the legacy bitcoin*.png in sync (some packaging/scripts reference these).
for f in bitcoin.png bitcoin_testnet.png bitcoin_regtest.png; do
  resize_png "$SRC_ICON" "$ROOT_DIR/share/pixmaps/$f" 1024
done

echo "[ICON] Regenerating Windows .ico resources..."
need python3
"$ROOT_DIR/share/qt/make_windows_icon.sh"

# Installer/UI icon used by NSIS and wx rc: share/pixmaps/pivx.ico
ICO_PACKER="$ROOT_DIR/contrib/devtools/png_to_ico.py"
TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR" 2>/dev/null || true' EXIT
SIZES_ICO=(256 128 64 48 40 32 24 20 16)
for size in "${SIZES_ICO[@]}"; do
  resize_png "$SRC_ICON" "$TMPDIR/icon-${size}.png" "$size"
done
python3 "$ICO_PACKER" "$ROOT_DIR/share/pixmaps/pivx.ico" $(for size in "${SIZES_ICO[@]}"; do printf "%q " "$TMPDIR/icon-${size}.png"; done)

echo "[ICON] Regenerating macOS .icns resources..."
ICNS_PACKER="$ROOT_DIR/contrib/devtools/png_to_icns.py"
SIZES_ICNS=(16 32 64 128 256 512 1024)
for size in "${SIZES_ICNS[@]}"; do
  resize_png "$SRC_ICON" "$TMPDIR/icon-${size}.png" "$size"
done
python3 "$ICNS_PACKER" "$ROOT_DIR/src/qt/res/icons/bitcoin.icns" $(for size in "${SIZES_ICNS[@]}"; do printf "%q " "$TMPDIR/icon-${size}.png"; done)
cp -f "$ROOT_DIR/src/qt/res/icons/bitcoin.icns" "$ROOT_DIR/share/pixmaps/bitcoin.icns"

echo "[ICON] Done."
