#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "STM8L151G6.h"

#define directWaveform 1
// #define framebuffer 1
// #define accel 1

// Prototypes

#ifdef directWaveform
uint16_t readMic();
void setLed(uint8_t led);
void unsetLed(uint8_t led);

uint8_t lastLed = 0;
uint8_t currLed = 0;
uint8_t rotationCenter = 45;
#endif  // directWaveform

// V3
// CPX-0, PA2
// CPX-1, PA4
// CPX-2, PB0
// CPX-3, PA5
// CPX-4, PD1
// CPX-5, PD2
// CPX-6, PD3
// CPX-7, PA3
// CPX-8, PB2
// CPX-9, PB1
// PORT_t *CPX_PORT[] = {&sfr_PORTA, &sfr_PORTA, &sfr_PORTB, &sfr_PORTA,
//                       &sfr_PORTD, &sfr_PORTD, &sfr_PORTD, &sfr_PORTA,
//                       &sfr_PORTB, &sfr_PORTB};
// uint8_t CPX_PIN[] = {PIN2, PIN4, PIN0, PIN5, PIN1,
//                      PIN2, PIN3, PIN3, PIN2, PIN1};

// V4
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
#define SW_A PB1
#define SW_B PB6


// Framebuffer ISR
#ifdef framebuffer
// Gamma corrected from 8b space to 4b space
uint8_t brightnesses[5] = {0x00, 0x10, 0x24, 0xAA, 0xFF};

uint8_t frameBuffer[90] = {0};
uint8_t brightnessPass = 0;

// Draw frame buffer on TIM4 interupt
ISR_HANDLER(TIM4_UPD_ISR, _TIM4_UIF_VECTOR_) {
  uint8_t ledSelect = 0;
  for (uint8_t i = sizeof(CPX_PIN) / sizeof(CPX_PIN[0]); i > 0; i--) {
    // Set (i-1) HIGH
    CPX_PORT[i - 1]->DDR.byte |= CPX_PIN[i - 1];
    CPX_PORT[i - 1]->CR1.byte |= CPX_PIN[i - 1];
    CPX_PORT[i - 1]->ODR.byte |= CPX_PIN[i - 1];
    for (uint8_t j = 0; j < sizeof(CPX_PIN) / sizeof(CPX_PIN[0]); j++) {
      if (j != i - 1) {
        if (((frameBuffer[ledSelect] >> brightnessPass) & 0x01)) {
          // Set (j) to LOW
          CPX_PORT[j]->DDR.byte |= CPX_PIN[j];
          CPX_PORT[j]->CR1.byte |= CPX_PIN[j];
          CPX_PORT[j]->ODR.byte &= ~CPX_PIN[j];
          for (uint16_t d = 0; d < 10; d++) {
            NOP();
          }
          // Set (j) to HI-Z
          CPX_PORT[j]->ODR.byte |= CPX_PIN[j];
          CPX_PORT[j]->DDR.byte &= ~CPX_PIN[j];
          CPX_PORT[j]->CR1.byte &= ~CPX_PIN[j];
        }
        ledSelect++;
      }
    }
    // Set (i-1) to HI-Z
    CPX_PORT[i - 1]->ODR.byte &= ~CPX_PIN[i - 1];
    CPX_PORT[i - 1]->DDR.byte &= ~CPX_PIN[i - 1];
    CPX_PORT[i - 1]->CR1.byte &= ~CPX_PIN[i - 1];
  }

  // Increment for next brightnessPass
  if (brightnessPass < 8) {
    brightnessPass++;
  } else {
    brightnessPass = 0;
  }
  sfr_TIM4.SR1.UIF = 0;  // clear timer 4 interrupt flag
  return;

}  // TLI_ISR

#endif  // framebuffer

// Direct Waveform ISR
#ifdef directWaveform
ISR_HANDLER(TIM4_UPD_ISR, _TIM4_UIF_VECTOR_) {
  rotationCenter = (rotationCenter+1)%90;
  sfr_TIM4.SR1.UIF = 0;  // clear timer 4 interrupt flag
  return;
}

// ADC end of conversion interupt
ISR_HANDLER(ADC1_EOC_ISR, _ADC1_EOC_VECTOR_){

  sfr_ADC1.CR1.ADON = 0;        // Turn off ADC
  uint16_t adc = sfr_ADC1.DRL.byte | ((uint16_t)sfr_ADC1.DRH.byte)<<8;
  // Calculate the LED angle/4 0-89
  currLed = (uint8_t)((4140 + rotationCenter + adc) % 90);
  
  unsetLed(lastLed);
  setLed(currLed);
  lastLed = currLed;

  sfr_ADC1.CR1.ADON = 1;        // Enable ADC
  sfr_ADC1.SQR2.CHSEL_S22 = 1;  // ADC to D0, ADC1_IN22
  for(int i = 0; i < 48; i++){} // Delay t_wakeup, 3us 48 cycles
  sfr_ADC1.CR1.START = 1;       // Start Conversion

  return;
}

#endif  // directWaveform

void main() {
  DISABLE_INTERRUPTS();

  // Bump up clock to 16MHz
  sfr_CLK.CKDIVR.byte = 0x00;

  // Init and Set all CPX pins to hi-z
  for (int k = 0; k < sizeof(CPX_PIN) / sizeof(CPX_PIN[0]); k++) {
    CPX_PORT[k]->DDR.byte &= ~CPX_PIN[k];
    CPX_PORT[k]->CR1.byte &= ~CPX_PIN[k];
  }

  // Timer 4 init
  sfr_CLK.PCKENR1.PCKEN12 = 1;  // activate tim4
  sfr_TIM4.CR1.CEN = 0;         // disable timer
  sfr_TIM4.CNTR.byte = 0x00;    // clear counter
  sfr_TIM4.CR1.ARPE = 1;        // auto-reload value buffered
  sfr_TIM4.EGR.byte = 0x00;     // clear pending events
  sfr_TIM4.PSCR.PSC = 14;       // set clock to 16Mhz/2^14 = 976.6Hz -> 1.024ms period
  sfr_TIM4.ARR.byte = 49;       // set autoreload value for 50.176ms (=49*1.024ms)
  sfr_TIM4.IER.UIE = 1;         // enable timer 4 interrupt
  sfr_TIM4.CR1.CEN = 1;         // start the timer

  // ADC init
  sfr_ADC1.CR1.RES = 0b00;      // Set ADC to 12b mode
  sfr_ADC1.TRIGR2.byte = 0xFF;  // disable Schmitt trigger
  sfr_CLK.PCKENR2.PCKEN20 = 1;  // Enable ADC Clock
  sfr_ADC1.CR2.SMTP1 = 0b111;   // Sample time 384
  sfr_ADC1.SQR1.DMAOFF = 1;     // DAMOFF for single ch
  sfr_ADC1.CR1.EOCIE = 1;       // Enable EOC Interupt
  // sfr_ADC1.CR1.CONT = 1;        // Continous sampling mode

  // First ADC Conversion
  sfr_ADC1.CR1.ADON = 1;        // Enable ADC
  sfr_ADC1.SQR2.CHSEL_S22 = 1;  // ADC to D0, ADC1_IN22
  for(int i = 0; i < 12; i++){} // Delay t_wakeup, 3us 48 cycles
  sfr_ADC1.CR1.START = 1;       // Start Conversion

  // ADC DMA setup


  ENABLE_INTERRUPTS();

  while (1) {
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

void setLed(uint8_t led) {
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

void unsetLed(uint8_t led) {
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