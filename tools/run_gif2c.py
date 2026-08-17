import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import importlib
import gif2c
importlib.reload(gif2c)

from PIL import Image

d = r'H:\espwork\esp-projects\vocat-xiaozhi'
out = os.path.join(d, 'main')


def convert(src, outc, outh, W, H, max_frames, step):
    W, H, max_frames, step = int(W), int(H), int(max_frames), int(step)
    im = Image.open(src)
    n_frames = getattr(im, 'n_frames', 1)
    indices = list(range(0, n_frames, step))
    if len(indices) > max_frames:
        idx = [round(i * (len(indices) - 1) / (max_frames - 1)) for i in range(max_frames)]
        indices = [indices[i] for i in idx]

    base = os.path.splitext(os.path.basename(outc))[0]
    name = 'gif_' + base
    frames = []
    for fi in indices:
        im.seek(fi)
        rgba = im.convert('RGBA')
        bg = Image.new('RGBA', rgba.size, (0, 0, 0, 0))
        bg.alpha_composite(rgba)
        frame = bg.convert('RGB').resize((W, H), Image.LANCZOS)
        px = frame.load()
        buf = bytearray()
        for y in range(H):
            for x in range(W):
                r, g, b = px[x, y][:3]
                v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
                buf += bytes([v & 0xFF, (v >> 8) & 0xFF])
        frames.append(bytes(buf))

    with open(outh, 'w', encoding='utf-8') as f:
        f.write('#pragma once\n#include <stdint.h>\n\n')
        f.write('#define %s_FRAMES %d\n' % (name.upper(), len(frames)))
        f.write('#define %s_W %d\n' % (name.upper(), W))
        f.write('#define %s_H %d\n' % (name.upper(), H))
        f.write('extern const uint8_t %s_frames[%d][%d];\n' % (name, len(frames), W * H * 2))
        f.write('extern const uint8_t %s_durations[%d];\n' % (name, len(frames)))

    with open(outc, 'w', encoding='utf-8') as f:
        f.write('/* Auto-generated from %s - do not edit. */\n' % os.path.basename(src))
        f.write('#include "%s"\n\n' % os.path.basename(outh))
        f.write('const uint8_t %s_frames[%d][%d] = {\n' % (name, len(frames), W * H * 2))
        for k, fr in enumerate(frames):
            f.write('  /* frame %d */\n  {\n' % k)
            # emit as hex array rows of 16 bytes
            for i in range(0, len(fr), 16):
                row = fr[i:i + 16]
                f.write('    ' + ','.join('0x%02x' % b for b in row) + ',\n')
            f.write('  },\n')
        f.write('};\n\n')
        durs = []
        for fi in indices:
            im.seek(fi)
            dms = im.info.get('duration', 0) or 80
            durs.append(min(max(int(dms) * step, 20), 500))
        f.write('const uint8_t %s_durations[%d] = {%s};\n' %
                (name, len(frames), ','.join(map(str, durs))))
    print('OK %d frames %dx%d -> %s / %s' % (len(frames), W, H, outc, outh))


for name in os.listdir(d):
    if not name.lower().endswith('.gif'):
        continue
    p = os.path.join(d, name)
    im = Image.open(p)
    w, h = im.size
    n = getattr(im, 'n_frames', 1)
    print('processing', repr(name), w, h, n)
    if w == 400 or n <= 8:
        convert(p, os.path.join(out, 'muyu_gif.c'), os.path.join(out, 'muyu_gif.h'),
                '180', '180', '5', '1')
    else:
        convert(p, os.path.join(out, 'heart_gif.c'), os.path.join(out, 'heart_gif.h'),
                '44', '44', '16', '6')
print('done')
