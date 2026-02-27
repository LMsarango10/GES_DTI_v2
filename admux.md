# Implementación ADEMUX — Failover Bidireccional LoRa ↔ NB-IoT

**Proyecto:** Paxcounter (proyecto nuevo, sin ADEMUX)  
**Objetivo:** Implementar health checks duales + failover automático bidireccional  
**Fecha:** 27 de febrero de 2026

---

## Índice

1. [Resumen ejecutivo](#1-resumen-ejecutivo)
2. [Estado actual por archivo](#2-estado-actual-por-archivo)
3. [Cambios necesarios por archivo](#3-cambios-necesarios-por-archivo)
4. [Flujo ADEMUX completo](#4-flujo-ademux-completo)
5. [Payload de telemetría (17 bytes)](#5-payload-de-telemetría-17-bytes)
6. [Orden de implementación](#6-orden-de-implementación)

---

## 1. Resumen ejecutivo

### ¿Qué es ADEMUX?

Sistema de failover automático bidireccional que:

- Envía **health checks** como confirmed uplinks por LoRa cada 2 minutos
- Envía **health checks** independientes por NB-IoT cada 1 minuto
- Cuando LoRa no responde (N health checks sin ACK) → datos cambian a NB-IoT
- Los health checks **siguen por LoRa** para detectar recuperación
- Cuando LoRa responde de nuevo → datos vuelven a LoRa
- NB-IoT está **siempre activo** (conectado al broker MQTT), no se enciende/apaga

### ¿Qué hay actualmente?

Failover reactivo básico: todo va por LoRa → si cola llena o busy → NB temporal → SD. Sin health checks, sin detección proactiva de pérdida de LoRa, sin telemetría del dispositivo.

### ¿Qué falta?

| Componente | Estado |
|---|---|
| Health checks duales | ❌ No existe |
| Payload telemetría 17 bytes | ❌ No existe |
| Flag `nb_data_mode` para routing | ❌ No existe |
| Confirmed uplink para health check | ❌ No existe |
| Conteo de fallos health check | ❌ No existe |
| Detección de recuperación LoRa | ❌ No existe |
| NB-IoT siempre activo | ❌ Solo se activa cuando `enabled=true` |
| Variables estado NB para telemetría | ❌ No existe |

---

## 2. Estado actual por archivo

### 2.1 `paxcounter.conf`

**Lo que hay:**
- Puertos definidos: COUNTERPORT=1, STATUSPORT=2, etc.
- `SEND_QUEUE_SIZE=500`
- `COUNTERMODE=0` (cyclic, unconfirmed)
- Constantes NB-IoT básicas en `nbiot.h` (no en conf)

**Lo que NO hay:**
- `TELEMETRYPORT` — no existe puerto dedicado para health check
- `MAX_HEALTHCHECK_FAILURES` — no existe threshold
- `HEALTHCHECK_INTERVAL_MINUTES` — no existe intervalo LoRa
- `NB_HEALTHCHECK_INTERVAL_MINUTES` — no existe intervalo NB

---

### 2.2 `lorawan.h`

**Lo que hay:**
```cpp
extern unsigned long lastConfirmedSendTime;
// Funciones: lora_stack_init, lora_enqueuedata, lora_queuereset, etc.
```

**Lo que NO hay:**
```cpp
// Estas variables NO existen:
extern uint8_t healthcheck_failures;
extern bool healthcheck_pending;
extern bool nb_data_mode;
```

---

### 2.3 `lorawan.cpp`

#### 2.3.1 Variables globales

**Lo que hay:**
```cpp
unsigned long lastJoinAttemptTime = 0;
unsigned long lastConfirmedSendTime = 0;
unsigned long joinStartedTime = 0;
bool firstJoin = true;
```

**Lo que NO hay:**
```cpp
// Estas variables NO existen:
uint8_t healthcheck_failures = 0;
bool healthcheck_pending = false;
bool nb_data_mode = false;
```

#### 2.3.2 `lora_send()` — Lógica de confirmed uplink

**Lo que hay (líneas relevantes):**
```cpp
#ifdef CONFIRMED_SEND_THRESHOLD
    bool sendConfirmed =
        (millis() - lastConfirmedSendTime) >
        (CONFIRMED_SEND_THRESHOLD * 60UL * 1000UL);
#else
    bool sendConfirmed = false;
#endif

    bool confirmedNow = ((cfg.countermode & 0x02) != 0) && sendConfirmed;
```

**Problema:** No distingue health checks de datos normales. Usa `CONFIRMED_SEND_THRESHOLD` genérico. No hay flag `healthcheck_pending`. No fuerza confirmed para TELEMETRYPORT.

**Lo que DEBERÍA hacer:**
```
Si puerto == TELEMETRYPORT:
    sendConfirmed = true (siempre)
    healthcheck_pending = true (para rastrear el ACK)
Sino:
    lógica actual de CONFIRMED_SEND_THRESHOLD
```

#### 2.3.3 `myEventCallback()` — EV_TXCOMPLETE

**Lo que hay:**
```cpp
case EV_TXCOMPLETE:
    RTCseqnoUp = LMIC.seqnoUp;
    RTCseqnoDn = LMIC.seqnoDn;
    if (LMIC.txrxFlags & TXRX_ACK) {
        ESP_LOGI(TAG, "Received ack");
#if (HAS_NBIOT)
        if (nb_isEnabled()) nb_disable();  // ← desactiva NB al recibir ACK
#endif
    }
    break;
```

**Problemas:**
1. No detecta health checks fallidos (sin ACK cuando healthcheck_pending)
2. No cuenta `healthcheck_failures`
3. No activa `nb_data_mode` cuando supera threshold
4. No detecta recuperación LoRa (ACK después de estar en failover)
5. Llama `nb_disable()` que vacía la cola NB → incompatible con NB siempre activo

**Lo que DEBERÍA hacer:**
```
Si ACK recibido:
    Si nb_data_mode estaba activo:
        Log "LoRa recovered!"
        nb_data_mode = false  (datos vuelven a LoRa)
    healthcheck_failures = 0
    healthcheck_pending = false

Sino si healthcheck_pending:
    healthcheck_failures++
    healthcheck_pending = false
    Si healthcheck_failures >= MAX_HEALTHCHECK_FAILURES y no en failover:
        nb_data_mode = true  (datos van a NB)
        Log "Activating failover to NB-IoT"
```

#### 2.3.4 `myEventCallback()` — EV_JOINED

**Lo que hay:**
```cpp
case EV_JOINED:
    lora_setupForNetwork(false);
#if (HAS_NBIOT)
    firstJoin = false;
    nb_disable();  // ← PROBLEMA: desactiva NB-IoT
#endif
    break;
```

**Problema:** `nb_disable()` apaga NB-IoT y mueve mensajes NB → LoRa. Con ADEMUX, NB debe estar **siempre activo**.

**Lo que DEBERÍA hacer:**
```cpp
case EV_JOINED:
    lora_setupForNetwork(false);
#if (HAS_NBIOT)
    firstJoin = false;
    // NB-IoT se mantiene siempre activo — no llamar nb_disable()
    if (nb_data_mode) {
        nb_data_mode = false;  // datos vuelven a LoRa
        healthcheck_failures = 0;
    }
#endif
    break;
```

#### 2.3.5 `myEventCallback()` — EV_LINK_DEAD

**Lo que hay:**
```cpp
case EV_LINK_DEAD:
    nb_enable(false);  // activa NB permanente + vacía cola LoRa
    LMIC_startJoining();
    break;
```

**Problema:** `nb_enable(false)` vacía la cola LoRa (incluidos health checks pendientes).

**Lo que DEBERÍA hacer:**
```cpp
case EV_LINK_DEAD:
    if (!nb_data_mode) {
        nb_data_mode = true;  // solo cambiar routing, no vaciar colas
    }
    LMIC_startJoining();
    break;
```

---

### 2.4 `nbiot.cpp`

#### 2.4.1 Variables globales

**Lo que hay:**
```cpp
bool nbIotEnabled = false;
bool nbTransportAvailable = true;
```

**Lo que NO hay:**
```cpp
// Variables de estado para telemetría — NO existen:
uint8_t nb_status_registered = 0;
uint8_t nb_status_connected = 0;
uint8_t nb_status_failures = 0;
uint8_t nb_status_rssi = 99;  // 99 = desconocido
```

#### 2.4.2 `loop()` — NB-IoT condicionado a `enabled`

**Lo que hay:**
```cpp
if (shouldCheckForUpdates || this->enabled) {
    if (!this->registered) {
        this->nb_registerNetwork();
        return;
    }
    if (!this->connected) {
        this->nb_connectNetwork();
        return;
    }
    if (this->enabled) {
        if (!this->mqttConnected) { ... }
        if (!this->subscribed) { ... }
    }
    if (!this->nb_checkStatus()) { ... }
}
// ...
if (this->enabled) {
    this->nb_readMessages();
    this->nb_sendMessages();
    this->consecutiveFailures = 0;
}
```

**Problema:** NB-IoT solo registra, conecta, y envía MQTT cuando `enabled=true`. Con ADEMUX, NB debe estar **siempre conectado** para enviar health checks independientemente del estado de LoRa.

**Lo que DEBERÍA hacer:**
```
// Siempre mantener conexión activa (sin depender de enabled):
Si no registrado → registrar
Si no conectado → conectar
Si no MQTT → conectar MQTT
Si no suscrito → suscribir
Verificar estado

// Enviar mensajes siempre que haya algo en cola:
Si hay mensajes en NbSendQueue → enviar
```

#### 2.4.3 `nb_sendMessages()` — Desactiva NB en modo temporal

**Lo que hay:**
```cpp
if (this->temporaryEnabled && uxQueueMessagesWaiting(NbSendQueue) == 0) {
    this->temporaryEnabled = false;
    nb_disable();  // ← PROBLEMA: apaga NB
}
```

**Lo que DEBERÍA hacer:**
```cpp
if (this->temporaryEnabled && uxQueueMessagesWaiting(NbSendQueue) == 0) {
    this->temporaryEnabled = false;
    // No desactivar NB-IoT, solo quitar modo temporal
}
```

#### 2.4.4 Funciones de estado — No exportan estado

**Lo que hay:** Las funciones `nb_registerNetwork()`, `nb_connectNetwork()`, `nb_checkStatus()`, `nb_resetStatus()` actualizan variables internas de la clase pero **no exportan a variables globales**.

**Lo que DEBERÍA hacer:** Cada función actualiza también las variables `nb_status_*` globales para que el health check las incluya en el payload.

#### 2.4.5 `nb_init()` — Lee config de SD

**Lo que hay:**
```cpp
void NbIotManager::nb_init() {
    sdLoadNbConfig(&nbConfig);
    if (strlen(nbConfig.ServerAddress) < 5) {
        ESP_LOGE(TAG, "Error in NB config, cant send");
        this->initializeFailures++;
        return;
    }
    // ...
}
```

**Opción a evaluar:** Hardcodear la config del servidor NB-IoT para eliminar dependencia de SD (como se hizo en el otro proyecto). Esto evita problemas de timing al arranque cuando el archivo `nb.cnf` ya existe y `nb_init()` se ejecuta demasiado rápido para el modem.

---

### 2.5 `nbiot.h`

**Lo que hay:**
```cpp
extern TaskHandle_t nbIotTask;
bool nb_enqueuedata(MessageBuffer_t *message);
void nb_queuereset(void);
void nb_enable(bool temporary);
void nb_disable(void);
bool nb_isEnabled(void);
esp_err_t nb_iot_init();
```

**Lo que NO hay:**
```cpp
// Declaraciones extern para variables de estado — NO existen:
extern uint8_t nb_status_registered;
extern uint8_t nb_status_connected;
extern uint8_t nb_status_failures;
extern uint8_t nb_status_rssi;
```

---

### 2.6 `senddata.cpp`

#### 2.6.1 Includes

**Lo que hay:**
```cpp
#include "senddata.h"
```

**Lo que NO hay:**
```cpp
#include "blescan.h"    // Para bt_module_ok, ble_module_ok
#include "nbiot.h"      // Para nb_status_registered, etc.
#include <esp_system.h> // Para esp_reset_reason()
#include "lorawan.h"    // Para nb_data_mode
```

#### 2.6.2 Variables globales

**Lo que hay:**
```cpp
Ticker sendcycler;
bool sent = false;
```

**Lo que NO hay:**
```cpp
uint8_t lastSendChannel = 0;  // 0=ninguno, 1=LoRa, 2=NB-IoT, 3=SD
```

#### 2.6.3 `SendPayload()` — Routing básico sin ADEMUX

**Lo que hay:**
```cpp
#if (HAS_LORA)
  bool enqueued = lora_enqueuedata(&SendBuffer);
  if (!enqueued) {
    // Cola llena → NB temporal → SD
    nb_enable(true);
    if (!nb_enqueuedata(&SendBuffer)) {
      // SD como último recurso
    }
  }
#endif
```

**Problema:** Todo va siempre por LoRa. No distingue health checks de datos. No usa `nb_data_mode` para routing. Cuando LoRa está "ok" (transmite pero nadie recibe), sigue enviando al vacío.

**Lo que DEBERÍA hacer:**
```
Si puerto == TELEMETRYPORT:
    Siempre por LoRa (confirmed) — para detectar recuperación
Sino si nb_data_mode == false:
    Por LoRa (normal)
    Si cola llena → SD (no NB temporal, porque NB ya está encolando health checks)
Sino (nb_data_mode == true):
    Por NB-IoT
    Si cola NB llena → SD
```

#### 2.6.4 `sendData()` — Sin health checks

**Lo que hay:** Solo construye payloads de contadores, MACs, BME, GPS, sensores, batería.

**Lo que NO hay:**
- Health check LoRa (cada 2 min) con payload 17 bytes en TELEMETRYPORT
- Health check NB-IoT independiente (cada 1 min) con mismo payload
- Recopilación de telemetría (uptime, temp CPU, heap, flags, RSSI, SNR, estado NB)

#### 2.6.5 `checkQueue()` — Usa `nb_isEnabled()`

**Lo que hay:**
```cpp
if (nb_isEnabled() && loraMessages >= MIN_SEND_MESSAGES_THRESHOLD) {
    // Drena cola LoRa → NB
}
```

**Lo que DEBERÍA hacer:** Usar `nb_data_mode` en vez de `nb_isEnabled()` para ser coherente con el nuevo routing.

---

### 2.7 `payload.cpp` / `payload.h` (no proporcionados aún)

**Lo que hay:** Función `addStatus()` con firma antigua (voltage, uptime uint64, cputemp float, mem uint32, reset1, reset2).

**Lo que DEBERÍA hacer:** Nueva firma con 17 bytes:
```cpp
void addStatus(uint32_t uptime, uint8_t cputemp,
               uint16_t free_heap_div16, uint16_t min_heap_div16,
               uint8_t reset_reason, uint8_t flags1, uint8_t flags2,
               uint8_t lora_rssi, int8_t lora_snr,
               uint8_t nb_rssi, uint8_t nb_failures,
               uint8_t flags3);
```

---

### 2.8 `blescan.cpp` / `blescan.h` (no proporcionados aún)

**Lo que DEBERÍA tener:** Variables globales `bt_module_ok` y `ble_module_ok` actualizadas en el handler BT/BLE, con `extern` en el header.

---

### 2.9 `rcommand.cpp` (no proporcionado aún)

**Lo que hay:** Función `get_status()` con firma antigua.

**Lo que DEBERÍA hacer:** Actualizar a la nueva firma de `addStatus()`.

---

## 3. Cambios necesarios por archivo

### Archivo 1: `paxcounter.conf`

| Cambio | Detalle |
|---|---|
| AÑADIR | `#define TELEMETRYPORT 14` |
| AÑADIR | `#define MAX_HEALTHCHECK_FAILURES 2` |
| AÑADIR | `#define HEALTHCHECK_INTERVAL_MINUTES 2` |
| AÑADIR | `#define NB_HEALTHCHECK_INTERVAL_MINUTES 1` |

---

### Archivo 2: `lorawan.h`

| Cambio | Detalle |
|---|---|
| AÑADIR | `extern uint8_t healthcheck_failures;` |
| AÑADIR | `extern bool healthcheck_pending;` |
| AÑADIR | `extern bool nb_data_mode;` |

---

### Archivo 3: `lorawan.cpp`

| Zona | Cambio | Detalle |
|---|---|---|
| Variables globales | AÑADIR | `uint8_t healthcheck_failures = 0;` |
| Variables globales | AÑADIR | `bool healthcheck_pending = false;` |
| Variables globales | AÑADIR | `bool nb_data_mode = false;` |
| `lora_send()` | MODIFICAR | Bloque `sendConfirmed`: forzar confirmed + healthcheck_pending cuando puerto=TELEMETRYPORT |
| `myEventCallback()` EV_TXCOMPLETE | REEMPLAZAR | Añadir lógica de conteo de fallos y activación/desactivación de `nb_data_mode` |
| `myEventCallback()` EV_JOINED | MODIFICAR | Quitar `nb_disable()`, solo resetear `nb_data_mode` |
| `myEventCallback()` EV_LINK_DEAD | MODIFICAR | Usar `nb_data_mode=true` en vez de `nb_enable(false)` |

---

### Archivo 4: `nbiot.h`

| Cambio | Detalle |
|---|---|
| AÑADIR | `extern uint8_t nb_status_registered;` |
| AÑADIR | `extern uint8_t nb_status_connected;` |
| AÑADIR | `extern uint8_t nb_status_failures;` |
| AÑADIR | `extern uint8_t nb_status_rssi;` |

---

### Archivo 5: `nbiot.cpp`

| Zona | Cambio | Detalle |
|---|---|---|
| Variables globales | AÑADIR | `nb_status_registered`, `nb_status_connected`, `nb_status_failures`, `nb_status_rssi` |
| `loop()` | MODIFICAR | NB siempre activo: quitar condicional `if (enabled)` para registro/conexión/MQTT |
| `loop()` final | MODIFICAR | Enviar mensajes siempre que haya cola, no solo cuando `enabled` |
| `nb_sendMessages()` | MODIFICAR | No llamar `nb_disable()` en modo temporal |
| `nb_registerNetwork()` | AÑADIR | Actualizar `nb_status_registered` |
| `nb_connectNetwork()` | AÑADIR | Actualizar `nb_status_connected` |
| `nb_checkStatus()` | AÑADIR | Actualizar `nb_status_registered/connected` en cada check |
| `nb_resetStatus()` | AÑADIR | Resetear variables `nb_status_*` |
| `loop()` inicio | AÑADIR | `nb_status_failures = consecutiveFailures` |
| `nb_init()` | EVALUAR | Considerar hardcodear config servidor (eliminar dependencia SD) |

---

### Archivo 6: `senddata.cpp`

| Zona | Cambio | Detalle |
|---|---|---|
| Includes | AÑADIR | `blescan.h`, `nbiot.h`, `esp_system.h`, `lorawan.h` |
| Variables globales | AÑADIR | `uint8_t lastSendChannel = 0;` |
| `SendPayload()` | REEMPLAZAR | Routing ADEMUX: TELEMETRY→LoRa, datos→según `nb_data_mode` |
| `sendData()` final | AÑADIR | Health check LoRa cada 2 min (payload 17 bytes + SendPayload TELEMETRYPORT) |
| `sendData()` final | AÑADIR | Health check NB cada 1 min (payload 17 bytes + nb_enqueuedata directo) |
| `checkQueue()` | MODIFICAR | Usar `nb_data_mode` en vez de `nb_isEnabled()` |

---

### Archivo 7: `payload.h`

| Cambio | Detalle |
|---|---|
| MODIFICAR | Cambiar firma de `addStatus()` a 12 parámetros (17 bytes) |

---

### Archivo 8: `payload.cpp`

| Cambio | Detalle |
|---|---|
| REEMPLAZAR | Cuerpo de `addStatus()` en encoder 1 (plain), encoder 2 (packed), encoder 3/4 (Cayenne) |

---

### Archivo 9: `blescan.cpp`

| Cambio | Detalle |
|---|---|
| AÑADIR | Variables globales `bool bt_module_ok = false;` y `bool ble_module_ok = false;` |
| MODIFICAR | `btHandler()`: actualizar variables tras init y reinit |

---

### Archivo 10: `rcommand.cpp`

| Cambio | Detalle |
|---|---|
| AÑADIR | Includes: `blescan.h`, `nbiot.h`, `esp_system.h` |
| REEMPLAZAR | Función `get_status()` con nueva firma de `addStatus()` |

---

### Archivo 11: Decoder ChirpStack (JavaScript)

| Cambio | Detalle |
|---|---|
| REEMPLAZAR | Bloque `if (fPort === 14)` con decoder de 17 bytes |

---

## 4. Flujo ADEMUX completo

### 4.1 Detección de pérdida de LoRa

```
sendData() cada SENDCYCLE segundos
    │
    ├── Envía contadores, MACs, etc. por LoRa (o NB si nb_data_mode)
    │
    └── Cada 2 min: Health check → SendPayload(TELEMETRYPORT)
                        │
                        ▼
                    SendPayload() detecta TELEMETRYPORT
                        │
                        ▼
                    lora_enqueuedata() → cola LoRa
                        │
                        ▼
                    lora_send() detecta TELEMETRYPORT
                        │
                        ▼
                    sendConfirmed = true
                    healthcheck_pending = true
                        │
                        ▼
                    LMIC_sendWithCallback() (confirmed)
                        │
                        ▼
                    EV_TXCOMPLETE
                    ┌───────┴───────┐
                    │               │
                ACK recibido    Sin ACK (healthcheck_pending=true)
                    │               │
                failures=0      failures++
                pending=false   pending=false
                    │               │
                Si nb_data_mode:    ¿failures >= MAX?
                  nb_data_mode=false    │
                  "LoRa recovered!"     ▼
                                    nb_data_mode = true
                                    "Activating failover"
```

### 4.2 Routing con `nb_data_mode`

```
SendPayload(port, prio)
    │
    ├── port == TELEMETRYPORT?
    │       SÍ → lora_enqueuedata() (siempre LoRa, confirmed)
    │
    ├── nb_data_mode == false?
    │       SÍ → lora_enqueuedata() (LoRa normal)
    │             Si cola llena → SD
    │
    └── nb_data_mode == true?
            SÍ → nb_enqueuedata() (NB-IoT)
                  Si cola llena → SD
```

### 4.3 Recuperación automática

```
nb_data_mode = true (datos por NB)
    │
    ├── Health checks SIGUEN por LoRa (confirmed) cada 2 min
    │
    ├── Cada health check: EV_TXCOMPLETE
    │       ├── Sin ACK → sigue en nb_data_mode
    │       └── ACK → nb_data_mode = false → "LoRa recovered!"
    │
    └── EV_JOINED (si se perdió sesión):
            nb_data_mode = false → datos vuelven a LoRa
```

### 4.4 Health checks NB-IoT independientes

```
sendData() cada SENDCYCLE
    │
    └── Cada 1 min: construye payload 17 bytes
                        │
                        ▼
                    nb_enqueuedata() directo (no pasa por SendPayload)
                        │
                        ▼
                    NbIotManager::loop() envía por MQTT
                        │
                        ▼
                    Broker MQTT recibe en:
                    DIVALGATE/application/1/device/[DevEUI]/rx
```

---

## 5. Payload de telemetría (17 bytes)

### Estructura

| Offset | Bytes | Campo | Codificación |
|--------|-------|-------|-------------|
| 0-3 | 4 | uptime | uint32 big-endian, segundos |
| 4 | 1 | cputemp | uint8, °C redondeado |
| 5-6 | 2 | free_heap | uint16 BE, valor real ÷ 16 |
| 7-8 | 2 | min_free_heap | uint16 BE, valor real ÷ 16 |
| 9 | 1 | reset_reason | uint8, esp_reset_reason() |
| 10 | 1 | flags1 | bitmap módulos |
| 11 | 1 | flags2 | healthcheck_failures LoRa |
| 12 | 1 | lora_rssi | uint8, valor absoluto |
| 13 | 1 | lora_snr | int8 |
| 14 | 1 | nb_rssi | uint8, CSQ (99=desconocido) |
| 15 | 1 | nb_failures | uint8, consecutiveFailures NB |
| 16 | 1 | flags3 | bitmap estado + canal |

### flags1 (offset 10)

| Bit | Significado |
|-----|-------------|
| 7 | WiFi scan configurado |
| 6 | BLE scan configurado |
| 5 | BT scan configurado |
| 4 | LoRa joined (LMIC.devaddr != 0) |
| 3 | NB-IoT habilitado |
| 2 | SD card presente |
| 1 | GPS fix |
| 0 | Reservado (WiFi init OK) |

### flags3 (offset 16)

| Bit | Significado |
|-----|-------------|
| 7 | NB-IoT registered |
| 6 | NB-IoT connected |
| 5-4 | CPU freq: 00=80, 01=160, 10=240 MHz |
| 3-2 | lastSendChannel: 00=ninguno, 01=LoRa, 10=NB-IoT, 11=SD |
| 1 | BT módulo init OK |
| 0 | BLE módulo init OK |

### reset_reason (offset 9)

| Valor | Significado |
|-------|-------------|
| 1 | POWERON |
| 2 | EXT_RESET |
| 3 | SW_RESET |
| 4 | PANIC |
| 5 | INT_WDT |
| 6 | TASK_WDT |
| 7 | WDT |
| 8 | DEEPSLEEP |
| 9 | BROWNOUT |
| 10 | SDIO |

---

## 6. Orden de implementación

### Fase 1: Infraestructura (sin cambiar comportamiento)

1. `paxcounter.conf` — Añadir constantes
2. `lorawan.h` — Añadir declaraciones extern
3. `lorawan.cpp` — Añadir variables globales (sin cambiar lógica aún)
4. `nbiot.h` — Añadir declaraciones extern nb_status_*
5. `nbiot.cpp` — Añadir variables globales nb_status_* + actualizar en funciones
6. `blescan.cpp` — Añadir variables bt_module_ok / ble_module_ok
7. `payload.h` + `payload.cpp` — Nueva firma addStatus()

### Fase 2: Lógica de detección (el cerebro)

8. `lorawan.cpp` — `lora_send()`: forzar confirmed para TELEMETRYPORT
9. `lorawan.cpp` — `EV_TXCOMPLETE`: conteo de fallos + activación nb_data_mode
10. `lorawan.cpp` — `EV_JOINED`: no desactivar NB, resetear nb_data_mode
11. `lorawan.cpp` — `EV_LINK_DEAD`: usar nb_data_mode en vez de nb_enable

### Fase 3: Routing y health checks

12. `senddata.cpp` — `SendPayload()`: routing ADEMUX según nb_data_mode
13. `senddata.cpp` — `sendData()`: health check LoRa cada 2 min
14. `senddata.cpp` — `sendData()`: health check NB cada 1 min
15. `senddata.cpp` — `checkQueue()`: adaptar a nb_data_mode

### Fase 4: NB-IoT siempre activo

16. `nbiot.cpp` — `loop()`: quitar condicional enabled para conexión
17. `nbiot.cpp` — `loop()` final: enviar siempre que haya cola
18. `nbiot.cpp` — `nb_sendMessages()`: no desactivar en modo temporal

### Fase 5: Complementos

19. `rcommand.cpp` — Actualizar get_status()
20. Decoder ChirpStack — Nuevo decoder 17 bytes

### Archivos necesarios pendientes de recibir

Para completar la implementación necesito:

- [ ] `payload.cpp` y `payload.h` — para modificar addStatus()
- [ ] `blescan.cpp` y `blescan.h` — para añadir variables bt/ble_module_ok
- [ ] `rcommand.cpp` — para actualizar get_status()
- [ ] `globals.h` — para verificar defines y tipos

---

## Notas importantes

### Separación de concerns

El flag `nb_data_mode` controla **solo el routing de datos**. No controla si NB-IoT está encendido o apagado. NB-IoT siempre está activo. Esto evita el bug del proyecto anterior donde un solo flag (`nbIotEnabled`) controlaba tanto el estado de la conexión como el routing.

### Health checks como sonda de LoRa

Los health checks por LoRa son **confirmed uplinks** que sirven como sonda activa. LMIC espera ACK y si no llega, `EV_TXCOMPLETE` sin flag `TXRX_ACK` dispara el incremento de `healthcheck_failures`. Esto funciona incluso cuando el dispositivo está desactivado en ChirpStack, que es exactamente el escenario que no se detectaba antes.

### NB-IoT como canal de monitorización siempre disponible

Los health checks por NB-IoT van cada 1 minuto independientemente del estado de LoRa. Esto permite que el sistema de monitorización (Zabbix) siempre tenga visibilidad del dispositivo, incluso cuando LoRa funciona perfectamente. Si NB deja de enviar health checks, Zabbix sabe que el dispositivo tiene un problema real (no solo LoRa caído).