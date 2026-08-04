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
// Rotates the atan2 gravity vector onto the earring's lowest LED, so the
// comet rests at the bottom of the hoop (was 68 = topmost, bubble-style).
#define BUBBLE_LEVEL_OFFSET 23
#define COMET_TAIL 8
#define TILT_DEADZONE 3
#define JOLT_DEADZONE 10
#define STILL_ENERGY_THRESHOLD 10
#define MOTION_CHARGE_START 8
#define MOTION_CHARGE_BURST 20
// Multiplicative keep-factor (Q8): 152/256 ≈ 0.59, so ~41% of charge is
// lost each 48 Hz sample — 2x the old 204/256 (20%) decay rate.
#define MOTION_CHARGE_DECAY 152
// Smaller divisor = faster proportional energy drain (was 183; half ≈ 2x).
#define ENERGY_DECAY_DIVISOR 91
#define MOTION_CHARGE_SCALE_DIVISOR 25
#define MOTION_CHARGE_INPUT_MAX 120
#define ACCEL_SAMPLE_TICKS 163
#define SERIAL_DIAGNOSTICS 1
#define ACCEL_DIAGNOSTICS SERIAL_DIAGNOSTICS
#define MIC_MODE_DIAGNOSTICS 0
#define ACCEL_DIAG_PRINT_SAMPLES 48
#define BASS_EXTENT_MAX (LED_COUNT / 2)  // 45: left/right arcs meet → full ring

// Prototypes
void setLed(uint8_t led);
void ledHigh(uint8_t led);
void ledLow(uint8_t led);
void cpxAllHiZ(void);
uint8_t cpxLayoutSafe(void);
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
void accelConfig(void);
void micPowerOn(void);
void micPowerOff(void);
uint8_t micBurst(uint8_t *density_out, uint8_t *spread_out);
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
volatile uint8_t solid_frames = 0;        // 48 Hz frame pulses from the ISR
volatile uint8_t bass_extent = 0;         // Bass arc half-width (0 silent, 45 = full ring)
volatile uint8_t bass_glow = 0;           // Bass arc brightness, 0-32
// Q8 reciprocal of bass_extent (256/extent), precomputed at 24 Hz so the
// render ISR can turn glow_dist into a 0..255 radial without a divide.
volatile uint8_t bass_inv_extent_q8 = 0;
volatile uint8_t mic_warmup = 0;          // Frames until the PDM mic's modulator output is trusted

// The USART has a single-byte buffer and mic captures blank the main loop
// for up to 18 ms while shell characters arrive 1 ms apart: capture loops
// stash incoming bytes between sub-windows so commands are not dropped.
// 4-deep ring: a command is 2 bytes and up to two can pile up per frame.
volatile uint8_t uart_stash[4];
volatile uint8_t uart_stash_head = 0;
volatile uint8_t uart_stash_tail = 0;
volatile uint8_t isr_frames = 0;          // Free-running 48 Hz frame counter

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
volatile uint8_t cpx_group_enabled = 1;
volatile uint8_t burst_pos = 0;
volatile uint8_t burst_radius = 0;
volatile uint8_t burst_life = 0;

// Comet tail falloff (full peak 32 * (9-d)/9). File-scope so the ISR
// does not re-materialize the table on every entry.
static const uint8_t comet_tail_lut[COMET_TAIL + 1] = {
  32, 28, 25, 21, 18, 14, 11, 7, 4
};

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
    sfr_TIM2.SR1.UIF = 0;
    return;
  }

  time_ticks++;

  // 48 Hz frame clock: paces mic envelope and accelerometer work.
  static uint16_t frame_div = 0;
  if (++frame_div >= ACCEL_SAMPLE_TICKS) {
    frame_div = 0;
    isr_frames++;
    solid_frames++;
  }

  // Snapshot volatiles once — the ISR used to re-read them on every branch.
  uint8_t led_to_light = nextPwmScanLed();
  uint8_t base = base_led;
  uint8_t still = still_mode;
  uint8_t e = energy;
  uint8_t bext = bass_extent;
  uint8_t bglow = bass_glow;
  uint8_t binv = bass_inv_extent_q8;
  uint8_t gwidth = glow_width_v;
  uint16_t gscale = glow_scale_v;
  uint8_t fthresh = frag_threshold_v;
  uint8_t sett = settle_extent;
  int8_t cdir = comet_dir;
  uint8_t ticks = time_ticks;

  uint8_t brightness;
  uint8_t glow_dist = ringDistance(led_to_light, base);

  if (still) {
    // Comet at the gravity low point: full-bright head, directional tail.
    brightness = 0;
    if (glow_dist == 0) {
      brightness = FULL_BRIGHTNESS;
    } else if (glow_dist <= COMET_TAIL) {
      uint8_t fwd = (led_to_light >= base)
                        ? (led_to_light - base)
                        : (led_to_light + LED_COUNT - base);
      uint8_t on_tail = (cdir > 0) ? (fwd >= LED_COUNT - COMET_TAIL)
                                   : (fwd <= COMET_TAIL);
      if (on_tail) {
        brightness = comet_tail_lut[glow_dist];
      } else if (glow_dist == 1) {
        brightness = FULL_BRIGHTNESS >> 2; // faint leading nose
      }
    }
    // Settling drain: former motion arc collapses into the comet.
    if (brightness == 0 && glow_dist <= sett) {
      brightness = 2;
    }
  } else {
    brightness = breathing_brightness;
  }

  if (!still && burst_life > 0) {
    uint8_t burst_forward = burst_pos + burst_radius;
    if (burst_forward >= LED_COUNT) {
      burst_forward -= LED_COUNT; // radius steps by 3; one subtract is enough
    }
    uint8_t burst_back = (burst_radius > burst_pos)
                             ? (uint8_t)(burst_pos + LED_COUNT - burst_radius)
                             : (uint8_t)(burst_pos - burst_radius);
    if (ringDistance(led_to_light, burst_forward) <= 1 ||
        ringDistance(led_to_light, burst_back) <= 1) {
      brightness = FULL_BRIGHTNESS;
    }
  }

  // Sparks: orbiting embers when charged, jolt dust when still.
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

  if (!still && glow_dist <= gwidth) {
    brightness += (uint8_t)(((uint16_t)(gwidth - glow_dist) * gscale) >> 8);
    if (brightness > FULL_BRIGHTNESS) {
      brightness = FULL_BRIGHTNESS;
    }
  }

  // Cheap temporal hash instead of LFSR: same visual noise, far less ISR work.
  uint8_t hash = (uint8_t)(led_to_light * 37u + ticks);

  if (!still && e > 12) {
    if (hash < e) {
      brightness += (hash & 0x07) + (e >> 6);
      if (brightness > FULL_BRIGHTNESS) {
        brightness = FULL_BRIGHTNESS;
      }
    }
  }

  // Fragmentation breaks the ring as movement energy rises.
  if (!still && e > 36 && brightness < FULL_BRIGHTNESS && glow_dist > 2) {
    uint8_t mask_pos = ((led_to_light * 5) + ticks + (e >> 3)) & 0x1F;
    if (mask_pos >= fthresh) {
      brightness = 0;
    }
  }

  // Sound layer: bass arc from the gravity low point. Lower ~50% solid;
  // upper half fades + sparkles. Full volume → half-width 45 → full ring.
  if (bext > 0) {
    uint8_t ab = 0;
    if (bext >= BASS_EXTENT_MAX || glow_dist < bext) {
      ab = bglow;
    } else if (glow_dist == bext) {
      ab = bglow >> 2;
    }

    if (ab > 0 && glow_dist > 0 && binv > 0) {
      uint16_t radial = (uint16_t)glow_dist * binv;
      if (radial > 255) {
        radial = 255;
      }

      // Upper half only: fade, shimmer, fragmentation.
      if (radial > 128) {
        uint8_t upper = (uint8_t)(radial - 128);
        if (bext > 12) {
          uint8_t fade = (uint8_t)(((uint16_t)upper * (bext - 12)) >> 5);
          if (fade >= ab) {
            ab = ab >> 2;
          } else {
            ab -= fade;
          }
        }

        if (bext > 10) {
          uint8_t chance = (uint8_t)(((uint16_t)bext * upper) >> 5);
          if (hash < chance) {
            ab += (hash & 0x07) + (bext >> 3);
            if (ab > FULL_BRIGHTNESS) {
              ab = FULL_BRIGHTNESS;
            }
          }

          if (bext > 18 && ab < FULL_BRIGHTNESS && glow_dist > 2) {
            uint8_t mask_pos = ((led_to_light * 5) + ticks + (bext >> 2)) & 0x1F;
            uint8_t thresh = 30 - (uint8_t)(((uint16_t)upper * 18) >> 7) - (bext >> 3);
            if (thresh < 10) {
              thresh = 10;
            }
            if (mask_pos >= thresh) {
              ab = 0;
            }
          }
        }
      }
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

  sfr_TIM2.SR1.UIF = 0;
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
      burst_life = 0;
      bass_extent = 0;
      bass_glow = 0;
      bass_inv_extent_q8 = 0;

      enableTim2();
      setLed(prevLed);
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

      if (!sleep && solid_frames > 0) {
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
            bass_glow = 0;
            bass_inv_extent_q8 = 0;
          } else if (ok) {
            if (spread < env_floor) {
              env_floor = spread;       // snap down to quieter floor
            } else if (++floor_tick >= 32) {
              floor_tick = 0;
              env_floor++;              // creep up slowly: sustained music
                                        // must not get eaten as "noise"
            }
            uint16_t sig = (spread > env_floor) ? (spread - env_floor) : 0;
            // 50% more mic sensitivity: amplify residual over the noise floor.
            sig = (uint16_t)((sig * 3) >> 1);
            if (sig > envelope) {
              envelope = sig;                       // instant attack
            } else {
              envelope -= (envelope >> 3);          // exponential release
              if (envelope > 0) envelope--;
            }

            // Map envelope → target extent/glow, then ease current values
            // toward the targets so the arc doesn't jump frame-to-frame.
            uint8_t target_ext = 0;
            uint8_t target_glow = 0;
            if (envelope >= 3) {
              uint16_t ev = envelope;
              // Soft midrange, reaches full ring (45) when loud.
              uint16_t ext = 2 + ((uint16_t)ev * 50) / (ev + 20);
              if (ext > BASS_EXTENT_MAX) {
                ext = BASS_EXTENT_MAX;
              }
              target_ext = (uint8_t)ext;
              uint16_t g = 6 + ((uint16_t)ev * 20) / (ev + 20);
              target_glow = (g > 26) ? 26 : (uint8_t)g;
            }
            // Attack closes half the gap; release a quarter — snappy open,
            // smooth close without hard snaps to zero.
            if (target_ext > bass_extent) {
              bass_extent += (uint8_t)((target_ext - bass_extent + 1) >> 1);
            } else if (target_ext < bass_extent) {
              uint8_t d = (uint8_t)((bass_extent - target_ext + 3) >> 2);
              bass_extent -= d ? d : 1;
            }
            if (target_glow > bass_glow) {
              bass_glow += (uint8_t)((target_glow - bass_glow + 1) >> 1);
            } else if (target_glow < bass_glow) {
              uint8_t d = (uint8_t)((bass_glow - target_glow + 3) >> 2);
              bass_glow -= d ? d : 1;
            }
            bass_inv_extent_q8 = bass_extent ? (uint8_t)(256u / bass_extent) : 0;
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

      if (!sleep && (++accel_tick >= ACCEL_SAMPLE_TICKS)) {
        accel_tick = 0;

        int8_t rx = rx_prev;
        int8_t ry = ry_prev;
        int8_t rz = rz_prev;
        if (!readAccelRaw(&rx, &ry, &rz)) {
          resetAccelBus();
          if (readAccelRaw(&rx, &ry, &rz)) {
            goto accel_read_ok;
          }
#if ACCEL_DIAGNOSTICS
          accel_fail_count++;
          {
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
#if ACCEL_DIAGNOSTICS
        accel_ok_count++;
#endif
        static uint8_t zero_sample_count = 0;
        if (rx == 0 && ry == 0 && rz == 0) {
          if (zero_sample_count < 0xFF) {
            zero_sample_count++;
          }
          if (zero_sample_count >= 8) {
            resetAccelBus();
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

        // Apply a deadzone to filter sensor noise when still.
        int16_t active_jolt = 0;
        if (jolt > cfg_jolt_deadzone) {
          active_jolt = jolt - cfg_jolt_deadzone;
        }

        // 2. Build motion charge, then let charge feed visual energy.
        // Isolated jolts decay away; repeated steps build enough charge to wake the pattern.
        uint16_t next_charge = ((uint16_t)motion_charge * MOTION_CHARGE_DECAY) >> 8;
        if (active_jolt > 0) {
          uint8_t charge_input = (uint8_t)active_jolt;
          if (charge_input > MOTION_CHARGE_INPUT_MAX) {
            charge_input = MOTION_CHARGE_INPUT_MAX;
          }
          charge_input = (charge_input + (cfg_charge_divisor - 1)) / cfg_charge_divisor;
          next_charge += charge_input;
          if (next_charge > 255) {
            next_charge = 255;
          }
        }
        motion_charge = (uint8_t)next_charge;

        // Proportional energy decay: one div instead of a subtract-loop.
        // energy/91 per 48 Hz sample with fractional remainder.
        static uint16_t energy_decay_accum = 0;
        uint16_t next_energy = energy;
        energy_decay_accum += energy;
        {
          uint16_t dec = energy_decay_accum / ENERGY_DECAY_DIVISOR;
          energy_decay_accum -= dec * ENERGY_DECAY_DIVISOR;
          if (dec >= next_energy) {
            next_energy = 0;
            energy_decay_accum = 0;
          } else {
            next_energy -= dec;
          }
        }
        if (motion_charge > cfg_charge_start) {
          next_energy += motion_charge - cfg_charge_start;
        }
        if (next_energy > 255) next_energy = 255;
        energy = (uint8_t)next_energy;
        still_mode = (energy <= STILL_ENERGY_THRESHOLD && motion_charge <= cfg_charge_start);

        // Precompute ISR render params at 48 Hz (no division in the ISR).
        // energy/255 ≈ energy>>8 for the small coefficients here.
        glow_width_v = 16 - (uint8_t)(((uint16_t)energy * 9) >> 8);
        if (glow_width_v < 7) {
          glow_width_v = 7;
        }
        glow_scale_v = ((uint16_t)(18 + (energy >> 4)) << 8) / glow_width_v;
        if (energy > 36) {
          frag_threshold_v = 28 - (uint8_t)(((uint16_t)(energy - 36) * 20) / 219);
        } else {
          frag_threshold_v = 28;
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
            // energy*2/255 ≈ energy>>7
            uint8_t spring_shift = 4 - (energy >> 7);
            int16_t spring_impulse = error >> spring_shift;
            if (spring_impulse == 0 && error != 0) {
              spring_impulse = (error > 0) ? 1 : -1;
            }

            angular_velocity_q4 += spring_impulse;
            if (angular_velocity_q4 > 120) angular_velocity_q4 = 120;
            else if (angular_velocity_q4 < -120) angular_velocity_q4 = -120;

            // Damping ~0.92 at rest → ~0.82 when charged (energy*30/255 ≈ energy>>3 * 30/32)
            uint8_t damping = 238 - (uint8_t)(((uint16_t)energy * 30) >> 8);
            angular_velocity_q4 = ((int16_t)angular_velocity_q4 * (int16_t)damping) >> 8;
        } else {
            angular_velocity_q4 = ((int16_t)angular_velocity_q4 * 220) >> 8;
        }

        // Velocity is clamped to ±120, so a single wrap is always enough.
        halo_angle_q4 += angular_velocity_q4;
        if (halo_angle_q4 < 0) {
          halo_angle_q4 += 1440;
        } else if (halo_angle_q4 >= 1440) {
          halo_angle_q4 -= 1440;
        }

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
        // stopped drains into the low point (two LEDs/sample so the hand-off
        // feels continuous without lingering).
        if (!still_mode) {
          settle_extent = LED_COUNT / 2;
        } else if (settle_extent > 1) {
          settle_extent -= 2;
        } else {
          settle_extent = 0;
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

        // 5. Base ring brightness follows energy (shift approx of *30/255).
        breathing_brightness = 2 + (uint8_t)(((uint16_t)energy * 30) >> 8);


#if ACCEL_DIAGNOSTICS
        {
          accel_diag_tick++;
          if (accel_diag_tick >= ACCEL_DIAG_PRINT_SAMPLES) {
            accel_diag_tick = 0;
            printf("ACCEL ok=%u fail=%u raw=%d,%d,%d jolt=%d active=%d charge=%u tilt=%d energy=%u base=%u vel=%d br=%u status=0x%02X\r\n",
                   accel_ok_count, accel_fail_count,
                   (int)rx, (int)ry, (int)rz,
                   (int)jolt, (int)active_jolt, motion_charge, (int)planar_tilt,
                   energy, base_led, (int)angular_velocity_q4,
                   breathing_brightness, accel_read_status());
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
  printf("cfg d=%u s=%u t=%u b=%u cpx=%u\r\n",
         cfg_jolt_deadzone, cfg_charge_divisor, cfg_charge_start, cfg_charge_burst,
         cpx_group_enabled);
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
  } else if (buf[0] == 'a' && len == 1) {
    // One-shot mic liveness test: healthy silence sits near dens=120/240
    // with a small spread; a stuck data line reads dens=0 or dens=240.
    // The mic runs continuously now, so no power cycling here.
    uint8_t dens = 0, spread = 0, ok = 0;
    for (uint8_t i = 0; i < 25; i++) {
      ok = micBurst(&dens, &spread);
    }
    printf(ok ? "MIC alive dens=%u/240 spread=%u\r\n"
              : "MIC dead (SPI timeout) dens=%u spread=%u\r\n",
           dens, spread);
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
      printf("? d/s/t/b<0-255> g l a p w\r\n");
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
