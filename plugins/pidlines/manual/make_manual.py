# make_manual.py - one-page A4 Thai manual for the pidlines plugin.
#
# Usage: python make_manual.py [OUTDIR] [--pdf FILE]
#   OUTDIR (default ./out, git-ignored): pidlines_manual.html (fragment for an
#   artifact) and print.html (full page). --pdf also prints print.html to FILE
#   with Edge headless: A4, no header/footer.
# Context: the symbol pictures are drawn from ../patterns (pid_*.dxf and
#   pidlines.ini, the source tree, not an installed copy) and the line is cut
#   the way planStations() cuts a straight line, so the table shows what the
#   plugin places. The text lives in body.html ({{ROWS}}, {{DIAGRAM}}).
# Why: lived in a session scratchpad until 2026-10-20; moved here so the
#   manual can follow later pattern changes.
# Gotchas:
#   - ORDER must list exactly the patterns in ../patterns, else the script stops.
#   - Fonts come from Google Fonts. Offline, Edge falls back to Leelawadee UI /
#     Tahoma and the page may spill onto a second sheet: check the page count
#     the script prints.
#   - In body.html `.diag text {text-anchor: middle}` beats an SVG
#     text-anchor attribute; position diagram labels with x instead.
#   - Print at 100%, not fit-to-page.
import argparse, configparser, math, re, subprocess, sys
from pathlib import Path
import ezdxf

HERE = Path(__file__).resolve().parent
DIRS = [HERE.parent / "patterns"]
ORDER = ["pneumatic", "undefined", "hydraulic", "capillary", "datalink", "datalink_ext", "mechanical",
         "em_guided", "em_unguided", "cws", "cwr", "hws", "hwr"]
EDGE = [Path(r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"),
        Path(r"C:\Program Files\Microsoft\Edge\Application\msedge.exe")]

ap = argparse.ArgumentParser(description="build the pidlines A4 manual")
ap.add_argument("outdir", nargs="?", default=str(HERE / "out"))
ap.add_argument("--pdf", help="also print to this PDF with Edge headless")
args = ap.parse_args()
OUT = Path(args.outdir)
OUT.mkdir(parents=True, exist_ok=True)

files, ini = {}, {}
for d in DIRS:
    for f in d.glob("pid_*.dxf"):
        files[f.stem[4:].lower()] = f
    p = d / "pidlines.ini"
    if p.exists():
        cp = configparser.ConfigParser()
        cp.read(p, encoding="utf-8")
        for s in cp.sections():
            ini.setdefault(s, {}).update({k: v.strip('"') for k, v in cp[s].items()})
missing = [n for n in ORDER if n not in files]
if missing or set(files) - set(ORDER):
    sys.exit(f"pattern set changed: missing {missing}, extra {set(files) - set(ORDER)}")


def bulge_arc(x1, y1, x2, y2, b):
    """SVG arc command from (x1,y1) to (x2,y2), DXF bulge b, coordinates already y-flipped"""
    c = math.hypot(x2 - x1, y2 - y1)
    th = 4 * math.atan(abs(b))
    r = c / (2 * math.sin(th / 2))
    # DXF b>0 = counter-clockwise; after flipping y that is sweep-flag 0
    return f"A{r:.4f} {r:.4f} 0 {1 if th > math.pi else 0} {0 if b > 0 else 1} {x2:.4f} {y2:.4f}"


def poly_d(pts, closed):
    d = f"M{pts[0][0]:.4f} {-pts[0][1]:.4f}"
    n = len(pts)
    for i in range(n if closed else n - 1):
        x1, y1, b = pts[i]
        x2, y2, _ = pts[(i + 1) % n]
        d += " " + (bulge_arc(x1, -y1, x2, -y2, b) if abs(b) > 1e-9 else f"L{x2:.4f} {-y2:.4f}")
    return d + (" Z" if closed else "")


def symbol(name):
    """(stroked path list, filled path list) of a pattern, around (0,0)"""
    stroke, fill = [], []
    for e in ezdxf.readfile(files[name]).modelspace():
        t = e.dxftype()
        if t == "LINE":
            s, en = e.dxf.start, e.dxf.end
            stroke.append(f"M{s.x:.4f} {-s.y:.4f} L{en.x:.4f} {-en.y:.4f}")
        elif t == "CIRCLE":
            c, r = e.dxf.center, e.dxf.radius
            stroke.append(f"M{c.x + r:.4f} {-c.y:.4f} A{r} {r} 0 1 0 {c.x - r:.4f} {-c.y:.4f} A{r} {r} 0 1 0 {c.x + r:.4f} {-c.y:.4f} Z")
        elif t == "LWPOLYLINE":
            stroke.append(poly_d([tuple(p) for p in e.get_points("xyb")], e.closed))
        elif t == "HATCH":
            for bp in e.paths:
                fill.append(poly_d([tuple(v) for v in bp.vertices], True))
        else:
            sys.exit(f"{files[name].name}: unhandled {t}")
    return stroke, fill


def place(d, x):
    """moves path data d (symbol at origin) to x along the line (line is horizontal)"""
    out, toks, num_i = [], d.replace(",", " ").split(), 0
    i = 0
    while i < len(toks):
        tk = toks[i]
        if tk[0] in "MLA":
            cmd, rest = tk[0], tk[1:]
            vals = [rest] + toks[i + 1:i + (2 if cmd != "A" else 7)]
            i += 2 if cmd != "A" else 7
            v = [float(a) for a in vals]
            if cmd == "A":
                v[5] += x
                out.append(f"A{v[0]:.4f} {v[1]:.4f} {v[2]:g} {int(v[3])} {int(v[4])} {v[5]:.4f} {v[6]:.4f}")
            else:
                out.append(f"{cmd}{v[0] + x:.4f} {v[1]:.4f}")
        else:
            out.append(tk)
            i += 1
    return " ".join(out)


def stations(L, spacing, gap):
    """planStations() for a straight open line"""
    n = max(1, math.floor(L / spacing + 1e-9))
    first = (L - (n - 1) * spacing) / 2
    h, minp = gap / 2, max(0.05, 0.25 * gap)
    return [(first + k * spacing, gap > 0 and first + k * spacing - h >= minp
             and first + k * spacing + h <= L - minp) for k in range(n)]


def sample_svg(name, L=24.0, pad=1.6, width_mm=26):
    cfg = ini.get(name, {})
    gap = float(cfg.get("gap", 0))
    keep = cfg.get("line", "1") != "0"
    spacing = float(cfg.get("spacing", 0)) or 8.0
    st = stations(L, spacing, gap if keep else 0)
    stroke, fill = symbol(name)
    parts = []
    if keep:
        cuts = sorted(t for t, c in st if c)
        a = 0.0
        for t in cuts:
            parts.append(f'<path d="M{a:.3f} 0 L{t - gap / 2:.3f} 0"/>')
            a = t + gap / 2
        parts.append(f'<path d="M{a:.3f} 0 L{L:.3f} 0"/>')
    for t, _ in st:
        parts += [f'<path d="{place(d, t)}"/>' for d in stroke]
        parts += [f'<path class="f" d="{place(d, t)}"/>' for d in fill]
    h = 4.0
    return (f'<svg class="smp" viewBox="{-pad} {-h / 2} {L + 2 * pad} {h}" style="width:{width_mm}mm" '
            f'aria-hidden="true">{"".join(parts)}</svg>')


def short_label(name):
    lab = ini.get(name, {}).get("label", name)
    return lab.split("  ")[0]


rows = []
for n in ORDER:
    cfg = ini.get(n, {})
    gap = cfg.get("gap", "—") if cfg.get("line", "1") != "0" else "ไม่มีเส้น"
    if gap not in ("—", "ไม่มีเส้น") and float(gap) == 0:
        gap = "ไม่ตัด"
    th, _, en = short_label(n).partition(" (")
    rows.append(f'<tr><td>{sample_svg(n)}</td><td><b>{th}</b><span class="en">{en.rstrip(")") + " · " if en else ""}<span class="mono">pid_{n}</span></span></td>'
                f'<td class="num">{gap}</td></tr>')
dash = '<svg class="smp" viewBox="-1.6 -2 27.2 4" style="width:26mm" aria-hidden="true"><path class="dash" d="M0 0 L24 0"/></svg>'
rows.append(f'<tr class="aside"><td>{dash}</td><td><b>สัญญาณไฟฟ้า</b><span class="en">electric · ไม่ใช้ปลั๊กอิน '
            f'วาดบน layer <span class="mono">PID-SIGNAL-ELEC</span></span></td><td class="num">—</td></tr>')

# spacing / gap diagram: the white data-link dot (a pattern that cuts), magnified
DL, DS = 30.0, 12.0
DG = float(ini["datalink"]["gap"])
dst = stations(DL, DS, DG)
st_d, _ = symbol("datalink")
dg = []
a = 0.0
for t, c in dst:
    dg.append(f'<path d="M{a} 0 L{t - DG / 2} 0"/>')
    a = t + DG / 2
dg.append(f'<path d="M{a} 0 L{DL} 0"/>')
for t, _ in dst:
    dg += [f'<path d="{place(d, t)}"/>' for d in st_d]
t1, t2 = dst[0][0], dst[1][0]
def dim(x1, x2, y, label, cls=""):
    return (f'<g class="dim {cls}"><path d="M{x1} {y} L{x2} {y}" marker-start="url(#ar)" marker-end="url(#ar)"/>'
            f'<path class="ext" d="M{x1} {y - 0.9} L{x1} {y + 0.9} M{x2} {y - 0.9} L{x2} {y + 0.9}"/>'
            f'<text x="{(x1 + x2) / 2}" y="{y - 0.9}">{label}</text></g>')
diagram = (
    '<svg class="diag" viewBox="-1 -6.9 32 11.3" aria-hidden="true">'
    '<defs><marker id="ar" viewBox="0 0 6 6" refX="3" refY="3" markerWidth="4" markerHeight="4" orient="auto-start-reverse">'
    '<path class="f" d="M0 0 L6 3 L0 6 Z"/></marker></defs>'
    + "".join(dg)
    + dim(t1, t2, -4.9, "ระยะห่าง")
    + f'<g class="dim acc"><path class="ext" d="M{t2 - DG / 2} -2.0 L{t2 - DG / 2} 0.9 M{t2 + DG / 2} -2.0 L{t2 + DG / 2} 0.9"/></g>'
    + f'<text class="lbl acc" x="{t2 + 1.2}" y="-2.45">ช่องตัด = gap × สเกล</text>'
    + dim(0, t1, 3.6, "ปลายเท่ากัน", "lite")
    + dim(t2, DL, 3.6, "ปลายเท่ากัน", "lite")
    + "</svg>")

body = (HERE / "body.html").read_text(encoding="utf-8")
html = body.replace("{{ROWS}}", "\n".join(rows)).replace("{{DIAGRAM}}", diagram)
(OUT / "pidlines_manual.html").write_text(html, encoding="utf-8")
(OUT / "print.html").write_text(
    '<!doctype html><html lang="th"><head><meta charset="utf-8">'
    '<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover"></head><body>'
    + html + "</body></html>", encoding="utf-8")
print("ok", len(rows), "rows")

if args.pdf:
    edge = next((e for e in EDGE if e.exists()), None)
    if not edge:
        sys.exit("Edge not found: " + ", ".join(map(str, EDGE)))
    pdf = Path(args.pdf).resolve()
    subprocess.run([str(edge), "--headless=new", "--disable-gpu", "--no-first-run",
                    "--user-data-dir=" + str(OUT / "edge-prof"), "--virtual-time-budget=15000",
                    "--no-pdf-header-footer", "--print-to-pdf=" + str(pdf),
                    (OUT / "print.html").resolve().as_uri()], check=True, timeout=180)
    data = pdf.read_bytes()
    pages = len(re.findall(rb"/Type\s*/Page[^s]", data))
    print(f"pdf {pdf}: {pages} page(s), {len(re.findall(rb'/FontFile', data))} embedded fonts")
    if pages != 1:
        sys.exit("expected one page")
