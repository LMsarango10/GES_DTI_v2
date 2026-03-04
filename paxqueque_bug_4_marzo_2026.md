# Bug: Corrupción de `paxqueue.q` en SD

## Descripción
La cola persistente en SD (`paxqueue.q`) se corrompe periódicamente, causando pérdida de mensajes en el ciclo afectado.

---

## Causa Raíz
`FILE_WRITE` en ESP32/Arduino-SD **trunca el archivo a 0 bytes** al abrirlo, equivalente a `fopen("w")` en C estándar, no `fopen("r+")`. Esto ocurre en dos puntos:

**Punto 1 — `sdqueueDequeue()`:**
```cpp
// Abre en READ para leer el registro
FileMySD f = mySD.open(PAXQUEUE_FILE, FILE_READ);
...
f.close();
// ← GAP: archivo intacto aquí

// Reabre en WRITE para actualizar cabecera → TRUNCA EL ARCHIVO
f = mySD.open(PAXQUEUE_FILE, FILE_WRITE);
writeHeader(f, h);  // solo escribe 20 bytes, registros restantes PERDIDOS
```

**Punto 2 — `sdqueueDequeue()` cuando `count==0`:**
```cpp
if (h.count == 0 || h.head > 128000) {
    sdq_compact_locked();  // abre con FILE_WRITE → trunca
}
```

**Punto 3 — `sdqueueInit()` (pre-fix):**
```cpp
if (!ok) {
    mySD.remove(PAXQUEUE_FILE);
    return sdqueueInit();  // RECURSIÓN INFINITA → stack overflow → watchdog
}
```

---

## Secuencia del Bug
```
1. Flusher vacía la cola → count=0
2. sdqueueDequeue() llama sdq_compact_locked()
3. sdq_compact_locked() abre FILE_WRITE → archivo truncado a 0 bytes
4. Cabecera desaparece
5. Siguiente sdqueueEnqueue() lee cabecera → CRC falla → "corrupta"
6. sdqueueEnqueue() intenta rebuilding → abre FILE_WRITE → trunca de nuevo
7. Bucle: cada rebuild re-corrompe lo que el anterior reconstruyó
```

---

## Comportamiento Antes del Fix

| Escenario | Resultado |
|-----------|-----------|
| Recursión llega a límite de stack | Watchdog reset (`rst:0x8 TG1WDT_SYS_RESET`) |
| Recursión limitada | Cola permanentemente corrupta, mensajes perdidos indefinidamente |

---

## Fix Aplicado
Eliminar recursión en `sdqueueInit()` y en `sdqueueEnqueue()`, recreando el archivo directamente sin llamadas recursivas.

**En `sdqueueInit()`:**
```cpp
// ANTES
if (!ok) {
    mySD.remove(PAXQUEUE_FILE);
    return sdqueueInit();  // recursión infinita
}

// DESPUÉS
if (!ok) {
    mySD.remove(PAXQUEUE_FILE);
    FileMySD f = mySD.open(PAXQUEUE_FILE, FILE_WRITE);
    if (!f) return false;
    PaxQHeader h{};
    h.head  = sizeof(PaxQHeader);
    h.tail  = sizeof(PaxQHeader);
    h.count = 0;
    bool created = writeHeader(f, h);
    f.close();
    return created;
}
```

**En `sdqueueEnqueue()`:**
```cpp
// ANTES
if (!ok) {
    mySD.remove(PAXQUEUE_FILE);
    if (!sdqueueInit()) { ... }  // puede entrar en recursión
    ...
}

// DESPUÉS
if (!ok) {
    mySD.remove(PAXQUEUE_FILE);
    FileMySD nf = mySD.open(PAXQUEUE_FILE, FILE_WRITE);
    if (!nf) { sdq_unlock(); return false; }
    h = {};
    h.head  = sizeof(PaxQHeader);
    h.tail  = sizeof(PaxQHeader);
    h.count = 0;
    if (!writeHeader(nf, h)) { nf.close(); sdq_unlock(); return false; }
    nf.close();
    // continúa y encola el mensaje normalmente
}
```

---

## Comportamiento Después del Fix

| Escenario | Resultado |
|-----------|-----------|
| Corrupción detectada en enqueue | Archivo reconstruido, 2-3 mensajes perdidos en ese ciclo |
| Ciclo siguiente | Funciona normalmente sin errores |
| Watchdog reset | Eliminado |
| Cola permanentemente corrupta | Eliminado |

---

## Limitación Residual
La causa raíz (`FILE_WRITE` trunca) sigue presente en `sdqueueDequeue()`. El fix evita la cascada de fallos pero no impide la corrupción inicial. La pérdida de 2-3 mensajes por ciclo corrupto es aceptable dado que:

- Los mensajes críticos (health check port 14) se envían vía `nb_send_direct()` antes de encolar
- La cola SD es un mecanismo de backup, no el canal primario
- El sistema se autorecupera en el siguiente ciclo sin intervención

---

## Hotfix Manual
Si se detecta cola corrupta persistente vía Zabbix:

```
Comando base64: CQA=
Opcode:         0x09 0x00
Efecto:         reset frío → recrea el archivo desde cero en el siguiente arranque
```