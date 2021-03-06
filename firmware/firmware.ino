/*
The MIT License (MIT)

Copyright (c) 2013 mirage335

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

/*
http://mirage335-site.dyndns.org
*/

/*
Credits:
Original example, "Interface between Arduino DM board and Linear Tech LTC2440 24-bit ADC Nov. 12 2010 John Beale" by John Beale, found at http://dangerousprototypes.com/forum/viewtopic.php?f=2&t=4247 .
Interrupt code based on example at http://www.engblaze.com/microcontroller-tutorial-avr-and-arduino-timer-interrupts/ .
Binary union based on example at http://forum.arduino.cc/index.php?topic=112597.0 .
No licenses were found with these examples. Public domain or fair use assumed.
*/

/*
Arduino DAQ Firmware
Emits samples in S32_LE format over USB Serial (ie. /dev/tty?) at the rate specificied by the sampleRate variable. Up to 3000 samples/sec has been successfully tested.

Default 150Hz sample rate, low pass filtered down to 45Hz, suitable for EEG pickup.

Unix machines can process this data with the following commands, among others:
stty -F /dev/ttyACM0 raw ; echo -n "" > /dev/ttyACM0 ; timeout 10 cat /dev/ttyACM0 > test ; wc -c test
stty -F /dev/ttyACM0 raw ; echo -n "" > /dev/ttyACM0 ; cat /dev/ttyACM0 | aplay -f S32_LE
stty -F /dev/ttyACM0 raw ; echo -n "" > /dev/ttyACM0 ; hexdump /dev/ttyACM0
*/

//SPI and interrupt libraries.
#include <SPI.h>
#include <avr/io.h>
#include <avr/interrupt.h>

//#define debug

//Macro function for low pass filter.
//Overuse can cause data and precision loss, especially with non-float variables.
#define lowPass(newValue, filteredValue, inertiaFloat)                            \
  filteredValue = filteredValue + (inertiaFloat * (newValue - filteredValue));

//IIR Biquad Filter.
//Parameters b0, b1, b2, a1, a2 are filter coefficients. See http://gnuradio.4.n7.nabble.com/IIR-filter-td40994.html and http://www.earlevel.com/main/2013/10/13/biquad-calculator-v2/ .
//Data is returned in the float named [filteredValue] . This variable must be externally declared.
//State variables unique_d1_name and unique_d2_name should be statically declared floats.
#define IIRbiquad(newValue, filteredValue, unique_d1_name, unique_d2_name, b0, b1, b2, a1, a2)			\
														\
	filteredValue = b0 * newValue + unique_d1_name;								\
	unique_d1_name = (float)b1 * (float)newValue + (float)a1 * filteredValue + unique_d2_name; 	\
	unique_d2_name = (float)b2 * (float)newValue + (float)a2 * filteredValue;

//High Order IIR Biquad Filter.
//Parameters b0, b1, b2, a1, a2 are filter coefficients. See http://gnuradio.4.n7.nabble.com/IIR-filter-td40994.html and http://www.earlevel.com/main/2013/10/13/biquad-calculator-v2/ .
//Data is returned in the float named [filteredValue] .
#define highOrderIIRbiquad(newValue, filteredValue, stateOneArrayName, stateTwoArrayName, b0, b1, b2, a1, a2, filterOrder)	\
	static float stateOneArrayName[(filterOrder+1)];									\
	static float stateTwoArrayName[(filterOrder+1)];									\
	lowerOrderFilteredValue = newValue;											\
																\
	for (filterLoop=0; filterLoop < filterOrder; filterLoop++) {								\
		IIRbiquad(lowerOrderFilteredValue, filteredValue, stateOneArrayName[filterLoop], stateTwoArrayName[filterLoop], b0, b1, b2, a1, a2) \
		lowerOrderFilteredValue = filteredValue;									\
	}

typedef union {
  long longValue;
  byte binary[4];
} binaryLong;

typedef union {
  float floatValue;
  byte binary[4];
} binaryFloat;

//*****CONFIG*****

//Maximum sample rate is less than the LTC2440 conversion rate, defined by the oversampling ratio, set by the speedBits variable. Faster speedBits settings lead to increased noise and loss of built-in low-pass filter (anti-aliasing) characteristics. See Table 3 in the LTC2440 datasheet.
const int sampleRate = 150;                           //Desired sample rate in Hz. Test with stty -F /dev/ttyACM0 raw ; echo -n "" > /dev/ttyACM0 ; timeout 10 cat /dev/ttyACM0 > test ; wc -c test .
const int speedBits=0b0101;                           //Oversample ratio, determines LTC2440 bandwidth and maximum sampling rate. See datasheet Tabel 3. {0b0001 = 3.52kHz, 0b0010=1.76kHz, 0b0101=220Hz, 0b0110=110Hz, 0b1111=6.875Hz}

//Maximizes IIR low-pass anti-alias filtering.
//WARNING: Extremely high filter orders (>300) will consume too much RAM, and may break bootloaders.
//WARNING: Old code, for reference only.
//const int filterOrder = (2750/sampleRate*5)%300;    //Autocalculates IIR filter maximum order up to 300.
//const int filterOrder = 300;                      //Only use for sampling rates < 50Hz.

const byte slaveSelectPin = 10;  // digital pin 10 for /CS
const int led = A4;

//*****CONFIG*****

const int convertSpeed = 0b10000000 | speedBits<<3;

//Global vairables allow interrupt preemption.
float volts = 0;
#ifdef debug
int millisOffset;
unsigned long sampleCount = 0;
#endif

void setup() {
    Serial.begin(115200);  // Maybe a lot faster over native USB.
    
    //Initiates first conversion cycle.
    pinMode (slaveSelectPin, OUTPUT);
    digitalWrite(slaveSelectPin,LOW);   // chip select is active low
    digitalWrite(slaveSelectPin,HIGH);
    
    //Configure SPI.
    SPI.begin();                          // initialize SPI, covering MOSI,MISO,SCK signals
    SPI.setBitOrder(MSBFIRST);            // data is clocked in MSB first
    SPI.setDataMode(SPI_MODE0);           // SCLK idle low (CPOL=0), MOSI read on rising edge (CPHI=0)
    SPI.setClockDivider(SPI_CLOCK_DIV2);  //8MHz clock. 250000 32bit samples/sec may be read at this rate.
    
    //Startup LED indicator.
    pinMode (led, OUTPUT);
    
    for (int i=0; i<=10; i++) {
      delay(100);
      digitalWrite(led, HIGH);
      delay(100);
      digitalWrite(led, LOW);
    }
    
    //Configure interrupt handler. This provides preemption and an accurate sample rate.
    // initialize Timer1
    cli();          // disable global interrupts
    TCCR1A = 0;     // set entire TCCR1A register to 0
    TCCR1B = 0;     // same for TCCR1B
    // set compare match register to desired timer count:
    OCR1A = (int)(16000000/8/sampleRate); //Counter setup.
    TCCR1B |= (1 << WGM12);               // turn on CTC mode
    TCCR1B |= (1 << CS11);
    TIMSK1 |= (1 << OCIE1A);              // enable timer compare interrupt:
    sei();                                // enable global interrupts:
    
    #ifdef debug
    millisOffset = millis();
    #endif
}

//Non critical tasks. Preempted by interrrupt handler.
void loop()
{
  #ifdef debug
  //Catches some underruns. Disable in production, to prevent false-positive due to eventual overflow.
  if (sampleCount <  sampleRate * 0.99 * (millis() - millisOffset) / 1000) {
    digitalWrite(led, HIGH);
  }
  #endif
  
  //Clipping diagnostic LED.
  if (abs(volts) > 2.45)
    digitalWrite(led, HIGH);
  else
    digitalWrite(led, LOW);
  
  delay(150);
}

//Critical path. Sample, process, and transmit data.
ISR(TIMER1_COMPA_vect)
{
  //LTC2440 SPI data acqusition cycle.
  static long b3;
  static long b2;
  static long b1;
  static long b0;
  digitalWrite(slaveSelectPin,LOW);   // take the SS pin low to select the chip
  delayMicroseconds(1);              // probably not needed, only need 25 nsec delay
  b3 = SPI.transfer(convertSpeed);   // B3
  b2 = SPI.transfer(0x00);           // B2
  b1 = SPI.transfer(0x00);           // B1
  b0 = SPI.transfer(0x00);           // B0
  // take the SS pin high to de-select the chip:
  digitalWrite(slaveSelectPin,HIGH);  //Next conversion cycle begins here.
  static long in = 0;         // incoming serial 32-bit word
  in = b3<<8;
  in |= b2;
  in = in<<8;
  in |= b1;
  in = in<<8;
  in |= b0;
  in &= 0x1FFFFFFF; // force high three bits to zero
  in = in>>5;   // truncate lowest 5 bits
  
  //Conversion to accurate voltage.
  volts = (in * (0.2980232594) * 0.000001);
  if (volts > (2.5))
    volts=((5)-volts)*(-1);  //Polarity correction.
  
  //IIR filters augment LTC2440 built-in anti-alias filter and 60Hz rejection filter, particularly at lower oversample rates (higher conversion rates).
  static float filteredVolts=0;
  
  static int filterLoop;
  static float lowerOrderFilteredValue;
  
  //Low pass filter. 45Hz corner frequency at 150Hz sample rate.
  highOrderIIRbiquad(volts, filteredVolts, lowPassStateOne, lowPassStateTwo, 0.39133426347022965, 0.7826685269404593, 0.39133426347022965, -0.3695259524151477, -0.19581110146577096, 8);
  
  //High pass (AC coupling) filter. 0.35Hz corner frequency at 150Hz sample rate. Mostly useful for removing amplifier flicker noise.
  highOrderIIRbiquad(filteredVolts, filteredVolts, highPassStateOne, highPassStateTwo, 0.9896867236566386, -1.9793734473132771, 0.9896867236566386, 1.9792670828350616, -0.9794798117914928, 2);
  
  //60Hz notch filter at 150Hz sample rate.
  highOrderIIRbiquad(filteredVolts, filteredVolts, sixtyNotchStateOne, sixtyNotchStateTwo, 0.9883808858331896, 1.5992338671088302, 0.9883808858331896, -1.5992338671088302, -0.9767617716663792, 1);
  
  //30Hz notch filter at 150H sample rate.
  highOrderIIRbiquad(filteredVolts, filteredVolts, thirtyNotchStateOne, thirtyNotchStateTwo, 0.9813339196216473, -0.6064977166393354, 0.9813339196216473, 0.6064977166393354, -0.9626678392432946, 1);
  
  //Human readable format.
  //Serial.println("");
  //Serial.println((float)volts,6);             //Raw.
  //Serial.println((float)(filteredVolts),6);   //Filtered.
  
  //Prepare value for binary readout.
  static binaryLong convertableMicroVolts;
  convertableMicroVolts.longValue = filteredVolts * 1000000;
  
  //Optional gain. Drops dynamic range..
  //convertableMicroVolts.longValue *= (long)1000000;
  
  //convertableMicroVolts.longValue *= (long)10000;
  
  //Compatible with aplay -f S32_LE .
  Serial.write(convertableMicroVolts.binary,4);
  
  #ifdef debug
  sampleCount++;
  #endif
}

