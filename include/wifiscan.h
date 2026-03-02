#ifndef _WIFISCAN_H
#define _WIFISCAN_H

// ESP32 Functions
#include <esp_wifi.h>

// Hash function for scrambling MAC addresses
#include "hash.h"

void wifi_sniffer_init(void);
void switch_wifi_sniffer (uint8_t state);
void IRAM_ATTR wifi_sniffer_packet_handler(void *buff, wifi_promiscuous_pkt_type_t type);
void switchWifiChannel(TimerHandle_t xTimer);

// >>> CAMBIO: Estado operativo del radio WiFi para health check
// true  = wifi_sniffer_init() completó OK, radio funcionando
// false = no inicializó o se apagó con switch_wifi_sniffer(0)
extern bool wifi_radio_ok;

#endif