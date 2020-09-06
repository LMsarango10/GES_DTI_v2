#include "nbiot.h"

// Local logging Tag
static const char TAG[] = "nbiot";

QueueHandle_t NbSendQueue;
TaskHandle_t nbIotTask = NULL;

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

  while (1) {
    nb_loop();
    delay(1000);
  }
}

void nb_loop() {
  MessageBuffer_t SendBuffer;
  ESP_LOGV(TAG, "Checking NB loop");
  if (millis() - lastMessage > MIN_SEND_TIME_THRESHOLD && uxQueueMessagesWaitingFromISR(NbSendQueue) > 0) {
    ESP_LOGV(TAG, "NB messages pending, sending");
    // fetch next or wait for payload to send from queue
    char wifiHashSensorName[] = "S01";
    char wifiCountSensorName[] = "S02";
    char bleHashSensorName[] = "S03";
    char bleCountSensorName[] = "S04";
    char btHashSensorName[] = "S05";
    char btCountSensorName[] = "S06";

    char componentName[] = "0004A30B005E67EA_";
    char url[] = "/data/gesinen_provider/";
    char serverIp[] = "82.223.2.207";
    long port = 8081;

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
        String(componentName) + String(wifiHashSensorName);
    wifiCountSensor["sensor"] =
        String(componentName) + String(wifiCountSensorName);
    bleHashSensor["sensor"] = String(componentName) + String(bleHashSensorName);
    bleCountSensor["sensor"] =
        String(componentName) + String(bleCountSensorName);
    btHashSensor["sensor"] = String(componentName) + String(btHashSensorName);
    btCountSensor["sensor"] = String(componentName) + String(btCountSensorName);

    JsonArray wifiHashObs = wifiHashSensor.createNestedArray("observations");
    JsonArray wifiCountObs = wifiCountSensor.createNestedArray("observations");
    JsonArray bleHashObs = bleHashSensor.createNestedArray("observations");
    JsonArray bleCountObs = bleCountSensor.createNestedArray("observations");
    JsonArray btHashObs = btHashSensor.createNestedArray("observations");
    JsonArray btCountObs = btCountSensor.createNestedArray("observations");

    initModem();
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
        // inicialmente sin timestamp
        JsonObject wifiCountObsValues = wifiCountObs.createNestedObject();
        JsonObject bleCountObsValues = bleCountObs.createNestedObject();
        JsonObject btCountObsValues = btCountObs.createNestedObject();

        wifiCountObsValues["value"] = countWifi;
        bleCountObsValues["value"] = countBle;
        btCountObsValues["value"] = countBt;
        msgCompleted = true;
        break;
      }
      case BTMACSPORT: {
        ESP_LOGI(TAG, "BTMACS");
        uint32_t timestamp = getUint32FromBuffer(SendBuffer.Message);
        uint16_t macCount = (SendBuffer.MessageSize - 4) / 4;
        for (int i = 0; i < macCount; i++) {
          uint32_t mac = getUint32FromBuffer(SendBuffer.Message + 4 + 4 * i);
          JsonObject btHashObsValues = btHashObs.createNestedObject();
          char buff[10];
          sprintf(buff, "%08X", mac);
          btHashObsValues["value"] = buff;
        }
        msgCompleted = true;
        break;
      }
      case BLEMACSPORT: {
        ESP_LOGI(TAG, "BLEMACS");
        uint32_t timestamp = getUint32FromBuffer(SendBuffer.Message);
        uint16_t macCount = (SendBuffer.MessageSize - 4) / 4;
        for (int i = 0; i < macCount; i++) {
          uint32_t mac = getUint32FromBuffer(SendBuffer.Message + 4 + 4 * i);
          JsonObject bleHashObsValues = bleHashObs.createNestedObject();
          char buff[10];
          sprintf(buff, "%08X", mac);
          bleHashObsValues["value"] = buff;
        }
        msgCompleted = true;
        break;
      }
      case WIFIMACSPORT: {
        ESP_LOGI(TAG, "WIFIMACS");
        uint32_t timestamp = getUint32FromBuffer(SendBuffer.Message);
        uint16_t macCount = (SendBuffer.MessageSize - 4) / 4;
        for (int i = 0; i < macCount; i++) {
          uint32_t mac = getUint32FromBuffer(SendBuffer.Message + 4 + 4 * i);
          JsonObject wifiHashObsValues = wifiHashObs.createNestedObject();
          char buff[10];
          sprintf(buff, "%08X", mac);
          wifiHashObsValues["value"] = buff;
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
      serializeJsonPretty(doc, buffer);
      ESP_LOGI(TAG, "JSON: %s", buffer);

      int res = 200; // postPage(serverIp, port, url, buffer);
      while (res != 200) {
        connectModem();
        postPage(serverIp, port, url, buffer);
        delay(5000);
      }
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
                          12288,       // stack size of task
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
