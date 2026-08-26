#!/usr/bin/env python3
"""Bake DejaVu Sans Mono into a 12x24 bitmap font as a C array.

The status screen draws straight into the framebuffer, with no Android and no
font engine, so the glyphs have to be in the binary.  Turkish letters are
included deliberately: leaving them out would mean writing "sinyal guclu"
instead of "sinyal güçlü" on a screen that is meant to be read at a glance.
"""
from PIL import Image, ImageDraw, ImageFont

W, H = 12, 24
YOL = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"

# ASCII 32..126, then the Turkish letters, then degree.
karakterler = [chr(c) for c in range(32, 127)]
karakterler += list("ğĞüÜşŞıİöÖçÇ°")

# Find the pixel size whose advance width is exactly W.
boyut = None
for s in range(8, 40):
    f = ImageFont.truetype(YOL, s)
    if f.getbbox("M")[2] - f.getbbox("M")[0] <= W - 1 and f.getlength("M") <= W:
        boyut = s
    else:
        break
f = ImageFont.truetype(YOL, boyut)
print("punto:", boyut, "ilerleme:", f.getlength("M"))

# Vertical placement: put the baseline so that ascender and descender both fit.
asc, desc = f.getmetrics()
ust = max(0, (H - (asc + desc)) // 2)

satirlar = []
for k in karakterler:
    im = Image.new("L", (W, H), 0)
    d = ImageDraw.Draw(im)
    d.text((0, ust), k, font=f, fill=255)
    px = im.load()
    baytlar = []
    for y in range(H):
        bit = 0
        for x in range(W):
            if px[x, y] > 110:
                bit |= (1 << (W - 1 - x))
        baytlar.append((bit >> 8) & 0xFF)
        baytlar.append(bit & 0xFF)
    satirlar.append((k, baytlar))

with open("font.h", "w", encoding="utf-8") as c:
    c.write("/* Uretilmis dosya - font-uret.py.  Elle duzenleme. */\n")
    c.write("/* DejaVu Sans Mono, %dx%d, %d glif.  Bunifold licence: public domain-ish\n"
            "   (DejaVu Fonts License, ekli). */\n" % (W, H, len(karakterler)))
    c.write("#define FONT_G %d\n#define FONT_Y %d\n#define FONT_N %d\n\n" % (W, H, len(karakterler)))
    c.write("static const unsigned char FONT[FONT_N][FONT_Y * 2] = {\n")
    for k, b in satirlar:
        gorunur = k if k.isprintable() and k != "\\" else "?"
        c.write("    { " + ", ".join("0x%02X" % x for x in b) + " },  /* %s */\n" % gorunur)
    c.write("};\n\n")
    # UTF-8 codepoint -> glyph index.  ASCII is direct; the rest is a table.
    c.write("static const unsigned short FONT_KOD[] = {\n    ")
    kodlar = [ord(k) for k in karakterler]
    c.write(", ".join(str(x) for x in kodlar))
    c.write("\n};\n")
print("font.h yazildi:", len(karakterler), "glif")
