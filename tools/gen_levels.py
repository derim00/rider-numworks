#!/usr/bin/env python3
"""Generate src/levels.h : track geometry for every level.

Level 1 is the exact track of the Unity scene (Main.unity): four
BezierCurveCollider2D line segments (smoothness 20) + their transforms.
The other levels are built with the small Track builder below.

Tracks are polylines. A segment is solid on its right side (relative to the
drawing direction), i.e. tracks drawn left->right are solid below. Loops are
drawn counter-clockwise so the car rides on their inner face.
"""
import math, os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "src", "levels.h")

YELLOW, RED, CYAN, PINK = 0, 1, 2, 3

# --------------------------------------------------------------- level 1 (original)
def bezier_pts(cps, hps, smooth=20, off=(0, 0)):
    pts = []
    def bz(t, p0, h0, h1, p1):
        u = 1 - t
        return (u ** 3 * p0[0] + 3 * u * u * t * h0[0] + 3 * u * t * t * h1[0] + t ** 3 * p1[0],
                u ** 3 * p0[1] + 3 * u * u * t * h0[1] + 3 * u * t * t * h1[1] + t ** 3 * p1[1])
    h = 0
    for i in range(len(cps) - 1):
        seg = [cps[i]] + [bz(k / smooth, cps[i], hps[h], hps[h + 1], cps[i + 1]) for k in range(1, smooth)] + [cps[i + 1]]
        h += 2
        for p in seg:
            q = (p[0] + off[0], p[1] + off[1])
            if not pts or (abs(pts[-1][0] - q[0]) > 1e-6 or abs(pts[-1][1] - q[1]) > 1e-6):
                pts.append(q)
    return pts

def level_original():
    polys = []
    # LineSegment (purple material = red), pos (0,-2.8)
    polys.append((RED, bezier_pts(
        [(-15.188935, -3.9574144), (-1.3519328, -2.487518), (8.215645, -2.133557), (16.365639, 1.0285825)],
        [(-11.533037, -1.9549663), (-6.960986, -0.89724135), (1.3240329, -3.2462065), (4.746307, -2.7897384),
         (12.485937, -1.3258841), (11.987413, -1.3842103)], off=(0, -2.8))))
    # LineSegment 2 (green material = yellow), pos (31.31,5.48)
    polys.append((YELLOW, bezier_pts(
        [(-12.532713, -6.999998), (-7.968355, -3.0187607)],
        [(-10.794186, -6.1123266), (-8.569168, -3.6714454)], off=(31.31, 5.48))))
    # LineSegment 3 (red), pos (42.07,4.1)
    polys.append((RED, bezier_pts(
        [(-16.628668, -2.1365683), (-1.7330399, -5.6210628), (18.293808, -0.99023485)],
        [(-14.158436, -5.766035), (-7.3420944, -4.030783), (0.9429245, -6.3797526), (15.332615, -3.3825715)],
        off=(42.07, 4.1))))
    # LineSegment 4 (yellow), pos (64.83,4.3)
    polys.append((YELLOW, bezier_pts(
        [(-2.6437683, -1.050695), (2.7299576, 1.7277284)],
        [(0.23131561, 0.36540127), (2.1440582, 0.917449)], off=(64.83, 4.3))))
    polys = [(c, [(p, 0) for p in pts]) for c, pts in polys]
    return dict(name="DESERT", polys=polys, loops=[],
                start=(-7.591, -1.403), finish=73.36 - 0.34, flag=(70.18, 7.1), deathy=-9.5)

# --------------------------------------------------------------- builder
class Track:
    def __init__(self, x, y, color=YELLOW):
        self.polys = []
        self.color = color
        self.cur = [(x, y)]
        self.tags = [0]
        self.loops = []  # (start segment tag id, end x)

    @property
    def x(self): return self.cur[-1][0]
    @property
    def y(self): return self.cur[-1][1]

    def _add(self, p, tag=0):
        self.cur.append(p)
        self.tags.append(tag)

    def line(self, dx, dy):
        self._add((self.x + dx, self.y + dy))
        return self

    def flat(self, dx):
        return self.line(dx, 0)

    def curve(self, dx, dy, s0=0.0, s1=0.0, n=None):
        """Cubic Hermite from current point to (+dx,+dy), slopes s0/s1 (dy/dx)."""
        x0, y0 = self.x, self.y
        n = n or max(4, int(abs(dx) / 0.35))
        for k in range(1, n + 1):
            t = k / n
            h00 = 2 * t ** 3 - 3 * t ** 2 + 1; h10 = t ** 3 - 2 * t ** 2 + t
            h01 = -2 * t ** 3 + 3 * t ** 2; h11 = t ** 3 - t ** 2
            y = h00 * y0 + h10 * dx * s0 + h01 * (y0 + dy) + h11 * dx * s1
            self._add((x0 + dx * t, y))
        return self

    def hills(self, count, wl, amp, s=0.0):
        for _ in range(count):
            self.curve(wl / 2, amp, s, 0)
            self.curve(wl / 2, -amp, 0, s)
        return self

    def loop(self, r, shift=1.4, n=56):
        """Counter clockwise loop starting at the current point (bottom)."""
        x0, y0 = self.x, self.y
        lid = len(self.loops) + 1
        self.loops.append(x0 + shift + r + 1.0)
        for k in range(1, n + 1):
            t = k / n
            a = -math.pi / 2 + 2 * math.pi * t
            cx = x0 + shift * t
            # phase A (entry half, up to the top) = +lid, phase B = -lid
            self._add((cx + r * math.cos(a), y0 + r + r * math.sin(a)), lid if t <= 0.5 else -lid)
        return self

    def gap(self, dx, dy, color=None):
        """End current polyline, start a new one further away."""
        self.end()
        if color is not None:
            self.color = color
        else:
            self.color = RED if self.color == YELLOW else YELLOW
        self.cur = [(self._lx + dx, self._ly + dy)]
        self.tags = [0]
        return self

    def end(self):
        if len(self.cur) > 1:
            self.polys.append((self.color, list(zip(self.cur, self.tags))))
        self._lx, self._ly = self.cur[-1]
        self.cur = [self.cur[-1]]
        self.tags = [0]
        return self

    def done(self):
        self.end()
        return self.polys

def level(name, t, start, finish_extra=3.0, flag_dy=2.2):
    polys = t.done()
    lastx, lasty = polys[-1][1][-1][0]
    miny = min(p[0][1] for c, pts in polys for p in pts)
    return dict(name=name, polys=polys, start=start, finish=lastx + finish_extra, loops=t.loops,
                flag=(lastx + finish_extra - 3.2, lasty + flag_dy), deathy=miny - 7)

# --------------------------------------------------------------- new levels
def level_hills():
    t = Track(-6, 0, YELLOW)
    t.flat(8).curve(6, -2, 0, -0.5).hills(3, 9, 1.4)
    t.curve(5, 1.2, 0, 0.45)          # kicker
    t.gap(4.0, -1.2).curve(6, -2.5, -0.5, 0).flat(3)
    t.hills(2, 12, 2.2)
    t.curve(5, 1.4, 0, 0.5)
    t.gap(4.2, -1.5).curve(7, -3, -0.6, 0).hills(2, 7, 1.0).flat(4)
    return level("COLLINES", t, (-3, 1.0))

def level_loop():
    t = Track(-6, 4, RED)
    t.flat(6).curve(10, -5, 0, 0).flat(4)
    t.loop(2.6)
    t.flat(5).curve(5, 1.2, 0, 0.45)
    t.gap(4.0, -1.5).curve(9, -4.5, -0.5, 0).flat(3)
    t.loop(2.8)
    t.flat(6).curve(5, 1.4, 0, 0.55)
    t.gap(4.5, -0.5).flat(3)
    return level("LOOPING", t, (-3, 5.0))

def level_jumps():
    t = Track(-6, 0, YELLOW)
    t.flat(10).curve(5, 1.2, 0, 0.45)
    for i, (g, dy) in enumerate([(3.5, -0.8), (4.0, -0.6), (4.5, -0.4), (5.0, -1.0)]):
        t.gap(g, dy).curve(4, -1.4, -0.45, 0).flat(3).curve(4, 1.2, 0, 0.45)
    t.gap(5, -1.5).curve(6, -2.5, -0.5, 0).flat(5)
    return level("SAUTS", t, (-3, 1.0))

def level_drop():
    t = Track(-6, 12, RED)
    t.flat(6).curve(14, -12, 0, -0.9).curve(8, 0, -0.9, 0)
    t.hills(2, 7, 0.8)
    t.curve(7, 2.5, 0, 0.6)
    t.gap(8, -2.5, YELLOW).curve(12, -7, -0.6, -0.3).curve(6, -1, -0.3, 0).flat(4)
    t.loop(2.8)
    t.flat(6).curve(6, 2.0, 0, 0.7)
    t.gap(5, -0.5, RED).flat(3)
    return level("CHUTE", t, (-3, 13.0))

def level_waves():
    t = Track(-6, 0, YELLOW)
    t.flat(8)
    amps = [1.0, 1.5, 2.0, 2.4]
    for a in amps:
        t.curve(6, a, 0, 0.0).curve(6, -a, 0, -0.2)
    t.curve(5, 1.4, 0, 0.5)
    t.gap(4.5, -1.2).curve(5, -2.0, -0.6, 0)
    for a in amps[::-1]:
        t.curve(6, a, 0, 0).curve(6, -a, 0, 0)
    t.flat(4)
    return level("VAGUES", t, (-3, 1.0))

def level_double():
    t = Track(-6, 9, RED)
    t.flat(6).curve(14, -9, 0, 0).flat(3)
    t.loop(2.4).flat(5.5).loop(2.4)
    t.flat(5).curve(6, 1.6, 0, 0.55)
    t.gap(4.0, -1.5, CYAN).curve(9, -4, -0.5, 0).flat(2)
    t.curve(4, 1.2, 0, 0.5)
    t.gap(2.6, -1.6, PINK).curve(5, -1.5, -0.4, 0).flat(3)
    return level("DOUBLE", t, (-3, 10.0))

def level_sky():
    t = Track(-6, 0, CYAN)
    t.flat(10).curve(5, 1.0, 0, 0.4)
    t.gap(3.3, -0.6, PINK).curve(4, -1.2, -0.4, 0).flat(1).curve(4, 1.2, 0, 0.45)
    t.gap(3.5, -0.6, CYAN).curve(4, -1.4, -0.45, 0).flat(1).curve(4, 1.3, 0, 0.5)
    t.gap(4.0, -0.8, PINK).curve(9, -5, -0.6, -0.2).curve(4, 0, -0.2, 0)
    t.loop(2.6)
    t.flat(4).curve(5, 1.6, 0, 0.6)
    t.gap(3.8, -0.9, CYAN).curve(4, -1.5, -0.5, 0).flat(1).curve(4, 1.6, 0, 0.6)
    t.gap(2.6, -1.5, PINK).flat(4)
    return level("CIEL", t, (-3, 1.0))

def level_final():
    t = Track(-6, 10, YELLOW)
    t.flat(6).curve(12, -9, 0, -0.6).curve(6, 0, -0.6, 0)
    t.loop(2.8)
    t.flat(3).hills(2, 9, 1.4)
    t.curve(5, 1.6, 0, 0.55)
    t.gap(4.5, -1.5, RED).curve(10, -6, -0.6, 0).flat(3)
    t.loop(2.4).flat(5.5).loop(2.4)
    t.flat(4).curve(5, 1.4, 0, 0.5)
    t.gap(3.0, -1.0, CYAN).curve(4, -1.3, -0.45, 0).flat(1).curve(4, 1.3, 0, 0.5)
    t.gap(3.0, -1.5, PINK).curve(9, -4, -0.6, 0).flat(3).curve(6, 2.4, 0, 0.75)
    t.gap(4.5, -0.5, YELLOW).flat(3)
    return level("FINAL", t, (-3, 11.0))

LEVELS = [level_original(), level_hills(), level_loop(), level_jumps(),
          level_waves(), level_drop(), level_double(), level_sky(), level_final()]

# --------------------------------------------------------------- emit
MAXSEG = 0.8
BUCKET = 2.0
MARGIN = 1.6  # car half-size + motion margin

def subdivide(pts):
    res = [pts[0]]
    for (a, _), (b, tag) in zip(pts, pts[1:]):
        L = math.hypot(b[0] - a[0], b[1] - a[1])
        k = max(1, int(math.ceil(L / MAXSEG)))
        for j in range(1, k + 1):
            res.append(((a[0] + (b[0] - a[0]) * j / k, a[1] + (b[1] - a[1]) * j / k), tag))
    return res

out = ["// Generated by tools/gen_levels.py - do not edit", "#pragma once", ""]
table = []
for li, L in enumerate(LEVELS):
    segs = []
    for color, pts in L["polys"]:
        pts = subdivide(pts)
        for (a, _), (b, tag) in zip(pts, pts[1:]):
            dx, dy = b[0] - a[0], b[1] - a[1]
            ln = math.hypot(dx, dy)
            if ln < 1e-5:
                continue
            segs.append((a[0], a[1], dx / ln, dy / ln, ln, color, tag))
    xs = [s[0] for s in segs] + [s[0] + s[2] * s[4] for s in segs]
    ys = [s[1] for s in segs] + [s[1] + s[3] * s[4] for s in segs]
    x0 = math.floor(min(xs) - 4)
    nb = int(math.ceil((max(xs) + 4 - x0) / BUCKET))
    buckets = [[] for _ in range(nb)]
    for i, s in enumerate(segs):
        sx0 = min(s[0], s[0] + s[2] * s[4]) - MARGIN
        sx1 = max(s[0], s[0] + s[2] * s[4]) + MARGIN
        for b in range(max(0, int((sx0 - x0) // BUCKET)), min(nb, int((sx1 - x0) // BUCKET) + 1)):
            buckets[b].append(i)
    starts, items = [0], []
    for b in buckets:
        items += b
        starts.append(len(items))
    p = f"lv{li}"
    out.append(f"static const seg_t {p}_segs[{len(segs)}] = {{")
    for s in segs:
        out.append("  {%.4ff,%.4ff,%.5ff,%.5ff,%.4ff,%d,%d}," % s)
    out.append("};")
    out.append(f"static const uint16_t {p}_bstart[{len(starts)}] = {{" + ",".join(map(str, starts)) + "};")
    out.append(f"static const uint16_t {p}_bitems[{len(items)}] = {{" + ",".join(map(str, items)) + "};")
    out.append(f"static const float {p}_loopx[{len(L['loops']) + 1}] = {{0" + "".join(",%.3ff" % x for x in L['loops']) + "};")
    table.append("  {\"%s\", %d, %s_segs, %.2ff, %d, %s_bstart, %s_bitems, %s_loopx, %.3ff, %.3ff, %.3ff, %.3ff, %.3ff, %.3ff, %.3ff, %.3ff}," % (
        L["name"], len(segs), p, x0, nb, p, p, p, L["start"][0], L["start"][1], L["finish"],
        L["flag"][0], L["flag"][1], L["deathy"], min(ys), max(ys)))
    print(f"{L['name']:10s} segs={len(segs):4d} x=[{min(xs):.1f},{max(xs):.1f}] y=[{min(ys):.1f},{max(ys):.1f}] finish={L['finish']:.1f}")
out.append(f"#define NUM_LEVELS {len(LEVELS)}")
out.append(f"#define BUCKET_W {BUCKET}f")
out.append("static const level_t levels[NUM_LEVELS] = {")
out += table
out.append("};")
open(OUT, "w").write("\n".join(out) + "\n")
