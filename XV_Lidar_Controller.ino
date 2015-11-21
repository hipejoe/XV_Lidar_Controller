/*
  XV Lidar Controller v1.2.2
 
 Copyright 2014 James LeRoy getSurreal
 https://github.com/getSurreal/XV_Lidar_Controller
 http://www.getsurreal.com/products/xv-lidar-controller
 
 Modified to add CRC checking - Doug Hilton, WD0UG November, 2015 mailto: six.speed (at) yahoo (dot) com
 
 See README for additional information 
 
 The F() macro in the Serial statements tells the compiler to keep your strings in PROGMEM
 */

#include <TimerThree.h> // used for ultrasonic PWM motor control
#include <PID.h>
#include <EEPROM.h>
#include "EEPROMAnything.h"
#include <SerialCommand.h>

const int N_ANGLES = 360;                                       // # of angles (0..359)
const int SHOW_ALL_ANGLES = N_ANGLES;                           // value means 'display all angle data, 0..359'

struct EEPROM_Config {
  byte id;
  char version[6];
  int motor_pwm_pin;    // pin connected to mosfet for motor speed control
  double rpm_setpoint;  // desired RPM (uses double to be compatible with PID library)
  double rpm_min;
  double rpm_max;
  double pwm_max;       // max analog value.  probably never needs to change from 1023
  double pwm_min;       // min analog pulse value to spin the motor
  int sample_time;      // how often to calculate the PID values

  // PID tuning values
  double Kp;
  double Ki;
  double Kd;

  boolean motor_enable;  // to spin the laser or not.  No data when not spinning
  boolean raw_data;  // to retransmit the seiral data to the USB port
  boolean show_dist;  //  controlled by ShowDist and HideDist commands
  boolean show_rpm;  // controlled by ShowRPM and HideRPM commands
  unsigned int show_angle;  // controlled by ShowAngle (0 - 359, 360 shows all)
} 
xv_config;

const byte EEPROM_ID = 0x05;  // used to validate EEPROM initialized

double pwm_val = 500;  // start with ~50% power
double pwm_last;
double motor_rpm;
unsigned long now;
unsigned long motor_check_timer = millis();
unsigned long motor_check_interval = 200;  
unsigned int rpm_err_thresh = 10;  // 2 seconds (10 * 200ms) to shutdown motor with improper RPM and high voltage
unsigned int rpm_err = 0;
unsigned long curMillis;
unsigned long lastMillis = millis();

// Added by DSH
const unsigned char COMMAND = 0xFA;        // Start of new packet
const int INDEX_LO = 0xA0;                 // lowest index value
const int INDEX_HI = 0xF9;                 // highest index value

const int N_DATA_QUADS = 4;                // there are 4 groups of data elements
const int N_ELEMENTS_PER_QUAD = 4;         // viz., 0=distance LSB; 1=distance MSB; 2=sig LSB; 3=sig MSB

// Offsets to bytes within 'Packet'
const int OFFSET_TO_START = 0;
const int OFFSET_TO_INDEX = OFFSET_TO_START + 1;
const int OFFSET_TO_SPEED_LSB = OFFSET_TO_INDEX + 1;
const int OFFSET_TO_SPEED_MSB = OFFSET_TO_SPEED_LSB + 1;
const int OFFSET_TO_4_DATA_READINGS = OFFSET_TO_SPEED_MSB + 1;
const int OFFSET_TO_CRC_L = OFFSET_TO_4_DATA_READINGS + (N_DATA_QUADS * N_ELEMENTS_PER_QUAD);
const int OFFSET_TO_CRC_M = OFFSET_TO_CRC_L + 1;
const int PACKET_LENGTH = OFFSET_TO_CRC_M + 1;  // length of a complete packet
// Offsets to the (4) elements of each of the (4) data quads
const int OFFSET_DATA_DISTANCE_LSB = 0;
const int OFFSET_DATA_DISTANCE_MSB = OFFSET_DATA_DISTANCE_LSB + 1;
const int OFFSET_DATA_SIGNAL_LSB = OFFSET_DATA_DISTANCE_MSB + 1;
const int OFFSET_DATA_SIGNAL_MSB = OFFSET_DATA_SIGNAL_LSB + 1;

int Packet[PACKET_LENGTH];                 // an input packet
int ixPacket = 0;                          // index into 'Packet' array
const int VALID_PACKET = 0;
const int INVALID_PACKET = VALID_PACKET + 1;
const byte INVALID_DATA_FLAG = (1 << 7);      // Mask for byte 1 of each data quad "Invalid data"
/* REF: https://github.com/Xevel/NXV11/wiki
The bit 7 of byte 1 seems to indicate that the distance could not be calculated.
It's interesting to see that when this bit is set, the second byte is always 80, and the values of the first byte seem to be 
only 02, 03, 21, 25, 35 or 50... When it's 21, then the whole block is 21 80 XX XX, but for all the other values it's the 
data block is YY 80 00 00 maybe it's a code to say what type of error ? (35 is preponderant, 21 seems to be when the beam is 
interrupted by the supports of the cover) .
*/
const byte STRENGTH_WARNING_FLAG = (1 << 6);  // Mask for byte 1 of each data quat "Strength Warning"
/*
The bit 6 of byte 1 is a warning when the reported strength is greatly inferior to what is expected at this distance. 
This may happen when the material has a low reflectance (black material...), or when the dot does not have the expected 
size or shape (porous material, transparent fabric, grid, edge of an object...), or maybe when there are parasitic 
reflections (glass... ).
*/
const byte BAD_DATA_MASK = (INVALID_DATA_FLAG | STRENGTH_WARNING_FLAG);

const byte eState_Find_COMMAND = 0;                        // 1st state: find 0xFA (COMMAND) in input stream
const byte eState_Build_Packet = eState_Find_COMMAND + 1;  // 2nd state: build the packet
int eState = eState_Find_COMMAND;
PID rpmPID(&motor_rpm, &pwm_val, &xv_config.rpm_setpoint, xv_config.Kp, xv_config.Ki, xv_config.Kd, DIRECT);

uint8_t inByte = 0;  // incoming serial byte
//uint16_t data_status = 0;
//uint16_t data_4deg_index = 0;
//uint16_t data_loop_index = 0;
uint8_t motor_rph_high_byte = 0; 
uint8_t motor_rph_low_byte = 0;
//uint8_t data0, data2;
uint16_t aryDist[N_DATA_QUADS] = {0,0,0,0};      // thre are (4) distances, one for each data quad
// so the maximum distance is 16383 mm (0x3FFF)
uint16_t aryQuality[N_DATA_QUADS] = {0,0,0,0};   // same with 'quality'
uint16_t motor_rph = 0;
uint16_t startingAngle = 0;                   // the first scan angle (of group of 4, based on 'index'), in degrees (0..359)

SerialCommand sCmd;

const int ledPin = 11;
boolean ledState = LOW;

// initialization (before 'loop')
void setup() {
  EEPROM_readAnything(0, xv_config);
  if( xv_config.id != EEPROM_ID) { // verify EEPROM values have been initialized
    initEEPROM();
  }
  pinMode(xv_config.motor_pwm_pin, OUTPUT); 
  Serial.begin(115200);                            // USB serial
  Serial1.begin(115200);                           // XV LDS data 

  Timer3.initialize(30);                           // set PWM frequency to 32.768kHz  

  rpmPID.SetOutputLimits(xv_config.pwm_min,xv_config.pwm_max);
  rpmPID.SetSampleTime(xv_config.sample_time);
  rpmPID.SetTunings(xv_config.Kp, xv_config.Ki, xv_config.Kd);
  rpmPID.SetMode(AUTOMATIC);

  initSerialCommands();
  pinMode(ledPin, OUTPUT);
  
  eState = eState_Find_COMMAND;
  for (ixPacket = 0; ixPacket < PACKET_LENGTH; ixPacket++)  // Initialize
    Packet[ixPacket] = 0;
  ixPacket = 0; 

  //hideRaw();      // DSH ONLY!!!!!!!!!!!!
  
}
// Main loop (forever)
void loop() {
  byte aryInvalidDataFlag[N_DATA_QUADS] = {0,0,0,0};  // non-zero = INVALID_DATA_FLAG or STRENGTH_WARNING_FLAG is set
  
  sCmd.readSerial();  // check for incoming serial commands
  if (Serial1.available() > 0) {                  // read byte from LIDAR and relay to USB    
    inByte = Serial1.read();                      // get incoming byte:
    if (xv_config.raw_data)
      Serial.print(inByte, BYTE);                 // relay

    // Switch, based on 'eState':
    // State 1: We're scanning for 0xFA (COMMAND) in the input stream
    // State 2: Build a complete data packet
    if (eState == eState_Find_COMMAND) {          // flush input until we get COMMAND byte
      if(inByte == COMMAND) {
        eState++;                                 // switch to 'build a packet' state
        Packet[ixPacket++] = inByte;              // store 1st byte of data into 'Packet'
      }
    }
    else {                                            // eState == eState_Build_Packet
      Packet[ixPacket++] = inByte;                    // keep storing input into 'Packet'
      if (ixPacket == PACKET_LENGTH) {                // we've got all the input bytes, so we're done building this packet
        if (eValidatePacket() == VALID_PACKET) {      // Check packet CRC
          startingAngle = processIndex();             // get the starting angle of this group (of 4), e.g., 0, 4, 8, 12, ...        
          processSpeed();                             // process the speed
          // process each of the (4) sets of data in the packet        
          for (int ix = 0; ix < N_DATA_QUADS; ix++)   // process the distance
            aryInvalidDataFlag[ix] = processDistance(ix);
          for (int ix = 0; ix < N_DATA_QUADS; ix++) { // process the signal strength (quality)
            aryQuality[ix] = 0;
            if (aryInvalidDataFlag[ix] == 0)
              processSignalStrength(ix);
          }
          if (xv_config.show_dist) {                       // the 'ShowDistance' command is active
            if (xv_config.show_angle == SHOW_ALL_ANGLES    // Are we showing all angles or just 1 angle?
            || ((xv_config.show_angle >= startingAngle) && (xv_config.show_angle < startingAngle + N_DATA_QUADS))) {
              for (int ix = 0; ix < N_DATA_QUADS; ix++) {  // process each of the (4) angles
                if ((xv_config.show_angle == SHOW_ALL_ANGLES) || (xv_config.show_angle == startingAngle + ix)) {
                  if (aryInvalidDataFlag[ix] == 0) {       // make sure that the 'Invalid Data' flag is clear                                       
                    Serial.print(startingAngle + ix);
                    Serial.print(F(": "));
                    Serial.print(int(aryDist[ix]));
                    Serial.print(F(" ("));
                    Serial.print(aryQuality[ix]);
                    Serial.println(F(")")); 
                  }
                  else {                    
                    /* // UNCOMMENT BELOW TO PRINT 'INVALID' and 'SIGNAL' ERROR MESSAGES IN REAL-TIME
                    Serial.print(startingAngle + ix);
                    Serial.print(F(": "));
                    for (int ix = 0; ix < PACKET_LENGTH; ix++) {
                      if (Packet[ix] < 0x10)
                        Serial.print("0");
                      Serial.print(Packet[ix], HEX);
                      Serial.print(" ");
                    }
                    for (int ix = 0; ix < N_DATA_QUADS; ix++) {
                      if (aryInvalidDataFlag[ix] & INVALID_DATA_FLAG)
                        Serial.print("{I}");
                      else
                        Serial.print("{_}");
                    }
                    for (int ix = 0; ix < N_DATA_QUADS; ix++) {
                      if (aryInvalidDataFlag[ix] & STRENGTH_WARNING_FLAG) 
                        Serial.print("{S}");
                      else
                        Serial.print("{_}");
                    }                    
                    Serial.println("");
                    */ // UNCOMMENT ABOVE
                  }  // else
                }  // if ((xv_config.show_angle == SHOW_ALL_ANGLES) ...
              }  // or (int ix = 0; ix < N_DATA_QUADS; ix++)
            }  // if (xv_config.show_angle == SHOW_ALL_ANGLES ...
          }  // if (xv_config.show_dist)
        }  // if (eValidatePacket() == 0
        else {
          //Serial.println("Skipping packet with bad CRC");          
        }
        // initialize a bunch of stuff before we switch back to State 1
        for (int ix = 0; ix < N_DATA_QUADS; ix++) {
          aryDist[ix] = 0;
          aryQuality[ix] = 0;
          aryInvalidDataFlag[ix] = 0;
        }
        for (ixPacket = 0; ixPacket < PACKET_LENGTH; ixPacket++)  // clear out this packet
          Packet[ixPacket] = 0;
        ixPacket = 0;      
        eState = eState_Find_COMMAND;                // This packet is done -- look for next COMMAND byte        
      }  // if (ixPacket == PACKET_LENGTH)
    }  // if (eState == eState_Find_COMMAND)
  }  // if (Serial1.available() > 0)
  if (xv_config.motor_enable) {  
    rpmPID.Compute();
    if (pwm_val != pwm_last) {
      Timer3.pwm(xv_config.motor_pwm_pin, pwm_val);  // replacement for analogWrite()
      pwm_last = pwm_val;
    }
    motorCheck();
  }  // if (xv_config.motor_enable)
}  // loop
/*
 * processIndex - Process the packet element 'index'
 * index is the index byte in the 90 packets, going from A0 (packet 0, readings 0 to 3) to F9 
 *    (packet 89, readings 356 to 359).
 * Enter with: N/A
 * Uses:       Packet
 *             ledState gets toggled if angle = 0
 *             ledPin = which pin the LED is connected to
 *             ledState = LED on or off
 *             xv_config.show_dist = true if we're supposed to show distance
 *             curMillis = milliseconds, now
 *             lastMillis = milliseconds, last time through this subroutine
 * Calls:      digitalWrite() - used to toggle LED pin
 *             Serial.print
 * Returns:    The first angle (of 4) in the current 'index' group
 */
uint16_t processIndex() {
  uint16_t angle = 0;
  uint16_t data_4deg_index = Packet[OFFSET_TO_INDEX] - INDEX_LO;
  angle = data_4deg_index * N_DATA_QUADS;     // 1st angle in the set of 4  
  if (angle == 0) {
    if (ledState) {
      ledState = LOW;
    } 
    else {
      ledState = HIGH;
    }
    digitalWrite(ledPin, ledState);
    if (xv_config.show_dist) {
      curMillis = millis();
      if(xv_config.show_angle == SHOW_ALL_ANGLES) {
        /*
        Serial.print(F("Time Interval: "));
        Serial.println(curMillis - lastMillis);
        */
      }
      lastMillis = curMillis;
    }
  } // if (angle == 0)
  return angle;
}
/*
 * processSpeed- Process the packet element 'speed'
 * speed is two-bytes of information, little-endian. It represents the speed, in 64th of RPM (aka value 
 *    in RPM represented in fixed point, with 6 bits used for the decimal part).
 * Enter with: N/A
 * Uses:       Packet
 *             angle = if 0 then enable display of RPM and PWM
 *             xv_config.show_rpm = true if we're supposed to display RPM and PWM
 * Calls:      Serial.print
 */
void processSpeed() {
  motor_rph_low_byte = Packet[OFFSET_TO_SPEED_LSB];
  motor_rph_high_byte = Packet[OFFSET_TO_SPEED_MSB];
  motor_rph = (motor_rph_high_byte << 8) | motor_rph_low_byte;
  motor_rpm = float( (motor_rph_high_byte << 8) | motor_rph_low_byte ) / 64.0;
  if (xv_config.show_rpm and startingAngle == 0) {
    Serial.print(F("RPM: "));
    Serial.print(motor_rpm);
    Serial.print(F("  PWM: "));   
    Serial.println(pwm_val);
  }
}
/*
 * Data 0 to Data 3 are the 4 readings. Each one is 4 bytes long, and organized as follows :
 *   byte 0 : <distance 7:0>
 *   byte 1 : <"invalid data" flag> <"strength warning" flag> <distance 13:8>
 *   byte 2 : <signal strength 7:0>
 *   byte 3 : <signal strength 15:8>
 */
/*
 * processDistance- Process the packet element 'distance'
 * Enter with: iQuad = which one of the (4) readings to process, value = 0..3
 * Uses:       Packet
 *             dist[] = sets distance to object in binary: ISbb bbbb bbbb bbbb
 *                                     so maximum distance is 0x3FFF (16383 decimal) millimeters (mm)
 * Calls:      N/A
 * Exits with: 0 = okay
 * Error:      1 << 7 = INVALID_DATA_FLAG is set
 *             1 << 6 = STRENGTH_WARNING_FLAG is set
 */
byte processDistance(int iQuad) {
  uint8_t dataL, dataM;
  aryDist[iQuad] = 0;                     // initialize
  int iOffset = OFFSET_TO_4_DATA_READINGS + (iQuad * N_DATA_QUADS) + OFFSET_DATA_DISTANCE_LSB;    
  // byte 0 : <distance 7:0> (LSB)
  // byte 1 : <"invalid data" flag> <"strength warning" flag> <distance 13:8> (MSB)
  dataM = Packet[iOffset + 1];           // get MSB of distance data + flags    
  if (dataM & BAD_DATA_MASK)             // if either INVALID_DATA_FLAG or STRENGTH_WARNING_FLAG is set...
    return dataM & BAD_DATA_MASK;        // ...then return non-zero
  dataL = Packet[iOffset];               // LSB of distance data
  aryDist[iQuad] = dataL | ((dataM & 0x3F) << 8);
  return 0;                              // okay
}
/*
 * processSignalStrength- Process the packet element 'signal strength'
 * Enter with: iQuad = which one of the (4) readings to process, value = 0..3
 * Uses:       Packet
 *             quality[] = signal quality
 * Calls:      N/A
 */
void processSignalStrength(int iQuad) {
  uint8_t dataL, dataM;
  aryQuality[iQuad] = 0;                        // initialize
  int iOffset = OFFSET_TO_4_DATA_READINGS + (iQuad * N_DATA_QUADS) + OFFSET_DATA_SIGNAL_LSB;    
  dataL = Packet[iOffset];                  // signal strength LSB  
  dataM = Packet[iOffset + 1];
  aryQuality[iQuad] = dataL | (dataM << 8);
}

/*
 * eValidatePacket - Validate 'Packet'
 * Enter with: 'Packet' is ready to check
 * Uses:       CalcCRC
 * Exits with: 0 = Packet is okay
 * Error:      non-zero = Packet is no good
 */
byte eValidatePacket() {
  unsigned long chk32;
  unsigned long checksum;
  const int bytesToCheck = PACKET_LENGTH - 2;
  const int CalcCRC_Len = bytesToCheck / 2;
  unsigned int CalcCRC[CalcCRC_Len];

  byte b1a, b1b, b2a, b2b;
  int ix;

  for (int ix = 0; ix < CalcCRC_Len; ix++)       // initialize 'CalcCRC' array
    CalcCRC[ix] = 0;
    
  // Perform checksum validity test
  for (ix = 0; ix < bytesToCheck; ix += 2)      // build 'CalcCRC' array
    CalcCRC[ix / 2] = Packet[ix] + ((Packet[ix + 1]) << 8);          

  chk32 = 0;
  for (ix = 0; ix < CalcCRC_Len; ix++) 
    chk32 = (chk32 << 1) + CalcCRC[ix];            
  checksum = (chk32 & 0x7FFF) + (chk32 >> 15);
  checksum &= 0x7FFF;
  b1a = checksum & 0xFF;
  b1b = Packet[OFFSET_TO_CRC_L];
  b2a = checksum >> 8;
  b2b = Packet[OFFSET_TO_CRC_M];
  if ((b1a == b1b) && (b2a == b2b)) 
    return VALID_PACKET;                       // okay
  else
    return INVALID_PACKET;                     // non-zero = bad CRC
}

/*
 * initEEPROM
 */
void initEEPROM() {
  xv_config.id = 0x05;
  strcpy(xv_config.version, "1.2.2");
  xv_config.motor_pwm_pin = 9;  // pin connected N-Channel Mosfet

  xv_config.rpm_setpoint = 300;  // desired RPM
  xv_config.rpm_min = 200;
  xv_config.rpm_max = 300;
  xv_config.pwm_min = 100;
  xv_config.pwm_max = 1023;
  xv_config.sample_time = 20;
  xv_config.Kp = 2.0;
  xv_config.Ki = 1.0;
  xv_config.Kd = 0.0;

  xv_config.motor_enable = true;
  xv_config.raw_data = true;
  xv_config.show_dist = false;
  xv_config.show_rpm = false;
  xv_config.show_angle = SHOW_ALL_ANGLES;

  EEPROM_writeAnything(0, xv_config);
}
/*
 * initSerialCommands
 */
void initSerialCommands() {
  sCmd.addCommand("help",       help);
  sCmd.addCommand("Help",       help);
  sCmd.addCommand("ShowConfig",  showConfig);
  sCmd.addCommand("SaveConfig", saveConfig);
  sCmd.addCommand("ResetConfig",initEEPROM);

  sCmd.addCommand("SetRPM",        setRPM);
  sCmd.addCommand("SetKp",         setKp);
  sCmd.addCommand("SetKi",         setKi);
  sCmd.addCommand("SetKd",         setKd);
  sCmd.addCommand("SetSampleTime", setSampleTime);

  sCmd.addCommand("ShowRPM",  showRPM);
  sCmd.addCommand("HideRPM",  hideRPM);
  sCmd.addCommand("ShowDist",  showDist);
  sCmd.addCommand("HideDist",  hideDist);
  sCmd.addCommand("ShowAngle",  showAngle);
  sCmd.addCommand("HideAngle",  hideDist);
  sCmd.addCommand("MotorOff", motorOff);
  sCmd.addCommand("MotorOn",  motorOn);
  sCmd.addCommand("HideRaw", hideRaw);
  sCmd.addCommand("ShowRaw",  showRaw);
}
/*
 * showRPM
 */
void showRPM() {
  xv_config.show_rpm = true;
  if (xv_config.raw_data == true) {
    hideRaw();
  }
  Serial.println(F(" "));
  Serial.print(F("Showing RPM data"));
  Serial.println(F(" "));
}
/*
 * hideRPM
 */
void hideRPM() {
  xv_config.show_rpm = false;
  Serial.println(F(" "));
  Serial.print(F("Hiding RPM data"));
  Serial.println(F(" "));
}

void showDist() {
  xv_config.show_dist = true;
  if (xv_config.raw_data == true) {
    hideRaw();
  }
  xv_config.show_angle = SHOW_ALL_ANGLES;
  Serial.println(F(" "));
  Serial.print(F("Showing Distance data <Angle>: <dist.mm> (quality)}"));
  Serial.println(F(" "));
}

void hideDist() {
  xv_config.show_dist = false;
  xv_config.show_angle = SHOW_ALL_ANGLES;              // set default back to 'show all angles'
  Serial.println(F(" "));
  Serial.print(F("Hiding Distance data"));
  Serial.println(F(" "));
}

void showAngle() {
  showDist(); 
  double sVal = 0.0;
  char *arg;
  boolean syntax_error = false;

  arg = sCmd.next();
  if (arg != NULL) {
    sVal = atoi(arg);    // Converts a char string to a int
    if (sVal < 0 or sVal > SHOW_ALL_ANGLES) {
      syntax_error = true;
    }
  }
  else {
    syntax_error = true;
  }

  arg = sCmd.next();
  if (arg != NULL) {
    syntax_error = true;
  }

  if (syntax_error) {
    Serial.println(F(" "));
    Serial.print(F("Incorrect syntax.  Example: ShowAngle 0 (0 - 359 or 360 for all)")); 
    Serial.println(F(" "));
  }
  else {
    xv_config.show_angle = sVal;
    Serial.println(F(" "));
    if (xv_config.show_angle == SHOW_ALL_ANGLES)
      Serial.print(F("Showing All Angles"));
    else {
      Serial.print(F("Showing Only Angle: "));
      Serial.println(sVal);
    }
    Serial.println(F(" "));
  }  // if (syntax_error)
}

void motorOff() {
  xv_config.motor_enable = false;
  Timer3.pwm(xv_config.motor_pwm_pin, 0);
  Serial.println(F(" "));
  Serial.print(F("Motor off"));
  Serial.println(F(" "));
}

void motorOn() {
  xv_config.motor_enable = true;
  Timer3.pwm(xv_config.motor_pwm_pin, pwm_val);
  rpm_err = 0;  // reset rpm error
  Serial.println(F(" "));
  Serial.print(F("Motor on"));
  Serial.println(F(" "));
}

void motorCheck() {  // Make sure the motor RPMs are good else shut it down
  now = millis();
  if (now - motor_check_timer > motor_check_interval){
    if ((motor_rpm < xv_config.rpm_min or motor_rpm > xv_config.rpm_max) and pwm_val > 1000) {
      rpm_err++;
    }
    else {
      rpm_err = 0;
    }
    if (rpm_err > rpm_err_thresh) {
      motorOff(); 
      ledState = LOW;
      digitalWrite(ledPin, ledState);
    }
    motor_check_timer = millis();
  }
}

void hideRaw() {
  xv_config.raw_data = false;
  Serial.println(F(" "));
  Serial.print(F("Raw lidar data disabled"));
  Serial.println(F(" "));
}

void showRaw() {
  xv_config.raw_data = true;
  hideDist();
  hideRPM();
  Serial.println(F(" "));
  Serial.print(F("Lidar data enabled"));
  Serial.println(F(" "));
}

void setRPM() {
  double sVal = 0.0;
  char *arg;
  boolean syntax_error = false;

  arg = sCmd.next();
  if (arg != NULL) {
    sVal = atof(arg);    // Converts a char string to a float
    if (sVal < xv_config.rpm_min) {
      sVal = xv_config.rpm_min;
      Serial.println(F(" "));
      Serial.print(F("RPM too low. Setting to minimum "));
      Serial.print(xv_config.rpm_min);
      Serial.println(F(" "));
    }
    if (sVal > xv_config.rpm_max) {
      sVal = xv_config.rpm_max;
      Serial.println(F(" "));
      Serial.print(F("RPM too high. Setting to maximum "));
      Serial.print(xv_config.rpm_max);
      Serial.println(F(" "));
    }
  }
  else {
    syntax_error = true;
  }

  arg = sCmd.next();
  if (arg != NULL) {
    syntax_error = true;
  }

  if (syntax_error) {
    Serial.println(F(" "));
    Serial.print(F("Incorrect syntax.  Example: SetRPM 300")); 
    Serial.println(F(" "));
  }
  else {
    Serial.print(F("Old RPM setpoint:"));
    Serial.println(xv_config.rpm_setpoint);
    xv_config.rpm_setpoint = sVal;
    //Serial.println(F(" "));
    Serial.print(F("New RPM setpoint: "));
    Serial.println(sVal);
    Serial.println(F(" "));
  }
}

void setKp() {
  double sVal = 0.0;
  char *arg;
  boolean syntax_error = false;

  arg = sCmd.next();
  if (arg != NULL) {
    sVal = atof(arg);    // Converts a char string to a float
  }
  else {
    syntax_error = true;
  }

  arg = sCmd.next();
  if (arg != NULL) {
    syntax_error = true;
  }

  if (syntax_error) {
    Serial.println(F(" "));
    Serial.print(F("Incorrect syntax.  Example: SetKp 1.0")); 
    Serial.println(F(" "));
  }
  else {
    Serial.println(F(" "));
    Serial.print(F("Setting Kp to: "));
    Serial.println(sVal);
    Serial.println(F(" "));
    xv_config.Kp = sVal;
    rpmPID.SetTunings(xv_config.Kp, xv_config.Ki, xv_config.Kd);
  }
}

void setKi() {
  double sVal = 0.0;
  char *arg;
  boolean syntax_error = false;

  arg = sCmd.next();
  if (arg != NULL) {
    sVal = atof(arg);    // Converts a char string to a float
  }
  else {
    syntax_error = true;
  }

  arg = sCmd.next();
  if (arg != NULL) {
    syntax_error = true;
  }

  if (syntax_error) {
    Serial.println(F(" "));
    Serial.print(F("Incorrect syntax.  Example: SetKi 0.5")); 
    Serial.println(F(" "));
  }
  else {
    Serial.println(F(" "));
    Serial.print(F("Setting Ki to: "));
    Serial.println(sVal);    
    Serial.println(F(" "));
    xv_config.Ki = sVal;
    rpmPID.SetTunings(xv_config.Kp, xv_config.Ki, xv_config.Kd);
  }
}

void setKd() {
  double sVal = 0.0;
  char *arg;
  boolean syntax_error = false;

  arg = sCmd.next();
  if (arg != NULL) {
    sVal = atof(arg);    // Converts a char string to a float
  }
  else {
    syntax_error = true;
  }

  arg = sCmd.next();
  if (arg != NULL) {
    syntax_error = true;
  }

  if (syntax_error) {
    Serial.println(F(" "));
    Serial.print(F("Incorrect syntax.  Example: SetKd 0.001")); 
    Serial.println(F(" "));
  }
  else {
    Serial.println(F(" "));
    Serial.print(F("Setting Kd to: "));
    Serial.println(sVal);    
    Serial.println(F(" "));
    xv_config.Kd = sVal;
    rpmPID.SetTunings(xv_config.Kp, xv_config.Ki, xv_config.Kd);
  }
}

void setSampleTime() {
  double sVal = 0.0;
  char *arg;
  boolean syntax_error = false;

  arg = sCmd.next();
  if (arg != NULL) {
    sVal = atoi(arg);    // Converts a char string to an integer
  }
  else {
    syntax_error = true;
  }

  arg = sCmd.next();
  if (arg != NULL) {
    syntax_error = true;
  }

  if (syntax_error) {
    Serial.println(F(" "));
    Serial.print(F("Incorrect syntax.  Example: SetSampleTime 20")); 
    Serial.println(F(" "));
  }
  else {
    Serial.println(F(" "));
    Serial.print(F("Setting Sample time to: "));
    Serial.println(sVal);    
    Serial.println(F(" "));
    xv_config.sample_time = sVal;
    rpmPID.SetSampleTime(xv_config.sample_time);
  }
}

void help() {
  if (xv_config.raw_data == true) {
    hideRaw();
  }
  Serial.println(F(" "));
  Serial.println(F(" "));

  Serial.print(F("XV Lidar Controller Firmware Version "));
  Serial.println(xv_config.version);
  Serial.print(F("GetSurreal.com *"));

  Serial.println(F(" "));
  Serial.println(F(" "));

  Serial.println(F("List of available commands (case sensitive)"));
  Serial.println(F("  ShowConfig    - Show the running configuration"));
  Serial.println(F("  SaveConfig    - Save the running configuration to EEPROM"));
  Serial.println(F("  ResetConfig   - Restore the original configuration"));
  Serial.println(F("  SetRPM        - Set the desired rotation speed (min: 200, max: 300)"));
  Serial.println(F("  SetKp         - Set the proportional gain"));
  Serial.println(F("  SetKi         - Set the integral gain"));
  Serial.println(F("  SetKd         - Set the derivative gain"));
  Serial.println(F("  SetSampleTime - Set the frequency the PID is calculated (ms)"));
  Serial.println(F("  ShowRPM       - Show the rotation speed"));
  Serial.println(F("  HideRPM       - Hide the rotation speed"));
  Serial.println(F("  ShowDist      - Show the distance data"));
  Serial.println(F("  HideDist      - Hide the distance data"));
  Serial.println(F("  ShowAngle     - Show distance data for a specific angle (0 - 359 or 360 for all)"));
  Serial.println(F("  HideAngle     - Hide distance data for all angles"));
  Serial.println(F("  MotorOff      - Stop spinning the lidar"));
  Serial.println(F("  MotorOn       - Enable spinning of the lidar"));
  Serial.println(F("  HideRaw       - Stop outputting the raw data from the lidar"));
  Serial.println(F("  ShowRaw       - Enable the output of the raw lidar data"));

  Serial.println(F(" "));
  Serial.println(F(" "));
}

void showConfig() {
  if (xv_config.raw_data == true) {
    hideRaw();
  }
  Serial.println(F(" "));
  Serial.println(F(" "));

  Serial.print(F("XV Lidar Controller Firmware Version "));
  Serial.println(xv_config.version);
  Serial.print(F("GetSurreal.com"));

  Serial.println(F(" "));
  Serial.println(F(" "));

  Serial.print(F("PWM pin: "));
  Serial.println(xv_config.motor_pwm_pin);

  Serial.print(F("Target RPM: "));
  Serial.println(xv_config.rpm_setpoint);

  Serial.print(F("Max PWM: "));
  Serial.println(xv_config.pwm_max);
  Serial.print(F("Min PWM: "));
  Serial.println(xv_config.pwm_min);

  Serial.print(F("PID Kp: "));
  Serial.println(xv_config.Kp);
  Serial.print(F("PID Ki: "));
  Serial.println(xv_config.Ki);
  Serial.print(F("PID Kd: "));
  Serial.println(xv_config.Kd);
  Serial.print(F("SampleTime: "));
  Serial.println(xv_config.sample_time);

  Serial.print(F("Motor Enable: "));
  Serial.println(xv_config.motor_enable);
  Serial.print(F("Show Raw Data: "));
  Serial.println(xv_config.raw_data);
  Serial.print(F("Show Dist Data: "));
  Serial.println(xv_config.show_dist);
  Serial.print(F("Show RPM Data: "));
  Serial.println(xv_config.show_rpm);
  Serial.print(F("Show Angle: "));
  Serial.println(xv_config.show_angle);

  Serial.println(F(" "));
  Serial.println(F(" "));

}

void saveConfig() {
  EEPROM_writeAnything(0, xv_config);
  Serial.print(F("Config Saved."));
}

