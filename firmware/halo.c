#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// #define __SDCC
#include "STM8L151G6.h"


#define directWaveform 1
// #define framebuffer 1
// #define accel 1

// Prototypes

#ifdef directWaveform
uint16_t readMic();
void setLed(uint8_t led);
void ledHigh(uint8_t led);
void ledLow(uint8_t led);

void initADC();
void enableADC();
void disableADC();
void initButton();
void initTim2(uint16_t timeout);
void enableTim2();
void disableTim2();
void initTim4(uint16_t timeout);
void enableTim4();
void disableTim4();
void initAutoWakeup(uint16_t timeout);
void enableAutoWakeup();
void disableAutoWakeup();

// Global previousLed
volatile uint8_t prevLed = 0;

int8_t rotationCenter = 45;
int8_t rotDir = 1;
uint8_t patternNum = 0; 
uint8_t buttonState = 0;
uint8_t sleepState = 0;
#endif  // directWaveform

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


// Direct Waveform ISR
#ifdef directWaveform

ISR_HANDLER(RTC_WAKEUP, _RTC_WAKEUP_VECTOR_) {
  setLed((prevLed + 1)%90);
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
  switch(patternNum){
    case 0:
      // Raw waveform
      sfr_ADC1.CR1.ADON = 0;        // Turn off ADC
      uint16_t adc = sfr_ADC1.DRL.byte | ((uint16_t)sfr_ADC1.DRH.byte)<<8;
      // Calculate the LED angle/4 0-89
      setLed((uint8_t)((4140 + rotationCenter + adc) % 90));
      
      sfr_ADC1.CR1.ADON = 1;        // Enable ADC
      sfr_ADC1.SQR2.CHSEL_S22 = 1;  // ADC to D0, ADC1_IN22
      for(int i = 0; i < 48; i++){} // Delay t_wakeup, 3us 48 cycles
      sfr_ADC1.CR1.START = 1;       // Start Conversion
    break;

    case 1:
      // Sparkle
      setLed(rand() % 90);
      for(uint32_t i = 0 ; i < 10000; i++){}
    break;

    case 2:
      setLed((prevLed + 1)%90);
    break;

    default:
    break;
  }
  return;
}

#endif  // directWaveform

// Button ISR
ISR_HANDLER(PORTB_ISR, _EXTI6_VECTOR_){
  buttonState = !buttonState;        // Change button state to pressed
  if(buttonState){                   // Button is in down state
    initTim4(49);                    //   Reset and start timer 4 with a 0.5s timeout
    enableTim4();                    //   Start timer
    if(!sleepState){                 //   If it is not alsleep
      patternNum = (patternNum+1)%3; //     Next pattern
    }
  } else {                           // Button is in up state
    disableTim4();                   // Disable timer
  }
  sfr_ITC_EXTI.SR1.P6F = 1; // Clear button interupt flag
}

// Button has been held for timeout period
ISR_HANDLER(TIM4_UPD_ISR, _TIM4_UIF_VECTOR_) {
  sleepState = !sleepState;
  if(sleepState){
    disableTim2();   // Turn off timer
    disableADC();    // Turn off ADC
    ledLow(prevLed); // All LEDs off
    // Low power mode
    ENTER_HALT();
  } else {
    enableTim2();   // Turn on timer
    enableADC();    // Turn on ADC
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

  initButton();
  initTim2(5000);
  initTim4(49);
  enableTim4();
  initADC();
  // initAutoWakeup(56);
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

void initTim2(uint16_t timeout){
  sfr_CLK.PCKENR1.PCKEN10 = 1;                   // activate tim2 clock gate
  sfr_TIM2.CR1.CEN = 0;                          // disable timer
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

void initAutoWakeup(uint16_t timeout){
  sfr_CLK.CRTCR.RTCSEL1 = 1;        // Set RTC source to LSI
  while(sfr_CLK.CRTCR.RTCSWBSY){};  // Wait for clock change to finish
  sfr_CLK.PCKENR2.PCKEN22 = 1;      // Enable RTC Clock gating
  sfr_RTC.WPR.KEY = 0xCA;           // Disable RTC write protection
  sfr_RTC.WPR.KEY = 0x53;           // Disable RTC write protection
  sfr_RTC.CR2.WUTE = 0;             // Disable the wakeup timer
  sfr_RTC.ISR1.WUTWF = 1;           // Wakeup timer write enable
  while(!sfr_RTC.ISR1.WUTWF){}      // Poll unil bit is written
  sfr_RTC.WUTRH.byte = 0;           // MSB 16b auto countdown
  sfr_RTC.WUTRL.byte = 2;           // LSB 16b auto countdown
  sfr_RTC.CR1.WUCKSEL = 3;          // RTCCLK/2 as wakeup clock
  sfr_RTC.CR2.WUTIE = 1;            // Enable wakeup timer interupt
  sfr_RTC.CR2.WUTE = 1;             // Enable the wakeup timer

  sfr_CLK.ICKCR.SAHALT = 1;  // switch off main regulator during halt mode
  ENTER_HALT();              // enter HALT mode
}

// Initlize Timer 4, timeout in ms
void initTim4(uint16_t timeout){
  sfr_CLK.PCKENR1.PCKEN12 = 1;  // activate tim4 clock gate
  sfr_TIM4.CR1.CEN = 0;         // disable timer
  sfr_TIM4.CNTR.byte = 0x00;    // clear counter
  sfr_TIM4.CR1.ARPE = 1;        // auto-reload value buffered
  sfr_TIM4.EGR.byte = 0x00;     // clear pending events
  sfr_TIM4.PSCR.PSC = 15;       // set clock to 16Mhz/2^15 = 488.3Hz -> 2.048ms period
  sfr_TIM4.CR1.OPM = 1;         // Single pulse mode
  sfr_TIM4.ARR.byte = 244;      // set autoreload value for 499.7ms (244*2.048ms)
  enableTim4();
}

void enableTim4(){
  sfr_CLK.PCKENR1.PCKEN12 = 1;  // activate tim4 clock gate
  sfr_TIM4.CNTR.byte = 0x00;    // clear counter
  sfr_TIM4.IER.UIE = 1;         // enable timer 4 interrupt
  sfr_TIM4.CR1.CEN = 1;         // start the timer
}

void disableTim4(){
  sfr_TIM4.IER.UIE = 0;         // disable interrupt
  sfr_TIM4.CR1.CEN = 0;         // disable timer
  sfr_CLK.PCKENR1.PCKEN12 = 0;  // disable tim4 clock gate
}

void initADC(){
  enableADC();
  // First ADC Conversion
  sfr_ADC1.CR1.ADON = 1;        // Enable ADC
  sfr_ADC1.SQR2.CHSEL_S22 = 1;  // ADC to D0, ADC1_IN22
  for(int i = 0; i < 12; i++){} // Delay t_wakeup, 3us 48 cycles
  sfr_ADC1.CR1.START = 1;       // Start Conversion
}

void enableADC(){
  // ADC init
  sfr_ADC1.CR1.RES = 0b00;      // Set ADC to 12b mode
  sfr_ADC1.TRIGR2.byte = 0xFF;  // disable Schmitt trigger
  sfr_CLK.PCKENR2.PCKEN20 = 1;  // Enable ADC Clock
  sfr_ADC1.CR2.SMTP1 = 0b111;   // Sample time 384
  sfr_ADC1.SQR1.DMAOFF = 1;     // DAMOFF for single ch
  sfr_ADC1.CR1.EOCIE = 1;       // Enable EOC Interupt
  // sfr_ADC1.CR1.CONT = 1;        // Continous sampling mode
  // ADC DMA setup
}

void disableADC(){
  sfr_ADC1.CR1.EOCIE = 0;       // Disable EOC Interupt
  sfr_CLK.PCKENR2.PCKEN20 = 0;  // Disable ADC Clock
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
  sfr_ITC_EXTI.CR2.P6IS = 3;          // Falling & Rising edge
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