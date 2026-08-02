#!/usr/bin/env python3
"""Flat rail icons for the v2 icon-only sidebar.

8 glyphs x 2 states (inactive sage, active cream), 64px transparent PNGs.
"""
import math
from PIL import Image, ImageDraw

INACTIVE = (154, 164, 140)   # 9AA48C sage-gray
ACTIVE   = (244, 246, 226)   # F4F6E2 cream
S = 64
SS = 4

def canvas():
    return Image.new('RGBA', (S*SS, S*SS), (0, 0, 0, 0))

def lw(w): return int(w*SS)

def g_sprout(d, col):
    c = S*SS/2
    d.line([(c, S*SS*0.78), (c, S*SS*0.45)], fill=col, width=lw(5))
    d.ellipse([S*SS*0.14, S*SS*0.30, c-2, S*SS*0.58], fill=col)
    d.ellipse([c+2, S*SS*0.30, S*SS*0.86, S*SS*0.58], fill=col)
    d.ellipse([S*SS*0.36, S*SS*0.10, S*SS*0.64, S*SS*0.42], fill=col)

def g_arrow_up(d, col):
    d.line([(S*SS*0.5, S*SS*0.78), (S*SS*0.5, S*SS*0.30)], fill=col, width=lw(5))
    d.polygon([(S*SS*0.5, S*SS*0.16), (S*SS*0.24, S*SS*0.42), (S*SS*0.76, S*SS*0.42)], fill=col)

def g_arrow_down(d, col):
    d.line([(S*SS*0.5, S*SS*0.22), (S*SS*0.5, S*SS*0.70)], fill=col, width=lw(5))
    d.polygon([(S*SS*0.5, S*SS*0.84), (S*SS*0.24, S*SS*0.58), (S*SS*0.76, S*SS*0.58)], fill=col)

def g_book(d, col):
    d.rounded_rectangle([S*SS*0.22, S*SS*0.16, S*SS*0.78, S*SS*0.84], radius=S*SS*0.06,
                        outline=col, width=lw(4))
    d.line([(S*SS*0.5, S*SS*0.16), (S*SS*0.5, S*SS*0.84)], fill=col, width=lw(3))

def g_server(d, col):
    for i in range(3):
        y0 = S*SS*(0.18 + i*0.24)
        d.rounded_rectangle([S*SS*0.16, y0, S*SS*0.84, y0 + S*SS*0.18], radius=S*SS*0.05,
                            outline=col, width=lw(3.5))
        d.ellipse([S*SS*0.24, y0 + S*SS*0.06, S*SS*0.30, y0 + S*SS*0.12], fill=col)

def g_snow(d, col):
    c = S*SS/2
    for a in range(6):
        ang = a*math.pi/3
        d.line([(c + math.cos(ang)*S*SS*0.10, c + math.sin(ang)*S*SS*0.10),
                (c + math.cos(ang)*S*SS*0.34, c + math.sin(ang)*S*SS*0.34)], fill=col, width=lw(4))
        for sgn in (-1, 1):
            ang2 = ang + sgn*0.5
            d.line([(c + math.cos(ang)*S*SS*0.24, c + math.sin(ang)*S*SS*0.24),
                    (c + math.cos(ang)*S*SS*0.24 + math.cos(ang2)*S*SS*0.08,
                     c + math.sin(ang)*S*SS*0.24 + math.sin(ang2)*S*SS*0.08)], fill=col, width=lw(3))

def g_gear(d, col):
    c = S*SS/2
    for a in range(8):
        ang = a*math.pi/4
        d.line([(c + math.cos(ang)*S*SS*0.30, c + math.sin(ang)*S*SS*0.30),
                (c + math.cos(ang)*S*SS*0.40, c + math.sin(ang)*S*SS*0.40)], fill=col, width=lw(5))
    d.ellipse([c-S*SS*0.30, c-S*SS*0.30, c+S*SS*0.30, c+S*SS*0.30], outline=col, width=lw(4))
    d.ellipse([c-S*SS*0.10, c-S*SS*0.10, c+S*SS*0.10, c+S*SS*0.10], fill=col)

def g_vote(d, col):
    d.rounded_rectangle([S*SS*0.18, S*SS*0.18, S*SS*0.82, S*SS*0.82], radius=S*SS*0.08,
                        outline=col, width=lw(4))
    d.line([(S*SS*0.32, S*SS*0.52), (S*SS*0.46, S*SS*0.66), (S*SS*0.70, S*SS*0.34)],
           fill=col, width=lw(5), joint='curve')

GLYPHS = {
    'dashboard': g_sprout, 'send': g_arrow_up, 'receive': g_arrow_down,
    'address': g_book, 'master': g_server, 'cold-staking': g_snow,
    'settings': g_gear, 'governance': g_vote,
}

for name, fn in GLYPHS.items():
    for suffix, col in [('', INACTIVE), ('-active', ACTIVE)]:
        img = canvas()
        fn(ImageDraw.Draw(img), col + (255,))
        img.resize((S, S), Image.LANCZOS).save(f'src/qt/res/images/ic-rail-{name}{suffix}.png')
print('rail icons written')
