Windows Build Guide
===================

Windows builds require cross-compilation from Linux or WSL2.

Requirements
------------

- Windows 10/11 64-bit
- WSL2 with Ubuntu 22.04+
- 50GB+ free disk space

Setup WSL2
----------

```powershell
# In Administrator PowerShell
wsl --install -d Ubuntu-22.04
```

Restart, then open Ubuntu terminal and complete setup.

Install Dependencies
--------------------

```bash
sudo apt-get update
sudo apt-get install -y build-essential git autoconf automake libtool \
    pkg-config curl g++-mingw-w64-x86-64 nsis

# Set mingw to posix threading
sudo update-alternatives --config x86_64-w64-mingw32-g++
# Select the posix option
sudo update-alternatives --config x86_64-w64-mingw32-gcc
# Select the posix option

# Install Rust
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
source ~/.cargo/env
```

Build
-----

```bash
git clone <repository-url>
cd OrganicLife

# Strip Windows PATH to avoid issues
PATH=$(echo "$PATH" | sed -e 's/:\/mnt.*//g')

# Disable WSL Win32 support during build
sudo bash -c "echo 0 > /proc/sys/fs/binfmt_misc/status"

# Build dependencies
make -C depends -j$(nproc) HOST=x86_64-w64-mingw32

# Configure and build
./autogen.sh
CONFIG_SITE=$PWD/depends/x86_64-w64-mingw32/share/config.site \
    ./configure --prefix=/ --disable-online-rust
make -j$(nproc)

# Re-enable WSL Win32 support
sudo bash -c "echo 1 > /proc/sys/fs/binfmt_misc/status"
```

Output
------

Windows executables in `src/`:
- `src/organiclifed.exe`
- `src/organiclife-cli.exe`
- `src/organiclife-tx.exe`
- `src/qt/organiclife-qt.exe`

Create Installer
----------------

```bash
make deploy
```

Creates `organiclife-*-win64-setup.exe` in the root directory.

Install to Windows Directory
----------------------------

```bash
make install DESTDIR=/mnt/c/organiclife
```

Important Notes
---------------

1. **Source location**: Must be in WSL filesystem (`/home/user/...`), NOT `/mnt/c/...`
2. **PATH**: Windows PATH causes issues - strip it before building
3. **Binfmt**: Disable WSL Win32 support during build to prevent autoconf issues
4. **Persistence**: Add `source ~/.cargo/env` to `~/.bashrc`

Troubleshooting
---------------

**"cannot find -l..." errors**
```bash
# Ensure you're using posix mingw
sudo update-alternatives --config x86_64-w64-mingw32-g++
```

**"cargo: command not found"**
```bash
source ~/.cargo/env
```

**Configure hangs or weird errors**
```bash
# Ensure Win32 binfmt is disabled
sudo bash -c "echo 0 > /proc/sys/fs/binfmt_misc/status"
```

**Out of memory**
```bash
make -j2  # Reduce parallel jobs
```

Alternative: MSYS2 Native Build
-------------------------------

For native Windows build without cross-compilation:

1. Install MSYS2 from https://www.msys2.org/
2. Open UCRT64 shell
3. Install dependencies:

```bash
pacman -Syu
pacman -S --needed git base-devel autoconf automake libtool make pkgconf \
    mingw-w64-ucrt-x86_64-toolchain \
    mingw-w64-ucrt-x86_64-qt6-base mingw-w64-ucrt-x86_64-qt6-tools \
    mingw-w64-ucrt-x86_64-qt6-svg mingw-w64-ucrt-x86_64-boost \
    mingw-w64-ucrt-x86_64-libevent mingw-w64-ucrt-x86_64-libsodium \
    mingw-w64-ucrt-x86_64-gmp mingw-w64-ucrt-x86_64-qrencode
```

4. Build:

```bash
git clone <repository-url>
cd OrganicLife
./autogen.sh
./configure --with-gui=qt6
make -j$(nproc)
```

Note: MSYS2 doesn't have Berkeley DB 4.8 - use cross-compile method for wallet-compatible builds, or add `--disable-wallet`.
