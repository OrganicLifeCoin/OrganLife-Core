# Build: Qt6 via `depends/` (static)

This repo can build the Qt GUI wallet with **Qt 6.x statically linked** by building Qt inside `depends/`.

## macOS (Apple Silicon / Intel)

Prereqs (Homebrew examples): `automake`, `libtool`, `pkg-config`, `cmake`, `python@3`.

```bash
cd /path/to/OrganicLife

./autogen.sh

HOST="$(./depends/config.guess)"
make -C depends -j"$(sysctl -n hw.ncpu)"

mkdir -p build-qt6 && cd build-qt6
CONFIG_SITE="$PWD/../depends/$HOST/share/config.site" ../configure --with-gui=qt6
make -j"$(sysctl -n hw.ncpu)" organiclife-qt
```

The resulting binary is `build-qt6/src/qt/organiclife-qt`.

## Linux (Ubuntu 22.04+)

Install typical build deps (package names vary by distro): `build-essential`, `autoconf`, `automake`, `libtool`, `pkg-config`, `cmake`, `python3`, `curl`, `git`.

```bash
cd /path/to/OrganicLife

./autogen.sh

HOST="$(./depends/config.guess)"
make -C depends -j"$(nproc)"

mkdir -p build-qt6 && cd build-qt6
CONFIG_SITE="$PWD/../depends/$HOST/share/config.site" ../configure --with-gui=qt6
make -j"$(nproc)" organiclife-qt
```

## Windows (recommended: cross-compile from Ubuntu 22.04)

This produces a Windows `.exe` using the MinGW toolchain.

```bash
cd /path/to/OrganicLife

./autogen.sh

HOST="x86_64-w64-mingw32"
make -C depends HOST="$HOST" -j"$(nproc)"

mkdir -p build-win64 && cd build-win64
CONFIG_SITE="$PWD/../depends/$HOST/share/config.site" ../configure --host="$HOST" --with-gui=qt6
make -j"$(nproc)" organiclife-qt.exe
```

If your system lacks the MinGW compilers (`x86_64-w64-mingw32-gcc/g++`), install your distro’s `mingw-w64` packages first.
