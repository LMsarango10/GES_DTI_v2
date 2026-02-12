
// Basic Config
#include "senddata.h"
static const char TAG[] = "senddata";

Ticker sendcycler;
bool sent = false;

void sendcycle() {
  xTaskNotifyFromISR(irqHandlerTask, SENDCYCLE_IRQ, eSetBits, NULL);
}

// put data to send in RTos Queues used for transmit over channels Lora and SPI
void SendPayload(uint8_t port, sendprio_t prio) {

  MessageBuffer_t SendBuffer; // contains MessageSize, MessagePort, MessagePrio, Message[]
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

  // enqueue message in device's send queues
#if (HAS_LORA)
  bool enqueued = lora_enqueuedata(&SendBuffer);
  if (!enqueued) {
    ESP_LOGW(TAG, "LoRa queue full (port %u, %u bytes)", 
             SendBuffer.MessagePort, SendBuffer.MessageSize);
    
#if (HAS_NBIOT)
    nb_enable(true);
    if (!nb_enqueuedata(&SendBuffer)) {
      ESP_LOGW(TAG, "NB-IoT also failed to enqueue");
#ifdef HAS_SDCARD
      if (isSDCardAvailable()) {
        sdqueueEnqueue(&SendBuffer);
        ESP_LOGI(TAG, "Message saved to SD (port %u, %u bytes)", 
                 SendBuffer.MessagePort, SendBuffer.MessageSize);
      } else {
        ESP_LOGE(TAG, "SD not available - MESSAGE LOST!");
      }
#endif
    } else {
      ESP_LOGI(TAG, "Message routed to NB-IoT");
    }
#else
#ifdef HAS_SDCARD
    if (isSDCardAvailable()) {
      sdqueueEnqueue(&SendBuffer);
      ESP_LOGI(TAG, "Message saved to SD (LoRa full, port %u, %u bytes)", 
               SendBuffer.MessagePort, SendBuffer.MessageSize);
    } else {
      ESP_LOGE(TAG, "SD not available - MESSAGE LOST!");
    }
#endif
#endif
  }
#endif

#if (!HAS_LORA && HAS_NBIOT)
  bool enqueued = nb_enqueuedata(&SendBuffer);
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
  
  // AÑADIR CADA N CICLOS:
//  static uint8_t cycle_counter = 0;
  //if (++cycle_counter >= 10) {  // Cada 10 ciclos
    //  cycle_counter = 0;
     // printQueueStats();
 // }

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

      // ---- MAC lists messages (unchanged logic) ----
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
} // sendData()


void checkQueue() {
#if (HAS_LORA && HAS_NBIOT)
  long loraMessages = get_lora_queue_pending_messages();
  MessageBuffer_t SendBuffer;
  auto loraQueueHandle = lora_get_queue_handle();

  // Verificamos si debemos activar el "Plan B" (NB-IoT)
  if (nb_isEnabled() && loraMessages >= MIN_SEND_MESSAGES_THRESHOLD) {
    ESP_LOGI(TAG, "LoRa saturado (%d msgs) -> Intentando derivar a NB-IoT...", loraMessages);
    
    // Drenamos la cola de LoRa
    while (uxQueueMessagesWaitingFromISR(loraQueueHandle) > 0) {
      if (xQueueReceive(loraQueueHandle, &SendBuffer, portMAX_DELAY) == pdTRUE) {
        
        // INTENTAMOS ENVIAR A NB-IOT
        if (!nb_enqueuedata(&SendBuffer)) {
            // Si nb_enqueuedata devuelve false, es que ni RAM ni SD funcionaron.
            // (Con la corrección 1, esto es muy difícil que pase, pero prevenimos)
            ESP_LOGE(TAG, "Fallo al derivar mensaje (Rechazado por NB y SD)");
            
            // Opcional: Podríamos intentar guardarlo en SD aquí directamente si no confiamos en nb_enqueuedata
            #ifdef HAS_SDCARD
            if (isSDCardAvailable()) sdqueueEnqueue(&SendBuffer);
            #endif
        }
      }
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

// Estadísticas de colas (útil para debug)
void printQueueStats() {
#if (HAS_LORA)
    ESP_LOGI(TAG, "📊 LoRa queue: %u/%u messages", 
             uxQueueMessagesWaiting(lora_get_queue_handle()),
             SEND_QUEUE_SIZE);
#endif

#if (HAS_NBIOT)
    extern QueueHandle_t nb_get_queue_handle();
    ESP_LOGI(TAG, "📊 NB-IoT queue: %u/%u messages", 
             uxQueueMessagesWaiting(nb_get_queue_handle()),
             SEND_QUEUE_SIZE);
#endif

#ifdef HAS_SDCARD
    if (isSDCardAvailable()) {
        ESP_LOGI(TAG, "📊 SD queue: %u messages", sdqueueCount());
    }
#endif
}