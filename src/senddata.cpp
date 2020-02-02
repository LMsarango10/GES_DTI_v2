// Basic Config
#include "senddata.h"

Ticker sendcycler;
bool sent = false;

void sendcycle() {
  xTaskNotifyFromISR(irqHandlerTask, SENDCYCLE_IRQ, eSetBits, NULL);
}

// put data to send in RTos Queues used for transmit over channels Lora and SPI
void SendPayload(uint8_t port, sendprio_t prio) {

  MessageBuffer_t
      SendBuffer; // contains MessageSize, MessagePort, MessagePrio, Message[]

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
#endif
#ifdef HAS_SPI
  spi_enqueuedata(&SendBuffer);
#endif

// write data to sdcard, if present
#ifdef HAS_SDCARD
//enqueued = false; //DEBUG
if (!enqueued)
  sdcardWriteFrame(&SendBuffer);
#endif

} // SendPayload

// interrupt triggered function to prepare payload to send
void sendData() {
  time_t tstamp;
  tstamp = get_rtctime();
  float temp = get_rtctemp();
  ESP_LOGD(TAG, "timestamp is %lu", tstamp);
  ESP_LOGD(TAG, "rtc temp is: %f", temp);
  uint8_t bitmask = cfg.payloadmask;
  uint8_t mask = 1;
  uint8_t* bfs;
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
      if (cfg.wifiscan)
        payload.addCount(macs_wifi, MAC_SNIFF_WIFI);
      if (cfg.blescan)
        payload.addCount(macs_ble, MAC_SNIFF_BLE);
      if (cfg.btscan)
        payload.addCount(macs_bt, MAC_SNIFF_BT);
#endif
#if (HAS_GPS)
      if (GPSPORT == COUNTERPORT) {
        // send GPS position only if we have a fix
        if (gps_hasfix()) {
          gps_storelocation(&gps_status);
          payload.addGPS(gps_status);
        } else
          ESP_LOGD(TAG, "No valid GPS position");
      }
#endif
#if (PAYLOAD_OPENSENSEBOX)
      if (cfg.wifiscan)
        payload.addCount(macs_wifi, MAC_SNIFF_WIFI);
      if (cfg.blescan)
        payload.addCount(macs_ble, MAC_SNIFF_BLE);
      if (cfg.btscan)
        payload.addCount(macs_bt, MAC_SNIFF_BT);
#endif
      SendPayload(COUNTERPORT, prio_high);
      ESP_LOGI(TAG, "enqueue mac counter");
      if (cfg.wifiscan)
      {
        std::vector<uint32_t> macs_vector;
        uint32_t dummy_mac;
        for (auto m : macs_list_wifi) {
          // ESP_LOGI(TAG, "%X", m);
          macs_vector.push_back(m);
        }
        // Space for Dummy MAC generation:
        // change 100 for the amount of MACs desired in a 2 minute interval.
        if(!sent){
          for (int i =0; i < 400; i++)
          {
            dummy_mac=(uint32_t)random(10000);
            macs_vector.push_back(dummy_mac);
          }//hasta aqui*/
          sent = true;
        }

        uint16_t total_macs = macs_vector.size();
        ESP_LOGI(TAG, "Total MAC counter currently is at: %d", total_macs);
        while (total_macs != 0) {
          uint16_t macs_to_send = 0;
          if (total_macs <= 11) {
            macs_to_send = total_macs;
          } else {
            macs_to_send = 11;
          }
          total_macs -= macs_to_send;
          payload.reset();
          //payload.setupMac(macs_wifi);
          payload.addTime(tstamp);
          for (int i = 0; i < macs_to_send; i++) {
            payload.addMac(macs_vector.back());
            macs_vector.pop_back();
          }
          ESP_LOGI(TAG, "Macs being sent in this payload: %d", macs_to_send);
          ESP_LOGI(TAG, "Lenght of Payload is: %d", sizeof(payload));
          SendPayload(WIFIMACSPORT, prio_low);
          ESP_LOGI(TAG, "enqueue mac message");
        }
      }
      if (cfg.blescan)
      {
        std::vector<uint32_t> macs_vector;
        uint32_t dummy_mac;
        for (auto m : macs_list_ble) {
          // ESP_LOGI(TAG, "%X", m);
          macs_vector.push_back(m);
        }
        // Space for Dummy MAC generation:
        // change 100 for the amount of MACs desired in a 2 minute interval.
        /*for (int i =0; i < 400; i++)
        {
          dummy_mac=(uint32_t)random(10000);
          macs_vector.push_back(dummy_mac);
        }//hasta aqui*/
        uint16_t total_macs = macs_vector.size();
        ESP_LOGI(TAG, "Total MAC counter currently is at: %d", total_macs);
        while (total_macs != 0) {
          uint16_t macs_to_send = 0;
          if (total_macs <= 11) {
            macs_to_send = total_macs;
          } else {
            macs_to_send = 11;
          }
          total_macs -= macs_to_send;
          payload.reset();
          //payload.setupMac(macs_wifi);
          payload.addTime(tstamp);
          for (int i = 0; i < macs_to_send; i++) {
            payload.addMac(macs_vector.back());
            macs_vector.pop_back();
          }
          ESP_LOGI(TAG, "Macs being sent in this payload: %d", macs_to_send);
          ESP_LOGI(TAG, "Lenght of Payload is: %d", sizeof(payload));
          SendPayload(BLEMACSPORT, prio_low);
          ESP_LOGI(TAG, "enqueue mac message");
        }
      }
      if (cfg.btscan)
      {
        std::vector<uint32_t> macs_vector;
        uint32_t dummy_mac;
        for (auto m : macs_list_wifi) {
          // ESP_LOGI(TAG, "%X", m);
          macs_vector.push_back(m);
        }
        // Space for Dummy MAC generation:
        // change 100 for the amount of MACs desired in a 2 minute interval.
        /*for (int i =0; i < 400; i++)
        {
          dummy_mac=(uint32_t)random(10000);
          macs_vector.push_back(dummy_mac);
        }//hasta aqui*/
        uint16_t total_macs = macs_vector.size();
        ESP_LOGI(TAG, "Total MAC counter currently is at: %d", total_macs);
        while (total_macs != 0) {
          uint16_t macs_to_send = 0;
          if (total_macs <= 11) {
            macs_to_send = total_macs;
          } else {
            macs_to_send = 11;
          }
          total_macs -= macs_to_send;
          payload.reset();
          //payload.setupMac(macs_wifi);
          payload.addTime(tstamp);
          for (int i = 0; i < macs_to_send; i++) {
            payload.addMac(macs_vector.back());
            macs_vector.pop_back();
          }
          ESP_LOGI(TAG, "Macs being sent in this payload: %d", macs_to_send);
          ESP_LOGI(TAG, "Lenght of Payload is: %d", sizeof(payload));
          SendPayload(BTMACSPORT, prio_low);
          ESP_LOGI(TAG, "enqueue mac message");
        }
      }
      // clear counter if not in cumulative counter mode
      if (cfg.countermode != 1) {
        reset_counters(); // clear macs container and reset all counters
        get_salt();       // get new salt for salting hashes
        ESP_LOGI(TAG, "Counter cleared");
      }
#ifdef HAS_DISPLAY
      else
        oledPlotCurve(macs_total, true);
#endif
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
        // send GPS position only if we have a fix
        if (gps_hasfix()) {
          gps_storelocation(&gps_status);
          payload.reset();
          payload.addGPS(gps_status);
          SendPayload(GPSPORT, prio_high);
        } else
          ESP_LOGD(TAG, "No valid GPS position");
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
  } // while (bitmask)

} // sendData()

void flushQueues() {
#if (HAS_LORA)
  lora_queuereset();
#endif
#ifdef HAS_SPI
  spi_queuereset();
#endif
}