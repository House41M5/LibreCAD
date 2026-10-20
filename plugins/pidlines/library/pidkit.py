"""pidkit.py - draw P&ID sheets and figures with ezdxf the way a person draws them
in LibreCAD with the PID library and the pidlines plugin.

Used by make_example.py (the example sheet) and ../guide/make_guide.py (the
guide's small figures), so both show exactly what LibreCAD would show.

Context:
  - Symbols become blocks named after their library file, inserted with the
    pen of the layer (what LibreCAD's Library Browser does).
  - Pipes are LINE pieces; an inline symbol gets a gap the length of the
    symbol (what Tools > Modify > Line Gap does: gap snap = middle).
  - Signal patterns are placed like the pidlines plugin places them: evenly
    spaced and centered per straight run, rotated with the run, mirrored on
    steep runs (mirror_steep), upright on vertical runs (upright_vertical),
    on the layer of the line, cutting the line only where the pattern has a gap.
Gotchas:
  - Crossings: pass the crossing point in `crossings`; the line drawn later
    (signal, connection) gets a 2 mm gap there.
"""
import math
from pathlib import Path

import ezdxf
from ezdxf.addons import Importer
from ezdxf.enums import TextEntityAlignment

R_INST = 5.0                  # instrument bubble radius (library symbol)


def upright(a):
    """fold an angle in degrees into (-90, 90]"""
    a %= 360.0
    if 90 + 1e-9 < a <= 270 + 1e-9:
        return a - 180
    if a > 270:
        return a - 360
    return a


class Drawing:
    def __init__(self, libdir, patdir, template=None):
        self.libdir, self.patdir = Path(libdir), Path(patdir)
        if template:
            self.doc = ezdxf.readfile(template)
        else:
            self.doc = ezdxf.new("R2000", setup=True)
            self.doc.header["$INSUNITS"] = 4
            for st in ("iso", "thsarabunnew"):
                self.doc.styles.add(st, font=st)
            if not self.doc.linetypes.has_entry("DASHEDTINY"):
                self.doc.linetypes.add("DASHEDTINY", pattern=[3.0, 2.0, -1.0], description="Dashed tiny")
            for name, color, lw, lt in (("PID-EQUIP", 7, 50, "CONTINUOUS"), ("PID-PIPE-MAIN", 7, 70, "CONTINUOUS"),
                                        ("PID-PIPE-MINOR", 8, 35, "CONTINUOUS"), ("PID-VALVE", 3, 35, "CONTINUOUS"),
                                        ("PID-INST", 6, 35, "CONTINUOUS"), ("PID-SIGNAL", 5, 25, "CONTINUOUS"),
                                        ("PID-SIGNAL-ELEC", 5, 25, "DASHEDTINY"), ("PID-TEXT", 7, 25, "CONTINUOUS"),
                                        ("PID-FRAME", 7, 50, "CONTINUOUS"), ("PID-FRAME-THIN", 7, 25, "CONTINUOUS")):
                self.doc.layers.add(name, color=color, lineweight=lw, linetype=lt)
        self.msp = self.doc.modelspace()

    # ------------------------------------------------------------ blocks and text
    def lib_file(self, name):
        hits = list(self.libdir.rglob(f"{name}.dxf")) or list(self.patdir.glob(f"{name}.dxf"))
        if not hits:
            raise SystemExit(f"symbol {name} not found in {self.libdir} or {self.patdir}")
        return hits[0]

    def block(self, name):
        if name not in self.doc.blocks:
            src = ezdxf.readfile(self.lib_file(name))
            blk = self.doc.blocks.new(name)
            imp = Importer(src, self.doc)
            imp.import_entities(src.modelspace(), blk)
            imp.finalize()
        return name

    def insert(self, name, p, layer, rot=0.0, sx=1.0, sy=None):
        sy = sx if sy is None else sy
        return self.msp.add_blockref(self.block(name), p,
                                     dxfattribs={"layer": layer, "rotation": rot, "xscale": sx, "yscale": sy})

    def text(self, s, p, h=2.5, align="MIDDLE_CENTER", rot=0.0, style="iso", layer="PID-TEXT"):
        t = self.msp.add_text(s, dxfattribs={"layer": layer, "height": h, "style": style, "rotation": rot})
        t.set_placement(p, align=TextEntityAlignment[align])
        return t

    def line(self, p, q, layer, **attrs):
        return self.msp.add_line(p, q, dxfattribs={"layer": layer, **attrs})

    # ------------------------------------------------------------ runs with gaps
    def run(self, pts, layer, gaps=()):
        """polyline pts drawn as LINE pieces; gaps = [(point on the run, length)] centered there"""
        for a, b in zip(pts, pts[1:]):
            L = math.dist(a, b)
            ux, uy = (b[0] - a[0]) / L, (b[1] - a[1]) / L
            cuts = []
            for (gx, gy), gl in gaps:
                t = (gx - a[0]) * ux + (gy - a[1]) * uy
                off = abs((gx - a[0]) * uy - (gy - a[1]) * ux)
                if off < 1e-6 and -1e-6 <= t <= L + 1e-6:
                    cuts.append((max(0.0, t - gl / 2), min(L, t + gl / 2)))
            cuts.sort()
            t0 = 0.0
            for c0, c1 in cuts + [(L, L)]:
                if c0 - t0 > 1e-6:
                    self.line((a[0] + ux * t0, a[1] + uy * t0), (a[0] + ux * c0, a[1] + uy * c0), layer)
                t0 = max(t0, c1)

    def pattern(self, pts, name, spacing, gap=0.0, mirror_steep=False, upright_vertical=False, layer="PID-SIGNAL"):
        """pidlines placement on each straight piece of pts; returns the gaps to cut"""
        gaps = []
        for a, b in zip(pts, pts[1:]):
            L = math.dist(a, b)
            n = max(1, math.floor(L / spacing + 1e-9))
            first = (L - (n - 1) * spacing) / 2
            ang = math.degrees(math.atan2(b[1] - a[1], b[0] - a[0]))
            vertical = abs(math.cos(math.radians(ang))) < math.sin(math.radians(1))
            for k in range(n):
                t = first + k * spacing
                p = (a[0] + (b[0] - a[0]) * t / L, a[1] + (b[1] - a[1]) * t / L)
                if upright_vertical and vertical:
                    self.insert(name, p, layer)
                else:
                    r = upright(ang)
                    self.insert(name, p, layer, r, 1.0, -1.0 if mirror_steep and abs(r) > 45 + 1e-9 else 1.0)
                if gap > 0:
                    gaps.append((p, gap))
        return gaps

    def electric(self, pts, crossings=()):
        self.run(pts, "PID-SIGNAL-ELEC", [(c, 2.0) for c in crossings])

    def pneumatic(self, pts, spacing=12.0):
        self.run(pts, "PID-SIGNAL")
        self.pattern(pts, "pid_pneumatic", spacing, mirror_steep=True)

    def signal(self, pts, pattern, spacing=12.0, gap=0.0, crossings=(), **kw):
        """any pidlines pattern on a signal line (gap > 0 for o, text ...)"""
        gaps = self.pattern(pts, pattern, spacing, gap, layer="PID-SIGNAL", **kw)
        self.run(pts, "PID-SIGNAL", gaps + [(c, 2.0) for c in crossings])

    def conn(self, pts, crossings=()):
        """connection to process: thin line from the pipe to the instrument"""
        self.run(pts, "PID-PIPE-MINOR", [(c, 2.0) for c in crossings])

    # ------------------------------------------------------------ instruments
    def bubble(self, kind, p, letters, number, h=2.5):
        self.insert(kind, p, "PID-INST")
        self.text(letters, (p[0], p[1] + 2.0), h)
        self.text(number, (p[0], p[1] - 2.0), h)
        return p

    @staticmethod
    def edge(c, toward, r=R_INST):
        dx, dy = toward[0] - c[0], toward[1] - c[1]
        L = math.hypot(dx, dy)
        return (c[0] + dx * r / L, c[1] + dy * r / L)

    def save(self, path):
        self.doc.saveas(path)
        return path
