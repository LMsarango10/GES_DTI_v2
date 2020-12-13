#include <BC95.hpp>

// SoftwareSerial bc95serial(8, 9);
HardwareSerial bc95serial(1);
char globalBuff[4096];
char TAG[] = "BC95";

void cleanbuffer() {
  while (bc95serial.available())
    bc95serial.read();
}

int readResponseBC(HardwareSerial *port, char *buff, int b_size,
                   uint32_t timeout = 500) {
  port->setTimeout(timeout);
  buff[0] = 0;
  int bytesRead = port->readBytes(buff, b_size);
  if(bytesRead > 0)
  {
    buff[bytesRead] = 0;
    ESP_LOGI(TAG, "%d bytes read", bytesRead);
    ESP_LOGI(TAG, "Message: %s", buff);
    return bytesRead;
  }
  else if (bytesRead < 0) return -1;
  return 0;
}

int readResponseWithStop(HardwareSerial *port, char *buff, int b_size, char* stopWord, unsigned long timeout) {
  buff[0] = 0;
  int p = 0;

  unsigned long startTime = millis();

  while(millis() - startTime < timeout) {
    if(port->available()) {
      buff[p++] = port->read();
      if(p >= b_size) {
        ESP_LOGE(TAG, "Buffer too small to receive message");
        return -1;
      }
    }
    if (strstr(buff, stopWord)) {
      ESP_LOGI(TAG, "Stopword %s found", stopWord);
      ESP_LOGI(TAG, "%d bytes read", p);
      ESP_LOGI(TAG, "Message: %s", buff);
      return p;
    }
  }
  ESP_LOGE(TAG, "Timeout waiting for stopword");
  return -2;
}

bool assertResponseBC(const char *expected, char *received, int bytesRead) {
  if (bytesRead <= 0)
    return false;
  return strstr(received, expected) != nullptr;
}

bool sendAndReadOkResponseBC(HardwareSerial *port, const char *command,
                             char *buffer, int bufferSize) {
  // ESP_LOGV(TAG, "Command: %s", command);
#ifdef DEBUG_MODEM
  ESP_LOGD(TAG, "Command: %s", command);
#endif
  port->println(command);
  int bytesRead = readResponseBC(port, buffer, bufferSize);
  return assertResponseBC("OK\r", buffer, bytesRead);
}

void initModem() {
  bc95serial.begin(9600, SERIAL_8N1, RX_PIN, TX_PIN);
  //pinMode(RESET_PIN, OUTPUT);
  // digitalWrite(RESET_PIN, HIGH);

  /*digitalWrite(RESET_PIN, HIGH);
  delay(1000);
  digitalWrite(RESET_PIN, LOW);
  delay(5000);*/
#ifdef DEBUG_MODEM
  // ESP_LOGD(TAG, bc95serial.readString().c_str());
#endif
}

bool networkReady() {
  bc95serial.println("AT+CEREG?");
  int bytesRead = readResponseBC(&bc95serial, globalBuff, sizeof(globalBuff));
  if (assertResponseBC("CEREG:0,1", globalBuff, bytesRead))
    return true;
  else
    return false;
}

void getCsq() {
  bc95serial.println("AT+CSQ");
  int bytesRead = readResponseBC(&bc95serial, globalBuff, sizeof(globalBuff));
}

void resetModem() {
  bc95serial.println("AT+NRB");
  delay(2000);
  int bytesRead = readResponseBC(&bc95serial, globalBuff, sizeof(globalBuff));
  assertResponseBC("REBOOTING", globalBuff, bytesRead);
  delay(5000);
  while (bc95serial.available() > 0)
    bc95serial.read();
  sendAndReadOkResponseBC(&bc95serial, "AT", globalBuff, sizeof(globalBuff));
  // sendAndReadOkResponseBC(&bc95serial, "AT+CEREG=0");
}

bool configModem() {
  return sendAndReadOkResponseBC(&bc95serial, "AT+QREGSWT=2", globalBuff,
                                 sizeof(globalBuff)) &&
         sendAndReadOkResponseBC(&bc95serial,
                                 "AT+CGDCONT=1,\"IP\",\"m2m.movistar.es\"",
                                 globalBuff, sizeof(globalBuff)) &&
         sendAndReadOkResponseBC(&bc95serial, "AT+CFUN=1", globalBuff,
                                 sizeof(globalBuff)) &&
         sendAndReadOkResponseBC(&bc95serial, "AT+COPS=0", globalBuff,
                                 sizeof(globalBuff));
  // sendAndReadOkResponseBC(&bc95serial, "AT+NCONFIG=AUTOCONNECT,TRUE");
}

bool attachNetwork()

{
  return sendAndReadOkResponseBC(&bc95serial, "AT+CGATT=1", globalBuff,
                                 sizeof(globalBuff)) &&
         sendAndReadOkResponseBC(&bc95serial, "AT+CGATT?", globalBuff,
                                 sizeof(globalBuff));
}

bool networkAttached() {
  return sendAndReadOkResponseBC(&bc95serial, "AT+CGPADDR", globalBuff,
                                 sizeof(globalBuff));
}

bool connectModem(char *ip, int port) {
  bool r1 = sendAndReadOkResponseBC(&bc95serial, "AT+CGPADDR", globalBuff,
                                    sizeof(globalBuff));
  delay(100);
  bool r2 = sendAndReadOkResponseBC(&bc95serial, "AT+NSOCR=STREAM,6,0,1",
                                    globalBuff, sizeof(globalBuff));
  delay(100);
  sprintf(globalBuff, "AT+NSOCO=1,%s,%d", ip, port);
  bool r3 = sendAndReadOkResponseBC(&bc95serial, globalBuff, globalBuff,
                                    sizeof(globalBuff));
  delay(100);
  return r1 && r2 && r3;
}

void disconnectModem() {
  sendAndReadOkResponseBC(&bc95serial, "AT+NSOCL=1", globalBuff,
                          sizeof(globalBuff));
}

bool receiveData(char *data, int bytesRead, int bufferLen) {
  bc95serial.print("AT+NSORF=1,");
  bc95serial.println(bytesRead, DEC);
  int responseBytes = readResponseBC(&bc95serial, data, bufferLen);
  if (!assertResponseBC("OK", data, responseBytes)) {
    return false;
  }

  char *socketPtr = strtok(data, ",");
  char *ipPtr = strtok(NULL, ",");
  char *portPtr = strtok(NULL, ",");
  char *lenPtr = strtok(NULL, ",");
  char *dataPtr = strtok(NULL, ",");

  int dataLen = strtoul(lenPtr, NULL, 10);
  int strLen = strlen(dataPtr);

  if (dataLen != bytesRead || strLen != 2 * bytesRead) {
    ESP_LOGE(TAG, "Size mismatch");
    return false;
  }

  char *tempBuff = new char[strLen + 1];
  memcpy(tempBuff, dataPtr, strLen + 1);

  for (int i = 0; i < strLen; i += 2) {
    char tmp[3];
    memcpy(tmp, tempBuff + i, 2);
    tmp[2] = 0;
    data[i / 2] = strtoul(tmp, NULL, 16);
  }
  data[bytesRead] = 0;
  delete tempBuff;
  return true;
}
enum sendStatus { INIT, DATAOK, SENTOK, RECOK };

int sendData(char *data, int datalen, char *responseBuff,
             int responseBuffSize) // returns bytes read
{
#define sendSerial bc95serial
#ifdef DEBUG_MODEM
  ESP_LOGD(TAG, "Data string: %s", data);
#endif
  sendSerial.print("AT+NSOSD=1,");
  sendSerial.print(datalen);
  sendSerial.print(",");
  for (int i = 0; i < datalen; i++) {
    char b2[3];
    sprintf(b2, "%02X", data[i]);
    sendSerial.print(b2);
  }
  sendSerial.println(",0x100,101");
  bool timeout = false;
  unsigned long startTime = millis();
  int buffPtr = 0;
  responseBuff[buffPtr] = 0;
  sendStatus status = INIT;
  while (!timeout) {
    timeout = (millis() - startTime > NBSENDTIMEOUT);
    delay(100);
    while (sendSerial.available()) {
      char value = sendSerial.read();
      if (value != '\r' && value != '\n') {
        responseBuff[buffPtr++] = value;
        if (buffPtr == responseBuffSize) {
          return -10;
        }
        responseBuff[buffPtr] = 0;
      }
    }
    char expected[18];
    switch (status) {
    case INIT: {
      sprintf(expected, "1,%d", datalen);
      if (strlen(responseBuff) == strlen(expected)) {
        char *val = strstr(responseBuff, expected);
        if (val != nullptr) {
          buffPtr = 0;
          responseBuff[buffPtr] = 0;
          status = DATAOK;
          continue;
        }
        return -1;
      }
      break;
    }
    case DATAOK: {
      sprintf(expected, "OK");
      if (strlen(responseBuff) == strlen(expected)) {
        char *val = strstr(responseBuff, expected);
        if (val != nullptr) {
          buffPtr = 0;
          responseBuff[buffPtr] = 0;
          status = SENTOK;
          continue;
        }
        return -2;
      }
      break;
    }
    case SENTOK: {
      sprintf(expected, "+NSOSTR:1,101,1");
      if (strlen(responseBuff) == strlen(expected)) {
        char *val = strstr(responseBuff, expected);
        if (val != nullptr) {
          buffPtr = 0;
          responseBuff[buffPtr] = 0;
          status = RECOK;
          continue;
        }
        return -3;
      }
      break;
    }
    case RECOK: {
      sprintf(expected, "+NSONMI:1,");
      if (strlen(responseBuff) == strlen(expected)) {
        char *val = strstr(responseBuff, expected);
        if (val != nullptr) {
          delay(100);
          while (bc95serial.available()) {
            responseBuff[buffPtr++] = bc95serial.read();
            if (buffPtr == responseBuffSize) {
              return -10;
            }
            responseBuff[buffPtr] = 0;
          }
          char *pch = val + sizeof("+NSONMI:1,") - 1;
          int bytesReceived = strtoul(pch, NULL, 10);
          if (bytesReceived == 0)
            return -5;
          return bytesReceived;
        }
        return -4;
      }
      break;
    }
    }
  }
  return 0;
}

int parseResponse(char *buff, int bytesReceived, int *responseCode) {
  char *httpCodeLine = strtok(buff, "\r\n");

  char *ptr;
  int bodyLen = 0;
  do {
    ptr = strtok(NULL, "\r\n");
    if (ptr == NULL) {
      ESP_LOGD(TAG, "Empty body");
      break;
    } else if (strlen(ptr) == 0) {
      ptr = strtok(NULL, "\r\n");
      ESP_LOGD(TAG, "Body: %s", ptr);
      bodyLen = strlen(ptr);
      break;
    }
  } while (ptr != NULL);

  char *responseCodeStr = strtok(httpCodeLine, " ");
  if (responseCodeStr == NULL) {
    return -1;
  }
  responseCodeStr = strtok(NULL, " ");
  if (responseCodeStr == NULL) {
    return -2;
  }
  *responseCode = strtoul(responseCodeStr, NULL, 10);
  if (bodyLen > 0)
    memcpy(buff, ptr, bodyLen);
  else
    buff[0] = 0;
  return bodyLen;
}

int postPage(char *domainBuffer, int thisPort, char *page, char *thisData, char* identityKey) {
  char outBuf[256];

  ESP_LOGI(TAG, "connecting...");

  if (connectModem(domainBuffer, thisPort)) {
    ESP_LOGI(TAG, "connected");
    // send the header
    globalBuff[0] = 0;
    sprintf(outBuf, "PUT %s HTTP/1.1\r\n", page);
    strcat(globalBuff, outBuf);
    sprintf(outBuf, "Host: %s\r\n", domainBuffer);
    strcat(globalBuff, outBuf);
    sprintf(outBuf, "Connection: close\r\nContent-Type: application/json\r\n");
    strcat(globalBuff, outBuf);
    sprintf(outBuf, "IDENTITY_KEY: %s\r\n", identityKey);
    strcat(globalBuff, outBuf);
    sprintf(outBuf, "Content-Length: %u\r\n", strlen(thisData));
    strcat(globalBuff, outBuf);
    strcat(globalBuff, "\r\n");

    // send the body (variables)
    strcat(globalBuff, thisData);
    int responseCode = 0;
    int bytesReceived = sendData(globalBuff, strlen(globalBuff), globalBuff,
                                 sizeof(globalBuff));
    if (bytesReceived < 0) {
      cleanbuffer();
      ESP_LOGE(TAG, "failed sending data with error code: %d", bytesReceived);
      responseCode = bytesReceived;
    } else if (bytesReceived > 0) {
      if (receiveData(globalBuff, bytesReceived, sizeof(globalBuff))) {
        ESP_LOGD(TAG, "Received %d bytes", bytesReceived);
        ESP_LOGD(TAG, "%s", globalBuff);
        int bodyBytes = parseResponse(globalBuff, bytesReceived, &responseCode);
        if (bodyBytes < 0) {
          ESP_LOGE(TAG, "Error: %d while parsing response", bodyBytes);
          responseCode = -12;
        } else {
          ESP_LOGD(TAG, "Response Code: %d", responseCode);
          ESP_LOGD(TAG, "Body: %s", globalBuff);
        }
      } else {
        cleanbuffer();
        ESP_LOGE(TAG, "Failed retrieving data");
        responseCode = -11;
      }
    } else {
      ESP_LOGE(TAG, "Timeout");
      responseCode = 0;
    }

    ESP_LOGI(TAG, "disconnecting.");
    disconnectModem();
    ESP_LOGI(TAG, "Return code %d", responseCode);
    return responseCode;
  } else {
    ESP_LOGE(TAG, "failed connecting");
    return -1;
  }
}

int connectMqtt(char *url, int port, char *password, char *clientId)
{
  ESP_LOGI(TAG, "SENDING TO Modem: AT+QMTOPEN=0,\"%s\",%d",url, port);

  bc95serial.print("AT+QMTOPEN=0,\"");
  bc95serial.print(url);
  bc95serial.print("\",");
  bc95serial.println(port);
  char data[64];
  int responseBytes = readResponseBC(&bc95serial, data, 64, 2000);
  if (!assertResponseBC("+QMTOPEN: 0,0", data, responseBytes)) {
    return -1;
  }

  ESP_LOGI(TAG, "SENDING TO Modem: AT+QMTCONN=0,\"%s\",\"gesinen\",\"%s\"",clientId, password);

  bc95serial.print("AT+QMTCONN=0,\"");
  bc95serial.print(clientId);
  bc95serial.print("\",\"gesinen\",\"");
  bc95serial.print(password);
  bc95serial.println("\"");

  responseBytes = readResponseBC(&bc95serial, data, 64, 2000);
  if (!assertResponseBC("+QMTCONN: 0,0,0", data, responseBytes)) {
    return -2;
  }
  return 0;
}

int subscribeMqtt(char *topic, int qos)
{

}
int unsubscribeMqtt(char *topic, int qos)
{

}
int checkSubscriptionMqtt(char *message)
{

}
int publishMqtt(char *topic, char *message, int qos)
{
  ESP_LOGI(TAG, "SENDING TO Modem: AT+QMTPUB=0,0,0,0,\"%s\"",topic);

  bc95serial.print("AT+QMTPUB=0,0,0,0,\"");
  bc95serial.print(topic);
  bc95serial.println("\"");

  char data[512];
  int responseBytes = readResponseBC(&bc95serial, data, 512);

  if (!assertResponseBC(">", data, responseBytes)) {
    bc95serial.write(26);
    return -1;
  }

  bc95serial.print(message);
  bc95serial.write(26);

  responseBytes = readResponseWithStop(&bc95serial, data, 512, "+QMTPUB: 0,0,0", 5000);
  while(bc95serial.available()) {
    bc95serial.read();
  }
  if(responseBytes < 0)
    return responseBytes;
  return 0;
}
int disconnectMqtt()
{
  ESP_LOGI(TAG, "SENDING TO Modem: AT+QMTDISC=0");
  bc95serial.println("AT+QMTDISC=0");
  //ESP_LOGI(TAG, "SENDING TO Modem: AT+QMTCLOSE=0");
  //bc95serial.println("AT+QMTCLOSE=0");
  char data[64];
  int responseBytes = readResponseBC(&bc95serial, data, 64);
  if (!assertResponseBC("OK", data, responseBytes)) {
    return -1;
  }
  return 0;
}