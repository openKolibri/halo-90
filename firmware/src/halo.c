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
uint16_t readAccelMag(void);
uint8_t accel_read_reg(uint8_t reg);
void accel_write_reg(uint8_t reg, uint8_t val);


// Global previousLed
volatile uint8_t prevLed = 0;
volatile uint8_t sleep = 0;

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
  // Advance the LED clockwise around the ring
  setLed((prevLed + 1) % 90);
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

  // Check initial state
  if (!(sfr_PORTD.IDR.byte & PIN0)) {
    sleep = 1;
  } else {
    // Set TIM2 to have exactly an interrupt every 10ms (1250 * 8us = 10ms)
    initTim2(1250);
    enableTim2();
    setLed(0);
  }

  ENABLE_INTERRUPTS();

  while(1){
    if (sleep) {
      ENTER_HALT();
    } else {
      WAIT_FOR_INTERRUPT();
      
      uint16_t mag = readAccelMag();
      uint16_t new_arr = 1250;
      
      // Typical 1G resting magnitude is ~64.
      if (mag > 64) {
        // Cap magnitude to prevent negative or overly fast periods
        if (mag > 314) mag = 314;
        new_arr = 1250 - ((mag - 64) * 4); // Min ARR ~ 250 (fastest speed)
      }
      
      sfr_TIM2.ARRH.byte = (uint8_t)(new_arr >> 8);
      sfr_TIM2.ARRL.byte = (uint8_t)new_arr;
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
  t = 10000; while (!sfr_I2C1.SR1.SB && --t); if (!t) return 0;
  
  sfr_I2C1.DR.byte = 0x30;
  t = 10000; while (!sfr_I2C1.SR1.ADDR && --t); if (!t) return 0;
  (void)sfr_I2C1.SR1.byte;
  (void)sfr_I2C1.SR3.byte;
  
  sfr_I2C1.DR.byte = reg;
  t = 10000; while (!sfr_I2C1.SR1.TXE && --t); if (!t) return 0;
  
  sfr_I2C1.CR2.START = 1;
  t = 10000; while (!sfr_I2C1.SR1.SB && --t); if (!t) return 0;
  
  sfr_I2C1.DR.byte = 0x31;
  t = 10000; while (!sfr_I2C1.SR1.ADDR && --t); if (!t) return 0;
  
  sfr_I2C1.CR2.ACK = 0;
  (void)sfr_I2C1.SR1.byte;
  (void)sfr_I2C1.SR3.byte;
  
  sfr_I2C1.CR2.STOP = 1;
  
  t = 10000; while (!sfr_I2C1.SR1.RXNE && --t); 
  if (t) val = sfr_I2C1.DR.byte;
  
  return val;
}

void accel_write_reg(uint8_t reg, uint8_t val) {
  uint16_t t;
  
  sfr_I2C1.CR2.START = 1;
  t = 10000; while (!sfr_I2C1.SR1.SB && --t); if (!t) return;
  
  sfr_I2C1.DR.byte = 0x30;
  t = 10000; while (!sfr_I2C1.SR1.ADDR && --t); if (!t) return;
  (void)sfr_I2C1.SR1.byte;
  (void)sfr_I2C1.SR3.byte;
  
  sfr_I2C1.DR.byte = reg;
  t = 10000; while (!sfr_I2C1.SR1.TXE && --t); if (!t) return;
  
  sfr_I2C1.DR.byte = val;
  t = 10000; while (!sfr_I2C1.SR1.TXE && --t); if (!t) return;
  
  sfr_I2C1.CR2.STOP = 1;
}

void initAccel(void) {
  sfr_CLK.PCKENR1.PCKEN13 = 1; 

  sfr_PORTC.DDR.byte &= ~((1<<0) | (1<<1));
  sfr_PORTC.CR1.byte &= ~((1<<0) | (1<<1));
  
  sfr_I2C1.CR1.PE = 0;
  sfr_I2C1.FREQR.FREQ = 16;
  sfr_I2C1.CCRH.byte = 0;
  sfr_I2C1.CCRL.byte = 0x50;
  sfr_I2C1.TRISER.TRISE = 17;
  sfr_I2C1.CR1.PE = 1;
  
  for(uint16_t i=0; i<10000; i++) NOP();
  
  accel_write_reg(0x20, 0x47); 
  accel_write_reg(0x23, 0x80);
}

uint16_t readAccelMag(void) {
  int8_t x = accel_read_reg(0x29);
  int8_t y = accel_read_reg(0x2B);
  int8_t z = accel_read_reg(0x2D);
  return abs(x) + abs(y) + abs(z);
}
