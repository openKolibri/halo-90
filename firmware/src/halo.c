// halo.c halo control and procesing
// Copyright (C) 2021 Kolibri - Sawaiz Syed

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

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

// CPX-0, PA2
// CPX-1, PA3
// CPX-2, PB3
// CPX-3, PA5
// CPX-4, PD3
// CPX-5, PD2
// CPX-6, PD1
// CPX-7, PA4
// CPX-8, PB6
// CPX-9, PB5
PORT_t *CPX_PORT[] = {&sfr_PORTA, &sfr_PORTA, &sfr_PORTB, &sfr_PORTA,
                      &sfr_PORTD, &sfr_PORTD, &sfr_PORTD, &sfr_PORTA,
                      &sfr_PORTB, &sfr_PORTB};
uint8_t CPX_PIN[] = {PIN2, PIN3, PIN3, PIN5, PIN3,
                     PIN2, PIN1, PIN4, PIN6, PIN5};



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
    // Magnet near (LOW): go to sleep
    disableTim2();
    ledLow(prevLed);
    sleep = 1;
  } else {
    // Magnet away (HIGH): wake up
    if (sleep) {
      sleep = 0;
      enableTim2();
      setLed(prevLed);
    }
  }
  sfr_ITC_EXTI.SR1.P0F = 1; // Clear interrupt flag
  return;
}



void main(void) {
  DISABLE_INTERRUPTS();

  // Bump up clock to 16MHz
  sfr_CLK.CKDIVR.byte = 0x00;

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

  // Check initial state
  if (!(sfr_PORTD.IDR.byte & PIN0)) {
    sleep = 1;
  } else {
    // Set TIM2 to ultra-fast POV speed for smooth rendering (25 * 8us = ~200us per LED)
    initTim2(25);
    enableTim2();
    setLed(0);
  }

  ENABLE_INTERRUPTS();

  while(1){
    if (sleep) {
      ENTER_HALT();
    } else {
      WAIT_FOR_INTERRUPT();
      
      int8_t rx, ry, rz;
      readAccelRaw(&rx, &ry, &rz);
      
      uint16_t mag = abs(rx) + abs(ry) + abs(rz);
      
      // If X and Y are low, the earring is lying flat (gravity is mostly on Z)
      if (abs(rx) < 15 && abs(ry) < 15) {
          effect_mode = 1;
      } else {
          effect_mode = 0;
          
          // Calculate dynamic base angle visually mapping gravity pointing down physically
          // atan2f(y, x) returns radians in [-pi, pi]
          float angle = atan2f((float)ry, (float)rx);
          if (angle < 0) angle += 2.0f * 3.14159f;
          
          // Map 0 -> 2pi radially directly array mapped
          uint8_t new_base = (uint8_t)((angle / 6.28318f) * 90.0f) % 90;
          
          // 180 degree shift from previous -22 offset pointing TOP, now correctly +23 to point DOWN
          uint8_t rotated_base = (new_base + 23) % 90; 
          
          // Distribute visual dynamic width against violent shaking vectors
          int16_t shake = (int16_t)mag - 64; 
          if (shake < 0) shake = 0;
          uint8_t new_width = shake / 3;
          if (new_width > 20) new_width = 20; // Cap width constraints symmetrically 

          // Push asynchronous execution hooks
          base_led = rotated_base;
          led_width = new_width;
      }
      
      // Commented out printf to prevent UART from bottlenecking the 9600 baud polling loop
      // Printing ~50 chars takes ~45ms, causing strict latency jitter!
      // printf("X:%4d Y:%4d Z:%4d MAG:%4d WIDTH:%2d BASE:%2d\r\n", rx, ry, rz, mag, led_width, base_led);
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
  
  sfr_I2C1.CR2.STOP = 1;
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

void readAccelRaw(int8_t *x, int8_t *y, int8_t *z) {
  *x = accel_read_reg(0x29);
  *y = accel_read_reg(0x2B);
  *z = accel_read_reg(0x2D);
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
