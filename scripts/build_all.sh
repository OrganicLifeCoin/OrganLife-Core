#!/usr/bin/env bash
# Build and package CTEAM artifacts for Linux amd64, Linux aarch64 (cross), and Windows.
# Produces two zips per platform: daemon+cli+tx+params and qt+params.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR" && git rev-parse --show-toplevel 2>/dev/null || true)"
if [ -z "$REPO_ROOT" ]; then
  if [ -f "$SCRIPT_DIR/../configure.ac" ]; then
    REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
  else
    REPO_ROOT="$SCRIPT_DIR"
  fi
fi

# shellcheck source=scripts/version.sh
. "$REPO_ROOT/scripts/version.sh"

DIST_DIR="$REPO_ROOT/dist"
PARAMS_DIR="$REPO_ROOT/params"

mkdir -p "$DIST_DIR"

DEFAULT_BUILD_JOBS="$( (nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4) | tr -d ' ' )"
BUILD_JOBS="${BUILD_JOBS:-$DEFAULT_BUILD_JOBS}"
AARCH64_DEBUG="${AARCH64_DEBUG:-false}"

log() { echo "[build_all] $*"; }

host_matches_target() {
  local target="$1"
  local host_arch
  host_arch="$(uname -m)"
  case "$target" in
    linux-amd64)
      [[ "$host_arch" == "x86_64" || "$host_arch" == "amd64" ]]
      ;;
    linux-aarch64)
      [[ "$host_arch" == "aarch64" || "$host_arch" == "arm64" ]]
      ;;
    *)
      return 1
      ;;
  esac
}

require_params() {
  if [ ! -d "$PARAMS_DIR" ]; then
    echo "params directory not found: $PARAMS_DIR" >&2
    exit 1
  fi
  local count
  count=$(ls "$PARAMS_DIR"/*.params 2>/dev/null | wc -l | tr -d ' ')
  if [ "$count" -eq 0 ]; then
    echo "no *.params found in $PARAMS_DIR" >&2
    exit 1
  fi
}

assert_file_arch() {
  local path="$1"
  local expect="$2"
  if [ ! -f "$path" ]; then
    echo "missing binary: $path" >&2
    exit 1
  fi
  local desc
  desc=$(file -b "$path" || true)
  if ! echo "$desc" | grep -Eq "$expect"; then
    echo "unexpected arch for $path" >&2
    echo "  got: $desc" >&2
    echo "  want regex: $expect" >&2
    exit 1
  fi
}

make_zip() {
  local zip_name="$1"
  shift
  local tmp
  tmp=$(mktemp -d)
  rm -f "$DIST_DIR/$zip_name"
  mkdir -p "$tmp/params"
  cp "$PARAMS_DIR"/*.params "$tmp/params/"
  for f in "$@"; do
    if [ ! -f "$f" ]; then
      echo "missing file for zip: $f" >&2
      rm -rf "$tmp"
      exit 1
    fi
    cp "$f" "$tmp/"
  done
  (cd "$tmp" && zip -r "$DIST_DIR/$zip_name" . >/dev/null)
  rm -rf "$tmp"
  log "wrote $DIST_DIR/$zip_name"
}

copy_windows_installer() {
  local version installer_expected installer_found
  version="$(get_client_package_version "$REPO_ROOT/configure.ac")"
  installer_expected="cteam-${version}-win64-setup.exe"

  if [ -f "$REPO_ROOT/$installer_expected" ]; then
    installer_found="$REPO_ROOT/$installer_expected"
  else
    installer_found=$(ls -t "$REPO_ROOT"/cteam-*-win64-setup.exe 2>/dev/null | head -n 1 || true)
  fi

  if [ -z "$installer_found" ]; then
    echo "missing windows installer (make deploy did not produce cteam-*-win64-setup.exe)" >&2
    exit 1
  fi

  cp "$installer_found" "$DIST_DIR/$installer_expected"
  log "wrote $DIST_DIR/$installer_expected"
}

build_linux_amd64() {
  log "building linux amd64..."
  if host_matches_target linux-amd64; then
    "$REPO_ROOT/scripts/build-depends.sh" --jobs "$BUILD_JOBS"
  else
    "$REPO_ROOT/scripts/build-depends.sh" --jobs "$BUILD_JOBS"
  fi

  local d="$REPO_ROOT/src/cteamd"
  local c="$REPO_ROOT/src/cteam-cli"
  local t="$REPO_ROOT/src/cteam-tx"
  local q="$REPO_ROOT/src/qt/cteam-qt"

  assert_file_arch "$d" "x86-64"
  assert_file_arch "$c" "x86-64"
  assert_file_arch "$t" "x86-64"
  assert_file_arch "$q" "x86-64"

  make_zip "CTEAM-linux-amd64-daemon.zip" "$d" "$c" "$t"
  make_zip "CTEAM-linux-amd64-qt.zip" "$q"
}

build_linux_aarch64() {
  if host_matches_target linux-aarch64; then
    log "building linux aarch64 (native)..."
    local native_args=()
    if [ "$AARCH64_DEBUG" = true ]; then
      native_args+=(--debug)
    fi
    "$REPO_ROOT/scripts/build-depends.sh" --jobs "$BUILD_JOBS" "${native_args[@]}"
  else
    log "building linux aarch64 (cross)..."
  local aarch64_args=()
  if command -v aarch64-linux-gnu-g++ >/dev/null 2>&1 && command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
    aarch64_args+=(--skip-system-deps)
  fi
  if [ "$AARCH64_DEBUG" = true ]; then
    aarch64_args+=(--debug)
  fi
  "$REPO_ROOT/scripts/build-depends-aarch64.sh" --jobs "$BUILD_JOBS" "${aarch64_args[@]}"
  fi

  local d="$REPO_ROOT/src/cteamd"
  local c="$REPO_ROOT/src/cteam-cli"
  local t="$REPO_ROOT/src/cteam-tx"
  local q="$REPO_ROOT/src/qt/cteam-qt"

  assert_file_arch "$d" "aarch64"
  assert_file_arch "$c" "aarch64"
  assert_file_arch "$t" "aarch64"
  assert_file_arch "$q" "aarch64"

  make_zip "CTEAM-linux-aarch64-daemon.zip" "$d" "$c" "$t"
  make_zip "CTEAM-linux-aarch64-qt.zip" "$q"
}

build_windows() {
  log "building windows x86_64..."
  local windows_args=()
  if command -v x86_64-w64-mingw32-g++ >/dev/null 2>&1; then
    windows_args+=(--skip-system-deps)
  fi
  "$REPO_ROOT/scripts/build-depends-windows.sh" --jobs "$BUILD_JOBS" "${windows_args[@]}"

  local d="$REPO_ROOT/src/cteamd.exe"
  local c="$REPO_ROOT/src/cteam-cli.exe"
  local t="$REPO_ROOT/src/cteam-tx.exe"
  local q="$REPO_ROOT/src/qt/cteam-qt.exe"

  assert_file_arch "$d" "PE32\+.*x86-64"
  assert_file_arch "$c" "PE32\+.*x86-64"
  assert_file_arch "$t" "PE32\+.*x86-64"
  assert_file_arch "$q" "PE32\+.*x86-64"

  make_zip "CTEAM-windows-x86_64-package.zip" "$d" "$c" "$t" "$q"
  make_zip "CTEAM-windows-x86_64-daemon.zip" "$d" "$c" "$t"
  make_zip "CTEAM-windows-x86_64-qt.zip" "$q"
  copy_windows_installer
}

main() {
  require_params

  local do_linux=true
  local do_aarch64=true
  local do_windows=true

  while [ $# -gt 0 ]; do
    case "$1" in
      --skip-linux) do_linux=false ;;
      --skip-aarch64) do_aarch64=false ;;
      --skip-windows) do_windows=false ;;
      --debug-aarch64|--aarch64-debug) AARCH64_DEBUG=true ;;
      --jobs)
        if [ $# -lt 2 ]; then
          echo "--jobs requires a value" >&2
          return 1
        fi
        BUILD_JOBS="$2"
        shift
        ;;
      --help|-h)
        cat <<'USAGE'
Usage: scripts/build_all.sh [--skip-linux] [--skip-aarch64] [--skip-windows] [--debug-aarch64] [--jobs N]
USAGE
        return 0
        ;;
      *)
        echo "unknown option: $1" >&2
        return 1
        ;;
    esac
    shift
  done

  if [ "$do_linux" = true ]; then
    build_linux_amd64
  fi
  if [ "$do_aarch64" = true ]; then
    build_linux_aarch64
  fi
  if [ "$do_windows" = true ]; then
    build_windows
  fi

  log "all artifacts are in $DIST_DIR"
}

main "$@"
