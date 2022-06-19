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

bool connectModem() {      
  if(!configModem()) {
    ESP_LOGE(TAG, "Could not config modem");    
    delay(1000);
    resetModem();
    return false;
  }
  unsigned long t0 = millis();
  ESP_LOGI(TAG, "Try connecting");
  while (millis() < t0 + 60000) {
    if (networkReady()) {
      if (attachNetwork()) {
        return true;
      }
      delay(1000);
    } else {
      ESP_LOGI(TAG, "Wait for network");
      delay(5000);
    }
  }
  resetModem();
  return false;
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
  sprintf(buffer, "%02d/%02d/%4dT%02d:%02d:%02d", day(timestamp), month(timestamp), year(timestamp), hour(timestamp), minute(timestamp), second(timestamp));
}

int sendNbMqtt(MessageBuffer_t *message, ConfigBuffer_t *config, char *devEui) {
  char topic[64];

  char messageBuffer[512];

  sprintf(topic, "%s/application/%s/device/%s/rx", config->GatewayId, config->ApplicationId, devEui);

  StaticJsonDocument<512> doc;

  doc["applicationID"] = config->ApplicationId;
  doc["applicationName"] = config->ApplicationName;
  doc["fPort"] = message->MessagePort;

  unsigned int base64_length;
  unsigned char * base64 = base64_encode((const unsigned char *)message->Message, message->MessageSize, &base64_length);
  if(base64[base64_length - 1] == 10) {
    base64[base64_length - 1] = 0;
  }
  doc["data"] = base64;
  doc["deviceName"] = devEui;
  doc["devEUI"] = devEui;

  serializeJson(doc, messageBuffer);
  free(base64);
  return publishMqtt(topic, messageBuffer, 0);
}

void nb_loop() {
  MessageBuffer_t SendBuffer;
  if (millis() - lastMessage > MIN_SEND_TIME_THRESHOLD && uxQueueMessagesWaiting(NbSendQueue) > 0) {
    ConfigBuffer_t conf;
    sdLoadNbConfig(&conf);
    if(strlen(conf.ServerAddress) < 5)
    {
      ESP_LOGE(TAG, "Error in NB config, cant send");
      return;
    }
    ESP_LOGI(TAG, "NB messages pending, sending");
    // fetch next or wait for payload to send from queue

    char devEui[17];
    sprintf(devEui, "%02x%02x%02x%02x%02x%02x%02x%02x", DEVEUI[0], DEVEUI[1], DEVEUI[2], DEVEUI[3], DEVEUI[4], DEVEUI[5], DEVEUI[6], DEVEUI[7]);

    if(!connectModem())
      return;    
    int connection_retries = 0;
    while(connection_retries < MQTT_CONN_RETRIES) {
      int conn_result = connectMqtt(conf.ServerAddress, conf.port, conf.ServerPassword, devEui);
      if(conn_result == 0){
        ESP_LOGI(TAG, "MQTT CONNECTED");
        break;
      }
      ESP_LOGI(TAG, "Could not connect to MQTT, retrying");
      delay(MQTT_RETRY_TIME);
      connection_retries++;
    }
    if (connection_retries == MQTT_CONN_RETRIES) {
      ESP_LOGI(TAG, "Could not connect to MQTT");
      return;
    }

    while (uxQueueMessagesWaiting(NbSendQueue) > 0) {
      xQueueReceive(NbSendQueue, &SendBuffer, portMAX_DELAY);
      int retries = 0;
      while(retries < MQTT_PUB_RETRIES) {
        int result = sendNbMqtt(&SendBuffer, &conf, devEui);
        if (result == 0) {
          break;
        }
        retries++;
        ESP_LOGE(TAG, "Could not send MQTT message, retry.");
        delay(MQTT_RETRY_TIME);
      }
      if(retries >= MQTT_PUB_RETRIES)
      {
        ESP_LOGE(TAG, "Max MQTT retries exceeded");
        SendBuffer.MessagePrio = prio_high;
        nb_enqueuedata(&SendBuffer);
        break;
      }
    }
    disconnectMqtt();
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
