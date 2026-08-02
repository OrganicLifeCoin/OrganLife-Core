#!/usr/bin/env python3
"""Render v1 (current) vs v2 (proposed) dashboard mockups side by side."""
import math
from PIL import Image, ImageDraw, ImageFont

DEEP=(79,122,46); MID=(110,155,69); SAGE=(143,179,95); PALE=(201,214,164)
CREAM=(246,248,236); HAIR=(220,229,192); INK=(34,51,26); MUTE=(126,122,98)
CARROT=(183,126,53); TOMATO=(176,80,46); WHITE=(255,255,255)

def font(sz, bold=True):
    import matplotlib
    base = matplotlib.get_data_path() + '/fonts/ttf/'
    return ImageFont.truetype(base + ('DejaVuSans-Bold.ttf' if bold else 'DejaVuSans.ttf'), sz)

def rr(d, box, r, fill=None, outline=None, width=1):
    d.rounded_rectangle(box, radius=r, fill=fill, outline=outline, width=width)

def glyph_sprout(d, cx, cy, s, col):
    d.line([(cx,cy+s*0.5),(cx,cy-s*0.1)], fill=col, width=max(2,int(s*0.12)))
    d.ellipse([cx-s*0.5,cy-s*0.5,cx-s*0.02,cy-s*0.02], fill=col)
    d.ellipse([cx+s*0.02,cy-s*0.5,cx+s*0.5,cy-s*0.02], fill=col)
    d.ellipse([cx-s*0.22,cy-s*0.85,cx+s*0.22,cy-s*0.28], fill=col)

def glyph_arrow(d, cx, cy, s, col, up=True):
    sgn = -1 if up else 1
    d.line([(cx,cy-sgn*s*0.5),(cx,cy+sgn*s*0.4)], fill=col, width=max(2,int(s*0.14)))
    d.polygon([(cx,cy-sgn*s*0.75),(cx-s*0.35,cy-sgn*s*0.2),(cx+s*0.35,cy-sgn*s*0.2)], fill=col)

def glyph_list(d, cx, cy, s, col):
    for i in range(3):
        y = cy - s*0.4 + i*s*0.4
        d.ellipse([cx-s*0.5, y-s*0.07, cx-s*0.36, y+s*0.07], fill=col)
        d.line([(cx-s*0.2, y),(cx+s*0.5, y)], fill=col, width=max(2,int(s*0.1)))

def glyph_book(d, cx, cy, s, col):
    d.rectangle([cx-s*0.45, cy-s*0.55, cx+s*0.45, cy+s*0.55], outline=col, width=max(2,int(s*0.1)))
    d.line([(cx,cy-s*0.55),(cx,cy+s*0.55)], fill=col, width=max(2,int(s*0.08)))

def glyph_server(d, cx, cy, s, col):
    for i in range(3):
        y = cy - s*0.42 + i*s*0.42
        rr(d, [cx-s*0.5, y-s*0.14, cx+s*0.5, y+s*0.14], s*0.07, outline=col, width=max(2,int(s*0.08)))
        d.ellipse([cx-s*0.36, y-s*0.05, cx-s*0.26, y+s*0.05], fill=col)

def glyph_gear(d, cx, cy, s, col):
    for a in range(8):
        ang = a*math.pi/4
        d.line([(cx+math.cos(ang)*s*0.42, cy+math.sin(ang)*s*0.42),
                (cx+math.cos(ang)*s*0.62, cy+math.sin(ang)*s*0.62)], fill=col, width=max(2,int(s*0.12)))
    d.ellipse([cx-s*0.42,cy-s*0.42,cx+s*0.42,cy+s*0.42], outline=col, width=max(2,int(s*0.1)))
    d.ellipse([cx-s*0.14,cy-s*0.14,cx+s*0.14,cy+s*0.14], fill=col)

def sparkline(d, box, col, fill=None):
    x0,y0,x1,y1 = box
    pts = [(x0 + (x1-x0)*i/14, y1 - (y1-y0)*v) for i,v in
           enumerate([0.2,0.35,0.28,0.5,0.42,0.62,0.55,0.7,0.66,0.8,0.74,0.9,0.84,0.97,1.0])]
    if fill:
        d.polygon(pts + [(x1,y1),(x0,y1)], fill=fill)
    d.line(pts, fill=col, width=3, joint='curve')
    d.ellipse([pts[-1][0]-5, pts[-1][1]-5, pts[-1][0]+5, pts[-1][1]+5], fill=col)

W,H = 1440,900

def render_v1():
    img = Image.new('RGB',(W,H),(243,245,234)); d = ImageDraw.Draw(img)
    # sidebar (white, current)
    d.rectangle([0,0,200,H], fill=WHITE, outline=HAIR)
    logo = Image.open('organiclifecoin-logo.png').convert('RGBA').resize((64,64), Image.LANCZOS)
    img.paste(logo,(68,24),logo)
    nav = ['Dashboard','Send','Receive','Transactions','Address Book','Masternodes','Governance','Settings']
    y = 120
    for i,n in enumerate(nav):
        if i==0:
            rr(d,[10,y-8,190,y+30],10,fill=(238,242,220))
            d.rectangle([10,y-8,18,y+30], fill=DEEP)
            col = DEEP
        else:
            col = (110,110,95)
        d.text((34,y),n,font=font(15),fill=col)
        y += 52
    # topbar gradient
    for x in range(200,W):
        t=(x-200)/(W-200)
        col=tuple(int((252,251,242)[i]+((220,229,192)[i]-(252,251,242)[i])*t) for i in range(3))
        d.line([(x,0),(x,72)], fill=col)
    d.rectangle([200,70,W,72], fill=DEEP)
    d.text((224,22),'Dashboard',font=font(22),fill=INK)
    d.text((W-360,18),'12,540.00 OLC',font=font(26),fill=INK)
    d.text((W-360,50),'available',font=font(11,False),fill=MUTE)
    # left card
    rr(d,[224,96,820,560],22,fill=WHITE,outline=HAIR)
    banner = Image.open('src/qt/res/images/bg-dashboard-banner.png').resize((572,120),Image.LANCZOS)
    img.paste(banner,(236,108))
    d.text((248,252),'Total balance',font=font(13,False),fill=MUTE)
    d.text((248,276),'12,540.00 OLC',font=font(40),fill=INK)
    d.text((248,336),'Locked: 2,000.00  ·  Immature: 0.00',font=font(13,False),fill=MUTE)
    rr(d,[248,372,368,412],12,fill=DEEP); d.text((282,382),'Send',font=font(15),fill=WHITE)
    rr(d,[380,372,520,412],12,outline=DEEP,width=2); d.text((410,382),'Receive',font=font(15),fill=DEEP)
    d.text((248,448),'Latest activity',font=font(14),fill=INK)
    for i in range(2):
        yy = 478+i*40
        rr(d,[248,yy,796,yy+32],8,fill=(252,253,246),outline=HAIR)
        d.text((264,yy+8),'Received from D8sX...k2Pz',font=font(12,False),fill=INK)
        d.text((690,yy+8),'+250.0',font=font(12),fill=DEEP)
    # right card
    rr(d,[844,96,1416,560],22,fill=WHITE,outline=HAIR)
    d.text((868,116),'Staking rewards',font=font(16),fill=INK)
    rr(d,[1180,112,1230,140],10,outline=HAIR); d.text((1192,118),'All',font=font(11,False),fill=INK)
    rr(d,[1240,112,1310,140],10,fill=(238,242,220),outline=SAGE); d.text((1250,118),'Month',font=font(11,False),fill=DEEP)
    rr(d,[1320,112,1392,140],10,outline=HAIR); d.text((1340,118),'Year',font=font(11,False),fill=INK)
    sparkline(d,(868,180,1392,340),DEEP)
    for i,(lab,val) in enumerate([('Stakes','18'),('Masternodes','3'),('Avg / day','42 OLC')]):
        x=868+i*180
        rr(d,[x,368,x+160,428],14,fill=(252,253,246),outline=HAIR)
        d.text((x+14,378),lab.upper(),font=font(10),fill=MUTE)
        d.text((x+14,396),val,font=font(17),fill=INK)
    d.text((868,452),'Recent transactions',font=font(14),fill=INK)
    for i in range(2):
        yy=480+i*40
        rr(d,[868,yy,1392,yy+32],10,fill=(252,253,246),outline=HAIR)
        d.text((884,yy+8),'D8sX...k2Pz',font=font(12,False),fill=INK)
        d.text((1300,yy+8),'+250.0',font=font(12),fill=DEEP)
    return img

def render_v2():
    img = Image.new('RGB',(W,H),(247,248,242)); d = ImageDraw.Draw(img)
    # flat icon-only sidebar
    d.rectangle([0,0,72,H], fill=(34,51,26))
    logo = Image.open('organiclifecoin-logo.png').convert('RGBA').resize((44,44),Image.LANCZOS)
    img.paste(logo,(14,20),logo)
    glyphs=[glyph_sprout,glyph_arrow,glyph_arrow,glyph_list,glyph_book,glyph_server,glyph_gear]
    y=120
    for i,g in enumerate(glyphs):
        if i==0:
            d.ellipse([10,y-24,62,y+28], fill=(61,82,38))
            col=(201,214,164)
        else:
            col=(150,158,132)
        if g==glyph_arrow:
            g(d,36,y+2,26,col,up=(i==1))
        else:
            g(d,36,y+2,26,col)
        y+=68
    # header
    d.text((104,32),'Good evening, grower',font=font(24),fill=INK)
    d.text((104,64),'Mainnet · synced 2 min ago',font=font(12,False),fill=MUTE)
    d.ellipse([W-60,36,W-28,68], outline=SAGE, width=3)
    d.ellipse([W-48,44,W-40,52], fill=DEEP)
    # stat cards
    stats=[('AVAILABLE','10,540 OLC',DEEP),('STAKING','2,000 OLC',CARROT),('REWARDS · 30D','126 OLC',TOMATO)]
    for i,(lab,val,chip) in enumerate(stats):
        x=104+i*448
        rr(d,[x,100,x+424,196],16,fill=WHITE,outline=(227,232,208))
        d.ellipse([x+20,124,x+68,172], fill=tuple(min(255,c+60) for c in chip[:3]))
        g=[glyph_sprout,glyph_server,glyph_list][i]
        g(d,x+44,148,22,WHITE)
        d.text((x+84,120),lab,font=font(11),fill=MUTE)
        d.text((x+84,142),val,font=font(24),fill=INK)
    # chart card
    rr(d,[104,220,980,560],16,fill=WHITE,outline=(227,232,208))
    d.text((128,244),'Harvest',font=font(17),fill=INK)
    d.text((128,270),'rewards over time',font=font(11,False),fill=MUTE)
    sparkline(d,(128,300,952,520),DEEP,fill=(232,238,214))
    # activity feed
    rr(d,[1004,220,1336-104+104+236,560],16,fill=WHITE,outline=(227,232,208))
    d.text((1028,244),'Field notes',font=font(17),fill=INK)
    d.text((1028,270),'recent activity',font=font(11,False),fill=MUTE)
    feed=[('Received','+250.0 OLC',DEEP),('Stake found','+2.0 OLC',CARROT),
          ('Sent','-80.0 OLC',TOMATO),('MN reward','+6.5 OLC',SAGE)]
    for i,(t,v,chip) in enumerate(feed):
        y=300+i*62
        d.ellipse([1028,y,1064,y+36], fill=tuple(min(255,c+60) for c in chip[:3]))
        d.ellipse([1040,y+10,1052,y+22], fill=WHITE)
        d.text((1078,y+2),t,font=font(13),fill=INK)
        d.text((1078,y+22),v,font=font(11,False),fill=MUTE)
    # flat tx table
    rr(d,[104,584,1336,584+280],16,fill=WHITE,outline=(227,232,208))
    d.text((128,608),'Transactions',font=font(17),fill=INK)
    d.text((760,612),'TYPE',font=font(10),fill=MUTE); d.text((900,612),'DATE',font=font(10),fill=MUTE)
    d.text((1180,612),'AMOUNT',font=font(10),fill=MUTE)
    rows=[('D8sX...k2Pz','Received','Aug 2','+250.0',DEEP),('F3mA...q9Rt','Stake','Aug 1','+2.0',CARROT),
          ('H7wB...z4Lp','Sent','Jul 30','-80.0',TOMATO),('K2nC...m8Vd','MN reward','Jul 29','+6.5',SAGE)]
    for i,(a,tp,dt,amt,col) in enumerate(rows):
        y=640+i*52
        d.line([(128,y),(1312,y)], fill=(238,240,226), width=1)
        d.ellipse([128,y+12,160,y+44], fill=(238,242,220))
        glyph_sprout(d,144,y+28,14,DEEP)
        d.text((176,y+16),a,font=font(13,False),fill=INK)
        d.text((760,y+16),tp,font=font(12,False),fill=MUTE)
        d.text((900,y+16),dt,font=font(12,False),fill=MUTE)
        d.text((1180,y+16),amt,font=font(13),fill=col)
    return img

v1, v2 = render_v1(), render_v2()
out = Image.new('RGB',(W, H*2+140),(255,255,255))
d = ImageDraw.Draw(out)
d.rectangle([0,0,W,60], fill=INK)
d.text((24,16),'V1 · current dashboard (as shipped now)',font=font(22),fill=CREAM)
out.paste(v1,(0,60))
d.rectangle([0,H+60,W,H+140], fill=DEEP)
d.text((24,H+84),'V2 · proposal — flat, icon-rail nav, stat cards, harvest chart, unified table',font=font(22),fill=CREAM)
out.paste(v2,(0,H+140))
out.save('design/v1-v2-dashboard.png')
print('mockup written')
