#!/usr/bin/env python3
"""OrganicLife Phase 3: build the full icon set from the master logo."""
import os
import subprocess
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "organiclifecoin-logo.png")

# ---- circular crop (drops the generator watermark in the corner) ----
img = Image.open(SRC).convert("RGBA")
W, H = img.size
alpha = img.getchannel("A")
px = alpha.load()
# find coin bbox ignoring the bottom-left watermark zone
minx, miny, maxx, maxy = W, H, 0, 0
for y in range(H):
    for x in range(W):
        if px[x, y] > 128 and not (x < 220 and y > 840):
            if x < minx: minx = x
            if x > maxx: maxx = x
            if y < miny: miny = y
            if y > maxy: maxy = y
cx, cy = (minx + maxx) // 2, (miny + maxy) // 2
r = min(maxx - minx, maxy - miny) // 2 - 2
print(f"coin bbox=({minx},{miny},{maxx},{maxy}) center=({cx},{cy}) r={r}")

mask = Image.new("L", (W, H), 0)
d = ImageDraw.Draw(mask)
d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=255)
coin = Image.new("RGBA", (2 * r, 2 * r), (0, 0, 0, 0))
coin.paste(img.crop((cx - r, cy - r, cx + r, cy + r)), (0, 0))
m2 = mask.crop((cx - r, cy - r, cx + r, cy + r))
coin.putalpha(m2)

def save(size, path):
    coin.resize((size, size), Image.LANCZOS).save(path)
    print(f"  {size:>4}px -> {os.path.relpath(path, ROOT)}")

# ---- master brand source (keeps the historic name used by scripts) ----
coin.resize((1024, 1024), Image.LANCZOS).save(os.path.join(ROOT, "CTEAMCoin.png"))
print("master -> CTEAMCoin.png")

# ---- qt resource logos ----
IMG = os.path.join(ROOT, "src/qt/res/images")
save(256, f"{IMG}/img-logo-organiclife.png")
save(512, f"{IMG}/img-logo-organiclife@2x.png")
save(768, f"{IMG}/img-logo-organiclife@3x.png")
save(60, f"{IMG}/img-nav-logo-organiclife.png")
save(96, f"{IMG}/ic-coin-organiclife.png")

# ---- qt app icons ----
ICONS = os.path.join(ROOT, "src/qt/res/icons")
save(1024, f"{ICONS}/bitcoin.png")
save(128, f"{ICONS}/overview.png")

# macOS .icns via iconutil
iconset = os.path.join(ROOT, "tools/olc.iconset")
os.makedirs(iconset, exist_ok=True)
for name, size in [("icon_16x16", 16), ("icon_16x16@2x", 32), ("icon_32x32", 32),
                   ("icon_32x32@2x", 64), ("icon_128x128", 128), ("icon_128x128@2x", 256),
                   ("icon_256x256", 256), ("icon_256x256@2x", 512),
                   ("icon_512x512", 512), ("icon_512x512@2x", 1024)]:
    coin.resize((size, size), Image.LANCZOS).save(f"{iconset}/{name}.png")
subprocess.run(["iconutil", "-c", "icns", iconset, "-o", f"{ICONS}/organiclife.icns"], check=True)
print("icns -> src/qt/res/icons/organiclife.icns")

# ---- share/pixmaps (linux/windows installer icons) ----
PX = os.path.join(ROOT, "share/pixmaps")
for size in (16, 24, 32, 48, 64, 128, 256, 512, 1024):
    save(size, f"{PX}/pivx{size}.png")
# multi-size .ico files
coin.save(f"{PX}/cteam.ico", sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
coin.save(f"{PX}/favicon.ico", sizes=[(16, 16), (32, 32), (48, 48)])
print("ico -> share/pixmaps/cteam.ico, favicon.ico")
# xpm variants
for size in (16, 24, 32, 48, 64, 128, 256, 512, 1024):
    coin.resize((size, size), Image.LANCZOS).convert("RGB").save(f"{PX}/pivx{size}.xpm")
print("xpm set regenerated")

# ---- remove legacy branded files ----
legacy = [
    "src/qt/res/images/img-logo-pivx.png", "src/qt/res/images/img-logo-pivx@2x.png",
    "src/qt/res/images/img-logo-pivx@3x.png", "src/qt/res/images/img-logo-pivx.svg",
    "src/qt/res/images/img-nav-logo-pivx.png", "src/qt/res/images/ic-coin-piv.svg",
    "src/qt/res/images/1776logo.png", "src/qt/res/images/logo1776.png",
    "src/qt/res/images/img-logo-cteam.png", "src/qt/res/images/img-logo-cteam@2x.png",
    "src/qt/res/images/img-logo-cteam@3x.png", "src/qt/res/images/img-nav-logo-cteam.png",
    "src/qt/res/images/ic-coin-cteam.png",
]
for f in legacy:
    p = os.path.join(ROOT, f)
    if os.path.exists(p):
        os.remove(p)
        print(f"removed {f}")
print("done")
