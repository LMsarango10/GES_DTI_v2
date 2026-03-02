# Health Check ADEMUX v2 — Documentación Técnica

**Proyecto:** GES_DTI_v2 (Paxcounter ADEMUX)
**Fecha:** 2 de marzo de 2026
**Rama:** develop
**Autor:** Leo Sarango / Gesinen

---

## 1. Resumen

Se implementaron dos mejoras al sistema de health check de los dispositivos STA (Paxcounter con ADEMUX):

1. **Detección de degradación de módulos BT/BLE** — `BTCycle()` y `BLECycle()` ahora retornan `bool`, permitiendo detectar cuando un módulo que funcionaba deja de funcionar.
2. **flags1 con estado operativo real** — `flags1` del payload de telemetría (fPort 14) ahora refleja si cada módulo **funciona realmente**, en vez de reportar solo la configuración o valores de compilación.

---

## 2. Estructura del payload de telemetría (fPort 14, 17 bytes)

| Offset | Bytes | Campo | Tipo | Descripción |
|--------|-------|-------|------|-------------|
| 0-3 | 4 | uptime | uint32 BE | Segundos desde boot |
| 4 | 1 | cputemp | uint8 | Temperatura CPU en °C |
| 5-6 | 2 | free_heap | uint16 BE | Heap libre ÷ 16 (multiplicar × 16 para bytes) |
| 7-8 | 2 | min_free_heap | uint16 BE | Heap mínimo ÷ 16 |
| 9 | 1 | reset_reason | uint8 | Razón del último reset (ver tabla) |
| 10 | 1 | **flags1** | bitmap | **Estado operativo real de módulos (v2)** |
| 11 | 1 | hc_failures | uint8 | Fallos de health check LoRa (sin ACK) |
| 12 | 1 | lora_rssi | uint8 | RSSI LoRa (valor absoluto, usar como negativo) |
| 13 | 1 | lora_snr | int8 | SNR LoRa en dB |
| 14 | 1 | nb_rssi | uint8 | CSQ NB-IoT (99 = desconocido) |
| 15 | 1 | nb_failures | uint8 | Fallos consecutivos NB-IoT |
| 16 | 1 | flags3 | bitmap | Estado NB-IoT + CPU + canal + módulos BT/BLE |

### Reset Reason (byte 9)

| Valor | Significado |
|-------|-------------|
| 0 | UNKNOWN |
| 1 | POWERON |
| 2 | EXT (reset externo) |
| 3 | SW (reset por software) |
| 4 | PANIC (excepción/crash) |
| 5 | INT_WDT (watchdog interrupción) |
| 6 | TASK_WDT (watchdog tarea) |
| 7 | WDT (watchdog genérico) |
| 8 | DEEPSLEEP |
| 9 | BROWNOUT (bajo voltaje) |

---

## 3. flags1 — Estado operativo real (v2)

### Formato anterior (v1) — OBSOLETO

```
Bit 7: cfg.wifiscan      ← Solo decía si estaba configurado, NO si funcionaba
Bit 6: cfg.blescan       ← Solo configuración
Bit 5: cfg.btscan        ← Solo configuración (reportaba 1 con módulo muerto)
Bit 4: LMIC.devaddr      ← OK (join = funciona)
Bit 3: nb_isEnabled()    ← Solo si estaba habilitado como canal, NO si el módulo respondía
Bit 2: HAS_SDCARD        ← Valor de compilación, siempre 1 (reportaba 1 con SD muerta)
Bit 1: gps_hasfix()      ← GPS (eliminado)
Bit 0: reservado
```

**Ejemplo real del problema:** Un STA con BT muerto y SD dañada reportaba `flags1 = 0xF4 = 11110100`, indicando BT=✅ y SD=✅. **Ambos eran mentira.**

### Formato nuevo (v2) — ACTUAL

```
Bit 7: wifi_radio_ok     ← ¿El radio WiFi inicializó y está escaneando?
Bit 6: ble_module_ok     ← ¿El CC41-A responde AT y escanea dispositivos?
Bit 5: bt_module_ok      ← ¿El HC-05 responde AT y escanea dispositivos?
Bit 4: LMIC.devaddr      ← ¿LoRa tiene sesión activa? (join completado)
Bit 3: nb_module_ok      ← ¿El BC95-G inicializó y responde a comandos AT?
Bit 2: isSDCardAvailable ← ¿La SD está montada y accesible en este momento?
Bit 1: reservado
Bit 0: reservado
```

**Mismo STA con v2:** `flags1 = 0xD8 = 11011000` → BT=❌ SD=❌ NB=✅. **Ahora refleja la verdad.**

### Comparación directa v1 vs v2

| Bit | Módulo | v1 (0xF4) | v2 (0xD8) | Realidad |
|-----|--------|-----------|-----------|----------|
| 7 | WiFi | ✅ (config ON) | ✅ (radio OK) | ✅ Funciona |
| 6 | BLE | ✅ (config ON) | ✅ (módulo OK) | ✅ Funciona |
| 5 | **BT** | **✅ (config ON)** | **❌ (módulo FALLO)** | **❌ Muerto** |
| 4 | LoRa | ✅ (joined) | ✅ (joined) | ✅ Funciona |
| 3 | **NB-IoT** | **❌ (not enabled)** | **✅ (responde)** | **✅ Funciona** |
| 2 | **SD** | **✅ (#ifdef)** | **❌ (no disponible)** | **❌ Muerta** |

---

## 4. flags3 — Sin cambios

```
Bit 7: nb_status_registered   ← NB-IoT registrado en red celular (+CEREG:0,5)
Bit 6: nb_status_connected    ← NB-IoT conexión de datos activa (+CGPADDR)
Bit 5-4: cpu_freq_code        ← 0=80MHz, 1=160MHz, 2=240MHz
Bit 3-2: lastSendChannel      ← 0=ninguno, 1=LoRa, 2=NB-IoT, 3=SD
Bit 1: bt_module_ok           ← Duplicado temporal (mismo que flags1 bit 5)
Bit 0: ble_module_ok          ← Duplicado temporal (mismo que flags1 bit 6)
```

> **Nota:** Los bits 1-0 de flags3 están duplicados temporalmente con flags1 bits 6-5 para mantener compatibilidad con decoders existentes. Se eliminarán en una versión futura cuando Zabbix lea de flags1.

---

## 5. Cambios implementados

### 5.1 Detección de degradación BT/BLE (`blecsan.cpp`)

**Problema:** Si un módulo BT o BLE inicializaba correctamente pero luego dejaba de responder, `bt_module_ok` / `ble_module_ok` nunca se actualizaban a `false`. El health check seguía reportando el módulo como funcional.

**Solución:** `BTCycle()` y `BLECycle()` cambiaron de `void` a `bool`:

```cpp
// Antes
void BTCycle(long baud);
void BLECycle(void);

// Después
bool BTCycle(long baud);   // true = scan OK, false = timeout/fallo
bool BLECycle(void);       // true = scan OK, false = error AT
```

En `btHandler()`, se añadió un contador de fallos consecutivos:

```cpp
bool scanOk = BTCycle(BT_BAUD);
if(scanOk) {
    bt_consecutive_fails = 0;
    bt_module_ok = true;
} else {
    bt_consecutive_fails++;
    if(bt_consecutive_fails >= MAX_CONSECUTIVE_SCAN_FAILS) {  // 3
        btInitialized = false;  // forzar reinit
        bt_module_ok = false;   // reportar al health check
    }
}
```

**Ejemplo de corrida:**

```
Ciclo 1: BTCycle() → true  → fails=0, bt_module_ok=true
Ciclo 2: BTCycle() → true  → fails=0, bt_module_ok=true
Ciclo 3: BTCycle() → false → fails=1, bt_module_ok=true  (aún OK)
Ciclo 4: BTCycle() → false → fails=2, bt_module_ok=true  (aún OK)
Ciclo 5: BTCycle() → false → fails=3, bt_module_ok=false ← DETECTADO
         → btInitialized=false → siguiente ciclo intenta reinitBT()
Ciclo 6: reinitBT() → true  → bt_module_ok=true (recuperado)
     o:  reinitBT() → false → bt_module_ok=false (sigue muerto)
```

**Archivos modificados:**
- `include/blescan.h` — Firmas `bool`, `extern uint8_t bt_consecutive_fails`, `#define MAX_CONSECUTIVE_SCAN_FAILS 3`
- `src/blecsan.cpp` — `BTCycle()`, `BLECycle()`, `btHandler()`

### 5.2 WiFi radio OK (`wifiscan.cpp`)

**Problema:** flags1 bit 7 reportaba `cfg.wifiscan` (configuración), no si el radio funcionaba.

**Solución:** Nueva variable global `wifi_radio_ok`:

```cpp
// wifiscan.cpp
bool wifi_radio_ok = false;

void wifi_sniffer_init(void) {
    // ... ESP_ERROR_CHECK en cada paso (aborta si falla) ...
    wifi_radio_ok = true;  // Si llegamos aquí, WiFi está OK
}

void switch_wifi_sniffer(uint8_t state) {
    if (state) {
        // ... encender ...
        wifi_radio_ok = true;
    } else {
        // ... apagar ...
        wifi_radio_ok = false;
    }
}
```

**Archivos modificados:**
- `include/wifiscan.h` — `extern bool wifi_radio_ok`
- `src/wifiscan.cpp` — Declaración y actualización en `init` y `switch`

### 5.3 NB-IoT módulo OK (`nbiot.cpp`)

**Problema:** flags1 bit 3 reportaba `nb_isEnabled()` (si estaba habilitado como canal de datos), no si el módulo BC95-G respondía.

**Solución:** Nueva variable global `nb_module_ok`:

```cpp
// nbiot.cpp
bool nb_module_ok = false;

void NbIotManager::nb_init() {
    // ... preConfigModem, configModem, attachNetwork ...
    initialized = true;
    nb_module_ok = true;   // Modem respondió OK
}

void NbIotManager::nb_resetStatus() {
    // ... reset de todo ...
    nb_module_ok = false;  // Módulo en estado desconocido
}
```

**Archivos modificados:**
- `include/nbiot.h` — `extern bool nb_module_ok`
- `src/nbiot.cpp` — Declaración y actualización en `nb_init()` y `nb_resetStatus()`

### 5.4 Nuevo flags1 en senddata.cpp

**Cambio en ambos health checks** (LoRa y NB-IoT independiente):

```cpp
// ANTES
flags1 |= (cfg.wifiscan ? 1 : 0) << 7;
flags1 |= (cfg.blescan ? 1 : 0)  << 6;
flags1 |= (cfg.btscan ? 1 : 0)   << 5;
flags1 |= (LMIC.devaddr ? 1 : 0) << 4;
flags1 |= (nb_isEnabled() ? 1:0) << 3;
flags1 |= (1)                    << 2;  // HAS_SDCARD
flags1 |= (gps_hasfix() ? 1 : 0) << 1;

// DESPUÉS
flags1 |= (wifi_radio_ok ? 1 : 0)         << 7;
flags1 |= (ble_module_ok ? 1 : 0)         << 6;
flags1 |= (bt_module_ok ? 1 : 0)          << 5;
flags1 |= (LMIC.devaddr ? 1 : 0)          << 4;
flags1 |= (nb_module_ok ? 1 : 0)          << 3;
flags1 |= (isSDCardAvailable() ? 1 : 0)   << 2;
```

**Archivo modificado:**
- `src/senddata.cpp` — Añadir `#include "wifiscan.h"`, reescribir flags1 en ambos bloques HC

---

## 6. Decodificación

### Script Python (`decode_hc_v2.py`)

```bash
python3 decode_hc_v2.py AAAAUDQc6hrDAdgAFxZjAOU=
```

### Decodificación manual de flags1

```
flags1 = 0xD8 = 11011000

Bit 7 = 1 → WiFi:   ✅ FUNCIONA
Bit 6 = 1 → BLE:    ✅ FUNCIONA
Bit 5 = 0 → BT:     ❌ NO FUNCIONA
Bit 4 = 1 → LoRa:   ✅ JOINED
Bit 3 = 1 → NB-IoT: ✅ RESPONDE
Bit 2 = 0 → SD:     ❌ NO DISPONIBLE
Bit 1 = 0 → (reservado)
Bit 0 = 0 → (reservado)

Módulos operativos: 4/6
```

---

## 7. Integración con Zabbix

Para cada bit de flags1, crear un ítem Zabbix con trigger:

| Ítem Zabbix | Extraer | Trigger |
|-------------|---------|---------|
| `sta.wifi.ok` | flags1 bit 7 | PROBLEM si = 0 |
| `sta.ble.ok` | flags1 bit 6 | PROBLEM si = 0 |
| `sta.bt.ok` | flags1 bit 5 | PROBLEM si = 0 |
| `sta.lora.ok` | flags1 bit 4 | PROBLEM si = 0 |
| `sta.nbiot.ok` | flags1 bit 3 | PROBLEM si = 0 |
| `sta.sd.ok` | flags1 bit 2 | PROBLEM si = 0 |

---

## 8. Bugs conocidos

### 8.1 Doble health check por NB-IoT

El health check LoRa (`SendPayload` + `nb_enqueuedata`) envía una copia por NB-IoT. Luego el health check NB-IoT independiente envía otro. Resultado: dos mensajes casi simultáneos en el topic DIVALGATE con valores ligeramente diferentes (race condition en `flags1` y `hc_failures`).

**Impacto:** Bajo. Datos redundantes.
**Fix pendiente:** Eliminar el `nb_enqueuedata` de la sección HC LoRa y dejar solo el HC NB-IoT independiente.

### 8.2 NB-IoT CSQ siempre 99

El módulo BC95-G no devuelve un valor válido en `AT+CSQ`. El byte 14 del payload siempre es 99 (desconocido).

**Impacto:** No se puede monitorizar calidad de señal celular en Zabbix.
**Fix pendiente:** Revisar la lectura de CSQ en el firmware NB-IoT.

---

## 9. Próximas fases

- **Fase 2:** Payload NB-IoT extendido (24 bytes) con diagnóstico detallado BT/BLE: contadores de reinit, paso de fallo, dispositivos por ciclo.
- **Fase 3:** Integración completa con Zabbix — ítems, triggers y dashboards para todos los campos de telemetría.
- **Fase 4:** Eliminar duplicados BT/BLE de flags3 y liberar bits 1-0 para futuro uso.