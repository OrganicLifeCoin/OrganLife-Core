#!/usr/bin/env python3
"""OrganicLife Coin v2 logo system - clean flat seedling mark, harvest palette.

Generates: master logo, coin, all Qt image assets (nav/topbar/about/splash/
banner/welcome/coin-icon) plus root masters for icon rebuilds.
"""
import math
from PIL import Image, ImageDraw, ImageFont, ImageFilter

# Harvest palette (matches theme CSS)
DEEP   = (79, 122, 46)     # 4F7A2E
MID    = (110, 155, 69)    # 6E9B45
SAGE   = (143, 179, 95)    # 8FB35F
PALE   = (201, 214, 164)   # C9D6A4
CREAM  = (246, 248, 236)   # F6F8EC
HAIR   = (220, 229, 192)   # DCE5C0
INK    = (34, 51, 26)      # 22331a
DK_BG  = (43, 51, 25)      # 2B3319

SS = 4  # supersample factor

def leaf(size, color):
    """Vesica-piscis leaf, long axis vertical, pointed tips. RGBA."""
    S = size * SS
    m1 = Image.new('L', (S, S), 0); m2 = Image.new('L', (S, S), 0)
    R, c = S * 0.75, S * 0.55
    d1, d2 = ImageDraw.Draw(m1), ImageDraw.Draw(m2)
    d1.ellipse([S/2 - c - R, S/2 - R, S/2 - c + R, S/2 + R], fill=255)
    d2.ellipse([S/2 + c - R, S/2 - R, S/2 + c + R, S/2 + R], fill=255)
    from PIL import ImageChops
    mask = ImageChops.multiply(m1, m2)
    img = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    img.paste(Image.new('RGBA', (S, S), color + (255,)), (0, 0), mask)
    return img.resize((size, size), Image.LANCZOS)

def paste_rot(canvas, img, center, angle):
    rot = img.rotate(angle, expand=True, resample=Image.BICUBIC)
    canvas.alpha_composite(rot, (int(center[0] - rot.width/2), int(center[1] - rot.height/2)))

def mark(size, main=DEEP, accent=SAGE):
    """Seedling: stem + two side leaves + small top leaf. Transparent bg."""
    S = size * SS
    c = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(c)
    # stem: gently tapered, slight curve
    stem_w = int(S * 0.045)
    pts_top, pts_bot = (S*0.50, S*0.34), (S*0.485, S*0.86)
    d.line([pts_bot, (S*0.492, S*0.60), pts_top], fill=main+(255,), width=stem_w)
    r = stem_w/2
    d.ellipse([pts_bot[0]-r, pts_bot[1]-r, pts_bot[0]+r, pts_bot[1]+r], fill=main+(255,))
    # side leaves
    lf = leaf(int(S*0.52), main)
    paste_rot(c, lf, (S*0.345, S*0.44), 42)
    paste_rot(c, lf, (S*0.645, S*0.44), -42)
    # top leaf
    tf = leaf(int(S*0.40), accent)
    paste_rot(c, tf, (S*0.50, S*0.185), 0)
    return c.resize((size, size), Image.LANCZOS)

def coin(px, ring=DEEP, face=CREAM, main=DEEP, accent=SAGE, transparent_edge=True):
    S = px * SS
    c = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(c)
    cx = cy = S/2
    R = S*0.49
    d.ellipse([cx-R, cy-R, cx+R, cy+R], fill=ring+(255,))          # outer rim
    r2 = R*0.90
    d.ellipse([cx-r2, cy-r2, cx+r2, cy+r2], fill=face+(255,))       # face
    r3 = R*0.82
    d.ellipse([cx-r3, cy-r3, cx+r3, cy+r3], outline=HAIR+(255,), width=int(S*0.006))
    c.alpha_composite(mark(int(r3*1.45), main, accent).resize((int(r3*1.45),)*2, Image.LANCZOS),
                      (int(cx - r3*0.725), int(cy - r3*0.70)))
    out = c.resize((px, px), Image.LANCZOS)
    if not transparent_edge:  # flatten on face color
        bg = Image.new('RGBA', out.size, face+(255,)); bg.alpha_composite(out); out = bg
    return out

def font(sz, bold=True):
    import matplotlib
    p = matplotlib.get_data_path() + '/fonts/ttf/DejaVuSans-Bold.ttf' if bold else \
        matplotlib.get_data_path() + '/fonts/ttf/DejaVuSans.ttf'
    return ImageFont.truetype(p, sz)

def hgrad(size, c1, c2):
    w, h = size
    base = Image.new('RGB', (w, h))
    for x in range(w):
        t = x/(w-1)
        ImageDraw.Draw(base).line([(x, 0), (x, h)],
            fill=tuple(int(c1[i]+(c2[i]-c1[i])*t) for i in range(3)))
    return base

# ---------- assets ----------
IMG = 'src/qt/res/images/'

# master mark + coin (root)
mark(1024).save('organiclifecoin-logo.png')
coin(1024).save('CTEAMCoin.png')

# in-wallet logos
for name, px in [('img-logo-organiclife.png', 256), ('img-logo-organiclife@2x.png', 512),
                 ('img-logo-organiclife@3x.png', 768)]:
    coin(px).save(IMG+name)
# nav logo (dark sidebar): light-colored mark
mark(60, main=PALE, accent=SAGE).save(IMG+'img-nav-logo-organiclife.png')
mark(52, main=PALE, accent=SAGE).save(IMG+'img-nav-logo.png')
# coin icon (balance list)
coin(96).save(IMG+'ic-coin-organiclife.png')

# about.png: mark + wordmark, transparent 304x100
ab = Image.new('RGBA', (304*SS, 100*SS), (0, 0, 0, 0))
ab.alpha_composite(mark(84*SS), (6*SS, 8*SS))
d = ImageDraw.Draw(ab)
d.text((96*SS, 22*SS), 'OrganicLife Coin', font=font(24*SS), fill=INK+(255,))
d.text((96*SS, 56*SS), 'OLC  ·  grow your own', font=font(11*SS, False), fill=(110, 118, 92, 255))
ab.resize((304, 100), Image.LANCZOS).save(IMG+'about.png')

# splash 768x533: deep green, centered coin, wordmark
sp = Image.new('RGB', (768*SS, 533*SS), DK_BG)
d = ImageDraw.Draw(sp)
for y in range(533*SS):  # subtle vertical dim gradient
    t = y/(533*SS-1)
    col = tuple(int(DK_BG[i]+(DEEP[i]-DK_BG[i])*t*0.55) for i in range(3))
    d.line([(0, y), (768*SS, y)], fill=col)
sp = sp.convert('RGBA')
ck = coin(250*SS, ring=SAGE, face=CREAM)
sp.alpha_composite(ck, (int((768*SS-250*SS)/2), 92*SS))
d = ImageDraw.Draw(sp)
tw = d.textlength('ORGANICLIFE COIN', font=font(34*SS))
d.text(((768*SS-tw)/2, 372*SS), 'ORGANICLIFE COIN', font=font(34*SS), fill=CREAM+(255,))
tw2 = d.textlength('grow your own', font=font(15*SS, False))
d.text(((768*SS-tw2)/2, 424*SS), 'grow your own', font=font(15*SS, False), fill=PALE+(255,))
sp.convert('RGB').resize((768, 533), Image.LANCZOS).save(IMG+'bg-splash.png')

# dashboard banner 1100x230: airy cream->sage, ghost leaves right
bn = hgrad((1100*SS, 230*SS), (247, 249, 238), (227, 234, 203)).convert('RGBA')
ghost = Image.new('RGBA', bn.size, (0, 0, 0, 0))
gl = leaf(int(230*SS*1.15), SAGE)
paste_rot(ghost, gl, (880*SS, 120*SS), 28)
paste_rot(ghost, gl, (1010*SS, 90*SS), -18)
ghost = ghost.point(lambda p: p)  # noop keep
alpha = ghost.split()[3].point(lambda a: int(a*0.16))
ghost.putalpha(alpha)
bn.alpha_composite(ghost)
mk = mark(150*SS)
bn.alpha_composite(mk, (60*SS, 40*SS))
d = ImageDraw.Draw(bn)
d.text((240*SS, 78*SS), 'OrganicLife Coin', font=font(34*SS), fill=INK+(255,))
d.text((240*SS, 128*SS), 'local growth, global roots', font=font(14*SS, False), fill=(96, 106, 76, 255))
bn.convert('RGB').resize((1100, 230), Image.LANCZOS).save(IMG+'bg-dashboard-banner.png')
bn.convert('RGB').resize((1100, 230), Image.LANCZOS).save(IMG+'img-dashboard-banner.jpg', quality=92)

# welcome bg 1200x800: cream + faint sprout watermark
wl = Image.new('RGB', (1200*SS, 800*SS), CREAM).convert('RGBA')
wm = mark(560*SS, main=SAGE, accent=PALE)
a = wm.split()[3].point(lambda x: int(x*0.10)); wm.putalpha(a)
wl.alpha_composite(wm, (640*SS, 180*SS))
wl.convert('RGB').resize((1200, 800), Image.LANCZOS).save(IMG+'bg-welcome.png')

print('v2 logo system written')
