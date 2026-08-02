#!/usr/bin/env bash
#
# OrganicLife Easy Build Script
# Builds OrganicLife using vcpkg for dependency management
#
# Usage: ./build.sh [options]
#
# Options:
#   --install-deps    Install required dependencies (auto-detect OS)
#   --no-gui          Build without Qt GUI (daemon only)
#   --daemon-only     Build only organiclifed (skips bench/tests)
#   --no-wallet       Build without wallet support
#   --with-zmq        Enable ZeroMQ notifications
#   --with-upnp       Enable UPnP port mapping
#   --with-natpmp     Enable NAT-PMP port mapping
#   --debug           Build with debug symbols
#   --release         Build optimized release (default)
#   --clean           Clean build directory before building
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

# Script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
VCPKG_DIR="${SCRIPT_DIR}/vcpkg"

# Default options
BUILD_TYPE="Release"
BUILD_GUI=true
BUILD_WALLET=true
DAEMON_ONLY=false
ENABLE_ZMQ=false
ENABLE_UPNP=false
ENABLE_NATPMP=false
CLEAN_BUILD=false
INSTALL_DEPS_ONLY=false
JOBS=""

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

# Show help
show_help() {
    head -n 21 "$0" | tail -n 18 | sed 's/^#//' | sed 's/^ //'
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
        --daemon-only)
            BUILD_GUI=false
            DAEMON_ONLY=true
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
        --with-natpmp)
            ENABLE_NATPMP=true
            shift
            ;;
        --debug)
            BUILD_TYPE="Debug"
            shift
            ;;
        --release)
            BUILD_TYPE="Release"
            shift
            ;;
        --clean)
            CLEAN_BUILD=true
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

# Detect OS
detect_os() {
    case "$(uname -s)" in
        Linux*)     OS="Linux";;
        Darwin*)    OS="macOS";;
        *)          OS="Unknown";;
    esac
    print_info "Detected OS: $OS"
}

# Normalize unsupported combinations for this build path
normalize_options() {
    # The CMake build currently always includes wallet sources and requires BerkeleyDB 4.8.
    if [ "$BUILD_WALLET" = false ]; then
        print_warning "--no-wallet is not supported by the CMake build in this repo; building with wallet enabled."
        BUILD_WALLET=true
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

# Check for required tools
check_requirements() {
    print_info "Checking requirements..."

    local missing=()

    # Check git
    if ! command -v git &> /dev/null; then
        missing+=("git")
    fi

    # Check cmake
    if ! command -v cmake &> /dev/null; then
        missing+=("cmake")
    fi

    # Check Rust/Cargo
    if ! command -v cargo &> /dev/null; then
        # Try to source cargo env in case it's installed but not in PATH
        if [ -f "$HOME/.cargo/env" ]; then
            source "$HOME/.cargo/env"
        fi
        if ! command -v cargo &> /dev/null; then
            missing+=("cargo")
        fi
    fi

    # Check C++ compiler
    if ! command -v g++ &> /dev/null && ! command -v clang++ &> /dev/null; then
        missing+=("C++ compiler")
    fi

    # Check pkg-config
    if ! command -v pkg-config &> /dev/null; then
        missing+=("pkg-config")
    fi

    # Check autoconf/automake (needed for autogen.sh)
    if ! command -v autoconf &> /dev/null; then
        missing+=("autoconf")
    fi

    if [ ${#missing[@]} -gt 0 ]; then
        print_error "Missing required tools:"
        for tool in "${missing[@]}"; do
            echo "  - $tool"
        done
        echo ""

        # Ask user if they want to install automatically
        if confirm "Would you like to install missing dependencies automatically?"; then
            install_platform_deps
            install_rust
            # Re-check after installation
            print_info "Re-checking requirements after installation..."
            check_requirements
            return
        fi

        echo ""
        print_info "Manual installation instructions:"
        echo ""
        if [ "$OS" = "Linux" ]; then
            print_info "On Ubuntu/Debian, install with:"
            echo "  sudo apt-get update"
            echo "  sudo apt-get install build-essential git cmake pkg-config curl autoconf automake libtool"
        elif [ "$OS" = "macOS" ]; then
            print_info "On macOS, install with:"
            echo "  xcode-select --install"
            echo "  brew install cmake pkg-config autoconf automake libtool"
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
    local x11_packages=""

    # Check for key XCB/X11 libraries needed by Qt6
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
            echo "    ./build.sh --no-gui"
            echo ""

            if confirm "Would you like to install the missing X11 packages automatically?"; then
                print_info "Installing X11 dependencies..."
                if command -v apt-get &> /dev/null; then
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
                elif command -v dnf &> /dev/null; then
                    sudo dnf install -y \
                        libX11-devel libXext-devel libSM-devel libICE-devel \
                        libxcb-devel libxkbcommon-devel libxkbcommon-x11-devel \
                        fontconfig-devel freetype-devel libXfixes-devel libXi-devel \
                        libXrender-devel mesa-libGL-devel mesa-libGLU-devel \
                        dbus-devel at-spi2-core-devel
                elif command -v pacman &> /dev/null; then
                    sudo pacman -S --needed \
                        libx11 libxext libsm libice libxcb libxkbcommon \
                        libxkbcommon-x11 fontconfig freetype2 libxfixes libxi \
                        libxrender mesa glu dbus at-spi2-core
                else
                    print_error "Could not auto-install. Please install manually using the commands above."
                    exit 1
                fi
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

# Install platform-specific dependencies
install_platform_deps() {
    if [ "$OS" = "macOS" ]; then
        install_macos_deps
    elif [ "$OS" = "Linux" ]; then
        install_linux_deps
    fi
}

# Install macOS-specific dependencies via Homebrew
install_macos_deps() {
    print_info "Installing macOS build dependencies..."

    # Check for Homebrew
    if ! command -v brew &> /dev/null; then
        print_error "Homebrew is required on macOS but not installed."
        echo "Install it with:"
        echo '  /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"'
        exit 1
    fi

    # All required Homebrew packages for vcpkg builds
    local brew_packages=(
        autoconf
        automake
        libtool
        autoconf-archive
        pkg-config
        cmake
        ninja
        python3
    )

    # Berkeley DB 4.8 is required for wallet support (not available in vcpkg for macOS)
    if [ "$BUILD_WALLET" = true ]; then
        brew_packages+=("berkeley-db@4")
    fi

    print_info "Checking/installing Homebrew packages..."
    local to_install=()
    for pkg in "${brew_packages[@]}"; do
        if ! brew list "$pkg" &> /dev/null 2>&1; then
            to_install+=("$pkg")
        fi
    done

    if [ ${#to_install[@]} -gt 0 ]; then
        print_info "Installing: ${to_install[*]}"
        brew install "${to_install[@]}"
    fi

    print_success "macOS dependencies ready"
}

# Install Linux-specific dependencies
install_linux_deps() {
    print_info "Checking Linux build dependencies..."

    # Detect package manager and check for required packages
    local missing_pkgs=()

    if command -v apt-get &> /dev/null; then
        # Debian/Ubuntu
        local apt_packages=(
            build-essential
            autoconf
            automake
            libtool
            autoconf-archive
            pkg-config
            cmake
            ninja-build
            python3
            curl
            git
        )

        for pkg in "${apt_packages[@]}"; do
            if ! dpkg -s "$pkg" &> /dev/null 2>&1; then
                missing_pkgs+=("$pkg")
            fi
        done

        if [ ${#missing_pkgs[@]} -gt 0 ]; then
            print_info "Installing missing packages: ${missing_pkgs[*]}"
            sudo apt-get update
            sudo apt-get install -y "${missing_pkgs[@]}"
        fi

    elif command -v dnf &> /dev/null; then
        # Fedora/RHEL
        local dnf_packages=(
            gcc-c++
            autoconf
            automake
            libtool
            autoconf-archive
            pkgconfig
            cmake
            ninja-build
            python3
            curl
            git
        )

        for pkg in "${dnf_packages[@]}"; do
            if ! rpm -q "$pkg" &> /dev/null 2>&1; then
                missing_pkgs+=("$pkg")
            fi
        done

        if [ ${#missing_pkgs[@]} -gt 0 ]; then
            print_info "Installing missing packages: ${missing_pkgs[*]}"
            sudo dnf install -y "${missing_pkgs[@]}"
        fi

    elif command -v pacman &> /dev/null; then
        # Arch Linux
        local pacman_packages=(
            base-devel
            autoconf
            automake
            libtool
            autoconf-archive
            pkgconf
            cmake
            ninja
            python
            curl
            git
        )

        for pkg in "${pacman_packages[@]}"; do
            if ! pacman -Qi "$pkg" &> /dev/null 2>&1; then
                missing_pkgs+=("$pkg")
            fi
        done

        if [ ${#missing_pkgs[@]} -gt 0 ]; then
            print_info "Installing missing packages: ${missing_pkgs[*]}"
            sudo pacman -S --noconfirm "${missing_pkgs[@]}"
        fi
    else
        print_warning "Unknown package manager. Please ensure these packages are installed:"
        echo "  - build-essential/gcc-c++, autoconf, automake, libtool"
        echo "  - autoconf-archive, pkg-config, cmake, ninja, python3"
    fi

    print_success "Linux dependencies ready"
}

# Setup vcpkg
setup_vcpkg() {
    print_info "Setting up vcpkg..."

    if [ ! -d "$VCPKG_DIR" ]; then
        print_info "Cloning vcpkg..."
        git clone https://github.com/microsoft/vcpkg.git "$VCPKG_DIR"
    fi

    if [ ! -x "$VCPKG_DIR/vcpkg" ]; then
        print_info "Bootstrapping vcpkg..."
        "$VCPKG_DIR/bootstrap-vcpkg.sh" -disableMetrics
    fi

    print_success "vcpkg is ready"
}

# Install dependencies
install_dependencies() {
    print_info "Installing dependencies via vcpkg..."

    # Determine triplet
    local triplet
    if [ "$OS" = "macOS" ]; then
        if [ "$(uname -m)" = "arm64" ]; then
            triplet="arm64-osx"
        else
            triplet="x64-osx"
        fi
    else
        triplet="x64-linux"
    fi

    print_info "Using triplet: $triplet"

    local vcpkg_args=(
        --triplet="$triplet"
        --x-manifest-root="$SCRIPT_DIR"
        --x-install-root="$VCPKG_DIR/installed"
    )

    # If GUI is disabled, ensure we don't install the manifest default features.
    if [ "$BUILD_GUI" = false ]; then
        vcpkg_args+=(--x-no-default-features)
    else
        # Explicitly request the GUI feature (works whether or not it's a default feature).
        vcpkg_args+=(--x-feature=gui)
    fi

    if [ "$ENABLE_ZMQ" = true ]; then
        vcpkg_args+=(--x-feature=zmq)
    fi
    if [ "$ENABLE_UPNP" = true ]; then
        vcpkg_args+=(--x-feature=upnp)
    fi
    if [ "$ENABLE_NATPMP" = true ]; then
        vcpkg_args+=(--x-feature=natpmp)
    fi

    "$VCPKG_DIR/vcpkg" install "${vcpkg_args[@]}"

    print_success "Dependencies installed"
}

# Run autogen and configure (required for pivx-config.h)
run_autotools_config() {
    print_info "Running autogen.sh to generate configure script..."

    if [ ! -f "$SCRIPT_DIR/configure" ]; then
        if [ -f "$SCRIPT_DIR/autogen.sh" ]; then
            cd "$SCRIPT_DIR"
            ./autogen.sh
        else
            print_error "autogen.sh not found"
            exit 1
        fi
    fi

    print_success "Configure script ready"
}

# Configure with CMake
configure_cmake() {
    print_info "Configuring with CMake..."

    # Clean build directory if requested
    if [ "$CLEAN_BUILD" = true ] && [ -d "$BUILD_DIR" ]; then
        print_info "Cleaning build directory..."
        rm -rf "$BUILD_DIR"
    fi

    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"

    # Determine triplet for vcpkg
    local triplet
    if [ "$OS" = "macOS" ]; then
        if [ "$(uname -m)" = "arm64" ]; then
            triplet="arm64-osx"
        else
            triplet="x64-osx"
        fi
    else
        triplet="x64-linux"
    fi

    local vcpkg_installed="$VCPKG_DIR/installed/$triplet"

    # Boost 1.69+ made boost_system header-only, but configure expects a library
    # Create an empty stub library if it doesn't exist
    if [ ! -f "$vcpkg_installed/lib/libboost_system.a" ]; then
        print_info "Creating stub libboost_system.a (header-only in modern Boost)..."
        echo "void boost_system_stub() {}" > /tmp/boost_system_stub.cpp
        c++ -c -o /tmp/boost_system_stub.o /tmp/boost_system_stub.cpp 2>/dev/null || true
        ar rcs "$vcpkg_installed/lib/libboost_system.a" /tmp/boost_system_stub.o 2>/dev/null || true
        rm -f /tmp/boost_system_stub.o /tmp/boost_system_stub.cpp
    fi

    # Remove old pivx-config.h if clean build to force reconfigure
    if [ "$CLEAN_BUILD" = true ]; then
        rm -f "$SCRIPT_DIR/src/config/pivx-config.h"
    fi

    # Set environment variables so ./configure (run by CMakeLists.txt) can find vcpkg libs
    export BOOST_ROOT="$vcpkg_installed"
    export BOOSTROOT="$vcpkg_installed"
    export CPPFLAGS="-I$vcpkg_installed/include ${CPPFLAGS:-}"
    export LDFLAGS="-L$vcpkg_installed/lib ${LDFLAGS:-}"
    export PKG_CONFIG_PATH="$vcpkg_installed/lib/pkgconfig:${PKG_CONFIG_PATH:-}"

    # For Boost specifically - tell configure where to find it
    export BOOST_CPPFLAGS="-I$vcpkg_installed/include"
    export BOOST_LDFLAGS="-L$vcpkg_installed/lib"

    # GMP, Sodium, libevent paths
    export GMP_CFLAGS="-I$vcpkg_installed/include"
    export GMP_LIBS="-L$vcpkg_installed/lib -lgmp"
    export SODIUM_CFLAGS="-I$vcpkg_installed/include"
    export SODIUM_LIBS="-L$vcpkg_installed/lib -lsodium"
    export EVENT_CFLAGS="-I$vcpkg_installed/include"
    export EVENT_LIBS="-L$vcpkg_installed/lib -levent"
    export EVENT_PTHREADS_CFLAGS="-I$vcpkg_installed/include"
    export EVENT_PTHREADS_LIBS="-L$vcpkg_installed/lib -levent_pthreads"

    print_info "vcpkg paths configured for autotools"

    # Avoid FetchContent cloning RELIC during chiabls build when a vendored copy exists.
    # (chiabls/src/CMakeLists.txt honors RELIC_SOURCE_DIR if it points to a valid CMake project.)
    if [ -z "${RELIC_SOURCE_DIR:-}" ] && [ -f "$SCRIPT_DIR/src/chiabls/contrib/relic/CMakeLists.txt" ]; then
        export RELIC_SOURCE_DIR="$SCRIPT_DIR/src/chiabls/contrib/relic"
        print_info "Using vendored RELIC from: $RELIC_SOURCE_DIR"
    fi

    # CMake options
    # Note: Use RelWithDebInfo instead of Release because PIVX requires assertions
    # (validation.cpp has #error if NDEBUG is defined)
    local actual_build_type="$BUILD_TYPE"
    if [ "$BUILD_TYPE" = "Release" ]; then
        actual_build_type="RelWithDebInfo"
    fi

    # Platform-specific compiler flags
    # -UNDEBUG: PIVX requires assertions (validation.cpp has #error if NDEBUG is defined)
    local extra_cxx_flags="-UNDEBUG"

    local cmake_opts=(
        "-G"
        "Ninja"
        "-DCMAKE_BUILD_TYPE=$actual_build_type"
        "-DCMAKE_TOOLCHAIN_FILE=$VCPKG_DIR/scripts/buildsystems/vcpkg.cmake"
        "-DVCPKG_TARGET_TRIPLET=$triplet"
        "-DVCPKG_INSTALLED_DIR=$VCPKG_DIR/installed"
        "-DCMAKE_PREFIX_PATH=$vcpkg_installed"
        "-DBOOST_R=--with-boost=$vcpkg_installed"
        "-DCMAKE_POLICY_VERSION_MINIMUM=3.5"
        "-DCMAKE_CXX_FLAGS=$extra_cxx_flags"
        "-DCMAKE_C_FLAGS=$extra_cxx_flags"
        # PIVX requires assertions - override default flags to remove -DNDEBUG
        "-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=-O2 -g"
        "-DCMAKE_CXX_FLAGS_RELEASE=-O3"
        "-DCMAKE_C_FLAGS_RELWITHDEBINFO=-O2 -g"
        "-DCMAKE_C_FLAGS_RELEASE=-O3"
    )

    # Add NO_QT environment variable if not building GUI
    if [ "$BUILD_GUI" = false ]; then
        export NO_QT=1
    fi

    # Pass extra args to the autotools ./configure step that CMakeLists.txt runs to generate pivx-config.h.
    # (This ensures the generated config matches the selected build options.)
    local extra_configure_args=()
    if [ "$BUILD_GUI" = false ]; then
        extra_configure_args+=("--without-gui")
    else
        # Ensure the generated config header matches the CMake/vcpkg Qt6 build and
        # doesn't accidentally pick up a system Qt5 installation.
        extra_configure_args+=("--with-gui=qt6")
        extra_configure_args+=("--with-qt-bindir=$vcpkg_installed/tools/qt6/bin")
    fi
    if [ "$ENABLE_ZMQ" = true ]; then
        extra_configure_args+=("--with-zmq")
    else
        extra_configure_args+=("--without-zmq")
    fi
    if [ "$ENABLE_UPNP" = true ]; then
        extra_configure_args+=("--with-miniupnpc")
    else
        extra_configure_args+=("--without-miniupnpc")
    fi
    if [ "$ENABLE_NATPMP" = true ]; then
        extra_configure_args+=("--with-natpmp")
    else
        extra_configure_args+=("--without-natpmp")
    fi
    export EXTRA_CONFIGURE_ARGS="${extra_configure_args[*]}"

    # Ensure vcpkg manifest install (triggered by the toolchain) matches our selected features.
    # CMake lists are semicolon-separated; keep this as a single argument.
    local manifest_features=()
    if [ "$BUILD_GUI" = true ]; then
        manifest_features+=("gui")
    fi
    if [ "$ENABLE_ZMQ" = true ]; then
        manifest_features+=("zmq")
    fi
    if [ "$ENABLE_UPNP" = true ]; then
        manifest_features+=("upnp")
    fi
    if [ "$ENABLE_NATPMP" = true ]; then
        manifest_features+=("natpmp")
    fi

    if [ "${#manifest_features[@]}" -gt 0 ]; then
        local joined_features
        joined_features="$(IFS=';'; echo "${manifest_features[*]}")"
        cmake_opts+=("-DVCPKG_MANIFEST_FEATURES=$joined_features")
    fi
    if [ "$BUILD_GUI" = false ]; then
        cmake_opts+=("-DVCPKG_MANIFEST_NO_DEFAULT_FEATURES=ON")
    fi

    cmake "${cmake_opts[@]}" "$SCRIPT_DIR"

    print_success "CMake configuration complete"
}

# Build
build_project() {
    print_info "Building OrganicLife..."

    cd "$BUILD_DIR"

    # Determine number of jobs
    local jobs_arg=""
    if [ -n "$JOBS" ]; then
        jobs_arg="-j$JOBS"
    else
        # Auto-detect (best-effort; fall back to 4)
        local ncpu=""
        if [ "$OS" = "macOS" ]; then
            ncpu="$(getconf _NPROCESSORS_ONLN 2>/dev/null || true)"
            if [ -z "$ncpu" ]; then
                ncpu="$(sysctl -n hw.ncpu 2>/dev/null || true)"
            fi
        else
            ncpu="$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || true)"
        fi
    if [ -z "$ncpu" ]; then
        ncpu=4
    fi
    jobs_arg="-j$ncpu"
    fi

    if [ "$DAEMON_ONLY" = true ]; then
        cmake --build . $jobs_arg --target organiclifed
    else
        cmake --build . $jobs_arg
    fi

    # Convenience: CMake places the Qt wallet under src/qt/. Copy it to the
    # build root so users can run `./build/organiclife-qt` like the other binaries.
    if [ "$DAEMON_ONLY" = false ] && [ "$BUILD_GUI" = true ]; then
        if [ -f "$BUILD_DIR/src/qt/organiclife-qt" ]; then
            cp -f "$BUILD_DIR/src/qt/organiclife-qt" "$BUILD_DIR/organiclife-qt"
            chmod +x "$BUILD_DIR/organiclife-qt" || true
        elif [ -f "$BUILD_DIR/src/qt/organiclife-qt.exe" ]; then
            cp -f "$BUILD_DIR/src/qt/organiclife-qt.exe" "$BUILD_DIR/organiclife-qt.exe"
        fi
    fi

    print_success "Build complete!"
}

# Print summary
print_summary() {
    echo ""
    print_success "=== Build Summary ==="
    echo "Build type:    $BUILD_TYPE"
    echo "GUI:           $([ "$BUILD_GUI" = true ] && echo "Yes" || echo "No")"
    echo "Daemon only:   $([ "$DAEMON_ONLY" = true ] && echo "Yes" || echo "No")"
    echo "Wallet:        $([ "$BUILD_WALLET" = true ] && echo "Yes" || echo "No")"
    echo "ZeroMQ:        $([ "$ENABLE_ZMQ" = true ] && echo "Yes" || echo "No")"
    echo "UPnP:          $([ "$ENABLE_UPNP" = true ] && echo "Yes" || echo "No")"
    echo "NAT-PMP:       $([ "$ENABLE_NATPMP" = true ] && echo "Yes" || echo "No")"
    echo ""
    echo "Binaries are located in: $BUILD_DIR"
    echo ""

    if [ -f "$BUILD_DIR/organiclifed" ]; then
        echo "  organiclifed        - OrganicLife daemon"
    fi
    if [ -f "$BUILD_DIR/organiclife-cli" ]; then
        echo "  organiclife-cli     - OrganicLife command-line client"
    fi
    if [ -f "$BUILD_DIR/organiclife-tx" ]; then
        echo "  organiclife-tx      - OrganicLife transaction tool"
    fi
    if [ -f "$BUILD_DIR/organiclife-qt" ]; then
        echo "  organiclife-qt      - OrganicLife Qt GUI wallet"
    elif [ -f "$BUILD_DIR/src/qt/organiclife-qt" ]; then
        echo "  src/qt/organiclife-qt - OrganicLife Qt GUI wallet"
    fi
    echo ""
}

# Full dependency installation (for --install-deps flag)
install_all_dependencies() {
    print_info "=== Installing All Dependencies ==="
    echo ""

    install_platform_deps
    install_rust
    setup_vcpkg

    echo ""
    print_success "All dependencies installed!"
    print_info "You can now run: ./build.sh"
    echo ""
}

# Main
main() {
    echo ""
    print_info "=== OrganicLife Build Script ==="
    echo ""

    detect_os
    normalize_options

    if [ "$OS" = "Unknown" ]; then
        print_error "Unsupported operating system"
        exit 1
    fi

    # Handle --install-deps flag
    if [ "$INSTALL_DEPS_ONLY" = true ]; then
        install_all_dependencies
        exit 0
    fi

    check_requirements
    check_gui_dependencies
    install_platform_deps
    setup_vcpkg
    install_dependencies
    run_autotools_config
    configure_cmake
    build_project
    print_summary
}

main
