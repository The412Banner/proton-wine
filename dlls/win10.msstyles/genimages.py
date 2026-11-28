#!/usr/bin/env python3
#
# Generates the bitmaps and win10.rc of the Wine Windows 10 visual style.
# Plain Python, no image libraries: run it in this directory to rewrite them.
#
# Copyright 2026 The412Banner
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2.1 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA

import math
import struct

SS = 4  # samples per pixel along each axis


def hexc(s):
    return (int(s[0:2], 16), int(s[2:4], 16), int(s[4:6], 16))


def mix(c1, c2, t):
    return tuple(c1[i] + (c2[i] - c1[i]) * t for i in range(3))


def stops_color(stops, t):
    t = min(max(t, 0.0), 1.0)
    for i in range(1, len(stops)):
        if t <= stops[i][0]:
            (t0, c0), (t1, c1) = stops[i - 1], stops[i]
            return mix(c0, c1, 0.0 if t1 == t0 else (t - t0) / (t1 - t0))
    return stops[-1][1]


# colour functions: (x, y) of the pixel centre -> rgb

def solid(c):
    return lambda x, y: c


def vgrad(y0, y1, stops):
    return lambda x, y: stops_color(stops, (y - y0) / (y1 - y0))


def hgrad(x0, x1, stops):
    return lambda x, y: stops_color(stops, (x - x0) / (x1 - x0))


def dgrad(x0, y0, x1, y1, c0, c1):
    return lambda x, y: mix(c0, c1, min(max(((x - x0) + (y - y0)) / ((x1 - x0) + (y1 - y0)), 0.0), 1.0))


def radial(cx, cy, rad, stops):
    return lambda x, y: stops_color(stops, math.hypot(x - cx, y - cy) / rad)


# shapes: (x, y) of a sample point -> inside

def rect(x0, y0, x1, y1):
    return lambda x, y: x0 <= x < x1 and y0 <= y < y1


def rrect(x0, y0, x1, y1, r):
    def inside(x, y):
        if x < x0 or x >= x1 or y < y0 or y >= y1:
            return False
        if r <= 0:
            return True
        cx = min(max(x, x0 + r), x1 - r)
        cy = min(max(y, y0 + r), y1 - r)
        return (x - cx) ** 2 + (y - cy) ** 2 <= r * r
    return inside


def ring(x0, y0, x1, y1, r, w):
    outer = rrect(x0, y0, x1, y1, r)
    inner = rrect(x0 + w, y0 + w, x1 - w, y1 - w, max(r - w, 0))
    return lambda x, y: outer(x, y) and not inner(x, y)


def disc(cx, cy, rad):
    return lambda x, y: (x - cx) ** 2 + (y - cy) ** 2 <= rad * rad


def annulus(cx, cy, outer, inner):
    return lambda x, y: inner * inner < (x - cx) ** 2 + (y - cy) ** 2 <= outer * outer


def polyline(points, width):
    half = width / 2.0

    def dist(px, py, ax, ay, bx, by):
        dx, dy = bx - ax, by - ay
        t = max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)))
        return math.hypot(px - ax - t * dx, py - ay - t * dy)

    segs = list(zip(points, points[1:]))
    return lambda x, y: any(dist(x, y, a[0], a[1], b[0], b[1]) <= half for a, b in segs)


def polygon(points):
    def inside(x, y):
        c = False
        n = len(points)
        for i in range(n):
            (x1, y1), (x2, y2) = points[i], points[(i + 1) % n]
            if (y1 > y) != (y2 > y) and x < x1 + (y - y1) * (x2 - x1) / (y2 - y1):
                c = not c
        return c
    return inside


def both(a, b):
    return lambda x, y: a(x, y) and b(x, y)


class Canvas:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.p = [(0.0, 0.0, 0.0, 0.0)] * (w * h)  # premultiplied

    def put(self, x, y, c, a):
        i = y * self.w + x
        r, g, b, pa = self.p[i]
        k = 1.0 - a
        self.p[i] = (c[0] * a + r * k, c[1] * a + g * k, c[2] * a + b * k, a + pa * k)

    def draw(self, inside, color, opacity=1.0):
        for y in range(self.h):
            for x in range(self.w):
                n = sum(1 for sy in range(SS) for sx in range(SS)
                        if inside(x + (sx + 0.5) / SS, y + (sy + 0.5) / SS))
                if n:
                    self.put(x, y, color(x + 0.5, y + 0.5), opacity * n / (SS * SS))

    def transformed(self, fn, w=None, h=None):
        """new canvas where pixel (x, y) = self[fn(x, y)]"""
        out = Canvas(w or self.w, h or self.h)
        for y in range(out.h):
            for x in range(out.w):
                sx, sy = fn(x, y)
                out.p[y * out.w + x] = self.p[sy * self.w + sx]
        return out

    def flip_v(self):
        return self.transformed(lambda x, y: (x, self.h - 1 - y))

    def flip_h(self):
        return self.transformed(lambda x, y: (self.w - 1 - x, y))

    def transpose(self):
        return self.transformed(lambda x, y: (y, x), self.h, self.w)


def write_bmp(path, frames, horizontal=False):
    w, h = frames[0].w, frames[0].h
    W, H = (w * len(frames), h) if horizontal else (w, h * len(frames))
    pixels = [(0.0, 0.0, 0.0, 0.0)] * (W * H)
    for i, f in enumerate(frames):
        ox, oy = (i * w, 0) if horizontal else (0, i * h)
        for y in range(h):
            pixels[(oy + y) * W + ox:(oy + y) * W + ox + w] = f.p[y * w:(y + 1) * w]
    data = bytearray()
    for y in range(H - 1, -1, -1):
        for x in range(W):
            r, g, b, a = pixels[y * W + x]
            alpha = int(round(a * 255))
            if alpha == 0:
                data += b'\0\0\0\0'
                continue
            data += bytes(max(0, min(255, int(round(v / a)))) for v in (b, g, r)) + bytes((alpha,))
    header = struct.pack('<2sIHHI', b'BM', 14 + 40 + len(data), 0, 0, 54)
    info = struct.pack('<IiiHHIIiiII', 40, W, H, 1, 32, 0, len(data), 2835, 2835, 0, 0)
    with open(path, 'wb') as f:
        f.write(header + info + data)


# colours: Windows 10 light and dark, flat

LIGHT = dict(
    window=dict(caption=hexc('ffffff'), caption_inactive=hexc('ffffff'), text=(0, 0, 0), text_inactive=(109, 109, 109),
                frame=hexc('aaaaaa'), glyph=hexc('000000'), glyph_inactive=hexc('6d6d6d'), glyph_disabled=hexc('c8c8c8'),
                hot=hexc('e5e5e5'), pressed=hexc('cccccc')),
    btn=dict(normal=(hexc('adadad'), hexc('e1e1e1')), hot=(hexc('0078d7'), hexc('e5f1fb')),
             pressed=(hexc('005499'), hexc('cce4f7')), disabled=(hexc('bfbfbf'), hexc('cccccc')),
             default=(hexc('0078d7'), hexc('e1e1e1'))),
    box=dict(normal=(hexc('333333'), hexc('ffffff')), hot=(hexc('0078d7'), hexc('ffffff')),
             pressed=(hexc('005499'), hexc('cce4f7')), disabled=(hexc('cccccc'), hexc('ffffff'))),
    check=hexc('333333'), check_hot=hexc('0078d7'), check_disabled=hexc('cccccc'),
    group_border=hexc('dcdcdc'),
    sb=dict(track=hexc('f0f0f0'), thumb=dict(normal=hexc('cdcdcd'), hot=hexc('a6a6a6'), pressed=hexc('606060'),
                                             disabled=hexc('f0f0f0')),
            arrow=dict(normal=hexc('f0f0f0'), hot=hexc('dadada'), pressed=hexc('606060'), disabled=hexc('f0f0f0')),
            glyph=dict(normal=hexc('606060'), hot=hexc('000000'), pressed=hexc('ffffff'), disabled=hexc('bfbfbf'))),
    drop=dict(normal=None, hot=(hexc('e5f1fb'), hexc('0078d7')), pressed=(hexc('cce4f7'), hexc('005499')),
              disabled=None),
    drop_glyph=dict(normal=hexc('606060'), hot=hexc('000000'), pressed=hexc('000000'), disabled=hexc('bfbfbf')),
    spin=dict(normal=(hexc('adadad'), hexc('f0f0f0')), hot=(hexc('0078d7'), hexc('e5f1fb')),
              pressed=(hexc('005499'), hexc('cce4f7')), disabled=(hexc('cccccc'), hexc('f0f0f0'))),
    tab=dict(border=hexc('d9d9d9'), normal=hexc('f0f0f0'), hot=hexc('d8eaf9'), hot_border=hexc('7eb4ea'),
             selected=hexc('ffffff'), disabled=hexc('f0f0f0')),
    progress_border=hexc('bcbcbc'), progress_trough=hexc('e6e6e6'),
    trackbar_rows=[hexc('d6d6d6'), hexc('e7eaea'), hexc('e7eaea'), hexc('d6d6d6')],
    thumb=dict(normal=hexc('007ad9'), hot=hexc('171717'), pressed=hexc('cccccc'), disabled=hexc('cccccc')),
    header=dict(normal=hexc('ffffff'), hot=hexc('d9ebf9'), pressed=hexc('bcdcf4'), line=hexc('e5e5e5')),
    tree=dict(closed=hexc('a6a6a6'), opened=hexc('262626'), hot=hexc('1cc4f7')),
    tb=dict(hot=(hexc('e5f3ff'), hexc('cce8ff')), pressed=(hexc('cce8ff'), hexc('99d1ff')),
            checked=(hexc('cce8ff'), hexc('99d1ff')), hotchecked=(hexc('e5f3ff'), hexc('99d1ff')),
            nearhot=(None, hexc('cce8ff'))),
    grip_dot=hexc('a0a0a0'), grip_shine=None,
    edge=dict(light=(227, 227, 227), highlight=(255, 255, 255), shadow=(160, 160, 160), dkshadow=(105, 105, 105),
              fill=(240, 240, 240)),
    text=(0, 0, 0), text_disabled=(131, 131, 131), group_text=(0, 0, 0),
    edit_border=(122, 122, 122), field=(245, 245, 245), field_disabled=(235, 235, 235),
)

DARK = dict(
    window=dict(caption=hexc('202020'), caption_inactive=hexc('2b2b2b'), text=(255, 255, 255), text_inactive=(180, 180, 180),
                frame=hexc('333333'), glyph=hexc('ffffff'), glyph_inactive=hexc('b4b4b4'), glyph_disabled=hexc('5a5a5a'),
                hot=hexc('3a3a3a'), pressed=hexc('555555')),
    btn=dict(normal=(hexc('5a5a5a'), hexc('333333')), hot=(hexc('8a8a8a'), hexc('454545')),
             pressed=(hexc('9b9b9b'), hexc('5a5a5a')), disabled=(hexc('3a3a3a'), hexc('2a2a2a')),
             default=(hexc('0078d7'), hexc('333333'))),
    box=dict(normal=(hexc('c8c8c8'), hexc('303030')), hot=(hexc('ffffff'), hexc('303030')),
             pressed=(hexc('ffffff'), hexc('5a5a5a')), disabled=(hexc('4a4a4a'), hexc('2a2a2a'))),
    check=hexc('ffffff'), check_hot=hexc('ffffff'), check_disabled=hexc('5a5a5a'),
    group_border=hexc('4a4a4a'),
    sb=dict(track=hexc('2b2b2b'), thumb=dict(normal=hexc('5a5a5a'), hot=hexc('7a7a7a'), pressed=hexc('a6a6a6'),
                                             disabled=hexc('2b2b2b')),
            arrow=dict(normal=hexc('2b2b2b'), hot=hexc('444444'), pressed=hexc('a6a6a6'), disabled=hexc('2b2b2b')),
            glyph=dict(normal=hexc('a6a6a6'), hot=hexc('ffffff'), pressed=hexc('1f1f1f'), disabled=hexc('4a4a4a'))),
    drop=dict(normal=None, hot=(hexc('454545'), hexc('8a8a8a')), pressed=(hexc('5a5a5a'), hexc('9b9b9b')),
              disabled=None),
    drop_glyph=dict(normal=hexc('c8c8c8'), hot=hexc('ffffff'), pressed=hexc('ffffff'), disabled=hexc('5a5a5a')),
    spin=dict(normal=(hexc('5a5a5a'), hexc('333333')), hot=(hexc('8a8a8a'), hexc('454545')),
              pressed=(hexc('9b9b9b'), hexc('5a5a5a')), disabled=(hexc('3a3a3a'), hexc('2a2a2a'))),
    tab=dict(border=hexc('4a4a4a'), normal=hexc('2b2b2b'), hot=hexc('3a3a3a'), hot_border=hexc('6a6a6a'),
             selected=hexc('383838'), disabled=hexc('2a2a2a')),
    progress_border=hexc('4a4a4a'), progress_trough=hexc('2b2b2b'),
    trackbar_rows=[hexc('4a4a4a'), hexc('2b2b2b'), hexc('2b2b2b'), hexc('4a4a4a')],
    thumb=dict(normal=hexc('0078d7'), hot=hexc('ffffff'), pressed=hexc('9b9b9b'), disabled=hexc('4a4a4a')),
    header=dict(normal=hexc('303030'), hot=hexc('3a3a3a'), pressed=hexc('4a4a4a'), line=hexc('4a4a4a')),
    tree=dict(closed=hexc('a6a6a6'), opened=hexc('e0e0e0'), hot=hexc('4cc2ff')),
    tb=dict(hot=(((255, 255, 255), 0.10), ((255, 255, 255), 0.18)), pressed=(((255, 255, 255), 0.20), ((255, 255, 255), 0.25)),
            checked=(((255, 255, 255), 0.15), ((255, 255, 255), 0.25)),
            hotchecked=(((255, 255, 255), 0.20), ((255, 255, 255), 0.30)), nearhot=(None, ((255, 255, 255), 0.12))),
    grip_dot=hexc('6a6a6a'), grip_shine=None,
    edge=dict(light=(58, 58, 58), highlight=(74, 74, 74), shadow=(21, 21, 21), dkshadow=(0, 0, 0), fill=(33, 33, 33)),
    text=(255, 255, 255), text_disabled=(117, 117, 117), group_text=(255, 255, 255),
    edit_border=(138, 138, 138), field=(48, 48, 48), field_disabled=(40, 40, 40),
)

GREEN = hexc('06b025')
RED = hexc('da2626')
YELLOW = hexc('dab807')


def frame(c, w, h, border, fill, bw=1):
    """a flat box: a border of bw pixels round a fill"""
    if border is not None:
        c.draw(rect(0, 0, w, h), solid(border))
    if fill is not None:
        c.draw(rect(bw if border is not None else 0, bw if border is not None else 0,
                    w - (bw if border is not None else 0), h - (bw if border is not None else 0)), solid(fill))


def paint(c, shape, colour):
    """colour is an rgb tuple or (rgb, opacity)"""
    if colour is None:
        return
    if isinstance(colour[0], tuple):
        c.draw(shape, solid(colour[0]), colour[1])
    else:
        c.draw(shape, solid(colour))


# push button: normal, hot, pressed, disabled, defaulted, defaulted animating

def pushbutton(s, state):
    w, h = 20, 21
    c = Canvas(w, h)
    border, fill = s['btn'][state]
    frame(c, w, h, border, fill)
    if state == 'default':
        c.draw(ring(1, 1, w - 1, h - 1, 0, 1), solid(border), 0.35)
    return c


# check box: 20 states; 1-4 unchecked, 5-8 checked, 9-12 mixed, 13-20 implicit and excluded

def checkbox(s, size, mark, state):
    k = size / 13.0
    c = Canvas(size, size)
    bw = max(1, round(k))
    border, fill = s['box'][state]
    frame(c, size, size, border, fill, bw)
    colour = {'disabled': s['check_disabled'], 'hot': s['check_hot']}.get(state, s['check'])
    if mark == 'checked':
        c.draw(polyline([(2.9 * k, 6.6 * k), (5.2 * k, 9.0 * k), (10.2 * k, 3.9 * k)], 1.35 * k), solid(colour))
    elif mark == 'mixed':
        c.draw(rect(3.5 * k, 3.5 * k, size - 3.5 * k, size - 3.5 * k), solid(colour))
    return c


def radiobutton(s, size, checked, state):
    k = size / 13.0
    c = Canvas(size, size)
    r = size / 2.0
    bw = max(1.0, round(k))
    border, fill = s['box'][state]
    c.draw(disc(r, r, r), solid(border))
    c.draw(disc(r, r, r - bw), solid(fill))
    if checked:
        colour = {'disabled': s['check_disabled'], 'hot': s['check_hot']}.get(state, s['check'])
        c.draw(disc(r, r, 3.0 * k), solid(colour))
    return c


def groupbox(s):
    c = Canvas(10, 10)
    c.draw(ring(0, 0, 10, 10, 0, 1), solid(s['group_border']))
    return c


# scroll bar: flat arrow buttons on the track colour, a flat thumb

def sb_button(s, w, h, state, kind):
    c = Canvas(w, h)
    if kind == 'thumb':
        c.draw(rect(0, 0, w, h), solid(s['sb']['track']))
        c.draw(rect(1, 1, w - 1, h - 1) if w >= 8 and h >= 8 else rect(0, 0, w, h), solid(s['sb']['thumb'][state]))
    else:
        c.draw(rect(0, 0, w, h), solid(s['sb']['arrow'][state]))
    return c


def triangle(colour, size, direction):
    c = Canvas(size, size)
    m = size / 2.0
    d = size / 4.0
    pts = {'up': [(m - d - 0.5, m + d / 2 + 0.5), (m, m - d / 2 - 0.5), (m + d + 0.5, m + d / 2 + 0.5)],
           'down': [(m - d - 0.5, m - d / 2 - 0.5), (m, m + d / 2 + 0.5), (m + d + 0.5, m - d / 2 - 0.5)],
           'left': [(m + d / 2 + 0.5, m - d - 0.5), (m - d / 2 - 0.5, m), (m + d / 2 + 0.5, m + d + 0.5)],
           'right': [(m - d / 2 - 0.5, m - d - 0.5), (m + d / 2 + 0.5, m), (m - d / 2 - 0.5, m + d + 0.5)]}[direction]
    c.draw(polygon(pts), solid(colour))
    return c


def chevron(colour, size, direction, width=1.1):
    c = Canvas(size, size)
    m = size / 2.0
    d = size / 3.2
    e = d / 2.0
    pts = {'up': [(m - d, m + e), (m, m - e), (m + d, m + e)],
           'down': [(m - d, m - e), (m, m + e), (m + d, m - e)],
           'left': [(m + e, m - d), (m - e, m), (m + e, m + d)],
           'right': [(m - e, m - d), (m + e, m), (m - e, m + d)]}[direction]
    c.draw(polyline(pts, width), solid(colour))
    return c


def gripper(s, state, vertical):
    return Canvas(8, 8)


def track(m, state, vertical):
    c = Canvas(17, 4) if vertical else Canvas(4, 17)
    c.draw(rect(0, 0, c.w, c.h), solid(m['sb']['track']))
    return c


def sizebox(m):
    c = Canvas(17, 17)
    for x, y in ((12, 12), (12, 8), (8, 12), (12, 4), (8, 8), (4, 12)):
        c.draw(rect(x, y, x + 2, y + 2), solid(m['grip_dot']))
    return c


# combo box drop-down button: nothing until hot, then a flat box

def drop_button(s, state):
    w, h = 17, 20
    c = Canvas(w, h)
    look = s['drop'][state]
    if look:
        fill, border = look
        frame(c, w, h, border, fill)
    return c


def spin_button(s, state):
    w, h = 15, 11
    c = Canvas(w, h)
    border, fill = s['spin'][state]
    frame(c, w, h, border, fill)
    return c


# tabs

def tab_item(s, state):
    w, h = 16, 21
    t = s['tab']
    c = Canvas(w, h)
    border = t['hot_border'] if state == 'hot' else t['border']
    c.draw(rect(0, 0, w, h), solid(border))
    c.draw(rect(1, 1, w - 1, h), solid(t[state]))
    return c


def tab_pane(s):
    c = Canvas(8, 8)
    c.draw(ring(0, 0, 8, 8, 0, 1), solid(s['tab']['border']))
    return c


# progress bar

def progress_bar(m):
    c = Canvas(12, 12)
    c.draw(rect(0, 0, 12, 12), solid(m['progress_border']))
    c.draw(rect(1, 1, 11, 11), solid(m['progress_trough']))
    return c


def progress_chunk(colour, vertical):
    c = Canvas(13, 10) if vertical else Canvas(10, 13)
    c.draw(rect(0, 0, c.w, c.h), solid(colour))
    return c


# trackbar

def trackbar_track(m, vertical):
    c = Canvas(8, 4)
    for i, colour in enumerate(m['trackbar_rows']):
        c.draw(rect(0, i, 8, i + 1), solid(colour))
    return c.transpose() if vertical else c


def trackbar_thumb(s, state, pointed):
    w, h = 11, 21
    c = Canvas(w, h)
    if pointed:
        shape = polygon([(0, 0), (w, 0), (w, 15), (w / 2.0, h), (0, 15)])
    else:
        shape = rect(0, 0, w, h)
    c.draw(shape, solid(s['thumb'][state]))
    return c


# header

def header_item(m, state):
    w, h = 12, 17
    c = Canvas(w, h)
    c.draw(rect(0, 0, w, h), solid(m['header'][state]))
    c.draw(rect(w - 1, 0, w, h), solid(m['header']['line']))
    c.draw(rect(0, h - 1, w, h), solid(m['header']['line']))
    return c


# tree view expand glyph: closed, opened, hot closed, hot opened

def tree_glyph(s, opened, hot=False):
    t = s['tree']
    colour = t['hot'] if hot else t['opened' if opened else 'closed']
    return chevron(colour, 9, 'down' if opened else 'right', 1.2)


# spin buttons

def spin_glyph(s, direction, state):
    return triangle(s['sb']['glyph'][state] if state != 'pressed' else s['drop_glyph']['pressed'], 7, direction)


# toolbar buttons: flat, square, light blue in the light variant and translucent white in the dark one
# normal, hot, pressed, disabled, checked, hot checked, near hot, other side hot

TOOLBAR_STATES = ['normal', 'hot', 'pressed', 'disabled', 'checked', 'hotchecked', 'nearhot', 'hot']
TOOLBAR_GLYPH = (128, 128, 128)


def toolbar_button(s, state):
    w = h = 16
    c = Canvas(w, h)
    if state in ('normal', 'disabled'):
        return c
    fill, border = s['tb'][state]
    paint(c, ring(0, 0, w, h, 0, 1), border)
    paint(c, rect(1, 1, w - 1, h - 1), fill)
    return c


def toolbar_separator():
    c = Canvas(8, 16)
    c.draw(rect(3, 2, 4, 14), solid((128, 128, 128)), 0.6)
    return c


def small_arrow(direction='down'):
    c = Canvas(7, 7)
    pts = {'down': [(1, 2.5), (6, 2.5), (3.5, 5)], 'right': [(2.5, 1), (2.5, 6), (5, 3.5)]}[direction]
    c.draw(polygon(pts), solid(TOOLBAR_GLYPH))
    return c


def double_chevron(vertical):
    c = Canvas(9, 9)
    for dx in (0, 3.5):
        c.draw(polyline([(1.5 + dx, 2), (3.5 + dx, 4.5), (1.5 + dx, 7)], 1.1), solid(TOOLBAR_GLYPH))
    return c.transpose() if vertical else c


def status_pane():
    c = Canvas(8, 8)
    c.draw(rect(7, 0, 8, 8), solid((128, 128, 128)), 0.5)
    return c


def status_gripper(m):
    c = Canvas(12, 12)
    for x, y in ((9, 9), (9, 5), (5, 9), (9, 1), (5, 5), (1, 9)):
        c.draw(rect(x, y, x + 2, y + 2), solid(m['grip_dot']))
    return c


def rebar_gripper():
    c = Canvas(4, 4)
    c.draw(rect(1, 1, 3, 3), solid((128, 128, 128)), 0.7)
    return c


PUSH_STATES = ['normal', 'hot', 'pressed', 'disabled', 'default', 'default']
BOX_STATES = ['normal', 'hot', 'pressed', 'disabled']
GEL_STATES = ['normal', 'hot', 'pressed', 'disabled']
CHECK_SIZES = [(13, 96), (16, 120), (20, 144), (26, 192)]
RADIO_SIZES = [(13, 96), (16, 120), (19, 144), (25, 192)]


def make_images(name, s):
    images = []

    def save(file, frames, horizontal=False):
        write_bmp(file, frames, horizontal)
        images.append(file)

    save(f'{name}_button.bmp', [pushbutton(s, st) for st in PUSH_STATES])
    for size, _ in CHECK_SIZES:
        frames = [checkbox(s, size, mark, st) for mark in ('none', 'checked', 'mixed') for st in BOX_STATES]
        frames += frames[:4] * 2
        save(f'{name}_checkbox_{size}px.bmp', frames)
    for size, _ in RADIO_SIZES:
        save(f'{name}_radiobutton_{size}px.bmp',
             [radiobutton(s, size, checked, st) for checked in (False, True) for st in BOX_STATES])
    save(f'{name}_groupbox.bmp', [groupbox(s)])

    # scroll bar: 16 arrow states (up, down, left, right) and 4 hover states
    arrow_states = GEL_STATES * 4 + ['normal'] * 4
    save(f'{name}_scrollbar_arrows.bmp', [sb_button(s, 17, 17, st, 'arrow') for st in arrow_states])
    glyphs = [triangle(s['sb']['glyph'][st], 9, d) for d in ('up', 'down', 'left', 'right') for st in GEL_STATES]
    glyphs += [triangle(s['sb']['glyph']['normal'], 9, d) for d in ('up', 'down', 'left', 'right')]
    save(f'{name}_scrollbar_glyphs.bmp', glyphs)
    thumb_states = GEL_STATES + ['normal']
    save(f'{name}_scrollbar_thumb_vert.bmp', [sb_button(s, 17, 17, st, 'thumb') for st in thumb_states])
    save(f'{name}_scrollbar_thumb_horz.bmp', [sb_button(s, 17, 17, st, 'thumb') for st in thumb_states])
    save(f'{name}_scrollbar_gripper_vert.bmp', [gripper(s, st, True) for st in thumb_states])
    save(f'{name}_scrollbar_gripper_horz.bmp', [gripper(s, st, False) for st in thumb_states])

    # combo box button
    save(f'{name}_combobox_button.bmp', [drop_button(s, st) for st in GEL_STATES])
    save(f'{name}_combobox_glyph.bmp', [chevron(s['drop_glyph'][st], 9, 'down') for st in GEL_STATES])

    # spin buttons
    save(f'{name}_spin_button.bmp', [spin_button(s, st) for st in GEL_STATES])
    for d in ('up', 'down', 'left', 'right'):
        save(f'{name}_spin_{d}_glyph.bmp', [spin_glyph(s, d, st) for st in GEL_STATES])

    # tabs
    save(f'{name}_tab_item.bmp', [tab_item(s, st) for st in ('normal', 'hot', 'selected', 'disabled', 'normal')])
    save(f'{name}_tab_pane.bmp', [tab_pane(s)])

    # trackbar thumbs: normal, hot, pressed, focused, disabled
    thumb = ['normal', 'hot', 'pressed', 'normal', 'disabled']
    bottom = [trackbar_thumb(s, st, True) for st in thumb]
    save(f'{name}_trackbar_thumb.bmp', [trackbar_thumb(s, st, False) for st in thumb])
    save(f'{name}_trackbar_thumb_bottom.bmp', bottom)
    save(f'{name}_trackbar_thumb_top.bmp', [f.flip_v() for f in bottom])
    save(f'{name}_trackbar_thumb_vert.bmp', [trackbar_thumb(s, st, False).transpose() for st in thumb])
    save(f'{name}_trackbar_thumb_right.bmp', [f.transpose() for f in bottom])
    save(f'{name}_trackbar_thumb_left.bmp', [f.transpose().flip_h() for f in bottom])

    save(f'{name}_tree_glyph.bmp', [tree_glyph(s, o, h) for o, h in ((False, False), (True, False), (False, True), (True, True))])
    save(f'{name}_toolbar_button.bmp', [toolbar_button(s, st) for st in TOOLBAR_STATES])
    save(f'{name}_rebar_chevron.bmp', [toolbar_button(s, st) for st in ('normal', 'hot', 'pressed')])
    return images


def make_mode_images(mode, m):
    """the images of the light (mode 'light_') or dark (mode 'dark_') variant that are not per scheme"""
    images = []

    def save(file, frames, horizontal=False):
        write_bmp(file, frames, horizontal)
        images.append(file)

    save(f'{mode}track_vert.bmp', [track(m, st, True) for st in ('normal', 'normal', 'pressed', 'disabled', 'normal')])
    save(f'{mode}track_horz.bmp', [track(m, st, False) for st in ('normal', 'normal', 'pressed', 'disabled', 'normal')])
    save(f'{mode}sizebox.bmp', [sizebox(m), sizebox(m).flip_h()] * 4)
    save(f'{mode}progress_bar.bmp', [progress_bar(m)])
    save(f'{mode}trackbar_track.bmp', [trackbar_track(m, False)])
    save(f'{mode}trackbar_track_vert.bmp', [trackbar_track(m, True)])
    save(f'{mode}header_item.bmp', [header_item(m, st) for st in ('normal', 'hot', 'pressed')])
    save(f'{mode}status_gripper.bmp', [status_gripper(m)])
    return images


def make_shared_images():
    images = []

    def save(file, frames, horizontal=False):
        write_bmp(file, frames, horizontal)
        images.append(file)

    save('progress_fill.bmp', [progress_chunk(st, False) for st in (GREEN, RED, YELLOW, GREEN)])
    save('progress_fill_vert.bmp', [progress_chunk(st, True) for st in (GREEN, RED, YELLOW, GREEN)])
    save('toolbar_separator.bmp', [toolbar_separator()])
    save('toolbar_separator_vert.bmp', [toolbar_separator().transpose()])
    save('toolbar_blank.bmp', [Canvas(16, 16)] * len(TOOLBAR_STATES))
    save('toolbar_dropdown_glyph.bmp', [small_arrow('down')] * len(TOOLBAR_STATES))
    save('rebar_chevron_glyph.bmp', [double_chevron(False)] * 3)
    save('rebar_chevron_vert_glyph.bmp', [double_chevron(True)] * 3)
    save('rebar_gripper.bmp', [rebar_gripper()])
    save('status_pane.bmp', [status_pane()])
    return images



# window caption buttons (for programs that draw their own title bar from the theme):
# normal, hot, pushed, disabled, then the same four for an inactive window

WINDOW_BUTTON_STATES = ['normal', 'hot', 'pressed', 'disabled'] * 2


def window_button_bg(s, close, state):
    w, h = 46, 30
    c = Canvas(w, h)
    wn = s['window']
    if state == 'hot':
        c.draw(rect(0, 0, w, h), solid(hexc('e81123') if close else wn['hot']))
    elif state == 'pressed':
        c.draw(rect(0, 0, w, h), solid(hexc('f1707a') if close else wn['pressed']))
    return c


def window_glyph(s, kind, close, state, inactive):
    n = 10
    c = Canvas(n, n)
    wn = s['window']
    if state == 'disabled':
        colour = wn['glyph_disabled']
    elif close and state in ('hot', 'pressed'):
        colour = (255, 255, 255)
    else:
        colour = wn['glyph_inactive'] if inactive and state == 'normal' else wn['glyph']
    if kind == 'close':
        c.draw(polyline([(0.5, 0.5), (9.5, 9.5)], 1.0), solid(colour))
        c.draw(polyline([(9.5, 0.5), (0.5, 9.5)], 1.0), solid(colour))
    elif kind == 'min':
        c.draw(rect(0, 5, 10, 6), solid(colour))
    elif kind == 'max':
        c.draw(ring(0, 0, 10, 10, 0, 1), solid(colour))
    elif kind == 'restore':
        c.draw(both(ring(2, 0, 10, 8, 0, 1), lambda x, y: not (x < 8 and y >= 2)), solid(colour))
        c.draw(ring(0, 2, 8, 10, 0, 1), solid(colour))
    elif kind == 'help':
        c.draw(polyline([(2.5, 3), (3.5, 1), (6.5, 1), (7.5, 3), (5, 5), (5, 7)], 1.0), solid(colour))
        c.draw(rect(4.5, 8.5, 5.5, 9.5), solid(colour))
    return c


def make_window_images(name, s):
    images = []

    def save(file, frames):
        write_bmp(file, frames)
        images.append(file)

    save(f'{name}_window_button.bmp', [window_button_bg(s, False, st) for st in WINDOW_BUTTON_STATES])
    save(f'{name}_window_close_button.bmp', [window_button_bg(s, True, st) for st in WINDOW_BUTTON_STATES])
    for kind in ('close', 'min', 'max', 'restore', 'help'):
        save(f'{name}_window_{kind}_glyph.bmp',
             [window_glyph(s, kind, kind == 'close', st, i >= 4) for i, st in enumerate(WINDOW_BUTTON_STATES)])
    return images


def rgb(c):
    return f'{c[0]} {c[1]} {c[2]}'


def ini(name, s, mode):
    """name: prefix of the scheme images, mode: prefix of the shared light or dark images"""
    dpi_files = lambda part, sizes: ''.join(
        f'ImageFile{i + 1} = {name}_{part}_{size}px.bmp\nMinDpi{i + 1} = {dpi}\n' for i, (size, dpi) in enumerate(sizes))
    return f"""[Globals]
EdgeLightColor = {rgb(s['edge']['light'])}
EdgeHighLightColor = {rgb(s['edge']['highlight'])}
EdgeShadowColor = {rgb(s['edge']['shadow'])}
EdgeDkShadowColor = {rgb(s['edge']['dkshadow'])}
EdgeFillColor = {rgb(s['edge']['fill'])}

[Button.Pushbutton]
BgType = ImageFile
ImageFile = {name}_button.bmp
ImageCount = 6
ImageLayout = Vertical
SizingType = Stretch
SizingMargins = 8, 8, 9, 9
ContentMargins = 3, 3, 3, 3
Transparent = True
TextColor = {rgb(s['text'])}

[Button.Pushbutton(Disabled)]
TextColor = {rgb(s['text_disabled'])}

[Button.Checkbox]
BgType = ImageFile
ImageCount = 20
ImageLayout = Vertical
SizingType = TrueSize
Transparent = True
ImageSelectType = Dpi
TrueSizeScalingType = Dpi
UniformSizing = True
{dpi_files('checkbox', CHECK_SIZES)}
[Button.Checkbox(UncheckedDisabled)]
TextColor = {rgb(s['text_disabled'])}

[Button.Checkbox(CheckedDisabled)]
TextColor = {rgb(s['text_disabled'])}

[Button.Checkbox(MixedDisabled)]
TextColor = {rgb(s['text_disabled'])}

[Button.Radiobutton]
BgType = ImageFile
ImageCount = 8
ImageLayout = Vertical
SizingType = TrueSize
Transparent = True
ImageSelectType = Dpi
TrueSizeScalingType = Dpi
UniformSizing = True
{dpi_files('radiobutton', RADIO_SIZES)}
[Button.Radiobutton(UncheckedDisabled)]
TextColor = {rgb(s['text_disabled'])}

[Button.Radiobutton(CheckedDisabled)]
TextColor = {rgb(s['text_disabled'])}

[Button.Groupbox]
BgType = ImageFile
ImageFile = {name}_groupbox.bmp
SizingType = Stretch
SizingMargins = 4, 4, 4, 4
BorderOnly = True
Transparent = True
TextColor = {rgb(s['group_text'])}

[ScrollBar.ArrowBtn]
BgType = ImageFile
ImageFile = {name}_scrollbar_arrows.bmp
ImageCount = 20
ImageLayout = Vertical
SizingType = Stretch
SizingMargins = 5, 5, 5, 5
Transparent = True
GlyphType = ImageGlyph
GlyphImageFile = {name}_scrollbar_glyphs.bmp
GlyphTransparent = True

[ScrollBar.ThumbBtnVert]
BgType = ImageFile
ImageFile = {name}_scrollbar_thumb_vert.bmp
ImageCount = 5
ImageLayout = Vertical
SizingType = Stretch
SizingMargins = 5, 5, 5, 5
Transparent = True

[ScrollBar.ThumbBtnHorz]
BgType = ImageFile
ImageFile = {name}_scrollbar_thumb_horz.bmp
ImageCount = 5
ImageLayout = Vertical
SizingType = Stretch
SizingMargins = 5, 5, 5, 5
Transparent = True

[ScrollBar.GripperVert]
BgType = ImageFile
ImageFile = {name}_scrollbar_gripper_vert.bmp
ImageCount = 5
ImageLayout = Vertical
SizingType = TrueSize
Transparent = True

[ScrollBar.GripperHorz]
BgType = ImageFile
ImageFile = {name}_scrollbar_gripper_horz.bmp
ImageCount = 5
ImageLayout = Vertical
SizingType = TrueSize
Transparent = True

[ScrollBar.LowerTrackVert]
BgType = ImageFile
ImageFile = {mode}track_vert.bmp
ImageCount = 5
ImageLayout = Vertical
SizingType = Stretch
SizingMargins = 3, 3, 0, 0

[ScrollBar.UpperTrackVert]
BgType = ImageFile
ImageFile = {mode}track_vert.bmp
ImageCount = 5
ImageLayout = Vertical
SizingType = Stretch
SizingMargins = 3, 3, 0, 0

[ScrollBar.LowerTrackHorz]
BgType = ImageFile
ImageFile = {mode}track_horz.bmp
ImageCount = 5
ImageLayout = Vertical
SizingType = Stretch
SizingMargins = 0, 0, 3, 3

[ScrollBar.UpperTrackHorz]
BgType = ImageFile
ImageFile = {mode}track_horz.bmp
ImageCount = 5
ImageLayout = Vertical
SizingType = Stretch
SizingMargins = 0, 0, 3, 3

[ScrollBar.SizeBox]
BgType = ImageFile
ImageFile = {mode}sizebox.bmp
ImageCount = 8
ImageLayout = Vertical
SizingType = TrueSize
Transparent = True
VAlign = Bottom
HAlign = Right

[ScrollBar.SizeBoxBkgnd]
FillColor = {rgb(s['sb']['track'])}

[ComboBox]
BgType = BorderFill
BorderSize = 1
BorderColor = {rgb(s['edit_border'])}
FillColor = {rgb(s['field'])}
ContentMargins = 0, 0, 0, 0

[ComboBox(Disabled)]
FillColor = {rgb(s['field_disabled'])}

[ComboBox.DropDownButton]
BgType = ImageFile
ImageFile = {name}_combobox_button.bmp
ImageCount = 4
ImageLayout = Vertical
SizingType = Stretch
SizingMargins = 5, 5, 5, 5
Transparent = True
GlyphType = ImageGlyph
GlyphImageFile = {name}_combobox_glyph.bmp
GlyphTransparent = True

[Edit]
BgType = BorderFill
BorderSize = 1
BorderColor = {rgb(s['edit_border'])}
FillColor = {rgb(s['field'])}

[Edit.EditText(Disabled)]
FillColor = {rgb(s['field_disabled'])}
TextColor = {rgb(s['text_disabled'])}

[Edit.EditText(ReadOnly)]
FillColor = {rgb(s['field_disabled'])}

[ListView]
BgType = BorderFill
BorderSize = 1
BorderColor = {rgb(s['edit_border'])}
FillColor = {rgb(s['field'])}
TextColor = {rgb(s['text'])}

[ListBox]
BgType = BorderFill
BorderSize = 1
BorderColor = {rgb(s['edit_border'])}
FillColor = {rgb(s['field'])}
TextColor = {rgb(s['text'])}

[TreeView]
BgType = BorderFill
BorderSize = 1
BorderColor = {rgb(s['edit_border'])}
FillColor = {rgb(s['field'])}
TextColor = {rgb(s['text'])}

[Spin.Up]
BgType = ImageFile
ImageFile = {name}_spin_button.bmp
ImageCount = 4
ImageLayout = Vertical
SizingType = Stretch
SizingMargins = 4, 4, 4, 4
Transparent = True
GlyphType = ImageGlyph
GlyphImageFile = {name}_spin_up_glyph.bmp
GlyphTransparent = True

[Spin.Down]
BgType = ImageFile
ImageFile = {name}_spin_button.bmp
ImageCount = 4
ImageLayout = Vertical
SizingType = Stretch
SizingMargins = 4, 4, 4, 4
Transparent = True
GlyphType = ImageGlyph
GlyphImageFile = {name}_spin_down_glyph.bmp
GlyphTransparent = True

[Spin.UpHorz]
BgType = ImageFile
ImageFile = {name}_spin_button.bmp
ImageCount = 4
ImageLayout = Vertical
SizingType = Stretch
SizingMargins = 4, 4, 4, 4
Transparent = True
GlyphType = ImageGlyph
GlyphImageFile = {name}_spin_right_glyph.bmp
GlyphTransparent = True

[Spin.DownHorz]
BgType = ImageFile
ImageFile = {name}_spin_button.bmp
ImageCount = 4
ImageLayout = Vertical
SizingType = Stretch
SizingMargins = 4, 4, 4, 4
Transparent = True
GlyphType = ImageGlyph
GlyphImageFile = {name}_spin_left_glyph.bmp
GlyphTransparent = True

[Tab.Pane]
BgType = ImageFile
ImageFile = {name}_tab_pane.bmp
SizingType = Stretch
SizingMargins = 2, 2, 2, 2
BorderOnly = True
Transparent = True

; tab pages keep the dialog colour, which follows the light or dark desktop theme
; (without a body part, uxtheme would paint the default black border fill as texture)
[Tab.Body]
BgType = None
""" + ''.join(f"""
[Tab.{part}]
BgType = ImageFile
ImageFile = {name}_tab_item.bmp
ImageCount = 5
ImageLayout = Vertical
SizingType = Stretch
SizingMargins = 5, 5, 5, 2
ContentMargins = 3, 3, 3, 2
Transparent = True
TextColor = {rgb(s['text'])}
""" for part in ('TabItem', 'TabItemLeftEdge', 'TabItemRightEdge', 'TabItemBothEdge',
                 'TopTabItem', 'TopTabItemLeftEdge', 'TopTabItemRightEdge', 'TopTabItemBothEdge')) + f"""
[Progress.Bar]
BgType = ImageFile
ImageFile = {mode}progress_bar.bmp
SizingType = Stretch
SizingMargins = 4, 4, 4, 4
ContentMargins = 3, 3, 3, 3
Transparent = True
ProgressChunkSize = 8
ProgressSpaceSize = 2

[Progress.BarVert]
BgType = ImageFile
ImageFile = {mode}progress_bar.bmp
SizingType = Stretch
SizingMargins = 4, 4, 4, 4
ContentMargins = 3, 3, 3, 3
Transparent = True

[Progress.Fill]
BgType = ImageFile
ImageFile = progress_fill.bmp
ImageCount = 4
ImageLayout = Vertical
SizingType = Tile
SizingMargins = 0, 0, 5, 5
Transparent = True

[Progress.FillVert]
BgType = ImageFile
ImageFile = progress_fill_vert.bmp
ImageCount = 4
ImageLayout = Vertical
SizingType = Tile
SizingMargins = 5, 5, 0, 0
Transparent = True

[TrackBar.Track]
BgType = ImageFile
ImageFile = {mode}trackbar_track.bmp
SizingType = Stretch
SizingMargins = 1, 1, 1, 1

[TrackBar.TrackVert]
BgType = ImageFile
ImageFile = {mode}trackbar_track_vert.bmp
SizingType = Stretch
SizingMargins = 1, 1, 1, 1
""" + ''.join(f"""
[TrackBar.{part}]
BgType = ImageFile
ImageFile = {name}_trackbar_{file}.bmp
ImageCount = 5
ImageLayout = Vertical
SizingType = TrueSize
Transparent = True
""" for part, file in (('Thumb', 'thumb'), ('ThumbBottom', 'thumb_bottom'), ('ThumbTop', 'thumb_top'),
                       ('ThumbVert', 'thumb_vert'), ('ThumbLeft', 'thumb_left'), ('ThumbRight', 'thumb_right'))) + f"""
[TrackBar.Tics]
Color = {rgb(s['text_disabled'])}

[TrackBar.TicsVert]
Color = {rgb(s['text_disabled'])}

[Header.HeaderItem]
BgType = ImageFile
ImageFile = {mode}header_item.bmp
ImageCount = 3
ImageLayout = Vertical
SizingType = Stretch
SizingMargins = 1, 3, 1, 4
ContentMargins = 4, 4, 2, 3
TextColor = {rgb(s['text'])}

[TreeView.Glyph]
BgType = ImageFile
ImageFile = {name}_tree_glyph.bmp
ImageCount = 4
ImageLayout = Vertical
SizingType = TrueSize
Transparent = True

; toolbars, status bars and rebars keep the window colour behind them
""" + ''.join(f"""
[ToolBar.{part}]
BgType = ImageFile
ImageFile = {name}_toolbar_button.bmp
ImageCount = 8
ImageLayout = Vertical
SizingType = Stretch
SizingMargins = 4, 4, 4, 4
Transparent = True
""" for part in ('Button', 'DropDownButton', 'SplitButton')) + f"""
[ToolBar.SplitButtonDropDown]
BgType = ImageFile
ImageFile = toolbar_blank.bmp
ImageCount = 8
ImageLayout = Vertical
SizingType = Stretch
Transparent = True
GlyphType = ImageGlyph
GlyphImageFile = toolbar_dropdown_glyph.bmp
GlyphTransparent = True

[ToolBar.Separator]
BgType = ImageFile
ImageFile = toolbar_separator.bmp
SizingType = Stretch
SizingMargins = 3, 3, 3, 3
Transparent = True

[ToolBar.SeparatorVert]
BgType = ImageFile
ImageFile = toolbar_separator_vert.bmp
SizingType = Stretch
SizingMargins = 3, 3, 3, 3
Transparent = True

[Status]
BgType = BorderFill
BorderSize = 0
FillColor = {rgb(s['edge']['fill'])}
TextColor = {rgb(s['text'])}

[Status.Pane]
BgType = ImageFile
ImageFile = status_pane.bmp
SizingType = Stretch
SizingMargins = 0, 2, 1, 1
Transparent = True

[Status.GripperPane]
BgType = None

[Status.Gripper]
BgType = ImageFile
ImageFile = {mode}status_gripper.bmp
SizingType = TrueSize
Transparent = True

[Rebar]
BgType = None

[Rebar.Gripper]
BgType = ImageFile
ImageFile = rebar_gripper.bmp
SizingType = Tile
Transparent = True

[Rebar.GripperVert]
BgType = ImageFile
ImageFile = rebar_gripper.bmp
SizingType = Tile
Transparent = True

[Rebar.Chevron]
BgType = ImageFile
ImageFile = {name}_rebar_chevron.bmp
ImageCount = 3
ImageLayout = Vertical
SizingType = Stretch
SizingMargins = 4, 4, 4, 4
Transparent = True
GlyphType = ImageGlyph
GlyphImageFile = rebar_chevron_glyph.bmp
GlyphTransparent = True

[Rebar.ChevronVert]
BgType = ImageFile
ImageFile = {name}_rebar_chevron.bmp
ImageCount = 3
ImageLayout = Vertical
SizingType = Stretch
SizingMargins = 4, 4, 4, 4
Transparent = True
GlyphType = ImageGlyph
GlyphImageFile = rebar_chevron_vert_glyph.bmp
GlyphTransparent = True

[Window.Caption]
BgType = BorderFill
BorderSize = 0
FillColor = {rgb(s['window']['caption'])}
TextColor = {rgb(s['window']['text'])}
ContentMargins = 0, 0, 0, 0
CaptionMargins = 8, 0, 0, 0

[Window.Caption(Inactive)]
FillColor = {rgb(s['window']['caption_inactive'])}
TextColor = {rgb(s['window']['text_inactive'])}

[Window.Caption(Disabled)]
FillColor = {rgb(s['window']['caption_inactive'])}
TextColor = {rgb(s['window']['text_inactive'])}

[Window.SmallCaption]
BgType = BorderFill
BorderSize = 0
FillColor = {rgb(s['window']['caption'])}
TextColor = {rgb(s['window']['text'])}

[Window.SmallCaption(Inactive)]
FillColor = {rgb(s['window']['caption_inactive'])}
TextColor = {rgb(s['window']['text_inactive'])}

[Window.MaxCaption]
BgType = BorderFill
BorderSize = 0
FillColor = {rgb(s['window']['caption'])}
TextColor = {rgb(s['window']['text'])}
CaptionMargins = 8, 0, 0, 0

[Window.MaxCaption(Inactive)]
FillColor = {rgb(s['window']['caption_inactive'])}
TextColor = {rgb(s['window']['text_inactive'])}

[Window.MinCaption]
BgType = BorderFill
BorderSize = 0
FillColor = {rgb(s['window']['caption'])}
TextColor = {rgb(s['window']['text'])}

[Window.MinCaption(Inactive)]
FillColor = {rgb(s['window']['caption_inactive'])}
TextColor = {rgb(s['window']['text_inactive'])}

[Window.FrameLeft]
BgType = BorderFill
BorderSize = 0
FillColor = {rgb(s['window']['frame'])}

[Window.FrameRight]
BgType = BorderFill
BorderSize = 0
FillColor = {rgb(s['window']['frame'])}

[Window.FrameBottom]
BgType = BorderFill
BorderSize = 0
FillColor = {rgb(s['window']['frame'])}

[Window.SmallFrameLeft]
BgType = BorderFill
BorderSize = 0
FillColor = {rgb(s['window']['frame'])}

[Window.SmallFrameRight]
BgType = BorderFill
BorderSize = 0
FillColor = {rgb(s['window']['frame'])}

[Window.SmallFrameBottom]
BgType = BorderFill
BorderSize = 0
FillColor = {rgb(s['window']['frame'])}

[Window.MinButton]
BgType = ImageFile
ImageFile = {name}_window_button.bmp
ImageCount = 8
ImageLayout = Vertical
SizingType = Stretch
SizingMargins = 0, 0, 0, 0
ContentMargins = 0, 0, 0, 0
Transparent = True
GlyphType = ImageGlyph
GlyphImageFile = {name}_window_min_glyph.bmp
GlyphTransparent = True

[Window.MaxButton]
BgType = ImageFile
ImageFile = {name}_window_button.bmp
ImageCount = 8
ImageLayout = Vertical
SizingType = Stretch
SizingMargins = 0, 0, 0, 0
ContentMargins = 0, 0, 0, 0
Transparent = True
GlyphType = ImageGlyph
GlyphImageFile = {name}_window_max_glyph.bmp
GlyphTransparent = True

[Window.CloseButton]
BgType = ImageFile
ImageFile = {name}_window_close_button.bmp
ImageCount = 8
ImageLayout = Vertical
SizingType = Stretch
SizingMargins = 0, 0, 0, 0
ContentMargins = 0, 0, 0, 0
Transparent = True
GlyphType = ImageGlyph
GlyphImageFile = {name}_window_close_glyph.bmp
GlyphTransparent = True

[Window.SmallCloseButton]
BgType = ImageFile
ImageFile = {name}_window_close_button.bmp
ImageCount = 8
ImageLayout = Vertical
SizingType = Stretch
SizingMargins = 0, 0, 0, 0
ContentMargins = 0, 0, 0, 0
Transparent = True
GlyphType = ImageGlyph
GlyphImageFile = {name}_window_close_glyph.bmp
GlyphTransparent = True

[Window.RestoreButton]
BgType = ImageFile
ImageFile = {name}_window_button.bmp
ImageCount = 8
ImageLayout = Vertical
SizingType = Stretch
SizingMargins = 0, 0, 0, 0
ContentMargins = 0, 0, 0, 0
Transparent = True
GlyphType = ImageGlyph
GlyphImageFile = {name}_window_restore_glyph.bmp
GlyphTransparent = True

[Window.HelpButton]
BgType = ImageFile
ImageFile = {name}_window_button.bmp
ImageCount = 8
ImageLayout = Vertical
SizingType = Stretch
SizingMargins = 0, 0, 0, 0
ContentMargins = 0, 0, 0, 0
Transparent = True
GlyphType = ImageGlyph
GlyphImageFile = {name}_window_help_glyph.bmp
GlyphTransparent = True
"""


def rc_text(ini_text):
    lines = []
    for line in ini_text.split('\n'):
        lines.append('"' + line.replace('\\', '\\\\').replace('"', '\\"') + '\\r\\n"')
    return '\n'.join(lines)


HEADER = """/*
 * Wine Windows 10 visual style, generated by genimages.py
 *
 * Copyright 2026 The412Banner
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "resources.h"

LANGUAGE LANG_ENGLISH, SUBLANG_DEFAULT

STRINGTABLE
{
    IDS_COLOR_DISPLAY_NAME_LIGHT  "Light"
    IDS_COLOR_DISPLAY_NAME_DARK   "Dark"
    IDS_COLOR_TOOLTIP_LIGHT       "Light, for a light color scheme"
    IDS_COLOR_TOOLTIP_DARK        "Dark, for a dark color scheme"
    IDS_SIZE_DISPLAY_NAME_NORMAL  "Normal"
    IDS_SIZE_TOOLTIP_NORMAL       "Normal"
}

LANGUAGE LANG_NEUTRAL, SUBLANG_NEUTRAL

1 PACKTHEM_VERSION {0x3}

1 COLORNAMES
{
"Light\\0"
"Dark\\0"
"\\0"
}

1 SIZENAMES
{
"NormalSize\\0"
"\\0"
}

1 FILERESNAMES
{
"LIGHT_INI\\0"
"DARK_INI\\0"
"\\0"
}

THEMES_INI TEXTFILE
{
"[documentation]\\r\\n"
"DisplayName = Wine Windows 10\\r\\n"
"ToolTip = Windows 10 style buttons and controls\\r\\n"
}
"""

FOOTER = """
#define WINE_FILEDESCRIPTION_STR "Wine XP Theme"
#define WINE_FILENAME_STR "winexp.msstyles"
#define WINE_FILEVERSION 1,0,0,1

#include "wine/wine_common_ver.rc"
"""


def main():
    images = make_shared_images() + make_mode_images('light_', LIGHT) + make_mode_images('dark_', DARK)
    out = [HEADER]
    for name, p in (('light', LIGHT), ('dark', DARK)):
        images += make_images(name, p) + make_window_images(name, p)
        text = ini(name, p, name + '_')
        out.append(f'\n{name.upper()}_INI TEXTFILE\n{{\n{rc_text(text)}\n}}\n')
    out.append('\n/* images */\n')
    for file in images:
        out.append(f'/* @makedep: {file} */\n{file.replace(".", "_").upper()} BITMAP "{file}"\n\n')
    out.append(FOOTER)
    with open('win10.rc', 'w') as f:
        f.write(''.join(out))
    print(f'{len(images)} images')


if __name__ == '__main__':
    main()
