/* code snippets taken from
https://github.com/nkolban/esp32-snippets/tree/master/BLE/scanner
*/

#include "blescan.h"

#define BT_BD_ADDR_HEX(addr)                                                   \
  addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]

// local Tag for logging
static const char TAG[] = "bluetooth";

int readResponse(HardwareSerial* port, char* buff, int b_size, uint32_t timeout=3000)
{
  port->setTimeout(timeout);
  int bytesRead = port->readBytesUntil('\n',buff, b_size);
  if(bytesRead > 0)
  {
    buff[bytesRead] = 0;
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

bool sendAndReadOkResponse(HardwareSerial* port, const char* command)
{
  ESP_LOGD(TAG, "Command: %s", command);
  port->println(command);
  port->flush();
  char buffer[64];
  int bytesRead = readResponse(port, buffer, sizeof(buffer));
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

  ESP_LOGD(TAG, "Parse input: MAC: %04X%02X%06X, BT Class: %06X, RSSI: %d", p1, p2, p3, btClass, rssi);

  uint8_t mac[6];
  mac[0] = (p1 >> 8) & 0xFF;
  mac[1] = (p1) & 0xFF;
  mac[2] = (p2) & 0xFF;
  mac[3] = (p3 >> 16) & 0xFF;
  mac[4] = (p3 >> 8) & 0xFF;
  mac[5] = (p3) & 0xFF;

  char tempBuffer[64];
  sprintf(tempBuffer, "Device MAC: ");
  for (int n = 0; n < 6; n++)
  {
    sprintf(tempBuffer + 2*n, "%02X",mac[n]);
  }
  ESP_LOGD(TAG, "MAC parsed: %s", tempBuffer);
  mac_add((uint8_t *)mac, rssi, MAC_SNIFF_BT);
  return 0;
}

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
    ESP_LOGD(TAG, "%s", tempBuffer);
    mac_add((uint8_t *)mac, 100, MAC_SNIFF_BLE);
  }
  while(BLESerial.available())
  {
    BLESerial.read();
  }
  return 0;
}

void initBLE()
{
  digitalWrite(EN_BLE, HIGH);
  pinMode(EN_BLE, OUTPUT);
  BLESerial.begin(9600, SERIAL_8N1, RX_BLE, TX_BLE);
  sendAndReadOkResponse(&BLESerial,"AT");
  BLESerial.println("AT+ROLE1");
  char buffer[64];
  int bytesRead = readResponse(&BLESerial, buffer, sizeof(buffer));
  if( !assertResponse("+ROLE=1\r", buffer, bytesRead) )
  {
    ESP_LOGE(TAG, "ERROR INITIALIZING BLE");
    return;
  }
  readResponse(&BLESerial, buffer, sizeof(buffer));
  if( !assertResponse("OK\r", buffer, bytesRead) )
  {
    ESP_LOGE(TAG,"ERROR INITIALIZING BLE");
    return;
  }
  ESP_LOGD(TAG, "BLE Initialized as Master");
  delay(2500);
}


void BLECycle(void)
{
  if(!cfg.blescan) return;
  ESP_LOGD(TAG, "cycling ble scan");
  ESP_LOGD(TAG, "Set BLE inquiry mode");
  char buffer[64];
  sendAndReadOkResponse(&BLESerial,"AT+INQ");
  int bytesRead = readResponse(&BLESerial, buffer, sizeof(buffer));
  if(! assertResponse("+INQS\r", buffer, bytesRead))
  {
    ESP_LOGE(TAG, "Error setting BLE INQ mode");
    return;
  }

  //ENTER INQ MODE
  ESP_LOGD(TAG, "start INQ mode ");
  long start_time = millis();
  while(millis() - start_time < (BTLE_SCAN_TIME/2) * 1000)
  {
    bytesRead = readResponse(&BLESerial, buffer, sizeof(buffer));
    if(assertResponse("+INQE\r", buffer, bytesRead))
    {
      ESP_LOGD(TAG, "finish INQ mode ");
      bytesRead = readResponse(&BLESerial, buffer, sizeof(buffer));
      int devicesDetected = getDetectedDevices(buffer, bytesRead);
      ESP_LOGD(TAG, "%d devices detected", devicesDetected);
      getMacsFromBLE(devicesDetected);
      break;
    }
  }
  return;
}

void initBT()
{
  pinMode(EN_BT, OUTPUT);  // this pin will pull the HC-05 pin 34 (key pin) HIGH to switch module to AT mode
  digitalWrite(EN_BT, HIGH);
  BTSerial.begin(38400, SERIAL_8N1, RX_BT, TX_BT);  // HC-05 default speed in AT command more
  ESP_LOGD(TAG, "Initialize BT inquiry mode");
  if (!(
    sendAndReadOkResponse(&BTSerial, "AT") &&
    sendAndReadOkResponse(&BTSerial,"AT+RMAAD") &&
    sendAndReadOkResponse(&BTSerial,"AT+ROLE=1") &&
    sendAndReadOkResponse(&BTSerial,"AT+RESET")))
    {
      ESP_LOGE(TAG, "Error initializing BT");
    }
  delay(2000);
  if (!(
    sendAndReadOkResponse(&BTSerial,"AT+CMODE=1") &&
    sendAndReadOkResponse(&BTSerial,"AT+INIT") &&
    sendAndReadOkResponse(&BTSerial, "AT+INQM=1,10000,7")))
    {
      ESP_LOGE(TAG, "Error initializing BT");
    }
  ESP_LOGD(TAG, "BT initialized as Master");
}

void BTCycle(void)
{
  if(!cfg.btscan) return;
  ESP_LOGD(TAG, "cycling bt scan");

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
      ESP_LOGD(TAG, "finish INQ mode");
      break;
    }
    if(assertResponse("+INQ:", buffer, bytesRead))
    {
      ESP_LOGD(TAG, "Got message %s", buffer);
      getMacsFromBT(buffer, bytesRead);
    }
    delay(10);
  }
  return;
}

void btHandler(void *pvParameters)
{
  delay(500);
  initBT();
  delay(500);
  initBLE();
  while(true)
  {
    BTCycle();
    delay(5000);
    BLECycle();
    delay(5000);
  }
}