# Draws OpenJamboree's own icon (no Nintendo art): a party die tumbling out of a circuit-board tile, with confetti.
#   python tools/art/make_icon.py        -> docs/art/icon-1024.png, icon-512.png, icon-256.png
# Needs Pillow. Drawn at 4x and scaled down for clean edges.
import math, os, random
from PIL import Image, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', '..', 'docs', 'art')
S = 4096                      # working size; the icon is 1024
R = S // 1024                 # pixels per final pixel


def lerp(a, b, t): return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(len(a)))


def rounded_mask(size, radius):
    m = Image.new('L', (size, size), 0)
    ImageDraw.Draw(m).rounded_rectangle((0, 0, size - 1, size - 1), radius, fill=255)
    return m


def background():
    # deep night-blue to violet diagonal, the launcher's dark theme with a party glow
    bg = Image.new('RGB', (S, S))
    px = bg.load()
    c0, c1 = (24, 18, 64), (92, 30, 120)
    for y in range(0, S, 4):
        for x in range(0, S, 4):
            t = (x + y) / (2 * S)
            c = lerp(c0, c1, t)
            for dy in range(4):
                for dx in range(4):
                    px[x + dx, y + dy] = c
    d = ImageDraw.Draw(bg, 'RGBA')
    # circuit traces: the "recompiled" part, faint, in the lower left
    rnd = random.Random(7)
    for i in range(16):
        x = rnd.randint(-200, S // 2) ; y = rnd.randint(S // 2, S + 200)
        pts = [(x, y)]
        for _ in range(3):
            if rnd.random() < 0.5: x += rnd.choice((-1, 1)) * rnd.randint(150, 500)
            else: y -= rnd.randint(150, 500)
            pts.append((x, y))
        d.line(pts, fill=(140, 220, 255, 46), width=18)
        d.ellipse((pts[-1][0] - 40, pts[-1][1] - 40, pts[-1][0] + 40, pts[-1][1] + 40), fill=(140, 220, 255, 70))
    # soft spotlight behind the die
    glow = Image.new('L', (S, S), 0)
    ImageDraw.Draw(glow).ellipse((S * 0.18, S * 0.12, S * 0.92, S * 0.86), fill=150)
    glow = glow.filter(ImageFilter.GaussianBlur(S // 10))
    bg = Image.composite(Image.new('RGB', (S, S), (255, 170, 90)), bg, glow.point(lambda v: v * 0.55))
    return bg


def iso(cx, cy, size, a):
    """the three visible faces of a cube rotated by angle a, seen from above: returns top, left, right polygons"""
    h = size * 0.5
    def p(x, y, z):
        xr = x * math.cos(a) - y * math.sin(a); yr = x * math.sin(a) + y * math.cos(a)
        return (cx + (xr - yr) * 0.866, cy + (xr + yr) * 0.5 - z)
    A, B, C, D = p(-h, -h, h), p(h, -h, h), p(h, h, h), p(-h, h, h)
    A2, B2, C2, D2 = p(-h, -h, -h), p(h, -h, -h), p(h, h, -h), p(-h, h, -h)
    return [A, B, C, D], [D, C, C2, D2], [C, B, B2, C2]


def face_point(poly, u, v):
    """point at (u, v) in [0,1]^2 on a quad face"""
    (x0, y0), (x1, y1), (x2, y2), (x3, y3) = poly
    ax = x0 + (x1 - x0) * u; ay = y0 + (y1 - y0) * u
    bx = x3 + (x2 - x3) * u; by = y3 + (y2 - y3) * u
    return ax + (bx - ax) * v, ay + (by - ay) * v


PIPS = {1: [(.5, .5)], 2: [(.27, .27), (.73, .73)], 3: [(.25, .25), (.5, .5), (.75, .75)],
        4: [(.27, .27), (.73, .27), (.27, .73), (.73, .73)],
        5: [(.25, .25), (.75, .25), (.5, .5), (.25, .75), (.75, .75)],
        6: [(.27, .22), (.73, .22), (.27, .5), (.73, .5), (.27, .78), (.73, .78)]}


def die(img, cx, cy, size, angle):
    d = ImageDraw.Draw(img, 'RGBA')
    top, left, right = iso(cx, cy, size, angle)
    # shadow
    sh = Image.new('L', img.size, 0)
    ImageDraw.Draw(sh).ellipse((cx - size * 0.95, cy + size * 0.55, cx + size * 0.95, cy + size * 1.05), fill=170)
    sh = sh.filter(ImageFilter.GaussianBlur(size // 8))
    img.paste((20, 10, 40), mask=sh)
    d.polygon(left, fill=(226, 64, 84))        # red-coral sides, warm and festive
    d.polygon(right, fill=(184, 36, 70))
    d.polygon(top, fill=(255, 250, 242))       # ivory top
    for poly in (top, left, right):
        d.line(poly + [poly[0]], fill=(40, 16, 56, 255), width=int(size // 34), joint='curve')
    # pips: circles drawn in each face's own plane, so they follow its perspective
    for poly, n, col in ((top, 5, (40, 16, 56)), (left, 3, (255, 244, 230)), (right, 2, (255, 244, 230))):
        for u, v in PIPS[n]:
            ring = [face_point(poly, u + 0.09 * math.cos(t), v + 0.09 * math.sin(t)) for t in [k * math.pi / 16 for k in range(32)]]
            d.polygon(ring, fill=col)


def confetti(img):
    d = ImageDraw.Draw(img, 'RGBA')
    rnd = random.Random(26)
    cols = [(255, 214, 64), (80, 220, 200), (255, 120, 170), (120, 160, 255), (255, 255, 255)]
    for _ in range(46):
        x = rnd.uniform(S * 0.1, S * 0.95); y = rnd.uniform(S * 0.05, S * 0.62)
        if (x - S * 0.55) ** 2 + (y - S * 0.5) ** 2 < (S * 0.24) ** 2: continue
        w = rnd.uniform(40, 110); hgt = w * rnd.uniform(0.3, 0.55); a = rnd.uniform(0, math.pi)
        pts = [(x + math.cos(a) * dx - math.sin(a) * dy, y + math.sin(a) * dx + math.cos(a) * dy)
               for dx, dy in ((-w, -hgt), (w, -hgt), (w, hgt), (-w, hgt))]
        d.polygon(pts, fill=rnd.choice(cols) + (235,))
    # a few four-point sparkles
    for x, y, r in ((S * 0.2, S * 0.24, 150), (S * 0.84, S * 0.2, 110), (S * 0.8, S * 0.78, 90)):
        d.polygon([(x, y - r), (x + r * 0.22, y - r * 0.22), (x + r, y), (x + r * 0.22, y + r * 0.22),
                   (x, y + r), (x - r * 0.22, y + r * 0.22), (x - r, y), (x - r * 0.22, y - r * 0.22)],
                  fill=(255, 240, 200, 240))


def main():
    img = background()
    confetti(img)
    die(img, S * 0.52, S * 0.47, S * 0.335, math.radians(14))
    img = img.convert('RGBA')
    img.putalpha(rounded_mask(S, S // 5))
    os.makedirs(OUT, exist_ok=True)
    for n in (1024, 512, 256):
        img.resize((n, n), Image.LANCZOS).save(os.path.join(OUT, 'icon-%d.png' % n))
    print('icons in', os.path.normpath(OUT))


if __name__ == '__main__':
    main()
