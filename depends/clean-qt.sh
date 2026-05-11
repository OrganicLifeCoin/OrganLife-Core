#!/bin/bash
# Clean Qt packages from depends for a specific host

set -e

HOST="${1:-aarch64-apple-darwin}"

echo "Cleaning Qt packages for host: $HOST"

# Remove cached built packages
rm -rf built/*/qt-* built/*/native_qt-* 2>/dev/null || true

# Remove work directories
rm -rf work/build/$HOST/qt-* work/build/$HOST/native_qt-* 2>/dev/null || true
rm -rf work/staging/$HOST/qt-* work/staging/$HOST/native_qt-* 2>/dev/null || true

# Remove from host installation directory
rm -rf $HOST/lib/libQt* $HOST/lib/cmake/Qt* $HOST/include/Qt* 2>/dev/null || true
rm -rf $HOST/plugins 2>/dev/null || true

# Remove qmake and Qt tools
rm -f $HOST/native/bin/qmake* $HOST/native/bin/moc* $HOST/native/bin/rcc* $HOST/native/bin/uic* 2>/dev/null || true
rm -f $HOST/bin/qmake* $HOST/bin/moc* $HOST/bin/rcc* $HOST/bin/uic* 2>/dev/null || true

echo "Qt cleaned. Now rebuild with:"
echo "  make HOST=$HOST -j\$(nproc)"
