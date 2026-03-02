/* code snippets taken from
https://github.com/nkolban/esp32-snippets/tree/master/BLE/scanner
*/

#include "blescan.h"

#define BT_BD_ADDR_HEX(addr)                                                   \
  addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]

// local Tag for logging
static const char TAG[] = "bluetooth";

// Estado de los módulos BT y BLE para health check
bool bt_module_ok = false;
bool ble_module_ok = false;

// >>> CAMBIO: Contadores de fallos consecutivos de escaneo.
// Cuando un módulo que YA estaba inicializado falla al escanear
// (timeout en BTCycle o error AT en BLECycle), se incrementa el contador.
// Si llega a MAX_CONSECUTIVE_SCAN_FAILS, se marca el módulo como muerto
// y se fuerza reinicialización en el siguiente ciclo.
// Se resetea a 0 cuando el escaneo tiene éxito.
uint8_t bt_consecutive_fails = 0;
uint8_t ble_consecutive_fails = 0;

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

  uint8_t mac[6];
  mac[0] = (p1 >> 8) & 0xFF;
  mac[1] = (p1) & 0xFF;
  mac[2] = (p2) & 0xFF;
  mac[3] = (p3 >> 16) & 0xFF;
  mac[4] = (p3 >> 8) & 0xFF;
  mac[5] = (p3) & 0xFF;

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
  digitalWrite(BLEBTMUX_A, LOW);
  delay(100);
}

void setSerialToBLE() {
  BLESerial.flush();
  delay(100);
  BLESerial.updateBaudRate(9600);
  digitalWrite(BLEBTMUX_A, HIGH);
  delay(100);
}

bool initBLE()
{
  pinMode(EN_BLE, OUTPUT);
  pinMode(BLEBTMUX_A, OUTPUT);
  digitalWrite(EN_BLE, HIGH);
  setSerialToBLE();
  return reinitBLE();
}

// =====================================================================
// >>> CAMBIO: BLECycle ahora retorna bool
//
// Flujo de retorno:
//   - !cfg.blescan         → return true  (scan deshabilitado, no es fallo)
//   - Error en AT+INQ      → return false (módulo no respondió)
//   - Error en +INQS       → return false (módulo no entró en inquiry)
//   - Error en Scanning... → return false (módulo no comenzó scan)
//   - Recibe +INQE         → return true  (scan completó normalmente)
//   - Timeout/bytesRead<=0 → return true  (terminó sin error explícito)
//   - Recibe +INQ: (MACs)  → sigue leyendo, al final return true
// =====================================================================

#ifdef OLD_BLE_METHOD
bool BLECycle(void)
{
  if(!cfg.blescan) return true; // >>> CAMBIO: scan deshabilitado no es fallo
  ESP_LOGV(TAG, "cycling ble scan");
  ESP_LOGV(TAG, "Set BLE inquiry mode");
  char buffer[64];
  BLESerial.println("AT+INQ");
  int bytesRead = readResponse(&BLESerial, buffer, sizeof(buffer));
  if(! assertResponse("+INQS\r", buffer, bytesRead))
  {
    ESP_LOGD(TAG, "Error setting BLE INQ mode");
    return false; // >>> CAMBIO: fallo de comunicación con módulo
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
      return true; // >>> CAMBIO: scan completó OK
    }
  }
  // >>> CAMBIO: timeout sin recibir +INQE — se considera éxito parcial
  // (el módulo respondió al AT+INQ con +INQS, solo no terminó a tiempo)
  return true;
}
#else
bool BLECycle(void)
{
  if(!cfg.blescan) return true; // >>> CAMBIO: scan deshabilitado no es fallo
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
    return false; // >>> CAMBIO: módulo no respondió OK
  }
  bytesRead = readResponse(&BLESerial, buffer, sizeof(buffer), 1000);
#endif
  if(! assertResponse("+INQS\r", buffer, bytesRead))
  {
    ESP_LOGD(TAG, "Error setting BLE INQ mode");
    return false; // >>> CAMBIO: módulo no entró en inquiry mode
  }
  delay(1000);
  buffer[0] = 0;
#ifdef BLE_RETURNS_SCANNING
  bytesRead = readResponse(&BLESerial, buffer, sizeof(buffer), 1000);
  if(! assertResponse("Scanning...", buffer, bytesRead))
  {
    ESP_LOGD(TAG, "Error setting BLE INQ scanning mode");
    return false; // >>> CAMBIO: módulo no inició scanning
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
      return true; // >>> CAMBIO: scan terminó con +INQE
    }
    if(assertResponse("+INQ:", buffer, bytesRead))
    {
      devicesDetected += getMacsFromBLE(buffer, bytesRead);
    }
  }
  // >>> CAMBIO: salió del while sin error explícito → módulo respondió, scan OK
  return true;
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
  pinMode(EN_BT, OUTPUT);
  pinMode(BLEBTMUX_A, OUTPUT);
  digitalWrite(EN_BT, HIGH);
  setSerialToBT();
  ESP_LOGD(TAG, "Initialize BT inquiry mode at %d", baud);
  return reinitBT();
}

// =====================================================================
// >>> CAMBIO: BTCycle ahora retorna bool
//
// Flujo de retorno:
//   - !cfg.btscan          → return true  (scan deshabilitado, no es fallo)
//   - Recibe "OK\r"        → return true  (scan completó normalmente)
//   - Recibe "+INQ:" (MAC) → procesa MAC, sigue leyendo
//   - Timeout              → return false (módulo no respondió a tiempo)
// =====================================================================
bool BTCycle(long baud)
{
  if(!cfg.btscan) return true; // >>> CAMBIO: scan deshabilitado no es fallo
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
      return true; // >>> CAMBIO: scan completó OK
    }
    if(assertResponse("+INQ:", buffer, bytesRead))
    {
      getMacsFromBT(buffer, bytesRead);
    }
    delay(10);
  }
  ESP_LOGE(TAG, "Timeout while scanning BT");
  return false; // >>> CAMBIO: timeout = fallo de comunicación
}

// =====================================================================
// >>> CAMBIO: btHandler con detección de degradación
//
// LÓGICA:
// Antes:  Si el módulo inicializó OK, se asume OK para siempre.
//         Si BTCycle/BLECycle fallan, nadie se entera.
//
// Ahora:  Cada vez que BTCycle/BLECycle retorna false, se incrementa
//         un contador de fallos consecutivos. Si llega a
//         MAX_CONSECUTIVE_SCAN_FAILS (3), se marca el módulo como
//         muerto (bt_module_ok = false) y se fuerza reinicialización.
//         Si el escaneo tiene éxito, el contador se resetea a 0.
//
// EJEMPLO DE CORRIDA:
//
// Ciclo 1: BTCycle() → true  → bt_consecutive_fails = 0, bt_module_ok = true
// Ciclo 2: BTCycle() → true  → bt_consecutive_fails = 0, bt_module_ok = true
// Ciclo 3: BTCycle() → false → bt_consecutive_fails = 1, bt_module_ok = true  (aún OK)
// Ciclo 4: BTCycle() → false → bt_consecutive_fails = 2, bt_module_ok = true  (aún OK)
// Ciclo 5: BTCycle() → false → bt_consecutive_fails = 3, bt_module_ok = false ← DETECTADO
//          → btInitialized = false → siguiente ciclo intentará reinitBT()
// Ciclo 6: reinitBT() → true → bt_module_ok = true, bt_consecutive_fails = 0
//          (módulo recuperado)
// Ciclo 6: reinitBT() → false → bt_module_ok = false
//          (módulo sigue muerto, se reintentará en el siguiente ciclo)
//
// En el health check (flags3 bits 0-1), estos valores reflejan el estado
// REAL del módulo, no solo el estado del init inicial.
// =====================================================================
void btHandler(void *pvParameters)
{
  pinMode(BLEBTMUX_A, OUTPUT);
  delay(100);
  initBTSerial(BT_BAUD);
  bool btInitialized = initBT(BT_BAUD);
  bt_module_ok = btInitialized;
  bt_consecutive_fails = 0; // >>> CAMBIO: inicializar contador
  delay(100);

#ifdef LEGACY_MODULE
  initBLESerial();
#endif
  bool bleInitialized = initBLE();
  ble_module_ok = bleInitialized;
  ble_consecutive_fails = 0; // >>> CAMBIO: inicializar contador
  delay(100);

  ESP_LOGV(TAG,"start loop");
  while(true)
  {
    // ======================== BT ========================
    setSerialToBT();
    if(btInitialized)
    {
      // >>> CAMBIO: Evaluar resultado del escaneo
      bool scanOk = BTCycle(BT_BAUD);
      if(scanOk) {
        // Escaneo exitoso → resetear contador, confirmar módulo OK
        bt_consecutive_fails = 0;
        bt_module_ok = true;
      } else {
        // Escaneo falló → incrementar contador
        bt_consecutive_fails++;
        ESP_LOGW(TAG, "BT scan failed (%d/%d consecutive)",
                 bt_consecutive_fails, MAX_CONSECUTIVE_SCAN_FAILS);
        if(bt_consecutive_fails >= MAX_CONSECUTIVE_SCAN_FAILS) {
          // Demasiados fallos seguidos → módulo considerado muerto
          ESP_LOGE(TAG, "BT module degraded after %d consecutive scan failures, forcing reinit",
                   bt_consecutive_fails);
          btInitialized = false;
          bt_module_ok = false;
          bt_consecutive_fails = 0; // resetear para el ciclo de reinit
        }
      }
    }
    else {
      // Módulo no inicializado → intentar reinicializar
      btInitialized = reinitBT();
      bt_module_ok = btInitialized;
      if(btInitialized) {
        bt_consecutive_fails = 0;
        ESP_LOGI(TAG, "BT module recovered after reinit");
      }
    }

    delay(5000);

    // ======================== BLE ========================
    setSerialToBLE();
    if(bleInitialized)
    {
      // >>> CAMBIO: Evaluar resultado del escaneo
      bool scanOk = BLECycle();
      if(scanOk) {
        // Escaneo exitoso → resetear contador, confirmar módulo OK
        ble_consecutive_fails = 0;
        ble_module_ok = true;
      } else {
        // Escaneo falló → incrementar contador
        ble_consecutive_fails++;
        ESP_LOGW(TAG, "BLE scan failed (%d/%d consecutive)",
                 ble_consecutive_fails, MAX_CONSECUTIVE_SCAN_FAILS);
        if(ble_consecutive_fails >= MAX_CONSECUTIVE_SCAN_FAILS) {
          // Demasiados fallos seguidos → módulo considerado muerto
          ESP_LOGE(TAG, "BLE module degraded after %d consecutive scan failures, forcing reinit",
                   ble_consecutive_fails);
          bleInitialized = false;
          ble_module_ok = false;
          ble_consecutive_fails = 0; // resetear para el ciclo de reinit
        }
      }
    }
    else {
      // Módulo no inicializado → intentar reinicializar
      bleInitialized = reinitBLE();
      ble_module_ok = bleInitialized;
      if(bleInitialized) {
        ble_consecutive_fails = 0;
        ESP_LOGI(TAG, "BLE module recovered after reinit");
      }
    }

    delay(5000);
  }
}