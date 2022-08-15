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
  int bytesRead = port->readBytes(buff, b_size - 1);
  if (bytesRead > 0) {
    buff[bytesRead] = 0;
    ESP_LOGI(TAG, "%d bytes read", bytesRead);
    ESP_LOGI(TAG, "Message: %s", buff);
    return bytesRead;
  } else if (bytesRead < 0)
    return -1;
  return 0;
}

int readResponseWithStop(HardwareSerial *port, char *buff, int b_size,
                         char *stopWord, unsigned long timeout) {
  buff[0] = 0;
  int p = 0;

  unsigned long startTime = millis();

  while (millis() - startTime < timeout) {
    if (port->available()) {
      buff[p++] = port->read();
      if (p >= b_size) {
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
  bc95serial.setRxBufferSize(4096);
  // pinMode(RESET_PIN, OUTPUT);
  //  digitalWrite(RESET_PIN, HIGH);

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
  // next conditional allows for network roaming
  else if (assertResponseBC("CEREG:0,5", globalBuff, bytesRead))
    return true;
  else
    return false;
}

void getCsq() {
  bc95serial.println("AT+CSQ");
  int bytesRead = readResponseBC(&bc95serial, globalBuff, sizeof(globalBuff));
}

void resetModem() {
  ESP_LOGI(TAG, "Reset NBIOT modem");
  bc95serial.println("AT+NRB");
  delay(2000);
  int bytesRead = readResponseBC(&bc95serial, globalBuff, sizeof(globalBuff));
  assertResponseBC("REBOOTING", globalBuff, bytesRead);
  delay(7000);
  while (bc95serial.available() > 0)
    bc95serial.read();
  sendAndReadOkResponseBC(&bc95serial, "AT", globalBuff, sizeof(globalBuff));
}

bool configModem() {
  ESP_LOGI(TAG, "Config NBIOT modem");
  return sendAndReadOkResponseBC(&bc95serial, "AT+CEREG=0", globalBuff,
                                 sizeof(globalBuff)) &&
         sendAndReadOkResponseBC(&bc95serial, "AT+NSONMI=3", globalBuff,
                                 sizeof(globalBuff)) &&
#ifdef VODAFONE_VERSION
         sendAndReadOkResponseBC(&bc95serial, "AT+CSCON=0", globalBuff,
                                 sizeof(globalBuff)) &&
         sendAndReadOkResponseBC(&bc95serial, "AT+NPSMR=0", globalBuff,
                                 sizeof(globalBuff)) &&
         sendAndReadOkResponseBC(&bc95serial, "AT+CFUN=1", globalBuff,
                                 sizeof(globalBuff)) &&
         sendAndReadOkResponseBC(&bc95serial, "AT+QREGSWT=1", globalBuff,
                                 sizeof(globalBuff)) &&
         sendAndReadOkResponseBC(&bc95serial, "AT+COPS=1,2,\"21401\"",
                                 globalBuff, sizeof(globalBuff));
#endif
#ifdef AUTO_VERSION
  sendAndReadOkResponseBC(&bc95serial, "AT+NCONFIG=AUTOCONNECT,TRUE",
                          globalBuff, sizeof(globalBuff));
#endif
}

bool attachNetwork()

{
  return
#ifdef VODAFONE_VERSION
      sendAndReadOkResponseBC(&bc95serial,
                              "AT+CGDCONT=1,\"IP\",\"lpwa.vodafone.iot\"",
                              globalBuff, sizeof(globalBuff)); // &&
#endif
#ifdef AUTO_VERSION
  sendAndReadOkResponseBC(&bc95serial, "AT+CGATT=1", globalBuff,
                          sizeof(globalBuff)) &&
      sendAndReadOkResponseBC(&bc95serial, "AT+CGATT?", globalBuff,
                              sizeof(globalBuff));
#endif
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

int openSocket() {
  ESP_LOGV(TAG, "Openning socket");
  if (!sendAndReadOkResponseBC(&bc95serial, "AT+NSOCR=STREAM,6,0,1", globalBuff,
                               sizeof(globalBuff))) {
    return -1;
  }

  char *socketPtr = strtok(globalBuff, "\r\n");
  ESP_LOGV(TAG, "Open Socket: %s", socketPtr);
  int socket = atoi(socketPtr);
  return socket;
}

bool connectSocket(int socket, char *ip, int port) {
  ESP_LOGV(TAG, "Connecting socket");
  char outBuffer[64];
  sprintf(outBuffer, "AT+NSOCO=%d,%s,%d", socket, ip, port);
  return sendAndReadOkResponseBC(&bc95serial, outBuffer, globalBuff,
                                 sizeof(globalBuff));
}

int sendData(int socket, char *data, int datalen, char *responseBuff,
             int responseBuffSize) // returns bytes read
{
  ESP_LOGV(TAG, "Sending data: %s", data);
  char outBuffer[256];
  sprintf(outBuffer, "AT+NSOSD=%d,%d,", socket, datalen);
  for (int i = 0; i < datalen; i++) {
    char b2[3];
    sprintf(b2, "%02X", data[i]);
    strcat(outBuffer, b2);
  }
  strcat(outBuffer, ",0x100,101");
  responseBuff[0] = 0;
  if (!sendAndReadOkResponseBC(&bc95serial, outBuffer, responseBuff,
                               responseBuffSize)) {
    return -1;
  }

  bool timeout = false;
  unsigned long startTime = millis();
  int buffPtr = strlen(responseBuff);
  char* scanPtr = responseBuff;
  responseBuff[buffPtr] = 0;
  sendStatus status = INIT;
  while (!timeout) {
    timeout = (millis() - startTime > NBSENDTIMEOUT);
    delay(100);
    while (bc95serial.available()) {
      responseBuff[buffPtr++] = bc95serial.read();
      if (buffPtr == responseBuffSize) {
        return -10;
      }
      responseBuff[buffPtr] = 0;
    }
    char expected[18];
    switch (status) {
    case INIT: {
      sprintf(expected, "%d,%d\r\n", socket, datalen);

      char *val = strstr(responseBuff, expected);
      if (val != nullptr) {
        scanPtr = val + strlen(expected);
        status = DATAOK;
        continue;
      }
      break;
    }
    case DATAOK: {
      sprintf(expected, "OK\r\n");

      char *val = strstr(responseBuff, expected);
      if (val != nullptr) {
        scanPtr = val + strlen(expected);
        status = SENTOK;
        continue;
      }
      break;
    }
    case SENTOK: {
      sprintf(expected, "+NSOSTR:%d,101,1",socket);
      char *val = strstr(responseBuff, expected);
      if (val != nullptr) {
        scanPtr = val + strlen(expected);
        status = RECOK;
        continue;
      }
      break;
    }
    case RECOK: {
      strcpy(responseBuff, scanPtr);
      return strlen(responseBuff);
      break;
    }
    }
  }

  return 0;
}

int readResponseData(char *response, int responseLen, char *buffer,
                     int bufferSize) {
  ESP_LOGV(TAG, "Reading response: %s", response);
  char *socketPtr = strtok(response, ",");
  char *lenPtr = strtok(NULL, ",");
  char *dataPtr = strtok(NULL, "\r\n");
  char *endToken = strtok(NULL, "\r\n");
  int dataLen = strtoul(lenPtr, NULL, 10);
  int strLen = strlen(dataPtr);

  ESP_LOGD(TAG, "socketPtr: %s", socketPtr);
  ESP_LOGD(TAG, "lenPtr: %s", lenPtr);
  ESP_LOGD(TAG, "dataPtr: %s", dataPtr);
  ESP_LOGD(TAG, "dataLen: %d", dataLen);
  ESP_LOGD(TAG, "strLen: %d", strLen);

  if (dataLen != strLen / 2) {
    ESP_LOGE(TAG, "Size mismatch");
    return -1;
  }

  if (bufferSize < dataLen + 1) {
    ESP_LOGE(TAG, "Buffer too small");
    return -2;
  }

  char *tempBuff = new char[strLen + 1];
  memcpy(tempBuff, dataPtr, strLen + 1);
  for (int i = 0; i < strLen; i += 2) {
    char tmp[3];
    memcpy(tmp, tempBuff + i, 2);
    tmp[2] = 0;
    buffer[i / 2] = strtoul(tmp, NULL, 16);
  }
  buffer[strLen] = 0;
  delete tempBuff;
  return strLen;
}

int getReceivedBytes(int socket, char *buffer, int bufferSize) {
  char responseBuffer[2048];
  ESP_LOGV(TAG, "Getting received bytes");
  int readBytes = strlen(buffer);
  int buffPtr = readBytes;
  char* scanPtr = buffer;

  unsigned long startT = millis();
  while (millis() < startT + HTTP_READ_TIMEOUT) {
    ESP_LOGV(TAG, "buff: %s", scanPtr);
    delay(500);
    while (bc95serial.available()) {
      buffer[buffPtr++] =  bc95serial.read();
      readBytes+=1;
      if (buffPtr == bufferSize) {
        return -10;
      }
      buffer[buffPtr] = 0;
    }

    char expected[128];

    sprintf(expected, "+NSONMI:%d", socket);
    char *val = strstr(scanPtr, expected);
    if (val != nullptr) {
      char dataBuffer[2048];
      int len = readResponseData(scanPtr, strlen(scanPtr), dataBuffer,
                                 sizeof(dataBuffer));
      if (len < 0) {
        return len;
      }
      scanPtr = val + len;
      ESP_LOGV(TAG, "Data received from server: %s", dataBuffer);
      strcat(responseBuffer, dataBuffer);

      continue;
    }

    sprintf(expected, "+NSOCLI: %d", socket);
    val = strstr(scanPtr, expected);
    if (val != nullptr) {
      ESP_LOGV(TAG, "Socket closed");
      strcpy(buffer, responseBuffer);
      return strlen(responseBuffer);
    }
  };

  return strlen(buffer);
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

int getData(char *ip, int port, char *page, char *response) {
  char outBuf[256];

  int responseCode = 0;

  // Open socket
  int socketN = openSocket();

  bool connected = connectSocket(socketN, ip, port);

  // send the header
  globalBuff[0] = 0;
  sprintf(outBuf, "GET %s HTTP/1.1\r\n", page);
  strcat(globalBuff, outBuf);
  sprintf(outBuf, "Host: %s\r\n", ip);
  strcat(globalBuff, outBuf);
  strcat(globalBuff, "\r\n");
  ESP_LOGD(TAG, "Data string: %s", globalBuff);

  int sentOk = sendData(socketN, globalBuff, strlen(globalBuff), globalBuff,
                        sizeof(globalBuff));

  if (sentOk < 0) {
    ESP_LOGE(TAG, "Error sending data");
    return -1;
  }

  int bytesReceived = getReceivedBytes(socketN, globalBuff, sizeof(globalBuff));

  ESP_LOGV(TAG, "Data received: %s", globalBuff);

  if (bytesReceived < 0) {
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

  ESP_LOGI(TAG, "Return code %d", responseCode);
  return responseCode;
}

int postPage(char *domainBuffer, int thisPort, char *page, char *thisData,
             char *identityKey) {
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
    int bytesReceived = sendData(1, globalBuff, strlen(globalBuff), globalBuff,
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

bool checkMqttConnection() {
  ESP_LOGD(TAG, "SENDING TO Modem: AT+QMTCONN?");

  bc95serial.println("AT+QMTCONN?");
  char data[128];
  int bytesRead = readResponseBC(&bc95serial, data, 128);
  if (assertResponseBC("+QMTCONN: 0,3", data, bytesRead)) {
    return true;
  }
  return false;
}

int readMqttSubData(char *buff, int bufflen) {
  char data[2048];
  int bytesRead = readResponseBC(&bc95serial, data, sizeof(data));

  std::string response = std::string(data);
  int firstResponse = response.find("+QMTRECV: 0,0,");
  if (firstResponse == std::string::npos) {
    return -1;
  }
  int topicFirstQuote = response.find("\"", firstResponse);
  if (topicFirstQuote == std::string::npos) {
    return -2;
  }
  int topicSecondQuote = response.find("\"", topicFirstQuote + 1);
  if (topicSecondQuote == std::string::npos) {
    return -3;
  }
  int messageComma = response.find(",", topicSecondQuote + 1);
  if (messageComma == std::string::npos) {
    return -4;
  }

  int messageEnd = response.find("\n", messageComma + 1);
  if (messageEnd == std::string::npos) {
    return -5;
  }

  // extract message between quotes
  std::string topic = response.substr(topicFirstQuote + 1,
                                      topicSecondQuote - topicFirstQuote - 1);
  std::string message =
      response.substr(messageComma + 1, messageEnd - messageComma - 1);
  ESP_LOGD(TAG, "Message in Topic: %s", topic.c_str());
  ESP_LOGD(TAG, "Message: %s", message.c_str());

  // copy message to buffer
  strncpy(buff, message.c_str(), bufflen);
  return message.length();
}

bool dataAvailable() {
  if (bc95serial.available()) {
    return true;
  }
  return false;
}

int connectMqtt(char *url, int port, char *password, char *clientId) {
  ESP_LOGI(TAG, "SENDING TO Modem: AT+QMTOPEN=0,\"%s\",%d", url, port);

  bc95serial.print("AT+QMTOPEN=0,\"");
  bc95serial.print(url);
  bc95serial.print("\",");
  bc95serial.println(port);
  char data[128];
  int bytesRead = readResponseBC(&bc95serial, data, 128);

  if (!assertResponseBC("OK\r", data, bytesRead)) {
    return -1;
  }
  int responseBytes = 0;
  if (!assertResponseBC("+QMTOPEN: 0,0", data, bytesRead)) {
    for (int i = 0; i < 30; i++) {
      responseBytes = readResponseBC(&bc95serial, data, 128, 1000);
      if (responseBytes != 0) {
        break;
      }
      ESP_LOGI(TAG, "Wait for conn");
      delay(1000);
    }
    if (!assertResponseBC("+QMTOPEN: 0,0", data, responseBytes)) {
      return -1;
    }
  }

  ESP_LOGI(TAG, "SENDING TO Modem: AT+QMTCONN=0,\"%s\",\"gesinen\",\"%s\"",
           clientId, password);

  bc95serial.print("AT+QMTCONN=0,\"");
  bc95serial.print(clientId);
  bc95serial.print("\",\"gesinen\",\"");
  bc95serial.print(password);
  bc95serial.println("\"");

  bytesRead = readResponseBC(&bc95serial, data, 128);

  if (!assertResponseBC("OK\r", data, bytesRead)) {
    return -3;
  }

  for (int i = 0; i < 30; i++) {
    responseBytes = readResponseBC(&bc95serial, data, 128, 1000);
    if (responseBytes != 0) {
      break;
    }
    ESP_LOGI(TAG, "Wait for conn");
    delay(1000);
  }
  if (!assertResponseBC("+QMTCONN: 0,0,0", data, responseBytes)) {
    return -2;
  }
  return 0;
}

bool subscribeMqtt(char *topic) {
  ESP_LOGD(TAG, "SENDING TO Modem: AT+QMTSUB=0,1,\"%s\",%d", topic, 0);
  bc95serial.print("AT+QMTSUB=0,1,\"");
  bc95serial.print(topic);
  bc95serial.print("\",");
  bc95serial.println("0");
  char data[128];
  int bytesRead = readResponseBC(&bc95serial, data, 128);
  if (!assertResponseBC("OK\r", data, bytesRead)) {
    return false;
  }
  return true;
}

int unsubscribeMqtt(char *topic, int qos) {}
int checkSubscriptionMqtt(char *message) {}
int publishMqtt(char *topic, char *message, int qos) {
  ESP_LOGI(TAG, "SENDING TO Modem: AT+QMTPUB=0,0,0,0,\"%s\"", topic);

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

  responseBytes =
      readResponseWithStop(&bc95serial, data, 512, "+QMTPUB: 0,0,0", 5000);
  while (bc95serial.available()) {
    bc95serial.read();
  }
  if (responseBytes < 0)
    return responseBytes;
  return 0;
}
int disconnectMqtt() {
  ESP_LOGI(TAG, "SENDING TO Modem: AT+QMTDISC=0");
  bc95serial.println("AT+QMTDISC=0");
  // ESP_LOGI(TAG, "SENDING TO Modem: AT+QMTCLOSE=0");
  // bc95serial.println("AT+QMTCLOSE=0");
  char data[64];
  int responseBytes = readResponseBC(&bc95serial, data, 64);
  if (!assertResponseBC("OK", data, responseBytes)) {
    return -1;
  }
  return 0;
}