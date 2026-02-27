// Basic Config
#if (HAS_LORA)
#include "lorawan.h"
#endif

// Local logging Tag
static const char TAG[] = "lora";

unsigned long lastJoinAttemptTime = 0;
unsigned long lastConfirmedSendTime = 0;
unsigned long joinStartedTime = 0;
bool firstJoin = true;
uint8_t healthcheck_failures = 0;
bool healthcheck_pending = false;
bool nb_data_mode = false;

#if (HAS_LORA)

#if CLOCK_ERROR_PROCENTAGE > 7
#warning CLOCK_ERROR_PROCENTAGE value in lmic_config.h is too high; values > 7 will cause side effects
#endif

#if (TIME_SYNC_LORAWAN)
#ifndef LMIC_ENABLE_DeviceTimeReq
#define LMIC_ENABLE_DeviceTimeReq 1
#endif
#endif

// variable keep its values after restart or wakeup from sleep
RTC_NOINIT_ATTR u4_t RTCnetid, RTCdevaddr;
RTC_NOINIT_ATTR u1_t RTCnwkKey[16], RTCartKey[16];
RTC_NOINIT_ATTR int RTCseqnoUp, RTCseqnoDn;

QueueHandle_t LoraSendQueue;
TaskHandle_t lmicTask = NULL, lorasendTask = NULL;

// ===== SD persistent queue hooks & logging to paxcount.xx =====
#ifdef HAS_SDCARD
extern bool isSDCardAvailable(void);
extern bool sdqueueEnqueue(MessageBuffer_t *msg);
extern void sdqueueStartFlusher(void);
extern void sdcardWriteLine(const char *line);
#endif

// ===== NB hooks =====
#if (HAS_NBIOT)
extern void nb_enable(bool temporary);
extern bool nb_enqueuedata(MessageBuffer_t *message);
extern bool nb_isEnabled();
#endif

// ===== Small helper: hex to text for paxcount logging =====
static void _hex_of_payload(const uint8_t *b, size_t n, char *out, size_t outcap) {
    size_t p = 0;
    for (size_t i = 0; i < n && (p + 2) < outcap; i++) {
        p += snprintf(out + p, outcap - p, "%02X", b[i]);
    }
    out[(p < outcap) ? p : (outcap - 1)] = 0;
}

// ===== Log a TX event into paxcount.xx (plain) =====
static void _sd_log_tx(const char *tag, const MessageBuffer_t *m, const char *note) {
#ifdef HAS_SDCARD
    if (!isSDCardAvailable() || !m) return;
    // line: TAG,epoch,port,size,HEX[,note]
    char hex[2 * 256 + 1] = {0}; // ajusta si tu Message[] > 256
    _hex_of_payload(m->Message, m->MessageSize, hex, sizeof(hex));

    char line[512];
    snprintf(line, sizeof(line), "%s,%lu,%u,%u,%s%s%s",
             tag,
             (unsigned long)now(),
             (unsigned)m->MessagePort,
             (unsigned)m->MessageSize,
             hex,
             (note && note[0]) ? "," : "",
             (note && note[0]) ? note : "");
    sdcardWriteLine(line);
#endif
}

// table of LORAWAN MAC messages sent by the network to the device
static const mac_t MACdn_table[] = {
    {0x01, "ResetConf", 1}, {0x02, "LinkCheckAns", 2},
    {0x03, "LinkADRReq", 4}, {0x04, "DutyCycleReq", 1},
    {0x05, "RXParamSetupReq", 4}, {0x06, "DevStatusReq", 0},
    {0x07, "NewChannelReq", 5}, {0x08, "RxTimingSetupReq", 1},
    {0x09, "TxParamSetupReq", 1}, {0x0A, "DlChannelReq", 4},
    {0x0B, "RekeyConf", 1}, {0x0C, "ADRParamSetupReq", 1},
    {0x0D, "DeviceTimeAns", 5}, {0x0E, "ForceRejoinReq", 2},
    {0x0F, "RejoinParamSetupReq", 1}
};

// table of LORAWAN MAC messages sent by the device to the network
static const mac_t MACup_table[] = {
    {0x01, "ResetInd", 1}, {0x02, "LinkCheckReq", 0},
    {0x03, "LinkADRAns", 1}, {0x04, "DutyCycleAns", 0},
    {0x05, "RXParamSetupAns", 1}, {0x06, "DevStatusAns", 2},
    {0x07, "NewChannelAns", 1}, {0x08, "RxTimingSetupAns", 0},
    {0x09, "TxParamSetupAns", 0}, {0x0A, "DlChannelAns", 1},
    {0x0B, "RekeyInd", 1}, {0x0C, "ADRParamSetupAns", 0},
    {0x0D, "DeviceTimeReq", 0}, {0x0F, "RejoinParamSetupAns", 1}
};

class MyHalConfig_t : public Arduino_LMIC::HalConfiguration_t {
public:
    MyHalConfig_t() {}
    virtual void begin(void) override {
        SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
    }
};

static MyHalConfig_t myHalConfig{};

// LMIC pin mapping for Hope RFM95 / HPDtek HPD13A transceivers
static const lmic_pinmap myPinmap = {
    .nss = LORA_CS,
    .rxtx = LMIC_UNUSED_PIN,
    .rst = LORA_RST == NOT_A_PIN ? LMIC_UNUSED_PIN : LORA_RST,
    .dio = {LORA_IRQ, LORA_IO1, LORA_IO2 == NOT_A_PIN ? LMIC_UNUSED_PIN : LORA_IO2},
    .rxtx_rx_active = LMIC_UNUSED_PIN,
    .rssi_cal = 10,
    .spi_freq = 8000000, // 8MHz
    .pConfig = &myHalConfig
};

void lora_setupForNetwork(bool preJoin) {
    if (preJoin) {
#if CFG_LMIC_US_like
        LMIC_selectSubBand(1);
#elif CFG_LMIC_EU_like
        LMIC_setLinkCheckMode(true);
#endif
    } else {
        LMIC_setAdrMode(cfg.adrmode);
        if (!cfg.adrmode) LMIC_setDrTxpow(assertDR(cfg.loradr), cfg.txpower);

        ESP_LOGI(TAG, "DEVaddr: %08X", LMIC.devaddr);
        ESP_LOGI(TAG, "Radio parameters: %s / %s / %s",
                 getSfName(updr2rps(LMIC.datarate)),
                 getBwName(updr2rps(LMIC.datarate)),
                 getCrName(updr2rps(LMIC.datarate)));

        LMIC_getSessionKeys(&RTCnetid, &RTCdevaddr, RTCnwkKey, RTCartKey);
        LMIC_setLinkCheckMode(true);
    }
}

// DevEUI generator using devices's MAC address
void gen_lora_deveui(uint8_t *pdeveui) {
    uint8_t *p = pdeveui, dmac[6];
    int i = 0;
    esp_efuse_mac_get_default(dmac);

    *p++ = 0xFF;
    *p++ = 0xFE;
    for (i = 0; i < 6; i++) {
        *p++ = dmac[5 - i];
    }
}

void RevBytes(unsigned char *b, size_t c) {
    u1_t i;
    for (i = 0; i < c / 2; i++) {
        unsigned char t = b[i];
        b[i] = b[c - 1 - i];
        b[c - 1 - i] = t;
    }
}

// LMIC callback functions
void os_getDevKey(u1_t *buf) { memcpy(buf, APPKEY, 16); }
void os_getArtEui(u1_t *buf) {
    memcpy(buf, APPEUI, 8);
    RevBytes(buf, 8);
}
void os_getDevEui(u1_t *buf) {
    int i = 0, k = 0;
    memcpy(buf, DEVEUI, 8);
    for (i = 0; i < 8; i++) k += buf[i];
    if (k) {
        RevBytes(buf, 8);
    } else {
        gen_lora_deveui(buf);
    }
#ifdef MCP_24AA02E64_I2C_ADDRESS
    get_hard_deveui(buf);
    RevBytes(buf, 8);
#endif
}

void get_hard_deveui(uint8_t *pdeveui) {
#ifdef MCP_24AA02E64_I2C_ADDRESS
    uint8_t i2c_ret;
    Wire.begin(SDA, SCL, 100000);
    Wire.beginTransmission(MCP_24AA02E64_I2C_ADDRESS);
    Wire.write(MCP_24AA02E64_MAC_ADDRESS);
    i2c_ret = Wire.endTransmission();
    if (i2c_ret == 0) {
        char deveui[32] = "";
        uint8_t data;
        Wire.beginTransmission(MCP_24AA02E64_I2C_ADDRESS);
        Wire.write(MCP_24AA02E64_MAC_ADDRESS);
        Wire.endTransmission();
        Wire.requestFrom(MCP_24AA02E64_I2C_ADDRESS, 8);
        while (Wire.available()) {
            data = Wire.read();
            sprintf(deveui + strlen(deveui), "%02X ", data);
            *pdeveui++ = data;
        }
        ESP_LOGI(TAG, "Serial EEPROM found, read DEVEUI %s", deveui);
    } else {
        ESP_LOGI(TAG, "Could not read DEVEUI from serial EEPROM");
    }
    Wire.setClock(400000);
#endif
}

#if (VERBOSE)
void showLoraKeys(void) {
    uint8_t buf[32];
    os_getDevEui((u1_t *)buf);
    printKey("DevEUI", buf, 8, true);
    os_getArtEui((u1_t *)buf);
    printKey("AppEUI", buf, 8, true);
    os_getDevKey((u1_t *)buf);
    printKey("AppKey", buf, 16, false);
}
#endif // VERBOSE

// =============================================================
// LMIC send task (MEJORADO + FALLBACK SIN JOIN -> SD)
// =============================================================
void lora_send(void *pvParameters) {
    configASSERT(((uint32_t)pvParameters) == 1);

    static bool havePending = false;
    static MessageBuffer_t Pending;
    static uint32_t busyStartMs = 0;
    static uint16_t busyCount = 0;

    // edad del pending (para evitar busy intermitente infinito)
    static uint32_t pendingStartMs = 0;
    const uint32_t PENDING_MAX_AGE_MS = 90000;   // 90s
    const uint32_t LORA_BUSY_TIMEOUT_MS = 30000; // 30s


    while (1) {

        
#ifdef HAS_SDCARD
extern uint32_t sdqueueCount();
#endif
#ifdef HAS_SDCARD
// Solo pausar si la cola de SD está MUY llena (>50 mensajes)
// y LoRa tiene espacio limitado
if (isSDCardAvailable() && sdqueueCount() > 50 && 
    uxQueueMessagesWaiting(LoraSendQueue) > 8) {
  vTaskDelay(pdMS_TO_TICKS(100));
  continue;
}
#endif


        // =========================
        // ðŸ”´ SIN JOIN -> TODO A SD
        // =========================
        if (!LMIC.devaddr) {
            // Si hay un pending local, va a SD
            if (havePending) {
#ifdef HAS_SDCARD
                if (isSDCardAvailable()) {
                    sdqueueEnqueue(&Pending);
                    _sd_log_tx("TX_SD_ENQ", &Pending, "NO_JOIN_PENDING");
                }
#endif
                havePending = false;
                pendingStartMs = 0;
            }
            // Vaciar COLA LoRa completa -> SD
            MessageBuffer_t m;
            while (xQueueReceive(LoraSendQueue, &m, 0) == pdTRUE) {
#ifdef HAS_SDCARD
                if (isSDCardAvailable()) {
                    sdqueueEnqueue(&m);
                    _sd_log_tx("TX_SD_ENQ", &m, "NO_JOIN_QUEUE");
                }
#endif
            }
            // Esperar un poco y volver a intentar
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        // Tomar siguiente mensaje si no tenemos pending
        if (!havePending) {
            if (xQueueReceive(LoraSendQueue, &Pending, portMAX_DELAY) != pdTRUE) {
                ESP_LOGE(TAG, "Premature return from xQueueReceive() with no data!");
                continue;
            }
            havePending = true;
            busyStartMs = 0;
            busyCount = 0;
            pendingStartMs = millis();
        }

        // Failover por edad (aunque busy sea intermitente)
        if (havePending && pendingStartMs && (millis() - pendingStartMs) > PENDING_MAX_AGE_MS) {
            ESP_LOGW(TAG, "Pending age > %lus -> failover NB/SD", PENDING_MAX_AGE_MS / 1000);

#if (HAS_NBIOT)
            nb_enable(true);
            bool oknb = nb_enqueuedata(&Pending);
            if (oknb) {
                _sd_log_tx("TX_NB_ENQ", &Pending, "PENDING_AGE");
            } else {
#ifdef HAS_SDCARD
                if (isSDCardAvailable()) {
                    sdqueueEnqueue(&Pending);
                    _sd_log_tx("TX_SD_ENQ", &Pending, "PENDING_AGE");
                }
#endif
            }
#else
#ifdef HAS_SDCARD
            if (isSDCardAvailable()) {
                sdqueueEnqueue(&Pending);
                _sd_log_tx("TX_SD_ENQ", &Pending, "PENDING_AGE_NO_NB");
            }
#endif
#endif
            havePending = false;
            pendingStartMs = 0;
            busyStartMs = 0;
            busyCount = 0;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // Si LMIC estÃ¡ ocupado, esperamos (sin re-encolar)
        if (LMIC.opmode & OP_TXRXPEND) {
            if (busyStartMs == 0) busyStartMs = millis();
            busyCount++;
            ESP_LOGD(TAG, "LMIC busy, waiting... (%u) %lus",
                     busyCount, (millis() - busyStartMs) / 1000);

#if (HAS_NBIOT)
            // Failover si LMIC se queda ocupado demasiado tiempo (continuo)
            if ((millis() - busyStartMs) > LORA_BUSY_TIMEOUT_MS) {
                ESP_LOGW(TAG, "LoRa busy > %lus, failover a NB-IoT (temporal)",
                         LORA_BUSY_TIMEOUT_MS / 1000);

                nb_enable(true);
                bool oknb = nb_enqueuedata(&Pending);
                if (oknb) {
                    _sd_log_tx("TX_NB_ENQ", &Pending, "BUSY_TIMEOUT");
                } else {
#ifdef HAS_SDCARD
                    if (isSDCardAvailable()) {
                        sdqueueEnqueue(&Pending);
                        _sd_log_tx("TX_SD_ENQ", &Pending, "BUSY_TIMEOUT");
                    }
#endif
                }

                havePending = false;
                pendingStartMs = 0;
                busyStartMs = 0;
                busyCount = 0;
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }
#endif

            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        // Si ya no estÃ¡ busy, reset contadores busy (pero NO tocamos pendingStartMs)
        busyStartMs = 0;
        busyCount = 0;

// ADEMUX: health checks siempre confirmed
        bool sendConfirmed = false;
        if (Pending.MessagePort == TELEMETRYPORT) {
            sendConfirmed = true;
            healthcheck_pending = true;
        }
#ifdef CONFIRMED_SEND_THRESHOLD
        else if ((millis() - lastConfirmedSendTime) >
                 (CONFIRMED_SEND_THRESHOLD * 60UL * 1000UL)) {
            sendConfirmed = true;
        }
#endif

        bool confirmedNow = sendConfirmed || ((cfg.countermode & 0x02) != 0);

        // intentamos transmitir payload
        switch (LMIC_sendWithCallback(
            Pending.MessagePort,
            Pending.Message,
            Pending.MessageSize,
            confirmedNow,
            myTxCallback,
            NULL)) {

        case LMIC_ERROR_SUCCESS:
            ESP_LOGI(TAG, "%d byte(s) sent to LORA", Pending.MessageSize);
#ifdef HAS_SDCARD
            _sd_log_tx("TX_LORA_OK", &Pending, "");
#endif
            if (confirmedNow) {
                ESP_LOGD(TAG, "Sending confirmed lora message");
                lastConfirmedSendTime = millis();
            }
            havePending = false;
            pendingStartMs = 0;
            break;

        case LMIC_ERROR_TX_BUSY:
        case LMIC_ERROR_TX_FAILED:
            // No re-encolamos: reintentamos el mismo Pending
            vTaskDelay(pdMS_TO_TICKS(1000 + random(500)));
            break;

        case LMIC_ERROR_TX_TOO_LARGE:
        case LMIC_ERROR_TX_NOT_FEASIBLE:
            ESP_LOGW(TAG, "LoRa cannot send (too large/not feasible) -> trying NB, else SD");
#if (HAS_NBIOT)
            nb_enable(true);
            if (nb_enqueuedata(&Pending)) {
#ifdef HAS_SDCARD
                _sd_log_tx("TX_NB_ENQ", &Pending, "TOO_LARGE");
#endif
            } else {
#ifdef HAS_SDCARD
                if (isSDCardAvailable()) {
                    sdqueueEnqueue(&Pending);
                    _sd_log_tx("TX_SD_ENQ", &Pending, "TOO_LARGE");
                }
#endif
            }
#else
#ifdef HAS_SDCARD
            if (isSDCardAvailable()) {
                sdqueueEnqueue(&Pending);
                _sd_log_tx("TX_SD_ENQ", &Pending, "TOO_LARGE_NO_NB");
            }
#endif
#endif
            havePending = false;
            pendingStartMs = 0;
            break;

        default:
            ESP_LOGE(TAG, "LMIC error -> trying NB, else SD");
#if (HAS_NBIOT)
            nb_enable(true);
            if (nb_enqueuedata(&Pending)) {
#ifdef HAS_SDCARD
                _sd_log_tx("TX_NB_ENQ", &Pending, "LMIC_ERROR");
#endif
            } else {
#ifdef HAS_SDCARD
                if (isSDCardAvailable()) {
                    sdqueueEnqueue(&Pending);
                    _sd_log_tx("TX_SD_ENQ", &Pending, "LMIC_ERROR");
                }
#endif
            }
#else
#ifdef HAS_SDCARD
            if (isSDCardAvailable()) {
                sdqueueEnqueue(&Pending);
                _sd_log_tx("TX_SD_ENQ", &Pending, "LMIC_ERROR_NO_NB");
            }
#endif
#endif
            havePending = false;
            pendingStartMs = 0;
            break;
        }

        delay(2);
    }
}

esp_err_t lora_stack_init(bool do_join) {
    assert(SEND_QUEUE_SIZE);
    LoraSendQueue = xQueueCreate(SEND_QUEUE_SIZE, sizeof(MessageBuffer_t));
    if (LoraSendQueue == 0) {
        ESP_LOGE(TAG, "Could not create LORA send queue. Aborting.");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "LORA send queue created, size %d Bytes",
             SEND_QUEUE_SIZE * sizeof(MessageBuffer_t));

    ESP_LOGI(TAG, "Starting LMIC...");
    xTaskCreatePinnedToCore(lmictask, "lmictask", 4096, (void *)1, 2, &lmicTask, 1);

    if (do_join) {
        lastJoinAttemptTime = millis();
        if (!LMIC_startJoining())
            ESP_LOGI(TAG, "Already joined");
    } else {
        LMIC_reset();
        LMIC_setSession(RTCnetid, RTCdevaddr, RTCnwkKey, RTCartKey);
        LMIC.seqnoUp = RTCseqnoUp;
        LMIC.seqnoDn = RTCseqnoDn;
    }

    xTaskCreatePinnedToCore(lora_send, "lorasendtask", 4096, (void *)1, 1, &lorasendTask, 1);
    return ESP_OK;
}

bool check_queue_available() {
    return uxQueueSpacesAvailable(LoraSendQueue) > 0;
}

long get_lora_queue_pending_messages() {
    return uxQueueMessagesWaitingFromISR(LoraSendQueue);
}

// =============================================================
// lora_enqueuedata() (MEJORADO: purga -> SD + log paxcount)
// =============================================================
bool lora_enqueuedata(MessageBuffer_t *message) {
    bool enqueued = false;
    BaseType_t ret = pdFALSE;
    MessageBuffer_t DummyBuffer;
    sendprio_t prio = message->MessagePrio;

    switch (prio) {
    case prio_high:
        if (uxQueueSpacesAvailable(LoraSendQueue) == 0) {
            if (xQueueReceive(LoraSendQueue, &DummyBuffer, (TickType_t)0) == pdTRUE) {
#ifdef HAS_SDCARD
                if (isSDCardAvailable()) {
                    sdqueueEnqueue(&DummyBuffer);
                    _sd_log_tx("TX_SD_ENQ_PURGE", &DummyBuffer, "LORA_QUEUE_FULL");
                    ESP_LOGW(TAG, "LORA sendqueue purged -> moved to SD persistent queue (paxqueue.q)");
                }
#endif
            }
        }
        [[fallthrough]];

    case prio_normal:
        ret = xQueueSendToFront(LoraSendQueue, (void *)message, (TickType_t)0);
        break;

    case prio_low:
    default:
        ret = xQueueSendToBack(LoraSendQueue, (void *)message, (TickType_t)0);
        break;
    }

    if (ret != pdTRUE) {
        snprintf(lmic_event_msg + 14, LMIC_EVENTMSG_LEN - 14, "<>");
        ESP_LOGW(TAG, "LORA sendqueue is full");
    } else {
        enqueued = true;
        snprintf(lmic_event_msg + 14, LMIC_EVENTMSG_LEN - 14, "%2u",
                 uxQueueMessagesWaitingFromISR(LoraSendQueue));
    }
    return enqueued;
}

void lora_queuereset(void) { xQueueReset(LoraSendQueue); }

#if (TIME_SYNC_LORAWAN)
void IRAM_ATTR user_request_network_time_callback(void *pVoidUserUTCTime, int flagSuccess) {
    time_t *pUserUTCTime = (time_t *)pVoidUserUTCTime;
    lmic_time_reference_t lmicTimeReference;

    if (flagSuccess != 1) {
        ESP_LOGW(TAG, "LoRaWAN network did not answer time request");
        return;
    }

    flagSuccess = LMIC_getNetworkTimeReference(&lmicTimeReference);
    if (flagSuccess != 1) {
        ESP_LOGW(TAG, "LoRaWAN time request failed");
        return;
    }

    mask_user_IRQ();
    *pUserUTCTime = lmicTimeReference.tNetwork + 315964800;
    ostime_t ticksNow = os_getTime();
    ostime_t ticksRequestSent = lmicTimeReference.tLocal;
    time_t requestDelaySec = osticks2ms(ticksNow - ticksRequestSent) / 1000;
    setMyTime(*pUserUTCTime + requestDelaySec, 0, _lora);

finish:
    unmask_user_IRQ();
}
#endif // TIME_SYNC_LORAWAN

#if (HAS_NBIOT)
void checkJoinProcedure() {
    if (firstJoin && joinStartedTime > 0) {
        if (millis() - joinStartedTime > NB_LORA_JOIN_GRACE_TIME * 1000 * 60) {
            ESP_LOGI(TAG, "Join procedure time exceeded, enabling NBIOT");
            nb_enable(false);
            firstJoin = false;
        }
    }
    if (!nb_isEnabled()) {
        lastJoinAttemptTime = millis();
    }
    if (millis() - lastJoinAttemptTime >
        NB_LORA_JOIN_RETRY_TIME_MINUTES * 1000 * 60) {
        ESP_LOGI(TAG, "Retry join procedure");
        lastJoinAttemptTime = millis();
        LMIC_startJoining();
    }
}
#endif

// LMIC lorawan stack task
void lmictask(void *pvParameters) {
    configASSERT(((uint32_t)pvParameters) == 1);

    os_init_ex(&myPinmap);
    LMIC_registerRxMessageCb(myRxCallback, NULL);
    LMIC_registerEventCb(myEventCallback, NULL);
    LMIC_reset();

#ifdef CLOCK_ERROR_PROCENTAGE
    LMIC_setClockError(CLOCK_ERROR_PROCENTAGE * MAX_CLOCK_ERROR / 1000);
#endif

    while (1) {
        os_runloop_once();
        delay(2);
#if (HAS_NBIOT)
        checkJoinProcedure();
#endif
    }
}

// lmic event handler
void myEventCallback(void *pUserData, ev_t ev) {
    static const char *const evNames[] = {LMIC_EVENT_NAME_TABLE__INIT};
    uint8_t const msgWaiting = uxQueueMessagesWaiting(LoraSendQueue);

    if (ev < sizeof(evNames) / sizeof(evNames[0]))
        snprintf(lmic_event_msg, LMIC_EVENTMSG_LEN, "%-16s", evNames[ev] + 3);
    else
        snprintf(lmic_event_msg, LMIC_EVENTMSG_LEN, "LMIC event %-4u ", ev);

    switch (ev) {
    case EV_JOINING:
        lora_setupForNetwork(true);
        break;

    case EV_JOINED:
        lora_setupForNetwork(false);
#if (HAS_NBIOT)
        firstJoin = false;
        // NB-IoT siempre activo — no llamar nb_disable()
        if (nb_data_mode) {
            ESP_LOGI(TAG, "LoRa joined, deactivating NB-IoT failover");
            nb_data_mode = false;
            healthcheck_failures = 0;
        }
#endif
        break;

    case EV_JOIN_FAILED:
        snprintf(lmic_event_msg, LMIC_EVENTMSG_LEN, "%-16s", "JOIN_FAILED");
#if (HAS_NBIOT)
        nb_enable(false);
#endif
        break;

    case EV_TXCOMPLETE:
        RTCseqnoUp = LMIC.seqnoUp;
        RTCseqnoDn = LMIC.seqnoDn;
        if (LMIC.txrxFlags & TXRX_ACK) {
            ESP_LOGI(TAG, "Received ack");
            if (nb_data_mode) {
                ESP_LOGI(TAG, "LoRa recovered! Resuming primary channel (was failover with %u failures)", healthcheck_failures);
                nb_data_mode = false;
            }
            healthcheck_failures = 0;
            healthcheck_pending = false;
        }
        else if (healthcheck_pending) {
            healthcheck_failures++;
            healthcheck_pending = false;
            ESP_LOGW(TAG, "Health check no ACK, consecutive failures: %u", healthcheck_failures);
#if (HAS_NBIOT)
            if (healthcheck_failures >= MAX_HEALTHCHECK_FAILURES && !nb_data_mode) {
                ESP_LOGE(TAG, "LoRa failed %u health checks, activating NB-IoT failover", healthcheck_failures);
                nb_data_mode = true;
            }
#endif
        }
        break;

    case EV_JOIN_TXCOMPLETE:
        snprintf(lmic_event_msg, LMIC_EVENTMSG_LEN, "%-16s", "JOIN_WAIT");
        if (joinStartedTime == 0) joinStartedTime = millis();
        break;

    case EV_LINK_DEAD:
        snprintf(lmic_event_msg, LMIC_EVENTMSG_LEN, "%-16s", "LINK_DEAD");
#if (HAS_NBIOT)
        if (!nb_data_mode) {
            ESP_LOGE(TAG, "Link dead, activating NB-IoT failover");
            nb_data_mode = true;
        }
#endif
        lastJoinAttemptTime = millis();
        LMIC_startJoining();
        break;
    }

    if (msgWaiting)
        snprintf(lmic_event_msg + 14, LMIC_EVENTMSG_LEN - 14, "%2u", msgWaiting);

    ESP_LOGD(TAG, "%s", lmic_event_msg);
}

// receive message handler
void myRxCallback(void *pUserData, uint8_t port, const uint8_t *pMsg, size_t nMsg) {
    if (nMsg)
        ESP_LOGI(TAG, "Received %u byte(s) of payload on port %u", nMsg, port);
    else if (port)
        ESP_LOGI(TAG, "Received empty message on port %u", port);

    uint8_t nMac = pMsg - &LMIC.frame[0];
    if (port != MACPORT) --nMac;

    if (nMac) {
        ESP_LOGI(TAG, "%u byte(s) downlink MAC commands", nMac);
    }

    if (LMIC.pendMacLen) {
        ESP_LOGI(TAG, "%u byte(s) uplink MAC commands", LMIC.pendMacLen);
        mac_decode(LMIC.pendMacData, LMIC.pendMacLen, MACup_table,
                   sizeof(MACup_table) / sizeof(MACup_table[0]));
    }

    switch (port) {
    case MACPORT:
        break;
    case RCMDPORT:
        rcommand(pMsg, nMsg);
        break;
    default:
#if (TIME_SYNC_LORASERVER)
        if (port == TIMEPORT) {
            recv_timesync_ans(pMsg, nMsg);
            break;
        }
#endif
        ESP_LOGI(TAG, "Received data on unsupported port %u", port);
        break;
    }
}

// transmit complete message handler
void myTxCallback(void *pUserData, int fSuccess) {
#if (TIME_SYNC_LORASERVER)
    if (LMIC.pendTxPort == TIMEPORT)
        store_time_sync_req(osticks2ms(LMIC.txend));
#endif
}

// decode LORAWAN MAC message
void mac_decode(const uint8_t cmd[], const uint8_t cmdlen,
                const mac_t table[], const uint8_t tablesize) {
    if (!cmdlen) return;

    uint8_t foundcmd[cmdlen], cursor = 0;

    while (cursor < cmdlen) {
        int i = tablesize;
        while (i--) {
            if (cmd[cursor] == table[i].opcode) {
                cursor++;
                if ((cursor + table[i].params) <= cmdlen) {
                    memmove(foundcmd, cmd + cursor, table[i].params);
                    cursor += table[i].params;
                    ESP_LOGD(TAG, "MAC command %s", table[i].cmdname);
                } else {
                    ESP_LOGD(TAG, "MAC command 0x%02X with missing parameter(s)", table[i].opcode);
                }
                break;
            }
        }
        if (i < 0) {
            ESP_LOGD(TAG, "Unknown MAC command 0x%02X", cmd[cursor]);
            cursor++;
        }
    }
}

QueueHandle_t lora_get_queue_handle() { return LoraSendQueue; }

uint8_t getBattLevel() {
#if (defined HAS_PMU || defined BAT_MEASURE_ADC)
    uint16_t voltage = read_voltage();
    switch (voltage) {
    case 0:
        return MCMD_DEVS_BATT_NOINFO;
    case 0xffff:
        return MCMD_DEVS_EXT_POWER;
    default:
        return (voltage > OTA_MIN_BATT ? MCMD_DEVS_BATT_MAX : MCMD_DEVS_BATT_MIN);
    }
#else
    return MCMD_DEVS_BATT_NOINFO;
#endif
}

const char *getSfName(rps_t rps) {
    const char *const t[] = {"FSK", "SF7", "SF8", "SF9", "SF10", "SF11", "SF12", "SF?"};
    return t[getSf(rps)];
}
const char *getBwName(rps_t rps) {
    const char *const t[] = {"BW125", "BW250", "BW500", "BW?"};
    return t[getBw(rps)];
}
const char *getCrName(rps_t rps) {
    const char *const t[] = {"CR 4/5", "CR 4/6", "CR 4/7", "CR 4/8"};
    return t[getCr(rps)];
}

#endif // HAS_LORA