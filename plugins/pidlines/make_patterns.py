"""Generate the stock pid_*.dxf symbols for the pidlines plugin.

Each symbol is drawn around (0,0) with the signal line running along +X,
on layer 0 with colour BYBLOCK so it takes the colour of the insert's layer.
Sizes are mm at plugin scale 1 (ISA-5.1 shapes, sizes are our choice).
"""
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

# pneumatic: -//-
save("pneumatic", lambda m, a: [m.add_line((x - 0.6, -1.5), (x + 0.6, 1.5), dxfattribs=a) for x in (-0.75, 0.75)])
# capillary: -X-
save("capillary", lambda m, a: (m.add_line((-1, -1), (1, 1), dxfattribs=a),
                                m.add_line((-1, 1), (1, -1), dxfattribs=a)))
# hydraulic: -L-
save("hydraulic", lambda m, a: (m.add_line((-0.6, 1.25), (-0.6, -1.25), dxfattribs=a),
                                m.add_line((-0.6, -1.25), (0.6, -1.25), dxfattribs=a)))
# software / data link: -o-
save("datalink", lambda m, a: m.add_circle((0, 0), 0.75, dxfattribs=a))
print("\n".join(sorted(p.name for p in out.glob("pid_*.dxf"))))
