#!/usr/bin/env bash
set -euo pipefail

in_svg="${1:?input svg required}"
out_png="${2:?output png required}"
dpi="${3:?dpi required}"
package_name="${4:-}"

tmp_svg=""
cleanup() {
  if [[ -n "${tmp_svg}" ]]; then
    rm -f "${tmp_svg}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

render_with_rsvg() {
  sed "s/PACKAGE_NAME/${package_name//\//\\/}/" < "${in_svg}" | rsvg-convert -f png -d "${dpi}" -p "${dpi}" -o "${out_png}"
}

render_with_inkscape() {
  tmp_svg="${out_png}.tmp.svg"
  sed "s/PACKAGE_NAME/${package_name//\//\\/}/" < "${in_svg}" > "${tmp_svg}"
  inkscape "${tmp_svg}" --export-type=png --export-dpi="${dpi}" --export-filename="${out_png}" >/dev/null 2>&1
}

render_fallback() {
  local base_w=600
  local base_h=400
  local scale=$(( dpi / 36 ))
  if [[ "${scale}" -lt 1 ]]; then
    scale=1
  fi
  local w=$(( base_w * scale ))
  local h=$(( base_h * scale ))
  echo "warning: rsvg-convert/inkscape not found; generating placeholder background (${w}x${h}). Install 'librsvg' (rsvg-convert) for SVG rendering." >&2
  python3 "$(dirname "$0")/make_background_png.py" "${out_png}" "${w}" "${h}"
}

if command -v rsvg-convert >/dev/null 2>&1; then
  render_with_rsvg
elif command -v inkscape >/dev/null 2>&1; then
  render_with_inkscape
else
  render_fallback
fi

