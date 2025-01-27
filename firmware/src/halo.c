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
uint16_t readMic();
void setLed(uint8_t led);
void ledHigh(uint8_t led);
void ledLow(uint8_t led);

void initADC();
void enableADC();
void disableADC();
void initButton();
void enableButton();
void disableButton();
void initTim2(uint16_t timeout);
void enableTim2();
void disableTim2();
void initTim4(uint8_t timeout);
void enableTim4();
void disableTim4();
void initAutoWakeup();
void enableAutoWakeup(uint16_t timeout);
void disableAutoWakeup();

// Global previousLed
volatile uint8_t prevLed = 0;
int8_t rotationCenter = 45;
int8_t rotDir = 1;
volatile uint8_t patternNum = 0;
volatile uint8_t sleep = 0;

// CPX-0, PA2
// CPX-1, PA4
// CPX-2, PB3
// CPX-3, PA5
// CPX-4, PD1
// CPX-5, PD2
// CPX-6, PD3
// CPX-7, PA3
// CPX-8, PB7
// CPX-9, PB5
PORT_t *CPX_PORT[] = {&sfr_PORTA, &sfr_PORTA, &sfr_PORTB, &sfr_PORTA,
                      &sfr_PORTD, &sfr_PORTD, &sfr_PORTD, &sfr_PORTA,
                      &sfr_PORTB, &sfr_PORTB};
uint8_t CPX_PIN[] = {PIN2, PIN4, PIN3, PIN5, PIN1,
                     PIN2, PIN3, PIN3, PIN7, PIN5};

PORT_t *SW_PORT[] = {&sfr_PORTB, &sfr_PORTB};
uint8_t SW_PIN[] = {PIN1, PIN6};


ISR_HANDLER(RTC_WAKEUP, _RTC_WAKEUP_VECTOR_) {
  switch(patternNum){
    case 1:
      // Halo
      setLed((prevLed + 13)%90);
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
  rotationCenter = (rotationCenter+rotDir)%90;
  sfr_TIM2.SR1.UIF = 0;  // clear timer 2 interrupt flag
  return;
}

// ADC end of conversion interupt
ISR_HANDLER(ADC1_EOC_ISR, _ADC1_EOC_VECTOR_){
  // Raw waveform
  sfr_ADC1.CR1.ADON = 0;        // Turn off ADC
  uint16_t adc = sfr_ADC1.DRL.byte | ((uint16_t)sfr_ADC1.DRH.byte)<<8;
  // Calculate the LED angle/4 0-89
  setLed((uint8_t)((4140 + rotationCenter + adc) % 90));
  sfr_ADC1.SR.EOC = 0 ;    // Clear EOC bit
  
  sfr_ADC1.CR1.ADON = 1;        // Enable ADC
  sfr_ADC1.SQR2.CHSEL_S22 = 1;  // ADC to D0, ADC1_IN22
  for(int i = 0; i < 48; i++){} // Delay t_wakeup, 3us 48 cycles
  sfr_ADC1.CR1.START = 1;       // Start Conversion
  return;
}

// Button ISR
ISR_HANDLER(BUTTON_ISR, _EXTI6_VECTOR_){
  // Button release debounce
  for(uint16_t i = 0 ; i < 5000; i++){NOP();}
  if(!(SW_PORT[1]->IDR.byte & SW_PIN[1])){
    if(!sleep){
      initTim4(244);
      enableTim4();                      // Start timer
      patternNum = (patternNum+1)%3;     //   Next pattern
      switch(patternNum){
        case 0:
          sfr_CPU.CFG_GCR.AL = 0; // Enable main WFI loop
          disableAutoWakeup();
          enableADC();
          enableTim2();
        break;
        case 1:
          disableTim2();         // Turn off timer
          disableADC();          // Turn off ADC
          sfr_CPU.CFG_GCR.AL = 1;// Interupt only based, IRET to HALT
          enableAutoWakeup(2);
        break;
        case 2:
          disableADC();          // Turn off ADC
          disableTim2();         // Turn off timer
          sfr_CPU.CFG_GCR.AL = 1;// Interupt only based, IRET to HALT
          enableAutoWakeup(100);
        break;
      }
    } else {
      // I hate this so much, it is so inefficent
      // Should be implemented as the same TIM4 timeout
      // At least it lets me debounce better
      // And a boot animation
      uint16_t debounce = 0; 
      for(uint16_t i = 0 ; i < 20000; i++){
        if(!(SW_PORT[1]->IDR.byte & SW_PIN[1])){
          debounce++;
          setLed(debounce/200); 
        }
        if(debounce > 18000){SW_RESET();}
      }
      ledLow(prevLed);
    }
  }
  sfr_ITC_EXTI.SR1.P6F = 1; // Clear button interupt flag
  return;
}

// Button has been held for timeout period
ISR_HANDLER(TIM4_UPD_ISR, _TIM4_UIF_VECTOR_) {
  ledLow(prevLed);
  // If button is still down
  if(!(SW_PORT[1]->IDR.byte & SW_PIN[1])){
    if(!sleep){ 
      sfr_CPU.CFG_GCR.AL = 1;// Interupt only based, IRET to HALT
      disableTim2();         // Turn off timer
      disableADC();          // Turn off ADC
      disableTim4();
      disableAutoWakeup();
      sfr_TIM4.SR1.UIF = 0;  // clear timer 4 interrupt flag
      sleep = 1;
      ledLow(prevLed);       // All LEDs off
      ENTER_HALT();          // Low power mode
      return;
    }
  }
  sfr_TIM4.SR1.UIF = 0;  // clear timer 4 interrupt flag
  return;
}

void main() {
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
  initAutoWakeup();

  disableAutoWakeup();
  enableADC();
  enableTim2();

  ENABLE_INTERRUPTS();

  while(1){
    WAIT_FOR_INTERRUPT();
  }
}

uint16_t readMic() {
  sfr_ADC1.CR1.ADON = 1;        // Enable ADC
  sfr_ADC1.SQR2.CHSEL_S22 = 1;  // ADC to D0, ADC1_IN22
  for(int i = 0; i < 12; i++){} // Delay t_wakeup, 3us 48 cycles
  sfr_ADC1.CR1.START = 1;       // Start Conversion
  while (!sfr_ADC1.SR.EOC);     // conversion ready
  sfr_ADC1.CR1.ADON = 0;        // Turn off ADC
  return sfr_ADC1.DRL.byte | ((uint16_t)sfr_ADC1.DRH.byte)<<8;
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

void enableTim2(){
  sfr_CLK.PCKENR1.PCKEN10 = 1;  // activate tim4 clock gate
  sfr_TIM2.CNTRH.byte = 0x00;   // MSB clear counter
  sfr_TIM2.CNTRL.byte = 0x00;   // LSB clear counter
  sfr_TIM2.IER.UIE = 1;         // enable timer 4 interrupt
  sfr_TIM2.CR1.CEN = 1;         // start the timer
}

void disableTim2(){
  sfr_TIM2.IER.UIE = 0;         // disable interrupt
  sfr_TIM2.CR1.CEN = 0;         // disable timer
  sfr_CLK.PCKENR1.PCKEN10 = 0;  // disable tim4 clock gate
}

void initAutoWakeup(){
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

void disableAutoWakeup(){
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
  sfr_TIM4.CR1.OPM = 1;              // Single pulse mode
  sfr_CLK.PCKENR1.PCKEN12 = 0;       // disable tim4 clock gate
}

void enableTim4(){
  sfr_CLK.PCKENR1.PCKEN12 = 1;  // activate tim4 clock gate
  sfr_TIM4.CNTR.byte = 0x00;    // clear counter
  sfr_TIM4.EGR.UG = 1;          // Generate Update
  sfr_TIM4.IER.UIE = 1;         // enable timer 4 interrupt
  sfr_TIM4.CR1.CEN = 1;         // start the timer
}

void disableTim4(){
  sfr_TIM4.IER.UIE = 0;         // disable interrupt
  sfr_TIM4.CR1.CEN = 0;         // disable timer
  sfr_CLK.PCKENR1.PCKEN12 = 0;  // disable tim4 clock gate
}

void initADC(){
  sfr_ITC_SPR.SPR5.VECT18SPR = 0b01; // ADC Priority to Level 1 (lower)
  sfr_CLK.PCKENR2.PCKEN20 = 1;       // Enable ADC Clock
  sfr_ADC1.CR1.RES = 0b00;           // Set ADC to 12b mode
  sfr_ADC1.CR2.SMTP1 = 0b111;        // Sample time 384
  sfr_ADC1.SQR1.DMAOFF = 1;          // DAMOFF for single ch
  sfr_ADC1.CR1.CONT = 0;             // Disable sampling mode
  // ADC DMA setup
}

void enableADC(){
  sfr_ADC1.CR1.EOCIE = 1;       // Enable EOC Interupt
  // First ADC Conversion
  sfr_ADC1.CR1.ADON = 1;        // Enable ADC
  sfr_ADC1.SQR2.CHSEL_S22 = 1;  // ADC to D0, ADC1_IN22
  for(int i = 0; i < 12; i++){} // Delay t_wakeup, 3us 48 cycles
  sfr_ADC1.CR1.START = 1;       // Start Conversion
}

void disableADC(){
  sfr_ADC1.CR1.EOCIE = 0;       // Disable EOC Interupt
  sfr_ADC1.CR1.ADON = 0;        // Disable ADC
  // sfr_CLK.PCKENR2.PCKEN20 = 0;  // Disable ADC Clock
}

void initButton(){
  // Set switch pins.
  SW_PORT[0]->DDR.byte |= SW_PIN[0];  // DDR = 1 Output
  SW_PORT[0]->CR1.byte |= SW_PIN[0];  // CR1 = 1 Push Pull
  SW_PORT[0]->ODR.byte &= ~SW_PIN[0]; // ODR = 0 Low

  // Pullup with interupt
  SW_PORT[1]->DDR.byte &= ~SW_PIN[1]; // DDR = 0 Input
  SW_PORT[1]->CR1.byte |= SW_PIN[1];  // CR1 = 1 Pullup
  SW_PORT[1]->CR2.byte |= SW_PIN[1];  // CR2 = 1 Interupt
  sfr_ITC_EXTI.CR2.P6IS = 2;          // Falling edge only
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
