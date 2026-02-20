// Javascript program for creating a base template for charlieplexed led arrays
const fs = require("fs");

// Globals
const layers = ["Front", "In1.Cu", "In2.Cu", "Back"];
const radius = 11;
const numLed = 90;
const innerViaSpacing = 0.55;
const clearance = 0.1;
const traceWidth = 0.1;

// Create a map of 
const charlieMap = [];
for (let i = 0; i < 10; i++) {
  for (let j = 9; j >= 0; j--) {
    if (i != j) {
      charlieMap.push(j);
    }
  }
}

const layerMap = [
  { layer: layers[0], track: 1 },
  { layer: layers[0], track: 2 },
  { layer: layers[0], track: 3 },
  { layer: layers[1], track: 0 },
  { layer: layers[1], track: 1 },
  { layer: layers[1], track: 2 },
  { layer: layers[1], track: 3 },
  { layer: layers[2], track: 0 },
  { layer: layers[2], track: 2 },
  { layer: layers[2], track: 3 }
];

let maxTrack = 0;
for (let i = 0; i < layerMap.length; i++) {
  if (layerMap[i].track > maxTrack) {
    maxTrack = layerMap[i].track;
  }
}

// Net lookup
const netLabels = ["+3V", "GND", "CPX-0", "CPX-9", "CPX-8", "CPX-7", "CPX-6", "CPX-5", "CPX-4", "CPX-3", "CPX-2", "CPX-1", "RST", "SWIM", "MIC", "TX", "RX", "HALL", "SW", "MIC_PWR"];
function getNetId(name) {
  const idx = netLabels.indexOf(name);
  return idx >= 0 ? idx + 1 : 0;
}

function getNetStr(name) {
  const id = getNetId(name);
  return id ? ` (net ${id} "${name}")` : "";
}

// Utility functions
function placeSegment(layer, width, x0, y0, x, y, netName) {
  return `(segment (start ${x0} ${y0}) (end ${x} ${y}) (width ${width}) (layer "${layer}") ${getNetStr(netName)}) \n`;
}

function placeVia(x, y, netName) {
  return `(via (at ${x} ${y}) (size 0.45) (drill 0.25) (layers "Front" "Back") ${getNetStr(netName)}) \n`;
}

function createArc(radius, startAngle, endAngle, segments, thickness, layer, netName) {
  let arc = "";
  for (let i = 0; i < segments; i++) {
    let a0 = (startAngle + (i * (endAngle - startAngle) / segments)) * Math.PI / 180;
    let af = (startAngle + ((i + 1) * (endAngle - startAngle) / segments)) * Math.PI / 180;
    let x0 = Math.round(Math.sin(a0) * radius * 10000) / 10000;
    let y0 = Math.round(Math.cos(a0) * radius * 10000) / 10000;
    let xf = Math.round(Math.sin(af) * radius * 10000) / 10000;
    let yf = Math.round(Math.cos(af) * radius * 10000) / 10000;
    arc += placeSegment(layer, thickness, x0, y0, xf, yf, netName);
  }
  return arc;
}

function placeLED(ref, x, y, rot) {
  const commonPin = Math.floor((ref - 1) / 9);
  const sinkPin = charlieMap[ref - 1];
  return `(module BL-HUB37A-AV-TRB:D-0402 (layer "Front") (at ${x} ${y} ${rot})
      (fp_text reference "D${ref}" (at 5 0 ${rot}) (layer "F.SilkS") (effects (font (size 0.25 0.25) (thickness 0.05))))
      (fp_text value "LED" (at -4 0 ${rot}) (layer "F.SilkS") (effects (font (size 0.25 0.25) (thickness 0.05))))
      (pad "1" smd trapezoid (at -0.525 0 ${rot}) (size 0.4 0.5) (layers "F.Cu" "F.Paste" "F.Mask") (net ${getNetId("CPX-" + commonPin)} "CPX-${commonPin}"))
      (pad "2" smd trapezoid (at 0.525 0 ${rot}) (size 0.4 0.5) (layers "F.Cu" "F.Paste" "F.Mask") (net ${getNetId("CPX-" + sinkPin)} "CPX-${sinkPin}"))
    )\n`;
}

function placeBattery(ref, x, y, rot) {
  return `(module BAT-HLD-001:BAT-HLD-001-HALO (layer "Back") (at ${x} ${y} ${rot})
      (fp_text reference "BT${ref}" (at 0 0 ${rot}) (layer "B.SilkS") (effects (justify mirror) (font (size 0.25 0.25) (thickness 0.05))))
      (fp_text value "CR2032" (at 0 0 ${rot}) (layer "B.SilkS") (effects (justify mirror) (font (size 0.25 0.25) (thickness 0.05))))
      (pad "1" smd circle (at 0 0) (size 10 10) (layers "B.Cu") ${getNetStr("+3V")})
      (pad "2" smd circle (at 0 0) (size 18 18) (layers "B.Cu") ${getNetStr("GND")})
    )\n`;
}

function placeUc(x, y, rot) {
  return `(module STM8L15xxx:UFQFPN28 (layer "Front") (at ${x} ${y} ${rot})
      (fp_text reference "U1" (at 0 0) (layer "F.SilkS") (effects (font (size 1 1) (thickness 0.1))))
      (fp_text value "STM8L15xxx" (at 0 0) (layer "Dwgs.User") (effects (font (size 1 1) (thickness 0.1))))
    )\n`;
}

function placeHook(x, y, rot) {
  return `(module earringHookWire:earringHookWire (layer "Front") (at ${x} ${y} ${rot})
      (fp_text reference "H1" (at 0 -5) (layer "F.SilkS") (effects (font (size 1 1) (thickness 0.15))))
    )\n`;
}

function header() {
  let nets = '(net 0 "")\n';
  netLabels.forEach((name, i) => {
    nets += `  (net ${i + 1} "${name}")\n`;
  });

  return `(kicad_pcb (version 20241229) (generator "pcbnew") (generator_version "9.0")
  (general
    (thickness 0.8)
  )
  (layers
    (0 "Front" signal)
    (1 "In1.Cu" signal)
    (2 "In2.Cu" signal)
    (31 "Back" signal)
    (36 "B.SilkS" user)
    (37 "F.SilkS" user)
    (38 "B.Mask" user)
    (39 "F.Mask" user)
    (44 "Edge.Cuts" user)
    (46 "B.CrtYd" user hide)
    (47 "F.CrtYd" user hide)
  )
  (setup
    (stackup
      (layer "Front" (type "copper") (thickness 0.035))
      (layer "dielectric 1" (type "prepreg") (thickness 0.1) (material "FR4"))
      (layer "In1.Cu" (type "copper") (thickness 0.035))
      (layer "dielectric 2" (type "core") (thickness 0.44) (material "FR4"))
      (layer "In2.Cu" (type "copper") (thickness 0.035))
      (layer "dielectric 3" (type "prepreg") (thickness 0.1) (material "FR4"))
      (layer "Back" (type "copper") (thickness 0.035))
    )
    (pad_to_mask_clearance 0.05)
    (pcbplotparams
      (outputdirectory "gerbers")
    )
  )
  ${nets}
  (net_class "Default" "Default net class"
    (clearance ${clearance})
    (trace_width ${traceWidth})
    (via_dia 0.45)
    (via_drill 0.25)
  )
`;
}

// Generate File
let pcbFile = header();
pcbFile += placeUc(0, 0, 0);
pcbFile += placeHook(0, -13, 0);
pcbFile += placeBattery(1, 0, 0, 0);

// Outline
pcbFile += `(gr_arc (start 0 12) (mid 11.8177 2.0838) (end 4.1042 -11.2763) (stroke (width 0.05) (type solid)) (layer "Edge.Cuts"))`;
pcbFile += `(gr_arc (start 0 12) (mid -11.8177 2.0838) (end -4.1042 -11.2763) (stroke (width 0.05) (type solid)) (layer "Edge.Cuts"))`;

// LEDs
let angle = 0;
for (let i = 0; i < numLed; i++) {
  let a = angle * Math.PI / 180;
  let x = Math.round(Math.sin(a) * radius * 10000) / 10000;
  let y = Math.round(Math.cos(a) * radius * 10000) / 10000;
  pcbFile += placeLED(i + 1, x, y, angle - 90);
  angle += 360 / numLed;
}

// Connections
for (let i = 0; i < 10; i++) {
  let startArc = (i * 9) * (360 / numLed);
  let endArc = ((i * 9) + 8) * (360 / numLed);
  pcbFile += createArc(radius + 0.5, startArc, endArc, endArc - startArc, traceWidth, layers[0], "CPX-" + i);
}

// Inbetween
for (let i = 0; i < 9; i++) {
  let arcStart = (((i * 9) + 8) * (360 / numLed));
  let arcEnd = (((i + 1) * 9) + charlieMap.slice(i * 9, (i * 9) + 9).indexOf(i + 1)) * (360 / numLed);
  pcbFile += createArc(radius, arcStart, arcEnd, arcEnd - arcStart, traceWidth, layers[0], "CPX-" + (i + 1));
}

// Finalization
pcbFile += ")";

try { fs.unlinkSync("./pcb/halo-90.kicad_pcb"); } catch (e) { }
fs.writeFileSync("./pcb/halo-90.kicad_pcb", pcbFile);
console.log("PCB regenerated with KiCad 9 format and named nets.");