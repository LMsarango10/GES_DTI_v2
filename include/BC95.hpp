#ifndef _BC95_H
#define _BC95_H
#include <Arduino.h>

//#define bc95serial Serial1
//#define RESET_PIN 25
#define RX_PIN 34
#define TX_PIN 0

#define NBSENDTIMEOUT 25000

#define VERSION "vodafone"

#ifdef VERSION == "vodafone"
#define VODAFONE_VERSION
#else
#define AUTO_VERSION
#endif

#define DEBUG_MODEM

int readResponseBC(HardwareSerial *port, char *buff, int b_size, uint32_t timeout);
bool assertResponseBC(const char *expected, char *received, int bytesRead);
bool sendAndReadOkResponseBC(HardwareSerial *port, const char *command, char* buffer, int bufferSize);
void initModem();
void resetModem();
bool configModem();
bool networkReady();
void getCsq();
bool attachNetwork();
bool networkAttached();
bool connectModem(char *ip, int port);
void disconnectModem();
int connectMqtt(char *url, int port, char *password, char *clientId);
int subscribeMqtt(char *topic, int qos);
int unsubscribeMqtt(char *topic, int qos);
int checkSubscriptionMqtt(char *message);
int publishMqtt(char *topic, char *message, int qos);
int disconnectMqtt();
int postPage(char *domainBuffer, int thisPort, char *page, char *thisData, char* identityKey);

#endif