# Genesis mining helper (Quark)

This folder contains a small helper used to mine new genesis blocks for this codebase (Quark PoW, v1 blocks).

## Build (example)

From the repo root:

```bash
cc -c -O2 -I./src -o contrib/genesis/blake.o   src/crypto/blake.c
cc -c -O2 -I./src -o contrib/genesis/bmw.o     src/crypto/bmw.c
cc -c -O2 -I./src -o contrib/genesis/groestl.o src/crypto/groestl.c
cc -c -O2 -I./src -o contrib/genesis/jh.o      src/crypto/jh.c
cc -c -O2 -I./src -o contrib/genesis/keccak.o  src/crypto/keccak.c
cc -c -O2 -I./src -o contrib/genesis/skein.o   src/crypto/skein.c

c++ -std=c++17 -O2 -I./src -o contrib/genesis/mine_genesis_quark \
  contrib/genesis/mine_genesis_quark.cpp src/crypto/sha256.cpp \
  src/arith_uint256.cpp src/uint256.cpp src/utilstrencodings.cpp \
  contrib/genesis/blake.o contrib/genesis/bmw.o contrib/genesis/groestl.o \
  contrib/genesis/jh.o contrib/genesis/keccak.o contrib/genesis/skein.o
```

## Run (example)

```bash
./contrib/genesis/mine_genesis_quark --time 1768953600 --bits 1e0ffff0
```
