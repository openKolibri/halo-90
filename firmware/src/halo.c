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
uint16_t readMic(void);
void setLed(uint8_t led);
void ledHigh(uint8_t led);
void ledLow(uint8_t led);

void initADC(void);
void enableADC(void);
void disableADC(void);
void initButton(void);
void initHall(void);
void initMicPwr(void);
void enableButton(void);
void disableButton(void);
void initTim2(uint16_t timeout);
void enableTim2(void);
void disableTim2(void);
void initTim4(uint8_t timeout);
void enableTim4(void);
void disableTim4(void);
void initAutoWakeup(void);
void enableAutoWakeup(uint16_t timeout);
void disableAutoWakeup(void);



// The audio center is completely auto-calibrated dynamically (EMA filter)
// Visualizer is strictly calibrated: 120 dB SPL maps to 45 LEDs (Max Length)

// Button State Machine
volatile uint8_t button_timer = 0;

// LFSR PRNG State
volatile uint16_t rand_state = 0xACE1;

uint16_t fast_rand(void){
    uint16_t lsb = rand_state & 1;
    rand_state >>= 1;
    if (lsb) {
        rand_state ^= 0xB400u;
    }
    return rand_state;
}

// Precomputed Row and Col mapping for 90 LEDs
const uint8_t LED_COL[90] = {
  0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 
  1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 
  3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 4, 
  5, 5, 5, 5, 5, 5, 5, 5, 5, 6, 6, 6, 6, 6, 6, 
  6, 6, 6, 7, 7, 7, 7, 7, 7, 7, 7, 7, 8, 8, 8, 
  8, 8, 8, 8, 8, 8, 9, 9, 9, 9, 9, 9, 9, 9, 9, 
};

const uint8_t LED_ROW[90] = {
  9, 8, 7, 6, 5, 4, 3, 2, 1, 9, 8, 7, 6, 5, 4, 
  3, 2, 0, 9, 8, 7, 6, 5, 4, 3, 1, 0, 9, 8, 7, 
  6, 5, 4, 2, 1, 0, 9, 8, 7, 6, 5, 3, 2, 1, 0, 
  9, 8, 7, 6, 4, 3, 2, 1, 0, 9, 8, 7, 5, 4, 3, 
  2, 1, 0, 9, 8, 6, 5, 4, 3, 2, 1, 0, 9, 7, 6, 
  5, 4, 3, 2, 1, 0, 8, 7, 6, 5, 4, 3, 2, 1, 0, 
};

// Global variables
volatile uint8_t prevLed = 0;
int8_t rotationCenter = 45;
int8_t rotDir = 1;
volatile uint8_t patternNum = 1; // Default to Audio Mode
volatile uint8_t sleep = 0;

// CPX Mapping
PORT_t *CPX_PORT[10] = {&sfr_PORTC, &sfr_PORTC, &sfr_PORTD, &sfr_PORTB, &sfr_PORTB,
                        &sfr_PORTB, &sfr_PORTB, &sfr_PORTB, &sfr_PORTB, &sfr_PORTB};
uint8_t CPX_PIN[10] = {PIN1, PIN0, PIN4, PIN7, PIN6,
                       PIN3, PIN5, PIN4, PIN2, PIN1};

PORT_t *SW_PORT = &sfr_PORTA;
uint8_t SW_PIN = PIN5;

PORT_t *HALL_PORT = &sfr_PORTA;
uint8_t HALL_PIN = PIN2;

PORT_t *MIC_PWR_PORT = &sfr_PORTA;
uint8_t MIC_PWR_PIN = PIN4;


ISR_HANDLER(RTC_WAKEUP, _RTC_WAKEUP_VECTOR_) {
  if (sleep) {
      sfr_RTC.ISR2.WUTF = 0;
      return;
  }
  switch(patternNum){
    case 1:
      // Pattern 1 is purely Audio Mode (driven by ADC ISR). 
      // Do not run any LED overwrites from RTC Wakeup here.
    break;

    case 2:
      // Sparkle
      rand()%15 ? ledLow(prevLed) : setLed(rand() % 90);
    break;

    default:
    break;
  }
  sfr_RTC.ISR2.WUTF = 0;     // Reset wakeup flag
  return;
}

ISR_HANDLER(TIM2_UPD_ISR, _TIM2_OVR_UIF_VECTOR_) {
  if (sleep) {
      sfr_TIM2.SR1.UIF = 0;
      return;
  }
  rotDir = (HALL_PORT->IDR.byte & HALL_PIN) ? 1 : -1;
  rotationCenter = (rotationCenter+rotDir+90)%90;
  sfr_TIM2.SR1.UIF = 0;  // clear timer 2 interrupt flag
  return;
}

// ADC end of conversion interupt
ISR_HANDLER(ADC1_EOC_ISR, _ADC1_EOC_VECTOR_){
  // IMPORTANT: STM8 Hardware requires DRL to be read explicitly BEFORE DRH to unlock the shadow register!
  // A single-line C OR operator `|` does not guarantee execution order, which causes corrupt massive jumps!
  uint8_t low = sfr_ADC1.DRL.byte;
  uint8_t high = sfr_ADC1.DRH.byte;
  uint16_t adc = ((uint16_t)high << 8) | low;

  if (sleep) {
      sfr_ADC1.SR.EOC = 0;
      sfr_ADC1.CR1.ADON = 0; // Only turn off ADC natively when moving to SLEEP mode to prevent waking current
      return;
  }
  
  if (patternNum == 0) {
      // Test Mode (Raw ADC mapping to exactly 90 LEDs)
      setLed((uint8_t)(adc >> 5) % 90);
  }
  else if (patternNum == 1) {
      // Audio Visualizer
      // Self-Tuning DC offset (EMA Filter using 10-bit fractional precision)
      static uint32_t dc_center = 0; 

      // First-run initialization: snap instantly to the real idle microphone voltage
      if (dc_center == 0) {
          dc_center = (uint32_t)adc << 10;
      }
      
      // Real-time exponential moving average
      dc_center = dc_center - (dc_center >> 10) + adc; 

      // Extract the filtered DC baseline by shifting back down
      int16_t current_dc = (int16_t)(dc_center >> 10);
      int16_t amplitude = (int16_t)adc - current_dc;
      if (amplitude < 0) amplitude = -amplitude; 
      
      // Calculate instantaneous amplitude width
      // Physical PicoScope analysis: Max Audio output = ~130mV Peak-To-Peak (65mV Amp)
      // 65mV / 0.8mV ADC step = ~81 integer amplitude units.
      // 81 / 2 = 40 LEDs for maximum explosion!
      uint8_t target_width = (uint8_t)(amplitude / 2); 
      if (target_width > 44) target_width = 44; 
      
      // Smooth the width visually so it doesn't flicker too aggressively (software Low-Pass)
      static uint8_t smooth_width = 0;
      static uint8_t hold_counter = 0;
      
      // Noise floor gate
      if (target_width < 2) target_width = 0;

      if (target_width > smooth_width) {
          smooth_width = target_width; // Instant attack on loud sounds
          hold_counter = 100;          // Hold peak briefly
      } else {
          if (hold_counter > 0) {
              hold_counter--;
          } else if (smooth_width > 0) { 
              static uint8_t decay = 0;
              if (decay++ % 4 == 0) smooth_width--; // Smooth visual decay
          }
      }

      // Instead of sweeping 90 LEDs, just light up the *tips* of the audio envelope to form expanding dots,
      // or sweep just the inner 5-10 LEDs for a solid core to maintain maximum brightness!
      static uint8_t scan_idx = 0;
      static uint8_t scan_dir = 0;
      
      if (smooth_width == 0) {
          scan_idx = 0;
          setLed(rotationCenter);
      } else {
          // Rapidly sweep from center up to the smooth width
          if (scan_dir == 0) {
              setLed((rotationCenter + scan_idx + 90) % 90);
              scan_dir = 1;
          } else {
              setLed((rotationCenter - scan_idx + 90) % 90);
              scan_dir = 0;
              scan_idx++;
              if (scan_idx > smooth_width) {
                  scan_idx = 0;
              }
          }
      }
  }
  
  sfr_ADC1.SR.EOC = 0 ;    // Clear EOC bit
  sfr_ADC1.CR1.START = 1;  // Instantly Trigger Next Conversion cleanly
  return;
}

// Button ISR

// Hall Sensor ISR for Deep Sleep
ISR_HANDLER(HALL_ISR, _EXTI2_VECTOR_){
  // Debounce
  for(uint16_t i = 0 ; i < 5000; i++){NOP();}
  
  if(!(HALL_PORT->IDR.byte & HALL_PIN)){
      // Magnet detected - Go to Deep Sleep
      if(!sleep){
          sfr_CPU.CFG_GCR.AL = 1; // Interrupt only based, IRET to HALT
          disableTim2();          // Turn off timer
          disableADC();           // Turn off ADC
          disableTim4();
          disableAutoWakeup();
          sleep = 1;
          ledLow(prevLed);        // All LEDs off
          MIC_PWR_PORT->ODR.byte &= ~MIC_PWR_PIN; // Turn off MIC
      }
  } else {
      // Magnet removed - Wake up
      if(sleep){
          sleep = 0;
          sfr_CPU.CFG_GCR.AL = 0; // Enable main loop
          // Restore previous state based on patternNum
          if (patternNum == 0) {
              enableADC();
              enableTim2();
              MIC_PWR_PORT->ODR.byte |= MIC_PWR_PIN;
          } else if (patternNum == 1) {
              enableADC();
              enableTim2();
              MIC_PWR_PORT->ODR.byte |= MIC_PWR_PIN;
          } else if (patternNum == 2) {
              sfr_CPU.CFG_GCR.AL = 1; // Return to HALT after ISR
              enableAutoWakeup(2);
          } else if (patternNum == 3) {
              sfr_CPU.CFG_GCR.AL = 1; // Return to HALT after ISR
              enableAutoWakeup(100);
          }
      }
  }
  sfr_ITC_EXTI.SR1.P2F = 1; // Clear Hall interrupt flag
  return;
}


ISR_HANDLER(BUTTON_ISR, _EXTI5_VECTOR_){
  sfr_ITC_EXTI.SR1.P5F = 1; // Clear button interrupt flag early

  // Simple and highly robust blocking debounce loop (~50ms)
  for(uint32_t i = 0; i < 40000; i++) { NOP(); }

  if (!(SW_PORT->IDR.byte & SW_PIN)) {
      if (!sleep) {
          // If awake, cycle pattern
          patternNum = (patternNum+1)%4;
          switch(patternNum){
            case 0:
              sfr_CPU.CFG_GCR.AL = 0; 
              disableAutoWakeup();
              enableADC();
              enableTim2();
              MIC_PWR_PORT->ODR.byte |= MIC_PWR_PIN; // Turn ON MIC
            break;
            case 1:
              sfr_CPU.CFG_GCR.AL = 0; 
              disableAutoWakeup();
              enableADC();
              enableTim2();
              MIC_PWR_PORT->ODR.byte |= MIC_PWR_PIN; // Turn ON MIC
            break;
            case 2:
              MIC_PWR_PORT->ODR.byte &= ~MIC_PWR_PIN; // Turn OFF MIC
              disableTim2();         
              disableADC();          
              sfr_CPU.CFG_GCR.AL = 1;
              enableAutoWakeup(2);
            break;
            case 3:
              MIC_PWR_PORT->ODR.byte &= ~MIC_PWR_PIN; // Turn OFF MIC
              disableADC();          
              disableTim2();         
              sfr_CPU.CFG_GCR.AL = 1;
              enableAutoWakeup(100);
            break;
          }
      } else {
          // Wake up sequence timeout started (using TIM4)
          button_timer = 0;
          sfr_TIM4.CR1.OPM = 0; // Continuous mode for wake-up checker
          initTim4(50); // Fast 100ms ticks for boot animation
          enableTim4();
      }
  }
  
  // Clear again in case of bounces during the blocking delay
  sfr_ITC_EXTI.SR1.P5F = 1; 
  return;
}

ISR_HANDLER(TIM4_UPD_ISR, _TIM4_UIF_VECTOR_) {
  sfr_TIM4.SR1.UIF = 0;

  if (sleep) {
      if (!(SW_PORT->IDR.byte & SW_PIN)) {
          // Button is held while asleep
          button_timer++;
          setLed(button_timer % 90);
          if (button_timer > 10) { 
              // Held for a full second while asleep -> Reboot
              SW_RESET();
          }
      } else {
          // Button released early, cancel wake
          disableTim4();
          ledLow(prevLed);
      }
  }
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

  initADC();
  initTim2(5000);
  initButton();
  initHall();
  initMicPwr();
  initAutoWakeup();

  disableAutoWakeup();
  enableADC();
  enableTim2();

  ENABLE_INTERRUPTS();

  while(1){
    WAIT_FOR_INTERRUPT();
  }
}

uint16_t readMic(void){
  sfr_ADC1.CR1.ADON = 1;        // Enable ADC
  sfr_ADC1.SQR4.CHSEL_S3 = 1;   // ADC to PA3, ADC1_IN3
  for(int i = 0; i < 48; i++){ NOP(); } // Delay t_wakeup, 3us 48 cycles
  sfr_ADC1.CR1.START = 1;       // Start Conversion
  while (!sfr_ADC1.SR.EOC);     // conversion ready
  
  uint8_t low = sfr_ADC1.DRL.byte;
  uint8_t high = sfr_ADC1.DRH.byte;
  sfr_ADC1.CR1.ADON = 0;        // Turn off ADC AFTER reading shadow logic!
  
  return ((uint16_t)high << 8) | low;
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

void initAutoWakeup(void){
  sfr_CLK.CRTCR.RTCSEL1 = 1;                      // Set RTC source to LSI
  while(sfr_CLK.CRTCR.RTCSWBSY){};                // Wait for clock change to finish
  sfr_CLK.PCKENR2.PCKEN22 = 1;                    // Enable RTC Clock gating
  sfr_RTC.WPR.KEY = 0xCA;                         // Disable RTC write protection
  sfr_RTC.WPR.KEY = 0x53;                         // Disable RTC write protection
  sfr_RTC.CR2.WUTE = 0;                           // Disable the wakeup timer
  while(!sfr_RTC.ISR1.WUTWF){}                    // Poll unil bit is written
  sfr_RTC.WUTRH.byte = 0;                         // MSB 16b auto countdown
  sfr_RTC.WUTRL.byte = 1;                         // LSB 16b auto countdown
  sfr_RTC.CR1.WUCKSEL = 3;                        // RTCCLK/2 as wakeup clock
  sfr_CLK.ICKCR.SAHALT = 1;                       // switch off main regulator during halt mode
  sfr_CLK.PCKENR2.PCKEN22 = 0;                    // Disable RTC Clock gating
}

void enableAutoWakeup(uint16_t timeout){
  sfr_CLK.PCKENR2.PCKEN22 = 1;                    // Enable RTC Clock gating
  sfr_ITC_SPR.SPR2.VECT4SPR = 0b00;               // Interupt Priority to Level 2 (mid)
  sfr_RTC.WPR.KEY = 0xCA;                         // Disable RTC write protection
  sfr_RTC.WPR.KEY = 0x53;                         // Disable RTC write protection
  sfr_RTC.CR2.WUTE = 0;                           // Disable the wakeup timer
  while(!sfr_RTC.ISR1.WUTWF){}                    // Poll unil bit is written
  sfr_RTC.WUTRH.byte = (uint8_t)(timeout >> 8);   // MSB 16b auto countdown
  sfr_RTC.WUTRL.byte = (uint8_t)(timeout);        // LSB 16b auto countdown
  sfr_RTC.CR2.WUTIE = 1;                          // Enable wakeup timer interupt
  sfr_RTC.CR2.WUTE = 1;                           // Enable the wakeup timer
}

void disableAutoWakeup(void){
  sfr_RTC.WPR.KEY = 0xCA;           // Disable RTC write protection
  sfr_RTC.WPR.KEY = 0x53;           // Disable RTC write protection
  sfr_RTC.CR2.WUTE = 0;             // Disable the wakeup timer
  while(!sfr_RTC.ISR1.WUTWF){}      // Poll unil bit is written
  sfr_RTC.CR2.WUTIE = 0;            // Enable wakeup timer interupt
  sfr_CLK.PCKENR2.PCKEN22 = 0;      // Disable RTC Clock gating
}

void initTim4(uint8_t timeout){
  sfr_CLK.PCKENR1.PCKEN12 = 1;       // activate tim4 clock gate
  sfr_ITC_SPR.SPR7.VECT25SPR = 0b11; // Interupt Priority to Level 3 (max)
  sfr_TIM4.CR1.CEN = 0;              // disable timer
  sfr_TIM4.CNTR.byte = 0x00;         // clear counter
  sfr_TIM4.CR1.ARPE = 1;             // auto-reload value buffered
  sfr_TIM4.CR1.URS = 1;              // Overflow only interupt 
  sfr_TIM4.EGR.byte = 0x00;          // clear pending events
  sfr_TIM4.PSCR.PSC = 15;            // set clock to 16Mhz/2^15 = 488.3Hz -> 2.048ms period
  sfr_TIM4.ARR.byte = timeout;       // set autoreload value for 499.7ms (244*2.048ms)
  sfr_TIM4.CR1.OPM = 1;              // Single pulse mode originally
  sfr_CLK.PCKENR1.PCKEN12 = 0;       // disable tim4 clock gate
}

void enableTim4(void){
  sfr_CLK.PCKENR1.PCKEN12 = 1;  // activate tim4 clock gate
  sfr_TIM4.CNTR.byte = 0x00;    // clear counter
  sfr_TIM4.EGR.UG = 1;          // Generate Update
  sfr_TIM4.IER.UIE = 1;         // enable timer 4 interrupt
  sfr_TIM4.CR1.CEN = 1;         // start the timer
}

void disableTim4(void){
  sfr_TIM4.IER.UIE = 0;         // disable interrupt
  sfr_TIM4.CR1.CEN = 0;         // disable timer
  sfr_CLK.PCKENR1.PCKEN12 = 0;  // disable tim4 clock gate
}

void initADC(void){
  // Ensure PA3 (ADC1_IN3) is configured as floating input for the microphone
  sfr_PORTA.DDR.byte &= ~PIN3; // Input
  sfr_PORTA.CR1.byte &= ~PIN3; // Floating (no pull-up)
  sfr_PORTA.CR2.byte &= ~PIN3; // No interrupt

  sfr_ITC_SPR.SPR5.VECT18SPR = 0b01; // ADC Priority to Level 1 (lower)
  sfr_CLK.PCKENR2.PCKEN20 = 1;       // Enable ADC Clock
  sfr_ADC1.CR1.RES = 0b00;           // Set ADC to 12b mode
  sfr_ADC1.CR2.SMTP1 = 0b111;        // Sample time 384
  sfr_ADC1.SQR1.DMAOFF = 1;          // DAMOFF for single ch
  sfr_ADC1.CR1.CONT = 0;             // Disable sampling mode
  sfr_ADC1.TRIGR4.TRIG3 = 1;         // Disable Schmitt trigger for ADC1_IN3 (PA3)
  // ADC DMA setup
}

void enableADC(void){
  sfr_ADC1.CR1.EOCIE = 1;       // Enable EOC Interupt
  // First ADC Conversion
  sfr_ADC1.CR1.ADON = 1;        // Enable ADC
  sfr_ADC1.SQR4.CHSEL_S3 = 1;   // ADC to PA3, ADC1_IN3
  for(int i = 0; i < 48; i++){ NOP(); } // Delay t_wakeup, 3us 48 cycles
  sfr_ADC1.CR1.START = 1;       // Start Conversion
}

void disableADC(void){
  sfr_ADC1.CR1.EOCIE = 0;       // Disable EOC Interupt
  sfr_ADC1.CR1.ADON = 0;        // Disable ADC
  // sfr_CLK.PCKENR2.PCKEN20 = 0;  // Disable ADC Clock
}

void initButton(void){
  // Pullup with interupt
  SW_PORT->DDR.byte &= ~SW_PIN; // DDR = 0 Input
  SW_PORT->CR1.byte |= SW_PIN;  // CR1 = 1 Pullup
  SW_PORT->CR2.byte |= SW_PIN;  // CR2 = 1 Interupt
  sfr_ITC_EXTI.CR2.P5IS = 2;          // Falling edge only
}

void initHall(void){
  HALL_PORT->DDR.byte &= ~HALL_PIN; // DDR = 0 Input
  HALL_PORT->CR1.byte |= HALL_PIN;  // CR1 = 1 Pullup
  HALL_PORT->CR2.byte |= HALL_PIN;  // CR2 = 1 Interrupt
  sfr_ITC_EXTI.CR1.P2IS = 0b11;     // Rising and falling edge
}

void initMicPwr(void){
  MIC_PWR_PORT->DDR.byte |= MIC_PWR_PIN; // DDR = 1 Output
  MIC_PWR_PORT->CR1.byte |= MIC_PWR_PIN; // CR1 = 1 Push-Pull
  MIC_PWR_PORT->ODR.byte |= MIC_PWR_PIN; // ODR = 1 High
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
  uint8_t col = LED_COL[led];
  uint8_t row = LED_ROW[led];

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
  uint8_t col = LED_COL[led];
  uint8_t row = LED_ROW[led];

  // Set row to HI-Z
  CPX_PORT[row]->ODR.byte |= CPX_PIN[row];
  CPX_PORT[row]->DDR.byte &= ~CPX_PIN[row];
  CPX_PORT[row]->CR1.byte &= ~CPX_PIN[row];
  // Set column to HI-Z
  CPX_PORT[col]->ODR.byte &= ~CPX_PIN[col];
  CPX_PORT[col]->DDR.byte &= ~CPX_PIN[col];
  CPX_PORT[col]->CR1.byte &= ~CPX_PIN[col];
}
