#ifndef _BC95_H
#define _BC95_H
#include <Arduino.h>
#include "globals.h"
#include "lorawan.h"

//#define bc95serial Serial1
//#define RESET_PIN 25
#define RX_PIN 34
#define TX_PIN 0

#define NBSENDTIMEOUT 25000
#define HTTP_READ_TIMEOUT 10000
#define HTTP_SOCKET_TIMEOUT 2000

#define APN "lpwa.vodafone.iot"

#define DEBUG_MODEM

int readResponseBC(HardwareSerial *port, char *buff, int b_size, uint32_t timeout);
bool assertResponseBC(const char *expected, char *received, int bytesRead);
bool sendAndReadOkResponseBC(HardwareSerial *port, const char *command, char* buffer, int bufferSize, uint32_t timeout);
void initModem();
void resetModem();
bool configModem();
bool preConfigModem();
bool networkReady();
void getCsq();
bool attachNetwork();
bool networkAttached();
bool connectModem(char *ip, int port);
void disconnectModem();
int connectMqtt(char *url, int port, char *username, char *password, char *clientId);
bool subscribeMqtt(char *topic);
bool checkMqttConnection();
int readMqttSubData(char* buffer, int bufferLen);
bool dataAvailable();
int unsubscribeMqtt(char *topic, int qos);
int checkSubscriptionMqtt(char *message);
int publishMqtt(char *topic, char *message, int qos);
int disconnectMqtt();
int postPage(char *domainBuffer, int thisPort, char *page, char *thisData, char* identityKey);
int getData(char *ip, int port, char *page, char *responseBuffer, int responseBufferSize, int *responseSizePtr);

#endif