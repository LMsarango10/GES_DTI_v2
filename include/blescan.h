#ifndef _BLESCAN_H
#define _BLESCAN_H

#include "globals.h"
#include "macsniff.h"

// Bluetooth specific includes
#include "soc/timer_group_struct.h"
#include "soc/timer_group_reg.h"

#define BT_OLD_MODULE 1// para usar el viejo modulo
// #define BT_OLD_MODULE2 1 // para usar versiones muy viejas que no inicializan BT

#ifdef BT_OLD_MODULE
#define BT_BAUD 38400
#else
#define BT_BAUD 9600
#endif
#define BLE_BAUD 9600

// #define LEGACY_MODULE Descomentar para usar placas viejas

#ifndef LEGACY_MODULE

#define RX_BLE 36
#define TX_BLE 4
#define EN_BLE 12
#define BLESerial Serial2
#define RX_BT 36 //
#define TX_BT 4 //
#define EN_BT 12 //
#define BTSerial Serial2

#else

#define RX_BLE 35  // para modulo viejo usar 35 nuevo 36
#define TX_BLE 14  // para modulo viejo usar 14 nuevo 4
#define EN_BLE 12 // para modulo viejo usar 12 nuevo 12
#define BLESerial Serial1 // para modelo viejo usar Serial1 nuevo Serial2
#define RX_BT 36 // para modulo viejo usar 15
#define TX_BT 4 // para modulo viejo usar 13
#define EN_BT 12 // para modulo viejo usar 02
#define BTSerial Serial2

#endif

#define BLEBTMUX_A 25
//#define BLEBTMUX_B 2

#define BTLE_SCAN_TIME 20

void start_BLEscan(void);
void stop_BLEscan(void);
bool initBLE(void);
bool initBT(long baud);
void btHandler(void *pvParameters);
void BLECycle(void);
void BTCycle(void);

#endif