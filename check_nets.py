import re

with open('pcb/halo-90.kicad_pcb', 'r') as f:
    pcb = f.read()

segments = re.findall(r'\(segment\s*\(start.*?\(end.*?\(width\s+([0-9.]+)\s*\).*?layer\s+"([^"]+)".*?\)', pcb, re.S)
if segments[:5]:
    print("Found segments. Examples:")
for s in segments[:5]:
    print(s)

# Do they have a (net ..) attribute?
net_segments = re.findall(r'\(segment.*?\(net\s+\d+.*?\).*?\)', pcb, re.S)
print("Segments with net attribute:", len(net_segments))

print("Total segments:", len(segments))
