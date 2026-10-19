"""Generate text line patterns (--CWS--CWS--) for the pidlines plugin.

The letters are strokes copied from a LibreCAD .lff font (ISO 3098 by default),
not TEXT entities, so the cut-out gap matches the letters exactly and every CAD
program draws them the same. Each code becomes pid_<code>.dxf, centered on
(0,0) along +X, and gets a [<code>] section (label, gap, spacing) in
pidlines.ini next to it; other sections of that file are kept as they are.

Usage:
  python make_text_patterns.py OUTDIR CODE[=label] ...
  e.g. python make_text_patterns.py patterns "CWS=Cooling water supply"
Options (env): LFF=<font.lff> (default: LibreCAD's iso.lff), HEIGHT=2.5 (mm),
  MARGIN=0.75 (mm each side of the text), SPACING=40 (mm, dialog default)
"""
import os
import re
import sys
from pathlib import Path

import ezdxf

LFF = Path(os.environ.get("LFF", r"C:\Program Files\LibreCAD\resources\fonts\iso.lff"))
HEIGHT = float(os.environ.get("HEIGHT", "2.5"))
MARGIN = float(os.environ.get("MARGIN", "0.75"))
SPACING = float(os.environ.get("SPACING", "40"))
CAP = 9.0   # .lff glyphs are drawn 9 units tall


def read_lff(path):
    """{char: [[(x, y, bulge into this vertex), ...], ...]}, letter spacing"""
    glyphs, spacing, cur = {}, 3.0, None
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        m = re.match(r"#\s*LetterSpacing:\s*([\d.]+)", line)
        if m:
            spacing = float(m.group(1))
        if not line or line.startswith("#"):
            cur = None if not line else cur
            continue
        m = re.match(r"\[([0-9A-Fa-f]{4,})\]", line)
        if m:
            cur = chr(int(m.group(1), 16))
            glyphs[cur] = []
            continue
        if cur is None:
            continue
        if line[0] == "C":          # reference to another glyph, resolved below
            glyphs[cur].append(("ref", chr(int(line[1:], 16))))
            continue
        pts = []
        for tok in line.split(";"):
            c = tok.split(",")
            if len(c) < 2:
                continue
            b = float(c[2][1:]) if len(c) == 3 and c[2].startswith("A") else 0.0
            pts.append((float(c[0]), float(c[1]), b))
        if len(pts) >= 2:
            glyphs[cur].append(pts)

    def strokes(ch, depth=0):
        out = []
        for s in glyphs.get(ch, []):
            if isinstance(s, tuple) and s[0] == "ref":
                if depth < 4:
                    out += strokes(s[1], depth + 1)
            else:
                out.append(s)
        return out

    return strokes, spacing


def layout(text, strokes, spacing):
    """strokes of the whole text in font units, and its width"""
    x, out = 0.0, []
    for i, ch in enumerate(text):
        st = strokes(ch)
        if not st:
            raise SystemExit(f"glyph {ch!r} not in {LFF}")
        maxx = max(p[0] for s in st for p in s)
        out += [[(px + x, py, b) for px, py, b in s] for s in st]
        x += maxx + (spacing if i < len(text) - 1 else 0)
    return out, x


def update_ini(path, sections):
    """replace or append [name] sections, keep everything else"""
    lines = path.read_text(encoding="utf-8").splitlines() if path.exists() else []
    out, skip = [], False
    for line in lines:
        m = re.match(r"\s*\[([^\]]+)\]", line)
        if m:
            skip = m.group(1).strip().lower() in sections
        if not skip:
            out.append(line)
    while out and not out[-1].strip():
        out.pop()
    for name, body in sections.items():
        out += ["", f"[{name}]"] + body
    path.write_text("\n".join(out) + "\n", encoding="utf-8")


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    outdir = Path(sys.argv[1])
    outdir.mkdir(parents=True, exist_ok=True)
    strokes, spacing = read_lff(LFF)
    k = HEIGHT / CAP
    sections = {}
    for arg in sys.argv[2:]:
        code, _, label = arg.partition("=")
        st, w = layout(code, strokes, spacing)
        doc = ezdxf.new("R2000")
        doc.header["$INSUNITS"] = 4
        msp = doc.modelspace()
        for s in st:   # bulge in .lff belongs to the segment arriving at a vertex; DXF wants it on the start
            pts = [((px - w / 2) * k, (py - CAP / 2) * k, s[i + 1][2] if i + 1 < len(s) else 0.0)
                   for i, (px, py, _) in enumerate(s)]
            msp.add_lwpolyline(pts, format="xyb", dxfattribs={"layer": "0", "color": 0})
        name = code.lower()
        doc.saveas(outdir / f"pid_{name}.dxf")
        gap = round(w * k + 2 * MARGIN, 3)
        sections[name] = [f'label="{label or code}  —{code}—"', f"gap={gap}", f"spacing={SPACING:g}"]
        print(f"pid_{name}.dxf  text width {w * k:.3f} mm  gap {gap} mm")
    update_ini(outdir / "pidlines.ini", sections)
    print(f"updated {outdir / 'pidlines.ini'}")


if __name__ == "__main__":
    main()
