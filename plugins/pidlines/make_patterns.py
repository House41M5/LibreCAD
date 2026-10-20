"""Generate the stock pid_*.dxf symbols for the pidlines plugin.

Each symbol is drawn around (0,0) with the signal line running along +X,
on layer 0 with colour BYBLOCK so it takes the colour of the insert's layer.
Sizes are mm at plugin scale 1 (ISA-5.1 shapes, sizes are our choice).

Not here on purpose: electric signal = the built-in Dash line type; process
and instrument-supply lines = plain solid lines. Sources disagree on two
shapes -- hydraulic (L vs single slash) and mechanical link. L follows the
ISA-5.1-based references; since 2026-10-20 the circles follow the user's ISA
line-type table: mechanical link = ring with a filled dot (◉); data link comes
white (o, inside one system, line cut) and black (●, between independent
computer-based systems such as DCS-to-PLC, drawn over the line).
"""
import math
import sys
from pathlib import Path
import ezdxf

out = Path(sys.argv[1] if len(sys.argv) > 1 else Path(__file__).parent / "patterns")
out.mkdir(parents=True, exist_ok=True)

def save(name, draw):
    doc = ezdxf.new("R2000")
    doc.header["$INSUNITS"] = 4  # mm
    msp = doc.modelspace()
    attribs = {"layer": "0", "color": 0}  # 0 = BYBLOCK
    draw(msp, attribs)
    doc.saveas(out / f"pid_{name}.dxf")

# pneumatic: -//- strokes at 45° to the line; with mirror_steep=1 in pidlines.ini a
# vertical line shows the same // as a horizontal one (no 45° stroke turns into \\)
save("pneumatic", lambda m, a: [m.add_line((x - 1.5, -1.5), (x + 1.5, 1.5), dxfattribs=a) for x in (-0.75, 0.75)])
# undefined signal (type not of concern, e.g. on a PFD): -/- one 45° stroke, mirror_steep like //
save("undefined", lambda m, a: m.add_line((-1.5, -1.5), (1.5, 1.5), dxfattribs=a))
# capillary: -X-
save("capillary", lambda m, a: (m.add_line((-1, -1), (1, 1), dxfattribs=a),
                                m.add_line((-1, 1), (1, -1), dxfattribs=a)))
# hydraulic: -L- (mirror_steep=1 in pidlines.ini: a vertical run shows L, not a backwards L)
save("hydraulic", lambda m, a: (m.add_line((-0.6, 1.25), (-0.6, -1.25), dxfattribs=a),
                                m.add_line((-0.6, -1.25), (0.6, -1.25), dxfattribs=a)))
# software / data link: -o-
save("datalink", lambda m, a: m.add_circle((0, 0), 0.75, dxfattribs=a))

def dot(m, r):
    """solid BYBLOCK hatch disc around (0,0), so it stays round at any zoom"""
    h = m.add_hatch(color=0, dxfattribs={"layer": "0"})
    h.paths.add_polyline_path([(r, 0, 1), (-r, 0, 1)], is_closed=True)

# data link between independent computer-based systems: -●- (outline + filled disc)
def datalink_ext(m, a):
    m.add_circle((0, 0), 0.75, dxfattribs=a)
    dot(m, 0.75)
save("datalink_ext", datalink_ext)

# mechanical link: -◉- (ring + filled dot; the line stops at the ring)
def mechanical(m, a):
    m.add_circle((0, 0), 0.75, dxfattribs=a)
    dot(m, 0.4)
save("mechanical", mechanical)

def wave(periods, length=3.0, amp=1.0, steps=24):
    """Sine wave centered on (0,0) along X: `periods` full periods, each `length` long,
    trough first then crest, as in the ISA line-type table."""
    half = periods * length / 2
    n = periods * steps
    return [(-half + i * 2 * half / n, -amp * math.sin(2 * math.pi * i / steps)) for i in range(n + 1)]

# electromagnetic or sonic signal, guided: -∿- (one period over the line)
save("em_guided", lambda m, a: m.add_lwpolyline(wave(1), dxfattribs=a))
# electromagnetic or sonic signal, not guided: ∿ ∿ (one period per symbol; the base line is deleted)
save("em_unguided", lambda m, a: m.add_lwpolyline(wave(1), dxfattribs=a))
print("\n".join(sorted(p.name for p in out.glob("pid_*.dxf"))))
