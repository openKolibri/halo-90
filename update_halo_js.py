import re

with open('pcb/halo.js', 'r') as f:
    js = f.read()

# 1. Update Header for KiCad 9 and set thickness to 0.8
js = js.replace('(version 20171130) (host pcbnew "(5.1.2)-2")', '(version 20241229) (generator "pcbnew") (generator_version "9.0")')
js = js.replace('(thickness 1)', '(thickness 0.8)')

# 2. Add full Net Definitions in the header builder
net_defs = '  (net 0 "")\\n'
net_names = ["+3V", "GND", "CPX-0", "CPX-9", "CPX-8", "CPX-7", "CPX-6", "CPX-5", "CPX-4", "CPX-3", "CPX-2", "CPX-1", "RST", "SWIM", "MIC", "TX", "RX", "HALL", "SW", "MIC_PWR"]
for i, name in enumerate(net_names):
    net_defs += f'  (net {i+1} "{name}")\\n'

js = re.sub(r'\(net 0 ""\).*?\(net 3 .*?\)', net_defs.replace('\\n', '\n'), js, flags=re.DOTALL)

# 3. Update trace utils to handle Net Name/ID
js = js.replace('function getNetId(netName) {', '') # remove previous partial injection if any
js = js.replace('function placeSegment(layer, width, x0, y0 , x ,y){', '''
function getNetId(netName) {
    const netMap = {"+3V":1, "GND":2, "CPX-0":3, "CPX-9":4, "CPX-8":5, "CPX-7":6, "CPX-6":7, "CPX-5":8, "CPX-4":9, "CPX-3":10, "CPX-2":11, "CPX-1":12, "RST":13, "SWIM":14, "MIC":15, "TX":16, "RX":17, "HALL":18, "SW":19, "MIC_PWR":20};
    return netMap[netName] || 0;
}

function placeSegment(layer, width, x0, y0 , x ,y, netName){
    var netId = getNetId(netName);
    var netStr = netId ? " (net " + netId + ")" : "";
    var segment = "(segment (start " + x0 + " " + y0 + ") (end " + x + " " + y + ") (width " + width + ") (layer \\"" + layer + "\\" )" + netStr + ")  \\n";
    return segment;
}'''.strip())

js = js.replace('function placeVia(x, y){', '''
function placeVia(x, y, netName){
  var netId = getNetId(netName);
  var netStr = netId ? " (net " + netId + ")" : "";
  var via = "(via (at "+ x + " " + y + ") (size 0.45) (drill 0.25) (layers \\"Front\\" \\"Back\\")" + netStr + ")  \\n";
  return via;
}'''.strip())

# 4. Update createArc to pass netName
js = js.replace('function createArc(radius, startAngle, endAngle, segments, thickness, layer){', 'function createArc(radius, startAngle, endAngle, segments, thickness, layer, netName){')
js = js.replace('arc+= placeSegment(layer, thickness, x0, y0, x, y);', 'arc+= placeSegment(layer, thickness, x0, y0, x, y, netName);')

# 5. Connect the calls in the loop
# Outer ring (CPX labels)
js = js.replace('pcbFile += createArc(radius+0.5,startArc,endArc, segments, 0.254, layers[0]);', 
                'pcbFile += createArc(radius+0.5,startArc,endArc, segments, traceWidth, layers[0], "CPX-" + i);')

# Inbetween ring (Wait, inbetween ring logic is tricky. Let's look at the mapping logic)
# arcStart is (((i*9)+8)*(360/numLed));
# arcEnd is (((i+1)*9) + charlieMap.slice(i*9, (i*9)+9).indexOf(i+1))*(360/numLed);
# Signal is (i+1)
js = js.replace('pcbFile += createArc(radius,arcStart,arcEnd,arcEnd-arcStart, traceWidth, layers[0]);',
                'pcbFile += createArc(radius,arcStart,arcEnd,arcEnd-arcStart, traceWidth, layers[0], "CPX-" + (i+1));')

# Inner Via ring (unnamed generic for now or map to signal if possible)
js = js.replace('pcbFile += placeVia(x, y);', 'pcbFile += placeVia(x, y, null);') # Placeholder

# Signal tracks
js = js.replace('pcbFile += placeSegment(layers[3], traceWidth, x0, y0 , x ,y);', 'pcbFile += placeSegment(layers[3], traceWidth, x0, y0 , x ,y, "CPX-" + charlieMap[led]);')
js = js.replace('pcbFile += createArc(radius - ((1+(innerViaSpacing*(maxTrack+1)))+((layerMap[charlieMap[led]].track)*2*clearence)),angle,endAngle,angle-endAngle, traceWidth, layerMap[charlieMap[led]].layer);',
                'pcbFile += createArc(radius - ((1+(innerViaSpacing*(maxTrack+1)))+((layerMap[charlieMap[led]].track)*2*clearence)),angle,endAngle,angle-endAngle, traceWidth, layerMap[charlieMap[led]].layer, "CPX-" + charlieMap[led]);')
js = js.replace('pcbFile += createArc(radius - ((1+(innerViaSpacing*(maxTrack+1)))+((layerMap[charlieMap[led]].track)*2*clearence)),endAngle, angle, angle-endAngle, traceWidth, layerMap[charlieMap[led]].layer);',
                'pcbFile += createArc(radius - ((1+(innerViaSpacing*(maxTrack+1)))+((layerMap[charlieMap[led]].track)*2*clearence)),endAngle, angle, angle-endAngle, traceWidth, layerMap[charlieMap[led]].layer, "CPX-" + charlieMap[led]);')

# Ring 0 and maxTrack
js = js.replace('pcbFile += createArc(radius,0,360,360, traceWidth, layerMap[layer].layer);',
                'pcbFile += createArc(radius,0,360,360, traceWidth, layerMap[layer].layer, "CPX-" + layer);')
js = js.replace('pcbFile += createArc(radius - ((1+(innerViaSpacing*(maxTrack+1)))+((layerMap[layer].track)*2*clearence)),0,360,360, traceWidth, layerMap[layer].layer);',
                'pcbFile += createArc(radius - ((1+(innerViaSpacing*(maxTrack+1)))+((layerMap[layer].track)*2*clearence)),0,360,360, traceWidth, layerMap[layer].layer, "CPX-" + layer);')

# Connect to under led ring and Signal Vias
js = js.replace('pcbFile += placeSegment(layerMap[charlieMap[led]].layer, traceWidth, x0, y0 , x ,y);',
                'pcbFile += placeSegment(layerMap[charlieMap[led]].layer, traceWidth, x0, y0 , x ,y, "CPX-" + charlieMap[led]);')
js = js.replace('pcbFile += createArc(radius-ofsett,arcStart,arcEnd,arcEnd-arcStart, traceWidth, layerMap[charlieMap[led]].layer);',
                'pcbFile += createArc(radius-ofsett,arcStart,arcEnd,arcEnd-arcStart, traceWidth, layerMap[charlieMap[led]].layer, "CPX-" + charlieMap[led]);')

with open('pcb/halo.js', 'w') as f:
    f.write(js)
