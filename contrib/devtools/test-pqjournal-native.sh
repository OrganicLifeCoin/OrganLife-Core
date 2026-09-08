#!/usr/bin/env bash
# Build isolated journal checks with the existing Linux/MinGW depends prefix.
# No production fault hooks: GNU ld wraps the I/O calls only in the test binary.
set -euo pipefail
: "${PQ_DEPENDS:?Set PQ_DEPENDS to the target depends prefix}"
: "${CXX:=g++}"
cd "$(dirname "$0")/../.."
target="$($CXX -dumpmachine)"
flags=()
suffix=""
write_symbol=_ZN9pqjournal2io8WriteAllEiPKhm
case "$target" in
    x86_64-w64-mingw32*) flags+=(-DWIN32 -static); suffix=.exe; write_symbol=_ZN9pqjournal2io8WriteAllEiPKhy ;;
    x86_64*-linux*|aarch64*-linux*) ;;
    *) echo "Requires a 64-bit Linux or MinGW GNU-linker toolchain" >&2; exit 1 ;;
esac
filesystem=("$PQ_DEPENDS"/lib/libboost_filesystem*.a)
atomic=("$PQ_DEPENDS"/lib/libboost_atomic*.a)
test "${#filesystem[@]}" = 1 && test -f "${filesystem[0]}"
test "${#atomic[@]}" = 1 && test -f "${atomic[0]}"
libraries=("${filesystem[0]}" "${atomic[0]}" -pthread)
if test -n "$suffix"; then libraries+=(-lws2_32 -lbcrypt); fi
out="$(mktemp -d "${TMPDIR:-/tmp}/olc-journal-native.XXXXXX")"
common=(-std=c++17 -O1 -UNDEBUG -ffunction-sections -fdata-sections -Isrc "-I$PQ_DEPENDS/include")
"$CXX" "${common[@]}" "${flags[@]}" src/test/pqjournal_io_tests.cpp src/pqjournal_io.cpp \
    "${libraries[@]}" -o "$out/journal-io-check$suffix"
wraps=(-Wl,--gc-sections)
for symbol in _ZN9pqjournal2io4SyncEi \
    _ZN9pqjournal2io10SyncParentERKN5boost10filesystem4pathE \
    _ZN9pqjournal2io4OpenERKN5boost10filesystem4pathENS0_4ModeE \
    _ZN9pqjournal2io7ReplaceERKN5boost10filesystem4pathES5_ "$write_symbol"; do
    wraps+=("-Wl,--wrap=$symbol")
done
"$CXX" "${common[@]}" "${flags[@]}" src/test/pqjournal_fault_tests.cpp \
    src/pqjournal.cpp src/pqjournal_io.cpp src/pqquorum.cpp src/uint256.cpp \
    src/utilstrencodings.cpp src/crypto/sha256.cpp src/support/cleanse.cpp \
    "${wraps[@]}" "${libraries[@]}" -o "$out/journal-fault-check$suffix"
echo "Journal checks built in $out"
if test -z "$suffix"; then
    "$out/journal-io-check"
    "$out/journal-fault-check"
else
    echo "Run both .exe files on native Windows with a local NTFS TEMP directory."
fi
