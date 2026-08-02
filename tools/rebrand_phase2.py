#!/usr/bin/env python3
"""OrganicLife Coin Phase 2 rebrand sweep.

Renames binaries/source files and rewrites CTEAM/pivx user-visible branding
to OrganicLife/OLC across the tree, with strict exclusions for generated,
vendored, binary and translation files.

Kept intentionally (internal/protocol lineage, not user-visible):
  - config/pivx-config.h (generated header name + its ~150 includes)
  - PIVX_* include guards, consensus.nPivxBadBlock* params, zpiv internals
  - copyright attribution lines for Bitcoin/Dash/PIVX (MIT requirement)
  - PIVX_QT_DIALOG_* dev-only env vars
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# ------------------------------------------------------------ file renames
RENAMES = [
    ("src/pivxd.cpp", "src/organiclifed.cpp"),
    ("src/pivx-cli.cpp", "src/organiclife-cli.cpp"),
    ("src/pivx-tx.cpp", "src/organiclife-tx.cpp"),
    ("src/pivxd-res.rc", "src/organiclifed-res.rc"),
    ("src/pivx-cli-res.rc", "src/organiclife-cli-res.rc"),
    ("src/pivx-tx-res.rc", "src/organiclife-tx-res.rc"),
    ("src/qt/pivxgui.h", "src/qt/organiclifegui.h"),
    ("src/qt/pivxgui.cpp", "src/qt/organiclifegui.cpp"),
    ("src/qt/pivx.qrc", "src/qt/organiclife.qrc"),
    ("src/qt/pivx_locale.qrc", "src/qt/organiclife_locale.qrc"),
    ("src/qt/res/pivx-qt-res.rc", "src/qt/res/organiclife-qt-res.rc"),
    ("src/test/test_pivx.h", "src/test/test_organiclife.h"),
    ("src/test/test_pivx.cpp", "src/test/test_organiclife.cpp"),
    ("contrib/cteam-cli.bash-completion", "contrib/organiclife-cli.bash-completion"),
    ("contrib/cteam-tx.bash-completion", "contrib/organiclife-tx.bash-completion"),
    ("contrib/cteamd.bash-completion", "contrib/organiclifed.bash-completion"),
    ("doc/man/cteam-cli.1", "doc/man/organiclife-cli.1"),
    ("doc/man/cteam-qt.1", "doc/man/organiclife-qt.1"),
    ("doc/man/cteam-tx.1", "doc/man/organiclife-tx.1"),
    ("doc/man/cteamd.1", "doc/man/organiclifed.1"),
]

# ------------------------------------------------------ replacement rules
# (pattern, replacement) applied in order to every swept text file.
PLACEHOLDER = "\x00ORGIMG{}\x00"
protected_images = []

RULES = [
    # --- specific phrases first
    ("The CTEAM Core developers", "The OrganicLife Coin developers"),
    ("CTEAM Core", "OrganicLife Core"),
    ("CTEAM.conf", "organiclifecoin.conf"),
    ("CTEAMParams", "OrganicLifeParams"),
    (".CTEAM-params", ".organiclifecoin-params"),
    ("CTEAM-testnet", "OrganicLife-testnet"),
    ("seed.CTEAM.cash", "seed.organiclife.network"),
    # --- ticker-ish units
    ("mcteam", "molc"),
    ("ucteam", "uolc"),
    ('QString("cteam")', 'QString("olc")'),
    # --- binaries (longest first)
    ("cteam-cli", "organiclife-cli"),
    ("cteam-tx", "organiclife-tx"),
    ("cteam-qt", "organiclife-qt"),
    ("cteamd", "organiclifed"),
    # --- urls
    ("github.com/cteam/cteam", "github.com/organiclifecoin/organiclifecoin"),
    # --- pivx build/gui names (longest first)
    ("moc_pivxgui", "moc_organiclifegui"),
    ("PIVXGUI", "OrganicLifeGUI"),
    ("pivxgui.h", "organiclifegui.h"),
    ("pivxgui.cpp", "organiclifegui.cpp"),
    ("pivx_locale.qrc", "organiclife_locale.qrc"),
    ("pivx-qt-res.rc", "organiclife-qt-res.rc"),
    ("pivx.qrc", "organiclife.qrc"),
    ("test_pivx-qt", "test_organiclife-qt"),
    ("test_pivx", "test_organiclife"),
    ("pivx-cli", "organiclife-cli"),
    ("pivx-tx", "organiclife-tx"),
    ("pivx-qt", "organiclife-qt"),
    ("pivxd", "organiclifed"),
    ("PIVXQt", "OrganicLifeQt"),
    ('"pivx-scriptch"', '"olc-scriptch"'),
    # --- PIVXGUI include line remnants handled by pivxgui rules above
]

# numeric-ticker rule for prose/docs: "4000 CTEAM" / "%1 CTEAM" -> OLC
TICKER_RE = re.compile(r'(?<=[0-9,`%]) CTEAM\b')

FINAL_RULES = [
    ("cteam", "organiclife"),
    ("CTEAM", "OrganicLife"),
]

SKIP_DIRS = {
    ".git", ".idea", ".cargo", "build-arm64", "build-x86_64", "build-logs",
    "dist", "dist-from-wsl", "autom4te.cache", "build-aux", "depends",
    "CTEAM-macos-universal-daemon", "CTEAM-macos-universal-qt",
    "obj", "obj-test", "build", "target",
    "secp256k1", "chiabls", "univalue", "leveldb", "crc32c",
    "node_modules", "__pycache__", "tools", "rust",
}
SKIP_FILES = {
    "configure", "configure~", "aclocal.m4", "Makefile.in",
    "pivx-config.h.in", "pivx-config.h.in~", ".DS_Store",
    "compile_commands.json",
}
SKIP_EXTS = {
    ".png", ".ico", ".icns", ".jpg", ".jpeg", ".webp", ".bmp", ".xpm",
    ".gif", ".svg", ".ts", ".po", ".mo", ".woff", ".woff2", ".ttf",
    ".a", ".o", ".so", ".dylib", ".dll", ".exe", ".dat", ".zip", ".gz",
}

PROTECT_SUBSTR = ["cteam.png", "cteam.ico", "cteam.icns", "CTEAM.icns", "CTEAMCoin.png"]


def protect(text):
    for i, s in enumerate(PROTECT_SUBSTR):
        text = text.replace(s, PLACEHOLDER.format(i))
    return text


def unprotect(text):
    for i, s in enumerate(PROTECT_SUBSTR):
        text = text.replace(PLACEHOLDER.format(i), s)
    return text


def sweep_file(path, stats):
    try:
        with open(path, "r", encoding="utf-8") as f:
            original = f.read()
    except (UnicodeDecodeError, ValueError):
        return
    text = protect(original)
    for pat, rep in RULES:
        if pat in text:
            stats[pat] += text.count(pat)
            text = text.replace(pat, rep)
    text, n_ticker = TICKER_RE.subn(" OLC", text)
    stats["<numeric-ticker>"] += n_ticker
    for pat, rep in FINAL_RULES:
        if pat in text:
            stats[pat] += text.count(pat)
            text = text.replace(pat, rep)
    text = unprotect(text)
    if text != original:
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)
        stats["<files-changed>"] += 1


def main():
    dry = "--dry-run" in sys.argv
    stats = {p: 0 for p, _ in RULES}
    stats.update({p: 0 for p, _ in FINAL_RULES})
    stats["<numeric-ticker>"] = 0
    stats["<files-changed>"] = 0

    for old, new in RENAMES:
        oldp, newp = os.path.join(ROOT, old), os.path.join(ROOT, new)
        if os.path.exists(oldp):
            if dry:
                print(f"mv {old} -> {new}")
            else:
                subprocess.run(["git", "mv", old, new], cwd=ROOT, check=True)
                print(f"renamed {old} -> {new}")
        elif not os.path.exists(newp):
            print(f"WARN: neither {old} nor {new} exists", file=sys.stderr)

    for dirpath, dirnames, filenames in os.walk(ROOT):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for fn in filenames:
            if fn in SKIP_FILES:
                continue
            ext = os.path.splitext(fn)[1].lower()
            if ext in SKIP_EXTS:
                continue
            p = os.path.join(dirpath, fn)
            if dry:
                continue
            sweep_file(p, stats)

    print("\n=== replacement counts ===")
    for k, v in stats.items():
        if v:
            print(f"  {k!r}: {v}")
    print("done" + (" (dry run)" if dry else ""))


if __name__ == "__main__":
    main()
