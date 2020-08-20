#ifndef _NBIOT
#define _NBIOT

#include "globals.h"
#include "rcommand.h"
#include "BC95.hpp"
#include <ArduinoJson.h>

#define MIN_SEND_TIME_THRESHOLD 1000
#define MIN_SEND_MESSAGES_THRESHOLD 5
#define MAX_NB_MESSAGES 30

#define NB_QUEUE_SIZE SEND_QUEUE_SIZE

extern TaskHandle_t nbIotTask;

bool nb_enqueuedata(MessageBuffer_t *message);
void nb_queuereset(void);
esp_err_t nb_iot_init();
void nb_loop();

#endif