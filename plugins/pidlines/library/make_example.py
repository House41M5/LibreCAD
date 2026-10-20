"""make_example.py - example P&ID: machine cooling water with a plate heat exchanger.

Usage: python make_example.py TEMPLATE.dxf LIBDIR PATTERNDIR OUT.dxf
  TEMPLATE   = PID_A3.dxf from make_template.py
  LIBDIR     = the PID symbol library from make_library.py
  PATTERNDIR = the pidlines pattern folder (pid_pneumatic.dxf, pid_cwr.dxf ...)

Context: drawn with pidkit the way the guide tells a person to draw it in
  LibreCAD (Library Browser blocks, pipes as LINEs cut by Line Gap under
  valves, signal patterns placed like the pidlines plugin).
  The system: cooling tower CT-101 -> pumps P-101A/B (duty/standby) ->
  strainer -> control valve TV-105 -> plate exchanger E-101 -> back to the
  tower; machine M-201 sends hot water (HWS) through E-101 and gets it back
  (HWR). TT-105 on HWR -> TIC-105 (PLC) -> TY-105 (I/P) -> TV-105 (fail
  open). LSL-108 on the basin opens make-up valve XV-109.
Gotchas:
  - Laid out in sheet mm on A3 20 mm too high, then moved down by DY so the
    drawing sits balanced above the title block; template entities stay.
  - Crossings: the line drawn later (signal, connection) gets a 2 mm gap.
"""
import math
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))   # python -I leaves the script dir out
from pidkit import R_INST as R, Drawing   # noqa: E402

TEMPLATE, LIBDIR, PATDIR, OUT = (Path(a) for a in sys.argv[1:5])
D = Drawing(LIBDIR, PATDIR, TEMPLATE)
FRAME = {e.dxf.handle for e in D.msp}      # template entities stay put
DY = -20.0
text, insert, run, conn, electric = D.text, D.insert, D.run, D.conn, D.electric
bubble, edge = D.bubble, D.edge

# --- cooling tower CT-101 (block scaled 1.5: bottom 190, top 220, basin line 195.25)
CT = (80.0, 205.0)
insert("eq_cooling_tower", CT, "PID-EQUIP", 0, 1.5)
text("CT-101", (110.0, 186.0), 3.0)                 # right of the suction pipe, clear of LSL-108
text("COOLING TOWER", (110.0, 181.5), 2.2)


def ct_left(y):
    return 60.5 + 6.0 * (y - 190.0) / 30.0


def ct_right(y):
    return 99.5 - 6.0 * (y - 190.0) / 30.0


# --- make-up water with solenoid valve XV-109
MU_Y = 193.0
XV = (40.0, MU_Y)
run([(26.0, MU_Y), (ct_left(MU_Y), MU_Y)], "PID-PIPE-MINOR", [(XV, 6.0)])
insert("vlv_solenoid", XV, "PID-VALVE")
text("XV-109", (XV[0], 202.6), 2.5)
text("MAKE-UP WATER", (23.0, 206.0), 2.2, "BOTTOM_LEFT")
text("25-MW-109", (49.0, 189.5), 2.2, "TOP_CENTER")
insert("ann_flow", (29.0, MU_Y), "PID-PIPE-MINOR")

# --- level switch low LSL-108 on the basin opens the make-up valve
LSL = bubble("ins_disc_field", (50.0, 172.0), "LSL", "108")
conn([edge(LSL, (66.0, LSL[1])), (66.0, LSL[1]), (66.0, 190.0)])
electric([edge(LSL, (30.0, LSL[1])), (33.5, LSL[1]), (33.5, 198.3), (XV[0] - 1.3, 198.3)], crossings=[(33.5, MU_Y)])

# --- suction from the basin to the pump header
run([(90.0, 190.0), (90.0, 150.0), (102.0, 150.0)], "PID-PIPE-MAIN")
run([(102.0, 150.0), (102.0, 120.0)], "PID-PIPE-MAIN")
text("100-CWS-101", (86.0, 170.0), 2.2, "MIDDLE_CENTER", 90)
insert("ann_flow", (90.0, 162.0), "PID-PIPE-MAIN", 270)

# --- pumps P-101A (duty) and P-101B (standby)
for y, sfx in ((150.0, "A"), (120.0, "B")):
    pump = (130.0, y)
    suct, chk, disc = (114.0, y), (144.0, y), (153.0, y)
    run([(102.0, y), (126.0, y)], "PID-PIPE-MAIN", [(suct, 6.0)])
    insert("vlv_gate", suct, "PID-VALVE")
    insert("eq_pump_centrifugal", pump, "PID-EQUIP")
    text(f"P-101{sfx}", (pump[0], y - 7.5), 2.5)
    run([(134.0, y), (164.0, y)], "PID-PIPE-MAIN", [(chk, 4.0), (disc, 6.0)])
    insert("vlv_check", chk, "PID-VALVE")
    insert("vlv_gate", disc, "PID-VALVE")
    pi = bubble("ins_disc_field", (138.0, y + 13.0), "PI", f"101{sfx}")
    conn([(138.0, y), edge(pi, (138.0, y))])
text("DUTY / STANDBY", (130.0, 106.5), 2.2)

# --- discharge header and main cooling water supply (CWS) to the exchanger
MAIN_Y = 171.0
HX = (295.0, 180.0)                        # eq_hx_plate scaled 1.4: x 288..302, y 167.4..192.6
STR = (180.0, MAIN_Y)
TV = (255.0, MAIN_Y)
run([(164.0, 120.0), (164.0, MAIN_Y)], "PID-PIPE-MAIN")
cross_tt105 = (244.0, MAIN_Y)              # TT-105 signal crosses here, the signal gets the gap
run([(164.0, MAIN_Y), (288.0, MAIN_Y)], "PID-PIPE-MAIN", [(TV, 6.0)])
insert("fit_strainer_y", STR, "PID-VALVE")
text("STR-101", (STR[0] + 3.0, 163.5), 2.2, "TOP_CENTER")
text("100-CWS-102", (212.0, 167.5), 2.2, "TOP_CENTER")
insert("ann_flow", (170.0, MAIN_Y), "PID-PIPE-MAIN")
insert("ann_flow", (278.0, MAIN_Y), "PID-PIPE-MAIN")

# PDI-102 across the strainer
pdi = bubble("ins_disc_field", (STR[0], 187.0), "PDI", "102")
for x in (STR[0] - 4.0, STR[0] + 4.0):
    yy = pdi[1] - math.sqrt(R * R - (x - pdi[0]) ** 2)
    conn([(x, MAIN_Y), (x, yy)])

# FT-103 -> FI-103 on the HMI
ft = bubble("ins_disc_field", (205.0, 187.0), "FT", "103")
conn([(205.0, MAIN_Y), edge(ft, (205.0, MAIN_Y))])
fi = bubble("ins_shared_cr", (205.0, 209.0), "FI", "103")
electric([(205.0, ft[1] + R), (205.0, fi[1] - R)])

# TT-104 -> TI-104 on the HMI (supply temperature)
tt4 = bubble("ins_disc_field", (228.0, 187.0), "TT", "104")
conn([(228.0, MAIN_Y), edge(tt4, (228.0, MAIN_Y))])
ti4 = bubble("ins_shared_cr", (228.0, 209.0), "TI", "104")
electric([(228.0, tt4[1] + R), (228.0, ti4[1] - R)])

# TV-105: control valve, fail open (cooling water keeps flowing if air or power is lost)
insert("vlv_control", TV, "PID-VALVE")
insert("act_fail_open", TV, "PID-VALVE")
insert("act_label_fo", TV, "PID-VALVE")
text("TV-105", (TV[0] + 8.5, MAIN_Y + 4.5), 2.5, "MIDDLE_LEFT")
ty = bubble("ins_disc_field", (TV[0], 196.0), "TY", "105")
text("I/P", (TV[0] + 7.0, 196.0), 2.2, "MIDDLE_LEFT")
D.pneumatic([(TV[0], ty[1] - R), (TV[0], MAIN_Y + 4.0 + 1.8)])
tic = bubble("ins_plc_cr", (TV[0], 222.0), "TIC", "105")
electric([(TV[0], tic[1] - R), (TV[0], ty[1] + R)])

# --- plate heat exchanger E-101
insert("eq_hx_plate", HX, "PID-EQUIP", 0, 1.4)
text("E-101", (HX[0], 200.5), 3.0)
text("PLATE HEAT EXCHANGER", (HX[0], 160.5), 2.2, "TOP_CENTER")

# --- cooling water return (CWR) to the top of the tower, one service marker mid run
CWR = [(288.0, 189.0), (276.0, 189.0), (276.0, 250.0), (110.0, 250.0), (110.0, 212.0), (ct_right(212.0), 212.0)]
cwr_gaps = D.pattern([(200.0, 250.0), (100.0, 250.0)], "pid_cwr", 200.0, gap=7.889, layer="PID-PIPE-MAIN")
run(CWR, "PID-PIPE-MAIN", cwr_gaps)
text("100-CWR-103", (230.0, 253.0), 2.2, "BOTTOM_CENTER")
insert("ann_flow", (276.0, 230.0), "PID-PIPE-MAIN", 90)
insert("ann_flow", (125.0, 250.0), "PID-PIPE-MAIN", 180)
tt6 = bubble("ins_disc_field", (195.0, 237.0), "TT", "106")
conn([(195.0, 250.0), edge(tt6, (195.0, 250.0))])
ti6 = bubble("ins_shared_cr", (172.0, 237.0), "TI", "106")
electric([edge(tt6, ti6), edge(ti6, tt6)])

# --- machine M-201 and its hot water loop (HWS to E-101, HWR back)
M = (365.0, 180.0)
insert("eq_box", M, "PID-EQUIP", 0, 1.0, 1.5)
text("M-201", (M[0], M[1] + 3.0), 3.0)
text("MACHINE", (M[0], M[1] - 3.0), 2.2)
run([(350.0, 189.0), (302.0, 189.0)], "PID-PIPE-MAIN",
    D.pattern([(330.0, 189.0), (306.0, 189.0)], "pid_hws", 100.0, gap=8.028, layer="PID-PIPE-MAIN"))
run([(302.0, 171.0), (350.0, 171.0)], "PID-PIPE-MAIN",
    D.pattern([(306.0, 171.0), (330.0, 171.0)], "pid_hwr", 100.0, gap=8.167, layer="PID-PIPE-MAIN"))
text("50-HWS-201", (330.0, 185.5), 2.2, "TOP_CENTER")
text("50-HWR-202", (330.0, 174.5), 2.2, "BOTTOM_CENTER")
insert("ann_flow", (346.0, 189.0), "PID-PIPE-MAIN", 180)
insert("ann_flow", (346.0, 171.0), "PID-PIPE-MAIN")
ti7 = bubble("ins_disc_field", (340.0, 204.0), "TI", "107")
conn([(340.0, 189.0), edge(ti7, (340.0, 189.0))])

# TT-105 on the water back to the machine -> TIC-105 (crosses the CWS line at x 244)
tt5 = bubble("ins_disc_field", (322.0, 156.0), "TT", "105")
conn([(322.0, 171.0), edge(tt5, (322.0, 171.0))])
electric([(322.0, tt5[1] - R), (322.0, 143.0), (244.0, 143.0), (244.0, 222.0), (TV[0] - R, 222.0)],
         crossings=[cross_tt105])

# --- legend (top right, free of the drawing)
LX0, LX1, LTOP = 300.0, 405.0, 296.0
ROWS = 8
LBOT = LTOP - 6.0 - ROWS * 6.5
D.msp.add_lwpolyline([(LX0, LBOT), (LX1, LBOT), (LX1, LTOP), (LX0, LTOP)], close=True,
                     dxfattribs={"layer": "PID-FRAME-THIN"})
D.line((LX0, LTOP - 6.0), (LX1, LTOP - 6.0), "PID-FRAME-THIN")
text("LEGEND", (LX0 + 2.0, LTOP - 3.0), 2.5, "MIDDLE_LEFT")
text("คำอธิบายสัญลักษณ์", (LX0 + 20.0, LTOP - 3.2), 2.6, "MIDDLE_LEFT", style="thsarabunnew")


def legend_row(i, label):
    y = LTOP - 6.0 - 6.5 * (i + 0.5)
    text(label, (LX0 + 31.0, y), 2.2, "MIDDLE_LEFT")
    return y, (LX0 + 4.0, LX0 + 26.0)


y, (a, b) = legend_row(0, "PROCESS LINE")
run([(a, y), (b, y)], "PID-PIPE-MAIN")
y, (a, b) = legend_row(1, "UTILITY / CONNECTION TO PROCESS")
run([(a, y), (b, y)], "PID-PIPE-MINOR")
y, (a, b) = legend_row(2, "ELECTRIC SIGNAL")
electric([(a, y), (b, y)])
y, (a, b) = legend_row(3, "PNEUMATIC SIGNAL")
D.pneumatic([(a, y), (b, y)])
y, (a, b) = legend_row(4, "SERVICE MARKER ON PIPE")
run([(a, y), (b, y)], "PID-PIPE-MAIN", D.pattern([(a, y), (b, y)], "pid_cws", 100.0, gap=7.75, layer="PID-PIPE-MAIN"))
for i, (kind, label) in enumerate((("ins_disc_field", "FIELD INSTRUMENT"),
                                   ("ins_shared_cr", "HMI DISPLAY, CONTROL ROOM"),
                                   ("ins_plc_cr", "PLC FUNCTION, CONTROL ROOM"))):
    y, (a, b) = legend_row(5 + i, label)
    insert(kind, ((a + b) / 2, y), "PID-INST", 0, 0.55)

# --- title block
fill = {
    "PID-0001": ("PID-EX-001", None),
    "PIPING & INSTRUMENTATION DIAGRAM": ("COOLING WATER SYSTEM FOR MACHINE M-201", None),
    "PROJECT NAME": ("ตัวอย่างประกอบคู่มือเขียน P&ID ด้วย LibreCAD", "thsarabunnew"),
    "YYYY-MM-DD": ("2026-10-20", None),
    "ISSUED FOR REVIEW": ("EXAMPLE FOR THE P&ID GUIDE", None),
}
first_dash = True
for t in D.msp.query("TEXT"):
    s = t.dxf.text
    if s in fill:
        t.dxf.text, st = fill[s]
        if st:
            t.dxf.style = st
            t.dxf.height = 3.4
    elif s == "-" and first_dash:          # DRAWN
        t.dxf.text, first_dash = "KAI", False

for e in D.msp:
    if e.dxf.handle not in FRAME:
        e.translate(0.0, DY, 0.0)

D.save(OUT)
print(OUT, len(D.msp), "entities,", len([b for b in D.doc.blocks if not b.name.startswith("*")]), "blocks")
