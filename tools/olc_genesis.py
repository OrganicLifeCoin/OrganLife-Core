#!/usr/bin/env python3
"""OrganicLife Coin (OLC) network parameter generator.

Replicates this codebase's exact genesis *transaction* serialization
(PIVX 5.x style: int16 nVersion + int16 nType) to:
  1. Validate the tx/merkle side against the existing CTEAM genesis blocks.
  2. Pick base58 prefixes with distinctive leading characters.
  3. Generate a fresh spork keypair (pure-python secp256k1).

WARNING: block-header hashing in this codebase is NOT sha256d.
CBlockHeader::GetHash() uses HashQuark() for nVersion < 4 (see
src/primitives/block.cpp), and the genesis blocks have nVersion=1.
The make_genesis()/validate functions below only reproduce the merkle
roots; the mined block hashes will NOT match the node. To mine or verify
genesis block hashes, use the C tool instead (byte-exact, links the
repo's own sphlib):

    cc -O2 -I src -I src/crypto tools/quark_miner.c \
       src/crypto/blake.c src/crypto/bmw.c src/crypto/groestl.c \
       src/crypto/jh.c src/crypto/keccak.c src/crypto/skein.c \
       src/crypto/aes_helper.c -o tools/quark_miner
    tools/quark_miner mine <merkle_hex> <ntime> <nbits_hex>
"""
import hashlib
import struct
import secrets
import sys

# ---------------------------------------------------------------- base58
B58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"

def sha256d(b: bytes) -> bytes:
    return hashlib.sha256(hashlib.sha256(b).digest()).digest()

def b58encode(b: bytes) -> str:
    n = int.from_bytes(b, "big")
    out = ""
    while n > 0:
        n, r = divmod(n, 58)
        out = B58[r] + out
    pad = 0
    for c in b:
        if c == 0:
            pad += 1
        else:
            break
    return "1" * pad + (out or "")

def b58check(version: bytes, payload: bytes) -> str:
    raw = version + payload
    return b58encode(raw + sha256d(raw)[:4])

# ------------------------------------------------------- script helpers
def push_data(data: bytes) -> bytes:
    n = len(data)
    if n < 0x4C:
        return bytes([n]) + data
    if n <= 0xFF:
        return b"\x4c" + bytes([n]) + data
    raise ValueError("too long")

def script_num(value: int) -> bytes:
    if value == 0:
        return b""
    neg = value < 0
    absvalue = -value if neg else value
    result = bytearray()
    while absvalue:
        result.append(absvalue & 0xFF)
        absvalue >>= 8
    if result[-1] & 0x80:
        result.append(0x80 if neg else 0)
    elif neg:
        result[-1] |= 0x80
    return bytes(result)

def build_script_sig(timestamp: bytes) -> bytes:
    # CScript() << 486604799 << CScriptNum(4) << pszTimestamp
    # NOTE: this codebase's operator<<(CScriptNum) pushes getvch() as data,
    # so CScriptNum(4) becomes push({0x04}) = 01 04, NOT OP_4 (0x54).
    return push_data(script_num(486604799)) + push_data(b"\x04") + push_data(timestamp)

def varint(n: int) -> bytes:
    if n < 0xFD:
        return bytes([n])
    if n <= 0xFFFF:
        return b"\xfd" + struct.pack("<H", n)
    if n <= 0xFFFFFFFF:
        return b"\xfe" + struct.pack("<I", n)
    return b"\xff" + struct.pack("<Q", n)

OP_CHECKSIG = b"\xac"
OP_TRUE = b"\x51"

# genesis output script used by this codebase (unspendable PIVX genesis key)
GENESIS_PUBKEY = bytes.fromhex(
    "04c10e83b2703ccf322f7dbd62dd5855ac7c10bd055814ce121ba32607d573b881"
    "0c02c0582aed05b4deb9c4b77b26d92428c61256cd42774babea0a073b2ed0c9")

def genesis_tx(timestamp: bytes, reward: int, out_script: bytes) -> bytes:
    tx = b""
    tx += struct.pack("<h", 1)          # nVersion (int16)
    tx += struct.pack("<h", 0)          # nType NORMAL (int16)
    tx += varint(1)                     # vin count
    tx += b"\x00" * 32                  # prevout hash
    tx += struct.pack("<I", 0xFFFFFFFF) # prevout n
    ss = build_script_sig(timestamp)
    tx += varint(len(ss)) + ss
    tx += struct.pack("<I", 0xFFFFFFFF) # nSequence
    tx += varint(1)                     # vout count
    tx += struct.pack("<q", reward)     # nValue
    tx += varint(len(out_script)) + out_script
    tx += struct.pack("<I", 0)          # nLockTime
    return tx

def genesis_header(merkle: bytes, ntime: int, nbits: int, nonce: int) -> bytes:
    h = struct.pack("<i", 1)            # nVersion
    h += b"\x00" * 32                   # hashPrevBlock
    h += merkle
    h += struct.pack("<III", ntime, nbits, nonce)
    return h

def bits_to_target(nbits: int) -> int:
    mantissa = nbits & 0xFFFFFF
    exponent = nbits >> 24
    return mantissa * (1 << (8 * (exponent - 3)))

def make_genesis(timestamp: bytes, ntime: int, nbits: int, reward: int = 0,
                 out_script: bytes | None = None, nonce_start: int = 0):
    if out_script is None:
        out_script = push_data(GENESIS_PUBKEY) + OP_CHECKSIG
    tx = genesis_tx(timestamp, reward, out_script)
    merkle = sha256d(tx)  # single tx => merkle root == txid
    target = bits_to_target(nbits)
    nonce = nonce_start
    while True:
        hdr = genesis_header(merkle, ntime, nbits, nonce)
        h = sha256d(hdr)
        if int.from_bytes(h, "little") <= target:
            return {
                "nonce": nonce, "time": ntime, "bits": nbits,
                "hash": h[::-1].hex(), "merkle": merkle[::-1].hex(),
                "tx": tx.hex(),
            }
        nonce += 1
        if nonce > 0xFFFFFFFF:
            raise RuntimeError("nonce space exhausted; change nTime")

# ------------------------------------------------------------ validation
def validate_against_cteam():
    checks = [
        # (name, timestamp, time, nonce, bits, expected_hash, expected_merkle)
        ("cteam-regtest", b"CTEAM Genesis 2026-05-05", 1778491634, 0, 0x207fffff,
         "33928ba611fd2bdc184827aec28969d5507114d4d3a7757d0ee2a292c6a23dcb",
         "c10e5c519df766e11290d700ce084d8c339bed1e56b068dade382784940c41bb"),
        ("cteam-testnet", b"CTEAM Testnet Genesis 2026-05-05", 1778491633, 839634, 0x1e0ffff0,
         "00000723b58e921e858251185dc07ad0c8fa2ffeb3dca130683e3794c28bceb5",
         "efa13ba9757a15d94955f1d6d35e3b6800d915f93c3c7b565f35ba66a9b09878"),
        ("cteam-mainnet", b"CTEAM Genesis 2026-07-27", 1785103200, 57002, 0x1e0ffff0,
         "00000af1784b2656d98a5052dfda8a2580835c7817bed4c0bb8294d36390fa62",
         "16f4d84a103eda63bca9d899579901214dd002c88b1b2bbdf74369bb8ba006cb"),
    ]
    ok = True
    for name, ts, ntime, nonce, nbits, ehash, emerkle in checks:
        tx = genesis_tx(ts, 0, push_data(GENESIS_PUBKEY) + OP_CHECKSIG)
        merkle = sha256d(tx)
        hdr = genesis_header(merkle, ntime, nbits, nonce)
        h = sha256d(hdr)
        got_hash, got_merkle = h[::-1].hex(), merkle[::-1].hex()
        match = got_hash == ehash and got_merkle == emerkle
        ok = ok and match
        print(f"  [{'OK ' if match else 'FAIL'}] {name}: hash={got_hash[:16]}... merkle={got_merkle[:16]}...")
        if not match:
            print(f"        expected hash={ehash[:16]}... merkle={emerkle[:16]}...")
    return ok

# ------------------------------------------------------- prefix picker
def leading_char(version: bytes, samples: int = 64) -> str | None:
    """First base58 char must be stable across random payloads."""
    chars = set()
    for i in range(samples):
        payload = bytes([i % 256]) * 20 if i == 0 else secrets.token_bytes(20)
        chars.add(b58check(version, payload)[0])
    return chars.pop() if len(chars) == 1 else None

def find_prefix(want: str, start: int = 1, end: int = 255):
    for v in range(start, end + 1):
        if leading_char(bytes([v])) == want:
            return v
    return None

# ------------------------------------------------------- secp256k1
P = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEFFFFFC2F
N = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141
G = (0x79BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798,
     0x483ADA7726A3C4655DA4FBFC0E1108A8FD17B448A68554199C47D08FFB10D4B8)

def ec_add(p, q):
    if p is None: return q
    if q is None: return p
    if p[0] == q[0] and (p[1] + q[1]) % P == 0: return None
    if p == q:
        lam = (3 * p[0] * p[0]) * pow(2 * p[1], P - 2, P) % P
    else:
        lam = (q[1] - p[1]) * pow(q[0] - p[0], P - 2, P) % P
    x = (lam * lam - p[0] - q[0]) % P
    return (x, (lam * (p[0] - x) - p[1]) % P)

def ec_mul(k: int, point=G):
    result = None
    addend = point
    while k:
        if k & 1:
            result = ec_add(result, addend)
        addend = ec_add(addend, addend)
        k >>= 1
    return result

def gen_keypair():
    d = secrets.randbelow(N - 1) + 1
    x, y = ec_mul(d)
    pub = b"\x04" + x.to_bytes(32, "big") + y.to_bytes(32, "big")
    return d, pub

def hash160(b: bytes) -> bytes:
    return hashlib.new("ripemd160", hashlib.sha256(b).digest()).digest()

def wif(priv: int, secret_prefix: int, compressed: bool = False) -> str:
    payload = priv.to_bytes(32, "big") + (b"\x01" if compressed else b"")
    return b58check(bytes([secret_prefix]), payload)

# ------------------------------------------------------------------ main
def main():
    print("== Step 1: validate serialization against existing CTEAM genesis blocks ==")
    if not validate_against_cteam():
        sys.exit("Serialization mismatch - aborting before touching anything.")

    print("\n== Step 2: address prefix selection (mainnet P2PKH wants leading 'o') ==")
    pkh_v = find_prefix("o")
    print(f"  PUBKEY_ADDRESS  version {pkh_v} -> addresses start with 'o'  e.g. {b58check(bytes([pkh_v]), hash160(b'organiclifecoin'))}")
    scr_v = find_prefix("g", 1, 255)  # 'g' for garden/grove script addresses
    print(f"  SCRIPT_ADDRESS  version {scr_v} -> addresses start with 'g'  e.g. {b58check(bytes([scr_v]), hash160(b'olc-script'))}")
    stake_v = find_prefix("f", 1, 255)  # 'f' for farm staking addresses
    print(f"  STAKING_ADDRESS version {stake_v} -> addresses start with 'f'  e.g. {b58check(bytes([stake_v]), hash160(b'olc-stake'))}")
    wif_v = find_prefix("K", 128, 255) or find_prefix("L", 128, 255) or 204
    print(f"  SECRET_KEY      version {wif_v} -> WIF starts with '{leading_char(bytes([wif_v]))}'")

    print("\n== Step 3: fresh spork keypairs ==")
    for net in ("mainnet", "testnet"):
        d, pub = gen_keypair()
        addr = b58check(bytes([pkh_v]), hash160(pub))
        print(f"  [{net}]")
        print(f"    pubkey : {pub.hex()}")
        print(f"    privkey: {d.to_bytes(32, 'big').hex()}")
        print(f"    WIF    : {wif(d, wif_v)}  (prefix {wif_v})")
        print(f"    address: {addr}")

    print("\n== Step 4: magic bytes ==")
    for net in ("mainnet", "testnet", "regtest"):
        mb = secrets.token_bytes(4)
        print(f"  {net}: " + " ".join(f"0x{b:02x}" for b in mb))

    print("\n== Step 5: mine OrganicLife genesis blocks ==")
    TS_MAIN = b"OrganicLife Coin Genesis 2026-08-02"
    TS_TEST = b"OrganicLife Coin Testnet Genesis 2026-08-02"
    TS_REG  = b"OrganicLife Coin Regtest Genesis 2026-08-02"
    NTIME_MAIN = 1785744000   # 2026-08-02 16:00:00 UTC
    NTIME_TEST = 1785743999
    NTIME_REG  = 1785743998

    print("  mining regtest (trivial target)...")
    g = make_genesis(TS_REG, NTIME_REG, 0x207fffff)
    print(f"    nonce={g['nonce']} hash={g['hash']}")
    print(f"    merkle={g['merkle']}")

    for label, ts, nt in (("testnet", TS_TEST, NTIME_TEST), ("mainnet", TS_MAIN, NTIME_MAIN)):
        print(f"  mining {label} (bits 0x1e0ffff0, this can take a minute)...", flush=True)
        g = make_genesis(ts, nt, 0x1E0FFFF0)
        print(f"    nonce={g['nonce']} hash={g['hash']}")
        print(f"    merkle={g['merkle']}")

if __name__ == "__main__":
    main()
