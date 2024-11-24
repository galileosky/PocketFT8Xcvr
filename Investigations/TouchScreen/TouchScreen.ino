/*
NAME
  TouchScreen Explorer

DESCRIPTION
  Investigates properties/behaviors of the Adafruit 320x480 resistive touchscreen,
  especially those helpful to understanding erratic (bouncing, dropout, noise...)
  coordinate readings, and whether these arise from circuit board crosstalk or
  from inherent characteristics of the touchscreen.

USAGE
  The sketch accepts one-letter commands entered on the USB Serial port:
    (R)esistance:  Calculates the X-Plate resistance from X- to X+
    (N)oise:       Measures noise on the floating Y-Plate
    (T)ouch Event: Samples the Y-Plate voltage before/during/after touch event
    (S)ave         Saves the buffered samples to an SD file

EXERCISED
  + X-plate resistance
  + Idle (no touch) noise on Y-plate
  + Touch event noise (dropouts, erratic coordinates, etc)
  + Duration of a touch event

NOTE
  + From the Pocket FT8 schematic, we know the Teensy 4.1 YP and XM pins
  connect to the touchpad Y+ and X- pins through 510 ohm resistors.  This
  hardware dependency is used in some calculations below.
  + Pocket FT8 operates the MCP342x ADC with 12 bits at 240 samples/second.  The 
  range of possible readings from the ADC should be 0..2047.
  + We consider a touch event to begin when the ADC returns a value greater
  than or equal to the so-called MINPRESSURE.
  + We note when a touch event begins, and record samples into a FIFO
  ring buffer for 1.9 seconds

REFERENCES

ATTRIBUTION
  KQ7B, MIT License

*/



#include <Wire.h>
#include <SD.h>
#include "HX8357_t3n.h"
#include "MCP342x.h"

// These are the four touchscreen pins used by Pocket FT8 hardware
#define YP 38  // must be an analog pin, use "An" notation!
#define XM 37  // must be an analog pin, use "An" notation!
#define YM 36  // can be a digital pin
#define XP 39  // can be a digital pin

// Define the value of the resistors connecting the Teensy 4.1 YP and XM pins
// to the touchpad's Y+ and X- pins.
#define RYP 510  //Ohms
#define RXM 510  //Ohms

//Define Vcc
#define VCC 3.6  //Volts

// // Touchpad calibrarion as investigated on V1.01 hardware
// #define TS_MINX 123
// #define TS_MINY 104
// #define TS_MAXX 1715
// #define TS_MAXY 1130

//Other touchpad constants replicated from button.cpp in PocketFT8XcvrFW
#define MINPRESSURE 120
#define PENRADIUS 3

//Number of samples taken during one phase of the investigation
#define NSAMPLES 240  //About one seconds worth of samples

//Define the maximum possible ADC sampled voltage
#define maxADCreading 2.047  //"She can't do no more, Jim"

//Build the display object using pin numbers from Charlie's Pocket FT8 code
HX8357_t3n tft = HX8357_t3n(10, 9, 8, 11, 13, 12);  //Teensy 4.1 moved SCK to dig pin 13

//Build an instance of the MCP342x A/D Converter
uint8_t address = 0x69;  //Original I2C address was 0x68 but MCP342X V1.01 chip reports 0x68
MCP342x adc = MCP342x(address);

//Build an array for storing ADC samples
float samples[NSAMPLES];  //About one seconds

//State variable tracks when we've detected the beginning of a touch event
bool touchEventInProgress = false;



class RingBfrState {
public:
  int nextIn;   //Index of where next added value will be placed
  int nextOut;  //Index of where next removed value will be found
  int count;    //Number of values in the buffer
};

/*
** RingBfr -- A FIFO ring buffer of long integers
**
** Someday:  rewrite this class as a template for arbitrary types and buffer sizes
*/
class RingBfr {

public:

  /**
 * RingBfr constructor
**/
  RingBfr() {
    nextIn = nextOut = count = 0;
  }  //RingBfr()


  /**
 * @brief Reset a ring buffer to its initial conditions
 *
**/
  void reset() {
    nextIn = nextOut = count = 0;
  }  //reset()


  /**
 * Increment a ring buffer index, wrapping around to 0 if necessary
 *
 * @param  index The value to be advanced
 *
 * @return The advanced index
 *
 * The returned index value will always lie within 0..(bfrSize-1).  Advancement
 * is independent of the count of existing entries in the buffer.
 *
**/
  int advance(int index) {
    index++;                          //Increment a buffer index
    if (index >= bfrSize) index = 0;  //Wrap around to beginning?
    return index;
  }  //advance()


  /**
 * Retard (backup) a ring buffer index, wrapping around if necessary
 *
 * @param index The value to be retarded
 *
 * @return The retarded index value
 *
 * The returned value will always lie within 0..(bfrSize-1) even if the index parameter
 * lies outside that range.
 *
**/
  int retard(int index) {
    if (index >= bfrSize) index = bfrSize;  //Repair invalid index
    index--;                                //Backup the index
    if (index < 0) index = bfrSize - 1;     //Deal with wraparound
    return index;
  }  //retard()


  /**
 * Getter for the nextIn member
 *
 * @return Value of the nextIn member
 *
**/
  int getNextIn() {
    return nextIn;
  }


  /**
 * Getter for the bfrSize member
 *
 * @return The maximum number entries that the buffer can hold
 *
**/
  int getBfrSize() {
    return bfrSize;
  }


  /**
 * Add a value to the end of a FIFO ring buffer
 *
 * @param  newValue The value to be added
 * 
 * @return Updated count of entries in buffer or -1 if the buffer was found already full
 *
 * Adds newValue to the end of the ring buffer, advances nextIn, and increments the count.
 * Will not overwrite an existing buffer entry.
 *
**/
  int addNext(long newValue) {
    if (count >= bfrSize) return -1;  //Check for full buffer
    buffer[nextIn] = newValue;        //Add newValue to end of the buffer
    nextIn = advance(nextIn);         //Advance the nextIn buffer index
    count++;                          //Increment count of buffered values
    return count;                     //Number of buffered values
  }                                   //addNext()


  /**
 * Add a value to the end of a FIFO ring buffer, overwriting old values when full
 *
 * @param  newValue The value to be added
 * 
 * @return Updated count of entries in buffer (0..bfrSize)
 *
 * Similar to addNext() but overwrites the oldest entry when buffer overflows.
 * Updates nextOut to index the oldest, entry so that the  FIFO continues to work.
 *
**/
  int overwrite(long newValue) {
    buffer[nextIn] = newValue;  //Add newValue to end of the buffer
    nextIn = advance(nextIn);   //Advance the nextIn buffer index
    count++;                    //Increment count of entries
    if (count > bfrSize) {      //Did buffer overflow?
      nextOut = nextIn;         //After overflow, nextOut=nextIn=Index(Oldest Entry)
      count = bfrSize;          //Maintain buffer at its maximum capacity after overflow
    }
    return count;  //1..bfrSize
  }


  /**
 * Retrieve the next value from a FIFO ring buffer
 *
 * @param &value Retrieved value
 *
 * @return #elements remaining in buffer or -1 if buffer was already empty
 *
 * Retrieves the next entry, advances the nextOut index, and decrements the count
 *
**/
  int getNext(long &value) {

    if (count == 0) return -1;   //Check for empty buffer
    value = buffer[nextOut];     //Retrieve the oldest value from buffer
    nextOut = advance(nextOut);  //Advance the nextOut index
    count--;                     //Decrement count of remaining entries
    return count;                //Number of buffered entries remaining
  }                              //getNext()


  /**
 * Getter for the count of buffered entries
 *
**/
  int getCount() {
    return count;
  }


  /**
 * Set nextOut to the specified value
 *
 * @param index The specified new value
 *
 * @return Updated nextOut if success, else -1 if index lies outside the buffer's range
 *
 * The index must lie in the range, 0..(bfrSize-1)
 *
**/
  int setNextOut(unsigned index) {
    if (index >= bfrSize) return -1;  //Check for invalid index
    nextOut = index;                  //It's valid
    return nextOut;                   //Return valid index
  }


  /**
  *
  * Save the ring buffer's state
  *
  * Saves the ring buffer's indices and count so that the buffer can be
  * iterated multiple times.  Does not save the actual data nor the bfrSize.
  *
  **/
  void saveState(RingBfrState &savedState) {
    savedState.nextIn = nextIn;
    savedState.nextOut = nextOut;
    savedState.count = count;
  }  //saveState()




  /**
  *
  * Restore ring buffer's state
  *
  **/
  void restoreState(RingBfrState savedState) {
    nextIn = savedState.nextIn;
    nextOut = savedState.nextOut;
    count = savedState.count;
  }  //restoreState()


  //Private members
private:
  int nextIn;                      //Index of where next added value will be placed
  int nextOut;                     //Index of where next removed value will be found
  int count;                       //Number of values in the buffer
  static const int bfrSize = 480;  //Maximum number of buffer entries
  long buffer[bfrSize];            //Container for the actual buffered data entries

};  //class RingBfr




/**
 * Function to erase the Adafruit display
 *
**/
void eraseDisplay() {
  tft.fillScreen(HX8357_BLACK);
}



//Build the ring buffer object
RingBfr bfr;

//Initialization
void setup() {



  //Initialize the Arduino world and let console know we're starting
  Serial.begin(9600);
  delay(100);
  Serial.println("Starting...");

  //Initialize the display
  tft.begin(HX8357D);
  eraseDisplay();
  tft.setTextColor(HX8357_YELLOW);
  tft.setRotation(3);  //PocketFT8FW uses 3
  tft.setTextSize(2);


  //Setup to instrument a touch event
  printf("Tap (touch) the screen");
  bfr.reset();  //Reset the ring buffer

}  //setup()



//Define the various activities commanded by the user
enum ACTIVITY { IDLE,           //Awaiting user's command
                RESISTANCE,     //Measuring X-Plate resistance
                NOISE,          //Measuring noise on Y-Plate
                TOUCH,          //Instrumenting a touch event
                SAVE };         //Saving data to an SD file
enum ACTIVITY activity = IDLE;  //Current activity state variable

//Define vars that must be retained between calls to loop()
int eventStart;    //Records index of where touch event begins in ring buffer
int plotStart;     //Records index of where plot begins in ring buffer
int nSamples = 0;  //Number of samples acquired during touch event

//Loop executes user's commands
void loop() {

  //What activity is in-progress?
  switch (activity) {

    case IDLE:
      //Nothing --- Poll the Serial port for the user's next command
      switch (Serial.read()) {
        case 'R':
        case 'r':
          resetResistance();
          activity = RESISTANCE;  //Cmd to measure X-Plate resistance
          break;
        case 'N':
        case 'n':
          resetNoise();
          activity = NOISE;  //Cmd to measure noise on Y-Plate
          break;
        case 'T':
        case 't':
          resetTouch();
          activity = TOUCH;  //Cmd to instrument a touch event
          break;
        case 'S':
        case 's':
          activity = SAVE;  //Cmd to save sample buffer to SD file
          break;
        default:
          break;  //Await a valid user command
      }           //Serial.read
      break;

    case RESISTANCE:
      measureResistance();  //Measuring X-Plate resistance
      break;
    case NOISE:
      measureNoise();  //Measuring Y-Plate noise
      break;
    case TOUCH:
      instrumentTouchEvent();  //Instrumenting touch event
      break;
    case SAVE:
      saveData();  //Save the captured data to SD file
      break;
    default:
      activity = IDLE;  //Attempt recovery from unknown wreck
      break;
  }  //activity
}  //loop()




/**
 * Measure the resistance of the X-Plate
 *
 *
 **/
void resetResistance() {
  eraseDisplay();
  printf("Avoid touching the screen during resistance measuement\n");
}
void measureResistance() {

  MCP342x::Config status;
  long value;
  float sum, v2, rXplate;

  //Setup to measure the X-Plate resistance between XP and XM.
  //Note:  The connections are, GND---XPlate---RXM---VCC, so by measuring V(XM) we have created
  //a voltage divider consisting of the XPlate and RXM, and knowing VCC, we can calculate the unknown
  //XPlate resistance.
  Serial.println("Measuring X-Plate resistance...");
  pinMode(XM, OUTPUT);     //The X-Plate connections are XM...
  pinMode(XP, OUTPUT);     //and XP.
  pinMode(YM, INPUT);      //Float Y plate connections
  pinMode(YP, INPUT);      //Float Y plate connections
  digitalWrite(XM, HIGH);  //Connect Vcc through RXM to XM side of plate
  digitalWrite(XP, LOW);   //Ground XP side of the plate

  //Acquire ~1 second of samples of the voltage appearing on the touchpad's X- pin for the resistance calculation
  int i;
  for (i = 0; i < NSAMPLES; i++) {
    adc.convertAndRead(MCP342x::channel2, MCP342x::oneShot, MCP342x::resolution12, MCP342x::gain1, 100000, value, status);
    samples[i] = value;
  }

  //Calculate the average value of the voltage on touchpad's XM pin (Note:  it may be noisy)
  sum = 0.0;
  for (i = 0; i < NSAMPLES; i++) sum += samples[i];
  v2 = (sum / NSAMPLES) * maxADCreading;  //Average value of voltage appearing on touchpad's XM pin

  //Now we can calculate the resistance of the X-Plate
  rXplate = (RXM * v2) / (VCC - v2);
  printf("X-Plate resistance = %f Ohms\n", rXplate);

  //Calculate the stdev (noise) of the X-Plate voltage measurements.  Note:  We are trying to learn whether
  //erratic touch event coordinates arise from noise on the PCB or from a noisy connection between the
  //X and Y resistive plates during a touch event.  Here we see the PCB noise with ~1 mV sensitivity on
  //the biased Xm without a touch event.
  float sumDevSquared = 0.0;
  for (i = 0; i < NSAMPLES; i++) {
    float deviation = samples[i] - v2;
    sumDevSquared += (deviation * deviation);
  }
  float stdev = sqrt(sumDevSquared / NSAMPLES);
  printf("X-Plate noise measured at XM = %f volts\n", stdev);

}  //measureResistance()





/**
 * Measure noise
 *
 * This activity instruments the noise on the floating Y-Plate, akin to what the ADC samples
 * prior to and following a touch event.  Because the Y-Plate floats during this measurement,
 * this noise may exceed that seen during a touch event when the two plates are connected at a
 * touchpoint.  The samples are taken with about ~1 mV sensitivity.  Unlike many other
 * measurements made in this sketch, these *might* include negative values.  Since there is
 * no source applied to the floating plate, we seemingly can assume the observed noise
 * arose through crosstalk in the display, the PCB, or the ADC itself.
 **/
void resetNoise() {
  eraseDisplay();
  bfr.reset();  //Clear the bfr of old data
  nSamples = 0;
  printf("Avoid touching the screen during noise measurement\n");
}
void measureNoise() {

  long value;
  MCP342x::Config status;

  //Get the next value (0..2047) from the ADC
  adc.convertAndRead(MCP342x::channel2, MCP342x::oneShot, MCP342x::resolution12, MCP342x::gain1, 100000, value, status);

  //Add the ADC value to the ring buffer, overwriting the oldest entry when the buffer eventually fills
  bfr.overwrite(value);  //Always record the sampled value in the ring buffer

  //Tally this sample
  nSamples++;

  //Was this the final sample?
  if (nSamples == 480) {  //We want to acquire 2 seconds of noise data

    //Plot the buffered noise data
    plotBufferedData();
  }
}




/**
  * Save buffered data to SD file
  *
  * Saves the entire contents of the ring buffer to a single column CSV file.  The ring 
  * buffer's state remains unchanged.  Files are named, FILE01.CSV, FILE02.CSV, etc.  
  * Existing files by the same name, if any, are overwritten.
  *
  **/
unsigned filenameIndex = 0;
void saveData() {
  char filename[256];
  RingBfrState savedState;
  char s[256];
  long value;

  //Build the filename
  sprintf(filename, "/FILE%2d.CSV", filenameIndex);

  //Initialize the Teensy SD library if the card is available
  if (!SD.begin(BUILTIN_SDCARD)) {
    Serial.println("Error:  Unable to access the SD card");
    activity = IDLE;
    return;
  }

  //Create the file
  File dataFile = SD.open(filename, FILE_WRITE);
  if (!dataFile) {
    Serial.printf("Error:  Unable to create %s\n", filename);
    activity = IDLE;
    return;
  }

  //Write the contents of the ring buffer to dataFile
  bfr.saveState(savedState);  //Save ring buffer state
  for (int i = 0; i < bfr.getCount(); i++) {
    bfr.getNext(value);
    sprintf(s, "%d\n", value);
    dataFile.write(s);
  }
  bfr.restoreState(savedState);
  dataFile.close();
  activity = IDLE;
}



/**
 * Instrumenting a touch event
 *
 * The idea here is to capture the voltage on the Y-Plate before, during, and after a touch event
 * to observe the noise (dropouts, varying contact resistance between the plates, crosstalk, etc)
 * underlying the touchscreen's erratic behavior.  The 12-bit ADC has a resolution of about 1 mV
 * and negative readings seem unlikely while the plates are touching.
 *
 * Note:  nSamples counts the number of samples made after the user pressed the (T)ouch command
 * while bfr.count tracks the number of recorded samples starting about 0.1 second before the
 * touch event begins.  We expect to record a total of about 2 seconds of data prior to, during
 * and following the touch event.
 *
 **/
bool negativeReadingObserved;
void resetTouch() {
  eraseDisplay();
  bfr.reset();                      //Clear the bfr of old data
  nSamples = 0;                     //At this point, we have acquired no samples
  negativeReadingObserved = false;  //These are unexpected, at least during an event
  touchEventInProgress = false;     //Reset to capture a new event
  printf("Tap the touchscreen\n");  //Prompt the user
}
void instrumentTouchEvent() {

  long value;
  MCP342x::Config status;

  //Get the next value (0..2047) from the ADC
  adc.convertAndRead(MCP342x::channel2, MCP342x::oneShot, MCP342x::resolution12, MCP342x::gain1, 100000, value, status);
  if (value < 0) negativeReadingObserved = true;  //Unexpected

  //Add the ADC value to the ring buffer, overwriting the oldest entry when the buffer eventually fills
  bfr.overwrite(value);  //Always record the sampled value in the ring buffer

  //Did we receive the first valid sample of a new touch event?
  if (touchEventInProgress == false && value >= MINPRESSURE) {

    //Yes, a touch event has begun
    eventStart = bfr.retard(bfr.getNextIn());  //Record index of where touch event starts in ring buffer
    touchEventInProgress = true;               //State variable indicating touch event has begun

    //Start the plot either 24 or getCount() entries behind the eventStart index
    plotStart = eventStart;                 //Index of where touch event begins
    int nRetard = min(bfr.getCount(), 24);  //Number of entries to retard plotStart (accounting for bfr.count)
    for (int n = 1; n <= nRetard; n++)      //Loop retards plotStart 0.1 second of samples
      plotStart = bfr.retard(plotStart);    //Back the index one position
    bfr.setNextOut(plotStart);              //Set bfr.nextOut to index of where plot will begin
  }

  //Should we tally this sample as part of a touch event?
  if (touchEventInProgress)
    nSamples++;  //Yes, this sample is part of a touch event

  //Was this the final sample in the touch event?
  if (nSamples == (480 - 24)) {  //We want to acquire 1.9 seconds of data after the event starts

    //Yes, stop acquiring samples.  We have 0.1 second of data prior to event and 1.9 during the event
    touchEventInProgress = false;  //Setup for next touch event
    nSamples = 0;                  //Reset tally for next event

    //Plot the buffered data from the touch event
    if (negativeReadingObserved) {
      printf("A negative reading was observed, will not be plotted, but can be (S)aved\n");
    }
    plotBufferedData();
  }

}  //instrumentTouchEvent()




/**
 * Plot the buffered data on the display
 *
 * Plots the data from the ring buffer, leaving the buffer's state unchanged so it can
 * be accessed again.  Assumes that the buffered values were obtained from the 12-bit
 * ADC and range within 0..2047.  Assumes a 320x480 display.
 *
 * Negative readings plotted as 0.
 *
 **/
void plotBufferedData() {

  long value;

  RingBfrState savedState;

  //Save the ring buffer's state
  bfr.saveState(savedState);

  //Plot the acquired data from the ring buffer
  for (int i = 0; i < bfr.getCount(); i++) {
    bfr.getNext(value);                              //Retrieve value from FIFO ring buffer
    if (value < 0) value = 0;                        //Truncate negative readings
    value = 319.0 * float(value) / 2047.0;           //Scale value to fit on the 320x480 display's Y axis
    tft.drawPixel(i, (int16_t)value, HX8357_WHITE);  //Plot this point
  }

  //Restore the ring buffer's state
  bfr.restoreState(savedState);
  printf("You can save the plot data with the (S)ave command\n");

}  //plotBufferedData()