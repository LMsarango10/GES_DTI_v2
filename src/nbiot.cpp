
#include "nbiot.h"
// Local logging Tag
static const char TAG[] = "nbiot";
QueueHandle_t NbSendQueue;
QueueHandle_t NbControlQueue;
TaskHandle_t nbIotTask = NULL;
unsigned long lastMessage;
bool nbIotEnabled = false;


// Indica si NB puede usarse como transporte (no solo si la cola RAM existe)
bool nbTransportAvailable = true;


// =============================================================================
// VERSIÓN MEJORADA DE nb_enqueuedata() CON GESTIÓN INTELIGENTE DE COLAS
// =============================================================================

/**
 * @brief Encola un mensaje para envío por NB-IoT con fallback automático a SD
 * 
 * Estrategia:
 * 1. Intenta encolar en RAM (NbSendQueue) según prioridad
 * 2. Si es prioritario y cola llena: expulsa mensaje viejo a SD y encola nuevo
 * 3. Si falla todo: guarda en SD como último recurso
 * 
 * @param message Puntero al mensaje a encolar
 * @return true si el mensaje fue encolado (RAM o SD), false si se perdió
 */

bool nb_enqueuedata(MessageBuffer_t *message) {
    // Validación de entrada
    if (!message) {
        ESP_LOGE(TAG, "nb_enqueuedata: message is NULL");
        return false;
    }

    // Validación de tamaño del mensaje
    if (message->MessageSize == 0 ||
        message->MessageSize > PAYLOAD_BUFFER_SIZE) {
        ESP_LOGE(TAG, "nb_enqueuedata: invalid MessageSize=%u", message->MessageSize);
        return false;
    }

    // 🔴 Si NB no está disponible como transporte, mandamos directo a SD
    if (!nbTransportAvailable) {
    #ifdef HAS_SDCARD
        if (isSDCardAvailable()) {
            if (sdqueueEnqueue(message)) {
                ESP_LOGI(TAG,
                         "NB OFF -> mensaje guardado en SD (port=%u, size=%u)",
                         message->MessagePort, message->MessageSize);
                return true;
            } else {
                ESP_LOGE(TAG,
                         "NB OFF -> error en sdqueueEnqueue, mensaje perdido (port=%u, size=%u)",
                         message->MessagePort, message->MessageSize);
                return false;
            }
        } else {
            ESP_LOGE(TAG,
                     "NB OFF y SD no disponible -> mensaje perdido (port=%u, size=%u)",
                     message->MessagePort, message->MessageSize);
            return false;
        }
    #else
        ESP_LOGE(TAG,
                 "NB OFF y sin soporte SD -> mensaje perdido (port=%u, size=%u)",
                 message->MessagePort, message->MessageSize);
        return false;
    #endif
    }

    // =======================
    // LÓGICA ORIGINAL NB RAM
    // =======================
    BaseType_t     ret  = pdFALSE;
    MessageBuffer_t DummyBuffer;
    sendprio_t     prio = message->MessagePrio;

    // Variables para estadísticas
    static uint32_t total_enqueued = 0;
    static uint32_t ram_enqueued   = 0;
    static uint32_t sd_fallback    = 0;
    static uint32_t evictions      = 0;
    static uint32_t failures       = 0;

    // Información de estado actual de la cola
    UBaseType_t spaces_available = uxQueueSpacesAvailable(NbSendQueue);
    UBaseType_t messages_waiting = uxQueueMessagesWaiting(NbSendQueue);

    ESP_LOGD(TAG, "nb_enqueue: port=%u size=%u prio=%u queue=%u/%u",
             message->MessagePort,
             message->MessageSize,
             prio,
             messages_waiting,
             messages_waiting + spaces_available);

    // =================================================================
    // ESTRATEGIA 1: MENSAJE DE ALTA PRIORIDAD
    // =================================================================
    if (prio == prio_high) {
        // Si no hay espacio, forzar hueco expulsando mensaje antiguo
        if (spaces_available == 0) {
            ESP_LOGW(TAG,
                     "NB Queue full (%u msgs), evicting oldest for high-priority",
                     messages_waiting);

            // Extraer el mensaje más antiguo
            if (xQueueReceive(NbSendQueue, &DummyBuffer, (TickType_t)0) == pdTRUE) {
                evictions++;
                // Intentar salvarlo en SD
            #ifdef HAS_SDCARD
                if (isSDCardAvailable()) {
                    if (sdqueueEnqueue(&DummyBuffer)) {
                        ESP_LOGI(TAG,
                                 "✓ Evicted message saved to SD (port=%u, size=%u)",
                                 DummyBuffer.MessagePort, DummyBuffer.MessageSize);
                    } else {
                        ESP_LOGE(TAG,
                                 "✗ Evicted message LOST - SD enqueue failed");
                        failures++;
                    }
                } else {
                    ESP_LOGE(TAG,
                             "✗ Evicted message LOST - SD not available");
                    failures++;
                }
            #else
                ESP_LOGE(TAG,
                         "✗ Evicted message LOST - no SD support");
                failures++;
            #endif
            } else {
                ESP_LOGE(TAG,
                         "Failed to receive from supposedly full queue");
            }
        }

        // Intentar insertar al frente (máxima prioridad)
        ret = xQueueSendToFront(NbSendQueue, (void *)message, (TickType_t)0);
        if (ret == pdTRUE) {
            ram_enqueued++;
            total_enqueued++;
            ESP_LOGI(TAG,
                     "✓ HIGH priority message enqueued to NB RAM (port=%u)",
                     message->MessagePort);
            return true;
        }
    }
    // =================================================================
    // ESTRATEGIA 2: MENSAJE NORMAL/BAJA PRIORIDAD
    // =================================================================
    else {
        // Intentar insertar normalmente al final
        ret = xQueueSendToBack(NbSendQueue, (void *)message, (TickType_t)0);
        if (ret == pdTRUE) {
            ram_enqueued++;
            total_enqueued++;
            ESP_LOGD(TAG,
                     "✓ Normal priority message enqueued to NB RAM (port=%u)",
                     message->MessagePort);
            return true;
        }
    }

    // =================================================================
    // FALLBACK: GUARDAR EN SD SI FALLÓ LA COLA RAM
    // =================================================================
    if (ret != pdTRUE) {
        ESP_LOGW(TAG,
                 "NB RAM queue full, attempting SD fallback (port=%u, prio=%u)",
                 message->MessagePort, prio);
    #ifdef HAS_SDCARD
        if (isSDCardAvailable()) {
            if (sdqueueEnqueue(message)) {
                sd_fallback++;
                total_enqueued++;
                ESP_LOGI(TAG,
                         "✓ Message saved to SD (NB RAM full) - port=%u size=%u [SD fallbacks: %u]",
                         message->MessagePort,
                         message->MessageSize,
                         sd_fallback);
                // Imprimir estadísticas cada 10 mensajes a SD
                if (sd_fallback % 10 == 0) {
                    ESP_LOGI(TAG,
                             "📊 NB Stats: Total=%u RAM=%u SD=%u Evictions=%u Failures=%u",
                             total_enqueued, ram_enqueued, sd_fallback, evictions, failures);
                }
                return true; // ✓ Mensaje a salvo en SD
            } else {
                failures++;
                ESP_LOGE(TAG,
                         "✗ CRITICAL: SD enqueue FAILED - MESSAGE LOST! (port=%u, size=%u)",
                         message->MessagePort, message->MessageSize);
                return false; // ✗ Dato perdido
            }
        } else {
            failures++;
            ESP_LOGE(TAG,
                     "✗ CRITICAL: SD not available - MESSAGE LOST! (port=%u, size=%u)",
                     message->MessagePort, message->MessageSize);
            return false; // ✗ Dato perdido (sin SD)
        }
    #else
        failures++;
        ESP_LOGE(TAG,
                 "✗ CRITICAL: NB queue full, no SD support - MESSAGE LOST! (port=%u, size=%u)",
                 message->MessagePort, message->MessageSize);
        return false; // ✗ Dato perdido (sin soporte SD)
    #endif
    }

    // umm teoricamente No debería llegar aquí, pero por seguridad lo dejo 
    return false;
}


// =============================================================================
// FUNCIÓN AUXILIAR: Obtener estadísticas de la cola NB-IoT
// =============================================================================

/**
 * @brief Obtiene estadísticas actuales de la cola NB-IoT
 * @param stats Estructura donde se almacenarán las estadísticas
 */
void nb_get_queue_stats(struct nb_queue_stats_t *stats) {
    if (!stats) return;
    
    stats->messages_waiting = uxQueueMessagesWaiting(NbSendQueue);
    stats->spaces_available = uxQueueSpacesAvailable(NbSendQueue);
    stats->queue_size = stats->messages_waiting + stats->spaces_available;
    stats->usage_percent = (stats->messages_waiting * 100) / stats->queue_size;
    
#ifdef HAS_SDCARD
    stats->sd_queue_count = isSDCardAvailable() ? sdqueueCount() : 0;
#else
    stats->sd_queue_count = 0;
#endif
}

/**
 * @brief Imprime estadísticas de la cola NB-IoT
 */
void nb_print_queue_stats() {
    struct nb_queue_stats_t stats;
    nb_get_queue_stats(&stats);
    
    ESP_LOGI(TAG, "📊 NB Queue: %u/%u messages (%u%% full)", 
             stats.messages_waiting,
             stats.queue_size,
             stats.usage_percent);
    
#ifdef HAS_SDCARD
    if (stats.sd_queue_count > 0) {
        ESP_LOGI(TAG, "📊 SD Queue: %u messages pending", stats.sd_queue_count);
    }
#endif
}

/**
 * @brief Verifica si la cola NB-IoT tiene espacio disponible
 * @return true si hay espacio, false si está llena
 */
bool nb_queue_has_space() {
    return uxQueueSpacesAvailable(NbSendQueue) > 0;
}

/**
 * @brief Verifica si la cola NB-IoT está críticamente llena (>80%)
 * @return true si está críticamente llena
 */
bool nb_queue_is_critical() {
    UBaseType_t waiting = uxQueueMessagesWaiting(NbSendQueue);
    UBaseType_t total = waiting + uxQueueSpacesAvailable(NbSendQueue);
    return (waiting * 100 / total) > 80;
}

// =============================================================================
// ESTRUCTURA DE DATOS PARA ESTADÍSTICAS (Añadir al header .h)
// =============================================================================

/*

*/


uint32_t getUint32FromBuffer(uint8_t *buffer) {
    return ((buffer[0] << 24) & 0xFF000000) + ((buffer[1] << 16) & 0x00FF0000) +
           ((buffer[2] << 8) & 0x0000FF00) + (buffer[3] & 0x000000FF);
}

uint16_t getCount(uint8_t *buffer) {
    return ((buffer[0] << 8) & 0xFF00) + (buffer[1] & 0x00FF);
}

bool NbIotManager::nb_checkLastSoftwareVersion() {
    char buff[2048];
    int responseSize = 0;
    lastUpdateCheck = millis();
    if (getData(UPDATES_SERVER_IP, UPDATES_SERVER_PORT, UPDATES_SERVER_INDEX, buff,
                sizeof(buff), &responseSize) >= 0) {
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
        if (found != std::string::npos) {
            std::string version = bufferString.substr(0, found);
            ESP_LOGD(TAG, "Latest Version: %s", version.c_str());
            if (strcmp(version.c_str(), PROGVERSION) != 0) {
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
        if (uxQueueMessagesWaiting(NbControlQueue) > 0) {
            int controlMessage;
            xQueueReceive(NbControlQueue, &controlMessage, portMAX_DELAY);
            manager.set_enabled(controlMessage);
        }
        delay(100);
    }
}

void NbIotManager::set_enabled(int controlMessage) {
    if (controlMessage == 1) {
        this->enabled = true;
        this->temporaryEnabled = false;
    } else if (controlMessage == 2) {
        this->enabled = true;
        this->temporaryEnabled = true;
    } else {
        this->enabled = false;
    }
}

void NbIotManager::nb_init() {
    sdLoadNbConfig(&nbConfig);
    if (strlen(nbConfig.ServerAddress) < 5) {
        ESP_LOGE(TAG, "Error in NB config, cant send");
        this->initializeFailures++;
        return;
    }
    if (!preConfigModem()) {
        resetModem();
        if (!preConfigModem()) {
            ESP_LOGE(TAG, "Could not preconfig modem");
            this->initializeFailures++;
            return;
        }
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
    this->lastUpdateCheck = millis() - UPDATES_CHECK_INTERVAL;
    this->updateReadyToInstall = false;
    nbTransportAvailable = true;  // NB vuelve a estar disponible para transportar mensajes
}

void getSentiloTimestamp(char *buffer, uint32_t timestamp) {
    sprintf(buffer, "%02d/%02d/%4dT%02d:%02d:%02d", day(timestamp), month(timestamp),
            year(timestamp), hour(timestamp), minute(timestamp), second(timestamp));
}

int sendNbMqtt(MessageBuffer_t *message, ConfigBuffer_t *config, char *devEui) {
    char topic[64];
    char messageBuffer[512];
    sprintf(topic, "%s/application/%s/device/%s/rx", config->GatewayId, config->ApplicationId,
            devEui);
    StaticJsonDocument<512> doc;
    doc["applicationID"] = config->ApplicationId;
    doc["applicationName"] = config->ApplicationName;
    doc["fPort"] = message->MessagePort;
    unsigned int base64_length;
    unsigned char base64[128];
    int res = mbedtls_base64_encode(base64, sizeof(base64), &base64_length, message->Message,
                                   message->MessageSize);
    if (base64[base64_length - 1] == 10) {
        base64[base64_length - 1] = 0;
    }
    doc["data"] = base64;
    doc["deviceName"] = devEui;
    doc["devEUI"] = devEui;
    serializeJson(doc, messageBuffer);
    return publishMqtt(topic, messageBuffer, 0);
}

void NbIotManager::nb_registerNetwork() { this->registered = this->nb_checkNetworkRegister(); }

void NbIotManager::nb_connectNetwork() { this->connected = this->nb_checkNetworkConnected(); }

void NbIotManager::nb_connectMqtt() {
    sprintf(this->devEui, "%02x%02x%02x%02x%02x%02x%02x%02x", DEVEUI[0], DEVEUI[1], DEVEUI[2],
            DEVEUI[3], DEVEUI[4], DEVEUI[5], DEVEUI[6], DEVEUI[7]);
    int conn_result = connectMqtt(nbConfig.ServerAddress, nbConfig.port, nbConfig.ServerUsername,
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
    sprintf(topic, "%s/application/%s/device/%s/tx", nbConfig.GatewayId, nbConfig.ApplicationId,
            this->devEui);
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
if ( this->consecutiveFailures > MAX_CONSECUTIVE_FAILURES  ||
     this->initializeFailures  > MAX_INITIALIZE_FAILURES   ||
     this->registerFailures    > MAX_REGISTER_FAILURES     ||
     this->connectFailures     > MAX_CONNECT_FAILURES      ||
     this->mqttConnectFailures > MAX_MQTT_CONNECT_FAILURES ||
     this->subscribeFailures   > MAX_MQTT_SUBSCRIBE_FAILURES ) {

    ESP_LOGE(TAG, "Too many consecutive failures");
    this->nb_resetStatus();

    // Marcar NB como NO disponible para transporte
    nbTransportAvailable = false;

    // Opcional: volcar cola NB en RAM hacia SD para no perder nada
    MessageBuffer_t buf;
    while (uxQueueMessagesWaiting(NbSendQueue) > 0) {
        if (xQueueReceive(NbSendQueue, &buf, 0) == pdTRUE) {
        #ifdef HAS_SDCARD
            if (isSDCardAvailable()) {
                sdqueueEnqueue(&buf);
                ESP_LOGI(TAG, "NB2SD: mensaje movido de cola NB RAM a SD (port=%u, size=%u)",
                         buf.MessagePort, buf.MessageSize);
            }
        #endif
        }
    }

    return;
}

    if (!this->initialized) {
        this->nb_init();
        return;
    }

#ifdef UPDATES_ENABLED
    bool shouldCheckForUpdates =
        this->lastUpdateCheck + UPDATES_CHECK_INTERVAL < millis();
#endif

    if (shouldCheckForUpdates || this->enabled) {
        if (!this->registered) {
            this->nb_registerNetwork();
            return;
        }
        if (!this->connected) {
            this->nb_connectNetwork();
            return;
        }
        if (this->enabled) {
            if (!this->mqttConnected) {
                this->nb_connectMqtt();
                return;
            }
            if (!this->subscribed) {
                this->nb_subscribeMqtt();
                return;
            }
        }
        if (!this->nb_checkStatus()) {
            ESP_LOGD(TAG, "NB status changed");
            return;
        }
    }

#ifdef UPDATES_ENABLED
    if (shouldCheckForUpdates && this->nb_checkLastSoftwareVersion()) {
        if (downloadUpdates(std::string(updatesServerResponse))) {
            this->updateReadyToInstall = true;
        } else {
            ESP_LOGD(TAG, "Updates not downloaded, set to retry");
            this->updateReadyToInstall = false;
            this->lastUpdateCheck = millis() - UPDATES_CHECK_INTERVAL + UPDATES_CHECK_RETRY_INTERVAL;
        }
    }
    if (this->updateReadyToInstall) {
        ESP_LOGD(TAG, "Updates downloaded");
        if (updateFromFS()) {
            ESP_LOGD(TAG, "Updates installed");
        } else {
            ESP_LOGE(TAG, "Updates installation failed");
            removeUpdateFiles(std::string(updatesServerResponse));
        }
        sdcardInit();
    }
#endif

    if (this->enabled) {
        this->nb_readMessages();
        this->nb_sendMessages();
        this->consecutiveFailures = 0;
    }
}

void NbIotManager::nb_sendMessages() {
    MessageBuffer_t SendBuffer;
    if (uxQueueMessagesWaiting(NbSendQueue) > 0) {
        ESP_LOGD(TAG, "NB messages pending, sending");
        // fetch next or wait for payload to send from queue
        while (uxQueueMessagesWaiting(NbSendQueue) > 0) {
            xQueueReceive(NbSendQueue, &SendBuffer, portMAX_DELAY);
            int result = sendNbMqtt(&SendBuffer, &this->nbConfig, this->devEui);
            if (result == 0) {
                mqttSendFailures = 0;
                continue;
            }
            
ESP_LOGE(TAG, "Could not send MQTT message");
this->mqttSendFailures++;
SendBuffer.MessagePrio = prio_high;

#if (HAS_LORA)
  // Prefer LoRa if joined and there is room
  if (LMIC.devaddr && check_queue_available()) {
    ESP_LOGW(TAG, "NB failed -> moving message to LoRa queue (priority LoRa)");
    lora_enqueuedata(&SendBuffer);
    continue;
  }
#endif

// Retry NB enqueue; if cannot, persist to SD
if (!nb_enqueuedata(&SendBuffer)) {
#ifdef HAS_SDCARD
  if (isSDCardAvailable()) {
    sdqueueEnqueue(&SendBuffer);
    ESP_LOGW(TAG, "NB failed and NB queue full -> moved to SD persistent queue");
  }
#endif
}
// continue sending next messages (do not break hard)
continue;

        }
    }

    if (this->temporaryEnabled && uxQueueMessagesWaiting(NbSendQueue) == 0) {
        this->temporaryEnabled = false;
        ESP_LOGI(TAG, "NBIOT temporary mode disabled");
        nb_disable();
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
            const char *data64 = doc["data"];
            const unsigned char *dataPtr = reinterpret_cast<const unsigned char *>(data64);
            ESP_LOGD(TAG, "MQTT message data: %s", data64);

            unsigned int base64_length;
            unsigned char base64Decoded[64];
            int res = mbedtls_base64_decode(base64Decoded, sizeof(base64Decoded), &base64_length,
                                           dataPtr, strlen(data64));
            if (base64Decoded == NULL) {
                ESP_LOGE(TAG, "base64_decode() failed");
                return;
            }
            if (bytesRead > 0) {
                rcommand((uint8_t *)base64Decoded, base64_length);
            }
        } else {
            ESP_LOGE(TAG, "MQTT message read failed with code %d", bytesRead);
        }
    }
}

esp_err_t nb_iot_init() {
    assert(NB_QUEUE_SIZE);
    NbSendQueue = xQueueCreate(NB_QUEUE_SIZE, sizeof(MessageBuffer_t));
    NbControlQueue = xQueueCreate(2, sizeof(int));
    if (NbSendQueue == 0) {
        ESP_LOGE(TAG, "Could not create NBIOT send queue. Aborting.");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "NBIOT send queue created, size %d Bytes", SEND_QUEUE_SIZE * sizeof(MessageBuffer_t));
    initModem();

    ESP_LOGI(TAG, "Starting NBIOT TASK...");
    lastMessage = millis();
    xTaskCreatePinnedToCore(nb_send, "nbtask", 16384, (void *)1, 1, &nbIotTask, 1);
    return ESP_OK;
}

void nb_enable(bool temporary) {
    ESP_LOGD(TAG, "Enabling NBIOT");
    nbIotEnabled = true;

    MessageBuffer_t SendBuffer;

#ifdef HAS_LORA
    while (uxQueueMessagesWaitingFromISR(lora_get_queue_handle()) > 0) {
        if (xQueueReceive(lora_get_queue_handle(), &SendBuffer, portMAX_DELAY) == pdTRUE)
            nb_enqueuedata(&SendBuffer);
    }
#endif

    int nb_enable = 1;
    if (temporary) {
        nb_enable = 2;
        ESP_LOGI(TAG, "NBIOT temporary mode enabled");
    }
    xQueueSend(NbControlQueue, &nb_enable, 1);
}

void nb_disable() {
    ESP_LOGD(TAG, "Disabling NBIOT");
    nbIotEnabled = false;
    MessageBuffer_t SendBuffer;

#ifdef HAS_LORA
    while (uxQueueMessagesWaitingFromISR(NbSendQueue) > 0) {
        if (xQueueReceive(NbSendQueue, &SendBuffer, portMAX_DELAY) == pdTRUE)
            lora_enqueuedata(&SendBuffer);
    }
#endif

    int nb_disable = 0;
    xQueueSend(NbControlQueue, &nb_disable, 1);
}

bool nb_isEnabled() { return nbIotEnabled; }

void nb_queuereset() { xQueueReset(NbSendQueue); }
