#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
// Touch screen library with X Y and Z (pressure) readings as well
// as oversampling to avoid 'bouncing'
// (c) ladyada / adafruit
// Code under MIT License

#include "Arduino.h"
#include "pins_arduino.h"
#include <Wire.h>
#include "MCP342x.h"

//Define which I2C bus hosts the MCP342x
#define MCP342X_WIRE Wire2

#include "DEBUG.h"

#ifdef __AVR
#include <avr/pgmspace.h>
#elif defined(ESP8266)
#include <pgmspace.h>
#endif
#include "TouchScreen_I2C.h"

// increase or decrease the touchscreen oversampling. This is a little different
// than you make think: 1 is no oversampling, whatever data we get is
// immediately returned 2 is double-sampling and we only return valid data if
// both points are the same 3+ uses insert sort to get the median value. We
// found 2 is precise yet not too slow so we suggest sticking with it!
#define NUMSAMPLES 2

uint8_t address = 0x69;  //Original I2C address was 0x68 but V1.01 chip reports 0x68
MCP342x adc = MCP342x(address);

TSPoint::TSPoint(void) {
  x = y = z = 0;
}
/**
 * @brief Construct a new TSPoint::TSPoint object
 *
 * @param x0 The point's X value
 * @param y0 The point's Y value
 * @param z0 The point's Z value
 */
TSPoint::TSPoint(int16_t x0, int16_t y0, int16_t z0) {
  x = x0;
  y = y0;
  z = z0;
}
/**
 * @brief Check if the current point is **not** equivalent to another point
 *
 * @param p1 The other point being checked for equivalence
 * @return `true` : the two points are equivalent
 * `false`: the two points are **not** equivalent
 */
bool TSPoint::operator==(TSPoint p1) {
  return ((p1.x == x) && (p1.y == y) && (p1.z == z));
}
/**
 * @brief Check if the current point is **not** equivalent to another point
 *
 * @param p1 The other point being checked for equivalence

 * @return `true` :the two points are **not** equivalent
 * `false`: the two points are equivalent
 */
bool TSPoint::operator!=(TSPoint p1) {
  return ((p1.x != x) || (p1.y != y) || (p1.z != z));
}

#if (NUMSAMPLES > 2)
static void insert_sort(int array[], uint8_t size) {
  uint8_t j;
  int save;

  for (int i = 1; i < size; i++) {
    save = array[i];
    for (j = i; j >= 1 && save < array[j - 1]; j--)
      array[j] = array[j - 1];
    array[j] = save;
  }
}
#endif
/**
 * @brief Measure the X, Y, and pressure and return a TSPoint with the
 * measurements
 *
 * @return TSPoint The measured X, Y, and Z/pressure values
 */
TSPoint TouchScreen::getPoint(void) {
  //return(TSPoint(0,0,0));     //Uncomment to debug I2C/SI4735 noise issues
  int x, y, z;
  int samples[NUMSAMPLES];
  uint8_t i, valid;

  long value1 = 0;
  long value2 = 0;
  uint8_t err;
  MCP342x::Config status;

  valid = 1;

  pinMode(_yp, INPUT);
  pinMode(_ym, INPUT);
  pinMode(_xp, OUTPUT);
  pinMode(_xm, OUTPUT);

  digitalWrite(_xm, HIGH);
  digitalWrite(_xp, LOW);
  delayMicroseconds(20);  // Fast ARM chips need to allow voltages to settle

  for (i = 0; i < NUMSAMPLES; i++) {
    // samples[i] = analogRead(_yp);
    err = adc.convertAndRead(MCP342x::channel1, MCP342x::oneShot, MCP342x::resolution12, MCP342x::gain1, 1000000, value1, status);
    samples[i] = value1;
  }


#if NUMSAMPLES == 2
  // Allow small amount of measurement noise, because capacitive
  // coupling to a TFT display's signals can induce some noise.
  if (samples[0] - samples[1] < -4 || samples[0] - samples[1] > 4) {
    valid = 0;
  } else {
    samples[1] = (samples[0] + samples[1]) >> 1;  // average 2 samples
  }
#endif

  y = samples[1];


  pinMode(_xp, INPUT);
  pinMode(_xm, INPUT);
  pinMode(_yp, OUTPUT);
  pinMode(_ym, OUTPUT);

  digitalWrite(_ym, LOW);
  digitalWrite(_yp, HIGH);
  delayMicroseconds(20);  // Fast ARM chips need to allow voltages to settle


  for (i = 0; i < NUMSAMPLES; i++) {
    //samples[i] = analogRead(_xm);
    err = adc.convertAndRead(MCP342x::channel2, MCP342x::oneShot, MCP342x::resolution12, MCP342x::gain1, 1000000, value2, status);
    samples[i] = value2;
  }

#if NUMSAMPLES == 2
  // Allow small amount of measurement noise, because capacitive
  // coupling to a TFT display's signals can induce some noise.
  if (samples[0] - samples[1] < -4 || samples[0] - samples[1] > 4) {
    valid = 0;
  } else {
    samples[1] = (samples[0] + samples[1]) >> 1;  // average 2 samples
  }
#endif

  x = samples[1];



  if (!valid) {
    z = 0;
  } else {
    z = x + y;
  }
  return TSPoint(x, y, z);
}

TouchScreen::TouchScreen(uint8_t xp, uint8_t yp, uint8_t xm, uint8_t ym,
                         uint16_t rxplate = 0) {
  _yp = yp;
  _xm = xm;
  _ym = ym;
  _xp = xp;
  _rxplate = rxplate;


  MCP342X_WIRE.begin();
  MCP342x::generalCallReset();
  delay(1);  // MC342x needs 300us to settle, wait 1ms

#if defined(USE_FAST_PINIO)
  xp_port = portOutputRegister(digitalPinToPort(_xp));
  yp_port = portOutputRegister(digitalPinToPort(_yp));
  xm_port = portOutputRegister(digitalPinToPort(_xm));
  ym_port = portOutputRegister(digitalPinToPort(_ym));

  xp_pin = digitalPinToBitMask(_xp);
  yp_pin = digitalPinToBitMask(_yp);
  xm_pin = digitalPinToBitMask(_xm);
  ym_pin = digitalPinToBitMask(_ym);
#endif

  pressureThreshhold = 10;
}
/**
 * @brief Read the touch event's X value
 *
 * @return int the X measurement
 */
int TouchScreen::readTouchX(void) {
  pinMode(_yp, INPUT);
  pinMode(_ym, INPUT);
  digitalWrite(_yp, LOW);
  digitalWrite(_ym, LOW);

  pinMode(_xp, OUTPUT);
  digitalWrite(_xp, HIGH);
  pinMode(_xm, OUTPUT);
  digitalWrite(_xm, LOW);

  return (1023 - analogRead(_yp));
}
/**
 * @brief Read the touch event's Y value
 *
 * @return int the Y measurement
 */
int TouchScreen::readTouchY(void) {
  pinMode(_xp, INPUT);
  pinMode(_xm, INPUT);
  digitalWrite(_xp, LOW);
  digitalWrite(_xm, LOW);

  pinMode(_yp, OUTPUT);
  digitalWrite(_yp, HIGH);
  pinMode(_ym, OUTPUT);
  digitalWrite(_ym, LOW);

  return (1023 - analogRead(_xm));
}
/**
 * @brief Read the touch event's Z/pressure value
 *
 * @return int the Z measurement
 */
uint16_t TouchScreen::pressure(void) {
  // Set X+ to ground
  pinMode(_xp, OUTPUT);
  digitalWrite(_xp, LOW);

  // Set Y- to VCC
  pinMode(_ym, OUTPUT);
  digitalWrite(_ym, HIGH);

  // Hi-Z X- and Y+
  digitalWrite(_xm, LOW);
  pinMode(_xm, INPUT);
  digitalWrite(_yp, LOW);
  pinMode(_yp, INPUT);

  int z1 = analogRead(_xm);
  int z2 = analogRead(_yp);

  if (_rxplate != 0) {
    // now read the x
    float rtouch;
    rtouch = z2;
    rtouch /= z1;
    rtouch -= 1;
    rtouch *= readTouchX();
    rtouch *= _rxplate;
    rtouch /= 1024;

    return rtouch;
  } else {
    return (1023 - (z2 - z1));
  }
}
