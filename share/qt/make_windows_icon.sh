#!/bin/bash
# create multiresolution windows icon
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ICO_PACKER="${ROOT_DIR}/contrib/devtools/png_to_ico.py"

make_ico() {
  local icon_src="$1"
  local icon_dst="$2"

  # Windows requests a variety of icon sizes depending on DPI scaling.
  # Include 20x20 and 40x40 (125% scaling), plus the usual sizes.
  # Also order from largest to smallest to avoid contexts that pick the first entry.
  local -a SIZES=(256 128 64 48 40 32 24 20 16)

  if command -v convert >/dev/null 2>&1; then
    local tmpdir
    tmpdir="$(mktemp -d)"
    for size in "${SIZES[@]}"; do
      convert "${icon_src}" -resize "${size}x${size}" "${tmpdir}/icon-${size}.png"
    done
    # Pass in the desired order explicitly (some consumers pick the first entry).
    convert $(for size in "${SIZES[@]}"; do printf "%q " "${tmpdir}/icon-${size}.png"; done) "${icon_dst}"
    rm -rf "${tmpdir}"
    return
  fi

  if ! command -v sips >/dev/null 2>&1; then
    echo "Error: neither 'convert' (ImageMagick) nor 'sips' is available." >&2
    return 1
  fi

  if ! command -v python3 >/dev/null 2>&1; then
    echo "Error: python3 is required to pack PNGs into a .ico file." >&2
    return 1
  fi

  local tmpdir
  tmpdir="$(mktemp -d)"
  for size in "${SIZES[@]}"; do
    sips -z "${size}" "${size}" "${icon_src}" --out "${tmpdir}/icon-${size}.png" >/dev/null
  done
  # Pass in the desired order explicitly (some consumers pick the first entry).
  python3 "${ICO_PACKER}" "${icon_dst}" $(for size in "${SIZES[@]}"; do printf "%q " "${tmpdir}/icon-${size}.png"; done)
  rm -rf "${tmpdir}"
}

# mainnet
make_ico "${ROOT_DIR}/src/qt/res/icons/bitcoin.png" "${ROOT_DIR}/src/qt/res/icons/bitcoin.ico"

# testnet
make_ico "${ROOT_DIR}/src/qt/res/icons/bitcoin_testnet.png" "${ROOT_DIR}/src/qt/res/icons/bitcoin_testnet.ico"

# regtest
make_ico "${ROOT_DIR}/src/qt/res/icons/bitcoin_regtest.png" "${ROOT_DIR}/src/qt/res/icons/bitcoin_regtest.ico"
