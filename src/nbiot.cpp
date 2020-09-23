#include "nbiot.h"

// Local logging Tag
static const char TAG[] = "nbiot";

QueueHandle_t NbSendQueue;
TaskHandle_t nbIotTask = NULL;
ConfigBuffer_t nbConfig;

unsigned long lastMessage;

Ticker nbticker;

bool nb_enqueuedata(MessageBuffer_t *message) {
  // enqueue message in LORA send queue
  bool enqueued = false;
  BaseType_t ret = pdFALSE;
  MessageBuffer_t DummyBuffer;
  sendprio_t prio = message->MessagePrio;

  switch (prio) {
  case prio_high:
    // clear some space in queue if full, then fallthrough to prio_normal
    if (uxQueueSpacesAvailable(NbSendQueue) == 0) {
      xQueueReceive(NbSendQueue, &DummyBuffer, (TickType_t)0);
    }
  case prio_normal:
    ret = xQueueSendToFront(NbSendQueue, (void *)message, (TickType_t)0);
    break;
  case prio_low:
  default:
    ret = xQueueSendToBack(NbSendQueue, (void *)message, (TickType_t)0);
    break;
  }
  if (ret != pdTRUE) {
    snprintf(lmic_event_msg + 14, LMIC_EVENTMSG_LEN - 14, "<>");
    ESP_LOGW(TAG, "NBIOT sendqueue is full");
  } else {
    ESP_LOGI(TAG, "NBIOT message enqueued");
  }
  return enqueued;
}

void connectModem() {
  bool done = false;
  while (!done) {
    resetModem();
    configModem();
    unsigned long t0 = millis();
    ESP_LOGI(TAG, "Try connecting");
    while (millis() < t0 + 60000) {
      if (networkReady()) {
        if (attachNetwork()) {
          done = true;
          break;
        }
        delay(1000);
      } else {
        ESP_LOGI(TAG, "Wait for network");
        delay(5000);
      }
    }
  }
}

uint32_t getUint32FromBuffer(uint8_t *buffer) {
  return ((buffer[0] << 24) & 0xFF000000) + ((buffer[1] << 16) & 0x00FF0000) +
         ((buffer[2] << 8) & 0x0000FF00) + (buffer[3] & 0x000000FF);
}

uint16_t getCount(uint8_t *buffer) {
  return ((buffer[0] << 8) & 0xFF00) + (buffer[1] & 0x00FF);
}

void nb_send(void *pvParameters) {
  configASSERT(((uint32_t)pvParameters) == 1); // FreeRTOS check
  initModem();
  while (1) {
    nb_loop();
    delay(1000);
  }
}

void getSentiloTimestamp(char* buffer, uint32_t timestamp)
{
  sprintf(buffer, "%02d/%02d/%4d'T'%02d:%02d:%02d", day(timestamp), month(timestamp), year(timestamp), hour(timestamp), minute(timestamp), second(timestamp));
}

void nb_loop() {
  MessageBuffer_t SendBuffer;
  if (millis() - lastMessage > MIN_SEND_TIME_THRESHOLD && uxQueueMessagesWaitingFromISR(NbSendQueue) > 0) {
    ConfigBuffer_t conf;
    sdLoadNbConfig(&conf);
    if(strlen(conf.BaseUrl) < 5)
    {
      ESP_LOGE(TAG, "Error in NB config, cant send");
      return;
    }
    ESP_LOGV(TAG, "NB messages pending, sending");
    // fetch next or wait for payload to send from queue

    char url[100];
    url[0] = 0;
    strcat(url, conf.Path);
    //strcat(url, conf.ComponentName);

    const size_t capacity = 4096;
    DynamicJsonDocument doc(capacity);

    JsonArray sensors = doc.createNestedArray("sensors");
    JsonObject wifiHashSensor = sensors.createNestedObject();
    JsonObject wifiCountSensor = sensors.createNestedObject();
    JsonObject bleHashSensor = sensors.createNestedObject();
    JsonObject bleCountSensor = sensors.createNestedObject();
    JsonObject btHashSensor = sensors.createNestedObject();
    JsonObject btCountSensor = sensors.createNestedObject();

    wifiHashSensor["sensor"] =
        String(conf.ComponentName) + String(conf.WifiHashSensor);
    wifiCountSensor["sensor"] =
        String(conf.ComponentName) + String(conf.WifiCountSensor);
    bleHashSensor["sensor"] = String(conf.ComponentName) + String(conf.BleHashSensor);
    bleCountSensor["sensor"] =
        String(conf.ComponentName) + String(conf.BleCountSensor);
    btHashSensor["sensor"] = String(conf.ComponentName) + String(conf.BtHashSensor);
    btCountSensor["sensor"] = String(conf.ComponentName) + String(conf.BtCountSensor);

    JsonArray wifiHashObs = wifiHashSensor.createNestedArray("observations");
    JsonArray wifiCountObs = wifiCountSensor.createNestedArray("observations");
    JsonArray bleHashObs = bleHashSensor.createNestedArray("observations");
    JsonArray bleCountObs = bleCountSensor.createNestedArray("observations");
    JsonArray btHashObs = btHashSensor.createNestedArray("observations");
    JsonArray btCountObs = btCountSensor.createNestedArray("observations");

    connectModem();
    int msgCounter = 0;
    bool msgCompleted = false;
    while (msgCounter <= MAX_NB_MESSAGES &&  uxQueueMessagesWaiting(NbSendQueue) > 0 ) {
      xQueueReceive(NbSendQueue, &SendBuffer, portMAX_DELAY);
      msgCounter++;
      switch (SendBuffer.MessagePort) {
      case COUNTERPORT: {
        ESP_LOGI(TAG, "COUNT PORT");
        uint32_t timestamp = getUint32FromBuffer(SendBuffer.Message);
        uint16_t countWifi = getCount(SendBuffer.Message + 4);
        uint16_t countBle = getCount(SendBuffer.Message + 6);
        uint16_t countBt = getCount(SendBuffer.Message + 8);

        char sentiloTimestamp[36];
        getSentiloTimestamp(sentiloTimestamp, timestamp);

        JsonObject wifiCountObsValues = wifiCountObs.createNestedObject();
        JsonObject bleCountObsValues = bleCountObs.createNestedObject();
        JsonObject btCountObsValues = btCountObs.createNestedObject();

        wifiCountObsValues["value"] = countWifi;
        wifiCountObsValues["timestamp"] = sentiloTimestamp;
        bleCountObsValues["value"] = countBle;
        bleCountObsValues["timestamp"] = sentiloTimestamp;
        btCountObsValues["value"] = countBt;
        btCountObsValues["timestamp"] = sentiloTimestamp;
        msgCompleted = true;
        break;
      }
      case BTMACSPORT: {
        ESP_LOGI(TAG, "BTMACS");
        uint32_t timestamp = getUint32FromBuffer(SendBuffer.Message);

        char sentiloTimestamp[36];
        getSentiloTimestamp(sentiloTimestamp, timestamp);

        uint16_t macCount = (SendBuffer.MessageSize - 4) / 4;
        for (int i = 0; i < macCount; i++) {
          uint32_t mac = getUint32FromBuffer(SendBuffer.Message + 4 + 4 * i);
          JsonObject btHashObsValues = btHashObs.createNestedObject();
          char buff[10];
          sprintf(buff, "%08X", mac);
          btHashObsValues["value"] = buff;
          btHashObsValues["timestamp"] = sentiloTimestamp;
        }
        msgCompleted = true;
        break;
      }
      case BLEMACSPORT: {
        ESP_LOGI(TAG, "BLEMACS");
        uint32_t timestamp = getUint32FromBuffer(SendBuffer.Message);

        char sentiloTimestamp[36];
        getSentiloTimestamp(sentiloTimestamp, timestamp);

        uint16_t macCount = (SendBuffer.MessageSize - 4) / 4;
        for (int i = 0; i < macCount; i++) {
          uint32_t mac = getUint32FromBuffer(SendBuffer.Message + 4 + 4 * i);
          JsonObject bleHashObsValues = bleHashObs.createNestedObject();
          char buff[10];
          sprintf(buff, "%08X", mac);
          bleHashObsValues["value"] = buff;
          bleHashObsValues["timestamp"] = sentiloTimestamp;
        }
        msgCompleted = true;
        break;
      }
      case WIFIMACSPORT: {
        ESP_LOGI(TAG, "WIFIMACS");
        uint32_t timestamp = getUint32FromBuffer(SendBuffer.Message);

        char sentiloTimestamp[36];
        getSentiloTimestamp(sentiloTimestamp, timestamp);

        uint16_t macCount = (SendBuffer.MessageSize - 4) / 4;
        for (int i = 0; i < macCount; i++) {
          uint32_t mac = getUint32FromBuffer(SendBuffer.Message + 4 + 4 * i);
          JsonObject wifiHashObsValues = wifiHashObs.createNestedObject();
          char buff[10];
          sprintf(buff, "%08X", mac);
          wifiHashObsValues["value"] = buff;
          wifiHashObsValues["timestamp"] = sentiloTimestamp;
        }
        msgCompleted = true;
        break;
      }
      default:
        break;
      }
    }
    if (msgCompleted) {
      ESP_LOGI(TAG, "Message complete");
      char buffer[8192];
      serializeJson(doc, buffer);
      ESP_LOGI(TAG, "JSON: %s", buffer);

      int res = postPage(conf.BaseUrl, conf.port, url, buffer, conf.IdentityKey);
      while (res != 200) {
        delay(5000);
        connectModem();
        res = postPage(conf.BaseUrl, conf.port, url, buffer, conf.IdentityKey);
      }
      ESP_LOGI(TAG, "Finish sending message");
    }
  }
}

esp_err_t nb_iot_init() {
  assert(NB_QUEUE_SIZE);
  NbSendQueue = xQueueCreate(NB_QUEUE_SIZE, sizeof(MessageBuffer_t));
  if (NbSendQueue == 0) {
    ESP_LOGE(TAG, "Could not create NBIOT send queue. Aborting.");
    return ESP_FAIL;
  }
  ESP_LOGI(TAG, "NBIOT send queue created, size %d Bytes",
           SEND_QUEUE_SIZE * sizeof(MessageBuffer_t));

  // start lorawan stack
  ESP_LOGI(TAG, "Starting NBIOT TASK...");
  lastMessage = millis();
  xTaskCreatePinnedToCore(nb_send,   // task function
                          "nbtask", // name of task
                          16384,       // stack size of task
                          (void *)1,  // parameter of the task
                          1,          // priority of the task
                          &nbIotTask,  // task handle
                          1);         // CPU core

  // Start join procedure if not already joined,
  // lora_setupForNetwork(true) is called by eventhandler when joined
  // else continue current session
  return ESP_OK;
}

void nb_queuereset() { xQueueReset(NbSendQueue); }
