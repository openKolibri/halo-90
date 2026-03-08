import re

pcb_file = "/Users/sawaiz/Desktop/halo-90/pcb/halo-90.kicad_pcb"
with open(pcb_file, "r") as f:
    pcb_data = f.read()

# Pattern to find footprint blocks
# We'll iteratively find (footprint ...) or (module ...)
# We need to account for nested parens, so regex for a whole footprint might be tricky if not careful,
# but we can usually split by \n  (footprint or \n  (module
