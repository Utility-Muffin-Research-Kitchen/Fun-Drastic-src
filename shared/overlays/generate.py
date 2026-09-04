#!/usr/bin/env python3
"""Generate Fun Drastic overlay layout templates for any panel resolution.

An overlay pack is a folder of PNGs the hook blends over DraStic's output —
one per screen layout, in two scaling modes (aspect-fit and pixel-perfect).
A *template* is the neutral starting point: a dark fill with the emulator's
screen area(s) cut out transparent, so a theme author can paint a bezel around
where the screens actually render.

The screen rectangles below mirror funhook.c's compute_layouts() exactly, so a
generated template lines up with where the hook draws each screen at that
resolution. SINGLE and PIP are intentionally blank (fully transparent).

Usage:
    python3 generate.py                 # the common resolutions below
    python3 generate.py 1280x720 ...    # specific panel sizes
"""
import os, sys
from PIL import Image

NDS_W, NDS_H = 256, 192
PP_GAP = 92
FILL = (28, 28, 32, 255)   # neutral dark, sampled from the reference templates

COMMON = ["320x240", "480x320", "640x480", "720x480", "720x720",
          "800x600", "960x720", "1024x768", "1280x720"]

def fit43w(bw, bh):
    return bw if bw * 3 <= bh * 4 else bh * 4 // 3

def aspect_slots(W, H):
    """layout index -> list of (x,y,w,h) screen rects (aspect-fit mode)."""
    S = {}
    mw = W if W * 3 <= H * 4 else H * 4 // 3; mh = mw * 3 // 4
    S[0] = []  # PIP: blank template
    sw = fit43w(W, H); sh = sw * 3 // 4
    S[1] = []  # SINGLE: blank template
    mw = W; mh = mw * 3 // 4
    if mh > H * 3 // 4: mh = H * 3 // 4; mw = mh * 4 // 3
    mx = (W - mw) // 2; rem = H - mh
    tw = fit43w(W, rem); th = tw * 3 // 4; tx = (W - tw) // 2; ty = mh + (rem - th) // 2
    S[2] = [(mx, 0, mw, mh), (tx, ty, tw, th)]                       # FOCUS
    sw = fit43w(W, H // 2); sh = sw * 3 // 4; sx = (W - sw) // 2
    top = (H - sh * 2) // 2
    S[3] = [(sx, top, sw, sh), (sx, top + sh, sw, sh)]               # STACKED
    sw = W // 2; sh = sw * 3 // 4
    if sh > H: sh = H; sw = sh * 4 // 3
    lx = (W - sw * 2) // 2; cy = (H - sh) // 2
    S[4] = [(lx + sw, cy, sw, sh), (lx, cy, sw, sh)]                 # SIDE BY SIDE
    return S

def pp_slots(W, H):
    """layout index -> list of (x,y,w,h) screen rects (pixel-perfect mode)."""
    def scale(fits):
        for s in range(4, 0, -1):
            if fits(s): return s
        return 1
    f = scale(lambda s: NDS_W * s <= W and NDS_H * s < H)
    pfw, pfh = NDS_W * f, NDS_H * f
    pftw = (H - pfh) * NDS_W // NDS_H; pfth = H - pfh
    S = {0: [], 1: []}  # PIP / SINGLE blank
    S[2] = [((W - pfw) // 2, 0, pfw, pfh), ((W - pftw) // 2, pfh, pftw, pfth)]
    y3 = (H - (NDS_H * 2 + PP_GAP)) // 2
    S[3] = [((W - NDS_W) // 2, y3, NDS_W, NDS_H),
            ((W - NDS_W) // 2, y3 + NDS_H + PP_GAP, NDS_W, NDS_H)]
    x4 = (W - (NDS_W * 2 + PP_GAP)) // 2
    S[4] = [(x4 + NDS_W + PP_GAP, (H - NDS_H) // 2, NDS_W, NDS_H),
            (x4, (H - NDS_H) // 2, NDS_W, NDS_H)]
    return S

NAMES = {0: "pip", 1: "single", 2: "focus", 3: "stacked", 4: "sidebyside"}

def render(W, H, rects):
    im = Image.new("RGBA", (W, H), FILL if rects else (0, 0, 0, 0))
    for (x, y, w, h) in rects:
        for yy in range(max(0, y), min(H, y + h)):
            for xx in range(max(0, x), min(W, x + w)):
                im.putpixel((xx, yy), (0, 0, 0, 0))
    return im

def generate(res):
    W, H = (int(v) for v in res.lower().split("x"))
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), res, "Template")
    os.makedirs(out, exist_ok=True)
    asp, pp = aspect_slots(W, H), pp_slots(W, H)
    for idx, name in NAMES.items():
        render(W, H, asp[idx]).save(os.path.join(out, f"aspect_{name}.png"))
        render(W, H, pp[idx]).save(os.path.join(out, f"pp_{name}.png"))
    print(f"generated {res} -> {out}")

if __name__ == "__main__":
    for r in (sys.argv[1:] or COMMON):
        generate(r)
