// Basic Config
#include "senddata.h"
#include "blescan.h"    // Para bt_module_ok, ble_module_ok
#include "nbiot.h"      // Para nb_status_registered, nb_status_connected, etc.
#include <esp_system.h> // Para esp_reset_reason()
#include "lorawan.h"    // Para nb_data_mode, healthcheck_failures

static const char TAG[] = "senddata";

Ticker sendcycler;
bool sent = false;
// Canal de último envío de contadores: 0=ninguno, 1=LoRa, 2=NB-IoT, 3=SD
uint8_t lastSendChannel = 0;

void sendcycle() {
  xTaskNotifyFromISR(irqHandlerTask, SENDCYCLE_IRQ, eSetBits, NULL);
}

// put data to send in RTos Queues used for transmit over channels Lora and SPI
void SendPayload(uint8_t port, sendprio_t prio) {

  MessageBuffer_t SendBuffer;
  SendBuffer.MessageSize = payload.getSize();
  SendBuffer.MessagePrio = prio;

  switch (PAYLOAD_ENCODER) {
  case 1: // plain -> no mapping
  case 2: // packed -> no mapping
    SendBuffer.MessagePort = port;
    break;
  case 3: // Cayenne LPP dynamic -> all payload goes out on same port
    SendBuffer.MessagePort = CAYENNE_LPP1;
    break;
  case 4: // Cayenne LPP packed -> we need to map some paxcounter ports
    SendBuffer.MessagePort = CAYENNE_LPP2;
    switch (SendBuffer.MessagePort) {
    case COUNTERPORT:
      SendBuffer.MessagePort = CAYENNE_LPP2;
      break;
    case RCMDPORT:
      SendBuffer.MessagePort = CAYENNE_ACTUATOR;
      break;
    case TIMEPORT:
      SendBuffer.MessagePort = CAYENNE_DEVICECONFIG;
      break;
    }
    break;
  default:
    SendBuffer.MessagePort = port;
  }

  memcpy(SendBuffer.Message, payload.getBuffer(), SendBuffer.MessageSize);

// === ADEMUX: Routing inteligente ===
#if (HAS_LORA)
  bool enqueued = false;

  if (SendBuffer.MessagePort == TELEMETRYPORT) {
    // Health checks SIEMPRE por LoRa (confirmed) para monitorizar estado
    enqueued = lora_enqueuedata(&SendBuffer);
  }
  else if (!nb_data_mode) {
    // Modo normal: datos por LoRa
    enqueued = lora_enqueuedata(&SendBuffer);
    if (enqueued) lastSendChannel = 1;  // LoRa
  }
#if (HAS_NBIOT)
  else {
    // nb_data_mode activo: datos por NB-IoT
    enqueued = nb_enqueuedata(&SendBuffer);
    if (enqueued) lastSendChannel = 2;  // NB-IoT
    ESP_LOGD(TAG, "NB data mode: routed to NB-IoT (port %u, size %u)",
             SendBuffer.MessagePort, SendBuffer.MessageSize);
  }
#endif

  // Fallback si no se pudo encolar
  if (!enqueued) {
    ESP_LOGW(TAG, "Primary queue failed (port %u, %u bytes)",
             SendBuffer.MessagePort, SendBuffer.MessageSize);
#if (HAS_NBIOT)
    if (!nb_data_mode) {
      // Estábamos en LoRa y falló → intentar NB temporal
      nb_enable(true);
      if (!nb_enqueuedata(&SendBuffer)) {
#ifdef HAS_SDCARD
        if (isSDCardAvailable()) {
          sdqueueEnqueue(&SendBuffer);
          lastSendChannel = 3;
          ESP_LOGI(TAG, "Message saved to SD (port %u)", SendBuffer.MessagePort);
        } else {
          ESP_LOGE(TAG, "SD not available - MESSAGE LOST!");
        }
#endif
      } else {
        ESP_LOGI(TAG, "Message routed to NB-IoT (fallback)");
        lastSendChannel = 2;
      }
    } else {
      // Estábamos en NB y falló → SD
#ifdef HAS_SDCARD
      if (isSDCardAvailable()) {
        sdqueueEnqueue(&SendBuffer);
        lastSendChannel = 3;
        ESP_LOGI(TAG, "NB full, message saved to SD (port %u)", SendBuffer.MessagePort);
      } else {
        ESP_LOGE(TAG, "SD not available - MESSAGE LOST!");
      }
#endif
    }
#else
#ifdef HAS_SDCARD
    if (isSDCardAvailable()) {
      sdqueueEnqueue(&SendBuffer);
      lastSendChannel = 3;
    } else {
      ESP_LOGE(TAG, "SD not available - MESSAGE LOST!");
    }
#endif
#endif
  }

#elif (HAS_NBIOT)
  // Sin LoRa, todo va por NB-IoT
  bool enqueued = nb_enqueuedata(&SendBuffer);
  if (enqueued && SendBuffer.MessagePort != TELEMETRYPORT)
    lastSendChannel = 2;
  if (!enqueued) {
#ifdef HAS_SDCARD
    if (isSDCardAvailable()) sdqueueEnqueue(&SendBuffer);
#endif
  }
#endif

#ifdef HAS_SPI
  spi_enqueuedata(&SendBuffer);
#endif
} // SendPayload

// interrupt triggered function to prepare payload to send
void sendData() {
  time_t tstamp;
  tstamp = now();
  ESP_LOGD(TAG, "timestamp is %lu", tstamp);

  uint8_t bitmask = cfg.payloadmask;
  uint8_t mask = 1;

#if (HAS_GPS)
  gpsStatus_t gps_status;
#endif

  while (bitmask) {
    switch (bitmask & mask) {

#if ((WIFICOUNTER) || (BLECOUNTER))
    case COUNT_DATA:
      ESP_LOGI(TAG, "Total mac hashes detected: %u", macs_wifi + macs_ble + macs_bt);
      payload.reset();
      payload.addTime(tstamp);

#if !(PAYLOAD_OPENSENSEBOX)
      if (cfg.wifiscan) payload.addCount(macs_wifi, MAC_SNIFF_WIFI);
      if (cfg.blescan) payload.addCount(macs_ble, MAC_SNIFF_BLE);
      if (cfg.btscan)  payload.addCount(macs_bt,  MAC_SNIFF_BT);
#endif

#if (HAS_GPS)
      if (GPSPORT == COUNTERPORT) {
        if (gps_hasfix()) {
          gps_storelocation(&gps_status);
          payload.addGPS(gps_status);
        } else {
          ESP_LOGD(TAG, "No valid GPS position");
        }
      }
#endif

#if (PAYLOAD_OPENSENSEBOX)
      if (cfg.wifiscan) payload.addCount(macs_wifi, MAC_SNIFF_WIFI);
      if (cfg.blescan) payload.addCount(macs_ble, MAC_SNIFF_BLE);
      if (cfg.btscan)  payload.addCount(macs_bt,  MAC_SNIFF_BT);
#endif

      SendPayload(COUNTERPORT, prio_high);
      ESP_LOGI(TAG, "enqueue mac counter");

      if (cfg.wifiscan) {
        std::vector<uint32_t> macs_vector;
        for (auto m : macs_list_wifi) macs_vector.push_back(m);
        uint16_t total_macs = macs_vector.size();
        ESP_LOGI(TAG, "Total WIFI MAC counter currently is at: %d", total_macs);
        while (total_macs != 0) {
          uint16_t macs_to_send = (total_macs <= 11) ? total_macs : 11;
          total_macs -= macs_to_send;
          payload.reset();
          payload.addTime(tstamp);
          for (int i = 0; i < macs_to_send; i++) {
            payload.addMac(macs_vector.back());
            macs_vector.pop_back();
          }
          SendPayload(WIFIMACSPORT, prio_low);
        }
      }

      if (cfg.blescan) {
        std::vector<uint32_t> macs_vector;
        for (auto m : macs_list_ble) macs_vector.push_back(m);
        uint16_t total_macs = macs_vector.size();
        ESP_LOGI(TAG, "Total BLE MAC counter currently is at: %d", total_macs);
        while (total_macs != 0) {
          uint16_t macs_to_send = (total_macs <= 11) ? total_macs : 11;
          total_macs -= macs_to_send;
          payload.reset();
          payload.addTime(tstamp);
          for (int i = 0; i < macs_to_send; i++) {
            payload.addMac(macs_vector.back());
            macs_vector.pop_back();
          }
          SendPayload(BLEMACSPORT, prio_low);
        }
      }

      if (cfg.btscan) {
        std::vector<uint32_t> macs_vector;
        for (auto m : macs_list_bt) macs_vector.push_back(m);
        uint16_t total_macs = macs_vector.size();
        ESP_LOGI(TAG, "Total BT MAC counter currently is at: %d", total_macs);
        while (total_macs != 0) {
          uint16_t macs_to_send = (total_macs <= 11) ? total_macs : 11;
          total_macs -= macs_to_send;
          payload.reset();
          payload.addTime(tstamp);
          for (int i = 0; i < macs_to_send; i++) {
            payload.addMac(macs_vector.back());
            macs_vector.pop_back();
          }
          SendPayload(BTMACSPORT, prio_low);
        }
      }

      if (cfg.countermode != 1) {
        reset_counters();
        get_salt();
        ESP_LOGI(TAG, "Counter cleared");
      }
      break;
#endif

#if (HAS_BME)
    case MEMS_DATA:
      payload.reset();
      payload.addBME(bme_status);
      SendPayload(BMEPORT, prio_normal);
      break;
#endif

#if (HAS_GPS)
    case GPS_DATA:
      if (GPSPORT != COUNTERPORT) {
        if (gps_hasfix()) {
          gps_storelocation(&gps_status);
          payload.reset();
          payload.addGPS(gps_status);
          SendPayload(GPSPORT, prio_high);
        } else ESP_LOGD(TAG, "No valid GPS position");
      }
      break;
#endif

#if (HAS_SENSORS)
    case SENSOR1_DATA:
      payload.reset();
      payload.addSensor(sensor_read(1));
      SendPayload(SENSOR1PORT, prio_normal);
      break;
    case SENSOR2_DATA:
      payload.reset();
      payload.addSensor(sensor_read(2));
      SendPayload(SENSOR2PORT, prio_normal);
      break;
    case SENSOR3_DATA:
      payload.reset();
      payload.addSensor(sensor_read(3));
      SendPayload(SENSOR3PORT, prio_normal);
      break;
#endif

#if (defined BAT_MEASURE_ADC || defined HAS_PMU)
    case BATT_DATA:
      payload.reset();
      payload.addVoltage(read_voltage());
      SendPayload(BATTPORT, prio_normal);
      break;
#endif
    } // switch

    bitmask &= ~mask;
    mask <<= 1;
  } // while

// === ADEMUX: Health check LoRa (cada HEALTHCHECK_INTERVAL_MINUTES) ===
  {
    static unsigned long lastHealthCheck = 0;
    if ((millis() - lastHealthCheck) >= (HEALTHCHECK_INTERVAL_MINUTES * 60 * 1000)) {
      lastHealthCheck = millis();

      uint32_t uptime = (uint32_t)(millis() / 1000);
      uint8_t cputemp = (uint8_t)round(temperatureRead());
      uint16_t free_heap_div16 = (uint16_t)(ESP.getFreeHeap() / 16);
      uint16_t min_heap_div16 = (uint16_t)(ESP.getMinFreeHeap() / 16);
      uint8_t reset_reason = (uint8_t)esp_reset_reason();

      uint8_t flags1 = 0;
      flags1 |= (cfg.wifiscan ? 1 : 0) << 7;
      flags1 |= (cfg.blescan ? 1 : 0) << 6;
      flags1 |= (cfg.btscan ? 1 : 0) << 5;
#if (HAS_LORA)
      flags1 |= (LMIC.devaddr ? 1 : 0) << 4;
#endif
#if (HAS_NBIOT)
      flags1 |= (nb_isEnabled() ? 1 : 0) << 3;
#endif
#ifdef HAS_SDCARD
      flags1 |= (1) << 2;
#endif
#if (HAS_GPS)
      flags1 |= (gps_hasfix() ? 1 : 0) << 1;
#endif

      uint8_t flags2 = 0;
#if (HAS_LORA)
      flags2 = healthcheck_failures;
#endif

      uint8_t lora_rssi = 0;
      int8_t lora_snr = 0;
#if (HAS_LORA)
      lora_rssi = (uint8_t)(LMIC.rssi < 0 ? -LMIC.rssi : LMIC.rssi);
      lora_snr = (int8_t)LMIC.snr;
#endif

      uint8_t nb_rssi = 99;
      uint8_t nb_failures = 0;
#if (HAS_NBIOT)
      nb_rssi = nb_status_rssi;
      nb_failures = nb_status_failures;
#endif

      uint8_t flags3 = 0;
#if (HAS_NBIOT)
      flags3 |= (nb_status_registered ? 1 : 0) << 7;
      flags3 |= (nb_status_connected ? 1 : 0) << 6;
#endif
      uint8_t cpu_freq_code = 0;
      int cpuMHz = getCpuFrequencyMhz();
      if (cpuMHz <= 80) cpu_freq_code = 0;
      else if (cpuMHz <= 160) cpu_freq_code = 1;
      else cpu_freq_code = 2;
      flags3 |= (cpu_freq_code & 0x03) << 4;
      flags3 |= (lastSendChannel & 0x03) << 2;
      flags3 |= (bt_module_ok ? 1 : 0) << 1;
      flags3 |= (ble_module_ok ? 1 : 0) << 0;

      payload.reset();
      payload.addStatus(uptime, cputemp, free_heap_div16, min_heap_div16,
                        reset_reason, flags1, flags2,
                        lora_rssi, lora_snr,
                        nb_rssi, nb_failures, flags3);

      // Enviar por LoRa (confirmed uplink para health check)
      SendPayload(TELEMETRYPORT, prio_normal);

      // Enviar siempre también por NB-IoT
#if (HAS_NBIOT)
      {
        MessageBuffer_t nbMessage;
        nbMessage.MessageSize = payload.getSize();
        nbMessage.MessagePort = TELEMETRYPORT;
        nbMessage.MessagePrio = prio_normal;
        memcpy(nbMessage.Message, payload.getBuffer(), payload.getSize());
        nb_enqueuedata(&nbMessage);
      }
#endif

      ESP_LOGI(TAG, "Health check [Up:%u T:%u Heap:%u/%u Rst:%u F1:0x%02X F2:0x%02X RSSI:%u SNR:%d NbR:%u NbF:%u F3:0x%02X]",
               uptime, cputemp, free_heap_div16 * 16, min_heap_div16 * 16,
               reset_reason, flags1, flags2, lora_rssi, lora_snr,
               nb_rssi, nb_failures, flags3);
    }
  }

// === ADEMUX: Health check NB-IoT independiente (cada NB_HEALTHCHECK_INTERVAL_MINUTES) ===
#if (HAS_NBIOT)
  {
    static unsigned long lastNbHealthCheck = 0;
    if ((millis() - lastNbHealthCheck) >= (NB_HEALTHCHECK_INTERVAL_MINUTES * 60 * 1000)) {
      lastNbHealthCheck = millis();

      uint32_t uptime = (uint32_t)(millis() / 1000);
      uint8_t cputemp = (uint8_t)round(temperatureRead());
      uint16_t free_heap_div16 = (uint16_t)(ESP.getFreeHeap() / 16);
      uint16_t min_heap_div16 = (uint16_t)(ESP.getMinFreeHeap() / 16);
      uint8_t reset_reason = (uint8_t)esp_reset_reason();

      uint8_t flags1 = 0;
      flags1 |= (cfg.wifiscan ? 1 : 0) << 7;
      flags1 |= (cfg.blescan ? 1 : 0) << 6;
      flags1 |= (cfg.btscan ? 1 : 0) << 5;
#if (HAS_LORA)
      flags1 |= (LMIC.devaddr ? 1 : 0) << 4;
#endif
      flags1 |= (nb_isEnabled() ? 1 : 0) << 3;
#ifdef HAS_SDCARD
      flags1 |= (1) << 2;
#endif
#if (HAS_GPS)
      flags1 |= (gps_hasfix() ? 1 : 0) << 1;
#endif

      uint8_t flags2 = 0;
#if (HAS_LORA)
      flags2 = healthcheck_failures;
#endif

      uint8_t lora_rssi = 0;
      int8_t lora_snr = 0;
#if (HAS_LORA)
      lora_rssi = (uint8_t)(LMIC.rssi < 0 ? -LMIC.rssi : LMIC.rssi);
      lora_snr = (int8_t)LMIC.snr;
#endif

      uint8_t nb_rssi = nb_status_rssi;
      uint8_t nb_failures = nb_status_failures;

      uint8_t flags3 = 0;
      flags3 |= (nb_status_registered ? 1 : 0) << 7;
      flags3 |= (nb_status_connected ? 1 : 0) << 6;
      uint8_t cpu_freq_code = 0;
      int cpuMHz = getCpuFrequencyMhz();
      if (cpuMHz <= 80) cpu_freq_code = 0;
      else if (cpuMHz <= 160) cpu_freq_code = 1;
      else cpu_freq_code = 2;
      flags3 |= (cpu_freq_code & 0x03) << 4;
      flags3 |= (lastSendChannel & 0x03) << 2;
      flags3 |= (bt_module_ok ? 1 : 0) << 1;
      flags3 |= (ble_module_ok ? 1 : 0) << 0;

      payload.reset();
      payload.addStatus(uptime, cputemp, free_heap_div16, min_heap_div16,
                        reset_reason, flags1, flags2,
                        lora_rssi, lora_snr,
                        nb_rssi, nb_failures, flags3);

      MessageBuffer_t nbMessage;
      nbMessage.MessageSize = payload.getSize();
      nbMessage.MessagePort = TELEMETRYPORT;
      nbMessage.MessagePrio = prio_normal;
      memcpy(nbMessage.Message, payload.getBuffer(), payload.getSize());
      nb_enqueuedata(&nbMessage);

      ESP_LOGI(TAG, "NB health check [Up:%u T:%u Heap:%u/%u Rst:%u F1:0x%02X F2:0x%02X RSSI:%u SNR:%d NbR:%u NbF:%u F3:0x%02X]",
               uptime, cputemp, free_heap_div16 * 16, min_heap_div16 * 16,
               reset_reason, flags1, flags2, lora_rssi, lora_snr,
               nb_rssi, nb_failures, flags3);
    }
  }
#endif

} // sendData()


void checkQueue() {
#if (HAS_LORA && HAS_NBIOT)
  long loraMessages = get_lora_queue_pending_messages();
  MessageBuffer_t SendBuffer;
  auto loraQueueHandle = lora_get_queue_handle();

  if (nb_data_mode && loraMessages >= MIN_SEND_MESSAGES_THRESHOLD) {
    ESP_LOGI(TAG, "NB data mode active, transferring %d LoRa messages to NB-IoT", loraMessages);
    while (uxQueueMessagesWaitingFromISR(loraQueueHandle) > 0) {
      if (xQueueReceive(loraQueueHandle, &SendBuffer, portMAX_DELAY) == pdTRUE)
        nb_enqueuedata(&SendBuffer);
    }
  }
  else if (loraMessages >= NB_FAILOVER_MESSAGES_THRESHOLD) {
    ESP_LOGI(TAG, "LoRa queue threshold reached, sending %d messages through NB", loraMessages);
    nb_enable(true);
    while (uxQueueMessagesWaitingFromISR(loraQueueHandle) > 0) {
      if (xQueueReceive(loraQueueHandle, &SendBuffer, portMAX_DELAY) == pdTRUE)
        nb_enqueuedata(&SendBuffer);
    }
  }
#endif
}

void flushQueues() {
#if (HAS_LORA)
  lora_queuereset();
#endif
#if (HAS_NBIOT)
  nb_queuereset();
#endif
#ifdef HAS_SPI
  spi_queuereset();
#endif
}

void printQueueStats() {
#if (HAS_LORA)
    ESP_LOGI(TAG, "LoRa queue: %u/%u messages",
             uxQueueMessagesWaiting(lora_get_queue_handle()),
             SEND_QUEUE_SIZE);
#endif
#if (HAS_NBIOT)
    extern QueueHandle_t nb_get_queue_handle();
    ESP_LOGI(TAG, "NB-IoT queue: %u/%u messages",
             uxQueueMessagesWaiting(nb_get_queue_handle()),
             SEND_QUEUE_SIZE);
#endif
#ifdef HAS_SDCARD
    if (isSDCardAvailable()) {
        ESP_LOGI(TAG, "SD queue: %u messages", sdqueueCount());
    }
#endif
}