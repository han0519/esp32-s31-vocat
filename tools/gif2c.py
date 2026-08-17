"""
Convert animated GIFs into LVGL RGB565 frame arrays (C source) for the 喵伴
firmware. Usage:

    python gif2c.py <gif> <out.c> <out.h> <width> <height> <max_frames> [step]

- Every frame is resized to <width>x<height> (LANCZOS) and stored as raw
  RGB565 (16-bit, little-endian) bytes.
- <max_frames> caps how many frames are emitted (frames sampled evenly).
- [step] optionally decimates the animation (keep every N-th frame) to shrink
  flash usage while keeping motion smooth enough.
- Output is C arrays: `const uint8_t gif_xxx_frames[][W*H*2]` plus a descriptor
  table, ready to be #included and animated with lv_anim / lv_image.
"""
import os
import sys
from PIL import Image


def rgb565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def main():
    if len(sys.argv) < 7:
        print(__doc__)
        sys.exit(1)
    src, outc, outh, W, H, max_frames = sys.argv[1:7]
    W, H, max_frames = int(W), int(H), int(max_frames)
    step = int(sys.argv[7]) if len(sys.argv) > 7 else 1

    im = Image.open(src)
    n_frames = getattr(im, "n_frames", 1)
    indices = list(range(0, n_frames, step))
    if len(indices) > max_frames:
        # sample evenly
        idx = [round(i * (len(indices) - 1) / (max_frames - 1)) for i in range(max_frames)]
        indices = [indices[i] for i in idx]

    base = os.path.splitext(os.path.basename(outc))[0]
    name = "gif_" + base
    frames = []
    for fi in indices:
        im.seek(fi)
        # convert to RGBA, composite on transparent if needed
        rgba = im.convert("RGBA")
        bg = Image.new("RGBA", rgba.size, (0, 0, 0, 0))
        bg.alpha_composite(rgba)
        frame = bg.convert("RGB").resize((W, H), Image.LANCZOS)
        px = frame.load()
        buf = bytearray()
        for y in range(H):
            for x in range(W):
                r, g, b = px[x, y][:3]
                v = rgb565(r, g, b)
                buf += bytes([v & 0xFF, (v >> 8) & 0xFF])
        frames.append(bytes(buf))

    with open(outh, "w", encoding="utf-8") as f:
        f.write("#pragma once\n")
        f.write("#include <stdint.h>\n\n")
        f.write(f"#define {name.upper()}_FRAMES {len(frames)}\n")
        f.write(f"#define {name.upper()}_W {W}\n")
        f.write(f"#define {name.upper()}_H {H}\n")
        f.write(f"extern const uint8_t {name}_frames[{len(frames)}][{W * H * 2}];\n")
        f.write(f"extern const uint8_t {name}_durations[{len(frames)}];\n")

    with open(outc, "w", encoding="utf-8") as f:
        f.write(f"/* Auto-generated from {os.path.basename(src)} — do not edit. */\n")
        f.write('#include "' + os.path.basename(outh) + '"\n\n')
        f.write(f"const uint8_t {name}_frames[{len(frames)}][{W * H * 2}] = {{\n")
        for k, fr in enumerate(frames):
            f.write(f"  /* frame {k} */\n  {{")
            for i in range(0, len(fr), 12):
                f.write("\\x%02x\\x%02x\\x%02x\\x%02x\\x%02x\\x%02x\\x%02x\\x%02x\\x%02x\\x%02x\\x%02x\\x%02x" %
                        tuple(fr[i:i + 12]))
            f.write("},\n")
        f.write("};\n\n")
        # durations in ms per frame (sampled by step)
        durs = []
        for fi in indices:
            im.seek(fi)
            d = im.info.get("duration", 0) or 80
            durs.append(min(max(int(d) * step, 20), 500))
        f.write(f"const uint8_t {name}_durations[{len(frames)}] = {{{', '.join(map(str, durs))}}};\n")
    print(f"OK {len(frames)} frames, {W}x{H} RGB565 -> {outc} / {outh}")


if __name__ == "__main__":
    main()
