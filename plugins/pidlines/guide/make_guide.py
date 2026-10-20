"""make_guide.py - Thai guide "คู่มือเขียน P&ID ด้วย LibreCAD" (A4, about 15 pages).

Usage: python make_guide.py LIBDIR PATTERNDIR EXAMPLE.dxf [OUTDIR] [--pdf FILE]
  LIBDIR     = PID symbol library (make_library.py output, with catalog.json)
  PATTERNDIR = pidlines patterns (pid_*.dxf + pidlines.ini)
  EXAMPLE    = example sheet (make_example.py output)
  OUTDIR     = where guide.html goes (default ./out, git-ignored)
  --pdf FILE = also print it with Edge headless and check the page count

Context: guide_body.html holds the text, one <section class="page"> per
  sheet, with {{...}} placeholders this script fills: symbol tables from the
  library DXFs, signal line samples drawn like the plugin draws them, the
  instrument matrix, the layer table from make_template.LAYERS, figures drawn
  with pidkit, and the example sheet. Every picture comes from the files
  LibreCAD uses, so changing a symbol and re-running updates the guide.
Gotchas:
  - Pages are fixed-size boxes (no automatic flow): if a page's text grows,
    it is clipped at the bottom. The script prints each page's overflow
    after Edge has laid it out (via a small script in the page that writes
    into the title); --pdf fails when the page count is off.
  - Fonts come from Google Fonts; offline, Edge falls back and pages may
    overflow.
"""
import argparse
import configparser
import shutil
import json
import re
import subprocess
import sys
from html import escape, unescape
from pathlib import Path

import ezdxf

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "library"))
from dxfsvg import Picture            # noqa: E402
from make_template import LAYERS      # noqa: E402
from pidkit import Drawing            # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument("libdir")
ap.add_argument("patdir")
ap.add_argument("example")
ap.add_argument("outdir", nargs="?", default=str(HERE / "out"))
ap.add_argument("--pdf")
args = ap.parse_args()
LIB, PAT, EX, OUT = Path(args.libdir), Path(args.patdir), Path(args.example), Path(args.outdir)
OUT.mkdir(parents=True, exist_ok=True)

CATALOG = json.loads((LIB / "catalog.json").read_text(encoding="utf-8"))
INI = configparser.ConfigParser()
INI.read(PAT / "pidlines.ini", encoding="utf-8")
FONTS = {"iso": "'IBM Plex Sans Condensed',sans-serif", "standard": "'IBM Plex Sans Condensed',sans-serif",
         "thsarabunnew": "Sarabun,sans-serif"}
WIDTHS = {name: lw / 100.0 for name, _, lw, _, _ in LAYERS}
ACI = {1: "#d62728", 2: "#c9b400", 3: "#2ca02c", 4: "#17becf", 5: "#1f4fd6", 6: "#c51bb5", 7: "#111111", 8: "#808080"}


# ---------------------------------------------------------------- pictures

def file_picture(path):
    doc = ezdxf.readfile(path)
    return Picture(doc).add_all(doc.modelspace())


def symbol_svg(path, k, maxw, maxh, pen=0.28):
    pic = file_picture(path)
    x0, y0, x1, y1 = pic.bbox()
    w, h = max(x1 - x0, 1e-6), max(y1 - y0, 1e-6)
    scale = min(k, maxw / w, maxh / h)
    return pic.svg(scale=scale, pad=0.6, default_width=pen, fonts=FONTS, css_class="sym-svg")


GRAY = "#9aa4b0"


def drawing_svg(D, scale, box=None, pad=1.0, pen_scale=None, css="fig-svg", colors=None):
    pic = Picture(D.doc).add_all(D.msp)
    return pic.svg(scale=scale, box=box, pad=pad, widths=WIDTHS, pen_scale=pen_scale, fonts=FONTS, css_class=css,
                   colors={"CTX": GRAY, **(colors or {})})


def new_drawing():
    return Drawing(LIB, PAT)


# ---------------------------------------------------------------- tables

def ctx_valve(D):
    D.insert("vlv_generic", (0, 0), "CTX")


def ctx_valve_act(D):
    D.insert("vlv_generic", (0, 0), "CTX")
    D.insert("act_diaphragm", (0, 0), "CTX")


def ctx_end(gap):
    def draw(D):                      # valve on the left ending at the joint, pipe going on to the right
        D.insert("vlv_generic", (-3.0 - gap, 0), "CTX")
        D.line((gap, 0), (6.0, 0), "CTX")
    return draw


# symbols that only make sense next to what they attach to: that part is drawn gray
CONTEXT = {
    "act_manual": ctx_valve, "act_diaphragm": ctx_valve, "act_piston": ctx_valve, "act_motor": ctx_valve,
    "act_hydraulic": ctx_valve, "act_solenoid": ctx_valve,
    "act_fail_closed": ctx_valve_act, "act_fail_open": ctx_valve_act,
    "act_label_fo": ctx_valve, "act_label_fc": ctx_valve, "act_label_fl": ctx_valve, "act_label_fi": ctx_valve,
    "fit_flange": ctx_end(0.6), "fit_threaded": ctx_end(0.35), "fit_welded": ctx_end(0.35),
    "fit_socketweld": ctx_end(0.35),
}


def context_svg(name, k, maxw, maxh, pen=0.28):
    D = new_drawing()
    CONTEXT[name](D)
    D.insert(name, (0, 0), "SYM")
    pic = Picture(D.doc).add_all(D.msp)
    x0, y0, x1, y1 = pic.bbox()
    scale = min(k, maxw / max(x1 - x0, 1e-6), maxh / max(y1 - y0, 1e-6))
    return pic.svg(scale=scale, pad=0.6, default_width=pen, fonts=FONTS, css_class="sym-svg", colors={"CTX": GRAY})


def symtable(cat, cols=3, k=2.2, maxw=34, maxh=22, names=None):
    items = [c for c in CATALOG if c["cat"] == cat and (names is None or c["name"] in names)]
    cells = []
    for c in items:
        if c["name"] in CONTEXT:
            svg = context_svg(c["name"], k, maxw, maxh)
        else:
            svg = symbol_svg(LIB / c["file"], k, maxw, maxh)
        note = f'<span class="note">{escape(c["note"])}</span>' if c["note"] else ""
        cells.append(f'<figure class="sym"><div class="pic">{svg}</div><figcaption><b>{escape(c["th"])}</b>'
                     f'<span class="en">{escape(c["en"])}</span>{note}<code>{c["name"]}</code></figcaption></figure>')
    return f'<div class="symgrid c{cols}">' + "".join(cells) + "</div>"


SIGNALS = [
    ("pipe-main", "ท่อ process หลัก", "Piping, major process", "layer PID-PIPE-MAIN (0.7)", "—"),
    ("pipe-minor", "ท่อรอง / utility / ต่อเครื่องวัด", "Minor process, connection to process", "layer PID-PIPE-MINOR (0.35)", "—"),
    ("undefined", None, "Undefined signal", "ปลั๊กอิน: ลาย undefined", None),
    ("pneumatic", None, "Pneumatic signal", "ปลั๊กอิน: ลาย pneumatic", None),
    ("electric", "สัญญาณไฟฟ้า", "Electric signal", "layer PID-SIGNAL-ELEC (เส้นประในตัว)", "—"),
    ("hydraulic", None, "Hydraulic signal", "ปลั๊กอิน: ลาย hydraulic", None),
    ("capillary", None, "Capillary tube", "ปลั๊กอิน: ลาย capillary", None),
    ("em_guided", None, "Electromagnetic / sonic, guided", "ปลั๊กอิน: ลาย em_guided", None),
    ("em_unguided", None, "Electromagnetic / sonic, not guided", "ปลั๊กอิน: ลาย em_unguided", None),
    ("datalink", None, "Data link, in one system", "ปลั๊กอิน: ลาย datalink", None),
    ("datalink_ext", None, "Data link, between systems", "ปลั๊กอิน: ลาย datalink_ext", None),
    ("mechanical", None, "Mechanical link", "ปลั๊กอิน: ลาย mechanical", None),
    ("cws", "ป้ายบอกน้ำบนท่อ", "Service marker on a pipe", "ปลั๊กอิน: ลาย CWS CWR HWS HWR", None),
]


def ini_label(name):
    lab = INI.get(name, "label", fallback=name).strip('"')
    return lab.split("  ")[0]


def signal_sample(name, L=36.0):
    D = new_drawing()
    if name == "pipe-main":
        D.run([(0, 0), (L, 0)], "PID-PIPE-MAIN")
    elif name == "pipe-minor":
        D.run([(0, 0), (L, 0)], "PID-PIPE-MINOR")
    elif name == "electric":
        D.electric([(0, 0), (L, 0)])
    else:
        gap = INI.getfloat(name, "gap", fallback=0.0)
        spacing = INI.getfloat(name, "spacing", fallback=12.0)
        keep = INI.get(name, "line", fallback="1") != "0"
        gaps = D.pattern([(0, 0), (L, 0)], f"pid_{name}", spacing, gap if keep else 0.0,
                         layer="PID-PIPE-MAIN" if name == "cws" else "PID-SIGNAL")
        if keep:
            D.run([(0, 0), (L, 0)], "PID-PIPE-MAIN" if name == "cws" else "PID-SIGNAL", gaps)
    return drawing_svg(D, 1.25, box=(0, -2.5, L, 2.5), pad=0.3, pen_scale=1.0, css="line-svg")


def signal_table():
    rows = []
    for name, th, en, how, cut in SIGNALS:
        th = th or ini_label(name)
        if cut is None:
            if INI.get(name, "line", fallback="1") == "0":
                cut = "ไม่มีเส้น"
            else:
                g = INI.getfloat(name, "gap", fallback=0.0)
                cut = "ไม่ตัด" if g == 0 else f"ตัด {g:g}"
        rows.append(f'<tr><td class="smp">{signal_sample(name)}</td><td><b>{escape(th)}</b>'
                    f'<span class="en">{escape(en)}</span></td><td>{escape(how)}</td><td class="num">{cut}</td></tr>')
    return ('<table class="lines"><thead><tr><th>ตัวอย่าง</th><th>ชนิด</th><th>วาดใน LibreCAD</th>'
            '<th class="num">ตัดเส้น มม.</th></tr></thead><tbody>' + "".join(rows) + "</tbody></table>")


def instrument_matrix():
    types = [("disc", "อุปกรณ์เดี่ยว", "วงกลม"), ("shared", "จอแสดงผลร่วม DCS / HMI", "วงกลมในสี่เหลี่ยม"),
             ("comp", "ฟังก์ชันคอมพิวเตอร์", "หกเหลี่ยม"), ("plc", "PLC", "ข้าวหลามตัดในสี่เหลี่ยม")]
    locs = [("field", "ภาคสนาม", "ไม่มีเส้น"), ("cr", "ห้องควบคุม", "เส้นเดี่ยว"),
            ("crh", "ห้องควบคุม หลังแผง", "เส้นประเดี่ยว"), ("lp", "แผงย่อยภาคสนาม", "เส้นคู่"),
            ("lph", "แผงย่อย หลังแผง", "เส้นประคู่")]
    head = "".join(f"<th><b>{escape(a)}</b><span>{escape(b)}</span></th>" for _, a, b in locs)
    body = []
    for k, th, shape in types:
        cells = "".join(f'<td>{symbol_svg(LIB / "5 Instruments" / f"ins_{k}_{l}.dxf", 1.45, 16, 16)}'
                        f'<code>ins_{k}_{l}</code></td>' for l, _, _ in locs)
        body.append(f"<tr><th><b>{escape(th)}</b><span>{escape(shape)}</span></th>{cells}</tr>")
    return ('<table class="imatrix"><thead><tr><th></th>' + head + "</tr></thead><tbody>" + "".join(body)
            + "</tbody></table>")


def layer_table():
    rows = []
    for name, color, lw, lt, purpose in LAYERS:
        sw = ACI.get(color, "#111")
        dash = ' stroke-dasharray="2 1"' if lt != "CONTINUOUS" else ""
        line = (f'<svg class="lw" viewBox="0 0 22 3" width="22mm" height="3mm"><path d="M1 1.5H21" stroke="#111" '
                f'stroke-width="{lw / 100:.2f}"{dash}/></svg>')
        rows.append(f'<tr><td class="mono">{name}</td><td><span class="sw" style="background:{sw}"></span>{color}</td>'
                    f'<td class="num">{lw / 100:.2f}</td><td>{line}<span class="mono small">{lt.lower()}</span></td>'
                    f'<td>{escape(purpose)}</td></tr>')
    return ('<table class="layers"><thead><tr><th>layer</th><th>สี (ACI)</th><th class="num">หนา มม.</th>'
            '<th>ชนิดเส้น</th><th>ใช้กับ</th></tr></thead><tbody>' + "".join(rows) + "</tbody></table>")


LETTERS = [  # letter, measured / initiating variable, variable modifier, readout / passive, output / active
    ("A", "การวิเคราะห์ (pH, O₂, ความขุ่น ...)", "", "สัญญาณเตือน (alarm)", ""),
    ("B", "หัวเผา / การเผาไหม้", "", "", ""),
    ("C", "เลือกเอง (มักใช้ conductivity)", "", "", "ควบคุม (control)"),
    ("D", "เลือกเอง (มักใช้ density)", "ผลต่าง (differential)", "", ""),
    ("E", "แรงดันไฟฟ้า (voltage)", "", "ตัวตรวจจับ (primary element)", ""),
    ("F", "อัตราการไหล (flow)", "อัตราส่วน (ratio)", "", ""),
    ("G", "เลือกเอง", "", "หน้าปัด / กระจกมอง (gauge, glass)", ""),
    ("H", "มือ (hand)", "", "", ""),
    ("I", "กระแสไฟฟ้า (current)", "", "แสดงค่า (indicate)", ""),
    ("J", "กำลังไฟฟ้า (power)", "", "", ""),
    ("K", "เวลา", "อัตราการเปลี่ยนแปลง", "", ""),
    ("L", "ระดับ (level)", "", "หลอดไฟแสดง (light)", ""),
    ("M", "เลือกเอง (มักใช้ moisture)", "", "", ""),
    ("O", "เลือกเอง", "", "รูจำกัดการไหล (orifice)", ""),
    ("P", "ความดัน / สุญญากาศ", "", "จุดต่อทดสอบ (test point)", ""),
    ("Q", "ปริมาณ (quantity)", "รวมยอด (totalize)", "", ""),
    ("R", "รังสี (radiation)", "", "บันทึก (record)", ""),
    ("S", "ความเร็ว / ความถี่", "ความปลอดภัย (safety)", "", "สวิตช์ (switch)"),
    ("T", "อุณหภูมิ (temperature)", "", "", "ส่งสัญญาณ (transmit)"),
    ("U", "หลายตัวแปร (multivariable)", "", "หลายหน้าที่", "หลายหน้าที่"),
    ("V", "การสั่นสะเทือน", "", "", "วาล์ว / แดมเปอร์"),
    ("W", "น้ำหนัก / แรง", "", "ปลอกวัด (well)", ""),
    ("X", "ไม่จัดหมวด (unclassified)", "แกน X", "ไม่จัดหมวด", "ไม่จัดหมวด"),
    ("Y", "เหตุการณ์ / สถานะ", "แกน Y", "", "อุปกรณ์เสริม (relay, คำนวณ, แปลงสัญญาณ)"),
    ("Z", "ตำแหน่ง / ระยะ", "แกน Z", "", "ตัวขับ (actuator)"),
]


def letters_table():
    rows = "".join(f'<tr><td class="ltr">{l}</td><td>{escape(a)}</td><td>{escape(b)}</td><td>{escape(c)}</td>'
                   f'<td>{escape(d)}</td></tr>' for l, a, b, c, d in LETTERS)
    return ('<table class="letters"><thead><tr><th rowspan="2"></th><th colspan="2">อักษรตัวแรก</th>'
            '<th colspan="2">อักษรตัวถัดไป</th></tr><tr><th>ตัวแปรที่วัด</th><th>ตัวขยาย</th><th>อ่านค่า / passive</th>'
            '<th>สั่งงาน / output</th></tr></thead><tbody>' + rows + "</tbody></table>")


# ---------------------------------------------------------------- figures

def fig_crossing():
    out = []
    # a: not connected, the vertical (drawn later) line gets the gap
    D = new_drawing()
    D.run([(0, 0), (24, 0)], "PID-PIPE-MAIN")
    D.run([(12, -10), (12, 10)], "PID-PIPE-MAIN", [((12, 0), 2.0)])
    out.append(("ไม่ต่อกัน", "เส้นที่วาดทีหลังเว้นช่อง 2 มม.", drawing_svg(D, 1.6, box=(-1, -11, 25, 11))))
    D = new_drawing()
    D.run([(0, 0), (24, 0)], "PID-PIPE-MAIN")
    D.run([(12, 0), (12, -10)], "PID-PIPE-MAIN")
    out.append(("ต่อกัน", "ทำเป็นตัว T", drawing_svg(D, 1.6, box=(-1, -11, 25, 11))))
    D = new_drawing()
    D.run([(0, 0), (24, 0)], "PID-PIPE-MAIN")
    D.run([(9, 0), (9, -10)], "PID-PIPE-MAIN")
    D.run([(15, 0), (15, 10)], "PID-PIPE-MAIN")
    out.append(("เลี่ยงสี่แยก", "เยื้องเป็น T สองตัว", drawing_svg(D, 1.6, box=(-1, -11, 25, 11))))
    D = new_drawing()
    D.run([(0, 0), (24, 0)], "PID-PIPE-MAIN")
    D.electric([(12, -10), (12, 10)], crossings=[(12, 0)])
    out.append(("สัญญาณข้ามท่อ", "เว้นช่องที่เส้นสัญญาณ", drawing_svg(D, 1.6, box=(-1, -11, 25, 11))))
    return '<div class="figrow">' + "".join(
        f'<figure><div class="pic">{s}</div><figcaption><b>{a}</b><span>{b}</span></figcaption></figure>'
        for a, b, s in out) + "</div>"


def fig_inline_gap():
    steps = []
    D = new_drawing()
    D.run([(0, 0), (30, 0)], "PID-PIPE-MAIN")
    D.msp.add_circle((15, 0), 0.6, dxfattribs={"layer": "PID-TEXT"})
    steps.append(("1. ท่อเป็น LINE", "จุดที่จะวางวาล์ว", drawing_svg(D, 1.5, box=(-1, -6, 31, 8))))
    D = new_drawing()
    D.run([(0, 0), (30, 0)], "PID-PIPE-MAIN", [((15, 0), 6.0)])
    D.line((12, 2.2), (18, 2.2), "PID-TEXT")
    D.line((12, 1.4), (12, 3.0), "PID-TEXT")
    D.line((18, 1.4), (18, 3.0), "PID-TEXT")
    D.text("6", (15, 4.4), 2.2)
    steps.append(("2. Line Gap 6 มม.", "gap snap = middle", drawing_svg(D, 1.5, box=(-1, -6, 31, 8))))
    D = new_drawing()
    D.run([(0, 0), (30, 0)], "PID-PIPE-MAIN", [((15, 0), 6.0)])
    D.insert("vlv_gate", (15, 0), "PID-VALVE")
    steps.append(("3. แทรกวาล์วที่จุดเดิม", "ปลายท่อชนปลายวาล์วพอดี", drawing_svg(D, 1.5, box=(-1, -6, 31, 8))))
    D = new_drawing()
    D.run([(0, 0), (30, 0)], "PID-PIPE-MAIN", [((15, 0), 6.0)])
    D.insert("vlv_generic", (15, 0), "PID-VALVE")
    D.insert("act_diaphragm", (15, 0), "PID-VALVE")
    D.insert("act_fail_closed", (15, 0), "PID-VALVE")
    D.insert("act_label_fc", (15, 0), "PID-VALVE")
    steps.append(("4. actuator จุดเดียวกัน", "+ ลูกศร/ตัวอักษร FC", drawing_svg(D, 1.5, box=(-1, -6, 31, 8))))
    return '<div class="figrow">' + "".join(
        f'<figure><div class="pic">{s}</div><figcaption><b>{a}</b><span>{b}</span></figcaption></figure>'
        for a, b, s in steps) + "</div>"


def fig_bubble():
    D = new_drawing()
    D.bubble("ins_disc_field", (0, 0), "TT", "105")
    D.bubble("ins_plc_cr", (26, 0), "TIC", "105")
    # dimension: diameter and text positions (thin text-layer lines)
    D.line((-5, -8), (5, -8), "PID-FRAME-THIN")
    for x in (-5, 5):
        D.line((x, -7), (x, -9), "PID-FRAME-THIN")
    D.text("10", (0, -10.3), 2.0)
    D.line((7, 2.0), (12, 2.0), "PID-FRAME-THIN")
    D.line((7, -2.0), (12, -2.0), "PID-FRAME-THIN")
    D.text("+2", (13, 2.0), 2.0, "MIDDLE_LEFT")
    D.text("-2", (13, -2.0), 2.0, "MIDDLE_LEFT")
    return drawing_svg(D, 1.8, box=(-7, -12, 33, 7))


def fig_loop():
    D = new_drawing()
    y = 0.0
    tv = (40.0, y)
    D.run([(0, y), (80, y)], "PID-PIPE-MAIN", [(tv, 6.0)])
    D.insert("ann_flow", (8, y), "PID-PIPE-MAIN")
    D.insert("vlv_control", tv, "PID-VALVE")
    D.insert("act_fail_closed", tv, "PID-VALVE")
    D.text("TV-101", (tv[0] + 8.5, y + 4.5), 2.5, "MIDDLE_LEFT")
    tt = D.bubble("ins_disc_field", (66.0, 14.0), "TT", "101")
    D.conn([(66.0, y), D.edge(tt, (66.0, y))])
    tic = D.bubble("ins_plc_cr", (66.0, 38.0), "TIC", "101")
    D.electric([(66.0, tt[1] + 5), (66.0, tic[1] - 5)])
    ty = D.bubble("ins_disc_field", (tv[0], 24.0), "TY", "101")
    D.electric([(tic[0] - 5, tic[1]), (tv[0], tic[1]), (tv[0], ty[1] + 5)])
    D.pneumatic([(tv[0], ty[1] - 5), (tv[0], y + 5.8)])
    D.text("I/P", (tv[0] - 7, 24.0), 2.2, "MIDDLE_RIGHT")
    D.text("4-20 mA", (69, 26), 2.0, "MIDDLE_LEFT")
    D.text("4-20 mA", (53, 40.2), 2.0, "BOTTOM_CENTER")
    D.text("20-100 kPa", (tv[0] + 2.5, 12.5), 2.0, "MIDDLE_LEFT")
    return drawing_svg(D, 1.45, box=(-2, -9, 84, 45))


def fig_offpage():
    D = new_drawing()
    D.run([(0, 0), (22, 0)], "PID-PIPE-MAIN")
    D.insert("ann_offpage", (32, 0), "PID-PIPE-MAIN")
    D.text("PID-002", (30, 0), 2.5)
    D.text("100-CWS-102", (11, 1.6), 2.2, "BOTTOM_CENTER")
    D.text("TO E-201 / ZONE C4", (32, -5.5), 2.2)
    return drawing_svg(D, 1.35, box=(-1, -8, 44, 5))


def example_sheet(k=0.585):
    pic = file_picture(EX)
    return pic.svg(scale=k, box=(0, 0, 420, 297), pad=0, widths=WIDTHS, pen_scale=k, fonts=FONTS, css_class="sheet-svg")


def fig_symbol(name, k=3.0):
    hit = next(c for c in CATALOG if c["name"] == name)
    return symbol_svg(LIB / hit["file"], k, 60, 40)


def figrow(items, cls="figrow"):
    return f'<div class="{cls}">' + "".join(
        f'<figure><div class="pic">{s}</div><figcaption><b>{a}</b><span>{b}</span></figcaption></figure>'
        for a, b, s in items) + "</div>"


def fig_pfd_vs_pid():
    def base(D, detailed):
        D.insert("eq_tank_open", (12.0, 22.0), "PID-EQUIP", 0, 0.7)
        D.text("T-1", (12.0, 12.5), 2.5)
        D.insert("eq_pump", (40.0, 10.0), "PID-EQUIP")
        D.text("P-1", (40.0, 3.5), 2.5)
        D.insert("eq_hx_shell", (72.0, 22.0), "PID-EQUIP", 0, 0.6)
        D.text("E-1", (72.0, 15.0), 2.5)
        suct = [(20.4, 17.0), (26.0, 17.0), (26.0, 10.0), (36.0, 10.0)]
        disc = [(44.0, 10.0), (60.0, 10.0), (60.0, 22.0), (64.8, 22.0)]
        out = [(79.2, 22.0), (88.0, 22.0)]
        if detailed:
            D.run(suct, "PID-PIPE-MAIN", [((31.0, 10.0), 6.0)])
            D.insert("vlv_gate", (31.0, 10.0), "PID-VALVE")
            D.run(disc, "PID-PIPE-MAIN", [((49.0, 10.0), 4.0), ((55.0, 10.0), 6.0)])
            D.insert("vlv_check", (49.0, 10.0), "PID-VALVE")
            D.insert("vlv_gate", (55.0, 10.0), "PID-VALVE")
            pi = D.bubble("ins_disc_field", (45.5, 21.0), "PI", "1")
            D.conn([(45.5, 10.0), D.edge(pi, (45.5, 10.0))])
            D.run(out, "PID-PIPE-MAIN")
            tt = D.bubble("ins_disc_field", (84.0, 32.0), "TT", "2")
            D.conn([(84.0, 22.0), D.edge(tt, (84.0, 22.0))])
            D.text("50-W-01", (24.0, 5.0), 2.0)
        else:
            D.run(suct, "PID-PIPE-MAIN")
            D.run(disc, "PID-PIPE-MAIN")
            D.run(out, "PID-PIPE-MAIN")
            D.text("35°C · 12 m³/h", (52.0, 11.5), 2.0, "BOTTOM_CENTER")
        D.insert("ann_flow", (86.0, 22.0), "PID-PIPE-MAIN")
    a, b = new_drawing(), new_drawing()
    base(a, False)
    base(b, True)
    box = (2.0, 1.0, 90.0, 38.0)
    return figrow([("PFD", "อุปกรณ์หลัก ทางไหล สภาวะ", drawing_svg(a, 0.95, box=box, pad=0.5)),
                   ("P&amp;ID", "เพิ่มวาล์ว เครื่องวัด เลขท่อ และ tag", drawing_svg(b, 0.95, box=box, pad=0.5))])


def fig_compose():
    D = new_drawing()
    D.insert("vlv_ball", (0, 0), "PID-VALVE")
    D.text("+", (9.0, 1.0), 4.0)
    D.insert("act_motor", (18.0, -1.0), "PID-VALVE")
    D.text("=", (27.0, 1.0), 4.0)
    D.insert("vlv_ball", (38.0, 0), "PID-VALVE")
    D.insert("act_motor", (38.0, 0), "PID-VALVE")
    y = -17.0
    D.insert("vlv_generic", (0, y), "PID-VALVE")
    D.text("+", (9.0, y + 1.0), 4.0)
    D.insert("act_diaphragm", (18.0, y - 1.0), "PID-VALVE")
    D.text("+", (27.0, y + 1.0), 4.0)
    D.line((36.0, y), (36.0, y + 4.0), "CTX")
    D.insert("act_fail_closed", (36.0, y), "PID-VALVE")
    D.insert("act_label_fc", (36.0, y), "PID-VALVE")
    D.text("=", (45.0, y + 1.0), 4.0)
    for name in ("vlv_generic", "act_diaphragm", "act_fail_closed", "act_label_fc"):
        D.insert(name, (56.0, y), "PID-VALVE")
    return drawing_svg(D, 1.15, box=(-4, -22, 60, 9))


def fig_pump():
    D = new_drawing()
    y = 0.0
    D.run([(0, y), (24.0, y)], "PID-PIPE-MAIN", [((6.0, y), 6.0), ((20.5, y), 3.0)])
    D.insert("vlv_gate", (6.0, y), "PID-VALVE")
    D.insert("fit_strainer_y", (12.5, y), "PID-VALVE")
    D.insert("fit_reducer_ecc", (20.5, y), "PID-VALVE", 0, 1.0, -1.0)     # flat side up on the suction
    D.insert("eq_pump_centrifugal", (28.0, y), "PID-EQUIP")
    D.text("P-101", (28.0, y - 9.5), 2.5)
    D.run([(32.0, y), (62.0, y)], "PID-PIPE-MAIN", [((35.0, y), 3.0), ((41.0, y), 4.0), ((50.0, y), 6.0)])
    D.insert("fit_reducer", (35.0, y), "PID-VALVE", 180)
    D.insert("vlv_check", (41.0, y), "PID-VALVE")
    D.insert("vlv_gate", (50.0, y), "PID-VALVE")
    pi = D.bubble("ins_disc_field", (38.0, 12.5), "PI", "101")
    D.conn([(38.0, y), D.edge(pi, (38.0, y))])
    D.run([(45.5, y), (45.5, -9.0)], "PID-PIPE-MINOR", [((45.5, -5.0), 4.2)])
    D.insert("vlv_ball", (45.5, -5.0), "PID-VALVE", 90, 0.7)
    D.text("DR", (45.5, -11.0), 2.2)
    D.insert("ann_flow", (2.0, y), "PID-PIPE-MAIN")
    D.insert("ann_flow", (59.0, y), "PID-PIPE-MAIN")
    for x, lab, ly in ((6.0, "1", 6.5), (12.5, "2", 6.5), (20.5, "3", 6.5), (35.0, "4", -5.0), (41.0, "5", -5.0),
                       (50.0, "6", 6.5)):
        D.text(lab, (x, ly), 2.2)
    return drawing_svg(D, 1.5, box=(-1, -13, 63, 19))


def fig_drains():
    out = []
    for name, cap, valve, up in (("vent|VT", "จุดสูงสุดของท่อ", "vlv_ball", True),
                                 ("drain|DR", "จุดต่ำสุดของท่อ", "vlv_ball", False),
                                 ("sample point|SP", "จุดเก็บตัวอย่าง", "vlv_needle", False)):
        D = new_drawing()
        s = 1 if up else -1
        D.run([(0, 0), (24, 0)], "PID-PIPE-MAIN")
        D.run([(12, 0), (12, 9 * s)], "PID-PIPE-MINOR", [((12, 5 * s), 4.2)])
        D.insert(valve, (12, 5 * s), "PID-VALVE", 90, 0.7)
        D.text(name.split("|")[1], (12, 11 * s), 2.2)
        box = (-1, -4, 25, 13) if up else (-1, -13, 25, 4)
        out.append((name.split("|")[0], cap, drawing_svg(D, 1.4, box=box)))
    return figrow(out)


def fig_inst_conn():
    out = []
    D = new_drawing()
    D.run([(0, 0), (24, 0)], "PID-PIPE-MAIN")
    D.run([(12, 0), (12, 9.0)], "PID-PIPE-MINOR", [((12, 4.0), 4.2)])
    D.insert("vlv_gate", (12, 4.0), "PID-VALVE", 90, 0.7)
    D.bubble("ins_disc_field", (12, 14.0), "PI", "201")
    out.append(("เกจความดัน + วาล์วโคน", "root valve ปิดแล้วถอดเกจได้", drawing_svg(D, 1.3, box=(-1, -3, 25, 20))))
    D = new_drawing()
    D.run([(0, 0), (26, 0)], "PID-PIPE-MAIN")
    D.insert("fit_strainer_y", (13, 0), "PID-VALVE")
    pdi = D.bubble("ins_disc_field", (13, 13.0), "PDI", "202")
    for x in (9.0, 17.0):
        yy = pdi[1] - (25 - (x - 13) ** 2) ** 0.5
        D.conn([(x, 0), (x, yy)])
    out.append(("ผลต่างความดัน", "ต่อสองจุดคร่อมอุปกรณ์", drawing_svg(D, 1.3, box=(-1, -6, 27, 20))))
    D = new_drawing()
    D.insert("eq_vessel_v", (8, 4), "PID-EQUIP", 0, 0.75)
    lt = D.bubble("ins_disc_field", (24, 4), "LT", "203")
    D.conn([(12.5, 9.5), (24, 9.5), D.edge(lt, (24, 12))])
    D.conn([(12.5, -1.5), (24, -1.5), D.edge(lt, (24, -4))])
    out.append(("ระดับในถัง", "ต่อด้านบนและด้านล่างของถัง", drawing_svg(D, 1.3, box=(-1, -9, 31, 17))))
    return figrow(out)


def stage_drawing(stage):
    D = new_drawing()
    D.insert("eq_pump", (10.0, 0.0), "PID-EQUIP")
    D.text("P-1", (10.0, -6.5), 2.2)
    D.insert("eq_hx_plate", (44.0, 6.0), "PID-EQUIP", 0, 0.9)
    D.text("E-1", (44.0, 16.0), 2.2)
    if stage >= 2:
        gaps = [((22.0, 0.0), 4.0), ((28.0, 0.0), 6.0)] if stage >= 3 else []
        D.run([(-2.0, 0.0), (6.0, 0.0)], "PID-PIPE-MAIN")
        D.run([(14.0, 0.0), (39.5, 0.0)], "PID-PIPE-MAIN", gaps)
        D.run([(48.5, 0.0), (59.0, 0.0)], "PID-PIPE-MAIN")
        D.insert("ann_flow", (1.0, 0.0), "PID-PIPE-MAIN")
        D.insert("ann_flow", (52.0, 0.0), "PID-PIPE-MAIN")
        D.text("40-W-01", (34.0, -2.0), 1.8, "TOP_CENTER")
    if stage >= 3:
        D.insert("vlv_check", (22.0, 0.0), "PID-VALVE")
        D.insert("vlv_control", (28.0, 0.0), "PID-VALVE")
        D.insert("act_fail_open", (28.0, 0.0), "PID-VALVE")
    if stage >= 4:
        tt = D.bubble("ins_disc_field", (55.5, 12.0), "TT", "1")
        D.conn([(55.5, 0.0), D.edge(tt, (55.5, 0.0))])
        D.bubble("ins_plc_cr", (40.0, 26.0), "TIC", "1")
        D.electric([(55.5, 17.0), (55.5, 26.0), (45.0, 26.0)])
        D.electric([(35.0, 26.0), (28.0, 26.0), (28.0, 5.8)])
    return drawing_svg(D, 0.78, box=(-3, -9, 61, 32), pad=0.5)


def fig_buildup():
    labels = [("1. อุปกรณ์", "วางอุปกรณ์หลักและ tag"), ("2. ท่อ", "ลากท่อ ลูกศร เลขท่อ"),
              ("3. วาล์ว", "แทรกวาล์วแล้วตัดท่อ"), ("4. เครื่องวัด", "วงกลม tag และสัญญาณ")]
    return figrow([(a, b, stage_drawing(i + 1)) for i, (a, b) in enumerate(labels)])


def fig_mistakes():
    pairs = []
    D1, D2 = new_drawing(), new_drawing()
    D1.run([(0, 0), (20, 0)], "PID-PIPE-MAIN")
    D1.insert("vlv_gate", (10, 0), "PID-VALVE")
    D2.run([(0, 0), (20, 0)], "PID-PIPE-MAIN", [((10, 0), 6.0)])
    D2.insert("vlv_gate", (10, 0), "PID-VALVE")
    pairs.append(("ท่อวิ่งทะลุวาล์ว", "ตัดท่อใต้วาล์ว", D1, D2))
    D1, D2 = new_drawing(), new_drawing()
    D1.run([(0, 0), (20, 0)], "PID-PIPE-MAIN")
    D1.run([(10, -8), (10, 8)], "PID-PIPE-MAIN")
    D2.run([(0, 0), (20, 0)], "PID-PIPE-MAIN")
    D2.run([(7, 0), (7, -8)], "PID-PIPE-MAIN")
    D2.run([(13, 0), (13, 8)], "PID-PIPE-MAIN")
    pairs.append(("สี่แยก ต่อหรือไม่ต่อ", "เยื้องเป็น T สองตัว", D1, D2))
    D1, D2 = new_drawing(), new_drawing()
    D1.run([(0, 0), (20, 0)], "PID-PIPE-MAIN")
    D1.electric([(10, -7.5), (10, 8.5)])          # starts so that a dash lies across the pipe
    D2.run([(0, 0), (20, 0)], "PID-PIPE-MAIN")
    D2.electric([(10, -8), (10, 8)], crossings=[(10, 0)])
    pairs.append(("สัญญาณทับท่อ", "เว้นช่องที่จุดตัด", D1, D2))
    D1, D2 = new_drawing(), new_drawing()
    D1.run([(0, 0), (20, 0)], "PID-PIPE-MAIN")
    D1.insert("ins_disc_field", (10, 9), "PID-INST")
    D1.text("TT", (10, 9), 2.5)
    D1.conn([(10, 0), (10, 9)])
    D2.run([(0, 0), (20, 0)], "PID-PIPE-MAIN")
    D2.bubble("ins_disc_field", (10, 9), "TT", "301")
    D2.conn([(10, 0), (10, 4)])
    pairs.append(("ไม่มีเลขลูป เส้นทะลุวง", "tag ครบ เส้นถึงขอบวง", D1, D2))
    cells = []
    for bad, good, d1, d2 in pairs:
        box = (-1, -9, 21, 15)
        cells.append(f'<div class="mk"><figure><div class="pic">{drawing_svg(d1, 1.2, box=box, pad=0.3)}</div>'
                     f'<figcaption><b class="bad">ผิด</b><span>{bad}</span></figcaption></figure>'
                     f'<figure><div class="pic">{drawing_svg(d2, 1.2, box=box, pad=0.3)}</div>'
                     f'<figcaption><b class="good">ถูก</b><span>{good}</span></figcaption></figure></div>')
    return '<div class="mistakes">' + "".join(cells) + "</div>"


# ---------------------------------------------------------------- assemble

FILL = {
    "SYM_VALVES": lambda: symtable("valves", 3, 2.4, 30, 20),
    "SYM_ACTUATORS": lambda: symtable("actuators", 4, 2.2, 22, 22),
    "SYM_ENDS": lambda: symtable("fittings", 4, 3.0, 22, 14,
                                 names={"fit_flange", "fit_threaded", "fit_welded", "fit_socketweld"}),
    "SYM_FITTINGS": lambda: symtable("fittings", 3, 2.6, 30, 18,
                                     names={"fit_flange_pair", "fit_reducer", "fit_reducer_ecc", "fit_strainer_y",
                                            "fit_flexible", "fit_insulation"}),
    "SYM_ANNOTATION": lambda: symtable("annotation", 3, 1.6, 34, 16),
    "SYM_EQUIPMENT": lambda: symtable("equipment", 4, 1.25, 30, 30),
    "SIGNALS": signal_table,
    "IMATRIX": instrument_matrix,
    "LAYERS": layer_table,
    "LETTERS": letters_table,
    "FIG_CROSSING": fig_crossing,
    "FIG_INLINE": fig_inline_gap,
    "FIG_BUBBLE": fig_bubble,
    "FIG_LOOP": fig_loop,
    "FIG_OFFPAGE": fig_offpage,
    "EXAMPLE": example_sheet,
    "EXAMPLE_SMALL": lambda: example_sheet(0.29),
    "FIG_PFD": fig_pfd_vs_pid,
    "FIG_COMPOSE": fig_compose,
    "FIG_PUMP": fig_pump,
    "FIG_DRAINS": fig_drains,
    "FIG_INSTCONN": fig_inst_conn,
    "FIG_BUILDUP": fig_buildup,
    "FIG_MISTAKES": fig_mistakes,
    "SYMBOL_COUNT": lambda: str(len(CATALOG)),
}

body = (HERE / "guide_body.html").read_text(encoding="utf-8")
for key, fn in FILL.items():
    if "{{" + key + "}}" in body:
        body = body.replace("{{" + key + "}}", fn())
left = re.findall(r"\{\{([A-Z_]+)\}\}", body)
left = [k for k in left if k not in ("TOC", "PAGES")]
if left:
    sys.exit(f"unknown placeholders: {left}")

# pages: number them, add header/footer, build the table of contents
sections = re.findall(r'<section class="page([^"]*)"([^>]*)>', body)
total = len(sections)
toc, n = [], 0


def wrap(m):
    global n
    n += 1
    cls, attrs = m.group(1), m.group(2)
    head = re.search(r'data-head="([^"]*)"', attrs)
    t = re.search(r'data-toc="([^"]*)"', attrs)
    if t:
        toc.append((unescape(t.group(1)), n))
    h = head.group(1) if head else ""
    chrome = (f'<header class="run"><span>คู่มือเขียน P&amp;ID ด้วย LibreCAD</span><span>{h}</span></header>'
              f'<footer class="run"><span>ฉบับ 20 ต.ค. 2569</span><span>หน้า {n} / {total}</span></footer>')
    return f'<section class="page{cls}"{attrs} data-n="{n}">{chrome}<div class="content">'


body = re.sub(r'<section class="page([^"]*)"([^>]*)>', wrap, body)
body = body.replace("</section>", "</div></section>")
toc_html = "".join(f'<li><span>{escape(t)}</span><span class="dots"></span><span>{p}</span></li>' for t, p in toc)
body = body.replace("{{TOC}}", f'<ol class="toc">{toc_html}</ol>').replace("{{PAGES}}", str(total))

css = (HERE / "guide.css").read_text(encoding="utf-8")
probe = """<script>
// ?p=N shows only sheet N (for screenshots); the title reports pages whose content overflows
const only = new URLSearchParams(location.search).get('p');
if (only) document.documentElement.classList.add('only');
window.addEventListener('DOMContentLoaded', () => {
  if (only) document.querySelectorAll('section.page').forEach(s => { if (s.dataset.n !== only) s.remove(); });
});
window.addEventListener('load', () => setTimeout(() => {
  const bad = [];
  document.querySelectorAll('section.page .content').forEach(c => {
    const over = c.scrollHeight - c.clientHeight;
    if (over > 1) bad.push(c.parentElement.dataset.n + ':' + over);
  });
  document.title = 'OVERFLOW ' + (bad.join(',') || 'none');
}, 1500));
</script>"""
html_doc = ('<!doctype html><html lang="th"><head><meta charset="utf-8"><title>คู่มือเขียน P&amp;ID ด้วย LibreCAD</title>'
            '<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Chakra+Petch:wght@500;600;700'
            '&family=Sarabun:ital,wght@0,400;0,600;0,700;1,400&family=IBM+Plex+Mono:wght@400;500'
            '&family=IBM+Plex+Sans+Condensed:wght@400;500&display=swap">'
            f"<style>{css}</style>{probe}</head><body>{body}</body></html>")
out_html = OUT / "guide.html"
out_html.write_text(html_doc, encoding="utf-8")
shutil.copytree(HERE / "img", OUT / "img", dirs_exist_ok=True)     # screenshots referenced as img/...
print(f"{out_html}: {total} pages, {len(CATALOG)} symbols")

if args.pdf:
    edge = next((e for e in (Path(r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"),
                             Path(r"C:\Program Files\Microsoft\Edge\Application\msedge.exe")) if e.exists()), None)
    if not edge:
        sys.exit("Edge not found")
    pdf = Path(args.pdf).resolve()
    subprocess.run([str(edge), "--headless=new", "--disable-gpu", "--no-first-run",
                    "--user-data-dir=" + str(OUT / "edge-prof"), "--virtual-time-budget=20000",
                    "--no-pdf-header-footer", "--print-to-pdf=" + str(pdf), out_html.resolve().as_uri()],
                   check=True, timeout=300)
    data = pdf.read_bytes()
    pages = len(re.findall(rb"/Type\s*/Page[^s]", data))
    print(f"pdf {pdf}: {pages} pages (expected {total}), {len(re.findall(rb'/FontFile', data))} embedded fonts")
    if pages != total:
        sys.exit("page count differs: a page overflowed onto the next sheet")
