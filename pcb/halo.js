// Javascript program for creating a base template for charlieplexed led arrays
const fs = require("fs");

// Globals
var layers = ["Front", "In1.Cu", "In2.Cu", "Back"];
var radius = 11;
var numLed = 90;
var innerViaSpacing = 0.55
var clearence = 0.128;
var traceWidth = 0.128;

// Create a map of 
var charlieMap = [];
for(var i = 0 ; i < 10; i++){
  for(var j = 9 ; j >= 0; j--){
    if(i != j){
      charlieMap.push(j);
    }
  }
}

// Layer and track that coresponds to each signal
//  Track 0 is outermost and track 3 is inside
// Track 0 is under LEDs
layerMap = [{layer: layers[0],
              track: 1},
            {layer: layers[0],
              track: 2},
            {layer: layers[0],
              track: 3},
            {layer: layers[1],
              track: 0},
            {layer: layers[1],
              track: 1},
            {layer: layers[1],
              track: 2},
            {layer: layers[1],
              track: 3},
            {layer: layers[2],
              track: 0},
            {layer: layers[2],
              track: 2},
            {layer: layers[2],
              track: 3}
            ]

var maxTrack = 0;
for(layer in layerMap){
  if(layerMap[layer].track > maxTrack){
    maxTrack = layerMap[layer].track;
  }
}

// Empty string to start out pcb file
var pcbFile = header();
pcbFile += placeUc(0,0,0);
pcbFile += placeHook(0,-13,0);
pcbFile += placeBattery(1, 0, 0, 0);

console.debug("Hook, uC and Battery placed");

// Hardcoded, for now outline
pcbFile += "(gr_arc (start 0 0) (end 0 12) (angle 160) (layer Edge.Cuts) (width 0.05))"
pcbFile += "(gr_arc (start 0 0) (end 0 12) (angle -160) (layer Edge.Cuts) (width 0.05) )"
pcbFile += "(gr_arc (start 0 -13) (end -0.772467 -13.634562) (angle 101.100007) (layer Edge.Cuts) (width 0.05))"
pcbFile += "(gr_arc (start 6.75 -18.547) (end 4.104242 -11.276311) (angle 30.60227984) (layer Edge.Cuts) (width 0.05))"
pcbFile += "(gr_arc (start -6.75 -18.547) (end -4.104242 -11.276311) (angle -30.58992597) (layer Edge.Cuts) (width 0.05))"

// Place 90 0402 Leds in a ring arround 0,0 with a 12mm radius
var angle = 0;
for(var i = 0 ; i < numLed ; i ++){
    x=Math.round(Math.sin(angle * Math.PI/180) * radius * 10000)/10000;
    y=Math.round(Math.cos(angle * Math.PI/180) * radius * 10000)/10000;
    pcbFile += placeLED(i+1, x , y, angle - 90);
    // Increse angle step
    angle += 360/numLed;
}

console.debug(numLed + " LEDs Placed");

// Inner Via ring to breakout uC
var numBreakoutVias = 20;
for(var i = 0 ; i < numBreakoutVias ; i ++){
  x=Math.round(Math.sin(angle * Math.PI/180) * (radius-4.5) * 10000)/10000;
  y=Math.round(Math.cos(angle * Math.PI/180) * (radius-4.5) * 10000)/10000;
  pcbFile += placeVia(x, y);
  // Increse angle step
  angle += 360/numBreakoutVias;
}

console.debug(numBreakoutVias + " Breakout Vias Placed");

// Outer ring that connects the rows together
for(var i = 0 ; i < 10; i ++){
  var startArc = (i*9)*(360/numLed);
  var endArc = ((i*9)+8)*(360/numLed);
  var segments = endArc - startArc;
  pcbFile += createArc(radius+0.5,startArc,endArc, segments, 0.254, layers[0]);
}

console.debug("Row Connection outer arcs Placed");

// Inbetween ring
for(var i = 0 ; i < 10-1; i ++){
  // Connection Ring
  var arcStart = (((i*9)+8)*(360/numLed));
  var arcEnd = (((i+1)*9) + charlieMap.slice(i*9, (i*9)+9).indexOf(i+1))*(360/numLed);
  pcbFile += createArc(radius,arcStart,arcEnd,arcEnd-arcStart, traceWidth, layers[0]);
  // Start connection of inbetween ring
  x0= Math.round(Math.sin(arcStart * Math.PI/180) * (radius+0.5) * 10000)/10000;
  y0= Math.round(Math.cos(arcStart * Math.PI/180) * (radius+0.5) * 10000)/10000;
  x=  Math.round(Math.sin(arcStart * Math.PI/180) * (radius) * 10000)/10000;
  y=  Math.round(Math.cos(arcStart * Math.PI/180) * (radius) * 10000)/10000;
  pcbFile += placeSegment(layers[0], traceWidth, x0, y0 , x ,y);
  // End connection of inbetween ring
  x0= Math.round(Math.sin(arcEnd * Math.PI/180) * (radius-0.5) * 10000)/10000;
  y0= Math.round(Math.cos(arcEnd * Math.PI/180) * (radius-0.5) * 10000)/10000;
  x=  Math.round(Math.sin(arcEnd * Math.PI/180) * (radius) * 10000)/10000;
  y=  Math.round(Math.cos(arcEnd * Math.PI/180) * (radius) * 10000)/10000;
  pcbFile += placeSegment(layers[0], traceWidth, x0, y0 , x ,y);
}

console.debug("Row Connection mid arcs Placed");

// Bottom fixed postion between last and first led
pcbFile += createArc(radius,-1*(360/numLed),0*(360/numLed),(360/numLed), traceWidth, layers[0]);
x0= Math.round(Math.sin(-1*(360/numLed) * Math.PI/180) * (radius+0.5) * 10000)/10000;
y0= Math.round(Math.cos(-1*(360/numLed) * Math.PI/180) * (radius+0.5) * 10000)/10000;
x=  Math.round(Math.sin(-1*(360/numLed) * Math.PI/180) * (radius) * 10000)/10000;
y=  Math.round(Math.cos(-1*(360/numLed) * Math.PI/180) * (radius) * 10000)/10000;
pcbFile += placeSegment(layers[0], traceWidth, x0, y0 , x ,y);
x0= Math.round(Math.sin((0) * Math.PI/180) * (radius-0.5) * 10000)/10000;
y0= Math.round(Math.cos((0) * Math.PI/180) * (radius-0.5) * 10000)/10000;
x=  Math.round(Math.sin((0) * Math.PI/180) * (radius) * 10000)/10000;
y=  Math.round(Math.cos((0) * Math.PI/180) * (radius) * 10000)/10000;
pcbFile += placeSegment(layers[0], traceWidth, x0, y0 , x ,y);

console.debug("Row Connection terminations Placed");

// Inner Via ring to breakout 
for(var i = 0 ; i < numLed ; i ++){
  x=Math.round(Math.sin(angle * Math.PI/180) * (radius-1) * 10000)/10000;
  y=Math.round(Math.cos(angle * Math.PI/180) * (radius-1) * 10000)/10000;
  pcbFile += placeVia(x, y);
  // Increse angle step
  angle += 360/numLed;
}

console.debug("Inner Via Ring placed");

// Connect pad to breakout Via Segment
for(var i = 0 ; i < numLed ; i ++){
  x0= Math.round(Math.sin(angle * Math.PI/180) * (radius-0.5) * 10000)/10000;
  y0= Math.round(Math.cos(angle * Math.PI/180) * (radius-0.5) * 10000)/10000;
  x=  Math.round(Math.sin(angle * Math.PI/180) * (radius-1) * 10000)/10000;
  y=  Math.round(Math.cos(angle * Math.PI/180) * (radius-1) * 10000)/10000;
  pcbFile += placeSegment(layers[0], traceWidth, x0, y0 , x ,y);
  // Increse angle step
  angle += 360/numLed;
}

console.debug("Inner Via Ring connected");

// Place inner vias
for(var led = 0 ; led < charlieMap.length; led++ ){
  // Vias not needed for track 0
  if(layerMap[charlieMap[led]].track != 0){
    angle = led * (360/numLed);
    ofsett= (layerMap[charlieMap[led]].track*innerViaSpacing) + 1;
    x=Math.round(Math.sin(angle * Math.PI/180) * (radius-ofsett) * 10000)/10000;
    y=Math.round(Math.cos(angle * Math.PI/180) * (radius-ofsett) * 10000)/10000;
    pcbFile += placeVia(x, y);
    // Connect to Inner Vias
    x0= Math.round(Math.sin(angle * Math.PI/180) * (radius-ofsett) * 10000)/10000;
    y0= Math.round(Math.cos(angle * Math.PI/180) * (radius-ofsett) * 10000)/10000;
    x=  Math.round(Math.sin(angle * Math.PI/180) * (radius-1) * 10000)/10000;
    y=  Math.round(Math.cos(angle * Math.PI/180) * (radius-1) * 10000)/10000;
    pcbFile += placeSegment(layers[3], traceWidth, x0, y0 , x ,y);
  }
}

console.debug("Inner Signal Vias Placed");

// Innner signal tracks
for(var led = 0 ; led < charlieMap.length; led++ ){
  if(layerMap[charlieMap[led]].track != 0 && layerMap[charlieMap[led]].track != maxTrack){
    // Find the previous via on the innermost track
    var angle = 0;
    for(var scan = led ; scan < charlieMap.length; scan++){
      if(layerMap[charlieMap[scan]].layer == layerMap[charlieMap[led]].layer){
        if(layerMap[charlieMap[scan]].track > layerMap[charlieMap[led]].track){
          angle = scan * (360/numLed);
          // Offsett based on number of tracks
          angle -= (maxTrack-layerMap[charlieMap[led]].track) * (360/numLed)
          scan = charlieMap.length+1;
        }
      }
      // Treat as circular buffer
      if(scan == charlieMap.length-1){
        scan = 0;
      }
    }
    
    endAngle = led * (360/numLed);
    if(angle-endAngle > 0){
      pcbFile += createArc(radius - ((1+(innerViaSpacing*(maxTrack+1)))+((layerMap[charlieMap[led]].track)*2*clearence)),angle,endAngle,angle-endAngle, traceWidth, layerMap[charlieMap[led]].layer);  
    } else {
      endAngle = -1*(360-endAngle);
      pcbFile += createArc(radius - ((1+(innerViaSpacing*(maxTrack+1)))+((layerMap[charlieMap[led]].track)*2*clearence)),endAngle, angle, angle-endAngle, traceWidth, layerMap[charlieMap[led]].layer);  
    }
  }
}

console.debug("Inner Signal tracks connected");

// Track 0 goes under leds and has no breaks, innermost track 3 has no breaks
for(layer in layerMap){
  if(layerMap[layer].track == 0){
    pcbFile += createArc(radius,0,360,360, traceWidth, layerMap[layer].layer);  
  }
  if(layerMap[layer].track == maxTrack){
    pcbFile += createArc(radius - ((1+(innerViaSpacing*(maxTrack+1)))+((layerMap[layer].track)*2*clearence)),0,360,360, traceWidth, layerMap[layer].layer);
  }
}

// Connect to under led ring
for(var led = 0 ; led < charlieMap.length; led++ ){
  // Only for track 0
  if(layerMap[charlieMap[led]].track == 0){
    angle = led * (360/numLed);
    x0= Math.round(Math.sin(angle * Math.PI/180) * (radius) * 10000)/10000;
    y0= Math.round(Math.cos(angle * Math.PI/180) * (radius) * 10000)/10000;
    x=  Math.round(Math.sin(angle * Math.PI/180) * (radius-1) * 10000)/10000;
    y=  Math.round(Math.cos(angle * Math.PI/180) * (radius-1) * 10000)/10000;
    pcbFile += placeSegment(layerMap[charlieMap[led]].layer, traceWidth, x0, y0 , x ,y);
  }
}

// Connect Signal Vias to signal ring
for(var led = 0 ; led < charlieMap.length; led++ ){
  if(layerMap[charlieMap[led]].track != 0){
    angle = led * (360/numLed);
    ofsett= (layerMap[charlieMap[led]].track*innerViaSpacing) + 1;
    x0= Math.round(Math.sin(angle * Math.PI/180) * (radius-ofsett) * 10000)/10000;
    y0= Math.round(Math.cos(angle * Math.PI/180) * (radius-ofsett) * 10000)/10000;
    // Inner ring
    x=  Math.round(Math.sin(angle * Math.PI/180) * (radius - ((1+(innerViaSpacing*(maxTrack+1)))+((layerMap[charlieMap[led]].track)*2*clearence))) * 10000)/10000;
    y=  Math.round(Math.cos(angle * Math.PI/180) * (radius - ((1+(innerViaSpacing*(maxTrack+1)))+((layerMap[charlieMap[led]].track)*2*clearence))) * 10000)/10000;
    pcbFile += placeSegment(layerMap[charlieMap[led]].layer, traceWidth, x0, y0 , x ,y);

  }
}

// Create Arcs up to the ring
for(var led = 0 ; led < charlieMap.length; led++){
  if(layerMap[charlieMap[led]].track != maxTrack && layerMap[charlieMap[led]].track != 0){

      // Scan in to angle of maxTrackVia
      var arcEnd    =  led*(360/numLed);
      var arcStart  =  (led-(1*(maxTrack-layerMap[charlieMap[led]].track))) * (360/numLed);
      var ofsett= (layerMap[charlieMap[led]].track*innerViaSpacing) + 1;
      var dist = 0.5;

    pcbFile += createArc(radius-ofsett,arcStart,arcEnd,arcEnd-arcStart, traceWidth, layerMap[charlieMap[led]].layer);

//     ofsett= ((charlieMap[led] % 4)*innerViaSpacing) + 1 + innerViaSpacing;
//     x0= Math.round(Math.sin(arcStart * Math.PI/180) * (radius-ofsett - dist) * 10000)/10000;
//     y0= Math.round(Math.cos(arcStart * Math.PI/180) * (radius-ofsett - dist) * 10000)/10000;
//     x=  Math.round(Math.sin(arcStart * Math.PI/180) * (radius-ofsett) * 10000)/10000;
//     y=  Math.round(Math.cos(arcStart * Math.PI/180) * (radius-ofsett) * 10000)/10000;
//     pcbFile += placeSegment(layerMap[charlieMap[led]].layer, traceWidth, x0, y0 , x ,y);



//     // Connect each arc back up to ring
//     var heading = ((led+7)*(360/numLed));
//     x =  Math.round(Math.sin(heading * Math.PI/180) * 6 * 10000)/10000;
//     y =  Math.round(Math.cos(heading * Math.PI/180) * 6 * 10000)/10000;

//     switch (charlieMap[led]%4) {
//       case 0:
//         var startAngle = heading - 103;
//         var endAngle   = heading - 73;
//         var distance = 6 - clearence;
//         pcbFile += createNonCenteredArc(x, y, distance, startAngle, endAngle, endAngle-startAngle, traceWidth, layerMap[charlieMap[led]].layer);
//         break;

//       case 1:
//         var startAngle = heading - 103;
//         var endAngle   = heading - 75;
//         var distance = 6 - 2 * clearence;
//         pcbFile += createNonCenteredArc(x, y, distance, startAngle, endAngle, endAngle-startAngle, traceWidth, layerMap[charlieMap[led]].layer);
//         break;
    
//       default:
//         break;
//     }
  }
}



// End the file with a paren
pcbFile += ")"
// Delete old file if exists
try{
  fs.unlinkSync("./halo-90.kicad_pcb");
} catch {}

fs.writeFileSync("./halo-90.kicad_pcb", pcbFile);

// Functions to create and place objects
function createArc(radius, startAngle, endAngle, segments, thickness, layer){
    var arc="";
    for (var i = 0; i < segments; i++) {
        x0 = Math.round(Math.sin((startAngle + (i * (endAngle - startAngle) / segments)) * Math.PI / 180) * radius * 10000) / 10000;
        y0 = Math.round(Math.cos((startAngle + (i * (endAngle - startAngle) / segments)) * Math.PI / 180) * radius * 10000) / 10000;
        x = Math.round(Math.sin((startAngle + ((i + 1) * (endAngle - startAngle) / segments)) * Math.PI / 180) * radius * 10000) / 10000;
        y = Math.round(Math.cos((startAngle + ((i + 1) * (endAngle - startAngle) / segments)) * Math.PI / 180) * radius * 10000) / 10000;
        arc+= placeSegment(layer, thickness, x0, y0, x, y);
    }
    return arc;
}

function createNonCenteredArc(x, y, radius, startAngle, endAngle, segments, thickness, layer){
  var arc="";
  for (var i = 0; i < segments; i++) {
      x0 = Math.round(Math.sin((startAngle + (i * (endAngle - startAngle) / segments)) * Math.PI / 180) * radius * 10000) / 10000;
      y0 = Math.round(Math.cos((startAngle + (i * (endAngle - startAngle) / segments)) * Math.PI / 180) * radius * 10000) / 10000;
      xf = Math.round(Math.sin((startAngle + ((i + 1) * (endAngle - startAngle) / segments)) * Math.PI / 180) * radius * 10000) / 10000;
      yf = Math.round(Math.cos((startAngle + ((i + 1) * (endAngle - startAngle) / segments)) * Math.PI / 180) * radius * 10000) / 10000;
      arc+= placeSegment(layer, thickness, x0+x, y0+y, xf+x, yf+y);
  }
  return arc;
}

function placeSegment(layer, width, x0, y0 , x ,y){
    var segment = "(segment (start " + x0 + " " + y0 + ") (end " + x + " " + y + ") (width " + width + ")  (layer " + layer + "))  \n";
    return segment;
}

function placeVia(x, y){
  var via = "(via (at "+ x + " " + y + ") (size 0.45) (drill 0.2) (layers Front Back))  \n";
  return via;
}

function placeLED(ref, x , y, rot){
    var ledModule = "\
    (module BL-HUB37A-AV-TRB:D-0402 (layer Front)                         \n\
      (at "+ x + " " + y + " " + rot + ")                                 \n\
      (fp_text value LED (at -4 0 "+ rot +") (layer F.SilkS)              \n\
      (effects (font (size 0.25 0.25) (thickness 0.05))))                 \n\
      (fp_text reference D" + ref + " (at 5 0 "+ rot +") (layer F.SilkS)  \n\
      (effects (font (size 0.25 0.25) (thickness 0.05))))                 \n\
    )                                                                     \n\
    "                                                                                    
    return ledModule
}

function placeBattery(ref, x, y, rot){
  var batModule = "\
  (module BAT-HLD-001:BAT-HLD-001-HALO (layer Back)                       \n\
    (at "+ x + " " + y + " " + rot + ")                                   \n\
    (fp_text value CR2032 (at 0 0 "+ rot +") (layer B.SilkS)              \n\
    (effects (justify mirror) (font (size 0.25 0.25) (thickness 0.05))))  \n\
    (fp_text reference BT" + ref + " (at 0 0 "+ rot +") (layer B.SilkS)   \n\
    (effects (justify mirror) (font (size 0.25 0.25) (thickness 0.05))))  \n\
  )                                                                       \n\
  "  
  return batModule
    
}

function placeUc(x , y, rot){
  var ucModule = "\
  (module STM8L15xxx:UFQFPN28 (layer Front)                      \n\
  (at "+ x + " " + y + " " + rot + ")                            \n\
  (fp_text reference U1 (at 0 0) (layer F.SilkS)                 \n\
    (effects (font (size 1 1) (thickness 0.1)))                  \n\
  )                                                              \n\
  (fp_text value STM8L15xxx (at 0 0) (layer Dwgs.User)           \n\
    (effects (font (size 1 1) (thickness 0.1)))                  \n\
  )                                                              \n\
  (fp_text user %R (at 0 0) (layer F.Fab)                        \n\
    (effects (font (size 1 1) (thickness 0.1)))                  \n\
  )                                                              \n\
)"
  return ucModule;
}



function placeHook(x , y, rot){
 var hookModule = "\
 (module earringHookWire:earringHookWire (layer Front)            \n\
 (at "+ x + " " + y + " " + rot + ")                              \n\
  (fp_text reference H1 (at 0 -5) (layer F.SilkS)                 \n\
    (effects (font (size 1 1) (thickness 0.15))))                 \n\
  (fp_text value earringHookWire (at 0 -1.6) (layer Dwgs.User)    \n\
    (effects (font (size 1 1) (thickness 0.15))))                 \n\
  )                                                               \n\
 "
 return hookModule;
}

function header(){
var header = "\
  (kicad_pcb (version 20171130) (host pcbnew \"(5.1.2)-2\")            \n\
                                                                       \n\
  (general                                                             \n\
    (thickness 1)                                                      \n\
    (drawings 0)                                                       \n\
    (tracks 2)                                                         \n\
    (zones 0)                                                          \n\
    (modules 1)                                                        \n\
    (nets 3)                                                           \n\
  )                                                                    \n\
                                                                       \n\
  (page USLetter)                                                      \n\
  (title_block                                                         \n\
    (rev 1)                                                            \n\
  )                                                                    \n\
                                                                       \n\
  (layers                                                              \n\
    (0 Front signal)                                                   \n\
    (1 In1.Cu signal)                                                  \n\
    (2 In2.Cu signal)                                                  \n\
    (31 Back signal)                                                   \n\
    (34 B.Paste user)                                                  \n\
    (35 F.Paste user)                                                  \n\
    (36 B.SilkS user)                                                  \n\
    (37 F.SilkS user)                                                  \n\
    (38 B.Mask user)                                                   \n\
    (39 F.Mask user)                                                   \n\
    (40 Dwgs.User user hide)                                           \n\
    (41 Cmts.User user hide)                                           \n\
    (44 Edge.Cuts user)                                                \n\
    (45 Margin user hide)                                              \n\
    (46 B.CrtYd user hide)                                             \n\
    (47 F.CrtYd user hide)                                             \n\
    (48 B.Fab user)                                                    \n\
    (49 F.Fab user)                                                    \n\
  )                                                                    \n\
                                                                       \n\
  (setup                                                               \n\
    (last_trace_width 0.127)                                           \n\
    (user_trace_width 0.254)                                           \n\
    (user_trace_width 0.508)                                           \n\
    (user_trace_width 0.762)                                           \n\
    (trace_clearance 0.127)                                            \n\
    (zone_clearance 0.508)                                             \n\
    (zone_45_only no)                                                  \n\
    (trace_min 0.127)                                                  \n\
    (via_size 0.45)                                                    \n\
    (via_drill 0.2)                                                    \n\
    (via_min_size 0.45)                                                \n\
    (via_min_drill 0.02)                                               \n\
    (user_via 0.45 0.2)                                                \n\
    (user_via 0.889 0.381)                                             \n\
    (uvia_size 0.6858)                                                 \n\
    (uvia_drill 0.254)                                                 \n\
    (uvias_allowed no)                                                 \n\
    (uvia_min_size 0)                                                  \n\
    (uvia_min_drill 0)                                                 \n\
    (edge_width 0.0381)                                                \n\
    (segment_width 0.254)                                              \n\
    (pcb_text_width 0.3048)                                            \n\
    (pcb_text_size 1.524 1.524)                                        \n\
    (mod_edge_width 0.127)                                             \n\
    (mod_text_size 0.762 0.762)                                        \n\
    (mod_text_width 0.127)                                             \n\
    (pad_size 1.524 1.524)                                             \n\
    (pad_drill 0.762)                                                  \n\
    (pad_to_mask_clearance 0.0508)                                     \n\
    (aux_axis_origin 0 0)                                              \n\
    (visible_elements 7FFFFFFF)                                        \n\
    (pcbplotparams                                                     \n\
      (layerselection 0x010fc_ffffffff)                                \n\
      (usegerberextensions false)                                      \n\
      (usegerberattributes false)                                      \n\
      (usegerberadvancedattributes false)                              \n\
      (creategerberjobfile false)                                      \n\
      (excludeedgelayer true)                                          \n\
      (linewidth 0.152400)                                             \n\
      (plotframeref false)                                             \n\
      (viasonmask false)                                               \n\
      (mode 1)                                                         \n\
      (useauxorigin false)                                             \n\
      (hpglpennumber 1)                                                \n\
      (hpglpenspeed 20)                                                \n\
      (hpglpendiameter 15.000000)                                      \n\
      (psnegative false)                                               \n\
      (psa4output false)                                               \n\
      (plotreference true)                                             \n\
      (plotvalue false)                                                \n\
      (plotinvisibletext false)                                        \n\
      (padsonsilk false)                                               \n\
      (subtractmaskfromsilk true)                                      \n\
      (outputformat 1)                                                 \n\
      (mirror false)                                                   \n\
      (drillshape 0)                                                   \n\
      (scaleselection 1)                                               \n\
      (outputdirectory \"./gerbers\"))                                 \n\
  )                                                                    \n\
                                                                       \n\
  (net 0 \"\")                                                         \n\
                                                                       \n\
  (net_class Default \"This is the default net class.\"                \n\
    (clearance 0.127)                                                  \n\
    (trace_width 0.127)                                                \n\
    (via_dia 0.45)                                                     \n\
    (via_drill 0.2)                                                    \n\
    (uvia_dia 0.6858)                                                  \n\
    (uvia_drill 0.254)                                                 \n\
    (diff_pair_width 0.1524)                                           \n\
    (diff_pair_gap 0.1524)                                             \n\
  )"
  return header;
}