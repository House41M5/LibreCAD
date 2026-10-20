"""dxfsvg.py - draw DXF modelspace content as inline SVG for the P&ID guide.

Context: the guide's pictures (symbol tables, the example sheet) are drawn
  from the same DXF files LibreCAD uses, so a symbol changed in the library
  shows up changed in the manual. Inserts are expanded with ezdxf's
  virtual_entities(); curves go out as SVG bezier commands via ezdxf.path.
Gotchas:
  - Entities on layer 0 inside a block take the insert's layer (LibreCAD and
    AutoCAD rule), so stroke widths follow the layer the symbol sits on.
  - Line types: dash lengths are LibreCAD's, in paper mm (DASHED 12/6,
    DASHED2 6/3, DASHEDTINY 2/1, DOT 0.2/2, checked in a dxf2pdf print),
    scaled with the picture like a reduced print.
  - TEXT is drawn as SVG <text> with the page font, not LibreCAD's .lff
    font, so widths differ slightly from LibreCAD.
"""
import math
import re
from xml.sax.saxutils import escape

from ezdxf import path as ezpath

DASHES = {"DASHEDTINY": (2.0, 1.0), "DASHED2": (6.0, 3.0), "DASHED": (12.0, 6.0), "DASHEDX2": (24.0, 12.0),
          "HIDDEN": (6.0, 3.0), "HIDDEN2": (3.0, 1.5), "DOT": (0.2, 2.0), "DOT2": (0.2, 1.0)}


class Picture:
    """collected strokes, fills and texts in world coordinates (mm)"""

    def __init__(self, doc=None):
        self.doc = doc
        self.items = []

    # ------------------------------------------------------------ collect
    def _linetype(self, e, layer, parent_lt):
        lt = (e.dxf.get("linetype") or "BYLAYER").upper()
        if lt == "BYBLOCK":
            return parent_lt
        if lt == "BYLAYER" and self.doc is not None and self.doc.layers.has_entry(layer):
            return (self.doc.layers.get(layer).dxf.get("linetype") or "CONTINUOUS").upper()
        return lt

    def add(self, e, layer=None, parent_lt="CONTINUOUS"):
        t = e.dxftype()
        lay = e.dxf.get("layer") or "0"
        if layer is not None and lay == "0":
            lay = layer
        lt = self._linetype(e, lay, parent_lt)
        if t == "INSERT":
            for v in e.virtual_entities():
                self.add(v, lay, lt)
            return
        if t == "HATCH":
            for p in ezpath.from_hatch(e):
                self.items.append({"layer": lay, "kind": "fill", "path": p})
            return
        if t == "TEXT":
            d = e.dxf
            h, v = d.get("halign", 0), d.get("valign", 0)
            pos = d.align_point if (h or v) and d.hasattr("align_point") else d.insert
            # R2000 DXF keeps non-ASCII text as \U+XXXX escapes; LibreCAD decodes them too
            s = re.sub(r"\\U\+([0-9A-Fa-f]{4})", lambda m: chr(int(m.group(1), 16)), d.text)
            self.items.append({"layer": lay, "kind": "text", "text": s, "pos": (pos[0], pos[1]),
                               "height": d.height, "rot": d.get("rotation", 0.0), "h": h, "v": v,
                               "style": d.get("style", "")})
            return
        if t in ("LINE", "ARC", "CIRCLE", "ELLIPSE", "LWPOLYLINE", "POLYLINE", "SPLINE"):
            self.items.append({"layer": lay, "kind": "stroke", "path": ezpath.make_path(e),
                               "dash": DASHES.get(lt)})

    def add_all(self, entities, layer=None):
        for e in entities:
            self.add(e, layer)
        return self

    # ------------------------------------------------------------ measure
    def bbox(self):
        xs, ys = [], []
        for it in self.items:
            if it["kind"] == "text":
                x, y = it["pos"]
                w = it["height"] * 0.8 * len(it["text"])
                xs += [x - w, x + w]
                ys += [y - it["height"], y + it["height"]]
            else:
                for p in it["path"].control_vertices():
                    xs.append(p.x)
                    ys.append(p.y)
        if not xs:
            return (0, 0, 1, 1)
        return (min(xs), min(ys), max(xs), max(ys))

    # ------------------------------------------------------------ draw
    def svg(self, width_mm=None, scale=None, box=None, pad=1.0, widths=None, default_width=0.25,
            css_class="dxf", title=None, font="'IBM Plex Sans Condensed','Arial Narrow',sans-serif",
            fonts=None, pen_scale=None, colors=None):
        """inline <svg>. scale = svg mm per world mm (or width_mm to fit).
        widths: {layer: pen width in mm}; pen_scale multiplies them (None = 1, pens
        stay at their size); fonts: {text style: CSS font-family};
        colors: {layer: CSS colour} (default currentColor)"""
        x0, y0, x1, y1 = box or self.bbox()
        x0, y0, x1, y1 = x0 - pad, y0 - pad, x1 + pad, y1 + pad
        w, h = x1 - x0, y1 - y0
        if scale is None:
            scale = (width_mm / w) if width_mm else 1.0
        widths = widths or {}
        fonts = fonts or {}
        colors = colors or {}
        ps = 1.0 if pen_scale is None else pen_scale

        def X(x):
            return (x - x0) * scale

        def Y(y):
            return (y1 - y) * scale

        def d_of(p):
            out = []
            for sp in (p.sub_paths() if p.has_sub_paths else [p]):
                s = sp.start
                out.append(f"M{X(s.x):.3f} {Y(s.y):.3f}")
                for c in sp.commands():
                    n = type(c).__name__
                    e = c.end
                    if n == "LineTo":
                        out.append(f"L{X(e.x):.3f} {Y(e.y):.3f}")
                    elif n == "Curve4To":
                        out.append(f"C{X(c.ctrl1.x):.3f} {Y(c.ctrl1.y):.3f} {X(c.ctrl2.x):.3f} {Y(c.ctrl2.y):.3f} "
                                   f"{X(e.x):.3f} {Y(e.y):.3f}")
                    elif n == "Curve3To":
                        out.append(f"Q{X(c.ctrl.x):.3f} {Y(c.ctrl.y):.3f} {X(e.x):.3f} {Y(e.y):.3f}")
                    elif n == "MoveTo":
                        out.append(f"M{X(e.x):.3f} {Y(e.y):.3f}")
                if sp.is_closed:
                    out.append("Z")
            return " ".join(out)

        parts = []
        for it in self.items:
            pen = widths.get(it["layer"], default_width) * ps
            col = colors.get(it["layer"], "currentColor")
            if it["kind"] == "fill":
                parts.append(f'<path d="{d_of(it["path"])}" fill="{col}" fill-rule="evenodd" stroke="none"/>')
            elif it["kind"] == "stroke":
                dash = (f' stroke-dasharray="{it["dash"][0] * scale:.3f} {it["dash"][1] * scale:.3f}"'
                        if it["dash"] else "")
                parts.append(f'<path d="{d_of(it["path"])}" fill="none" stroke="{col}" '
                             f'stroke-width="{pen:.3f}" stroke-linecap="{"butt" if it["dash"] else "round"}" stroke-linejoin="round"{dash}/>')
            else:
                anchor = {0: "start", 1: "middle", 2: "end", 4: "middle"}.get(it["h"], "start")
                base = {0: "alphabetic", 1: "text-after-edge", 2: "central", 3: "text-before-edge"}.get(it["v"], "alphabetic")
                if it["h"] == 4:
                    base = "central"
                x, y = X(it["pos"][0]), Y(it["pos"][1])
                fs = it["height"] * scale / 0.72          # cap height ~ 0.72 em
                rot = f' transform="rotate({-it["rot"]:.2f} {x:.3f} {y:.3f})"' if abs(it["rot"]) > 1e-6 else ""
                ff = fonts.get(it["style"].lower())
                ff = f' font-family="{ff}"' if ff else ""
                parts.append(f'<text x="{x:.3f}" y="{y:.3f}" font-size="{fs:.3f}" text-anchor="{anchor}"{ff} '
                             f'dominant-baseline="{base}" fill="{col}" stroke="none"{rot}>{escape(it["text"])}</text>')
        t = f"<title>{escape(title)}</title>" if title else ""
        return (f'<svg class="{css_class}" xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w * scale:.3f} {h * scale:.3f}" '
                f'width="{w * scale:.3f}mm" height="{h * scale:.3f}mm" font-family="{font}">{t}' + "".join(parts) + "</svg>")


def file_picture(path, doc_reader):
    """Picture of a DXF file's modelspace; doc_reader = ezdxf.readfile"""
    doc = doc_reader(str(path))
    return Picture(doc).add_all(doc.modelspace())
