#include <BC95.hpp>

// SoftwareSerial bc95serial(8, 9);
HardwareSerial bc95serial(1);
char globalBuff[4096];
char TAG[] = "BC95";

// === ADEMUX: variables globales NUESTATS ===
int8_t  nb_status_rsrp      = 127;   // 127 = no disponible
int8_t  nb_status_snr_radio = 127;   // 127 = no disponible
uint8_t nb_status_ecl       = 0xFF;  // 0xFF = no disponible

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
    ESP_LOGD(TAG, "%d bytes read", bytesRead);
    ESP_LOGD(TAG, "Message: %s", buff);
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
      ESP_LOGV(TAG, "Stopword %s found", stopWord);
      ESP_LOGV(TAG, "%d bytes read", p);
      ESP_LOGV(TAG, "Message: %s", buff);
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
                             char *buffer, int bufferSize, uint32_t timeout = 500) {
  ESP_LOGV(TAG, "Command: %s", command);
  port->println(command);
  int bytesRead = readResponseBC(port, buffer, bufferSize, timeout);
  return assertResponseBC("OK\r", buffer, bytesRead);
}

void initModem() {
  bc95serial.setRxBufferSize(4096);
  bc95serial.begin(9600, SERIAL_8N1, RX_PIN, TX_PIN);
#ifdef DEBUG_MODEM
  // ESP_LOGD(TAG, bc95serial.readString().c_str());
#endif
}

bool networkReady() {
  bc95serial.println("AT+CEREG?");
  int bytesRead = readResponseBC(&bc95serial, globalBuff, sizeof(globalBuff));
  if (assertResponseBC("CEREG:0,1", globalBuff, bytesRead))
    return true;
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
  bc95serial.write(26);
  delay(1000);
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

bool preConfigModem() {
  ESP_LOGI(TAG, "Preconfiguring NBIOT modem");
  return sendAndReadOkResponseBC(&bc95serial, "AT", globalBuff,
                                 sizeof(globalBuff)) &&
         sendAndReadOkResponseBC(&bc95serial, "AT+NCONFIG=AUTOCONNECT,FALSE",
                                 globalBuff, sizeof(globalBuff));
}

bool configModem() {
  ESP_LOGI(TAG, "Config NBIOT modem");
  return sendAndReadOkResponseBC(&bc95serial, "AT+CEREG=0", globalBuff,
                                 sizeof(globalBuff)) &&
         sendAndReadOkResponseBC(&bc95serial, "AT+NBAND=8,20", globalBuff,
                                 sizeof(globalBuff)) &&
         sendAndReadOkResponseBC(&bc95serial, "AT+NCONFIG=CELL_RESELECTION,TRUE", globalBuff,
                                 sizeof(globalBuff)) &&
         sendAndReadOkResponseBC(&bc95serial, "AT+CSCON=0", globalBuff,
                                 sizeof(globalBuff)) &&
         sendAndReadOkResponseBC(&bc95serial, "AT+CFUN=1", globalBuff,
                                 sizeof(globalBuff), 10000) &&
         sendAndReadOkResponseBC(&bc95serial, "AT+QREGSWT=1", globalBuff,
                                 sizeof(globalBuff)) &&
         sendAndReadOkResponseBC(&bc95serial, "AT+NSONMI=3", globalBuff,
                                 sizeof(globalBuff));
}

bool attachNetwork() {
  return sendAndReadOkResponseBC(&bc95serial,
                              "AT+CGDCONT=1,\"IP\",\"" APN "\"",
                              globalBuff, sizeof(globalBuff)) &&
      sendAndReadOkResponseBC(&bc95serial, "AT+CGATT=1", globalBuff,
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
             int responseBuffSize) {
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
      sprintf(expected, "+NSOSTR:%d,101,1", socket);
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

int readResponseData(std::string response, char *buffer, int bufferSize) {
  int socketIndex = response.find(",");
  if (socketIndex == std::string::npos) {
    ESP_LOGD(TAG, "Cannot find socket index in response");
    return -1;
  }
  int lenIndex = response.find(",", socketIndex + 1);
  if (lenIndex == std::string::npos) {
    ESP_LOGD(TAG, "Cannot find length index in response");
    return -1;
  }
  int dataIndex = response.find("\r\n", lenIndex + 1);
  if (dataIndex == std::string::npos) {
    ESP_LOGD(TAG, "Cannot find data index in response");
    return -1;
  }

  std::string socketString = response.substr(0, socketIndex);
  std::string lenString = response.substr(socketIndex + 1, lenIndex - socketIndex - 1);
  std::string dataString = response.substr(lenIndex + 1, dataIndex - lenIndex - 1);

  int dataLen = strtoul(lenString.c_str(), NULL, 10);
  int strLen = strlen(dataString.c_str());

  if (dataLen != strLen / 2) {
    ESP_LOGE(TAG, "Size mismatch");
    return -1;
  }

  if (bufferSize < dataLen + 1) {
    ESP_LOGE(TAG, "Buffer too small");
    return -2;
  }

  char* tempBuff = (char*)dataString.c_str();
  for (int i = 0; i < strLen; i += 2) {
    char tmp[3];
    memcpy(tmp, tempBuff + i, 2);
    tmp[2] = 0;
    buffer[i / 2] = strtoul(tmp, NULL, 16);
  }
  buffer[strLen/2] = 0;
  return dataLen;
}

int getReceivedBytes(int socket, char *buffer, int bufferSize) {
  char responseBuffer[2048];
  responseBuffer[0] = 0;
  size_t responseBufferPos = 0;
  ESP_LOGV(TAG, "Getting received bytes");
  int readBytes = strlen(buffer);
  int buffPtr = readBytes;
  char* scanPtr = buffer;

  unsigned long startT = millis();
  while (millis() < startT + HTTP_READ_TIMEOUT) {
    while (bc95serial.available()) {
      buffer[buffPtr++] = bc95serial.read();
      readBytes += 1;
      if (buffPtr == bufferSize) {
        return -10;
      }
      buffer[buffPtr] = 0;
    }
    std::string current = std::string(scanPtr);
    char expected[32];

    sprintf(expected, "\r\n");

    size_t pos = current.find(expected);
    if (pos == std::string::npos) {
      continue;
    }

    std::string line = current.substr(0, pos + 2);
    scanPtr += line.length();

    sprintf(expected, "+NSONMI:%d", socket);
    if (line.find(expected) != std::string::npos) {
      char dataBuffer[2048];
      int len = readResponseData(line, dataBuffer, sizeof(dataBuffer));
      if (len < 0) {
        return len;
      }
      memcpy(responseBuffer + responseBufferPos, dataBuffer, len);
      responseBufferPos += len;
      continue;
    }

    sprintf(expected, "+NSOCLI: %d", socket);
    if (line.find(expected) != std::string::npos) {
      ESP_LOGV(TAG, "Socket closed");
      memcpy(buffer, responseBuffer, responseBufferPos);
      buffer[responseBufferPos] = 0;
      return responseBufferPos;
    }
  };

  memcpy(buffer, responseBuffer, responseBufferPos);
  return -2;
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

int parseResponseCode(char* buff, int buffSize) {
  std::string inputString = std::string(buff);
  size_t pos = inputString.find("\r\n");
  if (pos == std::string::npos) {
    return -1;
  }

  std::string httpResponseLine = inputString.substr(0, pos + 2);
  ESP_LOGV(TAG, "Response line: %s", httpResponseLine.c_str());

  size_t responseCodePos = httpResponseLine.find(" ");
  size_t responseCodePosEnd = httpResponseLine.find(" ", responseCodePos + 1);

  std::string responseCodeStr = httpResponseLine.substr(responseCodePos + 1, responseCodePosEnd - responseCodePos - 1);
  ESP_LOGV(TAG, "Response code: %s", responseCodeStr.c_str());
  return strtoul(responseCodeStr.c_str(), NULL, 10);
}

int parseContentLength(char* buff, int buffSize) {
  std::string inputString = std::string(buff);
  size_t pos = inputString.find("\r\n\r\n");
  if (pos == std::string::npos) {
    return -1;
  }

  std::string httpString = inputString.substr(0, pos + 4);

  std::for_each(httpString.begin(), httpString.end(), [](char & c) {
    c = ::tolower(c);
  });

  size_t contentLengthPos = httpString.find("content-length:");
  size_t contentLengthPosEnd = httpString.find("\r\n", contentLengthPos + 1);
  std::string contentLengthStr = httpString.substr(contentLengthPos + 15, contentLengthPosEnd - contentLengthPos - 15);

  ESP_LOGV(TAG, "Content Length str: %s", contentLengthStr.c_str());

  unsigned long contentLength = strtoul(contentLengthStr.c_str(), NULL, 10);
  return contentLength;
}

int parseData(char* buff, int dataSize, char* outBuff, int outBuffSize) {
  std::string inputString = std::string(buff);
  size_t pos = inputString.find("\r\n\r\n");
  if (pos == std::string::npos) {
    return -1;
  }

  char* dataPos = buff + pos + 4;
  memcpy(outBuff, dataPos, dataSize);
  return dataSize;
}

int getData(char *ip, int port, char *page, char *responseBuffer, int responseBufferSize, int *responseSizePtr) {
  char devEui[32];
  sprintf(devEui, "%02x%02x%02x%02x%02x%02x%02x%02x", DEVEUI[0],
          DEVEUI[1], DEVEUI[2], DEVEUI[3], DEVEUI[4], DEVEUI[5], DEVEUI[6],
          DEVEUI[7]);

  char outBuf[256];
  int responseCode = 0;

  int socketN = openSocket();
  bool connected = connectSocket(socketN, ip, port);

  char pageWithParams[256];
  std::string version = std::string(PROGVERSION);
  std::replace(version.begin(), version.end(), '.', '_');

  char localBuff[4096];
  sprintf(pageWithParams, "%s?deveui=%s&version=%s", page, devEui, version.c_str());
  localBuff[0] = 0;
  sprintf(outBuf, "GET %s HTTP/1.1\r\n", pageWithParams);
  strcat(localBuff, outBuf);
  sprintf(outBuf, "Host: %s\r\n", ip);
  strcat(localBuff, outBuf);
  strcat(localBuff, "\r\n");

  int sentOk = sendData(socketN, localBuff, strlen(localBuff), localBuff, sizeof(localBuff));

  if (sentOk < 0) {
    ESP_LOGE(TAG, "Error sending data");
    return -1;
  }

  int bytesReceived = getReceivedBytes(socketN, localBuff, sizeof(localBuff));

  if (bytesReceived < 0) {
    ESP_LOGE(TAG, "failed sending data with error code: %d", bytesReceived);
    responseCode = bytesReceived;
  } else if (bytesReceived > 0) {
    ESP_LOGD(TAG, "Received %d bytes", bytesReceived);
    responseCode = parseResponseCode(localBuff, bytesReceived);
    ESP_LOGD(TAG, "Response Code: %d", responseCode);

    if (responseCode != 200) {
      ESP_LOGE(TAG, "Error code: %d", responseCode);
      return responseCode;
    }

    char* inputBuffer = new char[bytesReceived + 1];
    memcpy(inputBuffer, localBuff, bytesReceived);
    inputBuffer[bytesReceived] = 0;
    int contentLength = parseContentLength(inputBuffer, bytesReceived);
    int parsedData = parseData(inputBuffer, contentLength, localBuff, sizeof(localBuff));
    delete inputBuffer;
    if (parsedData >= 0) {
      memcpy(responseBuffer, localBuff, parsedData);
      *responseSizePtr = parsedData;
      responseBuffer[parsedData] = 0;
      return responseCode;
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
    strcat(globalBuff, thisData);

    int responseCode = 0;
    int bytesReceived = sendData(1, globalBuff, strlen(globalBuff), globalBuff, sizeof(globalBuff));
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
  if (firstResponse == std::string::npos) return -1;

  int topicFirstQuote = response.find("\"", firstResponse);
  if (topicFirstQuote == std::string::npos) return -2;

  int topicSecondQuote = response.find("\"", topicFirstQuote + 1);
  if (topicSecondQuote == std::string::npos) return -3;

  int messageComma = response.find(",", topicSecondQuote + 1);
  if (messageComma == std::string::npos) return -4;

  int messageEnd = response.find("\n", messageComma + 1);
  if (messageEnd == std::string::npos) return -5;

  std::string topic = response.substr(topicFirstQuote + 1, topicSecondQuote - topicFirstQuote - 1);
  std::string message = response.substr(messageComma + 1, messageEnd - messageComma - 1);
  ESP_LOGD(TAG, "Message in Topic: %s", topic.c_str());
  ESP_LOGD(TAG, "Message: %s", message.c_str());

  strncpy(buff, message.c_str(), bufflen);
  return message.length();
}

bool dataAvailable() {
  if (bc95serial.available()) {
    return true;
  }
  return false;
}

void configureMqtt() {
  bc95serial.println("AT+QMTCFG=\"version\",0,4");
  char data[128];
  int bytesRead = readResponseBC(&bc95serial, data, 128);
  if (!assertResponseBC("OK\r", data, bytesRead)) {
    ESP_LOGE(TAG, "Error configuring MQTT version");
  }

  bc95serial.println("AT+QMTCFG=\"keepalive\",0,60");
  bytesRead = readResponseBC(&bc95serial, data, 128);
  if (!assertResponseBC("OK\r", data, bytesRead)) {
    ESP_LOGE(TAG, "Error configuring MQTT keepalive");
  }
}

int connectMqtt(char *url, int port, char* username, char *password, char *clientId) {
  disconnectMqtt();
  configureMqtt();
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
      if (responseBytes != 0) break;
      ESP_LOGI(TAG, "Wait for conn");
      delay(2000);
    }
    if (!assertResponseBC("+QMTOPEN: 0,0", data, responseBytes)) {
      return -1;
    }
  }

  char mqttRandomSeed[16];
  sprintf(mqttRandomSeed, "%d", random(1000000));

  ESP_LOGI(TAG, "SENDING TO Modem: AT+QMTCONN=0,\"%s-%s\",\"%s\",\"%s\"",
           clientId, mqttRandomSeed, username, password);

  bc95serial.print("AT+QMTCONN=0,\"");
  bc95serial.print(clientId);
  bc95serial.print("-");
  bc95serial.print(mqttRandomSeed);
  bc95serial.print("\",\"gesinen\",\"");
  bc95serial.print(password);
  bc95serial.println("\"");

  bytesRead = readResponseBC(&bc95serial, data, 128);

  if (!assertResponseBC("OK\r", data, bytesRead)) {
    return -3;
  }

  if (!assertResponseBC("+QMTCONN: 0,0,0", data, bytesRead)) {
    for (int i = 0; i < 30; i++) {
      responseBytes = readResponseBC(&bc95serial, data, 128, 1000);
      if (responseBytes != 0) break;
      ESP_LOGI(TAG, "Wait for conn");
      delay(2000);
    }
    if (!assertResponseBC("+QMTCONN: 0,0,0", data, responseBytes)) {
      return -2;
    }
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

  for (int i = 0; i < 5; i++) {
    bytesRead = readResponseBC(&bc95serial, data, 128, 1000);
    if (bytesRead > 0 && strstr(data, "+QMTSUB:") != nullptr) {
      ESP_LOGD(TAG, "QMTSUB confirmation consumed: %s", data);
      break;
    }
    if (bytesRead == 0) {
      ESP_LOGD(TAG, "Waiting for QMTSUB confirmation, attempt %d", i + 1);
    } else {
      ESP_LOGD(TAG, "Discarding unexpected data while waiting QMTSUB: %s", data);
    }
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

  responseBytes = readResponseWithStop(&bc95serial, data, 512, "+QMTPUB: 0,0,0", 5000);
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
  char data[64];
  int responseBytes = readResponseBC(&bc95serial, data, 64);
  if (!assertResponseBC("OK", data, responseBytes)) {
    return -1;
  }
  return 0;
}

// =========================================================
//  Obtener IMEI del modem Quectel BC95-G  (AT+CGSN=1)
// =========================================================
String bc95_getImei() {
  char buffer[256];

  ESP_LOGI(TAG, "Solicitando IMEI del módulo BC95-G...");
  cleanbuffer();
  sendAndReadOkResponseBC(&bc95serial, "ATE0", buffer, sizeof(buffer), 800);

  bc95serial.println("AT+CGSN=1");
  int len = readResponseBC(&bc95serial, buffer, sizeof(buffer), 2000);
  if (len <= 0) {
    ESP_LOGE(TAG, "No hubo respuesta al comando AT+CGSN=1");
    return "";
  }

  ESP_LOGD(TAG, "Respuesta cruda IMEI: %s", buffer);

  String resp = buffer;
  int p = resp.indexOf("+CGSN:");
  if (p < 0) {
    ESP_LOGE(TAG, "No se encontró +CGSN: en la respuesta");
    return "";
  }
  p += 6;

  String imei = "";
  for (int i = p; i < resp.length(); i++) {
    char c = resp[i];
    if (c >= '0' && c <= '9') imei += c;
    else if (imei.length() > 0) break;
  }

  if (imei.length() == 15) {
    ESP_LOGI(TAG, "IMEI del BC95-G: %s", imei.c_str());
    return imei;
  }

  ESP_LOGE(TAG, "IMEI inválido o incompleto: %s", imei.c_str());
  return "";
}

// =========================================================
// Obtener MSISDN del modem Quectel BC95-G (AT+CNUM)
// =========================================================
String bc95_getMsisdn() {
  char buffer[256];

  ESP_LOGI(TAG, "Solicitando MSISDN (numero telefonico SIM)...");
  cleanbuffer();
  sendAndReadOkResponseBC(&bc95serial, "ATE0", buffer, sizeof(buffer), 800);

  bc95serial.println("AT+CNUM");
  int len = readResponseBC(&bc95serial, buffer, sizeof(buffer), 2000);

  if (len <= 0) {
    ESP_LOGE(TAG, "No hubo respuesta al comando AT+CNUM");
    return "";
  }

  ESP_LOGD(TAG, "Respuesta cruda MSISDN: %s", buffer);

  String resp = buffer;
  int p = resp.indexOf("+CNUM:");
  if (p < 0) {
    ESP_LOGE(TAG, "No se encontró +CNUM: en la respuesta");
    return "";
  }

  int q1 = resp.indexOf('"', p + 6);
  if (q1 < 0) return "";
  int q2 = resp.indexOf('"', q1 + 1);
  if (q2 < 0) return "";
  int q3 = resp.indexOf('"', q2 + 1);
  if (q3 < 0) return "";
  int q4 = resp.indexOf('"', q3 + 1);
  if (q4 < 0) return "";

  String msisdn = resp.substring(q3 + 1, q4);
  ESP_LOGI(TAG, "MSISDN de la SIM: %s", msisdn.c_str());
  return msisdn;
}

// =========================================================
//  ADEMUX: Leer métricas de señal NB-IoT
//
//  Paso 1 — AT+NUESTATS=CELL (sin comillas, según datasheet BC95-G v1.4)
//  Respuesta: una línea CSV por celda:
//    NUESTATS:CELL,<earfcn>,<pci>,<primarycell>,<rsrp>,<rsrq>,<rssi>,<snr>
//  Todos los valores en centibels (÷10 para dBm/dB)
//  Campos (0-indexed tras el prefijo):
//    0=earfcn  1=pci  2=primarycell  3=rsrp  4=rsrq  5=rssi  6=snr
//
//  Paso 2 — AT+NUESTATS=RADIO (para ECL)
//  Respuesta multilínea, buscar:
//    NUESTATS:RADIO,ECL:<valor>
//
//  Actualiza globales: nb_status_rsrp, nb_status_snr_radio, nb_status_ecl
// =========================================================
bool bc95_getNuestats() {
  char buffer[512];

  // -------------------------------------------------------
  // PASO 1: AT+NUESTATS=CELL → RSRP y SNR
  // -------------------------------------------------------
  cleanbuffer();
  bc95serial.println("AT+NUESTATS=CELL");

  int len = readResponseWithStop(&bc95serial, buffer, sizeof(buffer), "OK\r\n", 5000);

  if (len <= 0) {
    ESP_LOGE(TAG, "NUESTATS=CELL: sin respuesta o timeout (len=%d)", len);
    nb_status_rsrp      = 127;
    nb_status_snr_radio = 127;
    nb_status_ecl       = 0xFF;
    return false;
  }

  ESP_LOGD(TAG, "NUESTATS=CELL raw: %s", buffer);

  // Buscar la primera línea "NUESTATS:CELL,"
  // Formato: NUESTATS:CELL,earfcn,pci,primarycell,rsrp,rsrq,rssi,snr
  char *line = strstr(buffer, "NUESTATS:CELL,");
  if (line == nullptr) {
    ESP_LOGE(TAG, "NUESTATS=CELL: no se encontró 'NUESTATS:CELL,'");
    nb_status_rsrp      = 127;
    nb_status_snr_radio = 127;
    nb_status_ecl       = 0xFF;
    return false;
  }

  // Avanzar tras el prefijo
  line += 14; // len("NUESTATS:CELL,") == 14

  // Parsear 7 campos CSV: earfcn,pci,primarycell,rsrp,rsrq,rssi,snr
  int fields[7];
  int nfields = 0;
  char *tok = strtok(line, ",\r\n");
  while (tok != nullptr && nfields < 7) {
    fields[nfields++] = atoi(tok);
    tok = strtok(nullptr, ",\r\n");
  }

  if (nfields < 7) {
    ESP_LOGE(TAG, "NUESTATS=CELL: solo %d campos (esperados 7)", nfields);
    nb_status_rsrp      = 127;
    nb_status_snr_radio = 127;
    nb_status_ecl       = 0xFF;
    return false;
  }

  // campo[3] = rsrp en centibels → /10 = dBm
  nb_status_rsrp = (int8_t)(fields[3] / 10);
  // campo[6] = snr (unidad directa, dB entero según datasheet)
  nb_status_snr_radio = (int8_t)fields[6];

  ESP_LOGD(TAG, "NUESTATS CELL: earfcn=%d pci=%d rsrp=%d(%d dBm) rsrq=%d rssi=%d snr=%d",
           fields[0], fields[1], fields[3], nb_status_rsrp,
           fields[4], fields[5], fields[6]);

  // -------------------------------------------------------
  // PASO 2: AT+NUESTATS=RADIO → ECL
  // Respuesta multilínea, buscar "NUESTATS:RADIO,ECL:<val>"
  // -------------------------------------------------------
  delay(200);
  cleanbuffer();
  bc95serial.println("AT+NUESTATS=RADIO");

  len = readResponseWithStop(&bc95serial, buffer, sizeof(buffer), "OK\r\n", 5000);

  if (len <= 0) {
    ESP_LOGW(TAG, "NUESTATS=RADIO: sin respuesta (ECL quedará N/A)");
    nb_status_ecl = 0xFF;
  } else {
    ESP_LOGV(TAG, "NUESTATS=RADIO raw: %s", buffer);

    // Buscar "NUESTATS:RADIO,ECL:"
    char *ecl_ptr = strstr(buffer, "NUESTATS:RADIO,ECL:");
    if (ecl_ptr != nullptr) {
      ecl_ptr += 19; // len("NUESTATS:RADIO,ECL:") == 19
      uint8_t ecl = (uint8_t)atoi(ecl_ptr);
      nb_status_ecl = (ecl <= 2) ? ecl : 0xFF;
      ESP_LOGD(TAG, "NUESTATS RADIO: ECL=%d", nb_status_ecl);
    } else {
      ESP_LOGW(TAG, "NUESTATS=RADIO: no se encontró 'ECL'");
      nb_status_ecl = 0xFF;
    }
  }

  ESP_LOGI(TAG, "NUESTATS → RSRP:%d dBm  SNR:%d dB  ECL:%d",
           nb_status_rsrp, nb_status_snr_radio, nb_status_ecl);

  return (nb_status_rsrp != 127);
}