#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SOURCE_SCRIPTS_DIR="$SCRIPT_DIR"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
TEST_ROOT="$(mktemp -d)"
trap 'rm -rf "$TEST_ROOT"' EXIT

cp "$REPO_ROOT/configure.ac" "$TEST_ROOT/configure.ac"
touch "$TEST_ROOT/organiclife-1.1.13-win64-setup.exe"

# Load build_all helpers without invoking its main build entry point.
mkdir -p "$TEST_ROOT/scripts"
cp "$SCRIPT_DIR/version.sh" "$TEST_ROOT/scripts/version.sh"
awk '/^main\(\)/{exit} {print}' "$SCRIPT_DIR/build_all.sh" > "$TEST_ROOT/build_all_helpers.sh"
source "$TEST_ROOT/build_all_helpers.sh"
DIST_DIR="$TEST_ROOT/dist"
mkdir -p "$DIST_DIR"

if (REPO_ROOT="$TEST_ROOT" copy_windows_installer); then
    echo "stale installer was accepted" >&2
    exit 1
fi

touch "$TEST_ROOT/OrganicLifeCoin-1.1.0-win64-setup.exe"
REPO_ROOT="$TEST_ROOT" copy_windows_installer
test -f "$DIST_DIR/OrganicLifeCoin-1.1.0-win64-setup.exe"

package_script="$SOURCE_SCRIPTS_DIR/build-depends-windows.sh"
grep -Fq 'VERSION="$(get_client_package_version "$REPO_ROOT/configure.ac")"' "$package_script"
if grep -Fq 'git describe --tags --dirty' "$package_script"; then
    echo "Windows package naming still uses git describe" >&2
    exit 1
fi
