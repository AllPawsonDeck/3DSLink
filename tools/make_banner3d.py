#!/usr/bin/env python3
"""EXPERIMENTAL: build a 3D HOME Menu banner (build/banner3d/banner.cgfx).

Not used by the build. The banner this made for 0.2.4 showed nothing and froze
the HOME Menu on real hardware, so treat its output as untested.

The scene is a small pop-up diorama seen through the HOME Menu banner camera:
a backdrop, an open 3DS whose top screen leans back, Wi-Fi waves that pulse
towards the viewer, a laptop whose screen mirrors the 3DS, and the 3DSLink
logo floating in front. Each piece is a textured quad at its own depth, so
the 3D slider separates them.

The glTF scene is converted with pycgfx (https://github.com/skyfloogle/pycgfx),
which must be cloned somewhere and passed with --pycgfx:

    git clone https://github.com/skyfloogle/pycgfx /tmp/pycgfx
    pip install gltflib pillow numpy
    python3 tools/make_banner3d.py --pycgfx /tmp/pycgfx

It also writes build/banner3d/preview.png (left eye, right eye and a red/cyan
anaglyph) so the composition can be checked without a 3DS.
"""
import argparse
import base64
import json
import math
import struct
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
WORK = ROOT / "build" / "banner3d"
OUT = WORK / "banner.cgfx"

# HOME Menu banner camera (from pycgfx's banner-camera.gltf).
CAM = np.array([0.0, 1.0, 44.786])
FOV_Y = 0.523599
ASPECT = 5 / 3

ORANGE = (255, 154, 31, 255)
BODY = (46, 104, 214, 255)
BODY_DARK = (31, 74, 160, 255)
SCREEN = (178, 226, 255, 255)
SCREEN_2 = (120, 196, 245, 255)
INK = (38, 43, 52, 255)
METAL = (196, 204, 214, 255)
METAL_DARK = (150, 160, 172, 255)
WHITE = (255, 255, 255, 255)
CLEAR = (0, 0, 0, 0)

SS = 4  # supersampling for smooth sprite edges


# ---- sprites -----------------------------------------------------------------

def sprite(w, h, draw_fn):
    """Draw at SS x resolution and downsample; returns an RGBA image w x h."""
    im = Image.new("RGBA", (w * SS, h * SS), CLEAR)
    draw_fn(ImageDraw.Draw(im), w * SS, h * SS)
    return im.resize((w, h), Image.LANCZOS)


def backdrop(d, w, h):
    for y in range(h):
        t = y / (h - 1)
        c = tuple(round(a + (b - a) * t) for a, b in zip((255, 255, 255), (196, 222, 247))) + (255,)
        d.line([(0, y), (w, y)], fill=c)
    # soft dots for a bit of texture
    for i in range(18):
        x = (i * 97) % w
        y = (i * 53) % h
        r = w * (0.02 + 0.015 * (i % 3))
        d.ellipse([x - r, y - r, x + r, y + r], fill=(230, 240, 252, 255))


def ds_top(d, w, h):
    r = w * 0.08
    d.rounded_rectangle([0, 0, w - 1, h - 1], r, fill=BODY, outline=BODY_DARK, width=SS * 3)
    m = w * 0.09
    d.rounded_rectangle([m, m, w - m, h - m * 1.1], r * 0.4, fill=SCREEN)
    d.rectangle([m, h * 0.6, w - m, h - m * 1.1], fill=SCREEN_2)  # a little "game"
    cx, cy, cr = w * 0.5, h * 0.36, w * 0.07
    d.ellipse([cx - cr, cy - cr, cx + cr, cy + cr], fill=ORANGE)  # the player


def ds_bottom(d, w, h):
    r = w * 0.08
    d.rounded_rectangle([0, 0, w - 1, h - 1], r, fill=BODY, outline=BODY_DARK, width=SS * 3)
    d.rectangle([w * 0.06, 0, w * 0.94, h * 0.07], fill=BODY_DARK)  # hinge
    bm = w * 0.24
    d.rounded_rectangle([bm, h * 0.17, w - bm, h * 0.8], r * 0.4, fill=SCREEN)
    cr = w * 0.075
    cx, cy = w * 0.12, h * 0.3
    d.ellipse([cx - cr, cy - cr, cx + cr, cy + cr], fill=BODY_DARK)
    for dx, dy in ((0, -1), (1, 0), (0, 1), (-1, 0)):
        bx, by, br = w * 0.88 + dx * w * 0.045, h * 0.3 + dy * w * 0.045, w * 0.025
        d.ellipse([bx - br, by - br, bx + br, by + br], fill=WHITE)


def wave(d, w, h):
    # One Wi-Fi arc; its circle centre is the middle of the left edge.
    rad = w * 0.92
    t = w * 0.16
    d.arc([-rad, h / 2 - rad, rad, h / 2 + rad], -40, 40, fill=ORANGE, width=int(t))


def laptop_screen(d, w, h):
    d.rounded_rectangle([0, 0, w - 1, h - 1], w * 0.05, fill=INK)
    m = w * 0.06
    d.rectangle([m, m, w - m, h - m * 0.8], fill=(18, 20, 25, 255))
    # the two streamed screens, like the viewer's "3DS" layout
    ah = h - m * 2.2
    k = ah / (480 + 34) * 0.92
    tw, th = 400 * k, 240 * k
    tx, ty = (w - tw) / 2, m + (ah - (480 + 34) * k) / 2 + m * 0.2
    d.rectangle([tx, ty, tx + tw, ty + th], fill=SCREEN)
    d.rectangle([tx, ty + th * 0.6, tx + tw, ty + th], fill=SCREEN_2)
    cx, cy, cr = tx + tw * 0.5, ty + th * 0.36, tw * 0.07
    d.ellipse([cx - cr, cy - cr, cx + cr, cy + cr], fill=ORANGE)
    bx, by = (w - 320 * k) / 2, ty + th + 34 * k
    d.rectangle([bx, by, bx + 320 * k, by + th], fill=SCREEN)


def laptop_base(d, w, h):
    d.polygon([(w * 0.08, 0), (w * 0.92, 0), (w - 1, h * 0.72), (0, h * 0.72)], fill=METAL)
    d.rectangle([0, h * 0.72, w - 1, h - 1], fill=METAL_DARK)
    # keys
    for row in range(3):
        y0 = h * (0.12 + row * 0.18)
        inset = w * (0.12 - row * 0.02)
        n = 12
        kw = (w - 2 * inset) / n
        for i in range(n):
            x0 = inset + i * kw
            d.rectangle([x0 + kw * 0.12, y0, x0 + kw * 0.88, y0 + h * 0.12], fill=(170, 179, 190, 255))


def find_font(size):
    for path in ("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
                 "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf"):
        try:
            return ImageFont.truetype(path, size)
        except OSError:
            pass
    return ImageFont.load_default()


def logo(d, w, h):
    font = find_font(int(h * 0.72))
    text = "3DSLink"
    tw = d.textlength(text, font=font)
    x, y = (w - tw) / 2, h * 0.08
    d.text((x, y), text, font=font, fill=INK, stroke_width=int(h * 0.06), stroke_fill=WHITE)


# ---- scene -------------------------------------------------------------------

def quat_x(deg):
    a = math.radians(deg) / 2
    return [math.sin(a), 0.0, 0.0, math.cos(a)]


# Layers are listed back to front (the order transparent quads are drawn in).
# size: world units; origin: where the node sits; pivot: point of the quad (in
# its own 0..1 coordinates, y up) placed at the node origin.
def layers():
    return [
        dict(name="backdrop", tex=sprite(64, 64, backdrop), size=(56, 34), origin=(0, 1, -12),
             pivot=(0.5, 0.5), alpha="OPAQUE"),
        dict(name="laptop_screen", tex=sprite(128, 96, laptop_screen), size=(12.5, 8.6),
             origin=(10.2, -2.6, -2.0), pivot=(0.5, 0.0), rot=-14),
        dict(name="laptop_base", tex=sprite(128, 32, laptop_base), size=(14.5, 3.4),
             origin=(10.2, -2.6, -0.6), pivot=(0.5, 1.0)),
        dict(name="ds_top", tex=sprite(128, 96, ds_top), size=(10.5, 7.6),
             origin=(-10.0, 1.2, 0.0), pivot=(0.5, 0.0), rot=-22),
        dict(name="ds_bottom", tex=sprite(128, 96, ds_bottom), size=(10.5, 7.0),
             origin=(-10.0, 1.4, 0.4), pivot=(0.5, 1.0)),
        dict(name="wave1", tex=sprite(64, 128, wave), size=(2.4, 4.6), origin=(-3.6, 3.2, 1.5),
             pivot=(0.0, 0.5), pulse=0.0),
        dict(name="wave2", tex=None, share="wave1", size=(4.2, 8.0), origin=(-3.6, 3.2, 3.0),
             pivot=(0.0, 0.5), pulse=0.33),
        dict(name="wave3", tex=None, share="wave1", size=(6.0, 11.4), origin=(-3.6, 3.2, 4.5),
             pivot=(0.0, 0.5), pulse=0.66),
        dict(name="logo", tex=sprite(256, 64, logo), size=(15.0, 3.75), origin=(0, -7.6, 4.0),
             pivot=(0.5, 0.5)),
    ]


def pot(n):
    p = 8
    while p < n:
        p *= 2
    return min(p, 256)


def build_gltf(items, path):
    buf = bytearray()
    views, accessors, meshes, materials, textures, images, nodes = [], [], [], [], [], [], []
    tex_index = {}

    def add_view(data, target=None):
        while len(buf) % 4:
            buf.append(0)
        views.append(dict(buffer=0, byteOffset=len(buf), byteLength=len(data),
                          **({"target": target} if target else {})))
        buf.extend(data)
        return len(views) - 1

    for it in items:
        # texture: pad the sprite into a power-of-two canvas, map its sub-rect
        src = it["tex"] if it["tex"] is not None else next(x for x in items if x["name"] == it["share"])["tex"]
        key = it["share"] if it["tex"] is None else it["name"]
        W, H = pot(src.width), pot(src.height)
        if key not in tex_index:
            canvas = Image.new("RGBA", (W, H), CLEAR)
            canvas.paste(src, (0, 0))
            png = WORK / f"{key}.png"
            canvas.save(png)
            images.append(dict(uri=png.name, name=key))
            textures.append(dict(source=len(images) - 1))
            tex_index[key] = len(textures) - 1
        u1, v1 = src.width / W, src.height / H

        w, h = it["size"]
        px, py = it["pivot"]
        x0, x1 = -px * w, (1 - px) * w
        y0, y1 = -py * h, (1 - py) * h
        pos = [(x0, y0, 0), (x1, y0, 0), (x1, y1, 0), (x0, y1, 0)]
        uv = [(0, v1), (u1, v1), (u1, 0), (0, 0)]  # glTF: v=0 is the top of the image
        verts = b"".join(struct.pack("<3f", *p) for p in pos)
        norms = b"".join(struct.pack("<3f", 0, 0, 1) for _ in pos)
        uvs = b"".join(struct.pack("<2f", *t) for t in uv)
        idx = struct.pack("<6H", 0, 1, 2, 0, 2, 3)
        a_pos = len(accessors)
        accessors.append(dict(bufferView=add_view(verts, 34962), componentType=5126, count=4, type="VEC3",
                              min=[min(p[i] for p in pos) for i in range(3)],
                              max=[max(p[i] for p in pos) for i in range(3)]))
        accessors.append(dict(bufferView=add_view(norms, 34962), componentType=5126, count=4, type="VEC3"))
        accessors.append(dict(bufferView=add_view(uvs, 34962), componentType=5126, count=4, type="VEC2"))
        accessors.append(dict(bufferView=add_view(idx, 34963), componentType=5123, count=6, type="SCALAR"))
        materials.append(dict(name=it["name"], alphaMode=it.get("alpha", "BLEND"), doubleSided=True,
                              pbrMetallicRoughness=dict(baseColorTexture=dict(index=tex_index[key]),
                                                        metallicFactor=0, roughnessFactor=1)))
        meshes.append(dict(name=it["name"], primitives=[dict(
            attributes=dict(POSITION=a_pos, NORMAL=a_pos + 1, TEXCOORD_0=a_pos + 2),
            indices=a_pos + 3, material=len(materials) - 1)]))
        node = dict(name=it["name"], mesh=len(meshes) - 1, translation=list(it["origin"]))
        if it.get("rot"):
            node["rotation"] = quat_x(it["rot"])
        nodes.append(node)

    # Waves pulse outwards one after another (2 s loop).
    period, steps = 2.0, 16
    times = [period * i / steps for i in range(steps + 1)]
    channels, samplers = [], []
    t_acc = len(accessors)
    accessors.append(dict(bufferView=add_view(struct.pack(f"<{len(times)}f", *times)), componentType=5126,
                          count=len(times), type="SCALAR", min=[0.0], max=[period]))
    for n, it in enumerate(items):
        if "pulse" not in it:
            continue
        vals = []
        for t in times:
            ph = (t / period - it["pulse"]) % 1.0
            s = 1.0 + 0.10 * math.sin(2 * math.pi * ph)
            vals.append((s, s, 1.0))
        acc = len(accessors)
        accessors.append(dict(bufferView=add_view(b"".join(struct.pack("<3f", *v) for v in vals)),
                              componentType=5126, count=len(vals), type="VEC3"))
        samplers.append(dict(input=t_acc, output=acc, interpolation="LINEAR"))
        channels.append(dict(sampler=len(samplers) - 1, target=dict(node=n, path="scale")))

    bin_path = WORK / "scene.bin"
    bin_path.write_bytes(bytes(buf))
    gltf = dict(
        asset=dict(version="2.0", generator="3DSLink make_banner3d.py"),
        scene=0, scenes=[dict(nodes=list(range(len(nodes))))], nodes=nodes, meshes=meshes,
        materials=materials, textures=textures, images=images,
        samplers=[dict(magFilter=9729, minFilter=9729, wrapS=33071, wrapT=33071)],
        buffers=[dict(uri=bin_path.name, byteLength=len(buf))], bufferViews=views, accessors=accessors,
        animations=[dict(name="pulse", channels=channels, samplers=samplers)],
    )
    for t in textures:
        t["sampler"] = 0
    path.write_text(json.dumps(gltf, indent=1))


# ---- preview (simple perspective renderer) --------------------------------------

def homography(src, dst):
    a = []
    for (x, y), (u, v) in zip(src, dst):
        a.append([x, y, 1, 0, 0, 0, -u * x, -u * y])
        a.append([0, 0, 0, x, y, 1, -v * x, -v * y])
    b = [c for p in dst for c in p]
    return np.linalg.solve(np.array(a, float), np.array(b, float))


def rot_x(deg):
    a = math.radians(deg)
    return np.array([[1, 0, 0], [0, math.cos(a), -math.sin(a)], [0, math.sin(a), math.cos(a)]])


def render(items, eye=0.0, W=400, H=240):
    out = Image.new("RGBA", (W, H), (0, 0, 0, 255))
    k = math.tan(FOV_Y / 2)
    d0 = CAM[2]
    for it in items:
        src = it["tex"] if it["tex"] is not None else next(x for x in items if x["name"] == it["share"])["tex"]
        w, h = it["size"]
        px, py = it["pivot"]
        local = [(-px * w, (1 - py) * h), ((1 - px) * w, (1 - py) * h), ((1 - px) * w, -py * h), (-px * w, -py * h)]
        R = rot_x(it.get("rot", 0))
        pts = []
        for lx, ly in local:
            p = R @ np.array([lx, ly, 0.0]) + np.array(it["origin"])
            d = CAM[2] - p[2]
            xn = (p[0] - (CAM[0] + eye)) / (d * k * ASPECT) + eye / (d0 * k * ASPECT)
            yn = (p[1] - CAM[1]) / (d * k)
            pts.append(((xn + 1) / 2 * W, (1 - yn) / 2 * H))
        tw, th = src.size
        coeffs = homography(pts, [(0, 0), (tw, 0), (tw, th), (0, th)])
        layer = src.transform((W, H), Image.PERSPECTIVE, tuple(coeffs), Image.BILINEAR)
        # Clip to the projected quad (transform repeats edge pixels outside it).
        mask = Image.new("L", (W, H), 0)
        ImageDraw.Draw(mask).polygon(pts, fill=255)
        alpha = Image.composite(layer.getchannel("A"), Image.new("L", (W, H), 0), mask)
        layer.putalpha(alpha)
        out.alpha_composite(layer)
    return out.convert("RGB")


def preview(items):
    left, right = render(items, -0.6), render(items, 0.6)
    ana = Image.merge("RGB", (left.getchannel("R"), right.getchannel("G"), right.getchannel("B")))
    sheet = Image.new("RGB", (400 * 2 + 30, 240 * 2 + 30), (40, 40, 40))
    sheet.paste(render(items), (10, 10))
    sheet.paste(ana, (420, 10))
    sheet.paste(left, (10, 260))
    sheet.paste(right, (420, 260))
    sheet.save(WORK / "preview.png")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pycgfx", help="path to a pycgfx checkout (omit to only render the preview)")
    args = ap.parse_args()
    WORK.mkdir(parents=True, exist_ok=True)
    items = layers()
    build_gltf(items, WORK / "scene.gltf")
    preview(items)
    print("preview:", WORK / "preview.png")
    if args.pycgfx:
        subprocess.run([sys.executable, "main.py", str(WORK / "scene.gltf"), str(OUT)],
                       cwd=args.pycgfx, check=True)
        size = OUT.stat().st_size
        print(f"wrote {OUT} ({size // 1024} KB)")
        if size > 512 * 1024:
            sys.exit("banner.cgfx is over the 3DS's 512 KB limit")


if __name__ == "__main__":
    main()
