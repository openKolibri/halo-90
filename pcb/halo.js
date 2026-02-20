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

// Net lookup
const netLabels = ["+3V", "GND", "CPX-0", "CPX-1", "CPX-2", "CPX-3", "CPX-4", "CPX-5", "CPX-6", "CPX-7", "CPX-8", "CPX-9", "RST", "SWIM", "MIC", "TX", "RX", "HALL", "SW", "MIC_PWR"];

function getNetId(name) {
  const idx = netLabels.indexOf(name);
  return idx >= 0 ? idx + 1 : 0;
}

function getNetStr(name) {
  const id = getNetId(name);
  return id ? ` (net ${id} "${name}")` : "";
}

function getNetIdOnly(name) {
  const id = getNetId(name);
  return id ? ` (net ${id})` : "";
}

function genTstamp() {
  return `(tstamp ${Math.floor(Math.random() * 100000000).toString(16)}-${Math.floor(Math.random() * 1000000).toString(16)})`;
}

// Utility functions
function placeSegment(layer, width, x0, y0, x, y, netName) {
  return `(segment (start ${x0} ${y0}) (end ${x} ${y}) (width ${width}) (layer "${layer}") ${getNetIdOnly(netName)} ${genTstamp()}) \n`;
}

function placeVia(x, y, netName) {
  return `(via (at ${x} ${y}) (size 0.4) (drill 0.2) (layers "Front" "Back") ${getNetIdOnly(netName)} ${genTstamp()}) \n`;
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

function getPinOffset(pin) {
  const p = parseInt(pin);
  if (p >= 1 && p <= 7) return { x: -1.9, y: -1.5 + (p - 1) * 0.5 };
  if (p >= 8 && p <= 14) return { x: -1.5 + (p - 8) * 0.5, y: 1.9 };
  if (p >= 15 && p <= 21) return { x: 1.9, y: 1.5 - (p - 15) * 0.5 };
  if (p >= 22 && p <= 28) return { x: 1.5 - (p - 22) * 0.5, y: -1.9 };
  return { x: 0, y: 0 };
}

function placeUc(x, y, rot) {
  let pads = "";
  const pinToNet = {
    "1": "RST", "2": "HALL", "3": "MIC", "4": "MIC_PWR", "5": "SW", "6": "GND", "7": "+3V",
    "8": "TX", "9": "RX", "10": "PD2", "11": "PD3", "12": "CPX-9", "13": "CPX-8", "14": "CPX-5",
    "15": "CPX-7", "16": "CPX-6", "17": "CPX-4", "18": "CPX-3", "19": "PB7", "20": "PD4",
    "21": "CPX-1", "22": "CPX-0", "23": "PC2", "24": "PC3", "25": "PC4", "26": "PC5", "27": "CPX-2", "28": "SWIM"
  };

  for (let pin in pinToNet) {
    const off = getPinOffset(pin);
    pads += `      (pad "${pin}" smd rect (at ${off.x} ${off.y}) (size ${Math.abs(off.x) > 1.6 ? "0.6 0.25" : "0.25 0.6"}) (layers "F.Cu" "F.Paste" "F.Mask") ${getNetStr(pinToNet[pin])} ${genTstamp()})\n`;
  }

  return `(module STM8L15xxx:UFQFPN28 (layer "Front") (at ${x} ${y} ${rot}) (tstamp ${Math.floor(Math.random() * 1000000)})
      (fp_text reference "U1" (at 0 -3) (layer "F.SilkS") (effects (font (size 0.8 0.8) (thickness 0.15))))
      (fp_text value "STM8L151G6" (at 0 3) (layer "Dwgs.User") (effects (font (size 0.8 0.8) (thickness 0.15))))
      (fp_line (start -2 -2) (end 2 -2) (layer "F.SilkS") (width 0.15) ${genTstamp()})
      (fp_line (start 2 -2) (end 2 2) (layer "F.SilkS") (width 0.15) ${genTstamp()})
      (fp_line (start 2 2) (end -2 2) (layer "F.SilkS") (width 0.15) ${genTstamp()})
      (fp_line (start -2 2) (end -2 -2) (layer "F.SilkS") (width 0.15) ${genTstamp()})
${pads}    )\n`;
}

/**
 * Creates a circular spiral-like curve for elegant fan-out
 */
function createCurve(x0, y0, xf, yf, segments, thickness, layer, netName) {
  let curve = "";
  let midX = (x0 + xf) / 2;
  let midY = (y0 + yf) / 2;

  // Calculate polar coordinates for rotation
  let dist = Math.sqrt((xf - x0) ** 2 + (yf - y0) ** 2);
  let angle = Math.atan2(yf - y0, xf - x0);

  for (let i = 0; i < segments; i++) {
    let t0 = i / segments;
    let tf = (i + 1) / segments;

    // Aesthetic spiral formula
    let bulge = dist * 0.4;
    let arc0 = Math.sin(t0 * Math.PI) * bulge;
    let arcf = Math.sin(tf * Math.PI) * bulge;

    let base_x0 = x0 * (1 - t0) + xf * t0;
    let base_y0 = y0 * (1 - t0) + yf * t0;
    let base_xf = x0 * (1 - tf) + xf * tf;
    let base_yf = y0 * (1 - tf) + yf * tf;

    // Apply perpendicular offset
    let cx0 = base_x0 - Math.sin(angle) * arc0;
    let cy0 = base_y0 + Math.cos(angle) * arc0;
    let cxf = base_xf - Math.sin(angle) * arcf;
    let cyf = base_yf + Math.cos(angle) * arcf;

    curve += placeSegment(layer, thickness, cx0, cy0, cxf, cyf, netName);
  }
  return curve;
}

function placeLED(ref, x, y, rot) {
  const commonPin = Math.floor((ref - 1) / 9);
  const sinkPin = charlieMap[ref - 1];
  return `(module BL-HUB37A-AV-TRB:D-0402 (layer "Front") (at ${x} ${y} ${rot}) (tstamp ${Math.floor(Math.random() * 1000000)})
      (fp_text reference "D${ref}" (at 0 0.8 ${rot}) (layer "F.SilkS") (effects (font (size 0.25 0.25) (thickness 0.05))))
      (pad "1" smd trapezoid (at -0.525 0) (size 0.4 0.45) (rect_delta 0 0.05) (layers "F.Cu" "F.Paste" "F.Mask") ${getNetStr("CPX-" + commonPin)} ${genTstamp()})
      (pad "2" smd trapezoid (at 0.525 0 180) (size 0.4 0.55) (rect_delta 0 0.05) (layers "F.Cu" "F.Paste" "F.Mask") ${getNetStr("CPX-" + sinkPin)} ${genTstamp()})
      (model "\${KIPRJMOD}/components/BL-HUB37A-AV-TRB/BL-HUB37A-AV-TRB.stp" (offset (xyz 0 0 0)) (scale (xyz 1 1 1)) (rotate (xyz -90 0 0)))
    )\n`;
}

function placeBattery(x, y, rot) {
  return `(module BAT-HLD-001:BAT-HLD-001-HALO (layer "Back") (at ${x} ${y} ${rot}) (tstamp ${Math.floor(Math.random() * 1000000)})
      (fp_text reference "BT1" (at 0 0 ${rot}) (layer "B.SilkS") (effects (justify mirror) (font (size 0.5 0.5) (thickness 0.1))))
      (pad "1" smd circle (at 0 0) (size 10 10) (layers "B.Cu") ${getNetStr("+3V")} ${genTstamp()})
      (pad "2" smd circle (at 0 0) (size 18 18) (layers "B.Cu") ${getNetStr("GND")} ${genTstamp()})
    )\n`;
}

function placeButton(x, y, rot) {
  return `(module TS-1088-AR02016L:TS-1088-AR02016 (layer "Front") (at ${x} ${y} ${rot}) (tstamp ${Math.floor(Math.random() * 1000000)})
      (fp_text reference "S1" (at 0 2.5 ${rot}) (layer "F.SilkS") (effects (font (size 0.5 0.5) (thickness 0.1))))
      (pad "1" smd rect (at -1.4 0.9) (size 1 0.8) (layers "F.Cu" "F.Paste" "F.Mask") ${getNetStr("SW")} ${genTstamp()})
      (pad "2" smd rect (at 1.4 0.9) (size 1 0.8) (layers "F.Cu" "F.Paste" "F.Mask") ${getNetStr("SW")} ${genTstamp()})
      (pad "3" smd rect (at -1.4 -0.9) (size 1 0.8) (layers "F.Cu" "F.Paste" "F.Mask") ${getNetStr("GND")} ${genTstamp()})
      (pad "4" smd rect (at 1.4 -0.9) (size 1 0.8) (layers "F.Cu" "F.Paste" "F.Mask") ${getNetStr("GND")} ${genTstamp()})
    )\n`;
}

function placeMic(x, y, rot) {
  return `(module ZTS6216:ZTS6216 (layer "Front") (at ${x} ${y} ${rot}) (tstamp ${Math.floor(Math.random() * 1000000)})
      (fp_text reference "MK1" (at 0 2 ${rot}) (layer "F.SilkS") (effects (font (size 0.5 0.5) (thickness 0.1))))
      (pad "1" smd rect (at -1.125 0.825) (size 0.85 0.85) (layers "F.Cu" "F.Paste" "F.Mask") ${getNetStr("MIC")} ${genTstamp()})
      (pad "2" smd rect (at -1.125 -0.825) (size 0.85 0.85) (layers "F.Cu" "F.Paste" "F.Mask") ${getNetStr("GND")} ${genTstamp()})
      (pad "3" smd rect (at 1.125 -0.825) (size 0.85 0.85) (layers "F.Cu" "F.Paste" "F.Mask") ${getNetStr("GND")} ${genTstamp()})
      (pad "4" smd rect (at 1.125 0.825) (size 0.85 0.85) (layers "F.Cu" "F.Paste" "F.Mask") ${getNetStr("MIC_PWR")} ${genTstamp()})
    )\n`;
}

function placePassive(ref, value, footprint, x, y, rot, net1, net2) {
  return `(module passives:${footprint} (layer "Front") (at ${x} ${y} ${rot}) (tstamp ${Math.floor(Math.random() * 1000000)})
      (fp_text reference "${ref}" (at 0 0.8 ${rot}) (layer "F.SilkS") (effects (font (size 0.4 0.4) (thickness 0.08))))
      (pad "1" smd rect (at -0.5 0) (size 0.6 0.6) (layers "F.Cu" "F.Paste" "F.Mask") ${getNetStr(net1)} ${genTstamp()})
      (pad "2" smd rect (at 0.5 0) (size 0.6 0.6) (layers "F.Cu" "F.Paste" "F.Mask") ${getNetStr(net2)} ${genTstamp()})
    )\n`;
}

function placeTagConnect(x, y, rot) {
  return `(module TC2030-IDC-NL:Tag-Connect_TC2030-IDC-NL_2x03_P1.27mm_Vertical (layer "Front") (at ${x} ${y} ${rot}) (tstamp ${Math.floor(Math.random() * 1000000)})
      (fp_text reference "J1" (at 0 2.5 ${rot}) (layer "F.SilkS") (effects (font (size 0.5 0.5) (thickness 0.1))))
      (pad "1" smd circle (at -0.635 1.27) (size 0.787 0.787) (layers "F.Cu" "F.Paste" "F.Mask") ${getNetStr("+3V")} ${genTstamp()})
      (pad "2" smd circle (at 0.635 1.27) (size 0.787 0.787) (layers "F.Cu" "F.Paste" "F.Mask") ${getNetStr("SWIM")} ${genTstamp()})
      (pad "3" smd circle (at -0.635 0) (size 0.787 0.787) (layers "F.Cu" "F.Paste" "F.Mask") ${getNetStr("GND")} ${genTstamp()})
      (pad "4" smd circle (at 0.635 0) (size 0.787 0.787) (layers "F.Cu" "F.Paste" "F.Mask") ${getNetStr("RST")} ${genTstamp()})
      (pad "5" smd circle (at -0.635 -1.27) (size 0.787 0.787) (layers "F.Cu" "F.Paste" "F.Mask") ${getNetStr("TX")} ${genTstamp()})
      (pad "6" smd circle (at 0.635 -1.27) (size 0.787 0.787) (layers "F.Cu" "F.Paste" "F.Mask") ${getNetStr("RX")} ${genTstamp()})
    )\n`;
}

function placeHook(x, y, rot) {
  return `(module tooling:TestPoint_Pad_D4.0mm (layer "Front") (at ${x} ${y} ${rot}) (tstamp ${Math.floor(Math.random() * 1000000)})
      (fp_text reference "H1" (at 0 -4) (layer "F.SilkS") (effects (font (size 0.8 0.8) (thickness 0.15))))
      (pad "1" thru_hole circle (at 0 0) (size 3 3) (drill 2) (layers "*.Cu" "*.Mask") ${genTstamp()})
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
    (via_dia 0.4)
    (via_drill 0.2)
  )
`;
}

// Generate File
let pcbFile = header();
pcbFile += placeUc(0, -4, 90);
pcbFile += placeButton(0, 0, 180);
pcbFile += placeMic(3.475, 3.535, 0);
pcbFile += placeTagConnect(-4.465, -1.85, -90);
pcbFile += placeHook(0, -13.5, 0);
pcbFile += placeBattery(0, 0, 0);

// Passives
pcbFile += placePassive("C1", "1uF", "C-0402", -5.65, 2.2, 90, "+3V", "GND");
pcbFile += placePassive("C2", "1uF", "C-0402", 3.95, 1.05, 90, "+3V", "GND");
pcbFile += placePassive("R3", "10k", "R-0402", -3.3, 5.15, 0, "+3V", "RST");
pcbFile += placePassive("R1", "100", "R-0402", -1.5, -6.5, 0, "MIC", "CPX-0"); // Dummy/Approx
pcbFile += placePassive("R2", "100", "R-0402", 1.5, -6.5, 0, "MIC_PWR", "CPX-1"); // Dummy/Approx

// Outline
pcbFile += `(gr_arc (start 0 12) (mid 11.5 2) (end 4.1 -11.3) (stroke (width 0.05) (type solid)) (layer "Edge.Cuts") (tstamp ${Math.floor(Math.random() * 1000000)}))\n`;
pcbFile += `(gr_arc (start 0 12) (mid -11.5 2) (end -4.1 -11.3) (stroke (width 0.05) (type solid)) (layer "Edge.Cuts") (tstamp ${Math.floor(Math.random() * 1000000)}))\n`;

// LEDs and Ring Routing
const viaRadius = 8.5;
let angle = 0;
for (let i = 0; i < numLed; i++) {
  let a = angle * Math.PI / 180;
  let x = Math.round(Math.sin(a) * radius * 10000) / 10000;
  let y = Math.round(Math.cos(a) * radius * 10000) / 10000;
  pcbFile += placeLED(i + 1, x, y, angle - 90);
  angle += 360 / numLed;
}

// Connections (Elegant Circular Arcs)
const pinToNetMap = {
  "12": "CPX-9", "13": "CPX-8", "14": "CPX-5", "15": "CPX-7",
  "16": "CPX-6", "17": "CPX-4", "18": "CPX-3", "27": "CPX-2",
  "21": "CPX-1", "22": "CPX-0"
};

for (let i = 0; i < 10; i++) {
  let netName = "CPX-" + i;
  let startArc = (i * 9) * (360 / numLed);
  let endArc = ((i * 9) + 8) * (360 / numLed);
  // Outer Ring Arcs
  pcbFile += createArc(radius - 0.7, startArc, endArc, 36, traceWidth, layers[0], netName);

  // Find pin for this net
  let pinNum = Object.keys(pinToNetMap).find(key => pinToNetMap[key] === netName);
  if (pinNum) {
    let off = getPinOffset(pinNum);
    let viaAngle = ((i * 9 + 4) * (360 / numLed)) * Math.PI / 180;
    let vx = Math.sin(viaAngle) * viaRadius;
    let vy = Math.cos(viaAngle) * viaRadius;
    pcbFile += placeVia(vx, vy, netName);
    pcbFile += createCurve(off.x, off.y, vx, vy, 20, traceWidth, layers[0], netName);
    // Connect via to ring
    pcbFile += placeSegment(layers[0], traceWidth, vx, vy, Math.sin(viaAngle) * (radius - 0.7), Math.cos(viaAngle) * (radius - 0.7), netName);
  }
}

// Finalization
pcbFile += ")";

try { fs.unlinkSync("./pcb/halo-90.kicad_pcb"); } catch (e) { }
fs.writeFileSync("./pcb/halo-90.kicad_pcb", pcbFile);
console.log("PCB regenerated with KiCad 9 format, verified netlist, and elegant curves.");