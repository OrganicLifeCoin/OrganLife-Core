#!/usr/bin/env bash
#
# Build macOS release artifacts:
# - Universal ZIP (GUI app + all binaries)
# - Universal DMG (GUI app)
# - Universal "daemon-only" ZIP (organiclifed, organiclife-cli, organiclife-tx)
#
# This script uses the depends system (two builds: arm64 + x86_64) and then
# merges the resulting binaries via lipo.
#
# Usage:
#   scripts/build_mac.sh
#
# Environment overrides:
#   VERSION_OVERRIDE=...     (default: `v<CLIENT_VERSION_MAJOR>.<CLIENT_VERSION_MINOR>.<CLIENT_VERSION_REVISION>.<CLIENT_VERSION_BUILD>`)
#   BUNDLE_SHORT_VERSION_OVERRIDE=... (default: `<CLIENT_VERSION_MAJOR>.<CLIENT_VERSION_MINOR>.<CLIENT_VERSION_REVISION>`)
#   BUNDLE_VERSION_OVERRIDE=... (default: `<CLIENT_VERSION_MAJOR>.<CLIENT_VERSION_MINOR>.<CLIENT_VERSION_REVISION>.<CLIENT_VERSION_BUILD>`)
#   JOBS=...                 (default: `sysctl -n hw.ncpu`, capped at 8)
#   OUT_DIR=...              (default: repo root)
#   CONFIGURE_FLAGS=...      (extra flags passed to ./configure)
#   SIGN_IDENTITY=...        (optional; if set and --sign is used)
#   NOTARY_PROFILE=...       (optional; keychain profile name for notarytool)
#   NOTARY_APPLE_ID=...      (optional; alternative to NOTARY_PROFILE)
#   NOTARY_TEAM_ID=...       (optional; alternative to NOTARY_PROFILE)
#   NOTARY_PASSWORD=...      (optional; app-specific password; alternative to NOTARY_PROFILE)
#   NOTARY_KEY=...           (optional; App Store Connect API key .p8 path; alternative to NOTARY_PROFILE)
#   NOTARY_KEY_ID=...        (optional; App Store Connect API key id; alternative to NOTARY_PROFILE)
#   NOTARY_ISSUER=...        (optional; App Store Connect API issuer id; alternative to NOTARY_PROFILE)
#
# Options:
#   --clean-depends          remove depends outputs for both arches
#   --clean-build            remove build dirs for both arches
#   --skip-depends           do not run `make -C depends ...`
#   --skip-build             do not run `configure && make` (package only)
#   --sign                   codesign all artifacts + notarize + staple (default)
#   --sign-only              codesign only (skip notarization)
#   --notarize               notarize + staple (requires --sign or --sign-only)
#   --notary-profile NAME    keychain profile name for notarytool (or set NOTARY_PROFILE)
#
set -euo pipefail
IFS=$'\n\t'

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR" && git rev-parse --show-toplevel 2>/dev/null || true)"
if [[ -z "$REPO_ROOT" ]]; then
  echo "ERROR: must run inside a git checkout" >&2
  exit 1
fi

# shellcheck source=scripts/version.sh
. "$REPO_ROOT/scripts/version.sh"

DEPENDS_DIR="$REPO_ROOT/depends"
OUT_DIR="${OUT_DIR:-$REPO_ROOT}"

VERSION_FULL="$(get_client_version_full "$REPO_ROOT/configure.ac")"
VERSION="${VERSION_OVERRIDE:-v${VERSION_FULL}}"
BUNDLE_SHORT_VERSION="${BUNDLE_SHORT_VERSION_OVERRIDE:-$(get_client_marketing_version "$REPO_ROOT/configure.ac")}"
BUNDLE_VERSION="${BUNDLE_VERSION_OVERRIDE:-${VERSION_FULL}}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 8)}"
if [[ "$JOBS" -gt 8 ]]; then
  JOBS=8
fi

CONFIGURE_FLAGS="${CONFIGURE_FLAGS:-}"

CLEAN_DEPENDS=false
CLEAN_BUILD=false
SKIP_DEPENDS=false
SKIP_BUILD=false
DO_SIGN=false
SIGN_ONLY=false
DO_NOTARIZE=false
NOTARY_PROFILE="${NOTARY_PROFILE:-}"
NOTARY_APPLE_ID="${NOTARY_APPLE_ID:-}"
NOTARY_TEAM_ID="${NOTARY_TEAM_ID:-}"
NOTARY_PASSWORD="${NOTARY_PASSWORD:-}"
NOTARY_KEY="${NOTARY_KEY:-}"
NOTARY_KEY_ID="${NOTARY_KEY_ID:-}"
NOTARY_ISSUER="${NOTARY_ISSUER:-}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --clean-depends) CLEAN_DEPENDS=true; shift ;;
    --clean-build)   CLEAN_BUILD=true; shift ;;
    --skip-depends)  SKIP_DEPENDS=true; shift ;;
    --skip-build)    SKIP_BUILD=true; shift ;;
    --sign)          DO_SIGN=true; shift ;;
    --sign-only)     DO_SIGN=true; SIGN_ONLY=true; shift ;;
    --notarize)      DO_NOTARIZE=true; shift ;;
    --notary-profile) NOTARY_PROFILE="$2"; shift 2 ;;
    -h|--help)
      awk '/^#( |$)/{sub(/^# ?/,"");print}' "$0" | sed -n '1,120p'
      exit 0
      ;;
    *)
      echo "ERROR: unknown option: $1" >&2
      exit 2
      ;;
  esac
done

need() { command -v "$1" >/dev/null 2>&1 || { echo "ERROR: missing tool: $1" >&2; exit 1; }; }

pick_default_codesign_identity() {
  # Prefer a Developer ID Application identity.
  # Output: full identity string (as accepted by codesign -s).
  #
  # Example line:
  #   1) ABCDEF... "Developer ID Application: Example, Inc. (TEAMID)"
  if ! command -v security >/dev/null 2>&1; then
    return 1
  fi
  security find-identity -v -p codesigning 2>/dev/null | \
    awk -F'"' '/Developer ID Application:/{print $2; exit}'
}

ensure_notary_profile() {
  if [[ -n "$NOTARY_PROFILE" ]]; then
    return 0
  fi
  if ! command -v xcrun >/dev/null 2>&1; then
    return 1
  fi

  # Best-effort on older notarytool builds that supported list-profiles.
  # Output format varies slightly across Xcode versions; handle common patterns.
  local first_profile
  first_profile="$(
    xcrun notarytool list-profiles 2>/dev/null | \
      awk '
        NF {
          line = $0
          sub(/^[[:space:]]+/, "", line)
          if (line ~ /^(NAME|Profile|Profiles:|[-]+)$/) next
          n = split(line, f, /[[:space:]]+/)
          name = f[1]
          sub(/^\*/, "", name)
          if (name != "") { print name; exit }
        }
      '
  )"
  if [[ -n "${first_profile:-}" ]]; then
    NOTARY_PROFILE="$first_profile"
    return 0
  fi

  # Newer notarytool builds (like Xcode 16 CLT) removed list-profiles.
  # Probe a set of likely profile names by attempting a harmless history call.
  local candidate=""
  if [[ -n "${NOTARY_PROFILE_CANDIDATES:-}" ]]; then
    for candidate in ${NOTARY_PROFILE_CANDIDATES//,/ }; do
      [[ -z "$candidate" ]] && continue
      if xcrun notarytool history --keychain-profile "$candidate" >/dev/null 2>&1; then
        NOTARY_PROFILE="$candidate"
        return 0
      fi
    done
  fi

  for candidate in AC_NOTARY AC_PASSWORD NOTARY notary; do
    if xcrun notarytool history --keychain-profile "$candidate" >/dev/null 2>&1; then
      NOTARY_PROFILE="$candidate"
      return 0
    fi
  done

  return 1
}

notary_submit() {
  # Usage: notary_submit <file>
  local file="$1"
  need xcrun
  if ! xcrun notarytool --help >/dev/null 2>&1; then
    echo "ERROR: xcrun notarytool not available (Xcode 13+ required)." >&2
    exit 1
  fi

  if ensure_notary_profile; then
    xcrun notarytool submit "$file" --keychain-profile "$NOTARY_PROFILE" --wait
    return 0
  fi

  if [[ -n "$NOTARY_KEY" && -n "$NOTARY_KEY_ID" && -n "$NOTARY_ISSUER" ]]; then
    xcrun notarytool submit "$file" --key "$NOTARY_KEY" --key-id "$NOTARY_KEY_ID" --issuer "$NOTARY_ISSUER" --wait
    return 0
  fi

  if [[ -n "$NOTARY_APPLE_ID" && -n "$NOTARY_TEAM_ID" && -n "$NOTARY_PASSWORD" ]]; then
    xcrun notarytool submit "$file" --apple-id "$NOTARY_APPLE_ID" --team-id "$NOTARY_TEAM_ID" --password "$NOTARY_PASSWORD" --wait
    return 0
  fi

  echo "ERROR: notarization requires NOTARY_PROFILE (or credentials via NOTARY_APPLE_ID/NOTARY_TEAM_ID/NOTARY_PASSWORD or NOTARY_KEY/NOTARY_KEY_ID/NOTARY_ISSUER)." >&2
  echo "" >&2
  echo "Recommended (keychain profile):" >&2
  echo "  xcrun notarytool store-credentials 'AC_NOTARY' --apple-id 'you@icloud.com' --team-id 'TEAMID' --password 'APP_SPECIFIC_PASSWORD'" >&2
  echo "  NOTARY_PROFILE='AC_NOTARY' SIGN_IDENTITY='Developer ID Application: ... (TEAMID)' ./scripts/build_mac.sh --sign" >&2
  echo "" >&2
  echo "Alternative (env vars, no keychain profile):" >&2
  echo "  NOTARY_APPLE_ID='you@icloud.com' NOTARY_TEAM_ID='TEAMID' NOTARY_PASSWORD='APP_SPECIFIC_PASSWORD' ./scripts/build_mac.sh --sign" >&2
  echo "" >&2
  echo "Alternative (App Store Connect API key):" >&2
  echo "  NOTARY_KEY='/path/AuthKey_XXXX.p8' NOTARY_KEY_ID='KEYID' NOTARY_ISSUER='ISSUERID' ./scripts/build_mac.sh --sign" >&2
  exit 2
}

if [[ "$(uname -s)" != Darwin* ]]; then
  echo "ERROR: this script is for macOS" >&2
  exit 1
fi
if [[ "$(uname -m)" != arm64 ]]; then
  echo "ERROR: universal build is supported from Apple Silicon only (uname -m must be arm64)" >&2
  exit 1
fi

need xcode-select
if ! xcode-select -p >/dev/null 2>&1; then
  echo "ERROR: Xcode Command Line Tools not found. Run: xcode-select --install" >&2
  exit 1
fi

need make
need lipo
need file
need hdiutil
need ditto
need zip
need python3
need sed
need otool

if [[ ! -x "$DEPENDS_DIR/config.sub" ]]; then
  echo "ERROR: depends scripts not found. Expected $DEPENDS_DIR/config.sub" >&2
  exit 1
fi

if [[ "$DO_SIGN" == true ]] && [[ "$SIGN_ONLY" != true ]]; then
  # `--sign` is intended to produce fully-distributable artifacts by default.
  DO_NOTARIZE=true
fi

HOST_ARM64_RAW="${HOST_ARM64:-aarch64-apple-darwin}"
HOST_X86_64_RAW="${HOST_X86_64:-x86_64-apple-darwin}"
HOST_ARM64="$("$DEPENDS_DIR/config.sub" "$HOST_ARM64_RAW")"
HOST_X86_64="$("$DEPENDS_DIR/config.sub" "$HOST_X86_64_RAW")"

CONFIG_SITE_ARM64="$DEPENDS_DIR/$HOST_ARM64/share/config.site"
CONFIG_SITE_X86_64="$DEPENDS_DIR/$HOST_X86_64/share/config.site"

BUILD_DIR_ARM64="${BUILD_DIR_ARM64:-$REPO_ROOT/build-arm64}"
BUILD_DIR_X86_64="${BUILD_DIR_X86_64:-$REPO_ROOT/build-x86_64}"
BUILD_STATE_FILE=".build-mac-inputs.sha256"

BASE_UNIVERSAL="OrganicLife-macos-universal-qt"
BASE_DAEMON="OrganicLife-macos-universal-daemon"
OUT_ZIP_UNIVERSAL="$OUT_DIR/${BASE_UNIVERSAL}.zip"
OUT_ZIP_DAEMON="$OUT_DIR/${BASE_DAEMON}.zip"
OUT_DMG="$OUT_DIR/${BASE_UNIVERSAL}.dmg"

echo "========================================"
echo "  OrganicLife macOS Universal Build"
echo "========================================"
echo "Version:  $VERSION"
echo "Jobs:     $JOBS"
echo "Host arm: $HOST_ARM64"
echo "Host x64: $HOST_X86_64"
echo "Out dir:  $OUT_DIR"
echo ""

if [[ "$CLEAN_DEPENDS" == true ]]; then
  echo "[CLEAN] Removing depends outputs for $HOST_ARM64 and $HOST_X86_64"
  rm -rf "$DEPENDS_DIR/$HOST_ARM64" "$DEPENDS_DIR/$HOST_X86_64"
fi
if [[ "$CLEAN_BUILD" == true ]]; then
  echo "[CLEAN] Removing build dirs"
  rm -rf "$BUILD_DIR_ARM64" "$BUILD_DIR_X86_64"
fi

build_depends() {
  local host="$1"
  echo "[DEPENDS] Building $host ..."
  make -C "$DEPENDS_DIR" HOST="$host" JOBS="$JOBS" -j"$JOBS"
}

compute_tracked_build_fingerprint() {
  local host="$1"
  local cfg_site="$2"

  (
    cd "$REPO_ROOT"
    python3 - "$host" "$cfg_site" "$CONFIGURE_FLAGS" <<'PY'
import hashlib
import os
import subprocess
import sys

host, cfg_site, configure_flags = sys.argv[1:]

tracked_paths = [
    "src",
    "depends",
    "contrib",
    "share",
    "test",
    "autogen.sh",
    "configure.ac",
    "Makefile.am",
    "scripts/build_mac.sh",
]

repo_root = os.getcwd()
git = subprocess.run(
    ["git", "ls-files", "-z", "--"] + tracked_paths,
    cwd=repo_root,
    check=True,
    stdout=subprocess.PIPE,
)

fingerprint = hashlib.sha256()
fingerprint.update(f"host\0{host}\0".encode())
fingerprint.update(f"configure_flags\0{configure_flags}\0".encode())

cfg_site_abs = cfg_site if os.path.isabs(cfg_site) else os.path.join(repo_root, cfg_site)
if os.path.exists(cfg_site_abs):
    fingerprint.update(f"config_site\0{cfg_site}\0".encode())
    with open(cfg_site_abs, "rb") as handle:
        while True:
            chunk = handle.read(1024 * 1024)
            if not chunk:
                break
            fingerprint.update(chunk)
else:
    fingerprint.update(f"missing_config_site\0{cfg_site}\0".encode())

paths = [path for path in git.stdout.decode("utf-8", "surrogateescape").split("\0") if path]
for path in sorted(paths):
    abs_path = os.path.join(repo_root, path)
    fingerprint.update(f"path\0{path}\0".encode())
    if not os.path.exists(abs_path):
        fingerprint.update(b"missing\n")
        continue
    with open(abs_path, "rb") as handle:
        while True:
            chunk = handle.read(1024 * 1024)
            if not chunk:
                break
            fingerprint.update(chunk)

print(fingerprint.hexdigest())
PY
  )
}

distclean_tree() {
  if [[ -f "$REPO_ROOT/Makefile" || -f "$REPO_ROOT/config.status" || -f "$REPO_ROOT/src/Makefile" ]]; then
    (cd "$REPO_ROOT" && make distclean >/dev/null 2>&1 || true)
  fi
}

ensure_configure() {
  local reason=""
  if [[ ! -x "$REPO_ROOT/configure" ]]; then
    reason="missing configure"
  elif [[ "$REPO_ROOT/configure.ac" -nt "$REPO_ROOT/configure" ]]; then
    reason="configure.ac is newer than configure"
  elif [[ "$REPO_ROOT/Makefile.am" -nt "$REPO_ROOT/Makefile.in" ]]; then
    reason="Makefile.am is newer than Makefile.in"
  else
    local m4_dir=""
    local newer_macro=""
    for m4_dir in "$REPO_ROOT/build-aux/m4" "$REPO_ROOT/m4"; do
      [[ -d "$m4_dir" ]] || continue
      newer_macro="$(find "$m4_dir" -type f -newer "$REPO_ROOT/configure" -print -quit)"
      if [[ -n "$newer_macro" ]]; then
        reason="$(basename "$newer_macro") is newer than configure"
        break
      fi
    done
  fi

  if [[ -n "$reason" ]]; then
    echo "[AUTOGEN] Regenerating configure ($reason)..."
    (cd "$REPO_ROOT" && ./autogen.sh)
  fi
}

build_one() {
  local host="$1"
  local cfg_site="$2"
  local build_dir="$3"
  local expected_arch=""
  local qt_bindir_flag=""
  local build_state_file="$build_dir/$BUILD_STATE_FILE"
  local current_fingerprint=""
  local stored_fingerprint=""
  local rebuild_reason=""

  case "$host" in
    x86_64-apple-darwin) expected_arch="x86_64" ;;
    aarch64-apple-darwin) expected_arch="arm64" ;;
  esac

  # For x86_64 cross-builds on Apple Silicon, the target Qt prefix does not
  # contain host tools (moc/uic/rcc). Force using native_qt host tools so
  # configure can locate MOC without requiring Rosetta.
  if [[ "$host" == "x86_64-apple-darwin" ]]; then
    # Put target Qt wrappers first (so qmake queries target install paths), then
    # add native_qt host tools (moc/uic/rcc) from libexec.
    qt_bindir_flag="--with-qt-bindir=$DEPENDS_DIR/$host/bin:$DEPENDS_DIR/$host/native/bin:$DEPENDS_DIR/$host/native/libexec"
  fi

  if [[ ! -f "$cfg_site" ]]; then
    echo "ERROR: missing config.site for $host: $cfg_site" >&2
    exit 1
  fi

  current_fingerprint="$(compute_tracked_build_fingerprint "$host" "$cfg_site")"

  # Reuse an existing successful build only when tracked build inputs are unchanged.
  if [[ "$CLEAN_BUILD" != true ]] && \
     [[ -x "$build_dir/src/organiclifed" ]] && \
     [[ -x "$build_dir/src/organiclife-cli" ]] && \
     [[ -x "$build_dir/src/organiclife-tx" ]] && \
     [[ -x "$build_dir/src/qt/organiclife-qt" ]]; then
    if [[ -f "$build_state_file" ]]; then
      stored_fingerprint="$(tr -d '\n' < "$build_state_file")"
      if [[ "$stored_fingerprint" == "$current_fingerprint" ]]; then
        if [[ -n "$expected_arch" ]]; then
          check_arch "$expected_arch" "$build_dir/src/qt/organiclife-qt"
          check_arch "$expected_arch" "$build_dir/src/organiclifed"
        fi
        echo "[BUILD] $host -> $(basename "$build_dir") (reuse: tracked inputs unchanged)"
        return 0
      fi
      rebuild_reason="tracked inputs changed"
    else
      rebuild_reason="missing build fingerprint"
    fi
  elif [[ "$CLEAN_BUILD" == true ]]; then
    rebuild_reason="clean build requested"
  else
    rebuild_reason="missing prior build outputs"
  fi

  echo "[BUILD] $host -> $(basename "$build_dir") (rebuild: $rebuild_reason)"
  rm -rf "$build_dir"
  mkdir -p "$build_dir"
  (
    cd "$build_dir"
    CONFIG_SITE="$cfg_site" \
      "$REPO_ROOT/configure" \
      --with-gui=qt6 \
      --disable-tests \
      --disable-bench \
      --disable-gui-tests \
      --disable-maintainer-mode \
      --enable-mining-rpc \
      $qt_bindir_flag \
      $CONFIGURE_FLAGS
    make -j"$JOBS"
  )

  test -x "$build_dir/src/organiclifed"
  test -x "$build_dir/src/organiclife-cli"
  test -x "$build_dir/src/organiclife-tx"
  test -x "$build_dir/src/qt/organiclife-qt"
  printf '%s\n' "$current_fingerprint" > "$build_state_file"
}

check_arch() {
  local expected="$1"
  local bin="$2"
  if ! file "$bin" | grep -Eq "Mach-O.*${expected}"; then
    echo "ERROR: expected $expected binary: $bin" >&2
    file "$bin" || true
    exit 1
  fi
}

check_static_like() {
  local bin="$1"
  local deps
  deps="$(otool -L "$bin" 2>/dev/null | tail -n +2 | awk '{print $1}' || true)"
  if [[ -z "$deps" ]]; then
    echo "ERROR: failed to read linked libraries for: $bin" >&2
    exit 1
  fi
  if echo "$deps" | grep -Eq 'Qt[56]|QtCore|QtGui|QtWidgets|QtNetwork|QtSvg|QtConcurrent'; then
    echo "ERROR: $bin appears to link Qt dynamically (expected static Qt from depends)." >&2
    echo "       Linked libraries:" >&2
    otool -L "$bin" >&2 || true
    exit 1
  fi
  if echo "$deps" | grep -Eq '^/opt/homebrew/|^/usr/local/'; then
    echo "ERROR: $bin links against Homebrew/macports-style paths (expected depends/static)." >&2
    echo "       Linked libraries:" >&2
    otool -L "$bin" >&2 || true
    exit 1
  fi
  if echo "$deps" | grep -E '\\.dylib$' | grep -Ev '^(\\/usr\\/lib\\/|\\/System\\/Library\\/)' | grep -q .; then
    echo "ERROR: $bin links a non-system dylib (expected fully self-contained static deps)." >&2
    echo "       Linked libraries:" >&2
    otool -L "$bin" >&2 || true
    exit 1
  fi
}

if [[ "$SKIP_DEPENDS" == false ]]; then
  build_depends "$HOST_ARM64"
  build_depends "$HOST_X86_64"
fi

if [[ "$SKIP_BUILD" == false ]]; then
  distclean_tree
  ensure_configure
  build_one "$HOST_ARM64" "$CONFIG_SITE_ARM64" "$BUILD_DIR_ARM64"
  build_one "$HOST_X86_64" "$CONFIG_SITE_X86_64" "$BUILD_DIR_X86_64"
fi

for req in \
  "$BUILD_DIR_ARM64/src/organiclifed" \
  "$BUILD_DIR_X86_64/src/organiclifed" \
  "$BUILD_DIR_ARM64/src/qt/organiclife-qt" \
  "$BUILD_DIR_X86_64/src/qt/organiclife-qt"; do
  if [[ ! -x "$req" ]]; then
    echo "ERROR: missing built binary: $req" >&2
    echo "       If you want this script to build, omit --skip-build." >&2
    exit 1
  fi
done

echo "[PACK] Creating universal binaries..."
rm -rf "$OUT_DIR/$BASE_UNIVERSAL" "$OUT_DIR/$BASE_DAEMON"
rm -f "$OUT_ZIP_UNIVERSAL" "$OUT_ZIP_DAEMON" "$OUT_DMG"

mkdir -p "$OUT_DIR/$BASE_UNIVERSAL/bin"
mkdir -p "$OUT_DIR/$BASE_DAEMON/bin"
mkdir -p "$OUT_DIR/$BASE_UNIVERSAL/params"
mkdir -p "$OUT_DIR/$BASE_DAEMON/params"

PARAMS_SRC_DIR="$REPO_ROOT/params"
declare -a SAPLING_PARAMS=("sapling-output.params" "sapling-spend.params")
for p in "${SAPLING_PARAMS[@]}"; do
  if [[ ! -f "$PARAMS_SRC_DIR/$p" ]]; then
    echo "ERROR: missing params file: $PARAMS_SRC_DIR/$p" >&2
    echo "       (Needed for distribution; run ./params/install-params.sh if required.)" >&2
    exit 1
  fi
  cp -f "$PARAMS_SRC_DIR/$p" "$OUT_DIR/$BASE_UNIVERSAL/params/"
  cp -f "$PARAMS_SRC_DIR/$p" "$OUT_DIR/$BASE_DAEMON/params/"
done

declare -a BINS_ALL=("organiclifed" "organiclife-cli" "organiclife-tx")
for bin in "${BINS_ALL[@]}"; do
  lipo -create \
    "$BUILD_DIR_ARM64/src/$bin" \
    "$BUILD_DIR_X86_64/src/$bin" \
    -output "$OUT_DIR/$BASE_UNIVERSAL/bin/$bin"
  cp -f "$OUT_DIR/$BASE_UNIVERSAL/bin/$bin" "$OUT_DIR/$BASE_DAEMON/bin/$bin"
done

lipo -create \
  "$BUILD_DIR_ARM64/src/qt/organiclife-qt" \
  "$BUILD_DIR_X86_64/src/qt/organiclife-qt" \
  -output "$OUT_DIR/$BASE_UNIVERSAL/bin/organiclife-qt"

check_arch arm64 "$OUT_DIR/$BASE_UNIVERSAL/bin/organiclife-qt"
check_arch x86_64 "$OUT_DIR/$BASE_UNIVERSAL/bin/organiclife-qt"
check_static_like "$OUT_DIR/$BASE_UNIVERSAL/bin/organiclife-qt"

for bin in "${BINS_ALL[@]}"; do
  check_static_like "$OUT_DIR/$BASE_UNIVERSAL/bin/$bin"
done

echo "[PACK] Creating app bundle..."
APP_DISPLAY_NAME="OrganicLife"
APP_BUNDLE="$OUT_DIR/$BASE_UNIVERSAL/${APP_DISPLAY_NAME}.app"
mkdir -p "$APP_BUNDLE/Contents/MacOS" "$APP_BUNDLE/Contents/Resources"
cp -f "$OUT_DIR/$BASE_UNIVERSAL/bin/organiclife-qt" "$APP_BUNDLE/Contents/MacOS/${APP_DISPLAY_NAME}"
for p in "${SAPLING_PARAMS[@]}"; do
  cp -f "$PARAMS_SRC_DIR/$p" "$APP_BUNDLE/Contents/Resources/$p"
done

ICON_FILE=""

# Prefer branding icon source (png) and convert to .icns for Finder.
ICON_PNG=""
if [[ -f "$REPO_ROOT/src/qt/res/images/1776logo.png" ]]; then
  ICON_PNG="$REPO_ROOT/src/qt/res/images/1776logo.png"
elif [[ -f "$REPO_ROOT/src/qt/res/images/logo1776.png" ]]; then
  ICON_PNG="$REPO_ROOT/src/qt/res/images/logo1776.png"
elif [[ -f "$REPO_ROOT/1776logo.png" ]]; then
  ICON_PNG="$REPO_ROOT/1776logo.png"
elif [[ -f "$REPO_ROOT/logo1776.png" ]]; then
  ICON_PNG="$REPO_ROOT/logo1776.png"
fi

if [[ -n "$ICON_PNG" ]] && command -v iconutil >/dev/null 2>&1 && command -v sips >/dev/null 2>&1; then
  ICONSET_DIR="$(mktemp -d)"
  mkdir -p "$ICONSET_DIR/OrganicLife.iconset"
  for size in 16 32 128 256 512; do
    sips -z "$size" "$size" "$ICON_PNG" --out "$ICONSET_DIR/OrganicLife.iconset/icon_${size}x${size}.png" >/dev/null
    sips -z "$((size * 2))" "$((size * 2))" "$ICON_PNG" --out "$ICONSET_DIR/OrganicLife.iconset/icon_${size}x${size}@2x.png" >/dev/null
  done
  # 1024x1024 (optional but recommended)
  sips -z 1024 1024 "$ICON_PNG" --out "$ICONSET_DIR/OrganicLife.iconset/icon_512x512@2x.png" >/dev/null || true

  if iconutil -c icns "$ICONSET_DIR/OrganicLife.iconset" -o "$APP_BUNDLE/Contents/Resources/CTEAM.icns" >/dev/null 2>&1; then
    ICON_FILE="CTEAM.icns"
  fi
  rm -rf "$ICONSET_DIR" 2>/dev/null || true
fi

# Reuse the checked-in app icon if generation from a source PNG is unavailable.
if [[ -z "$ICON_FILE" ]] && [[ -f "$REPO_ROOT/src/qt/res/icons/bitcoin.icns" ]]; then
  cp -f "$REPO_ROOT/src/qt/res/icons/bitcoin.icns" "$APP_BUNDLE/Contents/Resources/CTEAM.icns"
  ICON_FILE="CTEAM.icns"
fi

ICON_PLIST_BLOCK=""
if [[ -n "$ICON_FILE" ]]; then
  ICON_PLIST_BLOCK="  <key>CFBundleIconFile</key>
  <string>${ICON_FILE}</string>"
fi

cat > "$APP_BUNDLE/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleDevelopmentRegion</key>
  <string>English</string>
  <key>CFBundleExecutable</key>
  <string>${APP_DISPLAY_NAME}</string>
${ICON_PLIST_BLOCK}
  <key>CFBundleIdentifier</key>
  <string>org.organiclife.organiclife-qt</string>
  <key>CFBundleInfoDictionaryVersion</key>
  <string>6.0</string>
  <key>CFBundleDisplayName</key>
  <string>${APP_DISPLAY_NAME}</string>
  <key>CFBundleName</key>
  <string>${APP_DISPLAY_NAME}</string>
  <key>CFBundlePackageType</key>
  <string>APPL</string>
  <key>CFBundleShortVersionString</key>
  <string>${BUNDLE_SHORT_VERSION}</string>
  <key>CFBundleVersion</key>
  <string>${BUNDLE_VERSION}</string>
  <key>LSMinimumSystemVersion</key>
  <string>10.15</string>
  <key>NSHighResolutionCapable</key>
  <true/>
  <key>LSArchitecturePriority</key>
  <array>
    <string>arm64</string>
    <string>x86_64</string>
  </array>
</dict>
</plist>
EOF

if [[ "$DO_SIGN" == true ]]; then
  need codesign
  if [[ -z "${SIGN_IDENTITY:-}" ]]; then
    SIGN_IDENTITY="$(pick_default_codesign_identity || true)"
  fi
  if [[ -z "${SIGN_IDENTITY:-}" ]]; then
    echo "ERROR: signing requested but SIGN_IDENTITY not set and no 'Developer ID Application' identity found." >&2
    echo "" >&2
    echo "Available identities:" >&2
    security find-identity -v -p codesigning >&2 || true
    echo "" >&2
    echo "Fix:" >&2
    echo "  export SIGN_IDENTITY='Developer ID Application: ... (TEAMID)'" >&2
    exit 2
  fi

  echo "[SIGN] Signing binaries + app + dmg with: $SIGN_IDENTITY"
  # Hardened runtime + timestamp is required for notarization.
  for bin in "${BINS_ALL[@]}" "organiclife-qt"; do
    codesign --force --options runtime --timestamp --sign "$SIGN_IDENTITY" "$OUT_DIR/$BASE_UNIVERSAL/bin/$bin"
  done
  for bin in "${BINS_ALL[@]}"; do
    codesign --force --options runtime --timestamp --sign "$SIGN_IDENTITY" "$OUT_DIR/$BASE_DAEMON/bin/$bin"
  done
  codesign --force --options runtime --timestamp --sign "$SIGN_IDENTITY" "$APP_BUNDLE/Contents/MacOS/${APP_DISPLAY_NAME}"
  codesign --force --options runtime --timestamp --sign "$SIGN_IDENTITY" "$APP_BUNDLE"
  codesign --verify --deep --strict --verbose=2 "$APP_BUNDLE" >/dev/null
  if command -v spctl >/dev/null 2>&1; then
    spctl --assess --type execute --verbose=4 "$APP_BUNDLE" >/dev/null 2>&1 || true
  fi
fi

echo "[PACK] Creating daemon ZIP..."
(cd "$OUT_DIR" && zip -qr "$OUT_ZIP_DAEMON" "$BASE_DAEMON")

echo "[PACK] Creating DMG..."
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR" 2>/dev/null || true' EXIT
RW_DMG="$TMP_DIR/pack.rw.dmg"
MOUNT_DIR="$TMP_DIR/mount"
mkdir -p "$MOUNT_DIR"

hdiutil create -size 1g -fs HFS+ -volname "OrganicLife" -type UDIF "$RW_DMG" >/dev/null
DEV="$(hdiutil attach "$RW_DMG" -mountpoint "$MOUNT_DIR" -nobrowse -noverify -noautoopen | awk 'NR==1{print $1}')"
ditto "$APP_BUNDLE" "$MOUNT_DIR/$(basename "$APP_BUNDLE")"
ln -s /Applications "$MOUNT_DIR/Applications" 2>/dev/null || true
hdiutil detach "$DEV" >/dev/null
hdiutil convert "$RW_DMG" -format UDZO -o "$OUT_DMG" >/dev/null

if [[ "$DO_SIGN" == true ]]; then
  echo "[SIGN] Signing dmg with: $SIGN_IDENTITY"
  codesign --force --timestamp --sign "$SIGN_IDENTITY" "$OUT_DMG"
fi

if [[ "$DO_NOTARIZE" == true ]]; then
  if [[ "$DO_SIGN" != true ]]; then
    echo "ERROR: notarization requested but signing is disabled. Use --sign or --sign-only." >&2
    exit 2
  fi

  echo "[NOTARY] Submitting DMG for notarization"
  notary_submit "$OUT_DMG"

  echo "[NOTARY] Stapling ticket to app + dmg"
  xcrun stapler staple -v "$APP_BUNDLE"
  xcrun stapler staple -v "$OUT_DMG"
  xcrun stapler validate -v "$APP_BUNDLE" || true
  xcrun stapler validate -v "$OUT_DMG" || true
fi

echo "[PACK] Creating ZIPs..."
(cd "$OUT_DIR" && zip -qr "$OUT_ZIP_UNIVERSAL" "$BASE_UNIVERSAL")

if [[ "$DO_NOTARIZE" == true ]]; then
  # The daemon zip has no stapling mechanism; but notarizing it can still help Gatekeeper.
  echo "[NOTARY] Submitting daemon ZIP for notarization"
  notary_submit "$OUT_ZIP_DAEMON"
fi

echo ""
echo "Outputs:"
ls -lh "$OUT_ZIP_UNIVERSAL" "$OUT_ZIP_DAEMON" "$OUT_DMG" | sed 's/^/  /'
echo ""
echo "Universal binary:"
lipo -info "$OUT_DIR/$BASE_UNIVERSAL/bin/organiclife-qt" | sed 's/^/  /'
