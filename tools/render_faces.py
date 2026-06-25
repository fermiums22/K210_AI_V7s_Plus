#!/usr/bin/env python3
"""Render 10 emotive robot-face images (320x240 JPG) for the K210 LCD with PIL:
metallic head, glowing eyes/ears, antennae; eyes/brows/mouth vary per emotion.

Usage: python tools/render_faces.py <out_dir>
"""
import os, sys, math
from PIL import Image, ImageDraw, ImageFilter

W, H = 320, 240
SS = 3                      # supersample for anti-aliasing
CX = W * SS // 2

# palette
STEEL_HI = (200, 208, 218)
STEEL_LO = (96, 104, 120)
PLATE_HI = (54, 60, 74)
PLATE_LO = (24, 28, 38)
BG_TOP = (18, 22, 34)
BG_BOT = (6, 8, 14)
DARK = (12, 14, 20)


def vgrad(w, h, top, bot):
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        t = y / max(1, h - 1)
        c = (int(top[0] + (bot[0] - top[0]) * t),
             int(top[1] + (bot[1] - top[1]) * t),
             int(top[2] + (bot[2] - top[2]) * t))
        for x in range(w):
            px[x, y] = c
    return img


def rounded_mask(w, h, box, r):
    m = Image.new("L", (w, h), 0)
    d = ImageDraw.Draw(m)
    d.rounded_rectangle(box, radius=r, fill=255)
    return m


def glow(layer, blur, passes=1):
    g = layer
    for _ in range(passes):
        g = g.filter(ImageFilter.GaussianBlur(blur))
    return g


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


EMOS = {
    "neutral":   dict(eye=(70, 200, 255), shape="round", brow="flat",  mouth="grille"),
    "happy":     dict(eye=(90, 235, 150), shape="arc",   brow="none",  mouth="smile"),
    "laughing":  dict(eye=(90, 235, 150), shape="arc",   brow="none",  mouth="open"),
    "excited":   dict(eye=(120, 230, 255), shape="wide", brow="up",    mouth="smallO"),
    "love":      dict(eye=(255, 110, 150), shape="heart", brow="none", mouth="smile"),
    "curious":   dict(eye=(120, 210, 255), shape="round", brow="oneup", mouth="small"),
    "surprised": dict(eye=(120, 220, 255), shape="wide", brow="up",    mouth="bigO"),
    "sad":       dict(eye=(90, 150, 245), shape="low",   brow="sad",   mouth="frown"),
    "angry":     dict(eye=(255, 70, 60),  shape="narrow", brow="angry", mouth="frown"),
    "sleepy":    dict(eye=(110, 130, 200), shape="half", brow="flat",  mouth="small"),
}


def draw_eye(base, glow_layer, cx, cy, p):
    col = p["eye"]
    shape = p["shape"]
    db = ImageDraw.Draw(base)
    dg = ImageDraw.Draw(glow_layer)
    r = 26 * SS
    if shape in ("round", "wide", "low", "narrow"):
        rr = r if shape == "round" else (int(r * 1.12) if shape == "wide" else r)
        yy = cy + (10 * SS if shape == "low" else 0)
        if shape == "narrow":
            db.ellipse([cx - rr, yy - rr // 2, cx + rr, yy + rr // 2], fill=col)
            dg.ellipse([cx - rr, yy - rr // 2, cx + rr, yy + rr // 2], fill=col)
        else:
            db.ellipse([cx - rr, yy - rr, cx + rr, yy + rr], fill=col)
            dg.ellipse([cx - rr, yy - rr, cx + rr, yy + rr], fill=col)
            db.ellipse([cx - rr // 3, yy - rr // 2, cx + rr // 3, yy], fill=(255, 255, 255))
    elif shape == "arc":
        for w_ in range(0, 360, 6):
            a0 = 200 + w_ * 0
        db.arc([cx - r, cy - r, cx + r, cy + r], 200, 340, fill=col, width=8 * SS)
        dg.arc([cx - r, cy - r, cx + r, cy + r], 200, 340, fill=col, width=10 * SS)
    elif shape == "half":
        db.chord([cx - r, cy - r, cx + r, cy + r], 20, 160, fill=col)
        dg.chord([cx - r, cy - r, cx + r, cy + r], 20, 160, fill=col)
    elif shape == "heart":
        s = r
        for layer, dd in ((base, db), (glow_layer, dg)):
            dd.polygon([(cx, cy + s), (cx - s, cy - s // 4), (cx + s, cy - s // 4)], fill=col)
            dd.ellipse([cx - s, cy - s, cx, cy], fill=col)
            dd.ellipse([cx, cy - s, cx + s, cy], fill=col)


def draw_brow(base, cx, cy, kind, side):
    if kind == "none":
        return
    d = ImageDraw.Draw(base)
    col = (180, 188, 200)
    w = 9 * SS
    dx = 30 * SS
    yo = 44 * SS
    if kind == "flat":
        d.line([cx - dx, cy - yo, cx + dx, cy - yo], fill=col, width=w)
    elif kind == "up":
        d.line([cx - dx, cy - yo + 8 * SS, cx + dx, cy - yo - 8 * SS], fill=col, width=w)
    elif kind == "angry":
        if side == "L":
            d.line([cx - dx, cy - yo - 6 * SS, cx + dx, cy - yo + 14 * SS], fill=(210, 120, 110), width=w)
        else:
            d.line([cx + dx, cy - yo - 6 * SS, cx - dx, cy - yo + 14 * SS], fill=(210, 120, 110), width=w)
    elif kind == "sad":
        if side == "L":
            d.line([cx - dx, cy - yo + 12 * SS, cx + dx, cy - yo - 6 * SS], fill=col, width=w)
        else:
            d.line([cx + dx, cy - yo + 12 * SS, cx - dx, cy - yo - 6 * SS], fill=col, width=w)
    elif kind == "oneup":
        if side == "R":
            d.line([cx - dx, cy - yo + 10 * SS, cx + dx, cy - yo - 10 * SS], fill=col, width=w)
        else:
            d.line([cx - dx, cy - yo, cx + dx, cy - yo], fill=col, width=w)


def draw_mouth(base, glow_layer, p):
    d = ImageDraw.Draw(base)
    dg = ImageDraw.Draw(glow_layer)
    col = p["eye"]
    mouth = p["mouth"]
    my = 174 * SS
    if mouth == "grille":
        for i in range(-3, 4):
            x = CX + i * 11 * SS
            d.line([x, my - 13 * SS, x, my + 13 * SS], fill=lerp(col, DARK, 0.3), width=5 * SS)
        d.rounded_rectangle([CX - 44 * SS, my - 17 * SS, CX + 44 * SS, my + 17 * SS],
                            radius=8 * SS, outline=lerp(col, (255, 255, 255), 0.1), width=3 * SS)
    elif mouth == "smile":
        d.arc([CX - 48 * SS, my - 34 * SS, CX + 48 * SS, my + 28 * SS], 20, 160, fill=col, width=9 * SS)
        dg.arc([CX - 48 * SS, my - 34 * SS, CX + 48 * SS, my + 28 * SS], 20, 160, fill=col, width=11 * SS)
    elif mouth == "open":
        d.ellipse([CX - 32 * SS, my - 20 * SS, CX + 32 * SS, my + 24 * SS], fill=lerp(col, DARK, 0.55), outline=col, width=5 * SS)
    elif mouth == "bigO":
        d.ellipse([CX - 23 * SS, my - 22 * SS, CX + 23 * SS, my + 22 * SS], fill=lerp(col, DARK, 0.6), outline=col, width=5 * SS)
    elif mouth == "smallO":
        d.ellipse([CX - 13 * SS, my - 13 * SS, CX + 13 * SS, my + 13 * SS], fill=lerp(col, DARK, 0.6), outline=col, width=4 * SS)
    elif mouth == "frown":
        d.arc([CX - 48 * SS, my, CX + 48 * SS, my + 60 * SS], 200, 340, fill=col, width=8 * SS)
    elif mouth == "small":
        d.line([CX - 18 * SS, my, CX + 18 * SS, my], fill=col, width=6 * SS)


def draw_mouth_open(d, dg, p):
    # generic talking (open) mouth used for the "_t" frame of every emotion
    col = p["eye"]
    my = 174 * SS
    d.ellipse([CX - 27 * SS, my - 22 * SS, CX + 27 * SS, my + 26 * SS],
              fill=lerp(col, DARK, 0.5), outline=col, width=6 * SS)
    dg.ellipse([CX - 27 * SS, my - 22 * SS, CX + 27 * SS, my + 26 * SS],
               outline=col, width=8 * SS)


def render(emo, talk=False):
    p = EMOS[emo]
    w, h = W * SS, H * SS
    # background radial-ish (vertical) gradient
    img = vgrad(w, h, BG_TOP, BG_BOT)
    glow_layer = Image.new("RGB", (w, h), (0, 0, 0))

    # ---- antennae (drawn first; tips fully inside the frame with top margin) ----
    for ax in (122 * SS, 198 * SS):
        ImageDraw.Draw(img).line([ax, 50 * SS, ax - 5 * SS, 22 * SS], fill=(150, 156, 168), width=5 * SS)
        ImageDraw.Draw(glow_layer).ellipse([ax - 13 * SS, 12 * SS, ax + 1 * SS, 26 * SS], fill=p["eye"])
        ImageDraw.Draw(img).ellipse([ax - 11 * SS, 14 * SS, ax - 1 * SS, 24 * SS], fill=p["eye"])

    # ---- head (metallic) ----
    head_box = [44 * SS, 48 * SS, (W - 44) * SS, 230 * SS]
    head = vgrad(w, h, STEEL_HI, STEEL_LO)
    hm = rounded_mask(w, h, head_box, 62 * SS)
    img.paste(head, (0, 0), hm)
    ImageDraw.Draw(img).rounded_rectangle(head_box, radius=62 * SS, outline=(40, 44, 54), width=4 * SS)

    # ---- ear discs (headphones) ----
    for ex in (50 * SS, (W - 50) * SS):
        ImageDraw.Draw(img).ellipse([ex - 24 * SS, 120 * SS, ex + 24 * SS, 168 * SS], fill=(40, 46, 58))
        ImageDraw.Draw(glow_layer).ellipse([ex - 15 * SS, 130 * SS, ex + 15 * SS, 158 * SS], fill=p["eye"])
        ImageDraw.Draw(img).ellipse([ex - 12 * SS, 132 * SS, ex + 12 * SS, 156 * SS], fill=lerp(p["eye"], DARK, 0.25))

    # ---- dark face plate ----
    plate_box = [84 * SS, 80 * SS, (W - 84) * SS, 214 * SS]
    plate = vgrad(w, h, PLATE_HI, PLATE_LO)
    pm = rounded_mask(w, h, plate_box, 44 * SS)
    img.paste(plate, (0, 0), pm)
    ImageDraw.Draw(img).rounded_rectangle(plate_box, radius=44 * SS, outline=(70, 78, 92), width=3 * SS)

    # ---- eyes ----
    eyL, eyR, eyy = 126 * SS, 194 * SS, 126 * SS
    draw_eye(img, glow_layer, eyL, eyy, p)
    draw_eye(img, glow_layer, eyR, eyy, p)
    draw_brow(img, eyL, eyy, p["brow"], "L")
    draw_brow(img, eyR, eyy, p["brow"], "R")

    # ---- mouth ----
    if talk:
        draw_mouth_open(ImageDraw.Draw(img), ImageDraw.Draw(glow_layer), p)
    else:
        draw_mouth(img, glow_layer, p)

    # extras
    if emo == "sleepy":
        d = ImageDraw.Draw(img)
        d.text  # noop guard
        for (zx, zy, zs) in ((232 * SS, 60 * SS, 6), (250 * SS, 40 * SS, 9)):
            d.line([zx, zy, zx + zs * SS, zy], fill=(150, 160, 200), width=3 * SS)
            d.line([zx + zs * SS, zy, zx, zy + zs * SS], fill=(150, 160, 200), width=3 * SS)
            d.line([zx, zy + zs * SS, zx + zs * SS, zy + zs * SS], fill=(150, 160, 200), width=3 * SS)

    # composite glow under sharp
    gl = glow(glow_layer, 9 * SS, passes=1)
    img = Image.blend(img, Image.new("RGB", (w, h)), 0)  # keep type
    out = Image.new("RGB", (w, h))
    out.paste(img)
    # screen-blend the glow
    import PIL.ImageChops as IC
    out = IC.screen(out, gl)
    # re-draw sharp cores over glow by re-pasting bright eye/mouth from img where bright
    out = IC.lighter(out, img)

    out = out.resize((W, H), Image.LANCZOS)
    return out


def main(argv):
    out = argv[1] if len(argv) > 1 else "."
    os.makedirs(out, exist_ok=True)
    for emo in EMOS:
        render(emo, talk=False).save(os.path.join(out, "f_%s.jpg" % emo), "JPEG", quality=88)
        render(emo, talk=True).save(os.path.join(out, "f_%s_t.jpg" % emo), "JPEG", quality=88)
        print("rendered", emo, "(+ _t)")


if __name__ == "__main__":
    sys.exit(main(sys.argv))
