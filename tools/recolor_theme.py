#!/usr/bin/env python3
"""OrganicLife Phase 3: recolor the Qt theme from CTEAM copper to the
OrganicLife harvest palette (moss green / leaf / terracotta / cream / umber).

Applies a curated hex->hex mapping to CSS, SVG and guiconstants.h.
"""
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# old (copper/brown CTEAM theme) -> new (OrganicLife harvest theme)
MAP = {
    # primary accents: copper -> moss/leaf green
    "#9c4e1a": "#4f7a2e",  # copper primary      -> moss green
    "#c9823d": "#6e9b45",  # light copper/hover  -> leaf
    "#c08445": "#8fb35f",  # light copper on dark-> light leaf
    "#e5a15e": "#a8c46b",  # pale copper         -> pale leaf
    # dark surfaces/text: umber -> olive-umber
    "#3a2418": "#2b2b1a",
    "#130b08": "#0d1008",
    "#1c100b": "#141a0d",
    "#332016": "#2a3319",
    "#4a3022": "#3b4426",
    "#4a3b31": "#4a4a38",
    "#3a3029": "#3a3a2c",
    "#32313a": "#32362b",
    "#333320": "#333a20",
    # muted mid-tones: brown-gray -> olive-gray
    "#8a7667": "#7e7a62",
    "#8f7663": "#87906f",
    "#b69b82": "#a9b38d",
    "#6d4f3b": "#5d6b45",
    "#5e4a3c": "#525a3c",
    "#a78f7c": "#9aa37e",
    # light backgrounds: parchment/cream -> greener cream
    "#f3e5d6": "#eef0dc",
    "#fbf3e8": "#f6f4e8",
    "#f0e3d4": "#efefdd",
    "#fff0e0": "#f4f6e2",
    "#fff4e8": "#f8faea",
    "#fff8f5": "#fafbf0",
    "#e8dccf": "#e3e8ce",
    "#e6ddda": "#e2e6d2",
    "#ead2be": "#e4eac6",
    "#d7c2ae": "#ccd4ac",
    "#cdbba8": "#c2c7ae",
    "#c7c4c2": "#c6c8b8",
    "#f2f0f0": "#f0f0ea",
    "#f0f0f0": "#f0f0ea",
    "#f8f8f8": "#f8f8f2",
    "#9f9ea5": "#9a9e8e",
    # secondary accent: burnt orange -> terracotta
    "#c83b08": "#c4552a",
    "#d34108": "#c4552a",
    "#a93407": "#a84a2a",
    # success greens -> leaf green family
    "#33c084": "#46b96a",
    "#4dc084": "#5cc47e",
    "#1a9c4e": "#2e9e4a",
    "#339c4e": "#43a85c",
    # stragglers
    "#7b67a9": "#4f7a2e",  # leftover PIVX purple
    "#f84444": "#f05050",
}

# RGB QColor() values in guiconstants.h: (r,g,b) -> (r,g,b)
RGB_MAP = {
    (138, 118, 103): (126, 122, 98),   # #8A7667
    (201, 130, 61): (110, 155, 69),    # #C9823D
    (58, 36, 24): (43, 43, 26),        # #3A2418
    (232, 220, 207): (227, 232, 206),  # #E8DCCF
    (156, 78, 26): (79, 122, 46),      # #9C4E1A
}

HEX_RE = re.compile(r"#[0-9a-fA-F]{6}\b")
QCOLOR_RE = re.compile(r"QColor\(\s*(\d+),\s*(\d+),\s*(\d+)\s*\)")

TARGETS = [
    "src/qt/res/css",
    "src/qt/res/images",
    "src/qt/res/icons",
    "src/qt/guiconstants.h",
]

changed_files = 0
total_subs = 0

def sweep_text(text):
    global total_subs
    def hex_sub(m):
        global total_subs
        new = MAP.get(m.group(0).lower())
        if new:
            total_subs += 1
            return new
        return m.group(0)
    text = HEX_RE.sub(hex_sub, text)
    def qcolor_sub(m):
        global total_subs
        key = (int(m.group(1)), int(m.group(2)), int(m.group(3)))
        new = RGB_MAP.get(key)
        if new:
            total_subs += 1
            return f"QColor({new[0]}, {new[1]}, {new[2]})"
        return m.group(0)
    return QCOLOR_RE.sub(qcolor_sub, text)

for target in TARGETS:
    p = os.path.join(ROOT, target)
    files = []
    if os.path.isdir(p):
        for dirpath, _, filenames in os.walk(p):
            for fn in filenames:
                if fn.lower().endswith((".css", ".svg")):
                    files.append(os.path.join(dirpath, fn))
    elif os.path.isfile(p):
        files.append(p)
    for f in files:
        with open(f, "r", encoding="utf-8") as fh:
            orig = fh.read()
        new = sweep_text(orig)
        if new != orig:
            with open(f, "w", encoding="utf-8") as fh:
                fh.write(new)
            changed_files += 1
            print(f"recolored {os.path.relpath(f, ROOT)}")

print(f"\n{changed_files} files recolored, {total_subs} color substitutions")

# update comment color names in guiconstants.h
gc = os.path.join(ROOT, "src/qt/guiconstants.h")
with open(gc) as f:
    t = f.read()
t = t.replace("OrganicLife parchment #E8DCCF", "OrganicLife cream #E3E8CE")
t = t.replace("OrganicLife copper #9C4E1A", "OrganicLife moss #4F7A2E")
with open(gc, "w") as f:
    f.write(t)
print("guiconstants.h comments updated")
