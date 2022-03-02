/*

//////////////////////// ESP32-Paxcounter \\\\\\\\\\\\\\\\\\\\\\\\\\

Copyright  2018 Oliver Brandmueller <ob@sysadm.in>
Copyright  2018 Klaus Wilting <verkehrsrot@arcor.de>

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

NOTE:
Parts of the source files in this repository are made available under different
licenses. Refer to LICENSE.txt file in repository for more details.

//////////////////////// ESP32-Paxcounter \\\\\\\\\\\\\\\\\\\\\\\\\\

// Tasks and timers:

Task          Core  Prio  Purpose
-------------------------------------------------------------------------------
ledloop       0     3     blinks LEDs
spiloop       0     2     reads/writes data on spi interface
IDLE          0     0     ESP32 arduino scheduler -> runs wifi sniffer

lmictask      1     2     MCCI LMiC LORAWAN stack
clockloop     1     4     generates realtime telegrams for external clock
timesync_req  1     3     processes realtime time sync requests
irqhandler    1     1     cyclic tasks (i.e. displayrefresh) triggered by timers
gpsloop       1     1     reads data from GPS via serial or i2c
lorasendtask  1     1     feeds data from lora sendqueue to lmcic
IDLE          1     0     ESP32 arduino scheduler -> runs wifi channel rotator

Low priority numbers denote low priority tasks.

NOTE: Changing any timings will have impact on time accuracy of whole code.
So don't do it if you do not own a digital oscilloscope.

// ESP32 hardware timers
-------------------------------------------------------------------------------
0	displayIRQ -> display refresh -> 40ms (DISPLAYREFRESH_MS)
1 ppsIRQ -> pps clock irq -> 1sec
3	MatrixDisplayIRQ -> matrix mux cycle -> 0,5ms (MATRIX_DISPLAY_SCAN_US)


// Interrupt routines
-------------------------------------------------------------------------------

fired by hardware
DisplayIRQ      -> esp32 timer 0  -> irqHandlerTask (Core 1)
CLOCKIRQ        -> esp32 timer 1  -> ClockTask (Core 1)
ButtonIRQ       -> external gpio  -> irqHandlerTask (Core 1)
PMUIRQ          -> PMU chip gpio  -> irqHandlerTask (Core 1)

fired by software (Ticker.h)
TIMESYNC_IRQ    -> timeSync()     -> irqHandlerTask (Core 1)
CYCLIC_IRQ      -> housekeeping() -> irqHandlerTask (Core 1)
SENDCYCLE_IRQ   -> sendcycle()    -> irqHandlerTask (Core 1)
BME_IRQ         -> bmecycle()     -> irqHandlerTask (Core 1)


// External RTC timer (if present)
-------------------------------------------------------------------------------
triggers pps 1 sec impulse

*/

// Basic Config
#include "main.h"

configData_t cfg; // struct holds current device configuration
char lmic_event_msg[LMIC_EVENTMSG_LEN]; // display buffer for LMIC event message
uint8_t volatile channel = 0;           // channel rotation counter
uint16_t volatile macs_total = 0, macs_wifi = 0, macs_ble = 0, macs_bt = 0,
                  batt_voltage = 0; // globals for display

hw_timer_t *ppsIRQ = NULL, *displayIRQ = NULL, *matrixDisplayIRQ = NULL;

TaskHandle_t irqHandlerTask = NULL, ClockTask = NULL;
SemaphoreHandle_t I2Caccess;
bool volatile TimePulseTick = false;
time_t userUTCTime = 0;
timesource_t timeSource = _unsynced;

// container holding unique MAC address hashes with Memory Alloctor using PSRAM,
// if present
std::set<uint32_t, std::less<uint32_t>, Mallocator<uint32_t>> macs_list_wifi;
std::set<uint32_t, std::less<uint32_t>, Mallocator<uint32_t>> macs_list_ble;
std::set<uint32_t, std::less<uint32_t>, Mallocator<uint32_t>> macs_list_bt;

// initialize payload encoder
PayloadConvert payload(PAYLOAD_BUFFER_SIZE);

// set Time Zone for user setting from paxcounter.conf
TimeChangeRule myDST = DAYLIGHT_TIME;
TimeChangeRule mySTD = STANDARD_TIME;
Timezone myTZ(myDST, mySTD);
//#include "SoftwareSerial.h"

// local Tag for logging
static const char TAG[] = __FILE__;
//SoftwareSerial Bluetooth(RX_BT,TX_BT);
//https://www.youtube.com/watch?v=90JYo5e9eIk&ab_channel=RoelVandePaar
void setup() {
  
  pinMode(EN_BT, OUTPUT);
  digitalWrite(EN_BT,LOW);
  Serial.begin(115200);
  Serial.println("ready");
  initBT();
  BTCycle();

} // setup()

char c=' ';
void loop() {// vTaskDelete(NULL);
 if(BTSerial.available())
  {
    c=BTSerial.read();
    Serial.write(c);
  }
  if(Serial.available())
  {
    c=Serial.read();
    BTSerial.write(c);
  } }
