#!/usr/bin/env bash
#
# CTEAM Build Script using Depends System
# Builds CTEAM with static dependencies for release-quality binaries
#
# Usage: ./build-depends.sh [options]
#
# Options:
#   --install-deps    Install required dependencies (auto-detect OS)
#   --no-gui          Build without Qt GUI (daemon only)
#   --no-wallet       Build without wallet support
#   --with-zmq        Enable ZeroMQ notifications
#   --with-upnp       Enable UPnP port mapping
#   --no-config-site  Do not use depends config.site (CONFIG_SITE)
#   --debug           Build with debug symbols
#   --clean           Clean build before building
#   --force-autogen   Force running autogen.sh
#   --jobs N          Number of parallel build jobs (default: auto)
#   --help            Show this help message
#

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Script directory and repo root (allows moving script into a subdir like scripts/)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR" && git rev-parse --show-toplevel 2>/dev/null || true)"
if [ -z "$REPO_ROOT" ]; then
    if [ -f "$SCRIPT_DIR/../configure.ac" ]; then
        REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
    else
        REPO_ROOT="$SCRIPT_DIR"
    fi
fi
DEPENDS_DIR="${REPO_ROOT}/depends"

# Default options
BUILD_GUI=true
BUILD_WALLET=true
ENABLE_ZMQ=false
ENABLE_UPNP=false
DEBUG_BUILD=false
CLEAN_BUILD=false
FORCE_AUTOGEN=false
INSTALL_DEPS_ONLY=false
JOBS=""
USE_CONFIG_SITE=true
NATIVE_QT_TOOLS_READY=false

# Print colored message
print_msg() {
    local color=$1
    local msg=$2
    echo -e "${color}${msg}${NC}"
}

print_info() {
    print_msg "${BLUE}" "[INFO] $1"
}

print_success() {
    print_msg "${GREEN}" "[OK] $1"
}

print_warning() {
    print_msg "${YELLOW}" "[WARN] $1"
}

print_error() {
    print_msg "${RED}" "[ERROR] $1"
}

target_is_runnable_on_host() {
    local binary_path="$1"
    if [ ! -f "$binary_path" ]; then
        return 1
    fi

    local desc
    desc="$(file -b "$binary_path" 2>/dev/null || true)"
    case "$ARCH" in
        x86_64|amd64)
            echo "$desc" | rg -q "x86-64|x86_64"
            ;;
        aarch64|arm64)
            echo "$desc" | rg -q "ARM aarch64|aarch64"
            ;;
        *)
            return 1
            ;;
    esac
}

# Clean any previous configure/build artifacts to avoid mixed toolchains
distclean_tree() {
    if [ -f "$REPO_ROOT/Makefile" ] || [ -f "$REPO_ROOT/config.status" ] || [ -f "$REPO_ROOT/src/Makefile" ]; then
        print_info "Running make distclean..."
        (cd "$REPO_ROOT" && make distclean >/dev/null 2>&1 || true)
    fi
    purge_stale_subproject_artifacts
}

purge_stale_subproject_artifacts() {
    local subdirs=(
        "$REPO_ROOT/src/chiabls"
        "$REPO_ROOT/src/secp256k1"
        "$REPO_ROOT/src/univalue"
    )
    local d
    for d in "${subdirs[@]}"; do
        if [ -d "$d" ]; then
            find "$d" -maxdepth 3 -type f -name '*.la' -delete 2>/dev/null || true
            find "$d" -maxdepth 3 -type d -name '.libs' -prune -exec rm -rf {} + 2>/dev/null || true
            find "$d" -maxdepth 3 -type d -name '.deps' -prune -exec rm -rf {} + 2>/dev/null || true
            find "$d" -maxdepth 8 -type f \( -name '*.o' -o -name '*.lo' -o -name '*.Po' -o -name '*.Plo' -o -name '*.obj' \) -delete 2>/dev/null || true
        fi
    done
}

# Show help
show_help() {
    head -n 19 "$0" | tail -n 16 | sed 's/^#//' | sed 's/^ //'
    exit 0
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --install-deps)
            INSTALL_DEPS_ONLY=true
            shift
            ;;
        --no-gui)
            BUILD_GUI=false
            shift
            ;;
        --no-wallet)
            BUILD_WALLET=false
            shift
            ;;
        --with-zmq)
            ENABLE_ZMQ=true
            shift
            ;;
        --with-upnp)
            ENABLE_UPNP=true
            shift
            ;;
        --no-config-site)
            USE_CONFIG_SITE=false
            shift
            ;;
        --debug)
            DEBUG_BUILD=true
            shift
            ;;
        --clean)
            CLEAN_BUILD=true
            shift
            ;;
        --force-autogen)
            FORCE_AUTOGEN=true
            shift
            ;;
        --jobs)
            JOBS="$2"
            shift 2
            ;;
        --help|-h)
            show_help
            ;;
        *)
            print_error "Unknown option: $1"
            echo "Use --help for usage information"
            exit 1
            ;;
    esac
done

# Detect OS and architecture
detect_platform() {
    case "$(uname -s)" in
        Linux*)     OS="Linux";;
        Darwin*)    OS="macOS";;
        *)          OS="Unknown";;
    esac

    ARCH="$(uname -m)"

    # Prefer the depends config.guess so our HOST matches the depends output dir
    # (e.g. aarch64-apple-darwin25.2.0), otherwise --prefix won't line up.
    if [ -n "${HOST:-}" ]; then
        print_info "Using HOST from environment: $HOST"
    elif [ -x "$DEPENDS_DIR/config.guess" ]; then
        HOST="$("$DEPENDS_DIR/config.guess")"
    else
        # Fallback (best-effort)
        case "$OS" in
            Linux)
                case "$ARCH" in
                    x86_64)  HOST="x86_64-pc-linux-gnu";;
                    aarch64) HOST="aarch64-linux-gnu";;
                    armv7l)  HOST="arm-linux-gnueabihf";;
                    *)       HOST="$ARCH-pc-linux-gnu";;
                esac
                ;;
            macOS)
                case "$ARCH" in
                    x86_64)  HOST="x86_64-apple-darwin";;
                    arm64)   HOST="arm64-apple-darwin";;
                    *)       HOST="$ARCH-apple-darwin";;
                esac
                ;;
        esac
    fi

    print_info "Detected: $OS ($ARCH)"
    print_info "Host triplet: $HOST"
}

# Check if running with sudo/root
is_root() {
    [ "$(id -u)" -eq 0 ]
}

setup_ccache() {
    if command -v ccache >/dev/null 2>&1; then
        local tmp="${CCACHE_TEMPDIR:-$REPO_ROOT/.ccache-tmp}"
        mkdir -p "$tmp"
        export CCACHE_TEMPDIR="$tmp"
    fi
}

# Prompt user for confirmation
confirm() {
    local prompt="$1"
    local default="${2:-n}"

    if [ "$default" = "y" ]; then
        prompt="$prompt [Y/n] "
    else
        prompt="$prompt [y/N] "
    fi

    read -r -p "$prompt" response
    response="${response:-$default}"
    [[ "$response" =~ ^[Yy]$ ]]
}

# Install dependencies for macOS
install_macos_deps() {
    print_info "Installing dependencies for macOS..."

    # Check for Xcode command line tools
    if ! xcode-select -p &> /dev/null; then
        print_info "Installing Xcode command line tools..."
        xcode-select --install
        echo ""
        print_warning "Please complete the Xcode tools installation dialog, then run this script again."
        exit 0
    fi

    # Check for Homebrew
    if ! command -v brew &> /dev/null; then
        print_info "Installing Homebrew..."
        /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

        # Add brew to path for Apple Silicon
        if [ -f /opt/homebrew/bin/brew ]; then
            eval "$(/opt/homebrew/bin/brew shellenv)"
        fi
    fi

    # Install packages via Homebrew
    local brew_packages="autoconf automake libtool pkg-config python3"
    print_info "Installing Homebrew packages: $brew_packages"
    brew install $brew_packages

    # Install Python setuptools (required for native_ds_store)
    print_info "Installing Python setuptools..."
    python3 -m pip install --break-system-packages setuptools 2>/dev/null || \
    python3 -m pip install --user setuptools 2>/dev/null || \
    python3 -m pip install setuptools 2>/dev/null || true

    print_success "macOS dependencies installed"
}

# Install dependencies for Linux (Debian/Ubuntu)
install_linux_deps_debian() {
    print_info "Installing dependencies for Debian/Ubuntu..."

    # Ubuntu 24.04+ no longer ships bsdmainutils (use bsdextrautils instead).
    local bsd_utils="bsdextrautils"
    if command -v apt-cache &> /dev/null; then
        if apt-cache show bsdmainutils &> /dev/null; then
            bsd_utils="bsdmainutils"
        fi
    fi

    # Keep this list slightly "over-complete" so depends builds are smooth.
    # Note: on Debian/Ubuntu, the `libtool` executable is provided by `libtool-bin`.
    local packages="build-essential git autoconf automake libtool libtool-bin pkg-config \
python3 python3-pip python3-setuptools ca-certificates curl \
cmake ninja-build bison gperf unzip zip xz-utils patch ${bsd_utils}"

    if is_root; then
        apt-get update
        apt-get install -y $packages
    else
        print_info "Running apt-get with sudo..."
        sudo apt-get update
        sudo apt-get install -y $packages
    fi

    print_success "Debian/Ubuntu dependencies installed"
}

# Install dependencies for Linux (Fedora/RHEL)
install_linux_deps_fedora() {
    print_info "Installing dependencies for Fedora/RHEL..."

    local packages="gcc-c++ git autoconf automake libtool pkgconfig python3 python3-pip python3-setuptools curl"

    if is_root; then
        dnf install -y $packages
    else
        print_info "Running dnf with sudo..."
        sudo dnf install -y $packages
    fi

    print_success "Fedora/RHEL dependencies installed"
}

# Install dependencies for Linux (Arch)
install_linux_deps_arch() {
    print_info "Installing dependencies for Arch Linux..."

    local packages="base-devel git autoconf automake libtool pkgconf python python-pip python-setuptools curl"

    if is_root; then
        pacman -Sy --noconfirm $packages
    else
        print_info "Running pacman with sudo..."
        sudo pacman -Sy --noconfirm $packages
    fi

    print_success "Arch Linux dependencies installed"
}

# Detect Linux distribution
detect_linux_distro() {
    if [ -f /etc/os-release ]; then
        . /etc/os-release
        DISTRO="$ID"
        DISTRO_FAMILY="$ID_LIKE"
    elif [ -f /etc/debian_version ]; then
        DISTRO="debian"
    elif [ -f /etc/fedora-release ]; then
        DISTRO="fedora"
    elif [ -f /etc/arch-release ]; then
        DISTRO="arch"
    else
        DISTRO="unknown"
    fi
}

# Install Rust if missing
install_rust() {
    if command -v cargo &> /dev/null; then
        print_success "Rust is already installed"
        return 0
    fi

    print_info "Installing Rust..."
    curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y

    # Source cargo environment
    if [ -f "$HOME/.cargo/env" ]; then
        source "$HOME/.cargo/env"
    fi

    if command -v cargo &> /dev/null; then
        print_success "Rust installed successfully"
    else
        print_warning "Rust installed but not in PATH. Please run: source ~/.cargo/env"
    fi
}

# Install all dependencies
install_dependencies() {
    print_info "=== Installing Dependencies ==="
    echo ""

    if [ "$OS" = "macOS" ]; then
        install_macos_deps
    elif [ "$OS" = "Linux" ]; then
        detect_linux_distro
        case "$DISTRO" in
            ubuntu|debian|linuxmint|pop)
                install_linux_deps_debian
                ;;
            fedora|rhel|centos|rocky|almalinux)
                install_linux_deps_fedora
                ;;
            arch|manjaro|endeavouros)
                install_linux_deps_arch
                ;;
            *)
                # Try to detect by family
                if [[ "$DISTRO_FAMILY" == *"debian"* ]]; then
                    install_linux_deps_debian
                elif [[ "$DISTRO_FAMILY" == *"fedora"* ]] || [[ "$DISTRO_FAMILY" == *"rhel"* ]]; then
                    install_linux_deps_fedora
                elif [[ "$DISTRO_FAMILY" == *"arch"* ]]; then
                    install_linux_deps_arch
                else
                    print_error "Unsupported Linux distribution: $DISTRO"
                    print_info "Please install the following packages manually:"
                    echo "  - C/C++ compiler (gcc, g++)"
                    echo "  - git, make, autoconf, automake, libtool, pkg-config"
                    echo "  - python3, python3-pip, python3-setuptools"
                    echo "  - curl"
                    return 1
                fi
                ;;
        esac
    fi

    # Install Rust (cross-platform)
    install_rust

    echo ""
    print_success "All dependencies installed!"
    echo ""
}

# Check for required tools
check_requirements() {
    print_info "Checking requirements..."

    local missing=()
    local missing_details=()

    # Check basic build tools
    for tool in git make autoconf automake pkg-config; do
        if ! command -v "$tool" &> /dev/null; then
            missing+=("$tool")
        fi
    done

    # libtool can be installed but the `libtool` binary might live in `libtool-bin` (Debian/Ubuntu).
    # Accept libtoolize/glibtoolize as sufficient for autotools bootstrapping.
    if ! command -v libtool &> /dev/null; then
        if ! command -v libtoolize &> /dev/null && ! command -v glibtoolize &> /dev/null; then
            missing+=("libtool")
        fi
    fi

    # Check C++ compiler
    if ! command -v g++ &> /dev/null && ! command -v clang++ &> /dev/null; then
        missing+=("C++ compiler")
        missing_details+=("C++ compiler (g++ or clang++)")
    fi

    # Check Rust/Cargo
    if ! command -v cargo &> /dev/null; then
        # Try to source cargo env in case it's installed but not in PATH
        if [ -f "$HOME/.cargo/env" ]; then
            source "$HOME/.cargo/env"
        fi
        if ! command -v cargo &> /dev/null; then
            missing+=("cargo")
            missing_details+=("Rust/Cargo")
        fi
    fi

    # Check Python 3
    if ! command -v python3 &> /dev/null; then
        missing+=("python3")
    fi

    # Check Python setuptools (required for macOS native_ds_store)
    if command -v python3 &> /dev/null; then
        if ! python3 -c "import setuptools" &> /dev/null; then
            missing+=("python3-setuptools")
            missing_details+=("Python setuptools")
        fi
    fi

    # Check curl (needed for downloading sources)
    if ! command -v curl &> /dev/null; then
        missing+=("curl")
    fi

    # macOS specific checks
    if [ "$OS" = "macOS" ]; then
        if ! xcode-select -p &> /dev/null; then
            missing+=("xcode-cli")
            missing_details+=("Xcode Command Line Tools")
        fi
    fi

    if [ ${#missing[@]} -gt 0 ]; then
        print_error "Missing required tools:"
        for tool in "${missing[@]}"; do
            echo "  - $tool"
        done
        echo ""

        # Ask user if they want to install automatically
        if confirm "Would you like to install missing dependencies automatically?"; then
            install_dependencies
            # Re-check after installation
            print_info "Re-checking requirements after installation..."
            check_requirements
            return
        fi

        echo ""
        print_info "Manual installation instructions:"
        echo ""
        if [ "$OS" = "Linux" ]; then
            detect_linux_distro
            case "$DISTRO" in
                ubuntu|debian|linuxmint|pop)
                    print_info "On Ubuntu/Debian, install with:"
                    echo "  sudo apt-get update"
                    echo "  sudo apt-get install build-essential git autoconf automake libtool pkg-config python3 python3-pip python3-setuptools curl bsdmainutils"
                    ;;
                fedora|rhel|centos)
                    print_info "On Fedora/RHEL, install with:"
                    echo "  sudo dnf install gcc-c++ git autoconf automake libtool pkgconfig python3 python3-pip python3-setuptools curl"
                    ;;
                arch|manjaro)
                    print_info "On Arch Linux, install with:"
                    echo "  sudo pacman -Sy base-devel git autoconf automake libtool pkgconf python python-pip python-setuptools curl"
                    ;;
                *)
                    print_info "Please install the equivalent of these packages for your distribution:"
                    echo "  build-essential git autoconf automake libtool pkg-config python3 python3-pip python3-setuptools curl"
                    ;;
            esac
        elif [ "$OS" = "macOS" ]; then
            print_info "On macOS, install with:"
            echo "  xcode-select --install"
            echo "  /bin/bash -c \"\$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)\""
            echo "  brew install autoconf automake libtool pkg-config python3"
            echo "  python3 -m pip install --break-system-packages setuptools"
        fi
        echo ""
        print_info "Then install Rust with:"
        echo "  curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh"
        echo "  source ~/.cargo/env"
        echo ""
        exit 1
    fi

    print_success "All required tools found"
}

# Check for X11/XCB libraries required for GUI build
check_gui_dependencies() {
    # Skip if not building GUI
    if [ "$BUILD_GUI" = false ]; then
        return 0
    fi

    print_info "Checking GUI build dependencies..."

    local missing_x11_libs=()

    # Check for key XCB/X11 libraries needed by Qt6 (depends builds Qt6 from source)
    if [ "$OS" = "Linux" ]; then
        # Check for critical XCB/X11 libraries needed by Qt6
        # Some xcb libs use different pkg-config names than their package names
        declare -A lib_checks=(
            ["xcb"]="xcb"
            ["xcb-xinerama"]="xcb-xinerama"
            ["xcb-xkb"]="xcb-xkb"
            ["xkbcommon-x11"]="xkbcommon-x11"
            ["xcb-randr"]="xcb-randr"
            ["xcb-image"]="xcb-image"
            ["xcb-keysyms"]="xcb-keysyms"
            ["xcb-icccm"]="xcb-icccm"
            ["xcb-sync"]="xcb-sync"
            ["xcb-xfixes"]="xcb-xfixes"
            ["xcb-shape"]="xcb-shape"
            ["xcb-render-util"]="xcb-renderutil"
            ["xcb-cursor"]="xcb-cursor"
            ["x11"]="x11"
            ["xext"]="xext"
            ["xfixes"]="xfixes"
            ["xi"]="xi"
            ["xrender"]="xrender"
            ["sm"]="sm"
            ["ice"]="ice"
            ["fontconfig"]="fontconfig"
            ["freetype2"]="freetype2"
        )

        for lib in "${!lib_checks[@]}"; do
            local pc_name="${lib_checks[$lib]}"
            local found=false
            
            # Check pkg-config
            if pkg-config --exists "$pc_name" 2>/dev/null; then
                found=true
            elif pkg-config --exists "lib$pc_name" 2>/dev/null; then
                found=true
            elif [ "$pc_name" = "xcb-renderutil" ] && pkg-config --exists "xcb-render-util" 2>/dev/null; then
                found=true
            fi
            
            # Fallback: check if library file exists (some Ubuntu packages don't have .pc files)
            if [ "$found" = false ]; then
                local lib_file="lib${pc_name}.so"
                if ldconfig -p 2>/dev/null | grep -q "$lib_file"; then
                    found=true
                elif [ -f "/usr/lib/x86_64-linux-gnu/$lib_file" ] || [ -f "/usr/lib64/$lib_file" ] || [ -f "/usr/lib/$lib_file" ]; then
                    found=true
                fi
            fi
            
            if [ "$found" = false ]; then
                missing_x11_libs+=("$lib")
            fi
        done

        if [ ${#missing_x11_libs[@]} -gt 0 ]; then
            echo ""
            print_error "Missing X11/XCB libraries required for GUI build:"
            echo "  Missing: ${missing_x11_libs[*]}"
            echo ""
            print_info "Install with:"
            echo ""
            echo "  Ubuntu/Debian:"
            echo "    sudo apt-get install -y libx11-dev libx11-xcb-dev libsm-dev libice-dev \\"
            echo "      libxcb-xinerama0-dev libxcb-xkb-dev libxcb-xinput-dev \\"
            echo "      libxcb-randr0-dev libxcb-image0-dev libxcb-keysyms1-dev \\"
            echo "      libxcb-icccm4-dev libxcb-sync-dev libxcb-xfixes0-dev \\"
            echo "      libxcb-shape0-dev libxcb-render-util0-dev libxcb-util-dev \\"
            echo "      libxcb-cursor-dev libxkbcommon-dev libxkbcommon-x11-dev \\"
            echo "      libfontconfig1-dev libfreetype6-dev libxext-dev libxfixes-dev \\"
            echo "      libxi-dev libxrender-dev libgl1-mesa-dev libglu1-mesa-dev \\"
            echo "      libdbus-1-dev libatspi2.0-dev"
            echo ""
            echo "  Fedora:"
            echo "    sudo dnf install -y libX11-devel libXext-devel libSM-devel libICE-devel \\"
            echo "      libxcb-devel libxkbcommon-devel libxkbcommon-x11-devel \\"
            echo "      fontconfig-devel freetype-devel libXfixes-devel libXi-devel \\"
            echo "      libXrender-devel mesa-libGL-devel mesa-libGLU-devel dbus-devel \\"
            echo "      at-spi2-core-devel"
            echo ""
            echo "  Arch:"
            echo "    sudo pacman -S libx11 libxext libsm libice libxcb libxkbcommon \\"
            echo "      libxkbcommon-x11 fontconfig freetype2 libxfixes libxi libxrender \\"
            echo "      mesa glu dbus at-spi2-core"
            echo ""
            print_info "Or build without GUI (faster, no X11 required):"
            echo ""
            echo "    ./build-depends.sh --no-gui"
            echo ""

            if confirm "Would you like to install the missing X11 packages automatically?"; then
                print_info "Installing X11 dependencies..."
                detect_linux_distro
                case "$DISTRO" in
                    ubuntu|debian|linuxmint|pop|*)
                        sudo apt-get update
                        sudo apt-get install -y \
                            libx11-dev libx11-xcb-dev libsm-dev libice-dev \
                            libxcb-xinerama0-dev libxcb-xkb-dev libxcb-xinput-dev \
                            libxcb-randr0-dev libxcb-image0-dev libxcb-keysyms1-dev \
                            libxcb-icccm4-dev libxcb-sync-dev libxcb-xfixes0-dev \
                            libxcb-shape0-dev libxcb-render-util0-dev libxcb-util-dev \
                            libxcb-cursor-dev libxkbcommon-dev libxkbcommon-x11-dev \
                            libfontconfig1-dev libfreetype6-dev libxext-dev libxfixes-dev \
                            libxi-dev libxrender-dev libgl1-mesa-dev libglu1-mesa-dev \
                            libdbus-1-dev libatspi2.0-dev
                        ;;
                    fedora|rhel|centos|rocky|almalinux)
                        sudo dnf install -y \
                            libX11-devel libXext-devel libSM-devel libICE-devel \
                            libxcb-devel libxkbcommon-devel libxkbcommon-x11-devel \
                            fontconfig-devel freetype-devel libXfixes-devel libXi-devel \
                            libXrender-devel mesa-libGL-devel mesa-libGLU-devel \
                            dbus-devel at-spi2-core-devel
                        ;;
                    arch|manjaro|endeavouros)
                        sudo pacman -S --needed \
                            libx11 libxext libsm libice libxcb libxkbcommon \
                            libxkbcommon-x11 fontconfig freetype2 libxfixes libxi \
                            libxrender mesa glu dbus at-spi2-core
                        ;;
                    *)
                        print_error "Could not auto-install. Please install manually using the commands above."
                        exit 1
                        ;;
                esac
                print_success "X11 dependencies installed"
            else
                print_error "Cannot build GUI without X11 libraries."
                print_info "Run with --no-gui to build daemon only, or install the packages above."
                exit 1
            fi
        fi
    fi

    print_success "GUI dependencies OK"
}

# Build dependencies
build_depends() {
    print_info "Building dependencies (this may take a while on first run)..."

    cd "$DEPENDS_DIR"

    if [ ! -f "Makefile" ] && [ ! -f "makefile" ] && [ ! -f "GNUmakefile" ]; then
        print_error "No Makefile found in: $DEPENDS_DIR"
        echo ""
        echo "This repo should contain a depends/Makefile. Common causes:"
        echo "  - The repo was not fully synced (rsync excluded depends/, or upload incomplete)."
        echo "  - You're running from a different directory than you think."
        echo ""
        echo "Quick checks:"
        echo "  ls -la \"$DEPENDS_DIR\""
        echo ""
        echo "Fix:"
        echo "  Re-rsync the project ensuring 'depends/Makefile' is included."
        exit 1
    fi

    if [ "$CLEAN_BUILD" = true ]; then
        print_info "Cleaning depends build directories..."
        make HOST="$HOST" clean || true
    fi

    # Determine number of jobs
    local jobs_opt=""
    if [ -n "$JOBS" ]; then
        jobs_opt="-j$JOBS"
    else
        local cpu_count=""
        if [ "$OS" = "macOS" ]; then
            cpu_count="$(sysctl -n hw.ncpu 2>/dev/null || true)"
            if [ -z "$cpu_count" ]; then
                cpu_count="$(getconf _NPROCESSORS_ONLN 2>/dev/null || true)"
            fi
        else
            cpu_count="$(nproc 2>/dev/null || true)"
            if [ -z "$cpu_count" ]; then
                cpu_count="$(getconf _NPROCESSORS_ONLN 2>/dev/null || true)"
            fi
        fi
        cpu_count="${cpu_count:-4}"
        jobs_opt="-j$cpu_count"
    fi

    # Build depends options
    local make_opts="$jobs_opt"

    if [ "$BUILD_GUI" = false ]; then
        make_opts="$make_opts NO_QT=1"
    fi

    if [ "$BUILD_WALLET" = false ]; then
        make_opts="$make_opts NO_WALLET=1"
    fi

    if [ "$ENABLE_ZMQ" = false ]; then
        make_opts="$make_opts NO_ZMQ=1"
    fi

    if [ "$ENABLE_UPNP" = false ]; then
        make_opts="$make_opts NO_UPNP=1"
    fi

    print_info "Running: make HOST=$HOST $make_opts"
    make HOST="$HOST" $make_opts

    print_success "Dependencies built successfully"
}

# Run autogen
run_autogen() {
    cd "$REPO_ROOT"

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

    if [ "$FORCE_AUTOGEN" = true ] || [ ! -f configure ] || [ "$CLEAN_BUILD" = true ]; then
        autogen_needed=true
        autogen_reason="forced/missing configure/clean build"
    elif [ -n "$current_autogen_hash" ] && [ "$current_autogen_hash" != "$previous_autogen_hash" ]; then
        autogen_needed=true
        autogen_reason="autotools inputs changed"
    elif [ configure.ac -nt configure ]; then
        autogen_needed=true
        autogen_reason="configure.ac newer than configure"
    elif find build-aux/m4 -type f -name "*.m4" -newer configure -print -quit 2>/dev/null | grep -q .; then
        autogen_needed=true
        autogen_reason="m4 macro newer than configure"
    elif [ ! -f Makefile.in ]; then
        autogen_needed=true
        autogen_reason="Makefile.in missing"
    elif find . -maxdepth 3 -type f \( -name "Makefile.am" -o -name "Makefile.*include" \) -newer Makefile.in -print -quit 2>/dev/null | grep -q .; then
        autogen_needed=true
        autogen_reason="Makefile inputs newer than Makefile.in"
    fi

    if [ "$autogen_needed" = true ]; then
        print_info "Running autogen.sh${autogen_reason:+ ($autogen_reason)}..."
        ./autogen.sh
        if [ -n "$current_autogen_hash" ]; then
            printf "%s\n" "$current_autogen_hash" > "$autogen_stamp"
        fi
        print_success "Configure script ready"
        return 0
    fi

    if [ -n "$current_autogen_hash" ] && [ ! -f "$autogen_stamp" ]; then
        printf "%s\n" "$current_autogen_hash" > "$autogen_stamp"
    fi

    print_info "Skipping autogen.sh (configure up to date)"
    # Avoid make re-running autotools on low-memory hosts.
    touch aclocal.m4 Makefile.in configure src/config/pivx-config.h.in 2>/dev/null || true
    print_success "Configure script ready"
}

# Prefer native Qt host tools from depends when available (needed for cross builds).
setup_native_qt_tools() {
    if [ "$NATIVE_QT_TOOLS_READY" = true ]; then
        return 0
    fi

    local depends_prefix="$DEPENDS_DIR/$HOST"
    local native_bin="$depends_prefix/native/bin"
    local native_libexec="$depends_prefix/native/libexec"
    local found=false

    if [ -d "$native_bin" ]; then
        case ":$PATH:" in
            *":$native_bin:"*) ;;
            *) PATH="$native_bin:$PATH" ;;
        esac
    fi
    if [ -d "$native_libexec" ]; then
        case ":$PATH:" in
            *":$native_libexec:"*) ;;
            *) PATH="$native_libexec:$PATH" ;;
        esac
    fi

    if [ -x "$native_bin/qmake6" ]; then
        export QMAKE="$native_bin/qmake6"
        found=true
    elif [ -x "$native_bin/qmake" ]; then
        export QMAKE="$native_bin/qmake"
        found=true
    elif [ -x "$native_libexec/qmake6" ]; then
        export QMAKE="$native_libexec/qmake6"
        found=true
    elif [ -x "$native_libexec/qmake" ]; then
        export QMAKE="$native_libexec/qmake"
        found=true
    fi

    if [ -x "$native_bin/rcc" ]; then
        export QT_RCC="$native_bin/rcc"
        export RCC="$native_bin/rcc"
        found=true
    elif [ -x "$native_libexec/rcc" ]; then
        export QT_RCC="$native_libexec/rcc"
        export RCC="$native_libexec/rcc"
        found=true
    fi
    if [ -x "$native_bin/moc" ]; then
        export MOC="$native_bin/moc"
        found=true
    elif [ -x "$native_libexec/moc" ]; then
        export MOC="$native_libexec/moc"
        found=true
    fi
    if [ -x "$native_bin/uic" ]; then
        export UIC="$native_bin/uic"
        found=true
    elif [ -x "$native_libexec/uic" ]; then
        export UIC="$native_libexec/uic"
        found=true
    fi

    if [ "$found" = true ]; then
        NATIVE_QT_TOOLS_READY=true
        print_info "Using native Qt tools from depends: $depends_prefix/native"
    fi
}

# Remove generated Qt files that may have been produced by an older moc/uic/rcc.
# Keeping stale outputs in-tree can break incremental builds after Qt tool upgrades.
clean_stale_qt_generated_files() {
    if [ "$BUILD_GUI" != true ]; then
        return 0
    fi

    local qt_dir="$REPO_ROOT/src/qt"
    if [ ! -d "$qt_dir" ]; then
        return 0
    fi

    print_info "Cleaning stale generated Qt files..."
    rm -f "$qt_dir"/*.moc "$qt_dir"/moc_*.cpp "$qt_dir"/qrc_*.cpp "$qt_dir"/ui_*.h 2>/dev/null || true
}

# Configure the build
configure_build() {
    print_info "Configuring build..."

    cd "$REPO_ROOT"

    # Clean if requested
    if [ "$CLEAN_BUILD" = true ]; then
        if [ -f Makefile ]; then
            make distclean || true
        fi
    fi

    # Configure options
    local config_opts="--prefix=$DEPENDS_DIR/$HOST"

    # Always pass --host so configure enters cross-compile mode when needed.
    # Also pass --build to make the intent explicit and avoid mis-detection.
    local build_triplet=""
    if [ -x "$DEPENDS_DIR/config.guess" ]; then
        build_triplet="$("$DEPENDS_DIR/config.guess")"
    fi
    if [ -n "$build_triplet" ]; then
        config_opts="$config_opts --build=$build_triplet"
    fi
    config_opts="$config_opts --host=$HOST"

    if [ "$BUILD_GUI" = false ]; then
        config_opts="$config_opts --without-gui"
    else
        # Depends currently builds Qt6; make this explicit so configure can use qmake/.prl fallback.
        config_opts="$config_opts --with-gui=qt6"
    fi

    if [ "$BUILD_WALLET" = false ]; then
        config_opts="$config_opts --disable-wallet"
    fi

    if [ "$ENABLE_ZMQ" = true ]; then
        config_opts="$config_opts --with-zmq"
    else
        config_opts="$config_opts --without-zmq"
    fi

    if [ "$ENABLE_UPNP" = true ]; then
        config_opts="$config_opts --with-miniupnpc"
    else
        config_opts="$config_opts --without-miniupnpc"
    fi

    config_opts="$config_opts --disable-tests --disable-bench"

    if [ "$DEBUG_BUILD" = true ]; then
        config_opts="$config_opts --enable-debug"
    fi

    local config_site="$DEPENDS_DIR/$HOST/share/config.site"
    if [ "$USE_CONFIG_SITE" = true ] && [ -f "$config_site" ]; then
        print_info "Using CONFIG_SITE: $config_site"
        print_info "Running: CONFIG_SITE=... ./configure $config_opts"
        setup_native_qt_tools
        CONFIG_SITE="$config_site" ./configure $config_opts
    else
        if [ "$USE_CONFIG_SITE" = true ]; then
            print_warning "No depends config.site found at: $config_site"
        else
            print_info "CONFIG_SITE disabled via --no-config-site"
        fi
        print_info "Running: ./configure $config_opts"
        setup_native_qt_tools
        ./configure $config_opts
    fi

    # If GUI was requested, ensure configure actually enabled it.
    if [ "$BUILD_GUI" = true ] && [ -f "$REPO_ROOT/config.log" ]; then
        local gui_line
        gui_line="$(grep -A1 "checking whether to build CTEAM GUI" "$REPO_ROOT/config.log" | tail -n 1 || true)"
        if echo "$gui_line" | grep -q "result: no"; then
            print_error "Qt GUI was requested but configure disabled it ($gui_line)."
            print_error "Try: ./build-depends.sh --clean --jobs ${JOBS:-<n>} (or pass --no-gui for daemon-only)."
            exit 1
        fi
    fi

    print_success "Configuration complete"
}

# Build CTEAM
build_pivx() {
    print_info "Building CTEAM..."

    cd "$REPO_ROOT"

    # Some build targets write marker files under "$top_srcdir/.cargo/".
    # Ensure the directory exists (it can be missing if the tree was synced
    # without dot-directories or after aggressive cleanups).
    mkdir -p "$REPO_ROOT/.cargo"

    # Determine number of jobs
    local jobs_opt=""
    if [ -n "$JOBS" ]; then
        jobs_opt="-j$JOBS"
    else
        local cpu_count=""
        if [ "$OS" = "macOS" ]; then
            cpu_count="$(sysctl -n hw.ncpu 2>/dev/null || true)"
            if [ -z "$cpu_count" ]; then
                cpu_count="$(getconf _NPROCESSORS_ONLN 2>/dev/null || true)"
            fi
        else
            cpu_count="$(nproc 2>/dev/null || true)"
            if [ -z "$cpu_count" ]; then
                cpu_count="$(getconf _NPROCESSORS_ONLN 2>/dev/null || true)"
            fi
        fi
        cpu_count="${cpu_count:-4}"
        jobs_opt="-j$cpu_count"
    fi

    setup_native_qt_tools
    clean_stale_qt_generated_files

    local src_targets=(cteamd cteam-cli cteam-tx)
    if [ "$BUILD_GUI" = true ]; then
        src_targets+=(qt/cteam-qt)
    fi

    make -C src $jobs_opt "${src_targets[@]}"

    print_success "Build complete!"
}

# Print summary
print_summary() {
    echo ""
    print_success "=== Build Summary ==="
    echo "Build type:    $([ "$DEBUG_BUILD" = true ] && echo "Debug" || echo "Release")"
    local gui_built=false
    if [ -f "$REPO_ROOT/src/qt/cteam-qt" ]; then
        gui_built=true
    fi
    if [ "$BUILD_GUI" = true ] && [ "$gui_built" = false ]; then
        echo "GUI:           Requested (not built)"
    else
        echo "GUI:           $([ "$BUILD_GUI" = true ] && echo "Yes" || echo "No")"
    fi
    echo "Wallet:        $([ "$BUILD_WALLET" = true ] && echo "Yes" || echo "No")"
    echo "ZeroMQ:        $([ "$ENABLE_ZMQ" = true ] && echo "Yes" || echo "No")"
    echo "UPnP:          $([ "$ENABLE_UPNP" = true ] && echo "Yes" || echo "No")"
    echo ""
    echo "Binaries are located in: $REPO_ROOT/src"
    echo ""

    if [ -f "$REPO_ROOT/src/cteamd" ]; then
        echo "  src/cteamd        - CTEAM daemon"
    fi
    if [ -f "$REPO_ROOT/src/cteam-cli" ]; then
        echo "  src/cteam-cli     - CTEAM command-line client"
    fi
    if [ -f "$REPO_ROOT/src/cteam-tx" ]; then
        echo "  src/cteam-tx      - CTEAM transaction tool"
    fi
    if [ -f "$REPO_ROOT/src/qt/cteam-qt" ]; then
        echo "  src/qt/cteam-qt   - CTEAM Qt GUI wallet"
    fi
    echo ""
    echo "To install system-wide, run: sudo make install"
    echo ""

    if [ "$OS" = "macOS" ] && [ "$BUILD_GUI" = true ]; then
        echo "macOS .app/.dmg packaging:"
        echo "  make deploy"
        echo "  (This produces CTEAM.app and a .dmg; double-clicking the raw src/qt/cteam-qt binary will open Terminal.)"
        echo ""
    fi
}

# Main
main() {
    echo ""
    print_info "=== CTEAM Build Script (depends system) ==="
    echo ""

    cd "$REPO_ROOT"

    detect_platform
    setup_ccache

    if [ "$OS" = "Unknown" ]; then
        print_error "Unsupported operating system"
        exit 1
    fi

    # Handle --install-deps flag
    if [ "$INSTALL_DEPS_ONLY" = true ]; then
        install_dependencies
        print_success "Dependencies installed. You can now run: ./build-depends.sh"
        exit 0
    fi

    print_info "This script builds CTEAM with static dependencies."
    print_info "First build will take longer as dependencies are compiled."
    echo ""

    check_requirements
    check_gui_dependencies
    build_depends
    distclean_tree
    run_autogen
    configure_build
    build_pivx
    print_summary
}

main
