#include "nbiot.h"

// Local logging Tag
static const char TAG[] = "nbiot";

QueueHandle_t NbSendQueue;
TaskHandle_t nbIotTask = NULL;

unsigned long lastMessage;

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

uint32_t getUint32FromBuffer(uint8_t *buffer) {
  return ((buffer[0] << 24) & 0xFF000000) + ((buffer[1] << 16) & 0x00FF0000) +
         ((buffer[2] << 8) & 0x0000FF00) + (buffer[3] & 0x000000FF);
}

uint16_t getCount(uint8_t *buffer) {
  return ((buffer[0] << 8) & 0xFF00) + (buffer[1] & 0x00FF);
}

bool NbIotManager::nb_checkLastSoftwareVersion()
{
  char buff[2048];
  int responseSize = 0;
  lastUpdateCheck = millis();
  if(getData(UPDATES_SERVER_IP, UPDATES_SERVER_PORT,UPDATES_SERVER_INDEX, buff, sizeof(buff), &responseSize) >= 0)
  {
    ESP_LOGD(TAG, "INDEX: %s", buff);
    ESP_LOGD(TAG, "DATALEN: %d", responseSize);
    strcpy(updatesServerResponse, buff);

    if (responseSize <= 0) {
      ESP_LOGD(TAG, "No response from server");
      return false;
    }

    ESP_LOGD(TAG, "Current Version: %s", PROGVERSION);
    std::string bufferString = std::string(updatesServerResponse);
    std::size_t found = bufferString.find("\r\n");
    if (found != std::string::npos)
    {
      std::string version = bufferString.substr(0, found);
      ESP_LOGD(TAG, "Latest Version: %s", version.c_str());
      if(strcmp(version.c_str(), PROGVERSION) != 0)
      {
        ESP_LOGI(TAG, "New Version available: %s", version.c_str());
        return true;
      }
    }
  }
  return false;
}

void nb_send(void *pvParameters) {
  configASSERT(((uint32_t)pvParameters) == 1); // FreeRTOS check
  NbIotManager manager = NbIotManager();
  while (1) {
    manager.loop();
    delay(100);
  }
}

void NbIotManager::nb_init() {
  sdLoadNbConfig(&nbConfig);
  if (strlen(nbConfig.ServerAddress) < 5) {
    ESP_LOGE(TAG, "Error in NB config, cant send");
    this->initializeFailures++;
    return;
  }

  resetModem();

  if (!configModem()) {
    ESP_LOGE(TAG, "Could not config modem");
    this->initializeFailures++;
    return;
  }
  if (!attachNetwork()) {
    ESP_LOGE(TAG, "Could not attach network");
    this->initializeFailures++;
    return;
  }
  this->initializeFailures = 0;
  initialized = true;
}

void getSentiloTimestamp(char *buffer, uint32_t timestamp) {
  sprintf(buffer, "%02d/%02d/%4dT%02d:%02d:%02d", day(timestamp),
          month(timestamp), year(timestamp), hour(timestamp), minute(timestamp),
          second(timestamp));
}

int sendNbMqtt(MessageBuffer_t *message, ConfigBuffer_t *config, char *devEui) {
  char topic[64];

  char messageBuffer[512];

  sprintf(topic, "%s/application/%s/device/%s/rx", config->GatewayId,
          config->ApplicationId, devEui);

  StaticJsonDocument<512> doc;

  doc["applicationID"] = config->ApplicationId;
  doc["applicationName"] = config->ApplicationName;
  doc["fPort"] = message->MessagePort;

  unsigned int base64_length;
  unsigned char base64[64];

  int res = mbedtls_base64_encode(base64, sizeof(base64), &base64_length, message->Message, message->MessageSize);
  if (base64[base64_length - 1] == 10) {
    base64[base64_length - 1] = 0;
  }

  doc["data"] = base64;
  doc["deviceName"] = devEui;
  doc["devEUI"] = devEui;

  serializeJson(doc, messageBuffer);
  return publishMqtt(topic, messageBuffer, 0);
}

void NbIotManager::nb_registerNetwork() {
  this->registered = this->nb_checkNetworkRegister();
}

void NbIotManager::nb_connectNetwork() {
  this->connected = this->nb_checkNetworkConnected();
}

void NbIotManager::nb_connectMqtt() {
  sprintf(this->devEui, "%02x%02x%02x%02x%02x%02x%02x%02x", DEVEUI[0],
          DEVEUI[1], DEVEUI[2], DEVEUI[3], DEVEUI[4], DEVEUI[5], DEVEUI[6],
          DEVEUI[7]);
  int conn_result = connectMqtt(nbConfig.ServerAddress, nbConfig.port,
                                nbConfig.ServerPassword, this->devEui);
  if (conn_result == 0) {
    ESP_LOGD(TAG, "MQTT CONNECTED");
    mqttConnected = true;
    this->mqttConnectFailures = 0;
  } else {
    ESP_LOGD(TAG, "MQTT CONNECTION FAILED");
    this->mqttConnectFailures++;
  }
  return;
}

void NbIotManager::nb_subscribeMqtt() {
  char topic[64];
  sprintf(topic, "%s/application/%s/device/%s/tx", nbConfig.GatewayId,
          nbConfig.ApplicationId, this->devEui);
  if (subscribeMqtt(topic)) {
    ESP_LOGD(TAG, "MQTT SUBSCRIBED");
    subscribed = true;
    this->subscribeFailures = 0;
  } else {
    this->subscribeFailures++;
  }
  return;
}

void NbIotManager::nb_resetStatus() {
  ESP_LOGD(TAG, "Reset nb status");
  initialized = false;
  registered = false;
  connected = false;
  mqttConnected = false;
  subscribed = false;

  consecutiveFailures = 0;
  initializeFailures = 0;
  registerFailures = 0;
  connectFailures = 0;
  mqttConnectFailures = 0;
  subscribeFailures = 0;
}

bool NbIotManager::nb_checkNetworkRegister() {
  if (networkReady()) {
    this->registerFailures = 0;
    return true;
  } else {
    this->registerFailures++;
    return false;
  }
}

bool NbIotManager::nb_checkNetworkConnected() {
  if (networkAttached()) {
    this->connectFailures = 0;
    return true;
  } else {
    this->connectFailures++;
    return false;
  }
}

bool NbIotManager::nb_checkMqttConnected() {
  if (checkMqttConnection()) {
    this->mqttConnectFailures = 0;
    return true;
  } else {
    this->mqttConnectFailures++;
    return false;
  }
}

bool NbIotManager::nb_checkStatus() {
  if (millis() - this->nbLastStatusCheck < NB_STATUS_CHECK_TIME_MS)
    return true;

  this->nbLastStatusCheck = millis();
  if (!this->nb_checkNetworkRegister()) {
    this->registered = false;
    ESP_LOGD(TAG, "Network unregistered");
    return false;
  }
  if (!this->nb_checkNetworkConnected()) {
    this->connected = false;
    ESP_LOGD(TAG, "Network disconnected");
    return false;
  }
  if (!this->nb_checkMqttConnected()) {
    this->mqttConnected = false;
    this->subscribed = false;
    ESP_LOGD(TAG, "MQTT disconnected");
    return false;
  }
  return true;
}

void NbIotManager::loop() {

  if (this->consecutiveFailures > MAX_CONSECUTIVE_FAILURES ||
      this->initializeFailures > MAX_INITIALIZE_FAILURES ||
      this->registerFailures > MAX_REGISTER_FAILURES ||
      this->connectFailures > MAX_CONNECT_FAILURES ||
      this->mqttConnectFailures > MAX_MQTT_CONNECT_FAILURES ||
      this->subscribeFailures > MAX_MQTT_SUBSCRIBE_FAILURES) {

    ESP_LOGE(TAG, "Too many consecutive failures");
    this->nb_resetStatus();
    return;
  }

  if (!this->initialized) {
    this->nb_init();
    return;
  }

  if (!this->registered) {
    this->nb_registerNetwork();
    return;
  }

  if (!this->connected) {
    this->nb_connectNetwork();
    return;
  }

  if (!this->mqttConnected) {
    this->nb_connectMqtt();
    return;
  }

  if (!this->subscribed) {
    this->nb_subscribeMqtt();
    return;
  }

  if (!this->nb_checkStatus()) {
    ESP_LOGD(TAG, "NB status changed");
    return;
  }

  if (this->lastUpdateCheck + UPDATES_CHECK_INTERVAL < millis()) {
    if(this->nb_checkLastSoftwareVersion()) {
      if(downloadUpdates(std::string(updatesServerResponse)))
      {
        ESP_LOGD(TAG, "Updates downloaded");
        if(updateFromFS())
        {
          ESP_LOGD(TAG, "Updates installed");
        }
        sdcardInit();
      }
      else {
        ESP_LOGD(TAG, "Updates not downloaded, set to retry");
        this->lastUpdateCheck = millis() - UPDATES_CHECK_RETRY_INTERVAL;
      }
    }
  }

  this->nb_readMessages();
  this->nb_sendMessages();
  this->consecutiveFailures = 0;
}

void NbIotManager::nb_sendMessages() {
  MessageBuffer_t SendBuffer;
  if (uxQueueMessagesWaiting(NbSendQueue) > 0) {
    ESP_LOGD(TAG, "NB messages pending, sending");
    // fetch next or wait for payload to send from queue

    while (uxQueueMessagesWaiting(NbSendQueue) > 0) {
      xQueueReceive(NbSendQueue, &SendBuffer, portMAX_DELAY);
      int retries = 0;
      int result = sendNbMqtt(&SendBuffer, &this->nbConfig, this->devEui);
      if (result == 0) {
        mqttSendFailures = 0;
        continue;
      }

      ESP_LOGE(TAG, "Could not send MQTT message, retry.");
      this->mqttSendFailures++;
      SendBuffer.MessagePrio = prio_high;
      nb_enqueuedata(&SendBuffer);
      break;
    }
  }
}

void NbIotManager::nb_readMessages() {
  if (dataAvailable()) {
    char data[2048];
    int bytesRead = readMqttSubData(data, sizeof(data));

    if (bytesRead > 0) {
      ESP_LOGD(TAG, "MQTT message received");
      StaticJsonDocument<1024> doc;

      DeserializationError error = deserializeJson(doc, data, bytesRead);

      if (error) {
        ESP_LOGE(TAG, "DeserializeJson() failed: %s", error.c_str());
        return;
      }
      const char *data = doc["data"]; // "ZWcBCg=="
      const unsigned char* dataPtr = reinterpret_cast<const unsigned char *>(data);

      ESP_LOGD(TAG, "MQTT message data: %s", data);
      unsigned int base64_length;
      unsigned char base64Decoded[64];
      int res = mbedtls_base64_decode(base64Decoded, sizeof(base64Decoded), &base64_length, dataPtr, strlen(data));

      if (base64Decoded == NULL) {
        ESP_LOGE(TAG, "base64_decode() failed");
        return;
      }

      if (bytesRead > 0) {
        rcommand((uint8_t *)base64Decoded, base64_length);
      }
    }
    else {
      ESP_LOGE(TAG, "MQTT message read failed with code %d", bytesRead);
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

  initModem();

  // start lorawan stack
  ESP_LOGI(TAG, "Starting NBIOT TASK...");
  lastMessage = millis();
  xTaskCreatePinnedToCore(nb_send,    // task function
                          "nbtask",   // name of task
                          16384,      // stack size of task
                          (void *)1,  // parameter of the task
                          1,          // priority of the task
                          &nbIotTask, // task handle
                          1);         // CPU core

  // Start join procedure if not already joined,
  // lora_setupForNetwork(true) is called by eventhandler when joined
  // else continue current session
  return ESP_OK;
}

void nb_queuereset() { xQueueReset(NbSendQueue); }
