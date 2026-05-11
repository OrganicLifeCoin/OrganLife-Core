Linux Build Guide
=================

Requirements
------------

- Ubuntu 22.04+ / Debian 12+ / Fedora 39+ / Arch Linux
- 8GB+ RAM recommended (16GB for parallel builds)
- 50GB+ free disk space

Ubuntu / Debian
---------------

Install prerequisites:

```bash
sudo apt-get update
sudo apt-get install -y build-essential git autoconf automake libtool pkg-config \
    python3 curl cmake ninja-build bsdmainutils

# Install Rust
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
source ~/.cargo/env
```

Build:

```bash
./build-depends.sh
```

Fedora
------

Install prerequisites:

```bash
sudo dnf install -y gcc-c++ git autoconf automake libtool pkgconfig \
    python3 curl cmake ninja-build

# Install Rust
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
source ~/.cargo/env
```

Build:

```bash
./build-depends.sh
```

Arch Linux
----------

Install prerequisites:

```bash
sudo pacman -S --needed base-devel git autoconf automake libtool pkgconf \
    cmake ninja python curl

# Install Rust
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
source ~/.cargo/env
```

Build:

```bash
./build-depends.sh
```

Build Options
-------------

```bash
./build-depends.sh --no-gui       # Daemon only (faster)
./build-depends.sh --debug        # Debug build
./build-depends.sh --clean        # Clean rebuild
./build-depends.sh --jobs 4       # Limit parallel jobs
```

Build Outputs
-------------

After `build-depends.sh`:
- `src/cteamd` - Daemon
- `src/cteam-cli` - CLI client
- `src/cteam-tx` - Transaction tool
- `src/qt/cteam-qt` - GUI wallet

Running
-------

```bash
# Start daemon
./src/cteamd -daemon

# Check status
./src/cteam-cli getblockchaininfo
./src/cteam-cli getbalance

# Stop daemon
./src/cteam-cli stop

# GUI wallet
./src/qt/cteam-qt
```

Data Directory
--------------

`~/.cteam/`

Systemd Service (Optional)
--------------------------

Create `/etc/systemd/system/cteamd.service`:

```ini
[Unit]
Description=CTEAM Daemon
After=network.target

[Service]
Type=forking
User=YOUR_USER
ExecStart=/path/to/cteamd -daemon
ExecStop=/path/to/cteam-cli stop
Restart=on-failure

[Install]
WantedBy=multi-user.target
```

Enable:
```bash
sudo systemctl enable cteamd
sudo systemctl start cteamd
sudo systemctl status cteamd
```

Troubleshooting
---------------

**"cargo: command not found"**
```bash
source ~/.cargo/env
# Add to .bashrc or .zshrc for persistence
echo 'source ~/.cargo/env' >> ~/.bashrc
```

**Out of memory**
```bash
./build-depends.sh --jobs 2
```

**Missing dependencies**
```bash
# Ubuntu/Debian
sudo apt-get install -y build-essential git autoconf automake libtool pkg-config

# Fedora
sudo dnf install -y gcc-c++ git autoconf automake libtool pkgconfig
```

**Clean rebuild**
```bash
./build-depends.sh --clean
```

Cross-Compilation
-----------------

To build for a different architecture or distro, use the depends system:

```bash
# Example: Build for x86_64 Linux
make -C depends -j$(nproc) HOST=x86_64-pc-linux-gnu
./autogen.sh
CONFIG_SITE=$PWD/depends/x86_64-pc-linux-gnu/share/config.site ./configure
make -j$(nproc)
```

WSL (Windows)
-------------

WSL2 is supported. Use Ubuntu 22.04+ in WSL2:

```bash
# In WSL2 terminal
sudo apt-get update
sudo apt-get install -y build-essential git autoconf automake libtool pkg-config \
    python3 curl cmake ninja-build

# Install Rust
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
source ~/.cargo/env

# Build
./build-depends.sh
```

GUI wallet requires an X server (WSLg on Windows 11, or VcXsrv on Windows 10).
