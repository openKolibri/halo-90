![wornDynamic]

# HALO-90
A distinct ring of light. An ethereal glow. Patterns that ebb and flow to the music. Ninety lights. All controllable. Powered by a common coin cell. An engrossing look with retro vibes with a modern touch. This is Halo-90.

![presentedCase]

The *HALO* product series, in which these earrings (Halo-90) are the first item, is a fully open source electronic jewelery line. It is designed with elegance and wearability in mind. `90` refers to the ninety individually controllable LEDs on the earring face. The built-in compute power is also suitable for creating complex light shows.

This is the technical manual for anyone wanting to modify, hack, remix, or program their own light patterns onto the earrings. The manual goes into fine detail about construction, assembly, and firmware which should encompass all basic knowledge.

## Table of Contents

* [Design](#design)
  + [Hardware](#hardware)
    - [LEDs](#leds)
    - [Microcontroller](#microcontroller)
    - [Battery](#battery)
    - [Microphone](#microphone)
    - [Button](#button)
    - [IMU](#imu)
    - [Passives](#passives)
    - [Alignment Pins](#alignment-pins)
    - [Connectors](#connectors)
  + [Case](#case)
    - [FDM Printed](#fdm-printed)
    - [Cast](#cast)
      * [Mould Master](#mould-master)
      * [Silicone Mould](#silicone-mould)
    - [Stone Casting](#stone-casting)
    - [Magents](#magents)
* [Firmware](#firmware)
  + [Modes](#modes)
    - [Dynamic](#dynamic)
    - [Sparkle](#sparkle)
    - [Halo](#halo)
    - [Power Managment](#power-managment)
  + [LED control](#led-control)
  + [Compiling](#compiling)
    - [Ubuntu/WSL](#ubuntu-wsl)
    - [Arch](#arch)
  + [Flashing](#flashing)
    - [Windows](#windows)
    - [Linux](#linux)
* [Absolute Maximum Ratings](#absolute-maximum-ratings)
* [Gaurenteed Ratings](#gaurenteed-ratings)
* [Manufacturing](#manufacturing)
  + [BOM](#bom)
  + [PCB](#pcb)
  + [PCB Assembly](#pcb-assembly)
    - [Assembly Detail Pictures](#assembly-detail-pictures)
    - [Potential Alternates](#potential-alternates)
  + [Physical Assembly](#physical-assembly)
  + [Production Scaling](#production-scaling)
    - [Electronics](#electronics)
    - [Case](#case-1)
* [Programer](#programer)
* [Artwork](#artwork)
* [Inventory and QC](#inventory-and-qc)
* [Packaging](#packaging)
* [Shipping](#shipping)
* [Safety](#safety)
* [Certifications](#certifications)
* [Liecence](#liecence)
* [Attribution](#attribution)
  + [Fonts](#fonts)
  + [Libraries](#libraries)
* [ToDo](#todo)

## Design
Design was always a core objective from the very beginning. It *has* to look good. Even more importantly, it must also be functional, as it is a piece of jewelery people are going to wear. If it looks too complex or it's too difficult to use, no one will want to wear it. Comfort was also major goal, as heavy earrings are painful to wear for extended amounts of time, no matter how stunning they are. It's simply not worth the pain.

![render]

The design had to allow for a variety of LED patterns with a range from subtle to attention grabbing. Sensors were added to make light patterns more personal and reactive to the local environment, such as the muted audio responsive pattern. At one moment, befit for a quiet restaurant and later a flashy wide pattern at a concert.

| Audio        | Halo        | Sparkle        |
| ------------ | ----------- | -------------- |
| ![PAT-audio] | ![PAT-halo] | ![PAT-sparkle] |


### Hardware
The electronics are kept minimal for cost reduction and manufacturing simplicity with pads and routing done for all but the IMU and its pullup resistors. They are not mounted, because there is no firmware support for that and it yields a lower cost variant. The design is done in [KiCad] 5.99 (nightly) and will be ported and set in the stable version. All components and libaries are embedded in the project. 

![IMG-schematic]

Also available as a [pdf][PDF-schematic]. The layout is done partially programatically using text manipulation and template stamping using javascript and node. The code is available [here][nodeHaloBuilding].

#### LEDs
There are 90 LEDs that make up the ring, All are regular `0402` red diodes. All the cathodes (K/-) face towards the center of the board, and are placed at `4°` intervals. The LEDS are charlieplexed with ten lines providing individual control. They are run at as high of a current as the battery's internal resistance and GPIO max current allows, so no resistors are used. The red LEDs, with their `2.0 V` - `2.6 V` forward voltage, permits maximizing battery usage
We are using [BL-HUB37A-AV-TRB], because it is low cost and has high availabilty across multiple vendors in China, but any `0402` LED with a V<sub>f</sub> below `2.7 V`, should yield an equivalent battery life.

![IMG-BL-HUB37A-AV-TRB]

#### Microcontroller
*STMicroelectonics's* [STM8L151G4] acts as the main controller for the earring. The low power microcontroller has a wide range of peripherals, a long expected production life, and low cost and availability in high quantities. Running at its max speed of `16 Mhz` is able to easily charliplex the 90 Leds at over `1 kHz`. The `12b ADC` is used to readout the microphone and has plenty of flash (up to `32k`) to store an assortment of light patterns or complex processed designs.

![IMG-STM8L15xxx]

#### Battery
*Linx Technologies's* aptly named [BAT-HLD-001] is a stamped die-cut sheet-metal battery-holder that is as low profile as possible. It is sized for a `CR2032` lithium cell. The metal acts as the anode while the pad on the PCB is the cathode. Battery life varies based on what threshold for brightness you are content with. Over its lifetime, the internal resistance of the battery increases and the voltage decreases. This means that the current possible also decreases and thus the brightness.

![IMG-BAT-HLD-001]

#### Microphone
The microphone selection was rather difficult due to a lack of specificaitons available with amplified MEMS microphones. *Knowles* [SPW2430HR5H-B] was selected, as it seemed reasonable and could be tested with Adafruit's breakout board fairly easily.

![IMG-SPW2430HR5H-B]

Using a built-in amplified MEMS microphone decreases the number of component placements and simplifies layout and verificiton, at the expense of not having instrumentation knowledge of your audio response.

#### Button
*C&K* [KXT3] series provides ultra low profile miniature tactile switches and we chose `KXT311LHS`, with a low actuation force of `100g`. It can be easily pressed with the edge of your nail, or a bit less comfortably with the back.

The button provides the functionality of changing light patterns by pressing and triggering it into a low power sleep mode by holding for `500 ms`. Completely available as an interupt to the microcontroller, so other uses can be implemented. 

![IMG-KXT3]

#### IMU
[LSM6DSM] is a 6DOF IMU with a three-axis accelerometer and three-axis gyroscope by *STMicroelectronics*. It communicates over I2C and is connected to the hardware I2C peripheral in the microcontroller. It allows a fast data stream at very low power. It also has additional low power modes and the ability to wake the main microcontroller over interupt with the routed interupt pin. 

![IMG-LSM6DSM]

#### Passives
There are three passives (five if the IMU is populated) that can be of any tolerance. There are two `1 uF` and `0.1 uF` decoupling capacitors and one `10k` pullup on the reset of the processor. The tow IMU pullups are `10k` I2C pullups.

#### Alignment Pins
There are four tooling holes/alignment pins that can be used for addons. They are `1.152 mm` (`45.35 mil`) in diameter and are placed `2.8 mm` and `5.5 mm` from the center.

![alignmentPins]

 The earring is `24 mm` in diameter, has a mass of `5.207 g` (`2.135 g` without the battery) and the top eylet extends `2 mm` extra, yeilding a bounding box of `24 mm x 26 mm x 6.36 mm`.

![mass]

#### Connectors
The only connectors on the board are six `⌀ 1 mm` copper circles that are exposed as contacts for spring pins. They are placed evenly across the board so it receives balanced forces from the custom programmer (or testing jig).

| Labels      | Dimentions       |
| ----------- | ---------------- |
| ![pgrmPads] | ![pgrmPlacement] |

The labeled pins have their descriptions in the table below.

| Pin  | Description                      |
| ---- | -------------------------------- |
| 3V0  | Connected to Batt+ and power net |
| GND  | Connectedto ground net           |
| TX   | GPIO PB2 for serial out          | 
| RX   | GPIO PB4 for serial in           | 
| RST  | Active low, 10k pullup           | 
| SWIM | Programing interface             | 

### Case
The case holds the earrings and two cells in its cavities. This allows for at least 36 hours of runtime available and organized. The earring case consists of two pieces held together with magnets. The cavities inside the case hold all components securely so that they do not rattle. The earrings display beautifully when the case is opened.

![cases]

The case is designed in CAD and made to house two earrings (with or without batteries inserted) as well as two additional batteries. All of the edges are filleted and the closed case is comfortable when held. One corner is chamfered which makes it easier to align both pieces together in the correct orientation, and the magnets are oriented to resist trying to close it whenever the directionality does not match. This provides a very satisfying tactile click when they align and close.

![caseRender]

#### FDM Printed
The first sets of cases are made of 3D printed plastic PLA. The top and bottom have bold contrasting colors that uniquely identify the brand. They are printed with a `20%` gyroid infill and at `200 um` layer height. The 3D models must be scaled up to `100.2%` to account for PLA shrinkage. 

![caseFDM]

#### Cast
A pebble or smooth seashell-like finish provides an air of luxury, with the distinctive material and style, which emboldens the brand. 

![caseBatch]

##### Mould Master
Moulding masters were made using the same 3D printed designs, printed at an `80 um` layer height, and then repeatedly filled, primed, and sanded with `P400` to `P3000` grit sandpaper on top of a glass plate to keep the straight faces square. This fills all of the air gaps which allows for a very smooth finish. It is then buffed to a shine with nail buffer sponges.

![caseMaster]

##### Silicone Mould
A platinum cure shore `A20` silicone rubber (Troll Factory [TYP-1]) is used to make a mould of the master. This has an accuracy of `~2 um` so it's able to reproduce a surface finish. Since the back is flat, an open-faced mould is made. The part, as well as the walls, are held in place with [Quakehold] museum wax, and mixing the exact amount of silicone needed (based on a CAD model) means a high yield. After curing, the mould box walls are cut off and the master is demoulded, yielding a silicone mould.

![caseMould]

#### Stone Casting
The mould can be cast with various materials, including other silicones, polyuretane, rubbers, and resins. In our case, we used plaster. We are using high compressive strength *Ernst Hinrichs* [Sockelgips FL] `Type-4 low expansion dental plaster`. These plasters have a thixotropic agent that allows them to flow better, which means that they mix thinner and reproduce in finer detail. Applying a surfactant to the mould, mixing with distilled water, and using a vibrating table produces castings with fewer bubbles. The back is roughly leveled off and set to cure. The part is demoulded and the back is hand finished with `P320-P3000` grit sandpaper.

![caseCasting]

After 24 hours, the part has reached its final hardness and has dried out completely. It can then be processed further with magents and dye. 

**Testing, process optimization and verification is still in progress.**

#### Magents
The magents are `6 mm x 1 mm` `N52` neodynium magnets that are glued in with UHU Max Repair Extreme adhesive. They are coated in `Ni-Cu-Ni`, at around `12 um`. The magnets are a very tight fit in their countersinks and are glued in to fit securely. They have a fixed orientation between both parts made, so swapping tops or bottoms with other sets is possible. There are few adhesives that work well when bonding two materials together that are already difficult to glue.

![6x1-magents]

Magnets are installed using a gluing jig. One jig for the top and one for the bottom. They have magnets that are installed, as well as a matching chamfer which prevents the wrong part from being placed or placing the part backwards by accident. 

![magnetsGlued]

The two jigs are color-coded as well. Adhesive is dispensed into the wells and the magnet is dropped, which automatically orients itself with the jig magnet. It can then be removed and set to cure.

![magnetJig]

## Firmware
The firmware is coded at the register level in C. The code is fully interupt-based and performs quite efficently. The toolchain is simple and built on open source tools. This makes it harder to code, but allows for significant optimization.
 
### Modes
There are multiple modes available on the halo earring that can be switched through when pressing the button. Each press of the button cycles to the next mode, eventually circling back around.

| Audio         | Halo        | Sparkle        |
| ------------- | ----------- | -------------- |
| ![GIF-audio]  | ![GIF-halo] | ![GIF-sparkle] |

#### Dynamic
Boot mode is the audio based dynamic mode. During every ADC cycle it reads the analog value and projects the audio waveform amplitude, based around a moving point on the ring with wrap around.

![PWR-audio]

Power profile readings show no correlation with audio level, and an `11.71 mA` power consumption with `105 uA` standard deviation. Projected battery life with a `220 mA` CR2032 cell is ~18.8 hours.

#### Halo
In the HALO mode, the entire light ring is lit. This is done through interlacing the LEDs lighting up. The deep sleep auto wakeup timer is set to wake up every two clock cycles of the low speed 32kHz oscillator. On every wake, it changes the led to the 13th following LED, looping around at 90.

```c
setLed((prevLed + 13)%90);
```

This allows for a cleaner and more consistent scan over the entire halo ring since 13 is the greatest integer factor (besides 91), for all of the LEDs to be turned on evenly around the ring. Greater spacing causes *frames* to interlace with one another, resulting in a cleaner visual experience.

![PWR-halo]

Power profile readings show a `10.88 mA` power consumption with `60 uA` standard deviation. Projected battery life with a `220 mA` CR2032 cell is ~20.2 hours.

#### Sparkle
Sparkle mode is the best for minimal power draw and is implemented in a single line of code. At `~320 hz`, the procesor wakes from deep sleep and runs the selection of which LED to light (if any).

```c
rand()%15 ? ledLow(prevLed) : setLed(rand() % 90);
```

Given a 1/15 chance, a random LED will light up. Othwerwise, any previously lit LEDs will be turned off. This results in a more visually pleasing pattern over just randomly lighting LEDs, which produce sharper bursts of light. Since the processor is only awake `0.002%` of the time, and the LED has a chance of being on only `6.6%` of the time, the power consumption is quite minimial. 

![PWR-sparkle]

Power profile readings show a `2.01 mA` power consumption with `327 uA` standard deviation. Projected battery life with a `220 mA` CR2032 cell is ~109.5 hours, (over 4.5 days).

#### Power Managment
Pressing and holding the button for `500 ms` will turn off all LEDs and put the cpu into deep sleep mode. In this mode the current draw is around `15 uA` and the only wake interupt is the button press.

![GIF-boot]

Pressing and holding the button will show a boot-up animation that lights up in a ring around the face. Holding the button until it makes a full revolution, about one second, will trigger a software reset of the halo turning it back on.

### LED control
Since the LEDs are configured in a charlieplex array, only one LED can be on at a time. Some optimization can allow multiple LEDs to light simultaneously, but at a cost of consistency in brightness and power draw. There are two low-level functions that can turn a individual LED on or off, as well as a helper function that remembers the last LED and turns it off before turning on the next one.

```c
void setLed(uint8_t led);
void ledHigh(uint8_t led);
void ledLow(uint8_t led);
```

The function to get the column and row from the LED number is duplicated below.
```c
uint8_t col = led / 9;
uint8_t topElements = 9 - col;
uint8_t row = 9 - (led % 9);
if (topElements <= (9 - row)) {
  row--;
}
```

To turn the LEDs off, the column and row are both set to high impedence. To turn it on, the column is set high and the row is set low.

The previous LED *must* be turned off before lighting up the next LED or else there is a risk of damaging the electronics. It is recommended to only use the `setLed` and `ledLow` functions.

### Compiling
Compiling is done with the SDCC (small device c compiler) and the included makefile. 

Steps, as an example, are given below for some systems but should easily be transferable to the distro of your choosing. The requirements are `make` and `sdcc`. They should both be available in the path. Once installation is completed, running `make` will generate the `halo.ihx` file, which is the binary to be flashed.

```bash
make
```

#### Ubuntu/WSL
```bash
sudo apt install -y make
sudo apt install -y sdcc
```
#### Arch
```bash
sudo pacman -S make
sudo pacman -S sdcc
```

### Flashing
A flashing software is required, along with a programmer that can program over the `SWIM` protocol. We are using third party `STLink-V2` clones, because the form factor of the genuine programmer is difficult to use and newer programmers do not support `SWIM`.

#### Windows
`STVP_CmdLine` is required as the flashing software and comes with the software package [ST Visual Programer](https://www.st.com/en/development-tools/stvp-stm32.html). This needs to be installed and `c/Program Files (x86)/STMicroelectronics/st_toolset/stvp/STVP_CmdLine.exe` needs to be added into the path.

Once it's installed, it can be run with the following flags. Preferably in WSL but should also be possible in CMD or PS.

```bash
STVP_CmdLine.exe -Device=STM8L15xG4 -FileProg=halo.ihx -verif -no_loop -no_log
```

It is also incorporated into the make file and can be run with the following:

```bash
make flash
```

#### Linux
[STM8Flash](https://github.com/vdudouyt/stm8flash) is an open source `SWIM` compatible flashing utililty for linux. The tool is built from source. Short instructions are written below.

```bash
git clone https://github.com/vdudouyt/stm8flash.git
cd stm8flash
make
sudo make install
```

Flashing can then be done with the following command:

```bash 
stm8flash -cstlink -pstm8l151 -w halo.ihx
```

It is also incorporated into the make file and can be run with the following:

```bash
make flash
```

## Absolute Maximum Ratings

| Parameter         | Min  | Max | Unit |
| ----------------- | ----:| ---:| ---- |
| Battery Voltage   | -0.3 | 3.6 | Volt |
| Temperature       |  -40 |  85 | °C   |
| Power Draw        |  15u | 25m | Amp  |

## Gaurenteed Ratings

| Parameter              | Min | Max | Unit |
| ---------------------- | ---:| ---:| ---- |
| Battery Voltage        | 1.8 | 3.6 | Volt |
| Operating Temperature  | -20 |  50 | °C   |
| Storage Temperature    | -40 |  85 | °C   |

The earrings should be fine to leave in a hot car (although the 3D printed plastic case could warp). If you are outside of these ratings, take care of yourself, you are either freezing or at risk of heat stroke. The earrings will be fine.

## Manufacturing
Although taking on novel uses of materials, the ability to manufacture at scale was always a primary focus. Parts were selected with strong supply chains and alternatives. Layout was designed with generous rules to accomodate for as many fabs as possible and the number of components, while unique components were minimized. The microphone and battery holder are from single vendors but they have proven track records and well-known supply chains. Alternatives to be tested are still proposed.

### BOM
The BOM was selected with parts that are common to the high-volume Chinese manufacturing market, have strong supply chains, and have many alternatives available in case a supplier stops manufacture or supply dips occur. The number of unique parts was kept to a minimum and the maximum amount of features can be implemented with "free" options, like SMD pads. The table of BOM is shown below.

| REF    | QTY | Manufacturer              | MPN              | Description                         |
| ------ | ---:| ------------------------- | ---------------- | ----------------------------------- |
| D1-D90 |  90 | Brightled                 | BL-HUB37A-AV-TRB | LED: RED 627-637nm 50mcd@20mA 0402  |
| U1     |   1 | STMicroelectronics        | STM8L151G4U6     | MCU: 8b 16Mhz 16k Flash UQFN-28-4x4 |
| MK1    |   1 | Knowles Electronics       | SPW2430HR5H-B    | MIC: Omni Si-Sonic 3.1x2.5x1.0mm    |
| BT1    |   1 | Linx Technologies         | BAT-HLD-001      | BAT: CR2032 Cell Holder             |
| S1     |   1 | C&K                       | KXT311LHS        | SW: Low Profile 3x2x0.6mm 100gf     |
| R3     |   1 | Uniroyal                  | 0402WGF1002TCE   | RES: 10k 5% 1/16W 0402              |
| C1     |   1 | Samsung Electro-Mechanics | CL05B104KO5NNNC  | CAP: MLCC 100nF 16V 0402            |
| C2     |   1 | Samsung Electro-Mechanics | CL05A105KA5NQNC  | CAP: MLCC 1uF 25V 0402              |

The [csv][BOMcsv] is provided with the sources.

### PCB
There is a single PCB. Although still common, some of the more precise requirements were needed to end up with a printed circuit board small enough.

| Paramter           | Value | Unit |
| ------------------ | -----:| ---- |
| Height             |    26 | mm   |
| Width              |    24 | mm   |
| Layers             |     4 | ul   |
| Copper             |   1.0 | oz   |
| Copper Inner       |   0.5 | oz   |
| Thickness          |   1.0 | mm   |
| Material           | FR4   | ul   |
| Min Drill          |   0.2 | mm   |
| Trace Size         |   5/5 | mil  |
| Mask               | Black | ul   |
| Silk               | White | ul   |
| E-Test             | Yes   | ul   |
| Surface Finish     | ENIG  | ul   |

The PCB has four layers 

| Front     | Inner 1   | Inner 2   | Back      |
| --------- | --------- | --------- | --------- |
| ![Layer0] | ![Layer1] | ![Layer2] | ![Layer3] |

### PCB Assembly
The whole board can be pick and placed. The table below shows some data that might be needed when requesting a quote.

| Parameter         | Value |
| ----------------- | -----:|
| Uniqe Parts       |     8 |
| SMD Parts         |     8 |
| Placements        |    97 |
| Solder Joints     |   222 |
| Front Components  |    96 |
| Back Components   |     1 |

#### Assembly Detail Pictures

| Assembly    | Front       |
| ----------- | ----------- |
| ![assembly] | ![front]    |

| Front Detail   | Back Detail   |
| -------------- | ------------- |
| ![frontDetail] | ![backDetail] |

| Front ISO   | Back ISO    |
| ----------- | ----------- |
| ![frontIso] | ![backIso]  |

#### Potential Alternates
Some alternates have not been tested. (Will update when I can get stock or have to switch suppliers)

| Stated            | Alternetive      | Tested |
| ----------------- | ---------------- | ------ |
| SPW2430HR5H-B     | ZILLTEK ZTS6016  | No     |
| BAT-HLD-001       | MY-2032-08       | No     |
| STM8L151G4U6      | STM8L151G6U*     | Yes    |

### Physical Assembly

Th earwire is attached with jewelery pliers through the hole. The 1mm hole is made for up to 0.8mm wire over 20ga. Gold plated french hooks are used. these are commonly available as jewelry findings.

![IMG-frenchEarwire]

### Production Scaling
The board can be hexagonally packed into a panel with tiny tabs since it's held on all sides. To increase production efficiency, only one side can be PnP and the battery holder is added afterwards by hand.

![hexPackedPanel]

#### Case
The moulds can create secondary masters out of rsing and then be used to make gang moulds, allowing for multiple castings in parallel. 

[gangMoulds]

## Programmer
The programmer has a hole at the top to allow a pin to push the button for testing.

![programmer]

The 3D printed base holds the board in place while the PCB is held to it with 3mm heat set inserts. The PCB acts as a compliant mechanism providing down pressure while still allowing it to be flexible enough to lift. 

The programmer uses *MillMax* ‎‎‎[0965-0-15-20-80-14-11-0]‎ spring pins on a PCB that matches exactly with (ToDo)

![springPins]

## Artwork
The design and layout is the main artwork on the  (ToDo)

The accompanying getting started card shown below also has artwork. This is completely protected.

| Front        | Back        |
| ------------ | ----------- |
| ![cardFront] | ![cardBack] | 

## Inventory and QC
Inventory can be managed with QR coded serialized tags. The serialization provides better quality control because it allows failure analysis and tracking, in case of issues traceable to the batch and assembly level.

## Packaging
We are packaging and shippping in `14 cm x 17 cm` padded envelopes with brand stamping. These fit under the Warenpost requirements and allow international shipping. The envelopes are verifed to be under 3 cm before dispatching. Custom labeled sleeves will be used for retail packaging.

## Shipping
The labels are printed with CN22 on the harmonized label schedule.

Lithium cells have special requirements for shipping. With air mail, small cells (up to four) are packaged securely and may be sent with the product. A note is required on the packaging, but no warning label is mandatory.

`Lithium metal batteries in compliance with Section II of PI969`

For international shipping, the following HS code is used.

`HALO HS Code - 7117.90.0000	Imitation Jewlery other`

## Safety
The edges are fully routed when possible or finished afterwards. The PCB is made from fiberglass so care must be taken because it can be abrasive on the edges. Clear coat nail polish can be applied to round and soften the edges without changing how it looks.

The CR2032 cells are quite safe, as they have only very small traces of lithium, and have a fairly high internal resistance. However, they must still be disposed of responsibly. LIR2032 or other rechargable 2032 cells should not be used because they have a higher voltage outside of the guaranteed parameters and significantly lower capacity (under 25%).

If the battery is placed in backwards, it will drain over time because there is no reverse polarity protection. It will heat up but should not damage anything, due to high internal resistance limiting the discharge.

The printed circuit boards are assembled in a lead-free process, and all components are RHOS certified.

The low voltage `3.0` as well as the currents used pose very little risk.

## Certifications
Certifications take time and effort but will make a better product by guaranteeing its safety to users and letting them use it in other projects. The table below shows the order in which we will obtain certifications.

| Cetrtifing Authority     | Status                    |
| -------------------------| ------------------------- |
| OSHW                     | Pending [DEXXXXXX]        |
| CE                       | No  (Self Certification)  |
| FCC                      | No  (Self Certification)  |
| WEEE                     | No  (yearly fee)          |

## Liecence
The product was designed by Sawaiz Syed for Kolibri who owns the copyright. Everything is released under permissive copyleft licenses, and copies of all licenses are included.

| Sector        | License      | Verison |
| ------------- | ------------ | -------:|
| Hardware      | [CERN-OHL-S] |     2.0 |
| Firmware      | [GNU GPL]    |     3.0 |
| Documentation | [CC BY-SA]   |     4.0 |

## Attribution
- Make
- SDCC
- KiCad
- [STM8Flash]
### Fonts
- [Nunito]
- [IBM PLEX Mono]
- [DejaVu] 
### Libraries
- [STM8 Headers] Copyright (c) - 2020 Georg Icking-Konert

## ToDo
- [ ] Led sometimes remains on after suthdown
- [ ] Randomly HALO pattren forms high low pattren 

<!-- Images and Links -->

<!-- Files -->
[BAT-HLD-001]:                ./pcb/components/BAT-HLD-001/BAT-HLD-001.pdf
[BL-HUB37A-AV-TRB]:           ./pcb/components/BL-HUB37A-AV-TRB/BL-HUB37A-AV-TRB.pdf
[KXT3]:                       ./pcb/components/KXT3/KXT3.pdf
[LSM6DSM]:                    ./pcb/components/LSM6DSM/LSM6DSM.pdf
[SPW2430HR5H-B]:              ./pcb/components/SPW2430HR5H-B/SPW2430HR5H-B.pdf
[STM8L151G4]:                 ./pcb/components/STM8L15xxx/STM8L15xxx.pdf

<!-- Links -->
[KiCad]:                      https://kicad.org/
[Quakehold]:                  https://www.quakehold.com/collectibles.html
[TYP-1]:                      https://trollfactory.de/produkte/silikon-kautschuk/haertegrad-shore/weich-shore-a25/7044/tfc-silikon-kautschuk-typ-1-abformsilikon-weich-1-1-nv-troll-factory-rtv
[0965-0-15-20-80-14-11-0]:    https://www.mill-max.com/products/pin/0965
[Sockelgips FL]:              https://www.hinrichs-dental-shop.de/sockelgips-fl-p-1469.html

<!-- Internal Links -->
[PDF-schematic]:              ./docs/design/hardware/schematic.pdf        
[nodeHaloBuilding]:           ./pcb/halo.js
[BOMcsv]:                     ./pcb/bom.csv
<!-- Intro -->
[wornDynamic]:                ./docs/intro/wornDynamic.gif                             "Worn earring reacting to music Model: Greta"
[presentedCase]:              ./docs/intro/haloSetDisplay.jpg                          "Pair of earings in holder"
[render]:                     ./docs/render.jpg                                        "Render of earrings showing propertions"
<!-- Design -->
[PAT-audio]:                  ./docs/patterns/audio.gif                                "Demonstrating animation of audio pattern"
[PAT-halo]:                   ./docs/patterns/halo.gif                                 "Demonstrating animation of halo pattern"
[PAT-sparkle]:                ./docs/patterns/sparkle.gif                              "Demonstrating animation of sparkle pattern"
<!-- Hardware -->
[IMG-schematic]:              ./docs/design/hardware/schematic.png                     "Image of schematic"

<!-- Components -->
[IMG-BAT-HLD-001]:            ./pcb/components/BAT-HLD-001/BAT-HLD-001.jpg              "CR2032 Battery Holder"
[IMG-BL-HUB37A-AV-TRB]:       ./pcb/components/BL-HUB37A-AV-TRB/BL-HUB37A-AV-TRB.jpg    "0402 Red LED"
[IMG-KXT3]:                   ./pcb/components/KXT3/KXT3.jpg                            "Miniature low profile button"
[IMG-LSM6DSM]:                ./pcb/components/LSM6DSM/LSM6DSM.jpg                      "6 axis IMU"
[IMG-SPW2430HR5H-B]:          ./pcb/components/SPW2430HR5H-B/SPW2430HR5H-B.jpg          "MEMS Microphone"
[IMG-STM8L15xxx]:             ./pcb/components/STM8L15xxx/STM8L15xxx.jpg                "Low power microcontroller"
[IMG-frenchEarwire]:          ./pcb/components/frenchEarwire/frenchEarwire.jpg          "Gold plated french earwire"
<!-- Magenets -->
[6x1-magents]:                ./docs/components/magnets.jpg                             "6mm x 1mm N53 disc mangents"
[magnetsGlued]:               ./docs/assembly/jigsGlue.jpg                              "Mangents glued using the jig"
[magnetJig]:                  ./docs/assembly/jigs.jpg                                  "Jigs used to glue magents into place in the correct orentation"
<!-- Connectors and pads -->
[alignmentPins]:              ./docs/connectors/pinDim.jpg                          "Holes for tooling and jig alginment"
[mass]:                       ./docs/connectors/mass.jpg                           "Halo and battery on scale for measuring mass"
[pgrmPads]:                   ./docs/design/hardware/pgrmPads.png                   "Labeling of the programming pads"
[pgrmPlacement]:              ./docs/design/hardware/pgrmPlacement.png              "Dimentions of the programming pads"
[springPins]:                 ./docs/components/springPin.jpg                       "Spring pin for programing"    
<!-- Case -->
[caseBatch]:                  ./docs/case/batch.jpg
[caseRender]:                 ./docs/case/caseRender.jpg
[cases]:                      ./docs/case/cases.jpg
[caseCasting]:                ./docs/case/casting.jpg
[caseFDM]:                    ./docs/case/fdmPrinted.jpg
[caseMaster]:                 ./docs/case/master.jpg
[caseMould]:                  ./docs/case/mould.jpg

<!-- Firmware -->
[PWR-audio]:                  ./docs/firmware/powerProfile/audioPowerProfile.png
[PWR-halo]:                   ./docs/firmware/powerProfile/haloPowerProfile.png
[PWR-sparkle]:                ./docs/firmware/powerProfile/sparklePowerProfile.png
[GIF-audio]:                  ./docs/firmware/powerProfile/audio.gif
[GIF-halo]:                   ./docs/firmware/powerProfile/halo.gif
[GIF-sparkle]:                ./docs/firmware/powerProfile/sparkle.gif
[GIF-boot]:                   ./docs/firmware/boot.gif

<!-- PCB Layers -->
[Layer0]:                     ./docs/layers/L0.png          "Front layer"
[Layer1]:                     ./docs/layers/L1.png          "Inner 1 layer"
[Layer2]:                     ./docs/layers/L2.png          "Inner 2 layer"
[Layer3]:                     ./docs/layers/L3.png          "Back layer"
<!-- PCB Assembly -->
[assembly]:                   ./docs/pcbAssembly/assemblyDraw.png                               "Drawing showing led and chip orentations"
[backDetail]:                 ./docs/pcbAssembly/back-detail.jpg                                "Detail view of back assembled"
[backIso]:                    ./docs/pcbAssembly/back-iso.jpg                                   "Isometric view of assembled back"
[frontDetail]:                ./docs/pcbAssembly/front-detail.jpg                               "Detail view of assembled front"
[front]:                      ./docs/pcbAssembly/front-full.jpg                                 "Front view of assembled board"
[frontIso]:                   ./docs/pcbAssembly/front-iso.jpg                                  "Isometric view of front assembled"
[hexPackedPanel]:             ./docs/pcbAssembly/hexPackedPanel.png                             "Panel with 35 boards"

<!-- Programmer -->
[programmer]:                 ./docs/programmer.jpg

<!-- Artwork -->
[cardFront]:                  ./docs/artwork/cardFront.png
[cardBack]:                   ./docs/artwork/cardBack.png


<!-- Certifications -->
[DE000087]:                   https://certification.oshwa.org/de000087.html

<!-- Licence -->
[CERN-OHL-S]:                 ./pcb/LICENSE
[GNU GPL]:                    ./firmware/LICENSE
[CC BY-SA]:                   ./docs/LICENSE

<!-- Attribution -->
[STM8Flash]:                  https://github.com/vdudouyt/stm8flash
[STM8 Headers]:               https://github.com/gicking/STM8_headers

<!-- Fonts -->
[DejaVu]:                     https://dejavu-fonts.github.io/
[IBM Plex Mono]:              https://www.ibm.com/plex/
[Nunito]:                     https://github.com/vernnobile/NunitoFont
