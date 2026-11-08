#!/usr/bin/env python3
"""marquees.py - game marquees for the menu, on the FW2 screen and the CRT.

  python tools/marquees.py build       [--roms roms] [--out build-marquees]
  python tools/marquees.py install     [--out build-marquees] [--port COM14]
                                       [--wilibsp <wilibsp checkout>]
  python tools/marquees.py install-cm0 [--out build-marquees]
                                       [--host pi@10.12.194.1] [--key ~/.ssh/fw2_cm0]

build: for every ROM zip directly in --roms (the games the menu lists), find
its marquee in <roms>/marquees/<name>.png (or its parent set's, see ALIASES)
and write two files. Needs Pillow.
  <out>/fw2/<name>.fmq  fits the FW2 menu's 472x100 band, 256 colours of its own
  <out>/crt/<name>.fmq  fits the CRT menu's 320x200 picture, dithered to one
                        fixed 256-colour palette (3 bits red and green, 2 blue)
                        that the FPGA's RGB333 palette shows exactly

install: hand the FW2's SD card to this PC (the same way `fw.py install-app`
does, through MAIN), copy fw2/*.fmq to /appdata/fwmame/marquees/ and hand the
card back. Quit FW-MAME on the FW2 first.

install-cm0: copy crt/*.fmq to ~/fwmame/marquees/ on the CM0 (scp), next to
~/fwmame/roms, where the menu supervisor (cm0/fe) looks for them.

.fmq ("FW-MAME marquee"), read by fw2/apps/fwmame/main.c:
  0   "FMQ1"
  4   u16 width, u16 height (little-endian)
  8   u16 colours (1..256)
  10  palette: colours x RGB565, high byte first (the panel's wire order)
  ..  PackBits of the width*height palette indexes, row after row:
      n = 0..127: n+1 literal bytes follow; n = 129..255: the next byte
      repeats 257-n times; 128: nothing.
256 colours and run-length compression keep a file at 15-45 KB: the app reads
the SD card through MAIN in 96-byte pieces, so size is load time.
"""
import argparse
import os
import pathlib
import shutil
import struct
import sys
import time

BAND_W, BAND_H = 472, 100              # FW2 menu band
CRT_W, CRT_H = 320, 200                # CRT menu picture (fbuf: <= 65,536 pixels)
SD_DIR = ("appdata", "fwmame", "marquees")
CM0_DIR = "fwmame/marquees"


def crt_palette():
    """256 colours: red and green 3 bits, blue 2 bits, each exact in RGB333."""
    lv3 = [round(i * 255 / 7) for i in range(8)]
    lv2 = [lv3[i] for i in (0, 2, 5, 7)]
    return [(r, g, b) for r in lv3 for g in lv3 for b in lv2]
# Marquee art is named after the parent set; a clone's zip uses the parent's.
ALIASES = {"mooncrs2": "mooncrst", "crushrlf": "crush", "zookeep2": "zookeep"}


def packbits(data: bytes) -> bytes:
    out, i, n = bytearray(), 0, len(data)
    while i < n:
        run = 1
        while i + run < n and run < 128 and data[i + run] == data[i]:
            run += 1
        if run >= 3:
            out += bytes((257 - run, data[i]))
            i += run
            continue
        j = i                                   # literals up to the next run of 3
        while j < n and j - i < 128:
            if j + 2 < n and data[j] == data[j + 1] == data[j + 2]:
                break
            j += 1
        out.append(j - i - 1)
        out += data[i:j]
        i = j
    return bytes(out)


def convert(png: pathlib.Path, box_w=BAND_W, box_h=BAND_H, crt=False):
    from PIL import Image
    im = Image.open(png).convert("RGB")
    w, h = im.size
    if w / h < (box_w / box_h) / 2:
        # Far narrower than the box (Mappy's is cabinet art, not a banner):
        # the title is at the top, so take a band of the box's shape from there.
        bh = round(w * box_h / box_w)
        top = round(h * 0.02)
        im = im.crop((0, top, w, min(h, top + bh)))
        w, h = im.size
    s = min(box_w / w, box_h / h)
    im = im.resize((max(1, round(w * s)), max(1, round(h * s))), Image.LANCZOS)
    if crt:
        pimg = Image.new("P", (1, 1))
        pimg.putpalette([c for rgb in crt_palette() for c in rgb])
        q = im.quantize(palette=pimg, dither=Image.Dither.FLOYDSTEINBERG)
        used = 256
    else:
        q = im.quantize(colors=256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
        used = max(q.getdata()) + 1
    pal = q.getpalette()[: 3 * 256]
    colours = bytearray()
    for k in range(used):
        r, g, b = pal[3 * k: 3 * k + 3]
        c = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        colours += struct.pack(">H", c)
    body = packbits(bytes(q.getdata()))
    return b"FMQ1" + struct.pack("<HHH", q.width, q.height, used) + bytes(colours) + body, q.size


def build(args):
    roms, out = pathlib.Path(args.roms), pathlib.Path(args.out)
    art = roms / "marquees"
    (out / "fw2").mkdir(parents=True, exist_ok=True)
    (out / "crt").mkdir(parents=True, exist_ok=True)
    made, missing = 0, []
    for z in sorted(roms.glob("*.zip")):
        name = z.stem.lower()
        png = art / f"{name}.png"
        if not png.is_file() and name in ALIASES:
            png = art / f"{ALIASES[name]}.png"
        if not png.is_file():
            missing.append(name)
            continue
        data, size = convert(png)
        (out / "fw2" / f"{name}.fmq").write_bytes(data)
        cdata, csize = convert(png, CRT_W, CRT_H, crt=True)
        (out / "crt" / f"{name}.fmq").write_bytes(cdata)
        made += 1
        print(f"{name:10s} {png.name:14s} FW2 {size[0]}x{size[1]} {len(data) // 1024} KB, "
              f"CRT {csize[0]}x{csize[1]} {len(cdata) // 1024} KB")
    print(f"{made} marquees in {out}" + (f"; none for: {', '.join(missing)}" if missing else ""))


def install(args):
    sys.path.insert(0, str(pathlib.Path(args.wilibsp) / "tools"))
    import fw                                    # wilibsp tools/fw.py: the SD handoff through MAIN
    files = sorted((pathlib.Path(args.out) / "fw2").glob("*.fmq"))
    if not files:
        sys.exit(f"no .fmq files in {args.out}/fw2: run build first")
    baseline = fw._mounted_volumes()
    fw._set_sd_host(args.port, True)
    try:
        time.sleep(fw.SD_HANDOFF_SETTLE_SECONDS)
        dest = pathlib.Path(fw._wait_for_sd(baseline, 30)).joinpath(*SD_DIR)
        dest.mkdir(parents=True, exist_ok=True)
        for f in files:
            tmp = dest / (f.name + ".tmp")
            shutil.copyfile(f, tmp)
            with tmp.open("r+b") as h:
                os.fsync(h.fileno())
            os.replace(tmp, dest / f.name)
        print(f"installed {len(files)} marquees to {dest}")
        time.sleep(fw.SD_HANDOFF_SETTLE_SECONDS)
    finally:
        fw._set_sd_host(args.port, False)


def install_cm0(args):
    import subprocess
    files = sorted((pathlib.Path(args.out) / "crt").glob("*.fmq"))
    if not files:
        sys.exit(f"no .fmq files in {args.out}/crt: run build first")
    key = ["-i", os.path.expanduser(args.key)] if args.key else []
    subprocess.run(["ssh", *key, args.host, f"mkdir -p {CM0_DIR}"], check=True)
    subprocess.run(["scp", "-q", *key, *map(str, files), f"{args.host}:{CM0_DIR}/"], check=True)
    print(f"installed {len(files)} CRT marquees to {args.host}:~/{CM0_DIR}")


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build")
    b.add_argument("--roms", default="roms")
    b.add_argument("--out", default="build-marquees")
    i = sub.add_parser("install")
    i.add_argument("--out", default="build-marquees")
    i.add_argument("--port", default="COM14")
    i.add_argument("--wilibsp", default=os.environ.get("WILIBSP_DIR", "../TPG/wilibsp"))
    c = sub.add_parser("install-cm0")
    c.add_argument("--out", default="build-marquees")
    c.add_argument("--host", default="pi@10.12.194.1")
    c.add_argument("--key", default="~/.ssh/fw2_cm0")
    args = p.parse_args()
    {"build": build, "install": install, "install-cm0": install_cm0}[args.cmd](args)


if __name__ == "__main__":
    main()
