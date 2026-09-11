#!/usr/bin/env python3
#
# Generates the bitmaps and winexp.rc of the Wine XP visual style.
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


# the three colour schemes

GREEN = [(0.0, hexc('d8f6d6')), (0.35, hexc('5dd65b')), (0.5, hexc('2bc22b')), (0.65, hexc('5dd65b')), (1.0, hexc('d8f6d6'))]
RED = [(0.0, hexc('f8d8d8')), (0.35, hexc('e36b6b')), (0.5, hexc('cf3434')), (0.65, hexc('e36b6b')), (1.0, hexc('f8d8d8'))]
YELLOW = [(0.0, hexc('fff4c8')), (0.35, hexc('f0cf4c')), (0.5, hexc('ddb21c')), (0.65, hexc('f0cf4c')), (1.0, hexc('fff4c8'))]
ORANGE = (hexc('fff0cf'), hexc('f8b330'))

SCHEMES = {
    'blue': dict(
        btn_border=hexc('003c74'), btn_default=(hexc('cee7ff'), hexc('6982ee')),
        box_border=hexc('1c5180'),
        sb=dict(border=hexc('9db6ed'), fill=(hexc('dde6fe'), hexc('bccefb')),
                hot_border=hexc('7e9be3'), hot_fill=(hexc('eaf0ff'), hexc('cddbff')),
                pressed_border=hexc('4f6fd6'), pressed_fill=(hexc('8aa6f0'), hexc('6e8ef1')),
                glyph=hexc('4d6185'), grip_light=hexc('eef4fe'), grip_dark=hexc('8cb0f8')),
        edit_border=(127, 157, 185), group_text=(0, 70, 213), group_text_dark=(140, 180, 255),
        tab_border=hexc('919b9c'), thumb_border=hexc('6a80b4'), tb_border=hexc('bfbcae'),
    ),
    'olive': dict(
        btn_border=hexc('3d5a1d'), btn_default=(hexc('d3e6a4'), hexc('8ea94b')),
        box_border=hexc('5c7431'),
        sb=dict(border=hexc('b3c393'), fill=(hexc('eff3df'), hexc('d7e0bb')),
                hot_border=hexc('9db372'), hot_fill=(hexc('f6f9ec'), hexc('e3eacd')),
                pressed_border=hexc('7b8f4c'), pressed_fill=(hexc('bccb92'), hexc('a6b976')),
                glyph=hexc('657746'), grip_light=hexc('fbfcf5'), grip_dark=hexc('a6b978')),
        edit_border=(164, 185, 127), group_text=(76, 99, 34), group_text_dark=(190, 212, 140),
        tab_border=hexc('a2a88a'), thumb_border=hexc('7d8c58'), tb_border=hexc('bcc0a4'),
    ),
    'silver': dict(
        btn_border=hexc('707086'), btn_default=(hexc('d7dbf2'), hexc('9ea3d3')),
        box_border=hexc('77778e'),
        sb=dict(border=hexc('b8b8c9'), fill=(hexc('fbfbfd'), hexc('dedee8')),
                hot_border=hexc('9c9cb4'), hot_fill=(hexc('ffffff'), hexc('e9e9f1')),
                pressed_border=hexc('83839d'), pressed_fill=(hexc('c9c9da'), hexc('b3b3c8')),
                glyph=hexc('55556d'), grip_light=hexc('ffffff'), grip_dark=hexc('a9a9bd')),
        edit_border=(165, 172, 181), group_text=(60, 60, 96), group_text_dark=(208, 208, 230),
        tab_border=hexc('a5a5b8'), thumb_border=hexc('7b7b94'), tb_border=hexc('b2b2c2'),
    ),
}

FACE = [(0.0, hexc('ffffff')), (0.5, hexc('f6f5f0')), (0.8, hexc('eceae2')), (1.0, hexc('d6d0c5'))]
FACE_PRESSED = [(0.0, hexc('e1dfd3')), (0.3, hexc('e6e4d9')), (1.0, hexc('f3f2ed'))]
DISABLED_BORDER = hexc('c9c7ba')
DISABLED_FACE = hexc('f5f4ea')
BOX_FILL = (hexc('dcdcd7'), hexc('ffffff'))
BOX_PRESSED = (hexc('b0b0a7'), hexc('e3e1d4'))
CHECK = hexc('21a121')
CHECK_DISABLED = hexc('cac8bb')
DOT = [(0.0, hexc('6be067')), (1.0, hexc('1a9b17'))]
DISABLED_GLYPH = hexc('c9c8c2')
GROUP_BORDER = hexc('d0d0bf')


# push button: normal, hot, pressed, disabled, defaulted, defaulted animating

def pushbutton(s, state):
    w, h = 20, 21
    c = Canvas(w, h)
    border = DISABLED_BORDER if state == 'disabled' else s['btn_border']
    c.draw(ring(0, 0, w, h, 3, 1), solid(border))
    if state == 'pressed':
        face = vgrad(1, h - 1, FACE_PRESSED)
    elif state == 'disabled':
        face = solid(DISABLED_FACE)
    else:
        face = vgrad(1, h - 1, FACE)
    c.draw(rrect(1, 1, w - 1, h - 1, 2), face)
    if state == 'hot':
        c.draw(ring(1, 1, w - 1, h - 1, 2, 2), vgrad(1, h - 1, [(0.0, ORANGE[0]), (1.0, ORANGE[1])]))
    elif state == 'default':
        top, bottom = s['btn_default']
        c.draw(ring(1, 1, w - 1, h - 1, 2, 2), vgrad(1, h - 1, [(0.0, top), (1.0, bottom)]))
    return c


# check box: 20 states; 1-4 unchecked, 5-8 checked, 9-12 mixed, 13-20 implicit and excluded

def checkbox(s, size, mark, state):
    k = size / 13.0
    c = Canvas(size, size)
    bw = max(1, round(k))
    c.draw(ring(0, 0, size, size, 0, bw), solid(DISABLED_BORDER if state == 'disabled' else s['box_border']))
    if state == 'disabled':
        fill = solid((255, 255, 255))
    elif state == 'pressed':
        fill = dgrad(bw, bw, size - bw, size - bw, *BOX_PRESSED)
    else:
        fill = dgrad(bw, bw, size - bw, size - bw, *BOX_FILL)
    c.draw(rect(bw, bw, size - bw, size - bw), fill)
    if state in ('hot', 'pressed'):
        c.draw(ring(bw, bw, size - bw, size - bw, 0, max(1, round(2 * k))),
               vgrad(bw, size - bw, [(0.0, ORANGE[0]), (1.0, ORANGE[1])]))
    colour = CHECK_DISABLED if state == 'disabled' else CHECK
    if mark == 'checked':
        c.draw(polyline([(3.3 * k, 6.3 * k), (5.4 * k, 8.6 * k), (9.8 * k, 4.2 * k)], 2.1 * k), solid(colour))
    elif mark == 'mixed':
        c.draw(rect(4 * k, 4 * k, size - 4 * k, size - 4 * k), solid(colour))
    return c


def radiobutton(s, size, checked, state):
    k = size / 13.0
    c = Canvas(size, size)
    r = size / 2.0
    bw = max(1.0, round(k))
    c.draw(annulus(r, r, r, r - bw), solid(DISABLED_BORDER if state == 'disabled' else s['box_border']))
    if state == 'disabled':
        fill = solid((255, 255, 255))
    elif state == 'pressed':
        fill = dgrad(0, 0, size, size, *BOX_PRESSED)
    else:
        fill = dgrad(0, 0, size, size, *BOX_FILL)
    c.draw(disc(r, r, r - bw), fill)
    if state in ('hot', 'pressed'):
        c.draw(annulus(r, r, r - bw, r - bw - 2 * k), vgrad(0, size, [(0.0, ORANGE[0]), (1.0, ORANGE[1])]))
    if checked:
        if state == 'disabled':
            c.draw(disc(r, r, 2.6 * k), solid(CHECK_DISABLED))
        else:
            c.draw(disc(r, r, 2.6 * k), radial(r - 0.7 * k, r - 0.7 * k, 3.3 * k, DOT))
    return c


def groupbox(s):
    c = Canvas(10, 10)
    c.draw(ring(0, 0, 10, 10, 3, 1), solid(GROUP_BORDER))
    return c


# scroll bar and combo box buttons

def gel(s, w, h, state, vertical=True):
    sb = s['sb']
    c = Canvas(w, h)
    if state == 'disabled':
        border, fill = hexc('e3e3db'), (hexc('f5f5f0'), hexc('f0f0ea'))
    elif state == 'hot':
        border, fill = sb['hot_border'], sb['hot_fill']
    elif state == 'pressed':
        border, fill = sb['pressed_border'], sb['pressed_fill']
    else:
        border, fill = sb['border'], sb['fill']
    c.draw(rrect(0, 0, w, h, 2.5), solid(border))
    grad = [(0.0, fill[0]), (1.0, fill[1])]
    c.draw(rrect(1, 1, w - 1, h - 1, 1.5), vgrad(1, h - 1, grad) if vertical else hgrad(1, w - 1, grad))
    if state != 'pressed':
        c.draw(rect(2, 1, w - 2, 2), solid((255, 255, 255)), 0.6)
    return c


def chevron(s, size, direction, state, width=2.2):
    c = Canvas(size, size)
    m = size / 2.0
    d = size / 3.0
    e = d / 2.0 + 0.25
    pts = {'up': [(m - d, m + e), (m, m - e), (m + d, m + e)],
           'down': [(m - d, m - e), (m, m + e), (m + d, m - e)],
           'left': [(m + e, m - d), (m - e, m), (m + e, m + d)],
           'right': [(m - e, m - d), (m + e, m), (m - e, m + d)]}[direction]
    colour = {'pressed': (255, 255, 255), 'disabled': DISABLED_GLYPH}.get(state, s['sb']['glyph'])
    c.draw(polyline(pts, width), solid(colour))
    return c


def gripper(s, state, vertical):
    c = Canvas(8, 8)
    if state != 'disabled':
        light, dark = s['sb']['grip_light'], s['sb']['grip_dark']
        for i in range(0, 8, 2):
            if vertical:
                c.draw(rect(1, i, 7, i + 1), solid(light))
                c.draw(rect(2, i + 1, 8, i + 2), solid(dark))
            else:
                c.draw(rect(i, 1, i + 1, 7), solid(light))
                c.draw(rect(i + 1, 2, i + 2, 8), solid(dark))
    return c


def track(state, vertical):
    if state == 'pressed':
        edge, centre = hexc('c2c1b8'), hexc('d9d8d0')
    elif state == 'disabled':
        edge, centre = hexc('f2f2ee'), hexc('fafaf8')
    else:
        edge, centre = hexc('eeede5'), hexc('fefefb')
    stops = [(0.0, edge), (0.35, centre), (0.65, centre), (1.0, edge)]
    if vertical:
        c = Canvas(17, 4)
        c.draw(rect(0, 0, 17, 4), hgrad(0, 17, stops))
    else:
        c = Canvas(4, 17)
        c.draw(rect(0, 0, 4, 17), vgrad(0, 17, stops))
    return c


def sizebox():
    c = Canvas(17, 17)
    for x, y in ((12, 12), (12, 8), (8, 12), (12, 4), (8, 8), (4, 12)):
        c.draw(rect(x, y, x + 2, y + 2), solid(hexc('b8b6a8')))
        c.draw(rect(x, y, x + 1, y + 1), solid((255, 255, 255)))
    return c


# tabs

def tab_item(s, state):
    w, h = 16, 21
    c = Canvas(w, h)
    border = DISABLED_BORDER if state == 'disabled' else s['tab_border']
    c.draw(both(ring(0, 0, w, h + 4, 3, 1), rect(0, 0, w, h)), solid(border))
    if state == 'selected':
        fill = vgrad(1, h, [(0.0, hexc('ffffff')), (1.0, hexc('fcfcfe'))])
    elif state == 'disabled':
        fill = solid(DISABLED_FACE)
    else:
        fill = vgrad(1, h, [(0.0, hexc('ffffff')), (1.0, hexc('ecebe6'))])
    c.draw(both(rrect(1, 1, w - 1, h + 4, 2), rect(0, 0, w, h)), fill)
    if state in ('hot', 'selected'):
        c.draw(both(rrect(0, 0, w, h + 4, 3), rect(0, 0, w, 3)),
               vgrad(0, 3, [(0.0, hexc('e68b2c')), (1.0, hexc('ffc73c'))]))
    return c


def tab_pane(s):
    c = Canvas(8, 8)
    c.draw(ring(0, 0, 8, 8, 0, 1), solid(s['tab_border']))
    return c


# progress bar

def progress_bar():
    c = Canvas(12, 12)
    c.draw(rrect(0, 0, 12, 12, 3), solid(hexc('b2b2b2')))
    c.draw(rrect(1, 1, 11, 11, 2), solid((255, 255, 255)))
    return c


def progress_chunk(stops, vertical):
    if vertical:
        c = Canvas(13, 10)
        c.draw(rect(0, 2, 13, 10), hgrad(0, 13, stops))
    else:
        c = Canvas(10, 13)
        c.draw(rect(0, 0, 8, 13), vgrad(0, 13, stops))
    return c


# trackbar

def trackbar_track(vertical):
    rows = [hexc('9d9c99'), hexc('d7d6cf'), hexc('f3f2ec'), (255, 255, 255)]
    c = Canvas(8, 4)
    for i, colour in enumerate(rows):
        c.draw(rect(0, i, 8, i + 1), solid(colour))
    return c.transpose() if vertical else c


def trackbar_thumb(s, state, pointed):
    w, h = 11, 21
    c = Canvas(w, h)
    body_h = 15 if pointed else h
    if pointed:
        outline = polygon([(0, 2), (2, 0), (w - 2, 0), (w, 2), (w, body_h), (w / 2.0, h), (0, body_h)])
        inner = polygon([(1, 2.5), (2.5, 1), (w - 2.5, 1), (w - 1, 2.5), (w - 1, body_h - 0.3),
                         (w / 2.0, h - 1.6), (1, body_h - 0.3)])
    else:
        outline = rrect(0, 0, w, h, 2)
        inner = rrect(1, 1, w - 1, h - 1, 1)
    border = DISABLED_BORDER if state == 'disabled' else s['thumb_border']
    c.draw(outline, solid(border))
    if state == 'pressed':
        body = vgrad(1, h, [(0.0, hexc('e1dfd3')), (1.0, hexc('cfccbe'))])
    elif state == 'disabled':
        body = solid(DISABLED_FACE)
    else:
        body = vgrad(1, h, [(0.0, hexc('ffffff')), (1.0, hexc('e7e6df'))])
    c.draw(inner, body)
    if state != 'disabled':
        accent = ORANGE[1] if state == 'hot' else CHECK
        light = ORANGE[0] if state == 'hot' else hexc('8fdc8f')
        band = rect(0, body_h - 3, w, h) if pointed else rect(0, h - 4, w, h)
        c.draw(both(inner, band), vgrad(body_h - 3 if pointed else h - 4, h, [(0.0, light), (1.0, accent)]))
    return c


# header

def header_item(state):
    w, h = 12, 17
    c = Canvas(w, h)
    top = hexc('dedcd2') if state == 'pressed' else hexc('ffffff')
    bottom = hexc('dedcd2') if state == 'pressed' else hexc('ebeadb')
    c.draw(rect(0, 0, w, h - 3), vgrad(0, h - 3, [(0.0, top), (1.0, bottom)]))
    band = ([hexc('f9c64f'), hexc('f9b11f'), hexc('e39a0e')] if state == 'hot'
            else [hexc('e2decd'), hexc('d6d2c2'), hexc('cbc7b8')])
    for i, colour in enumerate(band):
        c.draw(rect(0, h - 3 + i, w, h - 2 + i), solid(colour))
    if state != 'pressed':
        c.draw(rect(w - 2, 3, w - 1, h - 5), solid(hexc('aca899')))
        c.draw(rect(w - 1, 3, w, h - 5), solid((255, 255, 255)))
    return c


# tree view expand glyph: closed, opened, hot closed, hot opened

def tree_glyph(s, opened):
    c = Canvas(9, 9)
    c.draw(ring(0, 0, 9, 9, 0, 1), solid(hexc('7898b5')))
    c.draw(rect(1, 1, 8, 8), dgrad(1, 1, 8, 8, hexc('ffffff'), hexc('c6c6bb')))
    c.draw(rect(2, 4, 7, 5), solid((0, 0, 0)))
    if not opened:
        c.draw(rect(4, 2, 5, 7), solid((0, 0, 0)))
    return c


# spin buttons

def spin_glyph(s, direction, state):
    return chevron(s, 7, direction, state, 1.5)


# toolbar buttons: translucent faces, so that they fit light and dark windows alike
# normal, hot, pressed, disabled, checked, hot checked, near hot, other side hot

TOOLBAR_STATES = ['normal', 'hot', 'pressed', 'disabled', 'checked', 'hotchecked', 'nearhot', 'hot']
TOOLBAR_GLYPH = (120, 120, 120)


def toolbar_button(s, state):
    w = h = 16
    c = Canvas(w, h)
    if state in ('normal', 'disabled'):
        return c
    face, alpha = {'pressed': ((0, 0, 0), 0.14), 'checked': ((255, 255, 255), 0.4),
                   'hotchecked': ((255, 255, 255), 0.5)}.get(state, ((255, 255, 255), 0.3))
    c.draw(rrect(1, 1, w - 1, h - 1, 2), solid(face), alpha)
    c.draw(ring(0, 0, w, h, 3, 1), solid(s['tb_border']), 0.5 if state == 'nearhot' else 0.9)
    return c


def toolbar_separator():
    c = Canvas(8, 16)
    c.draw(rect(3, 2, 4, 14), solid(hexc('aca899')), 0.8)
    c.draw(rect(4, 2, 5, 14), solid((255, 255, 255)), 0.6)
    return c


def small_arrow(direction='down'):
    c = Canvas(7, 7)
    pts = {'down': [(1, 2.5), (6, 2.5), (3.5, 5)], 'right': [(2.5, 1), (2.5, 6), (5, 3.5)]}[direction]
    c.draw(polygon(pts), solid(TOOLBAR_GLYPH))
    return c


def double_chevron(vertical):
    c = Canvas(9, 9)
    for dx in (0, 3.5):
        c.draw(polyline([(1.5 + dx, 2), (3.5 + dx, 4.5), (1.5 + dx, 7)], 1.3), solid(TOOLBAR_GLYPH))
    return c.transpose() if vertical else c


def status_pane():
    c = Canvas(8, 8)
    c.draw(rect(6, 0, 7, 8), solid(hexc('aca899')), 0.8)
    c.draw(rect(7, 0, 8, 8), solid((255, 255, 255)), 0.6)
    return c


def status_gripper():
    c = Canvas(12, 12)
    for x, y in ((9, 9), (9, 5), (5, 9), (9, 1), (5, 5), (1, 9)):
        c.draw(rect(x, y, x + 2, y + 2), solid(hexc('b8b6a8')))
        c.draw(rect(x, y, x + 1, y + 1), solid((255, 255, 255)))
    return c


def rebar_gripper():
    c = Canvas(4, 4)
    c.draw(rect(1, 1, 2, 2), solid(hexc('a9a596')), 0.9)
    c.draw(rect(0, 0, 1, 1), solid((255, 255, 255)), 0.8)
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
    save(f'{name}_scrollbar_arrows.bmp', [gel(s, 17, 17, st) for st in arrow_states])
    glyphs = [chevron(s, 9, d, st) for d in ('up', 'down', 'left', 'right') for st in GEL_STATES]
    glyphs += [chevron(s, 9, d, 'normal') for d in ('up', 'down', 'left', 'right')]
    save(f'{name}_scrollbar_glyphs.bmp', glyphs)
    thumb_states = GEL_STATES + ['normal']
    save(f'{name}_scrollbar_thumb_vert.bmp', [gel(s, 17, 17, st, vertical=False) for st in thumb_states])
    save(f'{name}_scrollbar_thumb_horz.bmp', [gel(s, 17, 17, st) for st in thumb_states])
    save(f'{name}_scrollbar_gripper_vert.bmp', [gripper(s, st, True) for st in thumb_states])
    save(f'{name}_scrollbar_gripper_horz.bmp', [gripper(s, st, False) for st in thumb_states])

    # combo box button
    save(f'{name}_combobox_button.bmp', [gel(s, 17, 20, st) for st in GEL_STATES])
    save(f'{name}_combobox_glyph.bmp', [chevron(s, 9, 'down', st) for st in GEL_STATES])

    # spin buttons
    save(f'{name}_spin_button.bmp', [gel(s, 15, 11, st) for st in GEL_STATES])
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

    save(f'{name}_tree_glyph.bmp', [tree_glyph(s, o) for o in (False, True, False, True)])
    save(f'{name}_toolbar_button.bmp', [toolbar_button(s, st) for st in TOOLBAR_STATES])
    save(f'{name}_rebar_chevron.bmp', [toolbar_button(s, st) for st in ('normal', 'hot', 'pressed')])
    return images


def make_shared_images():
    images = []

    def save(file, frames, horizontal=False):
        write_bmp(file, frames, horizontal)
        images.append(file)

    save('track_vert.bmp', [track(st, True) for st in ('normal', 'normal', 'pressed', 'disabled', 'normal')])
    save('track_horz.bmp', [track(st, False) for st in ('normal', 'normal', 'pressed', 'disabled', 'normal')])
    save('sizebox.bmp', [sizebox(), sizebox().flip_h()] * 4)
    save('progress_bar.bmp', [progress_bar()])
    save('progress_fill.bmp', [progress_chunk(st, False) for st in (GREEN, RED, YELLOW, GREEN)])
    save('progress_fill_vert.bmp', [progress_chunk(st, True) for st in (GREEN, RED, YELLOW, GREEN)])
    save('trackbar_track.bmp', [trackbar_track(False)])
    save('trackbar_track_vert.bmp', [trackbar_track(True)])
    save('header_item.bmp', [header_item(st) for st in ('normal', 'hot', 'pressed')])
    save('toolbar_separator.bmp', [toolbar_separator()])
    save('toolbar_separator_vert.bmp', [toolbar_separator().transpose()])
    save('toolbar_blank.bmp', [Canvas(16, 16)] * len(TOOLBAR_STATES))
    save('toolbar_dropdown_glyph.bmp', [small_arrow('down')] * len(TOOLBAR_STATES))
    save('rebar_chevron_glyph.bmp', [double_chevron(False)] * 3)
    save('rebar_chevron_vert_glyph.bmp', [double_chevron(True)] * 3)
    save('rebar_gripper.bmp', [rebar_gripper()])
    save('status_pane.bmp', [status_pane()])
    save('status_gripper.bmp', [status_gripper()])
    return images


def rgb(c):
    return f'{c[0]} {c[1]} {c[2]}'


def ini(name, s, dark=False):
    dpi_files = lambda part, sizes: ''.join(
        f'ImageFile{i + 1} = {name}_{part}_{size}px.bmp\nMinDpi{i + 1} = {dpi}\n' for i, (size, dpi) in enumerate(sizes))
    return f"""[Globals]
EdgeLightColor = 241 239 226
EdgeHighLightColor = 255 255 255
EdgeShadowColor = 172 168 153
EdgeDkShadowColor = 113 111 100
EdgeFillColor = 236 233 216

[Button.Pushbutton]
BgType = ImageFile
ImageFile = {name}_button.bmp
ImageCount = 6
ImageLayout = Vertical
SizingType = Stretch
SizingMargins = 8, 8, 9, 9
ContentMargins = 3, 3, 3, 3
Transparent = True
TextColor = 0 0 0

[Button.Pushbutton(Disabled)]
TextColor = 161 161 146

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
TextColor = 161 161 146

[Button.Checkbox(CheckedDisabled)]
TextColor = 161 161 146

[Button.Checkbox(MixedDisabled)]
TextColor = 161 161 146

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
TextColor = 161 161 146

[Button.Radiobutton(CheckedDisabled)]
TextColor = 161 161 146

[Button.Groupbox]
BgType = ImageFile
ImageFile = {name}_groupbox.bmp
SizingType = Stretch
SizingMargins = 4, 4, 4, 4
BorderOnly = True
Transparent = True
TextColor = {rgb(s['group_text_dark'] if dark else s['group_text'])}

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
ImageFile = track_vert.bmp
ImageCount = 5
ImageLayout = Vertical
SizingType = Stretch
SizingMargins = 3, 3, 0, 0

[ScrollBar.UpperTrackVert]
BgType = ImageFile
ImageFile = track_vert.bmp
ImageCount = 5
ImageLayout = Vertical
SizingType = Stretch
SizingMargins = 3, 3, 0, 0

[ScrollBar.LowerTrackHorz]
BgType = ImageFile
ImageFile = track_horz.bmp
ImageCount = 5
ImageLayout = Vertical
SizingType = Stretch
SizingMargins = 0, 0, 3, 3

[ScrollBar.UpperTrackHorz]
BgType = ImageFile
ImageFile = track_horz.bmp
ImageCount = 5
ImageLayout = Vertical
SizingType = Stretch
SizingMargins = 0, 0, 3, 3

[ScrollBar.SizeBox]
BgType = ImageFile
ImageFile = sizebox.bmp
ImageCount = 8
ImageLayout = Vertical
SizingType = TrueSize
Transparent = True
VAlign = Bottom
HAlign = Right

[ScrollBar.SizeBoxBkgnd]
FillColor = 254 254 251

[ComboBox]
BgType = BorderFill
BorderSize = 1
BorderColor = {rgb(s['edit_border'])}
FillColor = 255 255 255
ContentMargins = 0, 0, 0, 0

[ComboBox(Disabled)]
FillColor = 235 235 228

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
FillColor = 255 255 255

[Edit.EditText(Disabled)]
FillColor = 235 235 228
TextColor = 161 161 146

[Edit.EditText(ReadOnly)]
FillColor = 235 235 228

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
TextColor = 0 0 0
""" for part in ('TabItem', 'TabItemLeftEdge', 'TabItemRightEdge', 'TabItemBothEdge',
                 'TopTabItem', 'TopTabItemLeftEdge', 'TopTabItemRightEdge', 'TopTabItemBothEdge')) + f"""
[Progress.Bar]
BgType = ImageFile
ImageFile = progress_bar.bmp
SizingType = Stretch
SizingMargins = 4, 4, 4, 4
ContentMargins = 3, 3, 3, 3
Transparent = True
ProgressChunkSize = 8
ProgressSpaceSize = 2

[Progress.BarVert]
BgType = ImageFile
ImageFile = progress_bar.bmp
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
ImageFile = trackbar_track.bmp
SizingType = Stretch
SizingMargins = 1, 1, 1, 1

[TrackBar.TrackVert]
BgType = ImageFile
ImageFile = trackbar_track_vert.bmp
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
Color = 161 161 146

[TrackBar.TicsVert]
Color = 161 161 146

[Header.HeaderItem]
BgType = ImageFile
ImageFile = header_item.bmp
ImageCount = 3
ImageLayout = Vertical
SizingType = Stretch
SizingMargins = 1, 3, 1, 4
ContentMargins = 4, 4, 2, 3
TextColor = 0 0 0

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
BgType = None

[Status.Pane]
BgType = ImageFile
ImageFile = status_pane.bmp
SizingType = Stretch
SizingMargins = 0, 2, 1, 1
Transparent = True

[Status.GripperPane]
BgType = ImageFile
ImageFile = status_pane.bmp
SizingType = Stretch
SizingMargins = 0, 2, 1, 1
Transparent = True

[Status.Gripper]
BgType = ImageFile
ImageFile = status_gripper.bmp
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
"""


def rc_text(ini_text):
    lines = []
    for line in ini_text.split('\n'):
        lines.append('"' + line.replace('\\', '\\\\').replace('"', '\\"') + '\\r\\n"')
    return '\n'.join(lines)


HEADER = """/*
 * Wine XP visual style, generated by genimages.py
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
    IDS_COLOR_DISPLAY_NAME_BLUE        "Default (blue)"
    IDS_COLOR_DISPLAY_NAME_OLIVE       "Olive Green"
    IDS_COLOR_DISPLAY_NAME_SILVER      "Silver"
    IDS_COLOR_DISPLAY_NAME_BLUE_DARK   "Default (blue), dark windows"
    IDS_COLOR_DISPLAY_NAME_OLIVE_DARK  "Olive Green, dark windows"
    IDS_COLOR_DISPLAY_NAME_SILVER_DARK "Silver, dark windows"
    IDS_COLOR_TOOLTIP_BLUE             "Default (blue)"
    IDS_COLOR_TOOLTIP_OLIVE            "Olive Green"
    IDS_COLOR_TOOLTIP_SILVER           "Silver"
    IDS_COLOR_TOOLTIP_BLUE_DARK        "Default (blue) for a dark color scheme"
    IDS_COLOR_TOOLTIP_OLIVE_DARK       "Olive Green for a dark color scheme"
    IDS_COLOR_TOOLTIP_SILVER_DARK      "Silver for a dark color scheme"
    IDS_SIZE_DISPLAY_NAME_NORMAL  "Normal"
    IDS_SIZE_TOOLTIP_NORMAL       "Normal"
}

LANGUAGE LANG_NEUTRAL, SUBLANG_NEUTRAL

1 PACKTHEM_VERSION {0x3}

1 COLORNAMES
{
"Blue\\0"
"Olive\\0"
"Silver\\0"
"BlueDark\\0"
"OliveDark\\0"
"SilverDark\\0"
"\\0"
}

1 SIZENAMES
{
"NormalSize\\0"
"\\0"
}

1 FILERESNAMES
{
"BLUE_INI\\0"
"OLIVE_INI\\0"
"SILVER_INI\\0"
"BLUEDARK_INI\\0"
"OLIVEDARK_INI\\0"
"SILVERDARK_INI\\0"
"\\0"
}

THEMES_INI TEXTFILE
{
"[documentation]\\r\\n"
"DisplayName = Wine XP\\r\\n"
"ToolTip = Windows XP style buttons and controls\\r\\n"
}
"""

FOOTER = """
#define WINE_FILEDESCRIPTION_STR "Wine XP Theme"
#define WINE_FILENAME_STR "winexp.msstyles"
#define WINE_FILEVERSION 1,0,0,1

#include "wine/wine_common_ver.rc"
"""


def main():
    images = make_shared_images()
    out = [HEADER]
    for name in ('blue', 'olive', 'silver'):
        images += make_images(name, SCHEMES[name])
    # the dark variants share the bitmaps, only a few text colours change
    for dark in (False, True):
        for name in ('blue', 'olive', 'silver'):
            res = name.upper() + ('DARK' if dark else '') + '_INI'
            out.append(f'\n{res} TEXTFILE\n{{\n{rc_text(ini(name, SCHEMES[name], dark))}\n}}\n')
    out.append('\n/* images */\n')
    for file in images:
        out.append(f'/* @makedep: {file} */\n{file.replace(".", "_").upper()} BITMAP "{file}"\n\n')
    out.append(FOOTER)
    with open('winexp.rc', 'w') as f:
        f.write(''.join(out))
    print(f'{len(images)} images')


if __name__ == '__main__':
    main()
