#!/usr/bin/env python3
"""OrganicLife Coin v4 logo - professional monoline leaf mark.

Single geometric leaf with negative-space center vein and short stem.
Restrained two-tone palette. Regenerates all Qt assets + root masters.
"""
from PIL import Image, ImageDraw, ImageFont, ImageChops

DEEP   = (79, 122, 46)     # 4F7A2E
SAGE   = (143, 179, 95)    # 8FB35F
PALE   = (201, 214, 164)   # C9D6A4
CREAM  = (246, 248, 236)   # F6F8EC
HAIR   = (220, 229, 192)   # DCE5C0
INK    = (34, 51, 26)      # 22331a
DK_BG  = (43, 51, 25)      # 2B3319

SS = 4

def vesica_mask(S, R_rel=0.75, c_rel=0.55):
    m1 = Image.new('L', (S, S), 0); m2 = Image.new('L', (S, S), 0)
    R, c = S * R_rel, S * c_rel
    d1, d2 = ImageDraw.Draw(m1), ImageDraw.Draw(m2)
    d1.ellipse([S/2 - c - R, S/2 - R, S/2 - c + R, S/2 + R], fill=255)
    d2.ellipse([S/2 + c - R, S/2 - R, S/2 + c + R, S/2 + R], fill=255)
    return ImageChops.multiply(m1, m2)

def mark(size, color=DEEP, vein=True):
    """Single upright leaf, negative-space vein, short stem. Transparent bg."""
    S = size * SS
    leaf_box = int(S * 0.86)
    lm = vesica_mask(leaf_box)
    if vein:
        # subtract a slim tapered midrib through the leaf's center
        vm = vesica_mask(int(leaf_box * 1.0), R_rel=0.90, c_rel=0.875)
        vein_layer = Image.new('L', (leaf_box, leaf_box), 0)
        vein_layer.paste(vm, (int(leaf_box*0.5 - vm.width/2), int(leaf_box*0.47 - vm.height/2)))
        lm = ImageChops.subtract(lm, vein_layer)
    c = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    leaf_img = Image.new('RGBA', (leaf_box, leaf_box), color + (255,))
    c.paste(leaf_img, (int(S/2 - leaf_box/2), int(S*0.44 - leaf_box/2)), lm)
    d = ImageDraw.Draw(c)
    sw = int(S * 0.035)
    d.line([(S/2, S*0.80), (S/2, S*0.92)], fill=color + (255,), width=sw)
    r = sw / 2
    d.ellipse([S/2 - r, S*0.92 - r, S/2 + r, S*0.92 + r], fill=color + (255,))
    return c.resize((size, size), Image.LANCZOS)

def coin(px, ring=DEEP, face=CREAM, color=DEEP):
    S = px * SS
    c = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(c)
    cx = cy = S/2
    R = S*0.49
    d.ellipse([cx-R, cy-R, cx+R, cy+R], fill=ring+(255,))
    r2 = R*0.90
    d.ellipse([cx-r2, cy-r2, cx+r2, cy+r2], fill=face+(255,))
    r3 = R*0.80
    d.ellipse([cx-r3, cy-r3, cx+r3, cy+r3], outline=HAIR+(255,), width=int(S*0.006))
    mk = mark(int(r3*1.42), color)
    c.alpha_composite(mk, (int(cx - r3*0.71), int(cy - r3*0.76)))
    return c.resize((px, px), Image.LANCZOS)

def font(sz, bold=True):
    import matplotlib
    base = matplotlib.get_data_path() + '/fonts/ttf/'
    return ImageFont.truetype(base + ('DejaVuSans-Bold.ttf' if bold else 'DejaVuSans.ttf'), sz)

def hgrad(size, c1, c2):
    w, h = size
    base = Image.new('RGB', (w, h)); dr = ImageDraw.Draw(base)
    for x in range(w):
        t = x/(w-1)
        dr.line([(x, 0), (x, h)], fill=tuple(int(c1[i]+(c2[i]-c1[i])*t) for i in range(3)))
    return base

IMG = 'src/qt/res/images/'

mark(1024).save('organiclifecoin-logo.png')
coin(1024).save('OrganicLifeCoin.png')

for name, px in [('img-logo-organiclife.png', 256), ('img-logo-organiclife@2x.png', 512),
                 ('img-logo-organiclife@3x.png', 768)]:
    coin(px).save(IMG+name)
mark(60, color=PALE, vein=False).save(IMG+'img-nav-logo-organiclife.png')
mark(52, color=PALE, vein=False).save(IMG+'img-nav-logo.png')
coin(96).save(IMG+'ic-coin-organiclife.png')

ab = Image.new('RGBA', (304*SS, 100*SS), (0, 0, 0, 0))
ab.alpha_composite(mark(84*SS), (10*SS, 8*SS))
d = ImageDraw.Draw(ab)
d.text((100*SS, 22*SS), 'OrganicLife Coin', font=font(24*SS), fill=INK+(255,))
d.text((100*SS, 56*SS), 'OLC  ·  grow your own', font=font(11*SS, False), fill=(110, 118, 92, 255))
ab.resize((304, 100), Image.LANCZOS).save(IMG+'about.png')

sp = Image.new('RGB', (768*SS, 533*SS), DK_BG)
d = ImageDraw.Draw(sp)
for y in range(533*SS):
    t = y/(533*SS-1)
    d.line([(0, y), (768*SS, y)], fill=tuple(int(DK_BG[i]+(DEEP[i]-DK_BG[i])*t*0.55) for i in range(3)))
sp = sp.convert('RGBA')
sp.alpha_composite(coin(250*SS, ring=SAGE), (int((768*SS-250*SS)/2), 92*SS))
d = ImageDraw.Draw(sp)
tw = d.textlength('ORGANICLIFE COIN', font=font(34*SS))
d.text(((768*SS-tw)/2, 372*SS), 'ORGANICLIFE COIN', font=font(34*SS), fill=CREAM+(255,))
tw2 = d.textlength('grow your own', font=font(15*SS, False))
d.text(((768*SS-tw2)/2, 424*SS), 'grow your own', font=font(15*SS, False), fill=PALE+(255,))
sp.convert('RGB').resize((768, 533), Image.LANCZOS).save(IMG+'bg-splash.png')

bn = hgrad((1100*SS, 230*SS), (247, 249, 238), (227, 234, 203)).convert('RGBA')
ghost = Image.new('RGBA', bn.size, (0, 0, 0, 0))
gm = mark(int(230*SS*1.15), SAGE)
rot = gm.rotate(18, expand=True, resample=Image.BICUBIC)
ghost.alpha_composite(rot, (int(880*SS - rot.width/2), int(120*SS - rot.height/2)))
alpha = ghost.split()[3].point(lambda a: int(a*0.14))
ghost.putalpha(alpha)
bn.alpha_composite(ghost)
bn.alpha_composite(mark(150*SS), (60*SS, 40*SS))
d = ImageDraw.Draw(bn)
d.text((240*SS, 78*SS), 'OrganicLife Coin', font=font(34*SS), fill=INK+(255,))
d.text((240*SS, 128*SS), 'local growth, global roots', font=font(14*SS, False), fill=(96, 106, 76, 255))
bn.convert('RGB').resize((1100, 230), Image.LANCZOS).save(IMG+'bg-dashboard-banner.png')
bn.convert('RGB').resize((1100, 230), Image.LANCZOS).save(IMG+'img-dashboard-banner.jpg', quality=92)

wl = Image.new('RGB', (1200*SS, 800*SS), CREAM).convert('RGBA')
wm = mark(560*SS, color=SAGE)
a = wm.split()[3].point(lambda x: int(x*0.10)); wm.putalpha(a)
wl.alpha_composite(wm, (640*SS, 180*SS))
wl.convert('RGB').resize((1200, 800), Image.LANCZOS).save(IMG+'bg-welcome.png')

print('v4 logo system written')
