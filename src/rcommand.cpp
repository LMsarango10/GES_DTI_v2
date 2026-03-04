// Basic Config
#include "globals.h"
#include "rcommand.h"
#include "blescan.h"    // Para bt_module_ok, ble_module_ok
#include "nbiot.h"      // Para nb_status_registered, etc.
#include <esp_system.h> // Para esp_reset_reason()

// ✅ Para crear task sin bloquear el callback LoRa / parser
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Local logging tag
static const char TAG[] = __FILE__;

// =========================================================
//  Helper: construye MessageBuffer y envía directo por NB-IoT
//  Usado por todos los comandos remotos que responden datos
// =========================================================
static void send_response_direct(uint8_t port, sendprio_t prio) {
  MessageBuffer_t buf;
  buf.MessageSize = payload.getSize();
  buf.MessagePort = port;
  buf.MessagePrio = prio;
  memcpy(buf.Message, payload.getBuffer(), buf.MessageSize);
  int result = nb_send_direct(&buf);
  if (result != 0) {
    // NB-IoT no disponible todavía (manager no listo) → fallback a SendPayload
    ESP_LOGW(TAG, "nb_send_direct failed (port=%u), fallback to SendPayload", port);
    SendPayload(port, prio);
  }
}

// =========================================================
//  IMEI over LoRa remote command (BC95) - OPCODE 0x8A
// =========================================================
#if (HAS_NBIOT)
extern String bc95_getImei();
static TaskHandle_t imeiTaskHandle = NULL;

extern String bc95_getMsisdn();
static TaskHandle_t msisdnTaskHandle = NULL;
static void msisdnTask(void *param);
static void get_msisdn(uint8_t val[]);

static void imeiTask(void *param);
static void get_imei(uint8_t val[]);
#endif

// set of functions that can be triggered by remote commands
void set_reset(uint8_t val[]) {
  switch (val[0]) {
  case 0:
    ESP_LOGI(TAG, "Remote command: restart device cold");
    do_reset(false);
    break;
  case 1:
    ESP_LOGI(TAG, "Remote command: reset MAC counter");
    reset_counters();
    get_salt();
    break;
  case 2:
    ESP_LOGI(TAG, "Remote command: reset device to factory settings");
    eraseConfig();
    break;
  case 3:
    ESP_LOGI(TAG, "Remote command: flush send queue");
    flushQueues();
    break;
  case 4:
    ESP_LOGI(TAG, "Remote command: restart device warm");
    do_reset(true);
    break;
  case 9:
    ESP_LOGI(TAG, "Remote command: software update via Wifi");
#if (USE_OTA)
    RTC_runmode = RUNMODE_UPDATE;
#endif
    break;
  default:
    ESP_LOGW(TAG, "Remote command: reset called with invalid parameter(s)");
  }
}

void set_rssi(uint8_t val[]) {
  cfg.rssilimit = val[0] * -1;
  ESP_LOGI(TAG, "Remote command: set RSSI limit to %d", cfg.rssilimit);
}

void set_salt(uint8_t val[]) {
  cfg.salt = (val[3] * 256 * 256 * 256) + (val[2] * 256 * 256) +
             (val[1] * 256) + val[0];
  cfg.saltVersion = (val[7] * 256 * 256 * 256) + (val[6] * 256 * 256) +
                    (val[5] * 256) + val[4];
  cfg.saltTimestamp = (val[11] * 256 * 256 * 256) + (val[10] * 256 * 256) +
                      (val[9] * 256) + val[8];
  ESP_LOGI(TAG, "Remote command: set SALT to %X", cfg.salt);
  ESP_LOGI(TAG, "Remote command: set SALT VERSION to %X", cfg.saltVersion);
  ESP_LOGI(TAG, "Remote command: set SALT TIMESTAMP to %X", cfg.saltTimestamp);
}

void get_userSalt(uint8_t val[]) {
  ESP_LOGI(TAG, "Remote command: get SALT");
  payload.reset();
  payload.addSalt(cfg.salt);
  payload.addSaltVersion(cfg.saltVersion);
  payload.addSaltTimestamp(cfg.saltTimestamp);
  send_response_direct(CONFIGPORT, prio_high);
}

void set_sendcycle(uint8_t val[]) {
  cfg.sendcycle = val[0] * 256 + val[1];
  sendcycler.detach();
  sendcycler.attach(cfg.sendcycle * 2, sendcycle);
  ESP_LOGI(TAG, "Remote command: set send cycle to %d seconds", cfg.sendcycle * 2);
}

void set_wifichancycle(uint8_t val[]) {
  cfg.wifichancycle = val[0];
  xTimerChangePeriod(WifiChanTimer, pdMS_TO_TICKS(cfg.wifichancycle * 10), 100);
  ESP_LOGI(TAG, "Remote command: set Wifi channel switch interval to %.1f seconds",
           cfg.wifichancycle / float(100));
}

void set_blescantime(uint8_t val[]) {
  cfg.blescantime = val[0];
  ESP_LOGI(TAG, "Remote command: set BLE scan time to %.1f seconds",
           cfg.blescantime / float(100));
  if (cfg.blescan) {
  }
}

void set_countmode(uint8_t val[]) {
  switch (val[0]) {
  case 0:
    cfg.countermode = 0;
    ESP_LOGI(TAG, "Remote command: set counter mode to cyclic unconfirmed");
    break;
  case 1:
    cfg.countermode = 1;
    ESP_LOGI(TAG, "Remote command: set counter mode to cumulative");
    break;
  case 2:
    cfg.countermode = 2;
    ESP_LOGI(TAG, "Remote command: set counter mode to cyclic confirmed");
    break;
  default:
    ESP_LOGW(TAG, "Remote command: set counter mode called with invalid parameter(s)");
    return;
  }
  reset_counters();
  get_salt();
}

void set_screensaver(uint8_t val[]) {
  ESP_LOGI(TAG, "Remote command: set screen saver to %s ", val[0] ? "on" : "off");
  cfg.screensaver = val[0] ? 1 : 0;
}

void set_display(uint8_t val[]) {
  ESP_LOGI(TAG, "Remote command: set screen to %s", val[0] ? "on" : "off");
  cfg.screenon = val[0] ? 1 : 0;
}

void set_gps(uint8_t val[]) {
  ESP_LOGI(TAG, "Remote command: set GPS mode to %s", val[0] ? "on" : "off");
  if (val[0]) {
    cfg.payloadmask = (uint8_t)GPS_DATA;
  } else {
    cfg.payloadmask &= (uint8_t)~GPS_DATA;
  }
}

void set_bme(uint8_t val[]) {
  ESP_LOGI(TAG, "Remote command: set BME mode to %s", val[0] ? "on" : "off");
  if (val[0]) {
    cfg.payloadmask = (uint8_t)MEMS_DATA;
  } else {
    cfg.payloadmask &= (uint8_t)~MEMS_DATA;
  }
}

void set_batt(uint8_t val[]) {
  ESP_LOGI(TAG, "Remote command: set battery mode to %s", val[0] ? "on" : "off");
  if (val[0]) {
    cfg.payloadmask = (uint8_t)BATT_DATA;
  } else {
    cfg.payloadmask &= (uint8_t)~BATT_DATA;
  }
}

void set_payloadmask(uint8_t val[]) {
  ESP_LOGI(TAG, "Remote command: set payload mask to %X", val[0]);
  cfg.payloadmask = val[0];
}

void set_sensor(uint8_t val[]) {
#if (HAS_SENSORS)
  switch (val[0]) {
  case 1:
  case 2:
  case 3:
    break;
  default:
    ESP_LOGW(TAG, "Remote command set sensor mode called with invalid sensor number");
    return;
  }
  ESP_LOGI(TAG, "Remote command: set sensor #%d mode to %s", val[0], val[1] ? "on" : "off");
  if (val[1])
    cfg.payloadmask = sensor_mask(val[0]);
  else
    cfg.payloadmask &= ~sensor_mask(val[0]);
#endif
}

void set_beacon(uint8_t val[]) {
  uint8_t id = val[0];
  memmove(val, val + 1, 6);
  beacons[id] = macConvert(val);
  ESP_LOGI(TAG, "Remote command: set beacon ID#%d", id);
  printKey("MAC", val, 6, false);
}

void set_monitor(uint8_t val[]) {
  ESP_LOGI(TAG, "Remote command: set beacon monitor mode to %s", val ? "on" : "off");
  cfg.monitormode = val[0] ? 1 : 0;
}

void set_loradr(uint8_t val[]) {
#if (HAS_LORA)
  if (validDR(val[0])) {
    cfg.loradr = val[0];
    ESP_LOGI(TAG, "Remote command: set LoRa Datarate to %d", cfg.loradr);
    LMIC_setDrTxpow(assertDR(cfg.loradr), KEEP_TXPOW);
    ESP_LOGI(TAG, "Radio parameters now %s / %s / %s",
             getSfName(updr2rps(LMIC.datarate)),
             getBwName(updr2rps(LMIC.datarate)),
             getCrName(updr2rps(LMIC.datarate)));
  } else
    ESP_LOGI(TAG, "Remote command: set LoRa Datarate called with illegal datarate %d", val[0]);
#else
  ESP_LOGW(TAG, "Remote command: LoRa not implemented");
#endif
}

void set_loraadr(uint8_t val[]) {
#if (HAS_LORA)
  ESP_LOGI(TAG, "Remote command: set LoRa ADR mode to %s", val[0] ? "on" : "off");
  cfg.adrmode = val[0] ? 1 : 0;
  LMIC_setAdrMode(cfg.adrmode);
#else
  ESP_LOGW(TAG, "Remote command: LoRa not implemented");
#endif
}

void set_blescan(uint8_t val[]) {
  ESP_LOGI(TAG, "Remote command: set BLE scanner to %s", val[0] ? "on" : "off");
  cfg.blescan = val[0] ? 1 : 0;
  if (cfg.blescan) {
  } else {
    macs_ble = 0;
  }
}

void set_btscan(uint8_t val[]) {
  ESP_LOGI(TAG, "Remote command: set BT scanner to %s", val[0] ? "on" : "off");
  cfg.btscan = val[0] ? 1 : 0;
  if (cfg.btscan) {
  } else {
    macs_bt = 0;
  }
}

void set_wifiscan(uint8_t val[]) {
  ESP_LOGI(TAG, "Remote command: set WIFI scanner to %s", val[0] ? "on" : "off");
  cfg.wifiscan = val[0] ? 1 : 0;
  switch_wifi_sniffer(cfg.wifiscan);
}

void set_wifiant(uint8_t val[]) {
  ESP_LOGI(TAG, "Remote command: set Wifi antenna to %s", val[0] ? "external" : "internal");
  cfg.wifiant = val[0] ? 1 : 0;
#ifdef HAS_ANTENNA_SWITCH
  antenna_select(cfg.wifiant);
#endif
}

void set_vendorfilter(uint8_t val[]) {
  ESP_LOGI(TAG, "Remote command: set vendorfilter mode to %s", val[0] ? "on" : "off");
  cfg.vendorfilter = val[0] ? 1 : 0;
}

void set_rgblum(uint8_t val[]) {
  cfg.rgblum = (val[0] >= 0 && val[0] <= 100) ? (uint8_t)val[0] : RGBLUMINOSITY;
  ESP_LOGI(TAG, "Remote command: set RGB Led luminosity %d", cfg.rgblum);
};

void set_lorapower(uint8_t val[]) {
#if (HAS_LORA)
  if (!cfg.adrmode) {
    cfg.txpower = val[0];
    ESP_LOGI(TAG, "Remote command: set LoRa TXPOWER to %d", cfg.txpower);
    LMIC_setDrTxpow(assertDR(cfg.loradr), cfg.txpower);
  } else
    ESP_LOGI(TAG, "Remote command: set LoRa TXPOWER, not executed because ADR is on");
#else
  ESP_LOGW(TAG, "Remote command: LoRa not implemented");
#endif
};

void get_config(uint8_t val[]) {
  ESP_LOGI(TAG, "Remote command: get device configuration");
  payload.reset();
  payload.addConfig(cfg);
  send_response_direct(CONFIGPORT, prio_high);
};

void get_status(uint8_t val[]) {
  ESP_LOGI(TAG, "Remote command: get device status");

  uint32_t up = (uint32_t)(millis() / 1000);
  uint8_t cputemp = (uint8_t)round(temperatureRead());
  uint16_t free_heap_div16 = (uint16_t)(ESP.getFreeHeap() / 16);
  uint16_t min_heap_div16 = (uint16_t)(ESP.getMinFreeHeap() / 16);
  uint8_t reset_reason = (uint8_t)esp_reset_reason();

  uint8_t flags1 = 0;
  flags1 |= (cfg.wifiscan ? 1 : 0) << 7;
  flags1 |= (cfg.blescan ? 1 : 0) << 6;
  flags1 |= (cfg.btscan ? 1 : 0) << 5;
#if (HAS_LORA)
  flags1 |= (LMIC.devaddr ? 1 : 0) << 4;
#endif
#if (HAS_NBIOT)
  flags1 |= (nb_isEnabled() ? 1 : 0) << 3;
#endif
#ifdef HAS_SDCARD
  flags1 |= (1) << 2;
#endif
#if (HAS_GPS)
  flags1 |= (gps_hasfix() ? 1 : 0) << 1;
#endif

  uint8_t flags2 = 0;
#if (HAS_LORA)
  extern uint8_t healthcheck_failures;
  flags2 = healthcheck_failures;
#endif

  uint8_t lora_rssi = 0;
  int8_t lora_snr = 0;
#if (HAS_LORA)
  lora_rssi = (uint8_t)(LMIC.rssi < 0 ? -LMIC.rssi : LMIC.rssi);
  lora_snr = (int8_t)LMIC.snr;
#endif

  uint8_t nb_rssi = 99;
  uint8_t nb_failures = 0;
#if (HAS_NBIOT)
  nb_rssi = nb_status_rssi;
  nb_failures = nb_status_failures;
#endif

  uint8_t flags3 = 0;
#if (HAS_NBIOT)
  flags3 |= (nb_status_registered ? 1 : 0) << 7;
  flags3 |= (nb_status_connected ? 1 : 0) << 6;
#endif
  uint8_t cpu_freq_code = 0;
  int cpuMHz = getCpuFrequencyMhz();
  if (cpuMHz <= 80) cpu_freq_code = 0;
  else if (cpuMHz <= 160) cpu_freq_code = 1;
  else cpu_freq_code = 2;
  flags3 |= (cpu_freq_code & 0x03) << 4;
  flags3 |= (bt_module_ok ? 1 : 0) << 1;
  flags3 |= (ble_module_ok ? 1 : 0) << 0;

  payload.reset();
  payload.addStatus(up, cputemp, free_heap_div16, min_heap_div16,
                    reset_reason, flags1, flags2,
                    lora_rssi, lora_snr,
                    nb_rssi, nb_failures, flags3);
  send_response_direct(STATUSPORT, prio_high);
};

void get_gps(uint8_t val[]) {
  ESP_LOGI(TAG, "Remote command: get gps status");
#if (HAS_GPS)
  gpsStatus_t gps_status;
  gps_storelocation(&gps_status);
  payload.reset();
  payload.addGPS(gps_status);
  send_response_direct(GPSPORT, prio_high);
#else
  ESP_LOGW(TAG, "GPS function not supported");
#endif
};

void get_bme(uint8_t val[]) {
  ESP_LOGI(TAG, "Remote command: get bme680 sensor data");
#if (HAS_BME)
  payload.reset();
  payload.addBME(bme_status);
  SendPayload(BMEPORT, prio_high);  // BME no es comando remoto crítico, flujo normal
#else
  ESP_LOGW(TAG, "BME sensor not supported");
#endif
};

void get_batt(uint8_t val[]) {
  ESP_LOGI(TAG, "Remote command: get battery voltage");
#if (defined BAT_MEASURE_ADC || defined HAS_PMU)
  payload.reset();
  payload.addVoltage(read_voltage());
  send_response_direct(BATTPORT, prio_normal);
#else
  ESP_LOGW(TAG, "Battery voltage not supported");
#endif
};

void get_time(uint8_t val[]) {
  ESP_LOGI(TAG, "Remote command: get time");
  payload.reset();
  payload.addTime(now());
  payload.addByte(timeStatus() << 4 | timeSource);
  send_response_direct(TIMEPORT, prio_high);
};

void set_time(uint8_t val[]) {
  ESP_LOGI(TAG, "Timesync requested by timeserver");
  timeSync();
};

void set_rtc_timestamp(uint8_t val[]) {
  uint32_t epoch = (uint32_t)val[0] * 256 * 256 * 256 + (uint32_t)val[1] * 256 * 256 +
                   (uint32_t)val[2] * 256 + (uint32_t)val[0];
  ESP_LOGI(TAG, "Force RTC timestamp to: %l", epoch);
  set_rtctime(epoch);
  setMyTime(epoch, 0, _rtc);
};

void set_flush(uint8_t val[]) {
  ESP_LOGI(TAG, "Remote command: flush");
};

void set_nb_server(uint8_t val[]) {
  ESP_LOGI(TAG, "Remote command: set_nb_server");
  ConfigBuffer_t conf;
  sdLoadNbConfig(&conf);
  for (int i = 0; i < 45; i++) {
    conf.ServerAddress[i] = val[i];
    if (val[i] == 0) break;
    if (i == 44) val[45] = 0;
  }
  ESP_LOGI(TAG, "Setting NB server to: %s", conf.ServerAddress);
  sdSaveNbConfig(&conf);
};

void set_nb_username(uint8_t val[]) {
  ESP_LOGI(TAG, "Remote command: set_nb_username");
  ConfigBuffer_t conf;
  sdLoadNbConfig(&conf);
  for (int i = 0; i < 45; i++) {
    conf.ServerUsername[i] = val[i];
    if (val[i] == 0) break;
    if (i == 44) val[45] = 0;
  }
  ESP_LOGI(TAG, "Setting NB username to: %s", conf.ServerUsername);
  sdSaveNbConfig(&conf);
};

void set_nb_password(uint8_t val[]) {
  ESP_LOGI(TAG, "Remote command: set_nb_password");
  ConfigBuffer_t conf;
  sdLoadNbConfig(&conf);
  for (int i = 0; i < 45; i++) {
    conf.ServerPassword[i] = val[i];
    if (val[i] == 0) break;
    if (i == 44) val[45] = 0;
  }
  ESP_LOGI(TAG, "Setting NB pass to: %s", conf.ServerPassword);
  sdSaveNbConfig(&conf);
};

void set_nb_gateway_id(uint8_t val[]) {
  ESP_LOGI(TAG, "Remote command: set_nb_gateway_id");
  ConfigBuffer_t conf;
  sdLoadNbConfig(&conf);
  for (int i = 0; i < 45; i++) {
    conf.GatewayId[i] = val[i];
    if (val[i] == 0) break;
    if (i == 44) val[45] = 0;
  }
  ESP_LOGI(TAG, "Setting NB gateway ID to: %s", conf.GatewayId);
  sdSaveNbConfig(&conf);
};

void set_nb_app_id(uint8_t val[]) {
  ESP_LOGI(TAG, "Remote command: set_nb_app_id");
  ConfigBuffer_t conf;
  sdLoadNbConfig(&conf);
  for (int i = 0; i < 5; i++) {
    conf.ApplicationId[i] = val[i];
    if (val[i] == 0) break;
    if (i == 4) val[5] = 0;
  }
  ESP_LOGI(TAG, "Setting NB app id to: %s", conf.ApplicationId);
  sdSaveNbConfig(&conf);
};

void set_nb_app_name(uint8_t val[]) {
  ESP_LOGI(TAG, "Remote command: set_nb_app_name");
  ConfigBuffer_t conf;
  sdLoadNbConfig(&conf);
  for (int i = 0; i < 31; i++) {
    conf.ApplicationName[i] = val[i];
    if (val[i] == 0) break;
    if (i == 30) val[31] = 0;
  }
  ESP_LOGI(TAG, "Setting NB app name to: %s", conf.ApplicationId);
  sdSaveNbConfig(&conf);
};

void set_nb_port(uint8_t val[]) {
  ESP_LOGI(TAG, "Remote command: set_nb_port");
  ConfigBuffer_t conf;
  sdLoadNbConfig(&conf);
  conf.port = val[1] * 256 + val[0];
  ESP_LOGI(TAG, "Setting NB port to: %d", conf.port);
  sdSaveNbConfig(&conf);
};

void set_reset_time(uint8_t val[]) {
  ESP_LOGI(TAG, "Remote command: set_reset_time");
  cfg.resettimer = val[0];
  ESP_LOGI(TAG, "Setting reset timer to: %d", cfg.resettimer);
};

#if (HAS_NBIOT)
static void imeiTask(void *param) {
  (void)param;

  String imei = bc95_getImei();

  payload.reset();
  payload.addByte(0x8A);

  if (imei.length() == 15) {
    for (int i = 0; i < 15; i++)
      payload.addByte((uint8_t)imei[i]);
  } else {
    payload.addByte(0x00);
  }

  send_response_direct(RCMDPORT, prio_high);

  imeiTaskHandle = NULL;
  vTaskDelete(NULL);
}

static void msisdnTask(void *param) {
    (void)param;

    String msisdn = bc95_getMsisdn();

    payload.reset();
    payload.addByte(0x8B);

    if (msisdn.length() > 0) {
        for (int i = 0; i < msisdn.length(); i++)
            payload.addByte((uint8_t)msisdn[i]);
    } else {
        payload.addByte(0x00);
    }

    send_response_direct(RCMDPORT, prio_high);

    msisdnTaskHandle = NULL;
    vTaskDelete(NULL);
}

static void get_msisdn(uint8_t val[]) {
    ESP_LOGI(TAG, "Remote command: get MSISDN (SIM phone number)");
    if (msisdnTaskHandle != NULL) {
        ESP_LOGW(TAG, "MSISDN task already running");
        return;
    }
    xTaskCreatePinnedToCore(msisdnTask, "msisdnTask", 4096, NULL, 1, &msisdnTaskHandle, 1);
}

static void get_imei(uint8_t val[]) {
  ESP_LOGI(TAG, "Remote command: get IMEI (BC95)");

  if (imeiTaskHandle != NULL) {
    ESP_LOGW(TAG, "IMEI task already running, ignoring request");
    return;
  }

  xTaskCreatePinnedToCore(imeiTask, "imeiTask", 4096, NULL, 1, &imeiTaskHandle, 1);
}
#endif

static cmd_t table[] = {
    {0x01, set_rssi, 1, true},      {0x02, set_countmode, 1, true},
    {0x03, set_gps, 1, true},       {0x04, set_display, 1, true},
    {0x05, set_loradr, 1, true},    {0x06, set_lorapower, 1, true},
    {0x07, set_loraadr, 1, true},   {0x08, set_screensaver, 1, true},
    {0x09, set_reset, 1, false},    {0x0a, set_sendcycle, 2, true},
    {0x0b, set_wifichancycle, 1, true}, {0x0c, set_blescantime, 1, true},
    {0x0d, set_vendorfilter, 1, false}, {0x0e, set_blescan, 1, true},
    {0x0f, set_wifiant, 1, true},   {0x10, set_rgblum, 1, true},
    {0x11, set_monitor, 1, true},   {0x12, set_beacon, 7, false},
    {0x13, set_sensor, 2, true},    {0x14, set_payloadmask, 1, true},
    {0x15, set_bme, 1, true},       {0x16, set_batt, 1, true},
    {0x17, set_wifiscan, 1, true},  {0x18, set_salt, 12, true},
    {0x19, set_btscan, 1, true},    {0x1A, set_nb_server, 45, true},
    {0x1B, set_nb_password, 45, true}, {0x1C, set_nb_app_id, 5, true},
    {0x1D, set_nb_app_name, 31, true}, {0x1E, set_nb_port, 2, true},
    {0x1F, set_nb_gateway_id, 45, true}, {0x20, set_rtc_timestamp, 4, true},
    {0x21, set_nb_username, 45, true},

    {0x80, get_config, 0, false},
    {0x81, get_status, 0, false},
    {0x83, get_batt, 0, false},
    {0x84, get_gps, 0, false},
    {0x85, get_bme, 0, false},
    {0x86, get_time, 0, false},
    {0x87, set_time, 0, false},
    {0x88, get_userSalt, 0, false},
    {0x89, set_reset_time, 1, true},

#if (HAS_NBIOT)
    {0x8A, get_imei, 0, false},
    {0x8B, get_msisdn, 0, false},
#endif

    {0x99, set_flush, 0, false}
};

static const uint8_t cmdtablesize =
    sizeof(table) / sizeof(table[0]);

void rcommand(const uint8_t cmd[], const uint8_t cmdlength) {
  if (cmdlength == 0)
    return;

  uint8_t foundcmd[cmdlength], cursor = 0;
  bool storeflag = false;

  while (cursor < cmdlength) {
    int i = cmdtablesize;
    while (i--) {
      if (cmd[cursor] == table[i].opcode) {
        cursor++;
        if ((cursor + table[i].params) <= cmdlength) {
          memmove(foundcmd, cmd + cursor, table[i].params);
          cursor += table[i].params;
          if (table[i].store)
            storeflag = true;
          table[i].func(foundcmd);
        } else
          ESP_LOGI(TAG,
                   "Remote command x%02X called with missing parameter(s), skipped",
                   table[i].opcode);
        break;
      }
    }

    if (i < 0) {
      ESP_LOGI(TAG, "Unknown remote command x%02X, ignored", cmd[cursor]);
      break;
    }
  }

  if (storeflag)
    saveConfig();
}