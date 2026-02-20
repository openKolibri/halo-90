import re

with open('pcb/halo.js', 'r') as f:
    js = f.read()

# Fix the hardcoded arcs in halo.js to use (start) (mid) (end) syntax for KiCad 9
def replace_arc(m):
    start_x = float(m.group(1))
    start_y = float(m.group(2))
    end_x = float(m.group(3))
    end_y = float(m.group(4))
    angle = float(m.group(5))
    layer = m.group(6)
    width = m.group(7)
    
    # KiCad 9 gr_arc: (start X Y) (mid X Y) (end X Y)
    # The original arcs have a start at (0,0) which is actually the CENTER in old KiCad.
    # Halo.js: (gr_arc (start 0 0) (end 0 12) (angle 160) (layer "Edge.Cuts") (width 0.05))
    # Radius is distance from (0,0) to (0,12) = 12.
    # Start point of arc is (0,12).
    # End point is start point rotated by 160 degrees.
    # Mid point is start point rotated by 80 degrees.
    import math
    rads_total = math.radians(angle)
    rads_mid = rads_total / 2.0
    
    # Initial point (x=0, y=12 relative to center 0,0)
    # Wait, KiCad coordinates: Y is positive down.
    # But Math expects standard. Let's just do the rotation.
    
    # Start: (0, 12)
    # End: (12*sin(160), 12*cos(160))
    # Mid: (12*sin(80), 12*cos(80))
    
    mx = round(12 * math.sin(rads_mid), 4)
    my = round(12 * math.cos(rads_mid), 4)
    ex = round(12 * math.sin(rads_total), 4)
    ey = round(12 * math.cos(rads_total), 4)
    
    # Note: start in KiCad 9 is the ACTUAL start point of the curve.
    return f'(gr_arc (start 0 12) (mid {mx} {my}) (end {ex} {ey}) (stroke (width {width}) (type solid)) (layer "{layer}"))'

# (gr_arc (start 0 0) (end 0 12) (angle 160) (layer "Edge.Cuts") (width 0.05))
# Use non-greedy match for layer and width
js = re.sub(r'\(gr_arc \(start ([0-9.-]+) ([0-9.-]+)\) \(end ([0-9.-]+) ([0-9.-]+)\) \(angle ([0-9.-]+)\) \(layer "([^"]+)"\) \(width ([0-9.-]+)\)\)', replace_arc, js)

# Also fix the earring hook arcs
# pcbFile += "(gr_arc (start 0 -13) (end -0.772467 -13.634562) (angle 101.100007) (layer Edge.Cuts) (width 0.05))"
# Actually, I should just fix the strings directly in halo.js because my update_arc_js.py might miss them if they aren't exactly matching.
# Let's rewrite the arcs in halo.js manually in the next step.

with open('pcb/halo.js', 'w') as f:
    f.write(js)
