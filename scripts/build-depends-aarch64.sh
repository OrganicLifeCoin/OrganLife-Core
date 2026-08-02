#!/usr/bin/env bash
#
# OrganicLife Build Script (Linux aarch64 cross-compile from x86_64)
#
# Usage: ./build-depends-aarch64.sh [options]
# This is a wrapper around build-depends.sh that sets up the aarch64 toolchain.
#

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log_info() { echo -e "${BLUE}[INFO]${NC} $1"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR" && git rev-parse --show-toplevel 2>/dev/null || true)"
if [ -z "$REPO_ROOT" ]; then
    if [ -f "$SCRIPT_DIR/../configure.ac" ]; then
        REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
    else
        REPO_ROOT="$SCRIPT_DIR"
    fi
fi

HOST=aarch64-linux-gnu
SKIP_DEPS=false
FORWARDED_ARGS=()

while [[ $# -gt 0 ]]; do
    case $1 in
        --skip-system-deps)
            SKIP_DEPS=true
            shift
            ;;
        --help|-h)
            echo "Usage: $0 [OPTIONS] [forwarded build-depends.sh options]"
            echo ""
            echo "Options:"
            echo "  --skip-system-deps    Skip installing aarch64 cross toolchain"
            echo "  --help, -h            Show this help"
            echo ""
            echo "All other options are forwarded to ./build-depends.sh"
            exit 0
            ;;
        *)
            FORWARDED_ARGS+=("$1")
            shift
            ;;
    esac
done

install_deps() {
    log_info "Installing aarch64 cross toolchain..."
    if [ "$(id -u)" -eq 0 ]; then
        apt-get update
        apt-get install -y \
            gcc-aarch64-linux-gnu \
            g++-aarch64-linux-gnu \
            binutils-aarch64-linux-gnu \
            bison
    else
        sudo apt-get update
        sudo apt-get install -y \
            gcc-aarch64-linux-gnu \
            g++-aarch64-linux-gnu \
            binutils-aarch64-linux-gnu \
            bison
    fi
}

cd "$REPO_ROOT"

if [ "$SKIP_DEPS" = false ]; then
    install_deps
else
    log_warn "Skipping cross toolchain installation"
fi

export HOST
# Ensure build-machine tools are native, and let depends pick target tools by HOST
export CC_FOR_BUILD="${CC_FOR_BUILD:-gcc}"
export CXX_FOR_BUILD="${CXX_FOR_BUILD:-g++}"
export AR_FOR_BUILD="${AR_FOR_BUILD:-ar}"
export RANLIB_FOR_BUILD="${RANLIB_FOR_BUILD:-ranlib}"
unset CC CXX AR RANLIB NM STRIP

log_info "Cross-compiling for $HOST"

# Clean transient native_qt build artifacts.
# Keep depends/$HOST/native intact, as cached Qt configure stages may reuse
# it for host tools (Qt6HostInfo/moc/rcc) during CMake regeneration.
rm -rf \
    "$REPO_ROOT/depends/work/build/$HOST/native_qt" \
    "$REPO_ROOT/depends/work/installed/$HOST/native_qt" \
    "$REPO_ROOT/depends/work/staging/$HOST/native_qt" 2>/dev/null || true

exec "$REPO_ROOT/scripts/build-depends.sh" "${FORWARDED_ARGS[@]}"
