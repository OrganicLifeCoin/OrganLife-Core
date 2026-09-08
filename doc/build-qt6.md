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

For Linux Qt tests, configure with `--enable-tests` and run
`QT_QPA_PLATFORM=offscreen src/qt/test/test_organiclife-qt` from the build directory.
Static Qt6 builds include the offscreen test plugin when its cached `.prl` is
available. Plain `minimal` has dummy fonts in builds without fontconfig, while
synthetic header drags cannot drive the XCB window manager's native move operation.
Keep real XCB rendering checks separate from these deterministic tests. Linux
startup loads an installed Noto, DejaVu or Liberation fallback only if its default
font cannot render basic text; already-readable fonts remain unchanged.

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

With `--enable-tests`, the static Qt6 test executable also links the cached
offscreen platform when available. In Windows PowerShell, run
`$env:QT_QPA_FONTDIR = "$env:WINDIR\Fonts"` followed by
`src\qt\test\test_organiclife-qt.exe -platform offscreen` for deterministic
widget tests. This FreeType backend needs an explicit installed-font directory;
it does not use the native Windows font lookup. Native window-manager drags and desktop-size constraints require
separate interactive checks with `-platform windows`; offscreen tests do not
qualify native window placement or rendering. Both runs must exit normally.
