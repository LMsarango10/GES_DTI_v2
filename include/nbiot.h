#ifndef _NBIOT
#define _NBIOT

#include "globals.h"
#include "rcommand.h"
#include "BC95.hpp"
#include "sdcard.h"
#include <ArduinoJson.h>
#include <Ticker.h>
#include <TimeLib.h>
#include "lorawan.h"

extern "C" {
#include "crypto/base64.h"
}

#define MIN_SEND_TIME_THRESHOLD 1000
#define MIN_SEND_MESSAGES_THRESHOLD 5
#define MQTT_PUB_RETRIES 2
#define MQTT_RETRY_TIME 1000
#define MQTT_CONN_RETRIES 5

#define NB_QUEUE_SIZE SEND_QUEUE_SIZE

extern TaskHandle_t nbIotTask;
extern Ticker nbticker;
extern ConfigBuffer_t nbConfig;

bool nb_enqueuedata(MessageBuffer_t *message);
void nb_queuereset(void);
esp_err_t nb_iot_init();
void nb_loop();

#endif