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

#define MAX_CONSECUTIVE_FAILURES 10
#define MAX_INITIALIZE_FAILURES 10
#define MAX_REGISTER_FAILURES 100
#define MAX_CONNECT_FAILURES 10
#define MAX_MQTT_CONNECT_FAILURES 10
#define MAX_MQTT_SUBSCRIBE_FAILURES 10

class NbIotManager {
    ConfigBuffer_t nbConfig;
    char devEui[17];
    bool initialized;
    bool registered;
    bool connected;
    bool mqttConnected;
    bool subscribed;

    // failure counters
    int consecutiveFailures;
    int initializeFailures;
    int registerFailures;
    int connectFailures;
    int mqttConnectFailures;
    int subscribeFailures;
    int mqttSendFailures;

    public:
        void loop();
    private:
        void nb_init();
        void nb_loop();
        bool nb_connectModem();
        void nb_registerNetwork();
        void nb_connectNetwork();
        void nb_connectMqtt();
        void nb_subscribeMqtt();
        void nb_readMessages();
        void nb_sendMessages();
        void nb_resetStatus();
        bool nb_checkStatus();
        bool nb_checkNetworkRegister();
        bool nb_checkNetworkConnected();
        bool nb_checkMqttConnected();
};

extern TaskHandle_t nbIotTask;

bool nb_enqueuedata(MessageBuffer_t *message);
void nb_queuereset(void);
esp_err_t nb_iot_init();

#define MIN_SEND_TIME_THRESHOLD 1000
#define MIN_SEND_MESSAGES_THRESHOLD 5
#define MQTT_PUB_RETRIES 2
#define MQTT_RETRY_TIME 1000
#define MQTT_CONN_RETRIES 5

#define NB_QUEUE_SIZE SEND_QUEUE_SIZE

#endif