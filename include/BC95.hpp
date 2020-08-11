#ifndef _BC95_H
#define _BC95_H
#include <Arduino.h>

#define bc95serial Serial2
#define RESET_PIN 25
#define RX_PIN 34
#define TX_PIN 23

#define DEBUG_MODEM

int readResponseBC(HardwareSerial *port, char *buff, int b_size, uint32_t timeout);
bool assertResponseBC(const char *expected, char *received, int bytesRead);
bool sendAndReadOkResponseBC(HardwareSerial *port, const char *command, char* buffer, int bufferSize);
void initModem();
void resetModem();
bool configModem();
bool networkReady();
bool attachNetwork();
bool networkAttached();
bool connectModem(char *ip, int port);
void disconnectModem();
int postPage(char *domainBuffer, int thisPort, char *page, char *thisData);

#endif