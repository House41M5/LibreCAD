"""make_template.py - P&ID drawing templates (A3 and A4 landscape) for LibreCAD.

Usage: python make_template.py [OUTDIR]          (default ./templates)
  Writes PID_A3.dxf and PID_A4.dxf: sheet frame with zone marks, title
  block, revision table, the PID-* layers and the text styles. Start a
  drawing with File > New From Template, or set the file as default under
  Options > Application Preferences > Paths > Template.

Context: P&IDs are not to scale; everything is drawn 1:1 in mm on the
  sheet, the frame's lower-left corner at (0, 0). $PLIMMIN/$PLIMMAX hold
  the paper size and $PSVPSCALE = 1 the print scale, so LibreCAD's print
  preview opens on the right paper at 1:1.
  Layers carry the pens (colour, lineweight); symbols from the PID library
  are BYBLOCK and take the pen of the layer they are inserted on.
Gotchas:
  - LibreCAD ignores the DXF STYLE table and takes a TEXT's style *name*
    as the font file name: "iso" -> iso.lff, "thsarabunnew" -> the Thai
    font installed in Documents\\LibreCAD\\fonts. Keep those names.
  - Thai text needs the Kai build of LibreCAD (Thai shaping); stock
    LibreCAD shows boxes.
  - Line type patterns print at their length in paper mm (checked in a
    dxf2pdf PDF: DASHED 12/6, DASHED2 6/3, DASHEDTINY 2/1); signal runs are
    10-20 mm long, so the electric layer uses DASHEDTINY.
  - Lineweights print only when LibreCAD prints with lineweights (default);
    "Draft" mode or a 0 width pen prints everything thin.
"""
import sys
from pathlib import Path

import ezdxf

HERE = Path(__file__).resolve().parent
OUT = Path(sys.argv[1]) if len(sys.argv) > 1 else HERE / "templates"

# name, ACI colour, lineweight (1/100 mm), linetype, purpose (Thai, for the manual)
LAYERS = [
    ("PID-EQUIP", 7, 50, "CONTINUOUS", "อุปกรณ์หลัก ถัง ปั๊ม เครื่องแลกเปลี่ยนความร้อน"),
    ("PID-PIPE-MAIN", 7, 70, "CONTINUOUS", "ท่อ process หลัก"),
    ("PID-PIPE-MINOR", 8, 35, "CONTINUOUS", "ท่อรอง utility ท่อต่อเครื่องวัด (process connection)"),
    ("PID-VALVE", 3, 35, "CONTINUOUS", "วาล์ว actuator ข้อต่อ"),
    ("PID-INST", 6, 35, "CONTINUOUS", "วงกลมเครื่องวัด แผง PLC/DCS"),
    ("PID-SIGNAL", 5, 25, "CONTINUOUS", "เส้นสัญญาณ (วางลายด้วยปลั๊กอิน PID signal line)"),
    ("PID-SIGNAL-ELEC", 5, 25, "DASHEDTINY", "สัญญาณไฟฟ้า (เส้นประในตัว ขีด 2 เว้น 1 มม.)"),
    ("PID-TEXT", 7, 25, "CONTINUOUS", "tag เลขท่อ ชื่ออุปกรณ์ หมายเหตุ"),
    ("PID-FRAME", 7, 50, "CONTINUOUS", "กรอบแบบ กรอบชื่อแบบ"),
    ("PID-FRAME-THIN", 7, 25, "CONTINUOUS", "เส้นแบ่งช่องในกรอบชื่อแบบ"),
]
SHEETS = {"A3": (420.0, 297.0), "A4": (297.0, 210.0)}
LEFT, MARGIN = 20.0, 10.0          # filing margin on the left, ISO 5457 style
TB_W = 180.0                       # title block width (ISO 7200 max)
TH, LAT = "thsarabunnew", "iso"    # text styles = LibreCAD font names


def build(name, size):
    W, H = size
    doc = ezdxf.new("R2000", setup=["linetypes"])   # no dimstyle arrow blocks cluttering the Block List
    hd = doc.header
    hd["$INSUNITS"] = 4
    hd["$MEASUREMENT"] = 1
    hd["$LTSCALE"] = 1.0
    hd["$PLIMMIN"] = (0.0, 0.0)
    hd["$PLIMMAX"] = (W, H)
    hd["$PSVPSCALE"] = 1.0
    hd["$PINSBASE"] = (0.0, 0.0, 0.0)
    hd["$LIMMIN"] = (0.0, 0.0)
    hd["$LIMMAX"] = (W, H)
    for st, font in ((LAT, "iso.lff"), (TH, "thsarabunnew.ttf")):
        if not doc.styles.has_entry(st):
            doc.styles.add(st, font=font)
    if not doc.linetypes.has_entry("DASHEDTINY"):     # LibreCAD DashLineTiny: 2 mm dash, 1 mm gap
        doc.linetypes.add("DASHEDTINY", pattern=[3.0, 2.0, -1.0], description="Dashed tiny __ __ __")
    for lname, color, lw, lt, _ in LAYERS:
        doc.layers.add(lname, color=color, linetype=lt, lineweight=lw)
    hd["$CLAYER"] = "PID-PIPE-MAIN"
    m = doc.modelspace()

    def rect(x0, y0, x1, y1, layer):
        m.add_lwpolyline([(x0, y0), (x1, y0), (x1, y1), (x0, y1)], close=True, dxfattribs={"layer": layer})

    def line(p, q, layer="PID-FRAME-THIN"):
        m.add_line(p, q, dxfattribs={"layer": layer})

    def text(s, x, y, h, style=LAT, align="LEFT", layer="PID-TEXT"):
        t = m.add_text(s, dxfattribs={"layer": layer, "height": h, "style": style})
        t.set_placement((x, y), align=ezdxf.enums.TextEntityAlignment[align])
        return t

    x0, y0, x1, y1 = LEFT, MARGIN, W - MARGIN, H - MARGIN
    rect(x0, y0, x1, y1, "PID-FRAME")

    # zone marks: columns 1..n left to right, rows A.. top to bottom (ISO 5457)
    cols = 8 if name == "A3" else 6
    rows = 6 if name == "A3" else 4
    for i in range(1, cols):
        x = x0 + (x1 - x0) * i / cols
        line((x, y0), (x, y0 - 3)); line((x, y1), (x, y1 + 3))
    for i in range(cols):
        x = x0 + (x1 - x0) * (i + 0.5) / cols
        text(str(i + 1), x, y1 + 4.5, 2.5, align="MIDDLE_CENTER", layer="PID-FRAME-THIN")
        text(str(i + 1), x, y0 - 4.5, 2.5, align="MIDDLE_CENTER", layer="PID-FRAME-THIN")
    for j in range(1, rows):
        y = y0 + (y1 - y0) * j / rows
        line((x0, y), (x0 - 3, y)); line((x1, y), (x1 + 3, y))
    for j in range(rows):
        y = y1 - (y1 - y0) * (j + 0.5) / rows
        letter = "ABCDEFGH"[j]
        text(letter, x0 - 4.5, y, 2.5, align="MIDDLE_CENTER", layer="PID-FRAME-THIN")
        text(letter, x1 + 4.5, y, 2.5, align="MIDDLE_CENTER", layer="PID-FRAME-THIN")

    # title block: bottom right, TB_W x 48
    tx0, tx1 = x1 - TB_W, x1
    yb = [y0, y0 + 10, y0 + 22, y0 + 36, y0 + 48]      # rows: people/date, dwg no, title, project
    rect(tx0, y0, tx1, yb[4], "PID-FRAME")
    for y in yb[1:4]:
        line((tx0, y), (tx1, y))

    def cell(xa, xb, ya, yb_, label, value, vh=3.0, vstyle=LAT):
        # Thai tone marks rise ~0.35 h above the cap line, lower vowels sink ~0.35 h
        text(label, xa + 1.2, yb_ - 3.4, 2.2, style=TH, layer="PID-FRAME-THIN")
        if value:
            text(value, xa + 1.5, ya + 1.5, vh, style=vstyle)

    # bottom row: drawn / checked / approved / date
    xs = [tx0, tx0 + 45, tx0 + 90, tx0 + 135, tx1]
    for x in xs[1:-1]:
        line((x, yb[0]), (x, yb[1]))
    for (xa, xb), (lab, val) in zip(zip(xs, xs[1:]), (("ผู้เขียน DRAWN", "-"), ("ตรวจ CHECKED", "-"),
                                                    ("อนุมัติ APPROVED", "-"), ("วันที่ DATE", "YYYY-MM-DD"))):
        cell(xa, xb, yb[0], yb[1], lab, val, 2.5)
    # second row: drawing number / rev / scale / sheet
    xs = [tx0, tx0 + 100, tx0 + 120, tx0 + 150, tx1]
    for x in xs[1:-1]:
        line((x, yb[1]), (x, yb[2]))
    for (xa, xb), (lab, val) in zip(zip(xs, xs[1:]), (("เลขที่แบบ DWG NO.", "PID-0001"), ("REV", "0"),
                                                    ("มาตราส่วน SCALE", "NTS"), ("แผ่น SHEET", "1 / 1"))):
        cell(xa, xb, yb[1], yb[2], lab, val, 3.5 if lab.startswith("เลขที่") else 3.0)
    cell(tx0, tx1, yb[2], yb[3], "ชื่อแบบ TITLE", "PIPING & INSTRUMENTATION DIAGRAM", 4.0)
    cell(tx0, tx1, yb[3], yb[4], "โครงการ / บริษัท PROJECT", "PROJECT NAME", 3.0)

    # revision table above the title block
    ry = [yb[4] + 5 * k for k in range(4)] + [yb[4] + 21]  # 3 rows of 5 + a 6 mm header
    rect(tx0, ry[0], tx1, ry[4], "PID-FRAME")
    for y in ry[1:4]:
        line((tx0, y), (tx1, y))
    rx = [tx0, tx0 + 14, tx0 + 40, tx1 - 20, tx1]
    for x in rx[1:-1]:
        line((x, ry[0]), (x, ry[4]))
    for (xa, xb), lab in zip(zip(rx, rx[1:]), ("REV", "วันที่ DATE", "รายละเอียด DESCRIPTION", "โดย BY")):
        text(lab, (xa + xb) / 2, ry[3] + 3.0, 2.2, style=TH, align="MIDDLE_CENTER", layer="PID-FRAME-THIN")
    text("0", (rx[0] + rx[1]) / 2, ry[0] + 2.5, 2.5, align="MIDDLE_CENTER")
    text("ISSUED FOR REVIEW", rx[2] + 1.5, ry[0] + 2.5, 2.5, align="MIDDLE_LEFT")

    # notes right above the revision table (the rest of the column stays free for the drawing)
    ny = ry[4] + 4
    text("2. DRAWING NOT TO SCALE.", tx0 + 1.2, ny, 2.5)
    text("1. INSTRUMENT SYMBOLS AND TAGS PER ISA-5.1.", tx0 + 1.2, ny + 5, 2.5)
    text("หมายเหตุ NOTES:", tx0 + 1.2, ny + 11, 2.8, style=TH)

    path = OUT / f"PID_{name}.dxf"
    path.parent.mkdir(parents=True, exist_ok=True)
    doc.saveas(path)
    return path


def main():
    for name, size in SHEETS.items():
        print(build(name, size))


if __name__ == "__main__":
    main()
