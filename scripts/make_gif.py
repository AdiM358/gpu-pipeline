#!/usr/bin/env python3
"""Assemble frame_NNN.ppm files rendered by the demo into an animated GIF.

The frames are exactly what the RTL wrote to its framebuffer (read back
through the hardware read port); this script only changes the container
format and scales 2x with nearest-neighbour so pixels stay visible.

usage: make_gif.py FRAME_DIR OUT.gif [--scale 2] [--ms 40]
"""
import argparse
import glob
import os

from PIL import Image


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("frames")
    ap.add_argument("out")
    ap.add_argument("--scale", type=int, default=2)
    ap.add_argument("--ms", type=int, default=40, help="frame duration")
    args = ap.parse_args()

    paths = sorted(glob.glob(os.path.join(args.frames, "frame_*.ppm")))
    if not paths:
        raise SystemExit(f"no frame_*.ppm in {args.frames}")
    frames = []
    for p in paths:
        im = Image.open(p).convert("RGB")
        if args.scale != 1:
            im = im.resize((im.width * args.scale, im.height * args.scale), Image.NEAREST)
        frames.append(im.quantize(colors=256, method=Image.MEDIANCUT))
    frames[0].save(args.out, save_all=True, append_images=frames[1:], duration=args.ms, loop=0,
                   optimize=True)
    print(f"wrote {args.out}: {len(frames)} frames, {os.path.getsize(args.out)} bytes")


if __name__ == "__main__":
    main()
