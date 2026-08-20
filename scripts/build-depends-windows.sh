#!/bin/bash
# Build OrganicLifeCoin Windows binaries on Ubuntu VPS
# Usage: ./build-depends-windows.sh
# This script installs dependencies, builds the Windows depends, configures, and compiles everything

set -e

# Configuration
HOST=x86_64-w64-mingw32
JOBS=$(nproc)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR" && git rev-parse --show-toplevel 2>/dev/null || true)"
if [ -z "$REPO_ROOT" ]; then
    if [ -f "$SCRIPT_DIR/../configure.ac" ]; then
        REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
    else
        REPO_ROOT="$SCRIPT_DIR"
    fi
fi
DEPENDS_DIR="$REPO_ROOT/depends"
BUILD_DIR="$REPO_ROOT/build-windows"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

log_step() {
    echo -e "${BLUE}[STEP]${NC} $1"
}

apt_cmd() {
    if [ "$(id -u)" -eq 0 ]; then
        apt-get "$@"
    else
        sudo apt-get "$@"
    fi
}

setup_ccache() {
    if command -v ccache >/dev/null 2>&1; then
        local tmp="${CCACHE_TEMPDIR:-$REPO_ROOT/.ccache-tmp}"
        mkdir -p "$tmp"
        export CCACHE_TEMPDIR="$tmp"
    fi
}

# Ensure MinGW uses POSIX threading (win32 toolchain breaks std::mutex/condvar)
ensure_mingw_posix_threads() {
    if ! command -v "$HOST-g++" &>/dev/null; then
        log_warn "MinGW compiler $HOST-g++ not found yet; skipping thread model check"
        return 0
    fi

    local thread_model
    thread_model="$("$HOST-g++" -v 2>&1 | sed -n 's/^Thread model: //p' | tail -n 1)"
    if [ "$thread_model" = "posix" ]; then
        log_info "MinGW thread model: posix"
        return 0
    fi

    if [ "$thread_model" = "win32" ]; then
        log_warn "MinGW thread model is win32; switching to posix (required)"
    else
        log_warn "Could not detect MinGW thread model (got: '$thread_model'); attempting to select posix anyway"
    fi

    if command -v update-alternatives &>/dev/null; then
        local gcc_posix gpp_posix
        gcc_posix="$(command -v "$HOST-gcc-posix" 2>/dev/null || true)"
        gpp_posix="$(command -v "$HOST-g++-posix" 2>/dev/null || true)"

        if [ -n "$gcc_posix" ]; then
            sudo update-alternatives --set "$HOST-gcc" "$gcc_posix" >/dev/null 2>&1 || true
        fi
        if [ -n "$gpp_posix" ]; then
            sudo update-alternatives --set "$HOST-g++" "$gpp_posix" >/dev/null 2>&1 || true
        fi
    fi

    thread_model="$("$HOST-g++" -v 2>&1 | sed -n 's/^Thread model: //p' | tail -n 1)"
    if [ "$thread_model" != "posix" ]; then
        log_error "MinGW thread model is still '$thread_model'. Install/select the POSIX toolchain (e.g. g++-mingw-w64-x86-64-posix) and re-run."
        exit 1
    fi
    log_info "MinGW thread model switched to posix"
}

# Function to check if we're on Ubuntu/Debian
check_os() {
    if [ -f /etc/os-release ]; then
        . /etc/os-release
        if [[ "$ID" != "ubuntu" && "$ID" != "debian" ]]; then
            log_warn "This script is designed for Ubuntu/Debian. Detected: $ID"
            read -p "Continue anyway? (y/N) " -n 1 -r
            echo
            if [[ ! $REPLY =~ ^[Yy]$ ]]; then
                exit 1
            fi
        fi
    else
        log_warn "Cannot detect OS. This script works best on Ubuntu/Debian."
    fi
}

# Install system dependencies
install_deps() {
    log_step "Installing system dependencies..."
    
    apt_cmd update
    
    # Core build tools
    apt_cmd install -y \
        build-essential \
        libtool \
        autotools-dev \
        automake \
        pkg-config \
        bsdmainutils \
        python3 \
        python3-pip \
        curl \
        git \
        wget \
        unzip \
        zip
    
    # MinGW cross compiler
    apt_cmd install -y \
        g++-mingw-w64-x86-64 \
        mingw-w64-x86-64-dev

    # Ensure POSIX threading is selected for MinGW (Ubuntu/Debian provide alternatives)
    ensure_mingw_posix_threads
    
    # CMake and build tools
    apt_cmd install -y \
        cmake \
        ninja-build \
        nasm \
        yasm
    
    # Compression libraries
    apt_cmd install -y \
        libz-dev \
        libbz2-dev \
        liblzma-dev \
        libssl-dev
    
    # Additional tools
    apt_cmd install -y \
        rsync \
        ccache \
        nsis
    
    log_info "System dependencies installed!"
}

# Install Rust with Windows target
install_rust() {
    log_step "Installing Rust..."
    
    if command -v rustc &> /dev/null; then
        log_info "Rust already installed: $(rustc --version)"
    else
        curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --default-toolchain stable
        source "$HOME/.cargo/env"
    fi
    
    # Add Windows target
    rustup target add x86_64-pc-windows-gnu 2>/dev/null || true
    
    log_info "Rust installed with Windows target!"
}

# Build depends
build_depends() {
    log_step "Building dependencies for Windows..."
    log_info "This will take 1-3 hours depending on VPS speed..."
    
    cd "$DEPENDS_DIR"
    
    # Check if already built
    if [ -f "$HOST/lib/libQt6Core.a" ]; then
        log_info "Dependencies already built, skipping..."
        log_info "To rebuild, delete: rm -rf $HOST"
    else
        # Build with appropriate parallelism
        # Use -j1 if parallel builds fail (some packages have issues)
        log_info "Building with $JOBS parallel jobs..."
        if ! make HOST=$HOST -j$JOBS; then
            log_warn "Parallel build failed, retrying with -j4..."
            make HOST=$HOST -j4
        fi
        log_info "Dependencies built successfully!"
    fi
    
    cd ..
}

# Configure the project
configure_project() {
    log_step "Configuring project..."
    
    # Clean previous configure/build artifacts to avoid mixed toolchains
    log_info "Cleaning previous configuration..."
    make distclean 2>/dev/null || true
    
    # Regenerate configure if explicitly requested, missing, or build inputs changed.
    local autogen_needed=false
    local autogen_reason=""
    local autogen_stamp="$REPO_ROOT/.autogen-inputs.sha256"
    local current_autogen_hash=""
    local previous_autogen_hash=""

    if command -v sha256sum >/dev/null 2>&1; then
        current_autogen_hash="$(
            {
                [ -f configure.ac ] && sha256sum configure.ac
                find build-aux/m4 -type f -name "*.m4" -print0 2>/dev/null | sort -z | xargs -0r sha256sum
                find . -maxdepth 3 -type f \( -name "Makefile.am" -o -name "Makefile.*include" \) -print0 2>/dev/null | sort -z | xargs -0r sha256sum
            } | sha256sum | awk '{print $1}'
        )"
        if [ -f "$autogen_stamp" ]; then
            previous_autogen_hash="$(cat "$autogen_stamp" 2>/dev/null || true)"
        fi
    fi

    if [ "$FORCE_AUTOGEN" = true ] || [ ! -f "configure" ]; then
        autogen_needed=true
        autogen_reason="forced/missing configure"
    elif [ -n "$current_autogen_hash" ] && [ "$current_autogen_hash" != "$previous_autogen_hash" ]; then
        autogen_needed=true
        autogen_reason="autotools inputs changed"
    elif [ configure.ac -nt configure ]; then
        autogen_needed=true
        autogen_reason="configure.ac newer than configure"
    elif find build-aux/m4 -type f -name "*.m4" -newer configure -print -quit 2>/dev/null | grep -q .; then
        autogen_needed=true
        autogen_reason="m4 macro newer than configure"
    elif [ ! -f "Makefile.in" ]; then
        autogen_needed=true
        autogen_reason="Makefile.in missing"
    elif find . -maxdepth 3 -type f \( -name "Makefile.am" -o -name "Makefile.*include" \) -newer Makefile.in -print -quit 2>/dev/null | grep -q .; then
        autogen_needed=true
        autogen_reason="Makefile inputs newer than Makefile.in"
    fi

    if [ "$autogen_needed" = true ]; then
        log_info "Running autogen.sh${autogen_reason:+ ($autogen_reason)}..."
        ./autogen.sh
        if [ -n "$current_autogen_hash" ]; then
            printf "%s\n" "$current_autogen_hash" > "$autogen_stamp"
        fi
    else
        if [ -n "$current_autogen_hash" ] && [ ! -f "$autogen_stamp" ]; then
            printf "%s\n" "$current_autogen_hash" > "$autogen_stamp"
        fi
        log_info "Skipping autogen.sh (configure already present)"
        # Avoid make re-running autotools on low-memory hosts.
        touch aclocal.m4 Makefile.in configure src/config/pivx-config.h.in 2>/dev/null || true
    fi
    
    # Configure with Windows settings
    log_info "Running configure..."
    local depends_prefix="$DEPENDS_DIR/$HOST"
    local target_bin="$depends_prefix/bin"
    # Ensure native Qt tools (moc/uic/rcc/...) are used during configure & build.
    # If the target (Windows) tools are picked up, rcc/moc can fail to run and lead to missing qInitResources_* symbols.
    local native_bin="$depends_prefix/native/bin"
    local native_libexec="$depends_prefix/native/libexec"
    local qt_bindir="$target_bin:$native_bin:$native_libexec"
    if [ -d "$target_bin" ]; then
        export PATH="$target_bin:$PATH"
        [ -x "$target_bin/qmake6" ] && export QMAKE="$target_bin/qmake6"
    fi
    if [ -d "$native_bin" ]; then
        export PATH="$PATH:$native_bin"
        if [ -z "${QMAKE:-}" ] && [ -x "$native_bin/qmake6" ]; then
            export QMAKE="$native_bin/qmake6"
        fi
    fi
    if [ -d "$native_libexec" ]; then
        export PATH="$PATH:$native_libexec"
    fi

    # Prefer Qt host tools from depends if present.
    # Newer builds use QT_RCC (not RCC) to avoid env var/toolchain collisions.
    if [ -x "$native_bin/rcc" ]; then
        export QT_RCC="$native_bin/rcc"
        export RCC="$native_bin/rcc" # backward compat if someone uses old configure
    elif [ -x "$native_libexec/rcc" ]; then
        export QT_RCC="$native_libexec/rcc"
        export RCC="$native_libexec/rcc"
    fi
    if [ -x "$native_bin/moc" ]; then
        export MOC="$native_bin/moc"
    elif [ -x "$native_libexec/moc" ]; then
        export MOC="$native_libexec/moc"
    fi
    if [ -x "$native_bin/uic" ]; then
        export UIC="$native_bin/uic"
    elif [ -x "$native_libexec/uic" ]; then
        export UIC="$native_libexec/uic"
    fi
    if ! ./configure \
        --prefix="$depends_prefix" \
        --host="$HOST" \
        --with-qt-bindir="$qt_bindir" \
        --with-gui=qt6 \
        --enable-mining-rpc \
        --disable-tests \
        --disable-bench \
        --disable-gui-tests \
        --with-qrencode; then
        local config_site="$depends_prefix/share/config.site"
        if [ -f "$config_site" ]; then
            log_warn "Configure failed without CONFIG_SITE; retrying with CONFIG_SITE=$config_site"
            CONFIG_SITE="$config_site" ./configure \
                --prefix="$depends_prefix" \
                --host="$HOST" \
                --with-qt-bindir="$qt_bindir" \
                --with-gui=qt6 \
                --disable-tests \
                --disable-bench \
                --disable-gui-tests \
                --with-qrencode
        else
            return 1
        fi
    fi
    
    log_info "Configuration complete!"
}

# Build the project
build_project() {
    log_step "Building OrganicLifeCoin..."
    log_info "Building with $JOBS parallel jobs..."
    
    make -j$JOBS
    
    log_info "Build complete!"
}

# Build Windows installer
build_installer() {
    log_step "Building Windows installer..."
    if ! command -v makensis &>/dev/null; then
        log_error "makensis (NSIS) not found. Install 'nsis' and re-run."
        return 1
    fi

    make deploy
    log_info "Installer build complete!"
}

# Package the binaries
package_binaries() {
    log_step "Packaging binaries..."
    
    VERSION=$(git describe --tags --dirty 2>/dev/null || echo "unknown")
    PACKAGE_DIR="OrganicLifeCoin-windows-$VERSION"
    
    mkdir -p "$PACKAGE_DIR"

    # Copy Windows executables
    cp src/organiclifed.exe "$PACKAGE_DIR/" 2>/dev/null || true
    cp src/organiclife-cli.exe "$PACKAGE_DIR/" 2>/dev/null || true
    cp src/organiclife-tx.exe "$PACKAGE_DIR/" 2>/dev/null || true
    cp src/qt/organiclife-qt.exe "$PACKAGE_DIR/" 2>/dev/null || true

    # Include Sapling params so the wallet can copy them to %APPDATA%\\OrganicLifeCoinParams on first run
    if [ -f "params/sapling-spend.params" ] && [ -f "params/sapling-output.params" ]; then
        mkdir -p "$PACKAGE_DIR/params"
        cp params/sapling-spend.params "$PACKAGE_DIR/params/"
        cp params/sapling-output.params "$PACKAGE_DIR/params/"
    else
        log_warn "Sapling params not found in ./params; Windows wallet may fail to start shielded features without manual params installation."
    fi
    
    # Create archive
    zip -r "$PACKAGE_DIR.zip" "$PACKAGE_DIR"
    
    log_info "Binaries packaged: $PACKAGE_DIR.zip"
}

# Print summary
print_summary() {
    echo ""
    echo "========================================"
    echo "  Build Complete!"
    echo "========================================"
    echo ""
    echo "Windows binaries:"
    ls -lh src/*.exe src/qt/*.exe 2>/dev/null || echo "  (executables in src/ and src/qt/)"
    echo ""
    echo "To run on Windows:"
    echo "  1. Copy organiclife-qt.exe to Windows machine"
    echo "  2. Run it (no installation needed)"
    echo ""
    echo "Package location:"
    ls -lh OrganicLifeCoin-windows-*.zip 2>/dev/null || true
    echo ""
    echo "Installer:"
    ls -lh OrganicLifeCoin-*-win64-setup*.exe 2>/dev/null || true
    echo ""
}

# Main function
main() {
    echo "========================================"
    echo "  OrganicLifeCoin Windows Build Script"
    echo "========================================"
    echo ""

    cd "$REPO_ROOT"
    
    # Check if running from correct directory
    if [ ! -f "configure.ac" ] || [ ! -d "src" ]; then
        log_error "Please run this script from the OrganicLifeCoin source directory"
        exit 1
    fi
    
    # Check OS
    check_os
    
    # Parse arguments
    SKIP_DEPS=false
    SKIP_DEPENDS_BUILD=false
    FORCE_AUTOGEN=false
    
    while [[ $# -gt 0 ]]; do
        case $1 in
            --skip-system-deps)
                SKIP_DEPS=true
                shift
                ;;
            --skip-depends)
                SKIP_DEPENDS_BUILD=true
                shift
                ;;
            --jobs|-j)
                JOBS="$2"
                shift 2
                ;;
            --help|-h)
                echo "Usage: $0 [OPTIONS]"
                echo ""
                echo "Options:"
                echo "  --skip-system-deps    Skip installing system dependencies"
                echo "  --skip-depends        Skip building depends (if already built)"
                echo "  --force-autogen       Force running autogen.sh"
                echo "  --jobs N, -j N        Use N parallel jobs (default: $(nproc))"
                echo "  --help, -h            Show this help"
                exit 0
                ;;
            --force-autogen)
                FORCE_AUTOGEN=true
                shift
                ;;
            *)
                log_error "Unknown option: $1"
                exit 1
                ;;
        esac
    done
    
    log_info "Using $JOBS parallel jobs"
    log_info "Target: $HOST"
    setup_ccache

    # Install system dependencies
    if [ "$SKIP_DEPS" = false ]; then
        install_deps
    else
        log_info "Skipping system dependencies installation"
        ensure_mingw_posix_threads
    fi
    
    # Install Rust
    install_rust
    
    # Build depends
    if [ "$SKIP_DEPENDS_BUILD" = false ]; then
        build_depends
    else
        log_info "Skipping depends build"
    fi
    
    # Configure
    configure_project
    
    # Build
    build_project

    # Build installer
    build_installer
    
    # Package
    package_binaries
    
    # Summary
    print_summary
}

# Run main function
main "$@"
