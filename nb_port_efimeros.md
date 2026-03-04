# FIX: Respuestas a comandos remotos — envío directo por NB-IoT

**Archivos modificados:** `nbiot.h`, `nbiot.cpp`, `rcommand.cpp`, `senddata.cpp`  
**Fecha:** 2026-03-04  
**Proyecto:** STA_ADEMUX / ESP32-Paxcounter fork  

---

## Problema

Cuando el dispositivo recibe un comando remoto vía MQTT/NB-IoT (p.ej. `get_config`, opcode `0x80`), la respuesta pasaba por `SendPayload()` → sistema de colas → `nb_enqueuedata()`. Esto causaba dos problemas:

### Problema 1: Corrupción de la cola SD

`nb_enqueuedata()` con `prio_high` tiene lógica de expulsión: si la cola RAM está llena, expulsa el mensaje más antiguo directamente a `sdqueueEnqueue()` **sin ningún check de puerto**. Si ese mensaje expulsado era otra respuesta a un comando anterior, entraba a la SD y podía causar corrupción por el bug de `fileSize_` desactualizado en SdFat.

```
get_config → SendPayload(CONFIGPORT=3, prio_high)
  → nb_data_mode=true → nb_enqueuedata(prio_high)
  → cola llena → expulsa mensaje viejo → sdqueueEnqueue()  ← CORRUPCIÓN
  → xQueueSendToFront() → return true
```

### Problema 2: Respuesta no inmediata

Las respuestas a comandos competían con los mensajes de datos en la cola NB-IoT. Si NB-IoT estaba ocupada enviando MACs WiFi/BLE, la respuesta al `get_config` podía tardar varios segundos o perderse.

### Por qué el fix anterior (isEphemeralPort) no funcionó

El fix de `isEphemeralPort()` en `senddata.cpp` estaba en el lugar equivocado: solo se evaluaba en el bloque de fallback cuando `enqueued=false`. Pero `nb_enqueuedata()` casi siempre devuelve `true` (tiene expulsión interna), por lo que el check nunca se ejecutaba. La corrupción ocurría dentro de `nb_enqueuedata()`, no en el fallback de `senddata.cpp`.

---

## Solución

Las respuestas a comandos remotos se envían **directamente** por NB-IoT, saltándose completamente el sistema de colas. La nueva función `nb_send_direct()` llama a `sendNbMqtt()` sin pasar por `NbSendQueue` ni por la SD.

### Ventajas

- No compite con la cola de datos de conteo (WiFi/BLE/BT MACs)
- No toca la SD en ningún caso
- La respuesta llega inmediatamente al operador
- Elimina la causa raíz de la corrupción: las respuestas nunca pasan por `nb_enqueuedata()`

---

## Cambios por archivo

### `nbiot.h`

**Antes:** `nbConfig` y `devEui` eran atributos `private` de `NbIotManager`.

**Después:** Movidos a `public` para que `nb_send_direct()` pueda acceder a ellos a través del puntero global al manager.

```cpp
// Añadido en sección public:
ConfigBuffer_t nbConfig;
char devEui[17];

// Declaración de la nueva función pública:
int nb_send_direct(MessageBuffer_t *message);
```

---

### `nbiot.cpp`

**Cambio 1:** Puntero global al manager.

```cpp
// Al inicio del archivo:
static NbIotManager *g_manager = nullptr;
```

**Cambio 2:** En `nb_send()`, el manager pasa a ser `static` y se asigna al puntero global:

```cpp
void nb_send(void *pvParameters) {
    configASSERT(((uint32_t)pvParameters) == 1);
    static NbIotManager manager;
    g_manager = &manager;  // ← exponer para nb_send_direct()
    while (1) { ... }
}
```

**Cambio 3:** `devEui` se inicializa en `nb_init()` (antes solo se hacía en `nb_connectMqtt()`):

```cpp
void NbIotManager::nb_init() {
    // ... configuración del modem ...
    sprintf(this->devEui, "%02x%02x%02x%02x%02x%02x%02x%02x",
            DEVEUI[0], DEVEUI[1], ... DEVEUI[7]);
}
```

**Cambio 4:** Forward declaration de `sendNbMqtt()` antes de `nb_send_direct()` para resolver orden de declaración:

```cpp
int sendNbMqtt(MessageBuffer_t *message, ConfigBuffer_t *config, char *devEui);
```

**Cambio 5:** Implementación de `nb_send_direct()`:

```cpp
int nb_send_direct(MessageBuffer_t *message) {
    if (!g_manager) {
        ESP_LOGE(TAG, "nb_send_direct: manager not ready");
        return -1;
    }
    int result = sendNbMqtt(message, &g_manager->nbConfig, g_manager->devEui);
    return result;
}
```

---

### `rcommand.cpp`

**Cambio principal:** Añadido helper `send_response_direct()` al inicio del archivo:

```cpp
static void send_response_direct(uint8_t port, sendprio_t prio) {
  MessageBuffer_t buf;
  buf.MessageSize = payload.getSize();
  buf.MessagePort = port;
  buf.MessagePrio = prio;
  memcpy(buf.Message, payload.getBuffer(), buf.MessageSize);
  int result = nb_send_direct(&buf);
  if (result != 0) {
    // Fallback si el manager aún no está listo (arranque)
    ESP_LOGW(TAG, "nb_send_direct failed (port=%u), fallback to SendPayload", port);
    SendPayload(port, prio);
  }
}
```

**Comandos migrados** de `SendPayload()` a `send_response_direct()`:

| Opcode | Función | Puerto |
|--------|---------|--------|
| `0x80` | `get_config()` | `CONFIGPORT = 3` |
| `0x81` | `get_status()` | `STATUSPORT = 2` |
| `0x83` | `get_batt()` | `BATTPORT = 8` |
| `0x84` | `get_gps()` | `GPSPORT = 4` |
| `0x86` | `get_time()` | `TIMEPORT = 9` |
| `0x88` | `get_userSalt()` | `CONFIGPORT = 3` |
| `0x8A` | `imeiTask()` | `RCMDPORT = 2` |
| `0x8B` | `msisdnTask()` | `RCMDPORT = 2` |

`get_bme()` conserva `SendPayload()` ya que el sensor BME680 no está presente en el hardware y no es un comando crítico.

---

### `senddata.cpp`

Revertido a versión original sin `isEphemeralPort()`. El fix anterior fue eliminado porque estaba en el lugar equivocado del flujo y era código muerto.

---

## Flujo antes y después

### Antes

```
ChirpStack → MQTT downlink → nb_readMessages() → rcommand()
→ get_config() → SendPayload(CONFIGPORT, prio_high)
  → nb_data_mode=true → nb_enqueuedata(prio_high)
    → si cola llena: expulsa mensaje → sdqueueEnqueue()  ← posible corrupción
    → xQueueSendToFront() → mensaje en cola
  → nb_sendMessages() lo envía cuando puede
→ respuesta sale con retraso
```

### Después

```
ChirpStack → MQTT downlink → nb_readMessages() → rcommand()
→ get_config() → send_response_direct(CONFIGPORT, prio_high)
  → nb_send_direct() → sendNbMqtt() directo
→ respuesta sale inmediatamente
→ SD nunca involucrada
→ cola de datos no afectada
```

---

## Comportamiento del fallback

Si `nb_send_direct()` devuelve `-1` (el manager aún no está listo, solo ocurre en los primeros segundos del arranque), `send_response_direct()` hace fallback automático a `SendPayload()`. Esto garantiza que los comandos enviados durante el arranque no se pierdan silenciosamente.

---

---

## Fix adicional: Health check por nb_send_direct()

**Fecha:** 2026-03-04  
**Archivo modificado:** `senddata.cpp`

### Problema

El health check (TELEMETRYPORT = 14) entraba a `nb_enqueuedata()` justo cuando NB-IoT estaba ocupada enviando datos de conteo. Aunque la cola RAM tenía espacio, la lógica de `nb_enqueuedata()` con `prio_normal` podía caer al fallback SD si había contención. Esto disparaba `sdqueueEnqueue()` consecutivos rápidos que corrompían `paxqueue.q`.

```
nb_sendMessages(): NB messages pending, sending
publishMqtt(): AT+QMTPUB...         ← NB ocupada
nb_enqueuedata(port=14, prio=1)     ← health check entra a cola
  → fallback SD                     ← corrupción aquí
sdqueueEnqueue(): port 14 count=N
sdqueueEnqueue(): CORRUPTA
```

### Solución

Igual que con los comandos remotos: el health check sale directo por `nb_send_direct()` sin pasar por ninguna cola.

**Dos cambios en `senddata.cpp`:**

```cpp
// Health check LoRa — antes:
nb_enqueuedata(&nbMessage);

// Health check LoRa — después:
nb_send_direct(&nbMessage);

// Health check NB-IoT independiente — antes:
nb_enqueuedata(&nbMessage);

// Health check NB-IoT independiente — después:
nb_send_direct(&nbMessage);
```

### Log verificado

```
nb_send_direct: ✓ sent ok (port=14)
NB health check [Up:140 T:52 Heap:118720/110016 Rst:1 F1:0xEC F2:0x00 RSSI:0 SNR:0 NbR:99 NbF:0 F3:0xE7]
```

Sin `sdqueueEnqueue()` de port 14. Sin corrupción.

---

## Logs esperados

**Operación normal:**
```
[I] rcommand: Remote command: get device configuration
[I] nbiot: nb_send_direct: sending port=3 size=27 directly
[I] nbiot: nb_send_direct: ✓ sent ok (port=3)
```

**Fallback durante arranque (raro):**
```
[I] rcommand: Remote command: get device configuration
[E] nbiot: nb_send_direct: manager not ready
[W] rcommand: nb_send_direct failed (port=3), fallback to SendPayload
```

**Lo que ya NO aparece:**
```
[I] sdcard: 📦 Paquete salvado en cola SD (port 3, 27 bytes, count=1)
```
