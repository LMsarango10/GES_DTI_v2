# Análisis Bug MQTT — Paxcounter STA_ADEMUX

**Dispositivo:** 171fe21d71b39000  
**Firmware:** Paxcounter con ADEMUX (LoRa + NB-IoT)  
**Fecha:** 2 de Marzo de 2026  
**Módulo afectado:** BC95-G (NB-IoT) — Comunicación MQTT  

---

## 1. Descripción del Problema

Los dispositivos paxcounter no publicaban datos por MQTT a través del módulo NB-IoT (BC95-G), a pesar de que:

- La red celular estaba operativa (`+CEREG:0,5` — registrado en roaming)
- La conexión MQTT se establecía con éxito (`+QMTCONN: 0,0,0`)
- La suscripción al topic se completaba (`+QMTSUB: 0,1,0,0`)
- Los mensajes se encolaban correctamente en la cola NB RAM

El dispositivo entraba en un **bucle infinito de reconexión** sin llegar nunca a ejecutar el `QMTPUB` (publicación).

---

## 2. Arquitectura del Sistema NB-IoT

### 2.1 Componentes

- **nbiot.cpp** — `NbIotManager`: gestiona el ciclo de vida NB-IoT (init, registro, conexión, MQTT, envío)
- **BC95.cpp** — Capa de comunicación AT con el módem Quectel BC95-G vía UART (HardwareSerial)
- **Cola NB RAM** — FreeRTOS Queue (`NbSendQueue`) de hasta 500 mensajes
- **Task dedicado** — `nbtask` ejecuta `NbIotManager::loop()` cada 100ms en Core 1

### 2.2 Flujo del Loop Principal

```
NbIotManager::loop() — se ejecuta cada ~100ms
│
├─ ¿Demasiados fallos? → nb_resetStatus() → SALE
│
├─ ¿No inicializado? → nb_init() → SALE
│
├─ ¿No registrado en red? → nb_registerNetwork() → SALE
│       └─ AT+CEREG? → busca CEREG:0,1 o CEREG:0,5
│
├─ ¿No conectado a red? → nb_connectNetwork() → SALE
│       └─ AT+CGPADDR → verifica IP asignada
│
├─ ¿MQTT no conectado? → nb_connectMqtt() → SALE
│       └─ AT+QMTOPEN → AT+QMTCONN
│
├─ ¿No suscrito? → nb_subscribeMqtt() → SALE
│       └─ AT+QMTSUB
│
├─ ¿Health check falla? → marca flags como false → SALE
│       └─ nb_checkStatus():
│           ├─ nb_checkNetworkRegister()   → AT+CEREG?
│           ├─ nb_checkNetworkConnected()  → AT+CGPADDR
│           └─ nb_checkMqttConnected()     → AT+QMTCONN?
│               └─ Si falla: mqttConnected=false, subscribed=false
│
├─ nb_readMessages() — lee downlinks MQTT
│
└─ nb_sendMessages() — PUBLICA mensajes de la cola
        └─ publishMqtt() → AT+QMTPUB
```

**Principio clave:** Cada paso con `→ SALE` significa que el loop hace `return` y no avanza al siguiente paso. Solo si TODOS los pasos pasan, se llega a `nb_sendMessages()`.

---

## 3. Causa Raíz del Bug

### 3.1 El Protocolo AT del BC95-G

El módem BC95-G responde a los comandos AT de forma **asíncrona en dos partes**. Ejemplo con `AT+QMTSUB`:

```
Firmware envía:  AT+QMTSUB=0,1,"topic",0
Módem responde:  OK                          ← Parte 1: confirma recepción del comando
                 +QMTSUB: 0,1,0,0            ← Parte 2: confirma ejecución (llega después)
```

La Parte 2 puede tardar varios segundos en llegar, dependiendo de la latencia de red.

### 3.2 El Código Original de `subscribeMqtt()`

```cpp
bool subscribeMqtt(char *topic) {
    bc95serial.print("AT+QMTSUB=0,1,\"");
    bc95serial.print(topic);
    bc95serial.print("\",");
    bc95serial.println("0");
    char data[128];
    int bytesRead = readResponseBC(&bc95serial, data, 128);  // timeout 500ms
    if (!assertResponseBC("OK\r", data, bytesRead)) {
        return false;
    }
    return true;  // ← Sale aquí, sin consumir +QMTSUB
}
```

**Problema:** La función lee solo el `OK` (Parte 1) y retorna `true`. La confirmación `+QMTSUB: 0,1,0,0` (Parte 2) **queda pendiente en el buffer UART del BC95-G**.

### 3.3 La Contaminación del Buffer Serial

100ms después, `nb_checkStatus()` ejecuta `checkMqttConnection()`:

```cpp
bool checkMqttConnection() {
    bc95serial.println("AT+QMTCONN?");
    char data[128];
    int bytesRead = readResponseBC(&bc95serial, data, 128);  // timeout 500ms
    if (assertResponseBC("+QMTCONN: 0,3", data, bytesRead)) {
        return true;
    }
    return false;
}
```

En este momento, el buffer UART contiene:

```
+QMTSUB: 0,1,0,0\r\n     ← Residuo del subscribe anterior
+QMTCONN: 0,3\r\nOK\r\n  ← Respuesta real al QMTCONN?
```

`readResponseBC()` lee los primeros bytes disponibles (timeout 500ms). Puede capturar:

- **Caso A:** Solo `+QMTSUB: 0,1,0,0` → busca `+QMTCONN: 0,3` → NO LO ENCUENTRA → `return false`
- **Caso B:** Ambas respuestas mezcladas → puede encontrar `+QMTCONN: 0,3` → `return true`

El Caso A ocurre frecuentemente, especialmente cuando LoRa está transmitiendo en paralelo y la CPU tiene menos tiempo para el task NB.

### 3.4 El Bucle de Reconexión

```
Ciclo 1: connectMqtt()     → +QMTCONN: 0,0,0 → mqttConnected = true → SALE
Ciclo 2: subscribeMqtt()   → OK → subscribed = true → SALE
         (deja +QMTSUB: 0,1,0,0 en el buffer)
Ciclo 3: nb_checkStatus()  → checkMqttConnection() → lee basura → return false
         → mqttConnected = false, subscribed = false → SALE
Ciclo 4: nb_connectMqtt()  → reconecta MQTT → SALE
Ciclo 5: nb_subscribeMqtt()→ suscribe otra vez, deja basura otra vez → SALE
Ciclo 6: nb_checkStatus()  → lee basura otra vez → return false → SALE
...
NUNCA llega a nb_sendMessages()
```

---

## 4. Solución Implementada

### 4.1 Cambio en `subscribeMqtt()` — BC95.cpp

Se modificó la función para que **consuma la confirmación asíncrona `+QMTSUB`** antes de retornar, dejando el buffer UART limpio.

#### Código Anterior

```cpp
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
```

#### Código Nuevo

```cpp
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

    // La respuesta del modem viene en dos partes:
    //   1. "OK"              ← ya la leímos arriba
    //   2. "+QMTSUB: 0,1,0,0" ← confirmación asíncrona
    // Si no consumimos la parte 2, queda en el buffer serial
    // y contamina la siguiente lectura (checkMqttConnection)
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
```

### 4.2 Resultado Verificado en Logs

```
[D] subscribeMqtt(): SENDING TO Modem: AT+QMTSUB=0,1,"DIVALGATE/.../tx",0
[D] readResponseBC(): Message: OK
[D] subscribeMqtt(): Waiting for QMTSUB confirmation, attempt 1
[D] subscribeMqtt(): Waiting for QMTSUB confirmation, attempt 2
[D] subscribeMqtt(): Waiting for QMTSUB confirmation, attempt 3
[D] readResponseBC(): Message: +QMTSUB: 0,1,0,0
[D] subscribeMqtt(): QMTSUB confirmation consumed: +QMTSUB: 0,1,0,0
[D] nbiot: MQTT SUBSCRIBED
```

Posterior health check limpio:

```
[D] checkMqttConnection(): SENDING TO Modem: AT+QMTCONN?
[D] readResponseBC(): Message: +QMTCONN: 0,3  OK
→ return true ✓
→ Llega a nb_sendMessages() → PUBLICA ✓
```

---

## 5. Comparativa: Antes vs Después

| Aspecto | Antes del Fix | Después del Fix |
|---------|--------------|-----------------|
| `subscribeMqtt()` consume `+QMTSUB` | No | Sí (hasta 5 reintentos × 1000ms) |
| Buffer UART post-subscribe | Contaminado | Limpio |
| `checkMqttConnection()` lee respuesta correcta | Intermitente (depende del timing) | Consistente |
| Llega a `nb_sendMessages()` | Raramente (bucle de reconexión) | Sí |
| Publicación MQTT | Falla o con gaps de 10+ minutos | Funciona cada ciclo |
| Tiempo extra en subscribe | 0ms | Hasta 5000ms (peor caso) |
| Archivos modificados | — | Solo `BC95.cpp` |
| Cambios en lógica de `nbiot.cpp` | — | Ninguno |

---

## 6. Flujograma: Antes del Fix

```
subscribeMqtt()
│
├─ Envía AT+QMTSUB
├─ Lee respuesta (500ms)
├─ ¿Contiene "OK"? 
│   ├─ NO → return false
│   └─ SÍ → return true
│          ("+QMTSUB: 0,1,0,0" QUEDA EN BUFFER)
│
▼ 100ms después...
│
checkMqttConnection()
│
├─ Envía AT+QMTCONN?
├─ Lee respuesta (500ms)
│   └─ Buffer contiene: "+QMTSUB: 0,1,0,0" + "+QMTCONN: 0,3"
│      Lee primeros bytes → puede ser la basura del QMTSUB
├─ ¿Contiene "+QMTCONN: 0,3"?
│   ├─ SÍ (suerte) → return true → PUBLICA
│   └─ NO (basura) → return false
│       └─ mqttConnected = false
│       └─ subscribed = false
│           └─ Siguiente ciclo: reconecta → re-suscribe → más basura → BUCLE
```

## 7. Flujograma: Después del Fix

```
subscribeMqtt()
│
├─ Envía AT+QMTSUB
├─ Lee respuesta (500ms)
├─ ¿Contiene "OK"?
│   ├─ NO → return false
│   └─ SÍ → Entra en bucle de consumo
│       │
│       ├─ Intento 1: Lee (1000ms) → ¿"+QMTSUB:"? → NO → descarta
│       ├─ Intento 2: Lee (1000ms) → ¿"+QMTSUB:"? → NO → descarta
│       ├─ Intento 3: Lee (1000ms) → ¿"+QMTSUB:"? → SÍ → consumed ✓ → break
│       │
│       └─ return true (BUFFER LIMPIO)
│
▼ 100ms después...
│
checkMqttConnection()
│
├─ Envía AT+QMTCONN?
├─ Lee respuesta (500ms)
│   └─ Buffer contiene SOLO: "+QMTCONN: 0,3\r\nOK\r\n"
├─ ¿Contiene "+QMTCONN: 0,3"?
│   └─ SÍ → return true ✓
│
▼
│
nb_sendMessages()
│
└─ publishMqtt() → AT+QMTPUB → DATOS LLEGAN AL BROKER ✓
```

---

## 8. Problemas Pendientes Identificados

### 8.1 Cuello de Botella en `publishMqtt()` — Prioridad Media

**Problema:** Cada `publishMqtt()` puede tardar hasta 5.5 segundos (500ms esperando `>` + 5000ms esperando `+QMTPUB: 0,0,0`). Si hay N mensajes en cola, son N × 5.5s bloqueando el task NB.

**Impacto:** Gaps de 1-3 minutos entre publicaciones cuando hay mensajes acumulados.

**Solución propuesta:** Reducir el timeout de `readResponseWithStop` en `publishMqtt()` o implementar envío no-bloqueante.

### 8.2 Update Check (`getData()`) Bloqueante — Prioridad Media-Alta

**Problema:** La función `getData()` para verificar actualizaciones OTA abre un socket TCP, envía HTTP GET, y espera respuesta. Con los timeouts internos (`NBSENDTIMEOUT`, `HTTP_READ_TIMEOUT`), puede bloquear el task NB durante 30+ segundos. Actualmente el servidor responde 404.

**Impacto:** Bloqueo periódico del task NB durante hasta 30 segundos sin beneficio funcional.

**Solución propuesta (de menor a mayor invasividad):**

- **Opción A:** Alargar `UPDATES_CHECK_INTERVAL` a 24h (cambiar un `#define`)
- **Opción B:** Desactivar `UPDATES_ENABLED` en compilación
- **Opción C:** Arreglar la URL del servidor de updates para eliminar el 404

### 8.3 Red Celular Inestable (`CEREG:0,2`) — Prioridad Baja

**Problema:** El BC95-G pierde la red celular periódicamente, mostrando `+CEREG:0,2` ("searching") durante varios minutos antes de reenganchar.

**Impacto:** Durante esos periodos no hay conectividad NB-IoT. LoRa sigue operando normalmente.

**Posibles causas:** Cobertura NB-IoT débil en la ubicación, problemas físicos del módulo BC95-G, o configuración de bandas.

### 8.4 Robustez Adicional de `checkMqttConnection()` — Prioridad Baja

**Problema:** Aunque el fix del subscribe elimina la fuente principal de basura, otros comandos AT o URCs inesperadas podrían contaminar el buffer en el futuro.

**Solución propuesta:** Implementar `readUntilExpected()` en `checkMqttConnection()` para que descarte basura y busque la respuesta real con reintentos. Esto añadiría una segunda capa de defensa.

---

## 9. Archivos Modificados

| Archivo | Función | Tipo de Cambio |
|---------|---------|----------------|
| `BC95.cpp` | `subscribeMqtt()` | Añadido bucle de consumo de `+QMTSUB` |

## 10. Archivos NO Modificados

| Archivo | Razón |
|---------|-------|
| `nbiot.cpp` | La lógica del loop y `nb_checkStatus()` es correcta; el bug estaba en la capa serial |
| `BC95.cpp` — `checkMqttConnection()` | Funciona correctamente cuando el buffer está limpio |
| `BC95.cpp` — `publishMqtt()` | Funcional, pero es candidato para optimización futura |
| `BC95.cpp` — `connectMqtt()` | Sin cambios necesarios |