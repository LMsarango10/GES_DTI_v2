#ifndef _BLESCAN_H
#define _BLESCAN_H

#include "globals.h"
#include "macsniff.h"

// Bluetooth specific includes
#include "soc/timer_group_struct.h"
#include "soc/timer_group_reg.h"

#define RX_BLE 35
#define TX_BLE 14
#define EN_BLE 12
#define BLESerial Serial2
#define RX_BT 35
#define TX_BT 14
#define EN_BT 2
#define BTSerial Serial2

#define BLEBTMUX_A 13
#define BLEBTMUX_B 15

#define BTLE_SCAN_TIME 20

void start_BLEscan(void);
void stop_BLEscan(void);
bool initBLE(void);
bool initBT(long baud);
void btHandler(void *pvParameters);
void BLECycle(void);
void BTCycle(void);

#endif