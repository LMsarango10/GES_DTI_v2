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
#include "updates.h"
#include "mbedtls/base64.h"

#define UPDATES_ENABLED
#define UPDATES_SERVER_IP "82.223.84.231"
#define UPDATES_SERVER_PORT 8000
#define UPDATES_SERVER_INDEX "/index.txt"
#define UPDATES_CHECK_INTERVAL 720*1000 // 1 hours for checking for updates
#define UPDATES_CHECK_RETRY_INTERVAL 30*1000 // 10 seconds for retrying to check for updates

#define MAX_CONSECUTIVE_FAILURES 5 // MAX NB Failures before restarting
#define MAX_INITIALIZE_FAILURES 5 // MAX NB Init Failures before restarting
#define MAX_REGISTER_FAILURES 200  // MAX NB Register Failures before restarting
#define MAX_CONNECT_FAILURES 5   // MAX NB Connect Failures before restarting
#define MAX_MQTT_CONNECT_FAILURES 5  // MAX NB MQTT Connect Failures before restarting
#define MAX_MQTT_SUBSCRIBE_FAILURES 5 // MAX NB MQTT Subscribe Failures before restarting

#define MIN_SEND_TIME_THRESHOLD 1000 // time (milliseconds) between message batches
#define MIN_SEND_MESSAGES_THRESHOLD 1 // min size for message batches when nb is ON

#define NB_FAILOVER_MESSAGES_THRESHOLD 50 // amount of messages in LORA queue before failover to NB - IMPORTANT: This value must be greater than MIN_SEND_MESSAGES_THRESHOLD

#define MQTT_PUB_RETRIES 2 // MQTT publish retries
#define MQTT_RETRY_TIME 1000 // time (milliseconds) between MQTT retries
#define MQTT_CONN_RETRIES 5 // MQTT connection retries

#define NB_STATUS_CHECK_TIME_MS 60000 // NB status check time (milliseconds)



class NbIotManager {
    ConfigBuffer_t nbConfig;
    bool enabled;
    bool temporaryEnabled;
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

    long nbLastStatusCheck;

    char updatesServerResponse[1600];
    long lastUpdateCheck;
    bool updateReadyToInstall;

    public:
        void loop();
        void set_enabled(int control);
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
        bool nb_checkLastSoftwareVersion();
};

extern TaskHandle_t nbIotTask;

bool nb_enqueuedata(MessageBuffer_t *message);
void nb_queuereset(void);
void nb_enable(bool temporary);
void nb_disable(void);
bool nb_isEnabled(void);
esp_err_t nb_iot_init();

#define NB_QUEUE_SIZE SEND_QUEUE_SIZE

#endif