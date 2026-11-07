"""Draws the iOS app icon (1024 x 1024 PNG): the Android launcher waveform
(saw into sine) as an orange LED line on the dark panel of the app.
usage: python ios/tools/make_icon.py  (needs Pillow; writes ios/HSynth/Assets.xcassets/AppIcon.appiconset/icon-1024.png)"""
import os

from PIL import Image, ImageDraw, ImageFilter

S = 1024
SS = 4  # supersampling
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', 'HSynth', 'Assets.xcassets', 'AppIcon.appiconset', 'icon-1024.png')

PANEL_TOP, PANEL_BOTTOM = (44, 47, 53), (22, 23, 26)
LED = (255, 140, 26)


def cubic(p0, p1, p2, p3, n=40):
    pts = []
    for i in range(1, n + 1):
        t = i / n
        u = 1 - t
        pts.append(tuple(u ** 3 * a + 3 * u * u * t * b + 3 * u * t * t * c + t ** 3 * d for a, b, c, d in zip(p0, p1, p2, p3)))
    return pts


def waveform():
    # the Android vector path in its 108 x 108 viewport:
    # M30,66 L42,42 L42,66 L54,42 L54,66 C60,40 66,40 72,54 C76,62 78,66 80,66
    pts = [(30, 66), (42, 42), (42, 66), (54, 42), (54, 66)]
    pts += cubic((54, 66), (60, 40), (66, 40), (72, 54))
    pts += cubic((72, 54), (76, 62), (78, 66), (80, 66))
    # centre it and scale it to the icon
    xs, ys = [p[0] for p in pts], [p[1] for p in pts]
    cx, cy = (min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2
    k = S * 0.62 / (max(xs) - min(xs))
    return [((x - cx) * k + S / 2, (y - cy) * k + S / 2) for x, y in pts]


def main():
    big = S * SS
    img = Image.new('RGB', (big, big))
    d = ImageDraw.Draw(img)
    for y in range(big):  # vertical gradient, like the hardware panel
        t = y / (big - 1)
        d.line([(0, y), (big, y)], fill=tuple(round(a + (b - a) * t) for a, b in zip(PANEL_TOP, PANEL_BOTTOM)))
    pts = [(x * SS, y * SS) for x, y in waveform()]
    width = int(S * 0.055 * SS)
    # glow under the line
    glow = Image.new('RGB', (big, big), (0, 0, 0))
    ImageDraw.Draw(glow).line(pts, fill=LED, width=width * 3)
    glow = glow.filter(ImageFilter.GaussianBlur(radius=S * 0.045 * SS))
    img = Image.blend(img, Image.composite(glow, img, glow.convert('L')), 0.55)
    d = ImageDraw.Draw(img)
    d.line(pts, fill=LED, width=width)
    r = width / 2
    for x, y in pts:  # round joins and caps
        d.ellipse([x - r, y - r, x + r, y + r], fill=LED)
    img = img.resize((S, S), Image.LANCZOS)
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    img.save(OUT, optimize=True)
    print('wrote', os.path.normpath(OUT))


if __name__ == '__main__':
    main()
