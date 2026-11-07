#!/usr/bin/env python3
"""Convert an image for the FW-MAME framebuffer: W x H pixels, 8 bits each,
and a 256-entry RGB333 palette (512 possible colors on the ladder board).

    python host/img2fb.py photo.jpg out --size 256x224    # writes out.raw, out.pal
    scp out.raw out.pal pi@10.12.194.1:/tmp/
    sudo /tmp/fbsend image /tmp/out.raw /tmp/out.pal --size 256x224   (on the CM0)

The image is scaled to fill W x H (aspect ratio is not kept: arcade pixels
aren't square either), reduced to RGB333 with Floyd-Steinberg dithering, and
then to at most 256 of those colors. out.pal is 256 little-endian uint16
values, bits 8:0 = {R[2:0], G[2:0], B[2:0]}. Needs Pillow.
"""
import argparse
import struct
from collections import Counter

from PIL import Image


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("image")
    p.add_argument("out", help="output name without extension")
    p.add_argument("--size", default="256x224")
    p.add_argument("--no-dither", action="store_true")
    args = p.parse_args()
    w, h = (int(v) for v in args.size.lower().split("x"))

    img = Image.open(args.image).convert("RGB").resize((w, h), Image.LANCZOS)
    # RGB333 with error diffusion, done by hand so all 512 colors are available.
    levels = [round(v * 255 / 7) for v in range(8)]
    px = [list(img.getpixel((x, y))) for y in range(h) for x in range(w)]
    codes = [0] * (w * h)
    for y in range(h):
        for x in range(w):
            i = y * w + x
            q = [min(7, max(0, round(c * 7 / 255))) for c in px[i]]
            codes[i] = q[0] << 6 | q[1] << 3 | q[2]
            if args.no_dither:
                continue
            err = [px[i][k] - levels[q[k]] for k in range(3)]
            for dx, dy, f in ((1, 0, 7), (-1, 1, 3), (0, 1, 5), (1, 1, 1)):
                xx, yy = x + dx, y + dy
                if 0 <= xx < w and yy < h:
                    j = yy * w + xx
                    for k in range(3):
                        px[j][k] += err[k] * f / 16

    used = [c for c, _ in Counter(codes).most_common()]
    if len(used) > 256:
        # Keep the 256 most used colors; map the rest to the nearest kept one.
        keep = used[:256]
        def nearest(c):
            r, g, b = c >> 6, c >> 3 & 7, c & 7
            return min(keep, key=lambda k: (r - (k >> 6)) ** 2 + (g - (k >> 3 & 7)) ** 2 + (b - (k & 7)) ** 2)
        remap = {c: (c if c in keep else nearest(c)) for c in used}
        codes = [remap[c] for c in codes]
        used = keep
    index = {c: i for i, c in enumerate(used)}
    pal = used + [0] * (256 - len(used))

    with open(args.out + ".raw", "wb") as f:
        f.write(bytes(index[c] for c in codes))
    with open(args.out + ".pal", "wb") as f:
        f.write(struct.pack("<256H", *pal))
    print(f"{args.out}.raw ({w}x{h}), {args.out}.pal: {len(used)} colors")


if __name__ == "__main__":
    main()
