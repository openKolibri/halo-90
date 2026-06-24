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
#define BUBBLE_LEVEL_OFFSET 68
#define TILT_DEADZONE 3
#define ACCEL_SAMPLE_TICKS 163
#define ACCEL_DIAGNOSTICS 1
#define ACCEL_DIAG_PRINT_SAMPLES 48

// Prototypes
void setLed(uint8_t led);
void ledHigh(uint8_t led);
void ledLow(uint8_t led);
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
void accel_write_reg(uint8_t reg, uint8_t val);
void printAccelConfig(const char *label);

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
volatile int16_t halo_angle_q4 = 0;       // Q4 angular position (0 to 1439)
volatile int16_t angular_velocity_q4 = 0; // Q4 angular velocity
volatile uint8_t time_ticks = 0;          // Dynamic mask reference counter
volatile uint8_t breathing_brightness = 32; // PWM duty cycle (0 to 32)
volatile uint8_t pwm_scan_led = 0;
volatile uint8_t pwm_scan_bank = 0;
volatile uint8_t pwm_phase = 0;
volatile uint8_t burst_pos = 0;
volatile uint8_t burst_radius = 0;
volatile uint8_t burst_life = 0;

// Orbiting spark particles
volatile uint8_t spark_pos[SPARK_COUNT] = {0, 0, 0, 0};
volatile int8_t spark_vel[SPARK_COUNT] = {0, 0, 0, 0};
volatile uint8_t spark_life[SPARK_COUNT] = {0, 0, 0, 0};

// Accelerometer SC7A20HTR (I2C)
// SDA: PC0, SCL: PC1, INT1: PD4, INT2: PB7

// Hall Sensor HAL2041S
// HALL: PD0

PORT_t *CPX_PORT[] = {&sfr_PORTA, &sfr_PORTA, &sfr_PORTB, &sfr_PORTA,
                      &sfr_PORTD, &sfr_PORTD, &sfr_PORTD, &sfr_PORTA,
                      &sfr_PORTB, &sfr_PORTB};
uint8_t CPX_PIN[] = {PIN2, PIN3, PIN3, PIN5, PIN3,
                     PIN2, PIN1, PIN4, PIN6, PIN5};

// Global loop rate-limiting tick
volatile uint8_t accel_tick = 0;

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

uint8_t pwmAllows(uint8_t led, uint8_t brightness) {
  if (brightness >= FULL_BRIGHTNESS) {
    return 1;
  }
  if (brightness == 0) {
    return 0;
  }

  // Per-LED phase offsets keep equally bright LEDs from blinking in lockstep.
  uint8_t phase = (pwm_phase + ((led * 13) & (BRIGHTNESS_STEPS - 1))) & (BRIGHTNESS_STEPS - 1);
  return phase < brightness;
}

ISR_HANDLER(TIM2_UPD_ISR, _TIM2_OVR_UIF_VECTOR_) {
  if (sleep) {
    sfr_TIM2.SR1.UIF = 0;  // clear timer 2 interrupt flag
    return;
  }

  time_ticks++;

  uint8_t led_to_light = nextPwmScanLed();
  uint8_t brightness = breathing_brightness;

  if (burst_life > 0) {
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

  uint8_t glow_width = 16 - ((uint16_t)energy * 9) / 255;
  uint8_t glow_dist = ringDistance(led_to_light, base_led);
  if (glow_dist <= glow_width) {
    uint8_t glow_peak = 18 + (energy >> 4);
    uint8_t glow = ((uint16_t)(glow_width - glow_dist) * glow_peak) / glow_width;
    brightness += glow;
    if (brightness > FULL_BRIGHTNESS) {
      brightness = FULL_BRIGHTNESS;
    }
  }

  if (energy > 12) {
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
  if (energy > 36 && brightness < FULL_BRIGHTNESS && glow_dist > 2) {
    uint8_t mask_pos = ((led_to_light * 5) + time_ticks + (energy >> 3)) & 0x1F;
    uint8_t threshold = 28 - ((uint16_t)(energy - 36) * 20) / 219;
    if (mask_pos >= threshold) {
      brightness = 0;
    }
  }

  if (pwmAllows(led_to_light, brightness)) {
    setLed(led_to_light);
  } else {
    ledLow(prevLed);
  }

  sfr_TIM2.SR1.UIF = 0;  // clear timer 2 interrupt flag
}

// Hall Sensor ISR
ISR_HANDLER(HALL_ISR, _EXTI0_VECTOR_) {
  if (!(sfr_PORTD.IDR.byte & PIN0)) {
    sleep = 1; // Trigger main loop to sequence a safe shutdown
  }
  sfr_ITC_EXTI.SR1.P0F = 1; // Clear interrupt flag
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

  initHall();
  initAccel();
  initUart();

  // Add a small delay for serial console attachment
  for(uint32_t d=0; d<300000; d++) NOP();

  printf("\r\n--- HALO-90 Booting ---\r\n");

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
  accel_write_reg(0x20, 0x47); 
  accel_write_reg(0x23, 0x80);
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
  if (!(sfr_PORTD.IDR.byte & PIN0)) {
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

      // Ensure EXTI flags are clear before entering halt loop
      sfr_ITC_EXTI.SR1.P0F = 1;

      // Enter ultra-low power HALT tightly while magnetic field is present (debounce-loop)
      while (!(sfr_PORTD.IDR.byte & PIN0)) {
        sfr_ITC_EXTI.SR1.P0F = 1; 
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
      accel_write_reg(0x20, 0x47);
      accel_write_reg(0x23, 0x80);
      printAccelConfig("wake");
      
      energy = 0;
      halo_angle_q4 = (int16_t)prevLed << 4;
      angular_velocity_q4 = 0;
      breathing_brightness = FULL_BRIGHTNESS;
      pwm_scan_led = 0;
      pwm_scan_bank = 0;
      pwm_phase = 0;
      burst_life = 0;

      enableTim2();
      setLed(prevLed);
      sleep = 0; 

    } else {
      WAIT_FOR_INTERRUPT();

      if (!sleep && (++accel_tick >= ACCEL_SAMPLE_TICKS)) {
        accel_tick = 0;

        int8_t rx = rx_prev;
        int8_t ry = ry_prev;
        int8_t rz = rz_prev;
        if (!readAccelRaw(&rx, &ry, &rz)) {
#if ACCEL_DIAGNOSTICS
          accel_fail_count++;
          accel_diag_tick++;
          if (accel_diag_tick >= ACCEL_DIAG_PRINT_SAMPLES) {
            accel_diag_tick = 0;
            printf("ACCEL read_fail ok=%u fail=%u addr8=0x%02X status=0x%02X\r\n",
                   accel_ok_count, accel_fail_count, sc7a20_addr, accel_read_reg(0x27));
          }
#endif
          angular_velocity_q4 = ((int16_t)angular_velocity_q4 * 220) >> 8;
          continue;
        }
#if ACCEL_DIAGNOSTICS
        accel_ok_count++;
#endif

        // 1. Compute Jolt Energy
        int16_t jolt = abs((int16_t)rx - rx_prev) + abs((int16_t)ry - ry_prev) + abs((int16_t)rz - rz_prev);
        rx_prev = rx;
        ry_prev = ry;
        rz_prev = rz;

        // Apply a small deadzone to filter sensor noise when still.
        int16_t active_jolt = 0;
        if (jolt > 2) {
          active_jolt = jolt - 2;
        }

        // 2. Update and Decay Energy (Q8)
        // Decays by ~0.9725 per frame (approx. (energy * 249) >> 8)
        uint16_t next_energy = ((uint16_t)energy * 249) >> 8;
        // Steps and jolts need to visibly disturb the ring.
        next_energy += (active_jolt * 12);
        if (next_energy > 255) next_energy = 255;
        energy = (uint8_t)next_energy;

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

        // 4. Update Burst and Orbiting Sparks Positions
        if (burst_life > 0) {
          burst_life--;
          burst_radius += 3;
          while (burst_radius >= LED_COUNT) {
            burst_radius -= LED_COUNT;
          }
        }

        if (active_jolt > 7) {
          uint8_t burst_duration = 8 + (active_jolt >> 1);
          if (burst_duration > 24) {
            burst_duration = 24;
          }

          burst_pos = base_led;
          burst_radius = 0;
          burst_life = burst_duration;

          for (uint8_t j = 0; j < SPARK_COUNT; j++) {
            spark_pos[j] = base_led;
            spark_vel[j] = (j & 1) ? 1 : -1;
            spark_life[j] = 10 + ((active_jolt + (j * 3)) & 0x0F);
          }
        }

        if (energy > 32) {
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
        }

        // 5. Update Breathing - cosine LUT sine wave, 5-second period.
        // The derivative is 0 at both extremes, so it lingers at dim and bright.
        static const uint8_t breath_lut[32] = {
          // (1-cos(2*pi*i/32))/2 * 16, i=0..31
           0,  0,  1,  1,  2,  4,  5,  6,
           8, 10, 11, 12, 14, 15, 15, 16,
          16, 16, 15, 15, 14, 12, 11, 10,
           8,  6,  5,  4,  2,  1,  1,  0
        };

        static uint16_t breath_tick = 0;
        breath_tick++;
        if (breath_tick >= 500) breath_tick = 0;

        // Map 0-499 to 0-31 LUT index
        uint8_t breath_idx = (uint8_t)((uint16_t)breath_tick * 32 / 500);
        uint8_t target_br = breath_lut[breath_idx];

        // Keep a low base so the bubble and motion chaos are obvious.
        target_br = 2 + (target_br >> 1);

        // Scale up toward full brightness (32/32) as energy rises.
        uint16_t current_br = target_br + (((uint16_t)energy * (32 - target_br)) / 255);
        breathing_brightness = (uint8_t)current_br;

#if ACCEL_DIAGNOSTICS
        accel_diag_tick++;
        if (accel_diag_tick >= ACCEL_DIAG_PRINT_SAMPLES) {
          accel_diag_tick = 0;
          printf("ACCEL ok=%u fail=%u raw=%d,%d,%d jolt=%d active=%d tilt=%d energy=%u base=%u vel=%d br=%u status=0x%02X\r\n",
                 accel_ok_count, accel_fail_count,
                 (int)rx, (int)ry, (int)rz,
                 (int)jolt, (int)active_jolt, (int)planar_tilt,
                 energy, base_led, (int)angular_velocity_q4,
                 breathing_brightness, accel_read_reg(0x27));
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
  // Hall sensor on PD0
  sfr_PORTD.DDR.byte &= ~PIN0; // Input
  sfr_PORTD.CR1.byte |= PIN0;  // Pull-up
  sfr_PORTD.CR2.byte |= PIN0;  // Interrupt enabled
  
  sfr_ITC_EXTI.CR1.P0IS = 3;   // Rising and falling edges
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
  uint8_t status = accel_read_reg(0x27);

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
