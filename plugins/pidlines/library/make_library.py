"""make_library.py - P&ID symbol library (valves, actuators, fittings, equipment,
instruments, annotation) for LibreCAD's Library Browser.

Usage: python make_library.py [OUTDIR] [--thumbs LIBRECAD.EXE]
  (OUTDIR default ./PID next to this script)
  Writes OUTDIR/<n Category>/<name>.dxf, one symbol per file, and
  OUTDIR/catalog.json (category, Thai/English names, notes) for the manual.
  --thumbs also writes <name>.png next to each file with LibreCAD's own
  "dxf2png" mode; Library Browser uses a PNG that is newer than its DXF
  instead of drawing one itself (its own thumbnails leave BYBLOCK fills out).
  Install: copy OUTDIR to Documents\\LibreCAD\\library\\PID (no admin needed;
  LibreCAD lists Documents\\LibreCAD\\library first, see RS_System).

Context: P&IDs are drawn not to scale on A3 at 1:1, so every symbol is drawn
  at its printed size in mm: valve body 6 x 3.6, instrument bubble 10,
  pump 8, actuator stem 4. Inline parts (valves, fittings) sit on the pipe
  axis = +X with the insertion point at their center; the pipe ends at
  x = +-L/2. Actuators are inserted at the same point as the valve body.
Why these shapes: they follow ISA-5.1 and the common practice shown in the
  user's reference (geminivalve.com P&ID symbols): gate = bow tie with a
  bar, ball = circle in a cut bow tie, check = "N" with an arrow, fail
  closed = arrow down the stem, fail open = arrow up. Shapes that sources
  disagree on are listed in the manual as "use the project legend".
Gotchas:
  - Layer 0, colour/lineweight BYBLOCK, linetype Continuous: an inserted
    symbol takes the pen of the layer it is placed on, but never its line
    type (a symbol stays solid on a dashed layer).
  - Letters (M, S, H, FO ...) are strokes from LibreCAD's iso.lff, not TEXT,
    so every CAD program draws them the same.
  - Block names must NOT start with "pid_": the pidlines plugin treats
    every pid_* insert lying on a line as one of its own symbols (it would
    delete a valve when re-placing a pattern on that pipe).
  - LibreCAD's Line Gap tool cuts LINE entities only: draw pipes with the
    Line tool when valves will be cut into them.
"""
import argparse
import json
import math
import subprocess
import sys
from pathlib import Path

import ezdxf

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
from make_text_patterns import CAP, LFF, layout, read_lff   # noqa: E402

ap = argparse.ArgumentParser(description="P&ID symbol library for LibreCAD")
ap.add_argument("outdir", nargs="?", default=str(HERE / "PID"))
ap.add_argument("--thumbs", metavar="LIBRECAD_EXE", help="make 128x128 PNG thumbnails with LibreCAD dxf2png")
ARGS = ap.parse_args()
OUT = Path(ARGS.outdir)
STROKES, LSPACE = read_lff(LFF)

A, B = 3.0, 1.8          # valve body half length / half height
STEM = 4.0               # actuator stem: valve center up to the actuator
R_INST = 5.0             # instrument bubble radius (10 mm)

CATS = {
    "valves": "1 Valves",
    "actuators": "2 Actuators",
    "fittings": "3 Fittings",
    "equipment": "4 Equipment",
    "instruments": "5 Instruments",
    "annotation": "6 Annotation",
}


class Sym:
    """one symbol = one DXF modelspace"""

    def __init__(self):
        self.doc = ezdxf.new("R2000")
        self.doc.header["$INSUNITS"] = 4        # mm: LibreCAD's paste scale stays 1
        self.m = self.doc.modelspace()
        self.a = {"layer": "0", "color": 0, "lineweight": -2, "linetype": "Continuous"}

    def line(self, p, q):
        self.m.add_line(p, q, dxfattribs=self.a)

    def poly(self, pts, closed=False):
        """pts: (x, y) or (x, y, bulge of the segment starting there)"""
        pts = [p if len(p) == 3 else (p[0], p[1], 0.0) for p in pts]
        self.m.add_lwpolyline(pts, format="xyb", close=closed, dxfattribs=self.a)

    def circle(self, c, r):
        self.m.add_circle(c, r, dxfattribs=self.a)

    def arc(self, c, r, a1, a2):
        """counter-clockwise from a1 to a2, degrees"""
        self.m.add_arc(c, r, a1, a2, dxfattribs=self.a)

    def fill(self, pts):
        """solid BYBLOCK fill, pts with optional bulge as in poly()"""
        pts = [p if len(p) == 3 else (p[0], p[1], 0.0) for p in pts]
        h = self.m.add_hatch(color=0, dxfattribs={"layer": "0", "lineweight": -2})
        h.paths.add_polyline_path(pts, is_closed=True)

    def disc(self, c, r):
        self.circle(c, r)
        self.fill([(c[0] + r, c[1], 1.0), (c[0] - r, c[1], 1.0)])

    def dashed(self, p, q, dash=0.9, gap=0.6):
        L = math.dist(p, q)
        ux, uy = (q[0] - p[0]) / L, (q[1] - p[1]) / L
        n = max(1, round((L + gap) / (dash + gap)))
        d = (L - (n - 1) * gap) / n             # dashes fill the line exactly, ends solid
        t = 0.0
        for _ in range(n):
            self.line((p[0] + ux * t, p[1] + uy * t), (p[0] + ux * (t + d), p[1] + uy * (t + d)))
            t += d + gap

    def text(self, s, cx, cy, h):
        """stroke letters from iso.lff, centered on (cx, cy), cap height h"""
        st, w = layout(s, STROKES, LSPACE)
        k = h / CAP
        for stroke in st:   # .lff bulge belongs to the segment arriving at a vertex
            pts = [(cx + (px - w / 2) * k, cy + (py - CAP / 2) * k, stroke[i + 1][2] if i + 1 < len(stroke) else 0.0)
                   for i, (px, py, _) in enumerate(stroke)]
            self.m.add_lwpolyline(pts, format="xyb", dxfattribs=self.a)

    def save(self, path):
        path.parent.mkdir(parents=True, exist_ok=True)
        self.doc.saveas(path)


CATALOG = []


def symbol(cat, name, th, en, note=""):
    def deco(draw):
        CATALOG.append({"cat": cat, "name": name, "th": th, "en": en, "note": note, "draw": draw})
        return draw
    return deco


# ---------------------------------------------------------------- valve parts

def bowtie(s, clip=None):
    """two triangles meeting at the center; clip(corner) -> where a slanted side stops"""
    for sx in (-1, 1):
        top, bot = (sx * A, B), (sx * A, -B)
        if clip is None:
            s.poly([top, bot, (0, 0)], closed=True)
        else:
            s.poly([clip(top), top, bot, clip(bot)])


def towards(r):
    """clip at distance r from the center along the side"""
    def f(p):
        L = math.hypot(*p)
        return (p[0] * r / L, p[1] * r / L)
    return f


def tri_down(s):        # third port, below
    s.poly([(-B, -A), (B, -A), (0, 0)], closed=True)


def tri_up(s):
    s.poly([(-B, A), (B, A), (0, 0)], closed=True)


def tri_right(s):
    s.poly([(A, -B), (A, B), (0, 0)], closed=True)


def stem(s, top=STEM, bottom=0.0):
    s.line((0, bottom), (0, top))


def dome(s, y=STEM, r=1.8):
    s.arc((0, y), r, 0, 180)
    s.line((-r, y), (r, y))


def box_letter(s, letter, half=1.6, y=STEM):
    s.poly([(-half, y), (half, y), (half, y + 2 * half), (-half, y + 2 * half)], closed=True)
    s.text(letter, 0, y + half, half * 1.15)


def arrowhead(s, tip, direction, length=0.9, half_angle=25):
    a = math.atan2(direction[1], direction[0])
    for d in (-half_angle, half_angle):
        b = a + math.radians(180 + d)
        s.line(tip, (tip[0] + length * math.cos(b), tip[1] + length * math.sin(b)))


# ---------------------------------------------------------------- 1 valves

@symbol("valves", "vlv_generic", "วาล์ว 2 ทาง (ทั่วไป)", "Valve, general (2-way)",
        "ใช้เมื่อยังไม่ระบุชนิด หรือเป็นตัววาล์วของวาล์วควบคุม")
def _(s):
    bowtie(s)


@symbol("valves", "vlv_gate", "วาล์วประตูน้ำ", "Gate valve", "บางโครงการวาดเป็นรูปโบว์เปล่า ดูตาราง legend ของโครงการ")
def _(s):
    bowtie(s)
    s.line((0, -B), (0, B))


@symbol("valves", "vlv_globe", "วาล์วลูกโลก", "Globe valve", "หรี่/ปรับอัตราการไหลได้")
def _(s):
    bowtie(s)
    s.disc((0, 0), 0.75)


@symbol("valves", "vlv_ball", "บอลวาล์ว", "Ball valve", "เปิด-ปิดเร็ว 1/4 รอบ")
def _(s):
    r = 0.95
    bowtie(s, towards(r))
    s.circle((0, 0), r)


@symbol("valves", "vlv_butterfly", "วาล์วปีกผีเสื้อ", "Butterfly valve", "ท่อขนาดใหญ่ ลดความยาววาล์ว")
def _(s):
    s.line((-A, -B), (-A, B))
    s.line((A, -B), (A, B))
    s.line((-A, -B), (A, B))
    s.disc((0, 0), 0.35)


@symbol("valves", "vlv_plug", "วาล์วปลั๊ก", "Plug valve")
def _(s):
    h = 0.8
    bowtie(s, lambda p: (math.copysign(h, p[0]), p[1] * h / A))
    s.poly([(-h, -h), (h, -h), (h, h), (-h, h)], closed=True)


@symbol("valves", "vlv_needle", "วาล์วเข็ม", "Needle valve", "ปรับละเอียด ท่อเล็ก/สายเครื่องวัด")
def _(s):
    bowtie(s)
    s.fill([(-0.6, 1.1), (0.6, 1.1), (0, 0)])


@symbol("valves", "vlv_diaphragm", "วาล์วไดอะแฟรม", "Diaphragm valve")
def _(s):
    bowtie(s)
    t = math.degrees(math.atan2(B, A))
    s.arc((0, 0), 1.5, t, 180 - t)


@symbol("valves", "vlv_pinch", "วาล์วบีบ", "Pinch valve")
def _(s):
    bowtie(s)
    t = math.degrees(math.asin(1.2 / 2.0))
    s.arc((0, 2.9), 2.0, 270 - t, 270 + t)
    s.arc((0, -2.9), 2.0, 90 - t, 90 + t)


@symbol("valves", "vlv_check", "เช็ควาล์ว (กันไหลกลับ)", "Check valve",
        "ไหลได้ทางเดียวตามลูกศร (ซ้ายไปขวา) กลับทิศให้ใช้ Mirror")
def _(s):
    a = 2.0
    s.line((-a, -B), (-a, B))
    s.line((-a, B), (a, -B))
    s.line((a, -B), (a, B))
    s.disc((-a, B), 0.3)
    arrowhead(s, (a, -B), (2 * a, -2 * B))


@symbol("valves", "vlv_angle", "วาล์วมุม", "Angle valve", "ทางเข้าด้านล่าง ทางออกด้านขวา")
def _(s):
    tri_down(s)
    tri_right(s)


@symbol("valves", "vlv_3way", "วาล์ว 3 ทาง", "Three-way valve")
def _(s):
    bowtie(s)
    tri_down(s)


@symbol("valves", "vlv_4way", "วาล์ว 4 ทาง", "Four-way valve")
def _(s):
    bowtie(s)
    tri_down(s)
    tri_up(s)


@symbol("valves", "vlv_relief", "วาล์วนิรภัย / ระบายความดัน", "Pressure relief / safety valve (PSV)",
        "ทางเข้าด้านล่าง ทางออกด้านขวา สปริงอยู่บน")
def _(s):
    tri_down(s)
    tri_right(s)
    stem(s, 1.0)
    pts, y, n = [(0, 1.0)], 1.0, 6
    for i in range(n):
        y += 0.55
        pts.append((0.8 if i % 2 == 0 else -0.8, y))
    pts.append((0, y + 0.4))
    s.poly(pts)


@symbol("valves", "vlv_regulator", "วาล์วลดความดัน (เรกูเลเตอร์)", "Pressure regulator, self-actuated",
        "ท่อรับความดันต่อด้านปลายน้ำ (ขวา)")
def _(s):
    bowtie(s)
    stem(s, 3.2)
    dome(s, 3.2, 1.6)
    s.poly([(1.6, 3.2), (4.6, 3.2), (4.6, 0.0)])


@symbol("valves", "vlv_control", "วาล์วควบคุม (หัวขับลม)", "Control valve, pneumatic diaphragm",
        "= วาล์ว 2 ทาง + หัวขับไดอะแฟรม")
def _(s):
    bowtie(s)
    stem(s)
    dome(s)


@symbol("valves", "vlv_solenoid", "โซลินอยด์วาล์ว", "Solenoid valve")
def _(s):
    bowtie(s)
    stem(s)
    box_letter(s, "S", 1.3)


@symbol("valves", "vlv_motor", "วาล์วมอเตอร์ (MOV)", "Motor-operated valve")
def _(s):
    bowtie(s)
    stem(s)
    box_letter(s, "M")


# ---------------------------------------------------------------- 2 actuators
# inserted at the valve center: the stem starts there

@symbol("actuators", "act_manual", "มือหมุน / ก้านโยก", "Manual (handwheel)")
def _(s):
    stem(s)
    s.line((-1.6, STEM), (1.6, STEM))


@symbol("actuators", "act_diaphragm", "หัวขับลม แบบไดอะแฟรม", "Pneumatic, diaphragm")
def _(s):
    stem(s)
    dome(s)


@symbol("actuators", "act_piston", "หัวขับลม แบบลูกสูบ", "Pneumatic, piston")
def _(s):
    stem(s)
    s.poly([(-1.8, STEM), (1.8, STEM), (1.8, STEM + 2.2), (-1.8, STEM + 2.2)], closed=True)
    for x in (-1.0, 1.0):
        s.line((x, STEM + 0.45), (x, STEM + 1.75))
    s.line((-1.0, STEM + 1.1), (1.0, STEM + 1.1))


@symbol("actuators", "act_motor", "มอเตอร์ไฟฟ้า", "Electric motor")
def _(s):
    stem(s)
    box_letter(s, "M")


@symbol("actuators", "act_hydraulic", "หัวขับไฮดรอลิก", "Hydraulic")
def _(s):
    stem(s)
    box_letter(s, "H")


@symbol("actuators", "act_solenoid", "โซลินอยด์", "Solenoid")
def _(s):
    stem(s)
    box_letter(s, "S", 1.3)


@symbol("actuators", "act_fail_closed", "ลูกศร: ไฟ/ลมดับแล้วปิด (FC)", "Fail closed arrow",
        "วางที่จุดเดียวกับวาล์ว ลูกศรชี้ลงหาวาล์ว")
def _(s):
    arrowhead(s, (0, 1.0), (0, -1))


@symbol("actuators", "act_fail_open", "ลูกศร: ไฟ/ลมดับแล้วเปิด (FO)", "Fail open arrow",
        "วางที่จุดเดียวกับวาล์ว ลูกศรชี้ขึ้น")
def _(s):
    arrowhead(s, (0, STEM - 1.0), (0, 1))


for _code, _th in (("FO", "เปิด"), ("FC", "ปิด"), ("FL", "ค้างตำแหน่งเดิม"), ("FI", "ไม่แน่นอน")):
    def _make(code=_code):
        def draw(s):
            s.text(code, 0, -B - 1.6, 1.8)
        return draw
    symbol("actuators", f"act_label_{_code.lower()}", f"ตัวอักษร {_code}: ไฟ/ลมดับแล้ว{_th}",
           f"Fail position letters {_code}", "วางที่จุดเดียวกับวาล์ว ตัวอักษรอยู่ใต้วาล์ว")(_make())


# ---------------------------------------------------------------- 3 fittings

@symbol("fittings", "fit_flange", "หน้าแปลน (ปลายวาล์ว)", "Flanged end", "วางชิดปลายวาล์วทั้งสองข้าง")
def _(s):
    s.line((0, -B), (0, B))


@symbol("fittings", "fit_flange_pair", "รอยต่อหน้าแปลน", "Flanged joint")
def _(s):
    for x in (-0.35, 0.35):
        s.line((x, -1.5), (x, 1.5))


@symbol("fittings", "fit_threaded", "ต่อเกลียว", "Threaded (screwed) end")
def _(s):
    s.circle((0, 0), 0.35)


@symbol("fittings", "fit_welded", "ต่อเชื่อม (butt weld)", "Welded end")
def _(s):
    h = 0.35
    s.fill([(-h, -h), (h, -h), (h, h), (-h, h)])


@symbol("fittings", "fit_socketweld", "ต่อเชื่อมแบบสวม (socket weld)", "Socket weld end")
def _(s):
    h = 0.35
    s.poly([(-h, -h), (h, -h), (h, h), (-h, h)], closed=True)


@symbol("fittings", "fit_reducer", "ข้อลด (ศูนย์ตรง)", "Concentric reducer", "ปลายใหญ่ทางซ้าย")
def _(s):
    s.poly([(-1.5, -1.2), (1.5, -0.6), (1.5, 0.6), (-1.5, 1.2)], closed=True)


@symbol("fittings", "fit_reducer_ecc", "ข้อลด (เยื้องศูนย์)", "Eccentric reducer", "ด้านล่างเรียบ ปลายใหญ่ทางซ้าย")
def _(s):
    s.poly([(-1.5, -1.2), (1.5, -1.2), (1.5, 0.0), (-1.5, 1.2)], closed=True)


@symbol("fittings", "fit_strainer_y", "ตัวกรองตัว Y (Y-strainer)", "Y-strainer", "ท่อวิ่งผ่านได้ ไม่ต้องตัดท่อ")
def _(s):
    d = (3.4, -3.4)
    p1, p2 = (-1.8, 0.0), (0.5, 0.0)
    q1, q2 = (p1[0] + d[0], p1[1] + d[1]), (p2[0] + d[0], p2[1] + d[1])
    s.poly([p1, q1, q2, p2])
    m1, m2 = ((p1[0] + p2[0]) / 2, 0.0), ((q1[0] + q2[0]) / 2, q1[1])
    s.dashed(m1, m2, 0.5, 0.35)


@symbol("fittings", "fit_flexible", "ท่ออ่อน / ข้อต่ออ่อน", "Flexible hose / joint", "ตัดท่อยาว 6 มม.")
def _(s):
    pts = [(-A + i * 2 * A / 48, 0.8 * math.sin(2 * math.pi * i / 24)) for i in range(49)]
    s.poly(pts)


@symbol("fittings", "fit_insulation", "ท่อหุ้มฉนวน", "Thermally insulated pipe", "วางบนท่อ ท่อวิ่งผ่าน")
def _(s):
    s.line((-4, 0.9), (4, 0.9))
    s.line((-4, -0.9), (4, -0.9))
    x = -4.0
    while x + 1.0 <= 4.0 + 1e-9:
        s.line((x, -0.9), (x + 1.0, 0.9))
        x += 0.8


# ---------------------------------------------------------------- 4 equipment

def pump_circle(s, r=4.0):
    s.circle((0, 0), r)
    return r


@symbol("equipment", "eq_pump", "ปั๊ม (ทั่วไป)", "Pump, general", "ทางเข้าซ้าย ทางออกขวา")
def _(s):
    r = pump_circle(s)
    s.poly([(r * math.cos(math.radians(120)), r * math.sin(math.radians(120))), (r, 0),
            (r * math.cos(math.radians(240)), r * math.sin(math.radians(240)))])


@symbol("equipment", "eq_pump_centrifugal", "ปั๊มหอยโข่ง", "Centrifugal pump")
def _(s):
    r = pump_circle(s)
    s.poly([(r * math.cos(math.radians(120)), r * math.sin(math.radians(120))), (r, 0),
            (r * math.cos(math.radians(240)), r * math.sin(math.radians(240)))])
    s.line((-r, 0), (r, 0))


@symbol("equipment", "eq_pump_pd", "ปั๊มแทนที่ (positive displacement)", "Positive displacement pump")
def _(s):
    r = pump_circle(s)
    s.poly([(r * math.cos(math.radians(120)), r * math.sin(math.radians(120))), (r, 0),
            (r * math.cos(math.radians(240)), r * math.sin(math.radians(240)))])
    h = 1.0
    s.poly([(-h - 0.6, -h), (h - 0.6, -h), (h - 0.6, h), (-h - 0.6, h)], closed=True)


@symbol("equipment", "eq_pump_gear", "ปั๊มเฟือง", "Gear pump")
def _(s):
    r = pump_circle(s)
    s.poly([(r * math.cos(math.radians(120)), r * math.sin(math.radians(120))), (r, 0),
            (r * math.cos(math.radians(240)), r * math.sin(math.radians(240)))])
    for y in (0.85, -0.85):
        s.circle((-0.6, y), 0.85)


@symbol("equipment", "eq_compressor", "คอมเพรสเซอร์", "Compressor", "ทางเข้าซ้าย ทางออกขวา")
def _(s):
    r = 5.0
    s.circle((0, 0), r)
    c, sn = r * math.cos(math.radians(30)), r * math.sin(math.radians(30))
    s.line((-c, sn), (c * 0.98, 1.0))
    s.line((-c, -sn), (c * 0.98, -1.0))


@symbol("equipment", "eq_fan", "พัดลม / โบลเวอร์", "Fan / blower")
def _(s):
    r = 4.0
    s.circle((0, 0), r)
    for k in range(3):
        a = math.radians(90 + 120 * k)
        tip = (2.9 * math.cos(a), 2.9 * math.sin(a))
        s.poly([(0, 0, 0.45), (tip[0], tip[1], 0.45)], closed=True)


@symbol("equipment", "eq_motor", "มอเตอร์", "Motor")
def _(s):
    s.circle((0, 0), 3.0)
    s.text("M", 0, 0, 2.4)


def heads(w, h, d, vertical):
    """closed outline with dished heads: body w x h, head depth d"""
    bulge = 2 * d / w if vertical else 2 * d / h
    if vertical:
        x, y = w / 2, h / 2
        return [(x, -y, 0), (x, y, bulge), (-x, y, 0), (-x, -y, bulge)]
    x, y = w / 2, h / 2
    return [(-x, -y, 0), (x, -y, bulge), (x, y, 0), (-x, y, bulge)]


@symbol("equipment", "eq_vessel_v", "ถังความดัน แนวตั้ง", "Pressure vessel, vertical")
def _(s):
    s.poly(heads(12, 20, 3, True), closed=True)


@symbol("equipment", "eq_vessel_h", "ถังความดัน แนวนอน", "Pressure vessel, horizontal")
def _(s):
    s.poly(heads(22, 12, 3, False), closed=True)


@symbol("equipment", "eq_tank_open", "ถังเปิด", "Open tank")
def _(s):
    s.poly([(-12, 9), (-12, -9), (12, -9), (12, 9)])


@symbol("equipment", "eq_tank_cone", "ถังหลังคากรวย", "Tank, cone roof")
def _(s):
    s.poly([(-12, 8), (-12, -9), (12, -9), (12, 8)], closed=True)
    s.poly([(-12, 8), (0, 12), (12, 8)])


@symbol("equipment", "eq_hx_plate", "เครื่องแลกเปลี่ยนความร้อนแบบแผ่น", "Plate heat exchanger")
def _(s):
    s.poly([(-5, -9), (5, -9), (5, 9), (-5, 9)], closed=True)
    for x in (-3.0, -1.0, 1.0, 3.0):
        s.line((x, -7.5), (x, 7.5))


@symbol("equipment", "eq_hx_shell", "เครื่องแลกเปลี่ยนความร้อนแบบเปลือก-ท่อ", "Shell & tube heat exchanger")
def _(s):
    s.poly(heads(24, 10, 2.5, False), closed=True)
    s.poly([(-9, -2.6), (9, -2.6), (9, 2.6), (-9, 2.6)], closed=True)
    pts = [(-9 + i * 1.5, 1.8 if i % 2 else -1.8) for i in range(13)]
    s.poly(pts)


@symbol("equipment", "eq_cooling_tower", "หอผึ่งเย็น (cooling tower)", "Cooling tower, mechanical draft")
def _(s):
    s.poly([(-13, -10), (13, -10), (9, 10), (-9, 10)], closed=True)
    s.line((-13, -6.5), (13, -6.5))          # basin
    s.line((-6, 10), (-6, 12))                # fan stack
    s.line((6, 10), (6, 12))
    s.line((-6, 12), (6, 12))
    s.arc((0, 12), 4.5, 15, 165)              # fan blades sweep
    s.line((0, 12), (0, 15.5))


@symbol("equipment", "eq_filter", "ตัวกรอง (filter)", "Filter")
def _(s):
    s.poly([(-4, -6), (4, -6), (4, 6), (-4, 6)], closed=True)
    s.dashed((-4, -6), (4, 6), 0.9, 0.6)


@symbol("equipment", "eq_box", "เครื่องจักร / ผู้ใช้ (กรอบ)", "Machine / user (box)", "เขียนชื่อและ tag ในกรอบเอง")
def _(s):
    s.poly([(-15, -8), (15, -8), (15, 8), (-15, 8)], closed=True)


# ---------------------------------------------------------------- 5 instruments

INST_TYPES = [
    ("disc", "อุปกรณ์เดี่ยว", "Discrete instrument"),
    ("shared", "จอแสดงผลร่วม (DCS/HMI)", "Shared display / control"),
    ("comp", "ฟังก์ชันคอมพิวเตอร์", "Computer function"),
    ("plc", "PLC", "Programmable logic controller"),
]
INST_LOCS = [
    ("field", "ภาคสนาม", "Field mounted", None),
    ("cr", "ห้องควบคุม เข้าถึงได้", "Main control room, accessible", "single"),
    ("crh", "ห้องควบคุม หลังแผง/เข้าถึงยาก", "Main control room, not accessible", "single_dashed"),
    ("lp", "แผงย่อยภาคสนาม เข้าถึงได้", "Local / auxiliary panel, accessible", "double"),
    ("lph", "แผงย่อย หลังแผง/เข้าถึงยาก", "Local / auxiliary panel, not accessible", "double_dashed"),
]


def inst_outline(s, kind):
    r = R_INST
    if kind == "disc":
        s.circle((0, 0), r)
        return r
    if kind == "shared":
        s.poly([(-r, -r), (r, -r), (r, r), (-r, r)], closed=True)
        s.circle((0, 0), r)
        return r
    if kind == "plc":
        s.poly([(-r, -r), (r, -r), (r, r), (-r, r)], closed=True)
        s.poly([(0, -r), (r, 0), (0, r), (-r, 0)], closed=True)
        return r
    # hexagon, flat top and bottom, 10 mm across the corners
    s.poly([(r * math.cos(math.radians(a)), r * math.sin(math.radians(a))) for a in range(0, 360, 60)], closed=True)
    return r


def half_chord(kind, y):
    """half length of the location bar at height y: edge to edge of the outer shape"""
    r = R_INST
    if kind == "disc":
        return math.sqrt(r * r - y * y)
    if kind in ("shared", "plc"):
        return r                                  # across the square
    return r - abs(y) / math.tan(math.radians(60))


for _k, _kth, _ken in INST_TYPES:
    for _l, _lth, _len, _bar in INST_LOCS:
        def _make(kind=_k, bar=_bar):
            def draw(s):
                inst_outline(s, kind)
                if bar is None:
                    return
                ys = (0.0,) if bar.startswith("single") else (0.6, -0.6)
                for y in ys:
                    w = half_chord(kind, y)
                    if bar.endswith("dashed"):
                        s.dashed((-w, y), (w, y), 0.8, 0.55)
                    else:
                        s.line((-w, y), (w, y))
            return draw
        symbol("instruments", f"ins_{_k}_{_l}", f"{_kth} · {_lth}", f"{_ken}, {_len.lower()}")(_make())


@symbol("instruments", "ins_interlock", "ลอจิก / อินเตอร์ล็อก", "Interlock / logic", "เขียน I หรือเลขอินเตอร์ล็อกข้างใน")
def _(s):
    r = R_INST
    s.poly([(0, -r), (r, 0), (0, r), (-r, 0)], closed=True)


# ---------------------------------------------------------------- 6 annotation

@symbol("annotation", "ann_flow", "ลูกศรทิศการไหล", "Flow arrow", "ชี้ไปทาง +X หมุนตามท่อ")
def _(s):
    s.fill([(1.25, 0), (-1.25, 0.9), (-1.25, -0.9)])


@symbol("annotation", "ann_offpage", "ป้ายต่อแผ่นอื่น", "Off-page connector",
        "เขียนเลขแบบปลายทางข้างใน · ชี้ไปทางที่ท่อไป")
def _(s):
    s.poly([(-10, -3), (6, -3), (10, 0), (6, 3), (-10, 3)], closed=True)


@symbol("annotation", "ann_tie_in", "จุดต่อเข้าระบบเดิม (tie-in)", "Tie-in point", "เขียนเลข tie-in ข้างใน")
def _(s):
    s.poly([(0, -3.5), (3.5, 0), (0, 3.5), (-3.5, 0)], closed=True)


def main():
    out = []
    for c in CATALOG:
        s = Sym()
        c["draw"](s)
        path = OUT / CATS[c["cat"]] / f"{c['name']}.dxf"
        s.save(path)
        out.append({k: v for k, v in c.items() if k != "draw"} | {"file": str(path.relative_to(OUT)).replace("\\", "/")})
    (OUT / "catalog.json").write_text(json.dumps(out, ensure_ascii=False, indent=1), encoding="utf-8")
    counts = {}
    for c in out:
        counts[c["cat"]] = counts.get(c["cat"], 0) + 1
    print(f"{len(out)} symbols -> {OUT}  " + "  ".join(f"{k} {v}" for k, v in counts.items()))
    if ARGS.thumbs:
        bad = 0
        for c in out:
            dxf = OUT / c["file"]
            png = dxf.with_suffix(".png")
            r = subprocess.run([ARGS.thumbs, "dxf2png", "-r", "128x128", "-o", str(png), str(dxf)],
                               capture_output=True, timeout=120)
            if r.returncode != 0 or not png.exists() or png.stat().st_mtime < dxf.stat().st_mtime:
                bad += 1
                print("thumbnail failed:", dxf)
        print(f"thumbnails: {len(out) - bad} of {len(out)}")


if __name__ == "__main__":
    main()
