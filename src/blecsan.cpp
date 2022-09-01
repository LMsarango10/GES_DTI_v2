/* code snippets taken from
https://github.com/nkolban/esp32-snippets/tree/master/BLE/scanner
*/

#include "blescan.h"

#define BT_BD_ADDR_HEX(addr)                                                   \
  addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]

// local Tag for logging
static const char TAG[] = "bluetooth";

int readResponse(HardwareSerial* port, char* buff, int b_size, uint32_t timeout=500)
{
  port->setTimeout(timeout);
  int bytesRead = port->readBytesUntil('\n',buff, b_size);
  if(bytesRead > 0)
  {
    buff[bytesRead] = 0;
    ESP_LOGV(TAG, "%d bytes read", bytesRead);
    ESP_LOGV(TAG, "Message: %s", buff);
    return bytesRead;
  }
  else if (bytesRead < 0) return -1;
  return 0;
}

bool assertResponse(const char* expected, char* received, int bytesRead)
{
  if(bytesRead <= 0) return false;
  return strstr(received, expected) != nullptr;
}
void flushPort(HardwareSerial* port) {
  while(port->available() > 0) {
    port->read();
  }
}
bool sendAndReadOkResponse(HardwareSerial* port, const char* command)
{
  flushPort(port);
  ESP_LOGV(TAG, "Command: %s", command);
  port->println(command);
  port->flush();
  char buffer[64];
  buffer[0] = 0;
  int bytesRead = readResponse(port, buffer, sizeof(buffer)-1, 2000);
  buffer[bytesRead] = 0;
  if(bytesRead > 0) {
    ESP_LOGV(TAG, "Response: %s", buffer);
  }

  return assertResponse("OK\r", buffer, bytesRead);
}

int getDetectedDevices(char* buff, int buffLen)
{
  if (!assertResponse("Devices Found", buff, buffLen)) return -1;
  return strtoul(buff + 14, NULL, 10);
}

void getMac(char* buff, uint8_t* out)
{
  for (int i =1 ; i < 7; i++)
  {
    char tempbuff[3];
    tempbuff[0] = buff[2*i];
    tempbuff[1] = buff[2*i + 1];
    tempbuff[2] = 0;
    out[i-1] = strtoul(tempbuff, NULL, 16) & 0xFF;
  }
  return;
}

int getMacsFromBT(char* buff, int bytesRead)
{
  if (bytesRead <= 0 )
    return 0;

  int startPos = 5;
  char* endPtr = nullptr;
  unsigned long p1 = strtoul(buff + startPos, &endPtr, 16);
  unsigned long p2 = strtoul(endPtr+1, &endPtr, 16);
  unsigned long p3 = strtoul(endPtr+1, &endPtr, 16);
  unsigned long btClass = strtoul(endPtr+1, &endPtr, 16) ;
  int16_t rssi = strtol(endPtr+1, NULL, 16) &0xFFFF;

  //ESP_LOGV(TAG, "Parse input: MAC: %04X%02X%06X, BT Class: %06X, RSSI: %d", p1, p2, p3, btClass, rssi);

  uint8_t mac[6];
  mac[0] = (p1 >> 8) & 0xFF;
  mac[1] = (p1) & 0xFF;
  mac[2] = (p2) & 0xFF;
  mac[3] = (p3 >> 16) & 0xFF;
  mac[4] = (p3 >> 8) & 0xFF;
  mac[5] = (p3) & 0xFF;

  /*char tempBuffer[64];
  sprintf(tempBuffer, "Device MAC: ");
  for (int n = 0; n < 6; n++)
  {
    sprintf(tempBuffer + 2*n, "%02X",mac[n]);
  }
  ESP_LOGV(TAG, "MAC parsed: %s", tempBuffer);*/
  mac_add((uint8_t *)mac, rssi, MAC_SNIFF_BT);
  return 0;
}

#ifdef OLD_BLE_METHOD
int getMacsFromBLE(int totalMacs)
{
  if (totalMacs <= 0 )
    return 0;
  BLESerial.println("AT+SHOW");
  char buffer[64];
  long start_time = millis();
  for (int i =0; i<totalMacs; i++)
  {
    int bytesRead = readResponse(&BLESerial, buffer, sizeof(buffer));
    if (!assertResponse("Device", buffer, bytesRead)) return -1;
    int deviceN = strtoul(buffer + 7, NULL, 10);
    bytesRead = readResponse(&BLESerial, buffer, sizeof(buffer));
    if (bytesRead != 15 || !assertResponse("0x", buffer, bytesRead))
    {
      ESP_LOGE(TAG, "error reading mac, ignoring");
      continue;
    }
    uint8_t mac[6];
    getMac(buffer, mac);
    char tempBuffer[64];
    sprintf(tempBuffer, "Device #%d MAC: ", deviceN);
    for (int n = 0; n < 6; n++)
    {
      sprintf(tempBuffer + 2*n, "%02X",mac[n]);
    }
    ESP_LOGV(TAG, "%s", tempBuffer);
    mac_add((uint8_t *)mac, 100, MAC_SNIFF_BLE);
  }
  while(BLESerial.available())
  {
    BLESerial.read();
  }
  return 0;
}
#else
int getMacsFromBLE(char* buffer, int bytesRead)
{
  if (!assertResponse("+INQ:", buffer, bytesRead)) return 0;

  char* endPtr = nullptr;

  std::string response = std::string(buffer);

  int startPos = response.find(" 0x") + 3;
  std::string macStr = response.substr(startPos);

  if (macStr.length() < 13)
  {
    ESP_LOGE(TAG, "error reading mac, ignoring");
    return 0;
  }

  ESP_LOGV(TAG, "MAC: %s", macStr.c_str());

  uint8_t macBytes[6];
  for (int i = 0; i < 6; i++)
  {
    macBytes[i] = strtoul(macStr.substr(i * 2, 2).c_str(), nullptr, 16);
  }
  mac_add((uint8_t *)macBytes, 0, MAC_SNIFF_BLE);
  return 1;
}
#endif

bool reinitBLE()
{
  if(!sendAndReadOkResponse(&BLESerial,"AT"))
  {
    ESP_LOGE(TAG, "ERROR INITIALIZING BLE, No AT response");
    return false;
  }
  char buffer[64];
  BLESerial.println("AT+ROLE");
  int bytesRead = readResponse(&BLESerial, buffer, sizeof(buffer));
  if(bytesRead <= 0)
  {
    ESP_LOGE(TAG, "ERROR INITIALIZING BLE, No response");
    return false;
  }
  if(!assertResponse("+ROLE=1\r", buffer, bytesRead))
  {
    BLESerial.println("AT+ROLE1");
    bytesRead = readResponse(&BLESerial, buffer, sizeof(buffer));

    if( !assertResponse("+ROLE=1\r", buffer, bytesRead) )
    {
      ESP_LOGE(TAG, "ERROR INITIALIZING BLE AT ROLE");
      return false;
    }
    readResponse(&BLESerial, buffer, sizeof(buffer));
    if( !assertResponse("OK\r", buffer, bytesRead) )
    {
      ESP_LOGE(TAG,"ERROR INITIALIZING BLE OK TIMEOUT");
      return false;
    }
  }
  ESP_LOGD(TAG, "BLE Initialized as Master");
  return true;
}

void initBTSerial(long baud)
{
  BTSerial.begin(baud, SERIAL_8N1, RX_BT, TX_BT);
  delay(1000);
}

void initBLESerial()
{
  BLESerial.begin(9600, SERIAL_8N1, RX_BLE, TX_BLE);
  delay(1000);
}

void setSerialToBT() {
  BTSerial.flush();
  delay(100) ;
  BTSerial.updateBaudRate(BT_BAUD);
  //initBTSerial(38400);
  digitalWrite(BLEBTMUX_A, LOW);
  delay(100);
}

void setSerialToBLE() {
  BLESerial.flush();
  delay(100);
  BLESerial.updateBaudRate(9600);
  //initBLESerial();
  digitalWrite(BLEBTMUX_A, HIGH);
  delay(100);
}

bool initBLE()
{
  pinMode(EN_BLE, OUTPUT);
  pinMode(BLEBTMUX_A, OUTPUT);
  digitalWrite(EN_BLE, HIGH);
  //initBLESerial();
  setSerialToBLE();
  return reinitBLE();
}

#ifdef OLD_BLE_METHOD
void BLECycle(void)
{
  if(!cfg.blescan) return;
  ESP_LOGV(TAG, "cycling ble scan");
  ESP_LOGV(TAG, "Set BLE inquiry mode");
  char buffer[64];
  BLESerial.println("AT+INQ");
  int bytesRead = readResponse(&BLESerial, buffer, sizeof(buffer));
  if(! assertResponse("+INQS\r", buffer, bytesRead))
  {
    ESP_LOGD(TAG, "Error setting BLE INQ mode");
    return;
  }

  //ENTER INQ MODE
  ESP_LOGV(TAG, "start INQ mode ");
  long start_time = millis();
  while(millis() - start_time < (BTLE_SCAN_TIME/2) * 1000)
  {
    bytesRead = readResponse(&BLESerial, buffer, sizeof(buffer));
    if(assertResponse("+INQE\r", buffer, bytesRead))
    {
      ESP_LOGV(TAG, "finish INQ mode ");
      ESP_LOGV(TAG, "Response: %s", buffer);
      buffer[0] = 0;
      bytesRead = readResponse(&BLESerial, buffer, sizeof(buffer));
      ESP_LOGV(TAG, "Response: %s", buffer);
      int devicesDetected = getDetectedDevices(buffer, bytesRead);
      if(devicesDetected == -1) {
        ESP_LOGV(TAG, "No devices detected");
      } else {
        ESP_LOGV(TAG, "%d devices detected", devicesDetected);
      }

      getMacsFromBLE(devicesDetected);
      break;
    }
  }
  return;
}
#else
void BLECycle(void)
{
  if(!cfg.blescan) return;
  ESP_LOGV(TAG, "cycling ble scan");
  ESP_LOGV(TAG, "Set BLE inquiry mode");
  char buffer[512];
  BLESerial.println("AT+INQ");

  //ENTER INQ MODE
  ESP_LOGV(TAG, "start INQ mode ");
  buffer[0] = 0;
  int bytesRead = readResponse(&BLESerial, buffer, sizeof(buffer), 1000);
#ifdef BLE_RETURNS_OK_ON_INIT
  if(! assertResponse("OK", buffer, bytesRead))
  {
    ESP_LOGD(TAG, "Error setting BLE INQ mode");
    return;
  }
  bytesRead = readResponse(&BLESerial, buffer, sizeof(buffer), 1000);
#endif
  if(! assertResponse("+INQS\r", buffer, bytesRead))
  {
    ESP_LOGD(TAG, "Error setting BLE INQ mode");
    return;
  }
  delay(1000);
  buffer[0] = 0;
#ifdef BLE_RETURNS_SCANNING
  bytesRead = readResponse(&BLESerial, buffer, sizeof(buffer), 1000);
  if(! assertResponse("Scanning...", buffer, bytesRead))
  {
    ESP_LOGD(TAG, "Error setting BLE INQ scanning mode");
    return;
  }
#endif
  delay(5000);
  long start_time = millis();
  int devicesDetected = 0;
  while(bytesRead > 0 || (millis() - start_time < (BTLE_SCAN_TIME/2) * 1000) )
  {
    buffer[0] = 0;
    bytesRead = readResponse(&BLESerial, buffer, sizeof(buffer), 5000);
    if(bytesRead <= 0)
    {
      break;
    }
    ESP_LOGV(TAG, "Response: %s", buffer);
    if(assertResponse("+INQE\r", buffer, bytesRead))
    {
      ESP_LOGV(TAG, "finish INQ mode ");

      if(devicesDetected == 0) {
        ESP_LOGV(TAG, "No devices detected");
      } else {
        ESP_LOGV(TAG, "%d devices detected", devicesDetected);
      }
      return;
    }
    if(assertResponse("+INQ:", buffer, bytesRead))
    {
      devicesDetected += getMacsFromBLE(buffer, bytesRead);
    }
  }
  return;
}
#endif

bool reinitBT()
{
  if (!(
    sendAndReadOkResponse(&BTSerial, "AT") &&
    sendAndReadOkResponse(&BTSerial, "AT+ORGL")
    ))
    {
      ESP_LOGD(TAG, "Error initializing BT");
      return false;
    }
  delay(5000);
  if (!(
    sendAndReadOkResponse(&BTSerial,"AT+RMAAD") &&
    sendAndReadOkResponse(&BTSerial,"AT+ROLE=1")
  ))
  {
    ESP_LOGD(TAG, "Error initializing BT");
    return false;
  }
  sendAndReadOkResponse(&BTSerial,"AT+RESET");
  delay(5000);
  if (!(
    sendAndReadOkResponse(&BTSerial,"AT+CMODE=1") &&
#ifdef BT_REQUIRES_INIT
    sendAndReadOkResponse(&BTSerial,"AT+INIT") &&
#endif
    sendAndReadOkResponse(&BTSerial, "AT+INQM=1,10000,7")))
    {
      ESP_LOGD(TAG, "Error initializing BT");
      return false;
    }
  ESP_LOGD(TAG, "BT initialized as Master");
  return true;
}
bool initBT(long baud)
{
  pinMode(EN_BT, OUTPUT);  // this pin will pull the HC-05 pin 34 (key pin) HIGH to switch module to AT mode
  pinMode(BLEBTMUX_A, OUTPUT);
  digitalWrite(EN_BT, HIGH);
  //initBTSerial(baud);  // HC-05 default speed in AT command more
  setSerialToBT();
  ESP_LOGD(TAG, "Initialize BT inquiry mode at %d", baud);
  return reinitBT();
}

void BTCycle(long baud)
{
  if(!cfg.btscan) return;
  //initBTSerial(baud);
  ESP_LOGV(TAG, "cycling bt scan");

  long startTime = millis();
  BTSerial.print("AT+INQ\r\n");
  BTSerial.flush();
  char buffer[64];
  int bytesRead = 0;

  while (millis() - startTime < (BTLE_SCAN_TIME)*1000)
  {
    bytesRead = readResponse(&BTSerial, buffer, sizeof(buffer), 500);
    if(assertResponse("OK\r", buffer, bytesRead))
    {
      ESP_LOGV(TAG, "finish INQ mode");
      return;
    }
    if(assertResponse("+INQ:", buffer, bytesRead))
    {
      //ESP_LOGV(TAG, "Got message %s", buffer);
      getMacsFromBT(buffer, bytesRead);
    }
    delay(10);
  }
  ESP_LOGE(TAG, "Timeout while scanning BT");
  return;
}

void btHandler(void *pvParameters)
{
  pinMode(BLEBTMUX_A, OUTPUT);
  /*pinMode(BLEBTMUX_B, OUTPUT);
  digitalWrite(BLEBTMUX_B, LOW);*/
  delay(100);
  initBTSerial(BT_BAUD);
  bool btInitialized = initBT(BT_BAUD);
  delay(100);

#ifdef LEGACY_MODULE
  initBLESerial();
#endif
  bool bleInitialized = initBLE();
  delay(100);

  ESP_LOGV(TAG,"start loop");
  while(true)
  {
    setSerialToBT();
    if(btInitialized)
      BTCycle(BT_BAUD);
    else
      btInitialized = reinitBT();

    delay(5000);

    setSerialToBLE();
    if(bleInitialized)
      BLECycle();
    else
      bleInitialized = reinitBLE();
    delay(5000);
  }
}