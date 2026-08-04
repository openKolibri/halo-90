// halo.c halo control and procesing
// Copyright (C) 2021 Kolibri - Sawaiz Syed

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// #define __SDCC
#include "STM8L151G6.h"

#define LED_COUNT 90
#define BRIGHTNESS_STEPS 32
#define FULL_BRIGHTNESS BRIGHTNESS_STEPS
#define SPARK_COUNT 4
#define SCAN_STRIDE 10
#define SCAN_BANKS 10
#define CPX_LINE_COUNT 10
#define GROUP_MAX_SINKS 3
#define GROUP_SLOTS 3
#define GROUP_BRIGHTNESS_MAX BRIGHTNESS_STEPS
// Rotates the atan2 gravity vector onto the earring's lowest LED, so the
// comet rests at the bottom of the hoop (was 68 = topmost, bubble-style).
#define BUBBLE_LEVEL_OFFSET 23
#define COMET_TAIL 8
#define TILT_DEADZONE 3
#define JOLT_DEADZONE 10
#define STILL_ENERGY_THRESHOLD 10
#define MOTION_CHARGE_START 8
#define MOTION_CHARGE_BURST 20
#define MOTION_CHARGE_DECAY 204
#define ENERGY_DECAY_DIVISOR 183
#define MOTION_CHARGE_SCALE_DIVISOR 25
#define MOTION_CHARGE_INPUT_MAX 120
// Double-tap detection on the 48 Hz jolt stream: a tap is an isolated jolt
// spike; two spikes inside the window with no sustained motion toggle the
// display mode. These values are from commit 1914966, which matched the
// hardware timing better than the SC7A20 hardware click engine on this board.
#define TAP_JOLT_MIN 110
#define TAP_WINDOW_SAMPLES 24   // ~0.5 s to land the second tap
#define TAP_GAP_SAMPLES 4       // ~80 ms refractory; one tap spans 1-2 samples
#define TAP_STILL_CHARGE_MAX 12
#define DISPLAY_MODE_COUNT 2    // motion+bass (combined), mic oscilloscope
#define ACCEL_SAMPLE_TICKS 163
#define SOLID_BREATH_CYCLE_TICKS 240
#define SOLID_BREATH_LUT_SIZE 64
#define SOLID_BREATH_BASE_RATE_Q4 16
#define SOLID_BREATH_ENERGY_RATE_Q4 32
#define SERIAL_DIAGNOSTICS 1
#define ACCEL_DIAGNOSTICS SERIAL_DIAGNOSTICS
#define TAP_CAL_DIAGNOSTICS 0
#define MIC_MODE_DIAGNOSTICS 0
#define ACCEL_DIAG_PRINT_SAMPLES 48

// Prototypes
void setLed(uint8_t led);
void ledHigh(uint8_t led);
void ledLow(uint8_t led);
void cpxAllHiZ(void);
uint8_t cpxLayoutSafe(void);
uint8_t cpxGroupSafe(uint8_t high, uint16_t low_mask);
uint8_t driveCpxGroup(uint8_t high, uint16_t low_mask);
void renderSolidGroup(uint8_t brightness);
uint16_t fast_rand(void);
uint16_t safe_rand(void);
uint8_t nextPwmScanLed(void);
uint8_t ringDistance(uint8_t a, uint8_t b);
uint8_t pwmAllows(uint8_t led, uint8_t brightness);

void initHall(void);
void initTim2(uint16_t timeout);
void enableTim2(void);
void disableTim2(void);

// Accelerometer & I2C Prototypes
void initAccel(void);
uint8_t readAccelRaw(int8_t *x, int8_t *y, int8_t *z);
uint8_t accel_read_reg(uint8_t reg);
uint8_t accel_read_status(void);
void accel_write_reg(uint8_t reg, uint8_t val);
void printAccelConfig(const char *label);
void resetAccelBus(void);
void cfgLoad(void);
void cfgSave(void);
void printCfg(void);
void pollUartCmd(void);
void uartStashPoll(void);
void enterSolidMode(void);
void exitSolidMode(void);
void setDisplayMode(uint8_t m);
uint8_t nextTapDisplayMode(uint8_t m);
void accelConfig(void);
void micPowerOn(void);
void micPowerOff(void);
uint8_t micBurst(uint8_t *density_out, uint8_t *spread_out);
uint8_t micScopeFrame(void);
uint8_t micEnvFrame(uint16_t *spread_out);

void initUart(void);
int putchar(int c);

// Galois LFSR for fast PRNG
volatile uint16_t lfsr_state = 0xACE1u;
uint16_t fast_rand(void) {
  uint16_t lsb = lfsr_state & 1;
  lfsr_state >>= 1;
  if (lsb) {
    lfsr_state ^= 0xB400u;
  }
  return lfsr_state;
}

uint16_t safe_rand(void) {
  uint16_t r;
  DISABLE_INTERRUPTS();
  r = fast_rand();
  ENABLE_INTERRUPTS();
  return r;
}

// Global previousLed and POV visualizer engine
volatile uint8_t prevLed = 0;
volatile uint8_t sleep = 0;
volatile uint8_t base_led = 0;
volatile uint8_t sc7a20_addr = 0x30;

// Balance Halo visual state variables
volatile uint8_t energy = 0;             // Q8 energy (0 to 255)
volatile uint8_t motion_charge = 0;      // Smoothed motion input (0 to 255)
volatile int16_t halo_angle_q4 = 0;       // Q4 angular position (0 to 1439)
volatile int16_t angular_velocity_q4 = 0; // Q4 angular velocity
volatile uint8_t time_ticks = 0;          // Dynamic mask reference counter
volatile uint8_t breathing_brightness = 2; // PWM duty cycle (0 to 32)
volatile int8_t comet_dir = 1;            // Last direction of travel; tail trails behind it
volatile uint8_t settle_extent = 0;       // Lit arc radius draining into the comet on settle
volatile uint8_t comet_peak = FULL_BRIGHTNESS; // Comet head level; eases down after long stillness
volatile uint8_t display_mode = 0;        // 0 = motion pattern, 1 = breathing solid ring
volatile uint8_t solid_breath = 1;        // Ring level in mode 1, driven by the breath curve
volatile uint8_t solid_frames = 0;        // 48 Hz frame pulses from the ISR while in mode 1
volatile uint8_t solid_wake_hold = 0;     // Frames of full darkness after waking in breathing mode
volatile uint8_t bass_extent = 0;         // Bass arc half-width in LEDs (0 = silent)
volatile uint8_t bass_glow = 0;           // Bass arc brightness, 0-32
volatile uint8_t mic_warmup = 0;          // Frames until the PDM mic's modulator output is trusted

// The USART has a single-byte buffer and mic captures blank the main loop
// for up to 18 ms while shell characters arrive 1 ms apart: capture loops
// stash incoming bytes between sub-windows so commands are not dropped.
// 4-deep ring: a command is 2 bytes and up to two can pile up per frame.
volatile uint8_t uart_stash[4];
volatile uint8_t uart_stash_head = 0;
volatile uint8_t uart_stash_tail = 0;
volatile uint8_t isr_frames = 0;          // Free-running 48 Hz frame counter
volatile uint8_t jolt_skip = 0;           // Hide the mode-exit tap from the motion pattern

// Sleep breathing curve for the solid ring, Apple-indicator style: cosine
// ease up over ~1.9 s, near-imperceptible hold at the peak, slower ~2.9 s
// ease back down, lingering at a dim floor of 1 (never off, never a blink -
// zero slope at both extremes). Stepped at 48 Hz: 240 steps = 5.0 s/cycle,
// 12 breaths per minute. Values are perceptual 1-32; the ISR's gamma LUT
// makes the fade eye-linear and the dithered PWM keeps it flicker-free.
static const uint8_t sleep_breath_lut[SOLID_BREATH_LUT_SIZE] = {
   1,  1,  1,  2,  3,  4,  5,  6,
   8, 10, 11, 13, 15, 17, 19, 21,
  22, 24, 26, 27, 28, 30, 31, 31,
  32, 32, 32, 32, 32, 31, 31, 31,
  30, 29, 28, 28, 27, 26, 25, 24,
  22, 21, 20, 19, 18, 16, 15, 14,
  12, 11, 10,  9,  8,  7,  6,  5,
   4,  3,  3,  2,  2,  1,  1,  1
};
static uint8_t solid_breath_tick = 0;     // 0-239 position in the cycle
static uint16_t solid_breath_phase_q4 = 0; // 0-3839, allows energy-scaled speed

// Render parameters precomputed at the 48 Hz sample rate so the 7.8 kHz
// render ISR never performs a (software) division.
volatile uint8_t glow_width_v = 16;
volatile uint16_t glow_scale_v = (18 << 8) / 16;
volatile uint8_t frag_threshold_v = 28;

// Live-tunable motion parameters: adjustable over the UART shell and
// persisted in data EEPROM (see pollUartCmd/cfgSave).
uint8_t cfg_jolt_deadzone = JOLT_DEADZONE;
uint8_t cfg_charge_divisor = MOTION_CHARGE_SCALE_DIVISOR;
uint8_t cfg_charge_start = MOTION_CHARGE_START;
uint8_t cfg_charge_burst = MOTION_CHARGE_BURST;
volatile uint8_t still_mode = 1;
volatile uint8_t pwm_scan_led = 0;
volatile uint8_t pwm_scan_bank = 0;
volatile uint8_t pwm_phase = 0;
volatile uint8_t solid_group_source = 0;
volatile uint8_t solid_group_slot = 0;
volatile uint8_t cpx_group_enabled = 1;
volatile uint16_t cpx_unsafe_group_blocks = 0;
volatile uint8_t burst_pos = 0;
volatile uint8_t burst_radius = 0;
volatile uint8_t burst_life = 0;

// Orbiting spark particles
volatile uint8_t spark_pos[SPARK_COUNT] = {0, 0, 0, 0};
volatile int8_t spark_vel[SPARK_COUNT] = {0, 0, 0, 0};
volatile uint8_t spark_life[SPARK_COUNT] = {0, 0, 0, 0};

// xlMic (0v9) board pinout, extracted from pcb/halo-90.kicad_pcb:
//   Accelerometer SC7A20HTR (I2C): SDA PC0, SCL PC1.
//     INT1/INT2 are NOT routed to the MCU on this revision.
//   Hall HAL2041S: OUT on PA2 (EXTI2 wake), VCC power-gated by PA3.
//   ALR (ambient light): sense on PA5, power-gated by PA4. Unused for now.
//   PDM microphone: CLK PB5, DATA PB7, power-gated by PD4. Unused for now.
//   UART: TX PC3, RX PC2. SWIM pin 28. PC4-PC6 unconnected.

// Charlieplex lines CPX-0..CPX-9 (net names in the PCB):
//   0=PB2 1=PD2 2=PD1 3=PD0 4=PB6 5=PB1 6=PB0 7=PB4 8=PD3 9=PB3
PORT_t *CPX_PORT[] = {&sfr_PORTB, &sfr_PORTD, &sfr_PORTD, &sfr_PORTD,
                      &sfr_PORTB, &sfr_PORTB, &sfr_PORTB, &sfr_PORTB,
                      &sfr_PORTD, &sfr_PORTB};
uint8_t CPX_PIN[] = {PIN2, PIN2, PIN1, PIN0, PIN6,
                     PIN1, PIN0, PIN4, PIN3, PIN3};

// Global loop rate-limiting tick
volatile uint8_t accel_tick = 0;

void cpxAllHiZ(void) {
  for (uint8_t k = 0; k < CPX_LINE_COUNT; k++) {
    CPX_PORT[k]->DDR.byte &= ~CPX_PIN[k];
    CPX_PORT[k]->CR1.byte &= ~CPX_PIN[k];
  }
}

uint8_t cpxLayoutSafe(void) {
  for (uint8_t a = 0; a < CPX_LINE_COUNT; a++) {
    if (CPX_PIN[a] == 0) {
      return 0;
    }
    for (uint8_t b = a + 1; b < CPX_LINE_COUNT; b++) {
      if (CPX_PORT[a] == CPX_PORT[b] && CPX_PIN[a] == CPX_PIN[b]) {
        return 0;
      }
    }
  }
  return 1;
}

uint8_t cpxGroupSafe(uint8_t high, uint16_t low_mask) {
  uint8_t sinks = 0;

  if (!cpx_group_enabled || high >= CPX_LINE_COUNT || low_mask == 0) {
    return 0;
  }
  if (low_mask & (uint16_t)~((1u << CPX_LINE_COUNT) - 1u)) {
    return 0;
  }
  if (low_mask & (uint16_t)(1u << high)) {
    return 0;
  }

  for (uint8_t low = 0; low < CPX_LINE_COUNT; low++) {
    if (low_mask & (uint16_t)(1u << low)) {
      sinks++;
      if (sinks > GROUP_MAX_SINKS) {
        return 0;
      }
      if (CPX_PORT[high] == CPX_PORT[low] && CPX_PIN[high] == CPX_PIN[low]) {
        return 0;
      }
    }
  }

  return sinks > 0;
}

uint8_t driveCpxGroup(uint8_t high, uint16_t low_mask) {
  if (!cpxGroupSafe(high, low_mask)) {
    cpx_unsafe_group_blocks++;
    cpxAllHiZ();
    return 0;
  }

  cpxAllHiZ();

  for (uint8_t low = 0; low < CPX_LINE_COUNT; low++) {
    if (low_mask & (uint16_t)(1u << low)) {
      CPX_PORT[low]->ODR.byte &= ~CPX_PIN[low];
      CPX_PORT[low]->CR1.byte |= CPX_PIN[low];
      CPX_PORT[low]->DDR.byte |= CPX_PIN[low];
    }
  }

  CPX_PORT[high]->ODR.byte |= CPX_PIN[high];
  CPX_PORT[high]->CR1.byte |= CPX_PIN[high];
  CPX_PORT[high]->DDR.byte |= CPX_PIN[high];
  return 1;
}

void renderSolidGroup(uint8_t brightness) {
  // One source line fans out to up to three sinks. Every LED is revisited
  // every 30 ISR slots; full-scale uses most of the slot for a brighter peak,
  // while the safety checks below still prevent any source/sink short state.
  static const uint8_t pulse_lut[GROUP_BRIGHTNESS_MAX + 1] = {
      0,  5,  7, 10, 13, 17, 21, 25,
     30, 35, 40, 45, 50, 55, 60, 65,
     70, 75, 80, 85, 90, 95,100,104,
    108,112,116,119,122,125,128,131,
    134
  };
  uint16_t low_mask = 0;
  uint8_t skipped = 0;
  uint8_t added = 0;
  uint8_t start = solid_group_slot * GROUP_MAX_SINKS;

  if (brightness > GROUP_BRIGHTNESS_MAX) {
    brightness = GROUP_BRIGHTNESS_MAX;
  }

  for (uint8_t low = 0; low < CPX_LINE_COUNT && added < GROUP_MAX_SINKS; low++) {
    if (low == solid_group_source) {
      continue;
    }
    if (skipped >= start) {
      low_mask |= (uint16_t)(1u << low);
      added++;
    }
    skipped++;
  }

  if (brightness > 0 && driveCpxGroup(solid_group_source, low_mask)) {
    for (uint8_t i = 0; i < pulse_lut[brightness]; i++) {
      NOP(); NOP(); NOP(); NOP();
    }
  }
  cpxAllHiZ();

  solid_group_slot++;
  if (solid_group_slot >= GROUP_SLOTS) {
    solid_group_slot = 0;
    solid_group_source++;
    if (solid_group_source >= CPX_LINE_COUNT) {
      solid_group_source = 0;
    }
  }
}

uint8_t nextPwmScanLed(void) {
  uint8_t led = pwm_scan_led;

  pwm_scan_led += SCAN_STRIDE;
  if (pwm_scan_led >= LED_COUNT) {
    pwm_scan_bank++;
    if (pwm_scan_bank >= SCAN_BANKS) {
      pwm_scan_bank = 0;
      pwm_phase++;
      if (pwm_phase >= BRIGHTNESS_STEPS) {
        pwm_phase = 0;
      }
    }
    pwm_scan_led = pwm_scan_bank;
  }

  return led;
}

uint8_t ringDistance(uint8_t a, uint8_t b) {
  uint8_t diff = (a > b) ? (a - b) : (b - a);
  if (diff > (LED_COUNT / 2)) {
    diff = LED_COUNT - diff;
  }
  return diff;
}

// Bit-reversed 5-bit counter for dithered PWM. Comparing brightness against
// the reversed phase spreads an LED's on-sweeps evenly across the 32-sweep
// cycle instead of bunching them: half brightness becomes an 87/2 = 43 Hz
// square wave rather than a visible 2.7 Hz blink, and every intermediate
// level flickers at 43 Hz or faster.
// NOTE: do not shorten the TIM2 period to raise these rates. The main loop
// paces the 48 Hz accelerometer sampling by counting its own iterations
// (ACCEL_SAMPLE_TICKS), and a faster ISR starves/retimes that loop - tried
// at 48 us on 2026-07-03 and tap + motion response broke on hardware.
static const uint8_t dither_phase[BRIGHTNESS_STEPS] = {
   0, 16,  8, 24,  4, 20, 12, 28,
   2, 18, 10, 26,  6, 22, 14, 30,
   1, 17,  9, 25,  5, 21, 13, 29,
   3, 19, 11, 27,  7, 23, 15, 31
};

// Gamma 2.2: pattern math stays perceptually linear (0-32); this LUT maps
// it to duty so fades look even instead of crushing the bright end.
// Nonzero inputs floor at 1 so faint tail pixels never vanish outright.
static const uint8_t gamma_lut[BRIGHTNESS_STEPS + 1] = {
   0,  1,  1,  1,  1,  1,  1,  1,
   2,  2,  2,  3,  4,  4,  5,  6,
   7,  8,  9, 10, 11, 13, 14, 15,
  17, 19, 20, 22, 24, 26, 28, 30,
  32
};

uint8_t pwmAllows(uint8_t led, uint8_t brightness) {
  if (brightness >= FULL_BRIGHTNESS) {
    return 1;
  }
  if (brightness == 0) {
    return 0;
  }

  // Per-LED phase offsets keep equally bright LEDs from blinking in lockstep.
  uint8_t phase = (pwm_phase + ((led * 13) & (BRIGHTNESS_STEPS - 1))) & (BRIGHTNESS_STEPS - 1);
  return dither_phase[phase] < brightness;
}

ISR_HANDLER(TIM2_UPD_ISR, _TIM2_OVR_UIF_VECTOR_) {
  if (sleep) {
    sfr_TIM2.SR1.UIF = 0;  // clear timer 2 interrupt flag
    return;
  }

  time_ticks++;

  // 48 Hz frame clock, independent of the main loop: paces the solid-mode
  // breathing and the accelerometer polling cadence.
  static uint16_t frame_div = 0;
  if (++frame_div >= ACCEL_SAMPLE_TICKS) {
    frame_div = 0;
    isr_frames++;
    solid_frames++;
  }

  uint8_t led_to_light = nextPwmScanLed();

  // Scope mode: the main loop draws waveform positions directly; leaving
  // the current LED lit between windows is what gives the POV persistence.
  if (display_mode == 1) {
    sfr_TIM2.SR1.UIF = 0;
    return;
  }

  uint8_t brightness;
  uint8_t glow_dist = ringDistance(led_to_light, base_led);

  if (still_mode) {
    // Comet resting at the low point: steady bright head (no breathing),
    // tail fading behind the direction of travel.
    // Tail falloff at full peak (32 * (9 - d) / 9), scaled by comet_peak.
    static const uint8_t tail_lut[COMET_TAIL + 1] = {32, 28, 25, 21, 18, 14, 11, 7, 4};
    brightness = 0;
    if (glow_dist == 0) {
      brightness = comet_peak;
    } else if (glow_dist <= COMET_TAIL) {
      uint8_t fwd = (led_to_light >= base_led)
                        ? (led_to_light - base_led)
                        : (led_to_light + LED_COUNT - base_led);
      uint8_t on_tail = (comet_dir > 0) ? (fwd >= LED_COUNT - COMET_TAIL)
                                        : (fwd <= COMET_TAIL);
      if (on_tail) {
        brightness = ((uint16_t)tail_lut[glow_dist] * comet_peak) >> 5;
      } else if (glow_dist == 1) {
        brightness = comet_peak >> 2; // faint nose on the leading side
      }
    }
    // Settling drain: the LEDs that were lit when motion stopped stay on and
    // the arc boundary slides down both sides into the comet, one LED per
    // sample, so the mode hand-off is a continuous collapse instead of a cut.
    if (brightness == 0 && glow_dist <= settle_extent) {
      brightness = 2;
    }
  } else {
    brightness = breathing_brightness;
  }

  if (!still_mode && burst_life > 0) {
    uint8_t burst_forward = burst_pos + burst_radius;
    uint8_t burst_back;

    while (burst_forward >= LED_COUNT) {
      burst_forward -= LED_COUNT;
    }

    if (burst_radius > burst_pos) {
      burst_back = (burst_pos + LED_COUNT) - burst_radius;
    } else {
      burst_back = burst_pos - burst_radius;
    }

    if (ringDistance(led_to_light, burst_forward) <= 1 || ringDistance(led_to_light, burst_back) <= 1) {
      brightness = FULL_BRIGHTNESS;
    }
  }

  // Sparks render in both modes: orbiting embers when charged, falling
  // points and jolt dust when still.
  for (uint8_t j = 0; j < SPARK_COUNT; j++) {
    if (spark_life[j] > 0) {
      uint8_t spark_dist = ringDistance(led_to_light, spark_pos[j]);
      if (spark_dist == 0) {
        brightness = FULL_BRIGHTNESS;
      } else if (spark_dist == 1 && brightness < 24) {
        brightness = 24;
      }
    }
  }

  if (!still_mode && glow_dist <= glow_width_v) {
    uint8_t glow = (uint8_t)(((uint16_t)(glow_width_v - glow_dist) * glow_scale_v) >> 8);
    brightness += glow;
    if (brightness > FULL_BRIGHTNESS) {
      brightness = FULL_BRIGHTNESS;
    }
  }

  if (!still_mode && energy > 12) {
    uint16_t r = fast_rand();
    if ((uint8_t)r < energy) {
      uint8_t shimmer = ((r >> 8) & 0x07) + (energy >> 6);
      brightness += shimmer;
      if (brightness > FULL_BRIGHTNESS) {
        brightness = FULL_BRIGHTNESS;
      }
    }
  }

  // Fragmentation breaks the ring as movement energy rises, while preserving the bubble core.
  if (!still_mode && energy > 36 && brightness < FULL_BRIGHTNESS && glow_dist > 2) {
    uint8_t mask_pos = ((led_to_light * 5) + time_ticks + (energy >> 3)) & 0x1F;
    if (mask_pos >= frag_threshold_v) {
      brightness = 0;
    }
  }

  // Sound layer: the bass envelope swells an arc out of the same gravity
  // low point the comet rests at, so the earring answers both motion and
  // music in one picture. Brightest-wins keeps the comet head visible on
  // top of the glow instead of averaging the two into mush.
  if (bass_extent > 0) {
    uint8_t ab = 0;
    if (glow_dist < bass_extent) {
      ab = bass_glow;
    } else if (glow_dist == bass_extent) {
      ab = bass_glow >> 2; // soft edge
    }
    if (ab > brightness) {
      brightness = ab;
    }
  }

  if (pwmAllows(led_to_light, gamma_lut[brightness])) {
    setLed(led_to_light);
  } else {
    ledLow(prevLed);
  }

  sfr_TIM2.SR1.UIF = 0;  // clear timer 2 interrupt flag
}

// Hall Sensor ISR
ISR_HANDLER(HALL_ISR, _EXTI2_VECTOR_) {
  if (!(sfr_PORTA.IDR.byte & PIN2)) {
    sleep = 1; // Trigger main loop to sequence a safe shutdown
  }
  sfr_ITC_EXTI.SR1.P2F = 1; // Clear interrupt flag
  return;
}

// Fast integer atan2 approximation for 8-bit MCU, directly outputs 0-89 LEDs
uint8_t fast_atan2_to_led(int8_t y, int8_t x) {
    int16_t abs_y = abs((int16_t)y);
    int16_t abs_x = abs((int16_t)x);
    int16_t a;
    
    if (abs_x == 0 && abs_y == 0) return 0;
    
    if (abs_x >= abs_y) {
        a = (11 * abs_y) / abs_x;
    } else {
        a = 22 - (11 * abs_x) / abs_y;
    }
    
    if (x < 0) {
        a = 45 - a;
    }
    if (y < 0) {
        a = 90 - a;
    }
    if (a >= LED_COUNT) {
      a -= LED_COUNT;
    }
    return (uint8_t)a;
}

void main(void) {
  DISABLE_INTERRUPTS();

  // Bump up clock to 16MHz
  sfr_CLK.CKDIVR.byte = 0x00;

  // Initialize ALL pins to Input Pull-Up initially to prevent any floating input buffer leakage
  sfr_PORTA.DDR.byte = 0x00; sfr_PORTA.CR1.byte = 0xFF; sfr_PORTA.CR2.byte = 0x00;
  sfr_PORTB.DDR.byte = 0x00; sfr_PORTB.CR1.byte = 0xFF; sfr_PORTB.CR2.byte = 0x00;
  sfr_PORTC.DDR.byte = 0x00; sfr_PORTC.CR1.byte = 0xFF; sfr_PORTC.CR2.byte = 0x00;
  sfr_PORTD.DDR.byte = 0x00; sfr_PORTD.CR1.byte = 0xFF; sfr_PORTD.CR2.byte = 0x00;
  sfr_PORTE.DDR.byte = 0x00; sfr_PORTE.CR1.byte = 0xFF; sfr_PORTE.CR2.byte = 0x00;
  sfr_PORTF.DDR.byte = 0x00; sfr_PORTF.CR1.byte = 0xFF; sfr_PORTF.CR2.byte = 0x00;

  // Init and Set all CPX pins to hi-z
  for (int k = 0; k < sizeof(CPX_PIN) / sizeof(CPX_PIN[0]); k++) {
    CPX_PORT[k]->DDR.byte &= ~CPX_PIN[k];
    CPX_PORT[k]->CR1.byte &= ~CPX_PIN[k];
  }
  cpx_group_enabled = cpxLayoutSafe();

  // Unused xlMic peripherals held off and quiet (awake and asleep):
  // mic power gate PD4 low, PDM_CLK PB5 / PDM_DTA PB7 driven low (the
  // unpowered mic never contends), ALR power gate PA4 low and its sense
  // node PA5 driven low while unpowered. No pin is left floating.
  sfr_PORTD.ODR.byte &= ~PIN4;
  sfr_PORTD.DDR.byte |= PIN4;
  sfr_PORTB.ODR.byte &= ~(PIN5 | PIN7);
  sfr_PORTB.DDR.byte |= (PIN5 | PIN7);
  sfr_PORTA.ODR.byte &= ~(PIN4 | PIN5);
  sfr_PORTA.DDR.byte |= (PIN4 | PIN5);

  initHall();
  initAccel();
  initUart();

  // Add a small delay for serial console attachment
  for(uint32_t d=0; d<300000; d++) NOP();

  printf("\r\n--- HALO-90 Booting ---\r\n");

  cfgLoad();
  printCfg();

  uint8_t found_addr = 0x00;
  for (uint8_t addr = 0x01; addr < 0x7F; addr++) {
    sfr_I2C1.CR2.START = 1;
    uint16_t t = 10000; while (!sfr_I2C1.SR1.SB && --t);
    if (!t) { sfr_I2C1.CR2.STOP = 1; continue; }
    
    sfr_I2C1.DR.byte = (addr << 1);
    t = 10000; while (!sfr_I2C1.SR1.ADDR && !(sfr_I2C1.SR2.byte & 0x04) && --t);
    
    if (sfr_I2C1.SR1.ADDR) {
      (void)sfr_I2C1.SR1.byte;
      (void)sfr_I2C1.SR3.byte;
      sfr_I2C1.CR2.STOP = 1;
      found_addr = addr;
      break;
    }
    
    sfr_I2C1.SR2.byte &= ~0x04; // clear AF
    sfr_I2C1.CR2.STOP = 1;
  }
  printf("I2C Scan found: 0x%02X\r\n", found_addr);
  if (found_addr != 0) {
      sc7a20_addr = (found_addr << 1);
  } else {
      printf("ACCEL WARN: no I2C ACK found, using default 8-bit addr 0x%02X\r\n", sc7a20_addr);
  }

  // Reload the initialization since it might have failed previously with the old address
  accelConfig();
  printAccelConfig("boot");



  // Enable Ultra-Low Power (disables internal references when halted)
  // Enable Flash/EEPROM power-down during HALT (Fixes the 1.2mA standby leak!)
  sfr_PWR.CSR2.ULP = 1;
  sfr_PWR.CSR2.FWU = 1;
  sfr_FLASH.CR1.EEPM = 1;

  int8_t rx_prev = 0, ry_prev = 0, rz_prev = 0;

  // Boot timer logic explicitly to ensure consistent sequence
  initTim2(15);
  disableTim2();

  // Check initial state
  if (!(sfr_PORTA.IDR.byte & PIN2)) {
    sleep = 1;
  } else {
    enableTim2();
    setLed(0);
  }

  ENABLE_INTERRUPTS();

#if ACCEL_DIAGNOSTICS
  uint16_t accel_ok_count = 0;
  uint16_t accel_fail_count = 0;
  uint8_t accel_diag_tick = 0;
#endif

  while(1){
    if (sleep) {
      // -- SAFE SLEEP SHUTDOWN SEQUENCE --
      setDisplayMode(0);  // wake into the motion+bass mode
      micPowerOff();      // mic is powered whenever awake; off for halt
      disableTim2();
      ledLow(prevLed);
      
      // Remove PE toggle as it ruins I2C clock configuration registers!
      // Simply power down the accelerometer.
      accel_write_reg(0x20, 0x00);
      
      // Explicitly set all CPX pins to OUT LOW to prevent current leakage through floating buffer
      for (int k = 0; k < sizeof(CPX_PIN) / sizeof(CPX_PIN[0]); k++) {
        CPX_PORT[k]->ODR.byte &= ~CPX_PIN[k];
        CPX_PORT[k]->CR1.byte |= CPX_PIN[k];
        CPX_PORT[k]->DDR.byte |= CPX_PIN[k];
      }

      // Disable UART explicitly to prevent back-powering any connected serial bridges!
      // PC3 = TX. We must disable transmitter and set pin to Output LOW or Floating to kill the 1.2mA leak.
      sfr_USART1.CR2.TEN = 0;
      sfr_USART1.CR2.REN = 0;
      sfr_PORTC.ODR.byte &= ~(1<<3);
      sfr_PORTC.CR1.byte |= (1<<3);
      sfr_PORTC.DDR.byte |= (1<<3);

      // On this board the accel INT pins are not routed to the MCU. The
      // mic (PWR_MIC PD4, PDM PB5/PB7) and ALR (ALR_PWR PA4, sense PA5)
      // rails are held off / driven low permanently from boot, so nothing
      // floats here. HALL_PWR (PA3) stays high through HALT - the hall
      // sensor is the wake source and GPIO state is retained in halt.

      // Sleep power budget with this configuration (typical, 25 C, 3.0 V):
      //   STM8L151 HALT, ULP=1 (internal ref off), Flash/EEPROM power-down:
      //     ~0.35 uA
      //   SC7A20 power-down (CTRL1 = 0x00):              ~0.5 uA
      //   Hall switch (powered via PA3 - the wake source): ~1-2 uA class
      //   Mic and ALR power-gated off:                     ~0 uA
      //   GPIO leakage with every pin driven or pulled:   <0.1 uA
      //   => ~0.9 uA for MCU + accel; ~2-3 uA system with the hall sensor.
      // I2C pull-ups (PC0/PC1) stay enabled: both lines idle high against
      // the sensor's high-Z pads, so they carry no static current. UART RX
      // (PC2) keeps its pull-up for a defined level with no bridge attached.
      // Wake is the PA2 EXTI2 edge from the hall output; no clocks run in HALT.

      // Ensure EXTI flags are clear before entering halt loop
      sfr_ITC_EXTI.SR1.P2F = 1;

      // Enter ultra-low power HALT tightly while magnetic field is present (debounce-loop)
      while (!(sfr_PORTA.IDR.byte & PIN2)) {
        sfr_ITC_EXTI.SR1.P2F = 1;
        ENTER_HALT();
      }
      
      // -- WE WAKE UP HERE AFTER EXTI AND PADDING (Magnet Removed) --

      // Re-initialize all CPX pins back to HI-Z for matrix multiplexing
      for (int k = 0; k < sizeof(CPX_PIN) / sizeof(CPX_PIN[0]); k++) {
        CPX_PORT[k]->ODR.byte &= ~CPX_PIN[k];
        CPX_PORT[k]->DDR.byte &= ~CPX_PIN[k];
        CPX_PORT[k]->CR1.byte &= ~CPX_PIN[k];
      }
      
      // Re-enable UART TX/RX
      sfr_PORTC.DDR.byte |= (1<<3);
      sfr_PORTC.CR1.byte |= (1<<3);
      sfr_USART1.CR2.TEN = 1;
      sfr_USART1.CR2.REN = 1;

      // Ensure initAccel is called to refresh configuration if I2C was disrupted
      initAccel();
      accelConfig();
      micPowerOn();
      mic_warmup = 24;
      printAccelConfig("wake");
      
      energy = 0;
      motion_charge = 0;
      halo_angle_q4 = (int16_t)prevLed << 4;
      angular_velocity_q4 = 0;
      breathing_brightness = 2;
      still_mode = 1;
      pwm_scan_led = 0;
      pwm_scan_bank = 0;
      pwm_phase = 0;
      solid_group_source = 0;
      solid_group_slot = 0;
      burst_life = 0;

      // Waking in breathing mode: come up from true darkness. Hold the
      // ring fully dark for ~0.5 s, then let the breath swell from the
      // trough of the curve instead of popping in mid-cycle.
      if (display_mode) {
        solid_breath_phase_q4 = 0;
        solid_breath_tick = 0;
        solid_breath = 0;
        solid_frames = 0;
        solid_wake_hold = 24;
      }

      enableTim2();
      if (!display_mode) {
        setLed(prevLed); // prime the motion display; breathing wakes from dark
      }
      sleep = 0; 

    } else {
      // The render ISR is the only wake source here, and the mic and I2C
      // capture paths disable it while they own the pins. If one ever
      // returns without restoring it, WFI would sleep forever - so make
      // the loop self-healing rather than trusting every exit path.
      if (!sleep && !sfr_TIM2.IER.UIE) {
        sfr_TIM2.SR1.UIF = 0;
        sfr_TIM2.IER.UIE = 1;
      }

      WAIT_FOR_INTERRUPT();

      if (!sleep) {
        pollUartCmd();
      }

      // The mic runs whenever the device is awake (the bass layer is part
      // of the motion mode, not a mode you switch into), but it is started
      // from the running loop rather than during boot: enabling SPI before
      // the render ISR and interrupts are up wedges the MCU.
      static uint8_t mic_started = 0;
      if (!sleep && !mic_started) {
        mic_started = 1;
        micPowerOn();
        mic_warmup = 48;
      }

      if (!sleep && display_mode == 0 && solid_frames > 0) {
        // Sound layer for the motion mode: one 8 ms capture every other
        // frame (24 Hz). Halving the rate keeps the render blackout near
        // 19% so the comet stays bright, while the envelope still tracks
        // beats faster than the eye resolves.
        solid_frames--;
        static uint8_t bass_phase = 0;
        if (++bass_phase >= 2) {
          bass_phase = 0;
          uint16_t spread = 0;
          uint8_t ok = micEnvFrame(&spread);
          static uint16_t env_floor = 8;
          static uint16_t envelope = 0;
          static uint8_t floor_tick = 0;
          if (mic_warmup > 0) {
            mic_warmup--;
            bass_extent = 0;
          } else if (ok) {
            if (spread < env_floor) {
              env_floor = spread;       // snap down to quieter floor
            } else if (++floor_tick >= 32) {
              floor_tick = 0;
              env_floor++;              // creep up slowly: sustained music
                                        // must not get eaten as "noise"
            }
            uint16_t sig = (spread > env_floor) ? (spread - env_floor) : 0;
            if (sig > envelope) {
              envelope = sig;                       // instant attack
            } else {
              envelope -= (envelope >> 3);          // exponential release
              if (envelope > 0) envelope--;
            }
            if (envelope < 3) {
              bass_extent = 0;          // silence: comet alone, no glow
            } else {
              uint16_t ext = 2 + ((envelope * 3) >> 2); // ~2-40 LEDs
              bass_extent = (ext > 40) ? 40 : (uint8_t)ext;
              uint16_t g = 6 + envelope;
              bass_glow = (g > 26) ? 26 : (uint8_t)g; // stays under the comet
            }
          }
#if MIC_MODE_DIAGNOSTICS
          static uint8_t env_diag = 0;
          if (++env_diag >= 24) {
            env_diag = 0;
            printf("ENV spread=%u floor=%u env=%u ext=%u glow=%u\r\n",
                   spread, env_floor, envelope, bass_extent, bass_glow);
          }
#endif
        }
      }

      if (!sleep && display_mode == 1) {
        // Oscilloscope mode: one ~18 ms drawing frame per pass, then fall
        // through with the accel divider forced so motion sampling and the
        // double-tap exit keep running at ~50 Hz despite the long passes.
        uint8_t peak = micScopeFrame();
        if (mic_warmup > 0) {
          mic_warmup--;
        }
#if MIC_MODE_DIAGNOSTICS
        static uint8_t scope_diag = 0;
        if (++scope_diag >= 50) {
          scope_diag = 0;
          printf("SCOPE peak=%u\r\n", peak);
        }
#endif
        accel_tick = ACCEL_SAMPLE_TICKS;
      }

      if (!sleep && (++accel_tick >= ACCEL_SAMPLE_TICKS)) {
        accel_tick = 0;

        int8_t rx = rx_prev;
        int8_t ry = ry_prev;
        int8_t rz = rz_prev;
        uint8_t render_paused = 0;
        if (display_mode && sfr_TIM2.IER.UIE) {
          render_paused = 1;
          sfr_TIM2.IER.UIE = 0;
          cpxAllHiZ();
        }
        if (!readAccelRaw(&rx, &ry, &rz)) {
          resetAccelBus();
          if (readAccelRaw(&rx, &ry, &rz)) {
            goto accel_read_ok;
          }
          if (render_paused) {
            sfr_TIM2.SR1.UIF = 0;
            sfr_TIM2.IER.UIE = 1;
          }
#if ACCEL_DIAGNOSTICS
          accel_fail_count++;
          if (display_mode < 2 || MIC_MODE_DIAGNOSTICS) {
            accel_diag_tick++;
            if (accel_diag_tick >= ACCEL_DIAG_PRINT_SAMPLES) {
              accel_diag_tick = 0;
              printf("ACCEL read_fail ok=%u fail=%u addr8=0x%02X status=0x%02X\r\n",
                     accel_ok_count, accel_fail_count, sc7a20_addr, accel_read_status());
            }
          }
#endif
          angular_velocity_q4 = ((int16_t)angular_velocity_q4 * 220) >> 8;
          continue;
        }
accel_read_ok:
        if (render_paused) {
          sfr_TIM2.SR1.UIF = 0;
          sfr_TIM2.IER.UIE = 1;
        }
#if ACCEL_DIAGNOSTICS
        accel_ok_count++;
#endif
        static uint8_t zero_sample_count = 0;
        if (rx == 0 && ry == 0 && rz == 0) {
          if (zero_sample_count < 0xFF) {
            zero_sample_count++;
          }
          if (zero_sample_count >= 8) {
            uint8_t reset_paused = 0;
            if (display_mode && sfr_TIM2.IER.UIE) {
              reset_paused = 1;
              sfr_TIM2.IER.UIE = 0;
              cpxAllHiZ();
            }
            resetAccelBus();
            if (reset_paused) {
              sfr_TIM2.SR1.UIF = 0;
              sfr_TIM2.IER.UIE = 1;
            }
            zero_sample_count = 0;
#if ACCEL_DIAGNOSTICS
            printf("ACCEL zero_stall reset\r\n");
#endif
            continue;
          }
        } else {
          zero_sample_count = 0;
        }

        // 1. Compute Jolt Energy
        int16_t jolt = abs((int16_t)rx - rx_prev) + abs((int16_t)ry - ry_prev) + abs((int16_t)rz - rz_prev);
        rx_prev = rx;
        ry_prev = ry;
        rz_prev = rz;

        // The tap that exits solid mode should not immediately kick the
        // motion pattern into a burst.
        if (jolt_skip) {
          jolt_skip = 0;
          jolt = 0;
        }

        // Apply a deadzone to filter sensor noise when still.
        int16_t active_jolt = 0;
        if (jolt > cfg_jolt_deadzone) {
          active_jolt = jolt - cfg_jolt_deadzone;
        }

        // 2. Build motion charge, then let charge feed visual energy.
        // Isolated jolts decay away; repeated steps build enough charge to wake the pattern.
        uint8_t tap_charge = motion_charge;
        uint16_t next_charge = ((uint16_t)motion_charge * MOTION_CHARGE_DECAY) >> 8;
        if (active_jolt > 0) {
          uint8_t charge_input = active_jolt;
          if (charge_input > MOTION_CHARGE_INPUT_MAX) {
            charge_input = MOTION_CHARGE_INPUT_MAX;
          }
          charge_input = (charge_input + (cfg_charge_divisor - 1)) / cfg_charge_divisor;
          next_charge += charge_input;
        }
        motion_charge = (uint8_t)next_charge;

        // Decay by about energy/183 per 48 Hz sample: roughly 5x longer
        // than the old energy/37 decay, but still proportional to energy.
        static uint16_t energy_decay_accum = 0;
        uint16_t next_energy = energy;
        energy_decay_accum += energy;
        while (energy_decay_accum >= ENERGY_DECAY_DIVISOR && next_energy > 0) {
          next_energy--;
          energy_decay_accum -= ENERGY_DECAY_DIVISOR;
        }
        if (next_energy == 0) {
          energy_decay_accum = 0;
        }
        if (motion_charge > cfg_charge_start) {
          next_energy += motion_charge - cfg_charge_start;
        }
        if (display_mode && active_jolt > 0) {
          // In breathing mode, make the speed react to motion directly.
          // The normal motion display keeps the charge gate so isolated taps
          // do not overdrive the visual pattern.
          uint8_t breath_input = active_jolt;
          if (breath_input > MOTION_CHARGE_INPUT_MAX) {
            breath_input = MOTION_CHARGE_INPUT_MAX;
          }
          next_energy += breath_input;
        }
        if (next_energy > 255) next_energy = 255;
        energy = (uint8_t)next_energy;
        still_mode = (energy <= STILL_ENERGY_THRESHOLD && motion_charge <= cfg_charge_start);

        static uint8_t tap_window = 0;
        // Starts armed-off for ~1 s: the first accelerometer reads after
        // boot are transient garbage and can fake a double-tap.
        static uint8_t tap_refractory = 48;
#if TAP_CAL_DIAGNOSTICS
        // Calibration trace: log every candidate spike, not just fires, so
        // a serial recording shows near-misses and their timing.
        if (active_jolt > 40) {
          printf("TAPCAL a=%d ch=%u pre=%u w=%u r=%u\r\n",
                 (int)active_jolt, motion_charge, tap_charge, tap_window,
                 tap_refractory);
        }
#endif
        if (tap_refractory > 0) {
          tap_refractory--;
        } else if (active_jolt > TAP_JOLT_MIN && tap_charge < TAP_STILL_CHARGE_MAX) {
          if (tap_window > 0) {
            setDisplayMode(nextTapDisplayMode(display_mode));
            tap_window = 0;
            motion_charge = 0;
            energy = 0;
            still_mode = 1;
#if ACCEL_DIAGNOSTICS
            printf("TAP mode=%u active=%d charge=%u\r\n",
                   display_mode, (int)active_jolt, motion_charge);
#endif
          } else {
            tap_window = TAP_WINDOW_SAMPLES;
          }
          tap_refractory = TAP_GAP_SAMPLES;
        }
        if (tap_window > 0) {
          tap_window--;
        }

        // Precompute the ISR's render parameters here at 48 Hz so the
        // 7.8 kHz ISR does table lookups and shifts only, no division.
        glow_width_v = 16 - (uint8_t)(((uint16_t)energy * 9) / 255);
        glow_scale_v = ((uint16_t)(18 + (energy >> 4)) << 8) / glow_width_v;
        if (energy > 36) {
          frag_threshold_v = 28 - (uint8_t)(((uint16_t)(energy - 36) * 20) / 219);
        }

        // 3. Physical Inertia System: bubble rides opposite the gravity projection.
        int16_t planar_tilt = abs((int16_t)rx) + abs((int16_t)ry);
        if (planar_tilt > TILT_DEADZONE) {
            uint8_t new_base = fast_atan2_to_led(ry, rx);
            uint8_t rotated_base = new_base + BUBBLE_LEVEL_OFFSET;
            if (rotated_base >= LED_COUNT) {
              rotated_base -= LED_COUNT;
            }

            int16_t target_q4 = (int16_t)rotated_base << 4;
            int16_t error = target_q4 - halo_angle_q4;
            if (error > 720) error -= 1440;
            else if (error < -720) error += 1440;

            // Spring inertia: calm motion is heavy; charged motion is tighter.
            uint8_t spring_shift = 4 - ((uint16_t)energy * 2) / 255;
            int16_t spring_impulse = error >> spring_shift;
            if (spring_impulse == 0 && error != 0) {
              spring_impulse = (error > 0) ? 1 : -1;
            }

            angular_velocity_q4 += spring_impulse;
            if (angular_velocity_q4 > 120) angular_velocity_q4 = 120;
            else if (angular_velocity_q4 < -120) angular_velocity_q4 = -120;

            // Damping moves from about 0.92 at rest to about 0.82 when charged.
            uint8_t damping = 238 - ((uint16_t)energy * 30) / 255;
            angular_velocity_q4 = ((int16_t)angular_velocity_q4 * (int16_t)damping) >> 8;
        } else {
            angular_velocity_q4 = ((int16_t)angular_velocity_q4 * 220) >> 8;
        }

        halo_angle_q4 += angular_velocity_q4;
        while (halo_angle_q4 < 0) halo_angle_q4 += 1440;
        while (halo_angle_q4 >= 1440) halo_angle_q4 -= 1440;

        base_led = (uint8_t)(halo_angle_q4 >> 4);

        // Track travel direction so the comet tail trails behind the motion.
        if (angular_velocity_q4 > 4) comet_dir = 1;
        else if (angular_velocity_q4 < -4) comet_dir = -1;

        // 4. Update Burst and Orbiting Sparks Positions
        if (burst_life > 0) {
          burst_life--;
          burst_radius += 3;
          while (burst_radius >= LED_COUNT) {
            burst_radius -= LED_COUNT;
          }
        }

        if (motion_charge > cfg_charge_burst && active_jolt > 3) {
          uint8_t burst_duration = 6 + (motion_charge >> 4);

          burst_pos = base_led;
          burst_radius = 0;
          burst_life = burst_duration;

          for (uint8_t j = 0; j < SPARK_COUNT; j++) {
            spark_pos[j] = base_led;
            spark_vel[j] = (j & 1) ? 1 : -1;
            spark_life[j] = 8 + ((motion_charge + (j * 3)) & 0x0F);
          }
        }

        for (uint8_t j = 0; j < SPARK_COUNT; j++) {
          if (spark_life[j] > 0) {
            if (spark_vel[j] > 0) {
              spark_pos[j]++;
              if (spark_pos[j] >= LED_COUNT) {
                spark_pos[j] = 0;
              }
            } else if (spark_pos[j] == 0) {
              spark_pos[j] = LED_COUNT - 1;
            } else {
              spark_pos[j]--;
            }
            spark_life[j]--;
          } else if (energy > 96) {
            uint16_t sr = safe_rand();
            if ((sr & 0x0F) == 0) {
              spark_pos[j] = base_led;
              spark_vel[j] = (sr & 1) ? 1 : -1;
              spark_life[j] = 10 + ((sr >> 8) & 15); // lifetime in 20ms steps
            }
          }
        }

        // 4b. Settling into stillness: the ring that was lit when motion
        // stopped drains into the low point. The lit arc starts at the full
        // half-ring and its boundary falls one LED per sample down both
        // sides until only the comet remains.
        if (!still_mode) {
          settle_extent = LED_COUNT / 2;
        } else if (settle_extent > 0) {
          settle_extent--;
        }

        // 4c. Resting power: after ~60 s of stillness, ease the comet head
        // down to a dim glow (one step per 8 samples, ~3 s fade). Any
        // motion snaps it straight back to full brightness.
        static uint16_t still_samples = 0;
        if (still_mode) {
          if (still_samples < 0xFFFF) still_samples++;
          if (still_samples > 2880 && (still_samples & 0x07) == 0 && comet_peak > 12) {
            comet_peak--;
          }
        } else {
          still_samples = 0;
          comet_peak = FULL_BRIGHTNESS;
        }

        // Spark dust: a sharp jolt while settled kicks a short-lived ember
        // off the comet, trailing behind its direction of travel.
        if (still_mode && active_jolt > 8) {
          for (uint8_t j = 0; j < SPARK_COUNT; j++) {
            if (spark_life[j] == 0) {
              spark_pos[j] = base_led;
              spark_vel[j] = -comet_dir;
              spark_life[j] = 4 + ((uint8_t)active_jolt & 7);
              break;
            }
          }
        }

        // 5. Base ring brightness follows energy directly: a dim floor when
        // barely moving, scaling smoothly to full (32/32) at the solid-ring
        // maximum. No oscillating modulation.
        breathing_brightness = 2 + (uint8_t)(((uint16_t)energy * 30) / 255);


#if ACCEL_DIAGNOSTICS
        if (display_mode < 2 || MIC_MODE_DIAGNOSTICS) {
          accel_diag_tick++;
          if (accel_diag_tick >= ACCEL_DIAG_PRINT_SAMPLES) {
            accel_diag_tick = 0;
            printf("ACCEL ok=%u fail=%u raw=%d,%d,%d jolt=%d active=%d charge=%u tilt=%d energy=%u base=%u vel=%d br=%u mode=%u status=0x%02X\r\n",
                   accel_ok_count, accel_fail_count,
                   (int)rx, (int)ry, (int)rz,
                   (int)jolt, (int)active_jolt, motion_charge, (int)planar_tilt,
                   energy, base_led, (int)angular_velocity_q4,
                   breathing_brightness, display_mode, accel_read_status());
          }
        }
#endif
      }
    }
  }
}

void initTim2(uint16_t timeout){
  sfr_CLK.PCKENR1.PCKEN10 = 1;                   // activate tim2 clock gate
  sfr_TIM2.CR1.CEN = 0;                          // disable timer
  sfr_ITC_SPR.SPR5.VECT19SPR = 0b01;             // Interupt Priority to Level 1 (lower)
  sfr_TIM2.CR1.ARPE = 1;                         // auto-reload value buffered
  sfr_TIM2.CNTRH.byte = 0x00;                    // MSB clear counter
  sfr_TIM2.CNTRL.byte = 0x00;                    // LSB clear counter
  sfr_TIM2.EGR.byte = 0x00;                      // clear pending events
  sfr_TIM2.PSCR.PSC = 7;                         // set clock to 16Mhz/2^7 = 125khz -> 8us period
  sfr_TIM2.ARRH.byte = (uint8_t)(timeout >> 8);  // set autoreload value
  sfr_TIM2.ARRL.byte = (uint8_t)timeout;         // period ~= (timeout + 1) * 8us
  sfr_TIM2.IER.UIE = 1;                          // enable timer 2 interrupt
  enableTim2();
}

void enableTim2(void){
  sfr_CLK.PCKENR1.PCKEN10 = 1;  // activate tim2 clock gate
  sfr_TIM2.CNTRH.byte = 0x00;   // MSB clear counter
  sfr_TIM2.CNTRL.byte = 0x00;   // LSB clear counter
  sfr_TIM2.IER.UIE = 1;         // enable timer 2 interrupt
  sfr_TIM2.CR1.CEN = 1;         // start the timer
}

void disableTim2(void){
  sfr_TIM2.IER.UIE = 0;         // disable interrupt
  sfr_TIM2.SR1.UIF = 0;         // clear pending interrupt flag
  sfr_TIM2.CR1.CEN = 0;         // disable timer
  sfr_CLK.PCKENR1.PCKEN10 = 0;  // disable tim2 clock gate
}

void initHall(void){
  // HALL_PWR (PA3) high: the hall sensor is GPIO power-gated on this board
  // and must stay powered through HALT - it is the wake source. GPIO state
  // is retained in halt, so this costs only the sensor's own current.
  sfr_PORTA.ODR.byte |= PIN3;
  sfr_PORTA.CR1.byte |= PIN3;
  sfr_PORTA.DDR.byte |= PIN3;

  // Hall output on PA2
  sfr_PORTA.DDR.byte &= ~PIN2; // Input
  sfr_PORTA.CR1.byte |= PIN2;  // Pull-up
  sfr_PORTA.CR2.byte |= PIN2;  // Interrupt enabled

  sfr_ITC_EXTI.CR1.P2IS = 3;   // Rising and falling edges
}

// The previous LED *MUST* be tuned off before lighting another
// This function automatically takes care of that
void setLed(uint8_t led){
  ledLow(prevLed);
  ledHigh(led);
  prevLed = led;
}

// Enable a specific LED
void ledHigh(uint8_t led) {
  uint8_t col = led / 9;
  uint8_t topElements = 9 - col;
  uint8_t row = 9 - (led % 9);
  if (topElements <= (9 - row)) {
    row--;
  }

  // Set column HIGH
  CPX_PORT[col]->DDR.byte |= CPX_PIN[col];
  CPX_PORT[col]->CR1.byte |= CPX_PIN[col];
  CPX_PORT[col]->ODR.byte |= CPX_PIN[col];
  // Set row LOW
  CPX_PORT[row]->DDR.byte |= CPX_PIN[row];
  CPX_PORT[row]->CR1.byte |= CPX_PIN[row];
  CPX_PORT[row]->ODR.byte &= ~CPX_PIN[row];
}

// Disable a specific LED
void ledLow(uint8_t led) {
  uint8_t col = led / 9;
  uint8_t topElements = 9 - col;
  uint8_t row = 9 - (led % 9);
  if (topElements <= (9 - row)) {
    row--;
  }

  // Set row to HI-Z
  CPX_PORT[row]->ODR.byte |= CPX_PIN[row];
  CPX_PORT[row]->DDR.byte &= ~CPX_PIN[row];
  CPX_PORT[row]->CR1.byte &= ~CPX_PIN[row];
  // Set column to HI-Z
  CPX_PORT[col]->ODR.byte &= ~CPX_PIN[col];
  CPX_PORT[col]->DDR.byte &= ~CPX_PIN[col];
  CPX_PORT[col]->CR1.byte &= ~CPX_PIN[col];
}

uint8_t accel_read_reg(uint8_t reg) {
  uint8_t val = 0;
  uint16_t t;
  
  sfr_I2C1.CR2.START = 1;
  t = 10000; while (!sfr_I2C1.SR1.SB && --t); if (!t) return 0xF1;
  
  sfr_I2C1.DR.byte = sc7a20_addr;
  t = 10000; while (!sfr_I2C1.SR1.ADDR && !(sfr_I2C1.SR2.byte & 0x04) && --t); 
  if (!sfr_I2C1.SR1.ADDR) { sfr_I2C1.SR2.byte &= ~0x04; sfr_I2C1.CR2.STOP = 1; return 0xF2; }
  (void)sfr_I2C1.SR1.byte;
  (void)sfr_I2C1.SR3.byte;
  
  sfr_I2C1.DR.byte = reg;
  t = 10000; while (!sfr_I2C1.SR1.TXE && !(sfr_I2C1.SR2.byte & 0x04) && --t); 
  if (!sfr_I2C1.SR1.TXE) { sfr_I2C1.SR2.byte &= ~0x04; sfr_I2C1.CR2.STOP = 1; return 0xF3; }
  
  sfr_I2C1.CR2.START = 1;
  t = 10000; while (!sfr_I2C1.SR1.SB && --t); if (!t) { sfr_I2C1.SR2.byte &= ~0x04; sfr_I2C1.CR2.STOP = 1; return 0xF4; }
  
  sfr_I2C1.DR.byte = sc7a20_addr | 0x01;
  t = 10000; while (!sfr_I2C1.SR1.ADDR && !(sfr_I2C1.SR2.byte & 0x04) && --t); 
  if (!sfr_I2C1.SR1.ADDR) { sfr_I2C1.SR2.byte &= ~0x04; sfr_I2C1.CR2.STOP = 1; return 0xF5; }
  
  sfr_I2C1.CR2.ACK = 0;
  (void)sfr_I2C1.SR1.byte;
  (void)sfr_I2C1.SR3.byte;
  
  sfr_I2C1.CR2.STOP = 1;
  
  t = 10000; while (!sfr_I2C1.SR1.RXNE && --t); 
  if (t) val = sfr_I2C1.DR.byte;
  else val = 0xF6;
  
  return val;
}

// STATUS_REG reads of 0xF0+ are either our own I2C error codes (0xF1-0xF6)
// or 0xFF, which is what a floating bus reads as; both have shown up as
// one-sample glitches in bench telemetry. Retry once before believing it.
uint8_t accel_read_status(void) {
  uint8_t s = accel_read_reg(0x27);
  if (s >= 0xF0) {
    s = accel_read_reg(0x27);
  }
  return s;
}

void accel_write_reg(uint8_t reg, uint8_t val) {
  uint16_t t;
  
  sfr_I2C1.CR2.START = 1;
  t = 10000; while (!sfr_I2C1.SR1.SB && --t); if (!t) return;
  
  sfr_I2C1.DR.byte = sc7a20_addr;
  t = 10000; while (!sfr_I2C1.SR1.ADDR && !(sfr_I2C1.SR2.byte & 0x04) && --t); 
  if (!sfr_I2C1.SR1.ADDR) { sfr_I2C1.SR2.byte &= ~0x04; sfr_I2C1.CR2.STOP = 1; return; }
  (void)sfr_I2C1.SR1.byte;
  (void)sfr_I2C1.SR3.byte;
  
  sfr_I2C1.DR.byte = reg;
  t = 10000; while (!sfr_I2C1.SR1.TXE && !(sfr_I2C1.SR2.byte & 0x04) && --t); 
  if (!sfr_I2C1.SR1.TXE) { sfr_I2C1.SR2.byte &= ~0x04; sfr_I2C1.CR2.STOP = 1; return; }
  
  sfr_I2C1.DR.byte = val;
  t = 10000; while (!sfr_I2C1.SR1.TXE && !(sfr_I2C1.SR2.byte & 0x04) && --t); 
  if (!sfr_I2C1.SR1.TXE) { sfr_I2C1.SR2.byte &= ~0x04; sfr_I2C1.CR2.STOP = 1; return; }
  
  // Wait for the shift register to physically finish pushing bits to wire
  t = 10000; while (!sfr_I2C1.SR1.BTF && !(sfr_I2C1.SR2.byte & 0x04) && --t);

  // Command physical STOP 
  sfr_I2C1.CR2.STOP = 1;
  // Tightly block until STOP sequence clears cleanly from the hardware bus
  t = 10000; while ((sfr_I2C1.CR2.byte & 0x02) && --t);
}

void printAccelConfig(const char *label) {
#if ACCEL_DIAGNOSTICS
  uint8_t who = accel_read_reg(0x0F);
  uint8_t ctrl1 = accel_read_reg(0x20);
  uint8_t ctrl4 = accel_read_reg(0x23);
  uint8_t status = accel_read_status();

  printf("ACCEL %s addr8=0x%02X who=0x%02X ctrl1=0x%02X ctrl4=0x%02X status=0x%02X\r\n",
         label, sc7a20_addr, who, ctrl1, ctrl4, status);

  if ((who & 0xF0) == 0xF0) {
    printf("ACCEL WARN: WHO_AM_I read looks like I2C timeout code\r\n");
  }
  if ((ctrl1 & 0x07) != 0x07) {
    printf("ACCEL WARN: XYZ axis enable bits are not all set\r\n");
  }
  if ((ctrl1 & 0xF0) == 0x00) {
    printf("ACCEL WARN: ODR bits show power-down\r\n");
  }
  if ((ctrl4 & 0x80) == 0x00) {
    printf("ACCEL WARN: BDU bit is not set\r\n");
  }
#else
  (void)label;
#endif
}

void resetAccelBus(void) {
  sfr_I2C1.CR1.PE = 0;
  sfr_I2C1.CR2.SWRST = 1;
  for(uint16_t i=0; i<1000; i++) NOP();
  sfr_I2C1.CR2.SWRST = 0;
  for(uint16_t i=0; i<1000; i++) NOP();

  initAccel();
  accelConfig();
}

uint8_t readAccelRaw(int8_t *x, int8_t *y, int8_t *z) {
  uint16_t t;
  
  sfr_I2C1.CR2.START = 1;
  t = 10000; while (!sfr_I2C1.SR1.SB && --t); if (!t) return 0;
  
  sfr_I2C1.DR.byte = sc7a20_addr;
  t = 10000; while (!sfr_I2C1.SR1.ADDR && !(sfr_I2C1.SR2.byte & 0x04) && --t); 
  if (!sfr_I2C1.SR1.ADDR) { sfr_I2C1.SR2.byte &= ~0x04; sfr_I2C1.CR2.STOP = 1; return 0; }
  (void)sfr_I2C1.SR1.byte;
  (void)sfr_I2C1.SR3.byte;
  
  // 0x28 is OUT_X_L. MSB set (0x80) -> 0xA8 for auto-increment read in LIS2DH12/SC7A20
  sfr_I2C1.DR.byte = 0xA8; 
  t = 10000; while (!sfr_I2C1.SR1.TXE && !(sfr_I2C1.SR2.byte & 0x04) && --t); 
  if (!sfr_I2C1.SR1.TXE) { sfr_I2C1.SR2.byte &= ~0x04; sfr_I2C1.CR2.STOP = 1; return 0; }
  
  sfr_I2C1.CR2.START = 1;
  t = 10000; while (!sfr_I2C1.SR1.SB && --t); if (!t) { sfr_I2C1.SR2.byte &= ~0x04; sfr_I2C1.CR2.STOP = 1; return 0; }
  
  sfr_I2C1.DR.byte = sc7a20_addr | 0x01;
  t = 10000; while (!sfr_I2C1.SR1.ADDR && !(sfr_I2C1.SR2.byte & 0x04) && --t); 
  if (!sfr_I2C1.SR1.ADDR) { sfr_I2C1.SR2.byte &= ~0x04; sfr_I2C1.CR2.STOP = 1; return 0; }
  
  sfr_I2C1.CR2.ACK = 1; // enable ACK for multi-byte read
  (void)sfr_I2C1.SR1.byte;
  (void)sfr_I2C1.SR3.byte;
  
  uint8_t buf[6];
  for (uint8_t i = 0; i < 6; i++) {
    if (i == 5) { // last byte
      sfr_I2C1.CR2.ACK = 0;
      sfr_I2C1.CR2.STOP = 1;
    }
    t = 10000; while (!sfr_I2C1.SR1.RXNE && --t); 
    if (t) buf[i] = sfr_I2C1.DR.byte;
    else {
      sfr_I2C1.CR2.ACK = 0;
      sfr_I2C1.CR2.STOP = 1;
      return 0;
    }
  }
  
  // Data in SC7A20 Normal mode (10-bit) is left-justified.
  // OUT_X_H = buf[1] (upper 8 bits)
  *x = (int8_t)buf[1];
  *y = (int8_t)buf[3];
  *z = (int8_t)buf[5];
  return 1;
}

void initAccel(void) {
  sfr_CLK.PCKENR1.PCKEN13 = 1; 

  // Reset peripheral before re-enabling
  sfr_I2C1.CR1.PE = 0;

  // Set PC0/PC1 as Inputs, WITH Internal Pull-Ups enabled.
  // The HW schematic lacks external pull-ups; I2C will fail without this.
  sfr_PORTC.DDR.byte &= ~((1<<0) | (1<<1));
  sfr_PORTC.CR1.byte |= ((1<<0) | (1<<1));
  sfr_PORTC.CR2.byte &= ~((1<<0) | (1<<1));
  
  sfr_I2C1.CR1.PE = 0;
  sfr_I2C1.FREQR.FREQ = 16;
  sfr_I2C1.CCRH.byte = 0;
  sfr_I2C1.CCRL.byte = 0x50;
  sfr_I2C1.TRISER.TRISE = 17;
  sfr_I2C1.CR1.PE = 1;
  
  for(uint16_t i=0; i<10000; i++) NOP();
}

// One-stop sensor configuration, called at boot, after wake, and by the
// bus recovery path. Keep the SC7A20 click engine disabled; mode changes use
// the proven software tap detector on the 48 Hz jolt stream.
void accelConfig(void) {
  accel_write_reg(0x20, 0x47); // CTRL1: 50 Hz low-power, XYZ enabled
  accel_write_reg(0x21, 0x00); // CTRL2: no high-pass/click filter
  accel_write_reg(0x22, 0x00); // CTRL3: no interrupt routing
  accel_write_reg(0x23, 0x80); // CTRL4: block data update
  accel_write_reg(0x24, 0x00); // CTRL5: no interrupt latch
  accel_write_reg(0x38, 0x00); // CLICK_CFG: click engine disabled
}

// ---- Solid mode transitions ----
// Switching modes is pure display state; the sensor keeps running identically
// in both modes and the software tap detector performs the mode change.
void enterSolidMode(void) {
  solid_breath_tick = 0;   // every entry starts at the dim floor
  solid_breath_phase_q4 = 0;
  solid_frames = 0;
  solid_wake_hold = 0;     // dark hold is only for waking from sleep
  solid_breath = 1;
  solid_group_source = 0;
  solid_group_slot = 0;
  display_mode = 1;
}

void exitSolidMode(void) {
  jolt_skip = 1;
  display_mode = 0;
}

uint8_t nextTapDisplayMode(uint8_t m) {
  // motion+bass -> oscilloscope -> motion+bass
  return (uint8_t)((m + 1) % DISPLAY_MODE_COUNT);
}

// One place for every mode transition (double-tap rotation, shell
// commands, sleep): tears down the current mode - powering the mic off
// when leaving a mic mode - and sets up the target.
//   0 motion comet with the bass envelope layered on top
//   1 mic oscilloscope
// The mic is powered in both modes, so transitions are display state only.
void setDisplayMode(uint8_t m) {
  if (display_mode == m) {
    return;
  }
  solid_frames = 0;
  solid_breath = 1;
  bass_extent = 0;
  if (m >= 1) {
    display_mode = m;
    return;
  }
  jolt_skip = 1; // back to motion: first polled delta is stale
  display_mode = 0;
}

void uartStashPoll(void) {
  if (sfr_USART1.SR.RXNE) {
    uint8_t next = (uint8_t)((uart_stash_head + 1) & 3);
    if (next != uart_stash_tail) {
      uart_stash[uart_stash_head] = sfr_USART1.DR.byte;
      uart_stash_head = next;
    } else {
      (void)sfr_USART1.DR.byte; // ring full: drop rather than deadlock
    }
  }
}

// ---- PDM microphone over SPI1 ----
// The xlMic board routes the PDM mic onto SPI1's natural pins: PDM_CLK is
// PB5 (SPI1_SCK) and PDM_DTA is PB7 (SPI1_MISO). Running SPI1 as a master
// in receive-only mode turns SCK into a free-running 1 MHz PDM clock
// (16 MHz / 16, within the mic's 1.0-3.25 MHz spec) while the mic's 1-bit
// sigma-delta stream arrives in DR a byte at a time. Receive-only mode
// never drives MOSI, so PB6 (charlieplex line CPX-4) is untouched.
// No decimation filter fits this MCU comfortably; instead each burst is
// reduced to ones-density statistics: silence sits near 50 percent density,
// a stuck data line reads 0 or 100 percent, and audio shows up as density
// variation between sub-blocks inside one 2 ms burst.

static const uint8_t popcount_lut[256] = {
  0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4,
  1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
  1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
  2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
  1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
  2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
  2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
  3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
  1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
  2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
  2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
  3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
  2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
  3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
  3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
  4, 5, 5, 6, 5, 6, 6, 7, 5, 6, 6, 7, 6, 7, 7, 8
};

#define MIC_SUB_BLOCK_BYTES 30  // 240 PDM bits per sub-block
#define MIC_SUB_BLOCKS 8        // 8 sub-blocks = 1920 bits = 1.92 ms at 1 MHz

void micPowerOn(void) {
  sfr_PORTD.ODR.byte |= PIN4;   // PWR_MIC on
  sfr_PORTB.DDR.byte &= ~PIN7;  // PDM_DTA -> MISO input, mic drives it
  sfr_PORTB.CR1.byte &= ~PIN7;
  // PDM_CLK -> SCK push-pull, fast slope. DDR before CR2: on an input pin
  // CR2 is the external-interrupt enable, and PB5 has no handler - setting
  // it even momentarily while the pin is still an input hangs the MCU on
  // an unhandled vector.
  sfr_PORTB.ODR.byte &= ~PIN5;
  sfr_PORTB.CR1.byte |= PIN5;
  sfr_PORTB.DDR.byte |= PIN5;
  sfr_PORTB.CR2.byte |= PIN5;

  sfr_CLK.PCKENR1.PCKEN14 = 1;  // SPI1 clock
  sfr_SPI1.CR1.byte = 0;
  sfr_SPI1.CR2.byte = 0;
  sfr_SPI1.CR2.SSM = 1;         // no NSS pin: PB4 stays a charlieplex line
  sfr_SPI1.CR2.SSI = 1;
  sfr_SPI1.CR2.RXONLY = 1;      // clock runs while SPE=1; MOSI never driven
  sfr_SPI1.CR1.BR = 3;          // 16 MHz / 16 = 1 MHz
  sfr_SPI1.CR1.MSTR = 1;
  // Free-run the clock from power-on: PDM mics shut their modulator down
  // whenever the clock stops and need ~ms of clocking to produce valid
  // data again, so a gated clock reads as all-zeros. Nobody reads DR
  // between bursts; each burst drains the overrun state first.
  sfr_SPI1.CR1.SPE = 1;
}

void micPowerOff(void) {
  sfr_SPI1.CR1.SPE = 0;
  sfr_CLK.PCKENR1.PCKEN14 = 0;
  sfr_PORTD.ODR.byte &= ~PIN4;  // PWR_MIC off
  // Park both PDM lines driven low again (unpowered mic never contends)
  sfr_PORTB.DDR.byte |= (PIN5 | PIN7); // outputs first
  sfr_PORTB.CR2.byte &= ~PIN5;         // then drop the slope/EXTI bit
  sfr_PORTB.ODR.byte &= ~(PIN5 | PIN7);
  sfr_PORTB.CR1.byte |= (PIN5 | PIN7);
}

// One 1.92 ms PDM burst. Returns 1 on success; density_out is the mean
// ones-count per 240-bit sub-block (silence ~120), spread_out the max-min
// across the 8 sub-blocks (audio activity). Pauses the render ISR: bytes
// arrive every 8 us and the 128 us render interrupt would force overruns.
uint8_t micBurst(uint8_t *density_out, uint8_t *spread_out) {
  uint8_t sub_ones[MIC_SUB_BLOCKS];
  uint16_t total = 0;
  uint16_t t;
  uint8_t render_paused = 0;

  if (sfr_TIM2.IER.UIE) {
    render_paused = 1;
    sfr_TIM2.IER.UIE = 0;
    cpxAllHiZ();
  }

  // The clock has been free-running with nobody reading: clear the overrun
  // (read DR then SR) and discard the stale byte so the stream is fresh.
  (void)sfr_SPI1.DR.byte;
  (void)sfr_SPI1.SR.byte;
  (void)sfr_SPI1.DR.byte;

  for (uint8_t blk = 0; blk < MIC_SUB_BLOCKS; blk++) {
    uint16_t ones = 0;
    for (uint8_t i = 0; i < MIC_SUB_BLOCK_BYTES; i++) {
      t = 2000; while (!sfr_SPI1.SR.RXNE && --t);
      if (!t) {
        if (render_paused) { sfr_TIM2.SR1.UIF = 0; sfr_TIM2.IER.UIE = 1; }
        return 0; // SPI never delivered: report failure, don't hang
      }
      ones += popcount_lut[sfr_SPI1.DR.byte];
    }
    sub_ones[blk] = (uint8_t)ones;
    total += ones;
    uartStashPoll();
  }

  if (render_paused) {
    sfr_TIM2.SR1.UIF = 0;
    sfr_TIM2.IER.UIE = 1;
  }

  uint8_t mn = 255, mx = 0;
  for (uint8_t blk = 0; blk < MIC_SUB_BLOCKS; blk++) {
    if (sub_ones[blk] < mn) mn = sub_ones[blk];
    if (sub_ones[blk] > mx) mx = sub_ones[blk];
  }
  *density_out = (uint8_t)(total / MIC_SUB_BLOCKS);
  *spread_out = mx - mn;
  return 1;
}

// Circular oscilloscope, replicating the master-branch analog-mic effect
// (its ADC ISR did setLed(offset + sample) per conversion): each 192-bit
// PDM window is one amplitude sample, drawn directly as a ring position.
// 96 windows = ~18 ms per frame with the render ISR paused; persistence
// of vision fuses the dots into a waveform arc whose width is loudness.
// Returns the frame's peak deviation for diagnostics.
uint8_t micScopeFrame(void) {
  static uint16_t dc_q4 = 96 << 4; // slow DC tracker (~50 ms), Q4
  uint8_t render_was_on = 0;
  uint8_t peak = 0;
  uint16_t t;

  if (sfr_TIM2.IER.UIE) {
    render_was_on = 1;
    sfr_TIM2.IER.UIE = 0;
  }

  (void)sfr_SPI1.DR.byte; // drain overrun from the free-running clock
  (void)sfr_SPI1.SR.byte;
  (void)sfr_SPI1.DR.byte;

  for (uint8_t w = 0; w < 96; w++) {
    uint8_t ones = 0;
    for (uint8_t i = 0; i < 24; i++) {
      t = 2000; while (!sfr_SPI1.SR.RXNE && --t);
      if (!t) {
        if (render_was_on) { sfr_TIM2.SR1.UIF = 0; sfr_TIM2.IER.UIE = 1; }
        return 0xFF; // SPI dead marker
      }
      ones += popcount_lut[sfr_SPI1.DR.byte];
    }
    dc_q4 += (uint16_t)(((int16_t)((uint16_t)ones << 4) - (int16_t)dc_q4) >> 8);
    int16_t dev = (int16_t)ones - (int16_t)(dc_q4 >> 4);
    if (dev > peak) peak = (uint8_t)dev;
    else if (-dev > peak) peak = (uint8_t)(-dev);
    if (mic_warmup == 0) {
      // Centered on the gravity low point, like the comet and the bass arc.
      int16_t led = (int16_t)base_led + (dev * 2); // x2 gain, wraps mod 90
      while (led < 0) led += 90;
      while (led >= 90) led -= 90;
      setLed((uint8_t)led);
    }
    uartStashPoll();
  }

  if (render_was_on) {
    sfr_TIM2.SR1.UIF = 0;
    sfr_TIM2.IER.UIE = 1;
  }
  return peak;
}

// Bass envelope frame: eight contiguous 1 ms sub-windows (1000 bits each).
// The long sub-window is a strong low-pass: sigma-delta noise averages out
// (idle spread is a few counts vs ~12 for 192-bit windows) and the 1 kHz
// sub-window rate makes the density trace sensitive to roughly 60-500 Hz -
// the bass/low-mid band the VU mode's 240-bit blocks are blind to. Returns
// the peak-to-peak density spread across the 8 ms.
uint8_t micEnvFrame(uint16_t *spread_out) {
  uint16_t mn = 0xFFFF, mx = 0;
  uint16_t t;
  uint8_t render_paused = 0;

  if (sfr_TIM2.IER.UIE) {
    render_paused = 1;
    sfr_TIM2.IER.UIE = 0;
    cpxAllHiZ();
  }

  (void)sfr_SPI1.DR.byte; // drain overrun from the free-running clock
  (void)sfr_SPI1.SR.byte;
  (void)sfr_SPI1.DR.byte;

  for (uint8_t w = 0; w < 8; w++) {
    uint16_t ones = 0;
    for (uint8_t i = 0; i < 125; i++) {
      t = 2000; while (!sfr_SPI1.SR.RXNE && --t);
      if (!t) {
        if (render_paused) { sfr_TIM2.SR1.UIF = 0; sfr_TIM2.IER.UIE = 1; }
        return 0;
      }
      ones += popcount_lut[sfr_SPI1.DR.byte];
    }
    if (ones < mn) mn = ones;
    if (ones > mx) mx = ones;
    uartStashPoll();
  }

  if (render_paused) {
    sfr_TIM2.SR1.UIF = 0;
    sfr_TIM2.IER.UIE = 1;
  }
  *spread_out = mx - mn;
  return 1;
}

// ---- Live tuning shell ----
// Newline-terminated single-letter commands over the UART:
//   d<n>  jolt deadzone      s<n>  charge scale divisor
//   t<n>  charge start       b<n>  burst threshold
//   g     toggle grouped solid rendering
//   p     print config       w     persist config to data EEPROM
#define CFG_EEPROM ((volatile uint8_t *)EEPROM_ADDR_START)
#define CFG_MAGIC 0xA5

void printCfg(void) {
  printf("cfg d=%u s=%u t=%u b=%u tap=%u/%u/%u\r\n",
         cfg_jolt_deadzone, cfg_charge_divisor, cfg_charge_start, cfg_charge_burst,
         TAP_JOLT_MIN, TAP_WINDOW_SAMPLES, TAP_GAP_SAMPLES);
  printf("cpx group=%u max_sinks=%u blocked=%u\r\n",
         cpx_group_enabled, GROUP_MAX_SINKS, cpx_unsafe_group_blocks);
}

void cfgLoad(void) {
  if (CFG_EEPROM[0] != CFG_MAGIC) {
    return; // nothing saved yet; keep compiled-in defaults
  }
  cfg_jolt_deadzone = CFG_EEPROM[1];
  cfg_charge_divisor = CFG_EEPROM[2] ? CFG_EEPROM[2] : 1;
  cfg_charge_start = CFG_EEPROM[3];
  cfg_charge_burst = CFG_EEPROM[4];
}

static void cfgWriteByte(volatile uint8_t *addr, uint8_t val) {
  uint16_t t;
  if (*addr == val) {
    return; // spare EEPROM wear on unchanged bytes
  }
  *addr = val;
  t = 10000; while (!sfr_FLASH.IAPSR.EOP && --t);
}

void cfgSave(void) {
  uint16_t t;
  sfr_FLASH.DUKR.byte = 0xAE;
  sfr_FLASH.DUKR.byte = 0x56;
  t = 10000; while (!sfr_FLASH.IAPSR.DUL && --t);
  if (!t) {
    printf("eeprom unlock failed\r\n");
    return;
  }
  // Values first, magic last, so a power cut mid-save can't leave a valid
  // magic pointing at torn data.
  cfgWriteByte(&CFG_EEPROM[1], cfg_jolt_deadzone);
  cfgWriteByte(&CFG_EEPROM[2], cfg_charge_divisor);
  cfgWriteByte(&CFG_EEPROM[3], cfg_charge_start);
  cfgWriteByte(&CFG_EEPROM[4], cfg_charge_burst);
  cfgWriteByte(&CFG_EEPROM[0], CFG_MAGIC);
  sfr_FLASH.IAPSR.DUL = 0; // relock
  printf("saved\r\n");
}

void pollUartCmd(void) {
  static char buf[6];
  static uint8_t len = 0;

  char c;
  if (uart_stash_tail != uart_stash_head) {
    c = (char)uart_stash[uart_stash_tail];
    uart_stash_tail = (uint8_t)((uart_stash_tail + 1) & 3);
  } else if (sfr_USART1.SR.RXNE) {
    c = sfr_USART1.DR.byte;
  } else {
    return;
  }

  if (c != '\r' && c != '\n') {
    if (c < ' ' || c > '~') {
      return; // line noise / port-open glitch bytes: ignore
    }
    if (len < sizeof(buf)) {
      buf[len++] = c;
    } else {
      len = 0; // overlong line: discard
    }
    return;
  }

  if (len == 0) {
    return;
  }
  if (buf[0] == 'p' && len == 1) {
    printCfg();
  } else if (buf[0] == 'g' && len == 1) {
    cpx_group_enabled = cpx_group_enabled ? 0 : cpxLayoutSafe();
    cpxAllHiZ();
    printCfg();
  } else if (buf[0] == 'o' && len == 1) {
    setDisplayMode((display_mode == 1) ? 0 : 1);
    printf("mode=%u\r\n", display_mode);
  } else if (buf[0] == 'a' && len == 1) {
    // One-shot mic liveness test: healthy silence sits near dens=120/240
    // with a small spread; a stuck data line reads dens=0 or dens=240.
    uint8_t was_on = (display_mode >= 1);
    uint8_t dens = 0, spread = 0, ok = 0;
    if (!was_on) {
      micPowerOn();
    }
    for (uint8_t i = 0; i < 25; i++) {
      ok = micBurst(&dens, &spread); // ~50 ms of clocked warmup
    }
    printf(ok ? "MIC alive dens=%u/240 spread=%u\r\n"
              : "MIC dead (SPI timeout) dens=%u spread=%u\r\n",
           dens, spread);
    if (!was_on) {
      micPowerOff();
    }
  } else if (buf[0] == 'w' && len == 1) {
    cfgSave();
    printCfg();
  } else {
    uint16_t v = 0;
    uint8_t ok = (len > 1);
    for (uint8_t i = 1; i < len; i++) {
      if (buf[i] < '0' || buf[i] > '9') {
        ok = 0;
        break;
      }
      v = (v * 10) + (buf[i] - '0');
    }
    if (ok && v <= 255) {
      switch (buf[0]) {
        case 'd': cfg_jolt_deadzone = (uint8_t)v; break;
        case 's': cfg_charge_divisor = v ? (uint8_t)v : 1; break;
        case 't': cfg_charge_start = (uint8_t)v; break;
        case 'b': cfg_charge_burst = (uint8_t)v; break;
        default: ok = 0;
      }
    } else {
      ok = 0;
    }
    if (ok) {
      printCfg();
    } else {
      printf("? d/s/t/b<0-255> g o l a p w\r\n");
    }
  }
  len = 0;
}

void initUart(void) {
  sfr_CLK.PCKENR1.PCKEN15 = 1;

  sfr_PORTC.DDR.byte |= (1<<3);
  sfr_PORTC.CR1.byte |= (1<<3);
  sfr_PORTC.CR2.byte |= (1<<3);

  sfr_PORTC.DDR.byte &= ~(1<<2);
  sfr_PORTC.CR1.byte |= (1<<2);

  sfr_USART1.BRR2.byte = 0x03; 
  sfr_USART1.BRR1.byte = 0x68;

  sfr_USART1.CR2.TEN = 1;
  sfr_USART1.CR2.REN = 1;
}

int putchar(int c) {
  sfr_USART1.DR.byte = c;
  while (!sfr_USART1.SR.TXE);
  return c;
}
