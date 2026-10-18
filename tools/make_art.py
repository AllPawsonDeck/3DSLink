#!/usr/bin/env python3
"""Draw the HOME Menu icon (48x48) and banner (256x128) for 3DSLink.

The motif nods to the Download Play icon: a 3DS on the left beaming its
screens over Wi-Fi to a computer on the right. Everything is drawn at 8x and
downsampled so edges stay smooth at icon size.

    python3 tools/make_art.py      # writes app/assets/{icon,banner}.png + banner.wav
"""
import struct
import wave
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "app" / "assets"

ORANGE = (255, 154, 31)
ORANGE_DARK = (214, 112, 6)
BODY = (46, 104, 214)        # 3DS shell
BODY_DARK = (31, 74, 160)
SCREEN = (178, 226, 255)
SCREEN_2 = (120, 196, 245)
INK = (38, 43, 52)           # laptop bezel
METAL = (190, 199, 210)
METAL_DARK = (150, 160, 172)
WHITE = (255, 255, 255)


def gradient(size, top, bottom):
    w, h = size
    img = Image.new("RGBA", size)
    px = img.load()
    for y in range(h):
        t = y / max(1, h - 1)
        c = tuple(round(a + (b - a) * t) for a, b in zip(top, bottom)) + (255,)
        for x in range(w):
            px[x, y] = c
    return img


def draw_3ds(d, x, y, w, s):
    """Open 3DS seen from the front. (x, y) is the top-left, w the width."""
    h_top = w * 0.66
    r = w * 0.09
    # top half
    d.rounded_rectangle([x, y, x + w, y + h_top], r, fill=BODY, outline=BODY_DARK, width=max(1, s))
    m = w * 0.1
    d.rounded_rectangle([x + m, y + m * 0.9, x + w - m, y + h_top - m * 0.9], r * 0.4, fill=SCREEN)
    # a little "game" on the top screen
    d.rectangle([x + m, y + h_top * 0.62, x + w - m, y + h_top - m * 0.9], fill=SCREEN_2)
    # hinge
    hy = y + h_top
    d.rounded_rectangle([x + w * 0.06, hy - s * 0.5, x + w * 0.94, hy + w * 0.08], r * 0.5, fill=BODY_DARK)
    # bottom half
    by = hy + w * 0.06
    bh = w * 0.62
    d.rounded_rectangle([x, by, x + w, by + bh], r, fill=BODY, outline=BODY_DARK, width=max(1, s))
    bm = w * 0.25
    d.rounded_rectangle([x + bm, by + bh * 0.16, x + w - bm, by + bh * 0.78], r * 0.4, fill=SCREEN)
    # circle pad and buttons
    cr = w * 0.07
    cx, cy = x + w * 0.125, by + bh * 0.3
    d.ellipse([cx - cr, cy - cr, cx + cr, cy + cr], fill=BODY_DARK)
    for dx, dy in ((0, -1), (1, 0), (0, 1), (-1, 0)):
        bx, byy = x + w * 0.875 + dx * w * 0.045, by + bh * 0.3 + dy * w * 0.045
        br = w * 0.025
        d.ellipse([bx - br, byy - br, bx + br, byy + br], fill=WHITE)


def draw_waves(d, cx, cy, radii, width, color):
    for rad in radii:
        d.arc([cx - rad, cy - rad, cx + rad, cy + rad], -42, 42, fill=color, width=width)


def draw_laptop(d, x, y, w, s):
    """Laptop/Chromebook showing the two streamed screens."""
    sh = w * 0.62
    r = w * 0.06
    d.rounded_rectangle([x, y, x + w, y + sh], r, fill=INK)
    m = w * 0.07
    d.rectangle([x + m, y + m, x + w - m, y + sh - m * 0.8], fill=(18, 20, 25))
    # the streamed 3DS screens, stacked like the viewer's "3DS" layout
    ax, ay = x + m, y + m
    aw, ah = w - 2 * m, sh - 1.8 * m
    gap = 34
    k = min(aw / 400, ah / (480 + gap)) * 0.9
    tw, th = 400 * k, 240 * k
    tx = ax + (aw - tw) / 2
    ty = ay + (ah - (480 + gap) * k) / 2
    d.rectangle([tx, ty, tx + tw, ty + th], fill=SCREEN)
    d.rectangle([tx, ty + th * 0.62, tx + tw, ty + th], fill=SCREEN_2)
    bx = ax + (aw - 320 * k) / 2
    by = ty + th + gap * k
    d.rectangle([bx, by, bx + 320 * k, by + th], fill=SCREEN)
    # base
    base_y = y + sh
    d.polygon([(x - w * 0.12, base_y + w * 0.1), (x + w * 1.12, base_y + w * 0.1),
               (x + w, base_y), (x, base_y)], fill=METAL)
    d.rectangle([x - w * 0.12, base_y + w * 0.1, x + w * 1.12, base_y + w * 0.13], fill=METAL_DARK)


def make_icon():
    s = 8
    img = gradient((48 * s, 48 * s), (255, 255, 255), (214, 232, 248))
    d = ImageDraw.Draw(img)
    draw_3ds(d, 2 * s, 9.5 * s, 16 * s, s)
    draw_waves(d, 18.5 * s, 20 * s, [3.5 * s, 7 * s, 10.5 * s], int(2.2 * s), ORANGE)
    draw_laptop(d, 31.5 * s, 14.5 * s, 14.5 * s, s)
    return img.resize((48, 48), Image.LANCZOS)


def find_font(size):
    for name in ("DejaVuSans-Bold.ttf", "LiberationSans-Bold.ttf", "Arial Bold.ttf"):
        for base in ("/usr/share/fonts/truetype/dejavu", "/usr/share/fonts/truetype/liberation",
                     "/usr/share/fonts/TTF", "/Library/Fonts", ""):
            try:
                return ImageFont.truetype(str(Path(base) / name) if base else name, size)
            except OSError:
                continue
    return ImageFont.load_default()


def make_banner():
    s = 4
    W, H = 256 * s, 128 * s
    img = gradient((W, H), (255, 255, 255), (206, 227, 247))
    d = ImageDraw.Draw(img)
    draw_3ds(d, 26 * s, 13 * s, 54 * s, s)
    draw_waves(d, 89 * s, 40 * s, [10 * s, 20 * s, 30 * s], int(5 * s), ORANGE)
    draw_laptop(d, 144 * s, 16 * s, 80 * s, s)
    font = find_font(19 * s)
    text = "3DSLink"
    tw = d.textlength(text, font=font)
    d.text(((W - tw) / 2, 100 * s), text, font=font, fill=INK)
    return img.resize((256, 128), Image.LANCZOS)


def make_silent_wav(path):
    # Silent on purpose. It must be 16-bit *stereo* 44.1 kHz: every build
    # whose banner sound was 22 kHz mono made the HOME Menu go "BIBIBI".
    rate = 44100
    with wave.open(str(path), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(struct.pack("<hh", 0, 0) * (rate * 3 // 2))


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    make_icon().convert("RGB").save(OUT / "icon.png")
    make_banner().convert("RGB").save(OUT / "banner.png")
    make_silent_wav(OUT / "banner.wav")
    print("wrote", OUT)


if __name__ == "__main__":
    main()
