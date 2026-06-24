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
#define SWEEP_STEP 13
#define BRIGHTNESS_STEPS 32
#define FULL_BRIGHTNESS BRIGHTNESS_STEPS
#define SPARK_COUNT 4

// Prototypes
void setLed(uint8_t led);
void ledHigh(uint8_t led);
void ledLow(uint8_t led);
uint16_t fast_rand(void);
uint16_t safe_rand(void);

void initHall(void);
void initTim2(uint16_t timeout);
void enableTim2(void);
void disableTim2(void);

// Accelerometer & I2C Prototypes
void initAccel(void);
void readAccelRaw(int8_t *x, int8_t *y, int8_t *z);
uint8_t accel_read_reg(uint8_t reg);
void accel_write_reg(uint8_t reg, uint8_t val);

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
volatile uint8_t effect_mode = 0; // 0 = ARC, 1 = FLAT RING
volatile uint8_t sc7a20_addr = 0x30;

// Balance Halo visual state variables
volatile uint8_t energy = 0;             // Q8 energy (0 to 255)
volatile int16_t halo_angle_q4 = 0;       // Q4 angular position (0 to 1439)
volatile int16_t angular_velocity_q4 = 0; // Q4 angular velocity
volatile uint8_t time_ticks = 0;          // Dynamic mask reference counter
volatile uint8_t sweep_led = 0;           // Independent sweep LED for solid ring
volatile uint8_t breathing_brightness = 32; // PWM duty cycle (0 to 32)
volatile uint8_t brightness_phase = 0;
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

ISR_HANDLER(TIM2_UPD_ISR, _TIM2_OVR_UIF_VECTOR_) {
  if (sleep) {
    sfr_TIM2.SR1.UIF = 0;  // clear timer 2 interrupt flag
    return;
  }

  time_ticks++;

  brightness_phase++;
  if (brightness_phase >= BRIGHTNESS_STEPS) {
    brightness_phase = 0;
  }

  if (effect_mode == 1) {
    sweep_led += SWEEP_STEP;
    if (sweep_led >= LED_COUNT) {
      sweep_led -= LED_COUNT;
    }

    if (brightness_phase < breathing_brightness) {
      setLed(sweep_led);
    } else {
      ledLow(prevLed);
    }

    sfr_TIM2.SR1.UIF = 0;
    return;
  }

  // Get a pseudo-random number
  uint16_t r = fast_rand();
  uint8_t r_prob = r & 0xFF;

  if (burst_life > 0) {
    uint8_t burst_led = burst_pos;
    if (r & 1) {
      burst_led += burst_radius;
      while (burst_led >= LED_COUNT) {
        burst_led -= LED_COUNT;
      }
    } else if (burst_radius > burst_led) {
      burst_led = (burst_led + LED_COUNT) - burst_radius;
    } else {
      burst_led -= burst_radius;
    }

    setLed(burst_led);
    sfr_TIM2.SR1.UIF = 0;
    return;
  }

  // Orbiting Sparks logic
  if (energy > 50 && r_prob > 240) {
    uint8_t chosen_spark = (r >> 8) & (SPARK_COUNT - 1);
    if (spark_life[chosen_spark] > 0) {
      if (brightness_phase < breathing_brightness) {
        setLed(spark_pos[chosen_spark]);
      } else {
        ledLow(prevLed);
      }

      sfr_TIM2.SR1.UIF = 0;
      return;
    }
  }

  // Determine LED selection
  uint8_t led_to_light = 0;
  uint8_t select_gravity = 0;

  // The threshold for gravity selection vs interlaced sweep
  // At energy = 0: 70% interlaced, 30% gravity
  // At energy = 255: 50% gravity selection for more fragments and sparkles.
  uint8_t gravity_threshold = 76 + ((uint16_t)energy * 52) / 255;

  if (r_prob < gravity_threshold) {
    select_gravity = 1;
  }

  if (select_gravity) {
    // Gravity Glow: light up near base_led
    // The spread narrows with energy, producing a brighter highlight.
    uint8_t range = 9 - ((uint16_t)energy * 6) / 255;

    int8_t offset = (int8_t)(((uint16_t)(r >> 8) * range) >> 8) - (int8_t)(range >> 1);
    int16_t target = (int16_t)base_led + offset;
    if (target < 0) {
      target += LED_COUNT;
    } else if (target >= LED_COUNT) {
      target -= LED_COUNT;
    }
    led_to_light = (uint8_t)target;
  } else {
    // Coprime sweep for solid background ring
    sweep_led += SWEEP_STEP;
    if (sweep_led >= LED_COUNT) {
      sweep_led -= LED_COUNT;
    }
    led_to_light = sweep_led;
  }

  // Apply Shimmer Sparkles:
  // Chance of a random sparkle increases with energy
  // At energy=255, we have up to 25% chance of a random sparkle
  // At energy=0, 0%
  uint8_t sparkle_threshold = energy >> 2; // energy / 4
  if (r_prob < sparkle_threshold) {
    led_to_light = (uint8_t)(((uint16_t)(r >> 8) * LED_COUNT) >> 8);
  }

  // Apply Fragmentation Mask:
  // If energy is high, we dynamically mask out parts of the ring to make it look fragmented
  uint8_t show_led = 1;
  if (energy > 80) {
    uint8_t mask_pos = (led_to_light + time_ticks) & 0x1F;
    // Threshold ranges from 30 (at energy=80) to 12 (at energy=255).
    uint8_t threshold = 30 - ((uint16_t)(energy - 80) * 18) / 175;
    if (mask_pos >= threshold) {
      show_led = 0;
    }
  }

  if (show_led && brightness_phase < breathing_brightness) {
    setLed(led_to_light);
  } else {
    // Turn off last LED to maintain dark space in fragmented arcs
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
  }

  // Reload the initialization since it might have failed previously with the old address
  accel_write_reg(0x20, 0x47); 
  accel_write_reg(0x23, 0x80);

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
      
      energy = 0;
      halo_angle_q4 = (int16_t)prevLed << 4;
      angular_velocity_q4 = 0;
      sweep_led = 0;
      breathing_brightness = FULL_BRIGHTNESS;
      brightness_phase = 0;
      burst_life = 0;

      enableTim2();
      setLed(prevLed);
      sleep = 0; 

    } else {
      WAIT_FOR_INTERRUPT();

      if (!sleep && (++accel_tick >= 156)) {
        accel_tick = 0;

        int8_t rx, ry, rz;
        readAccelRaw(&rx, &ry, &rz);

        // 1. Compute Jolt Energy
        int16_t jolt = abs((int16_t)rx - rx_prev) + abs((int16_t)ry - ry_prev) + abs((int16_t)rz - rz_prev);
        rx_prev = rx;
        ry_prev = ry;
        rz_prev = rz;

        // Apply deadzone to filter out sensor noise when still (threshold of 3 counts)
        int16_t active_jolt = 0;
        if (jolt > 3) {
          active_jolt = jolt - 3;
        }

        // 2. Update and Decay Energy (Q8)
        // Decays by ~0.9725 per frame (approx. (energy * 249) >> 8)
        uint16_t next_energy = ((uint16_t)energy * 249) >> 8;
        // Jolt adds to energy with a responsive gain of 8
        next_energy += (active_jolt * 8);
        if (next_energy > 255) next_energy = 255;
        energy = (uint8_t)next_energy;

        // 3. Physical Inertia System (First-Order LPF Gravity Tracker)
        // If X and Y are low, the earring is lying flat (effect_mode = 1)
        if (abs((int16_t)rx) < 15 && abs((int16_t)ry) < 15) {
            effect_mode = 1;
        } else {
            effect_mode = 0;

            // Target angle (gravity down direction)
            uint8_t new_base = fast_atan2_to_led(ry, rx);
            uint8_t rotated_base = new_base + 23;
            if (rotated_base >= LED_COUNT) {
              rotated_base -= LED_COUNT;
            }

            int16_t target_q4 = (int16_t)rotated_base << 4;
            int16_t error = target_q4 - halo_angle_q4;
            if (error > 720) error -= 1440;
            else if (error < -720) error += 1440;

            // Spring inertia: calm motion is heavy; charged motion is tighter.
            uint8_t spring_shift = 5 - ((uint16_t)energy * 2) / 255;
            int16_t spring_impulse = error >> spring_shift;
            if (spring_impulse == 0 && error != 0) {
              spring_impulse = (error > 0) ? 1 : -1;
            }

            angular_velocity_q4 += spring_impulse;
            if (angular_velocity_q4 > 120) angular_velocity_q4 = 120;
            else if (angular_velocity_q4 < -120) angular_velocity_q4 = -120;

            // Damping moves from about 0.92 at rest to about 0.82 when charged.
            uint8_t damping = 235 - ((uint16_t)energy * 25) / 255;
            angular_velocity_q4 = ((int16_t)angular_velocity_q4 * (int16_t)damping) >> 8;

            halo_angle_q4 += angular_velocity_q4;
            while (halo_angle_q4 < 0) halo_angle_q4 += 1440;
            while (halo_angle_q4 >= 1440) halo_angle_q4 -= 1440;

            base_led = (uint8_t)(halo_angle_q4 >> 4);
        }

        // 4. Update Burst and Orbiting Sparks Positions
        if (burst_life > 0) {
          burst_life--;
          burst_radius += 3;
          while (burst_radius >= LED_COUNT) {
            burst_radius -= LED_COUNT;
          }
        }

        if (active_jolt > 10) {
          uint8_t burst_duration = 6 + (active_jolt >> 2);
          if (burst_duration > 18) {
            burst_duration = 18;
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

        if (energy > 50) {
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
            } else if (energy > 128) {
              uint16_t sr = safe_rand();
              if ((sr & 0x1F) == 0) {
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

        // Scale up toward full brightness (32/32) as energy rises
        uint16_t current_br = target_br + (((uint16_t)energy * (32 - target_br)) / 255);
        breathing_brightness = (uint8_t)current_br;
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

void readAccelRaw(int8_t *x, int8_t *y, int8_t *z) {
  uint16_t t;
  
  sfr_I2C1.CR2.START = 1;
  t = 10000; while (!sfr_I2C1.SR1.SB && --t); if (!t) return;
  
  sfr_I2C1.DR.byte = sc7a20_addr;
  t = 10000; while (!sfr_I2C1.SR1.ADDR && !(sfr_I2C1.SR2.byte & 0x04) && --t); 
  if (!sfr_I2C1.SR1.ADDR) { sfr_I2C1.SR2.byte &= ~0x04; sfr_I2C1.CR2.STOP = 1; return; }
  (void)sfr_I2C1.SR1.byte;
  (void)sfr_I2C1.SR3.byte;
  
  // 0x28 is OUT_X_L. MSB set (0x80) -> 0xA8 for auto-increment read in LIS2DH12/SC7A20
  sfr_I2C1.DR.byte = 0xA8; 
  t = 10000; while (!sfr_I2C1.SR1.TXE && !(sfr_I2C1.SR2.byte & 0x04) && --t); 
  if (!sfr_I2C1.SR1.TXE) { sfr_I2C1.SR2.byte &= ~0x04; sfr_I2C1.CR2.STOP = 1; return; }
  
  sfr_I2C1.CR2.START = 1;
  t = 10000; while (!sfr_I2C1.SR1.SB && --t); if (!t) { sfr_I2C1.SR2.byte &= ~0x04; sfr_I2C1.CR2.STOP = 1; return; }
  
  sfr_I2C1.DR.byte = sc7a20_addr | 0x01;
  t = 10000; while (!sfr_I2C1.SR1.ADDR && !(sfr_I2C1.SR2.byte & 0x04) && --t); 
  if (!sfr_I2C1.SR1.ADDR) { sfr_I2C1.SR2.byte &= ~0x04; sfr_I2C1.CR2.STOP = 1; return; }
  
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
    else buf[i] = 0;
  }
  
  // Data in SC7A20 Normal mode (10-bit) is left-justified.
  // OUT_X_H = buf[1] (upper 8 bits)
  *x = (int8_t)buf[1];
  *y = (int8_t)buf[3];
  *z = (int8_t)buf[5];
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
