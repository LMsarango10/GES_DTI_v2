# ADEMUX — Health Check Payload (fPort 14)

> Dispositivo de referencia: `171fe21d71b39000`  
> Fecha: 2026-03-04

---

## 1. Descripción general

El payload de Health Check se envía por **fPort 14** (`TELEMETRYPORT`) con dos periodicidades:

- **LoRa** (confirmed uplink): cada `HEALTHCHECK_INTERVAL_MINUTES`
- **NB-IoT** (directo vía `nb_send_direct()`): cada `NB_HEALTHCHECK_INTERVAL_MINUTES`

Longitud fija: **17 bytes**, construido en `senddata.cpp` → `payload.addStatus()`.

---

## 2. Estructura del payload

| Offset | Bytes | Tipo   | Campo              | Descripción |
|--------|-------|--------|--------------------|-------------|
| 0–3    | 4     | uint32 | `uptime`           | Tiempo activo en segundos (big-endian) |
| 4      | 1     | uint8  | `cputemp`          | Temperatura CPU en °C |
| 5–6    | 2     | uint16 | `free_heap_div16`  | Heap libre / 16 → ×16 = bytes reales (big-endian) |
| 7–8    | 2     | uint16 | `min_heap_div16`   | Heap mínimo histórico / 16 (big-endian) |
| 9      | 1     | uint8  | `reset_reason`     | Motivo del último reset (`esp_reset_reason_t`) |
| 10     | 1     | uint8  | `flags1`           | Estado de módulos (ver §3) |
| 11     | 1     | uint8  | `flags2`           | `healthcheck_failures` — fallos HC LoRa acumulados |
| 12     | 1     | uint8  | `lora_rssi`        | RSSI LoRa último uplink confirmado (valor absoluto) |
| 13     | 1     | int8   | `lora_snr`         | SNR LoRa último uplink confirmado (con signo) |
| 14     | 1     | uint8  | `nb_rssi`          | CSQ NB-IoT vía `AT+CSQ` (99 = desconocido) |
| 15     | 1     | uint8  | `nb_failures`      | `nb_status_failures` — fallos consecutivos NB-IoT |
| 16     | 1     | uint8  | `flags3`           | Flags extendidos: NB status, CPU freq, canal (ver §4) |

---

## 3. flags1 (Byte 10)

| Bit   | Máscara | Variable firmware    | Descripción |
|-------|---------|----------------------|-------------|
| 7 MSB | `0x80`  | `wifi_radio_ok`      | WiFi radio inicializado |
| 6     | `0x40`  | `ble_module_ok`      | Módulo BLE operativo |
| 5     | `0x20`  | `bt_module_ok`       | Módulo BT Classic (HC-05) operativo |
| 4     | `0x10`  | `LMIC.devaddr != 0`  | Dispositivo unido a red LoRaWAN |
| 3     | `0x08`  | `nb_module_ok`       | Módulo NB-IoT (BC95-G) inicializado |
| 2     | `0x04`  | `isSDCardAvailable()`| SD card disponible |
| 1     | `0x02`  | —                    | No utilizado |
| 0 LSB | `0x01`  | —                    | No utilizado |

> **Nota:** si bit 4 = 0 (`LMIC.devaddr = 0`), los campos `lora_rssi` y `lora_snr` no son válidos.

---

## 4. flags3 (Byte 16)

| Bits  | Máscara | Variable firmware       | Descripción |
|-------|---------|-------------------------|-------------|
| 7 MSB | `0x80`  | `nb_status_registered`  | NB-IoT registrado en red (`AT+CEREG` = 1 o 5) |
| 6     | `0x40`  | `nb_status_connected`   | NB-IoT con sesión de datos activa |
| 5–4   | `0x30`  | `cpu_freq_code` (2 bits)| `0`=≤80MHz · `1`=≤160MHz · `2`=>160MHz |
| 3–2   | `0x0C`  | `lastSendChannel` (2 bits)| `0`=ninguno · `1`=LoRa · `2`=NB-IoT · `3`=SD |
| 1     | `0x02`  | `bt_module_ok`          | BT Classic operativo (redundante con flags1 bit5) |
| 0 LSB | `0x01`  | `ble_module_ok`         | BLE operativo (redundante con flags1 bit6) |

---

## 5. Reset reasons ESP-IDF (Byte 9)

| Código | Constante              | Descripción |
|--------|------------------------|-------------|
| 1      | `ESP_RST_POWERON`      | Arranque normal por alimentación |
| 2      | `ESP_RST_EXT`          | Reset externo por pin EN |
| 3      | `ESP_RST_SW`           | Reset por software (`esp_restart()`) |
| 4      | `ESP_RST_PANIC`        | Excepción / kernel panic |
| 5      | `ESP_RST_INT_WDT`      | Watchdog de interrupción |
| 6      | `ESP_RST_TASK_WDT`     | Watchdog de tarea |
| 7      | `ESP_RST_WDT`          | Otros watchdogs |
| 8      | `ESP_RST_DEEPSLEEP`    | Despertar de deep sleep |
| 9      | `ESP_RST_BROWNOUT`     | Caída de tensión (brownout) |
| 10     | `ESP_RST_SDIO`         | Reset por SDIO |

---

## 6. Ejemplo real — dispositivo `171fe21d71b39000`

### Payloads recibidos

| Campo                | Payload #1                   | Payload #2                   | Payload #3                   |
|----------------------|------------------------------|------------------------------|------------------------------|
| **Base64**           | `AAACbDQc/BkrAewAAABjAOc=`  | `AAACqDQc/A+sAewAAABjAOc=`  | `AAADXDQc/A+sAewAAABjAOc=`  |
| **HEX**              | `00 00 02 6C 34 1C FC 19 2B 01 EC 00 00 00 63 00 E7` | `00 00 02 A8 34 1C FC 0F AC 01 EC 00 00 00 63 00 E7` | `00 00 03 5C 34 1C FC 0F AC 01 EC 00 00 00 63 00 E7` |
| **Uptime**           | 620 min (~10.3h)             | 680 min (+60 min)            | 860 min (+180 min) ⚠️        |
| **CPU Temp**         | 52°C                         | 52°C                         | 52°C                         |
| **Free Heap**        | 118.720 B                    | 118.720 B                    | 118.720 B                    |
| **Min Heap**         | 103.088 B                    | 103.088 B                    | 103.088 B                    |
| **Reset reason**     | `1` POWERON ✅               | `1` POWERON ✅               | `1` POWERON ✅               |
| **flags1**           | `0xEC`                       | `0xEC`                       | `0xEC`                       |
| → wifi_radio_ok      | ✅                            | ✅                            | ✅                            |
| → ble_module_ok      | ✅                            | ✅                            | ✅                            |
| → bt_module_ok       | ✅                            | ✅                            | ✅                            |
| → LoRa joined        | ❌ devaddr=0                  | ❌ devaddr=0                  | ❌ devaddr=0                  |
| → nb_module_ok       | ✅                            | ✅                            | ✅                            |
| → SD card            | ✅                            | ✅                            | ✅                            |
| **flags2 (HC fails)**| 0                            | 0                            | 0                            |
| **LoRa RSSI**        | 0 ⚠️                         | 0 ⚠️                         | 0 ⚠️                         |
| **LoRa SNR**         | 0 ⚠️                         | 0 ⚠️                         | 0 ⚠️                         |
| **NB RSSI (CSQ)**    | 99 (N/A) ⚠️                  | 99 (N/A) ⚠️                  | 99 (N/A) ⚠️                  |
| **NB failures**      | 0                            | 0                            | 0                            |
| **flags3**           | `0xE7`                       | `0xE7`                       | `0xE7`                       |
| → NB registrado      | ✅                            | ✅                            | ✅                            |
| → NB conectado       | ✅                            | ✅                            | ✅                            |
| → CPU freq           | >160 MHz                     | >160 MHz                     | >160 MHz                     |
| → lastSendChannel    | 1 (LoRa)                     | 1 (LoRa)                     | 1 (LoRa)                     |
| → bt_module_ok       | ✅                            | ✅                            | ✅                            |
| → ble_module_ok      | ✅                            | ✅                            | ✅                            |

---

## 7. Routing del HC en senddata.cpp

```cpp
if (SendBuffer.MessagePort == TELEMETRYPORT) {
    // Health checks SIEMPRE por LoRa confirmed uplink
    enqueued = lora_enqueuedata(&SendBuffer);
}

// Además, copia directa por NB-IoT (independiente de nb_data_mode)
nb_send_direct(&nbMessage);
```

El HC **siempre** viaja por LoRa para poder detectar pérdida de conectividad y activar failover. La copia NB-IoT es adicional y no altera `lastSendChannel`.

---

## 8. Anomalías conocidas

| # | Campo | Síntoma | Causa probable |
|---|-------|---------|----------------|
| 1 | `lora_rssi` / `lora_snr` | Siempre `0` | `LMIC.devaddr = 0` al construir el payload → join no completado aún |
| 2 | `nb_rssi` (CSQ) | Siempre `99` | `AT+CSQ` no se ejecuta antes del HC, o módulo sin medición disponible |
| 3 | `bt_module_ok` / `ble_module_ok` | Reportan `OK` | Pendiente confirmar si refleja estado real del HC-05 o el workaround de fake BLE counts |