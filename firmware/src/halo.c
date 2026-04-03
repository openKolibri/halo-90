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

// Prototypes
void setLed(uint8_t led);
void ledHigh(uint8_t led);
void ledLow(uint8_t led);

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

// Global previousLed and POV visualizer engine
volatile uint8_t prevLed = 0;
volatile uint8_t sleep = 0;
volatile int8_t led_offset = 0;
volatile int8_t led_dir = 1;
volatile uint8_t base_led = 0;
volatile uint8_t led_width = 0;
volatile uint8_t effect_mode = 0; // 0 = ARC, 1 = FLAT RING
volatile uint8_t flat_led_idx = 0;
volatile uint8_t sc7a20_addr = 0x30;

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
  // Persistence of Vision Sweep Engine
  if (effect_mode == 1) {
    setLed(flat_led_idx);
    flat_led_idx += 9;
    if (flat_led_idx >= 90) {
      flat_led_idx -= 89;
      if (flat_led_idx >= 9) flat_led_idx = 0;
    }
  } else if (led_width == 0) {
    setLed(base_led);
    led_offset = 0;
    led_dir = 1;
  } else {
    led_offset += led_dir;
    if (led_offset >= (int8_t)led_width) {
      led_offset = (int8_t)led_width;
      led_dir = -1;
    } else if (led_offset <= -(int8_t)led_width) {
      led_offset = -(int8_t)led_width;
      led_dir = 1;
    }
    int16_t target_led = (int16_t)base_led + led_offset;
    while (target_led < 0) target_led += 90;
    while (target_led >= 90) target_led -= 90;
    setLed((uint8_t)target_led);
  }

  sfr_TIM2.SR1.UIF = 0;  // clear timer 2 interrupt flag
  return;
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
    return (uint8_t)(a % 90);
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

  // LPF state variables
  static int16_t smooth_base_q4 = 0;
  static int16_t smooth_width_q4 = 0;

  // Boot timer logic explicitly to ensure consistent sequence
  initTim2(25);
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
      
      enableTim2();
      setLed(prevLed);
      sleep = 0; 

    } else {
      WAIT_FOR_INTERRUPT();
      
      if (!sleep && (++accel_tick >= 100)) {
        accel_tick = 0;
        
        int8_t rx, ry, rz;
        readAccelRaw(&rx, &ry, &rz);
        
        uint16_t mag = abs((int16_t)rx) + abs((int16_t)ry) + abs((int16_t)rz);
        
        // If X and Y are low, the earring is lying flat (gravity is mostly on Z)
        if (abs((int16_t)rx) < 15 && abs((int16_t)ry) < 15) {
            effect_mode = 1;
        } else {
            effect_mode = 0;
            
            // Map 0 -> 2pi radially directly array mapped
            uint8_t new_base = fast_atan2_to_led(ry, rx);
            
            // 180 degree shift from previous -22 offset pointing TOP, now correctly +23 to point DOWN
            uint8_t rotated_base = (new_base + 23) % 90; 
            
            // Distribute visual dynamic width against violent shaking vectors
            int16_t shake = (int16_t)mag - 64; 
            if (shake < 0) shake = 0;
            uint8_t new_width = shake / 3;
            if (new_width > 20) new_width = 20; // Cap width constraints symmetrically 

            // Organic Earring Smoothing Filter (Fixed Point Low-Pass LPF)
            int16_t target_q4 = (int16_t)rotated_base << 4;
            int16_t diff = target_q4 - smooth_base_q4;
            
            // Shortest path around 90-LED circle (90 * 16 = 1440)
            if (diff > 720) diff -= 1440;
            else if (diff < -720) diff += 1440;
            
            smooth_base_q4 += diff / 4; // alpha = 0.25 smooth glide
            
            if (smooth_base_q4 < 0) smooth_base_q4 += 1440;
            else if (smooth_base_q4 >= 1440) smooth_base_q4 -= 1440;
            
            int16_t width_target_q4 = (int16_t)new_width << 4;
            smooth_width_q4 += (width_target_q4 - smooth_width_q4) / 4;

            // Push asynchronous execution hooks
            base_led = (uint8_t)(smooth_base_q4 >> 4);
            led_width = (uint8_t)(smooth_width_q4 >> 4);
        }
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
  sfr_TIM2.ARRH.byte = (uint8_t)(timeout >> 8);  // set autoreload value for 50.176ms (=49*1.024ms)
  sfr_TIM2.ARRL.byte = (uint8_t)timeout;         // set autoreload value for 50.176ms (=49*1.024ms)
  sfr_TIM2.IER.UIE = 1;                          // enable timer 4 interrupt
  enableTim2();
}

void enableTim2(void){
  sfr_CLK.PCKENR1.PCKEN10 = 1;  // activate tim4 clock gate
  sfr_TIM2.CNTRH.byte = 0x00;   // MSB clear counter
  sfr_TIM2.CNTRL.byte = 0x00;   // LSB clear counter
  sfr_TIM2.IER.UIE = 1;         // enable timer 4 interrupt
  sfr_TIM2.CR1.CEN = 1;         // start the timer
}

void disableTim2(void){
  sfr_TIM2.IER.UIE = 0;         // disable interrupt
  sfr_TIM2.CR1.CEN = 0;         // disable timer
  sfr_CLK.PCKENR1.PCKEN10 = 0;  // disable tim4 clock gate
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
