#include "globals.h"
#include "payload.h"

PayloadConvert::PayloadConvert(uint8_t size) {
  buffer = (uint8_t *)malloc(size);
  cursor = 0;
}

PayloadConvert::~PayloadConvert(void) { free(buffer); }

void PayloadConvert::reset(void) { cursor = 0; }

uint8_t PayloadConvert::getSize(void) { return cursor; }

uint8_t *PayloadConvert::getBuffer(void) { return buffer; }

/* ---------------- plain format without special encoding ---------- */

#if (PAYLOAD_ENCODER == 1)

void PayloadConvert::addByte(uint8_t value) { buffer[cursor++] = (value); }

void PayloadConvert::addCount(uint16_t value, uint8_t snifftype) {
  buffer[cursor++] = highByte(value);
  buffer[cursor++] = lowByte(value);
}

void PayloadConvert::addSalt(uint32_t value) {
  buffer[cursor++] = (value >> 24) & 0xFF;
  buffer[cursor++] = (value >> 16) & 0xFF;
  buffer[cursor++] = (value >> 8) & 0xFF;
  buffer[cursor++] = (value ) & 0xFF;
}

void PayloadConvert::addSaltVersion(uint32_t value) {
  buffer[cursor++] = (value >> 24) & 0xFF;
  buffer[cursor++] = (value >> 16) & 0xFF;
  buffer[cursor++] = (value >> 8) & 0xFF;
  buffer[cursor++] = (value ) & 0xFF;
}

void PayloadConvert::addSaltTimestamp(uint32_t value) {
  buffer[cursor++] = (value >> 24) & 0xFF;
  buffer[cursor++] = (value >> 16) & 0xFF;
  buffer[cursor++] = (value >> 8) & 0xFF;
  buffer[cursor++] = (value ) & 0xFF;
}

void PayloadConvert::setupMac(uint16_t amount) {
  buffer[cursor++] = (amount >> 8) & 0xFF;
  buffer[cursor++] = (amount ) & 0xFF;
}

void PayloadConvert::addMac(uint32_t value) {
  buffer[cursor++] = (value >> 24) & 0xFF;
  buffer[cursor++] = (value >> 16) & 0xFF;
  buffer[cursor++] = (value >> 8) & 0xFF;
  buffer[cursor++] = (value ) & 0xFF;
}

void PayloadConvert::addAlarm(int8_t rssi, uint8_t msg) {
  buffer[cursor++] = rssi;
  buffer[cursor++] = msg;
}

void PayloadConvert::addVoltage(uint16_t value) {
  buffer[cursor++] = highByte(value);
  buffer[cursor++] = lowByte(value);
}

void PayloadConvert::addConfig(configData_t value) {
  buffer[cursor++] = value.loradr;
  buffer[cursor++] = value.txpower;
  buffer[cursor++] = value.adrmode;
  buffer[cursor++] = value.screensaver;
  buffer[cursor++] = value.screenon;
  buffer[cursor++] = value.countermode;
  buffer[cursor++] = highByte(value.rssilimit);
  buffer[cursor++] = lowByte(value.rssilimit);
  buffer[cursor++] = value.sendcycle;
  buffer[cursor++] = value.wifichancycle;
  buffer[cursor++] = value.blescantime;
  buffer[cursor++] = value.blescan;
  buffer[cursor++] = value.wifiant;
  buffer[cursor++] = value.vendorfilter;
  buffer[cursor++] = value.rgblum;
  buffer[cursor++] = value.payloadmask;
  buffer[cursor++] = value.monitormode;
  memcpy(buffer + cursor, value.version, 10);
  cursor += 10;
}

// === ADEMUX: addStatus extendido a 19 bytes ===
// Offset 14: nb_rsrp (antes nb_rssi/CSQ) — encoding: (-rsrp_dBm)-44, 0xFF=N/A
// Offset 17: nb_snr  — encoding: snr_dB+20, 0xFF=N/A
// Offset 18: nb_ecl  — directo 0/1/2, 0xFF=N/A
void PayloadConvert::addStatus(uint32_t uptime, uint8_t cputemp,
                               uint16_t free_heap_div16, uint16_t min_heap_div16,
                               uint8_t reset_reason, uint8_t flags1, uint8_t flags2,
                               uint8_t lora_rssi, int8_t lora_snr,
                               uint8_t nb_rsrp, uint8_t nb_failures,
                               uint8_t flags3,
                               uint8_t nb_snr_encoded, uint8_t nb_ecl) {
  // Offset 0-3: uptime (uint32, big-endian)
  buffer[cursor++] = (byte)((uptime & 0xFF000000) >> 24);
  buffer[cursor++] = (byte)((uptime & 0x00FF0000) >> 16);
  buffer[cursor++] = (byte)((uptime & 0x0000FF00) >> 8);
  buffer[cursor++] = (byte)((uptime & 0x000000FF));
  // Offset 4: cputemp
  buffer[cursor++] = cputemp;
  // Offset 5-6: free_heap / 16 (uint16, big-endian)
  buffer[cursor++] = highByte(free_heap_div16);
  buffer[cursor++] = lowByte(free_heap_div16);
  // Offset 7-8: min_free_heap / 16 (uint16, big-endian)
  buffer[cursor++] = highByte(min_heap_div16);
  buffer[cursor++] = lowByte(min_heap_div16);
  // Offset 9: reset_reason
  buffer[cursor++] = reset_reason;
  // Offset 10: flags1
  buffer[cursor++] = flags1;
  // Offset 11: flags2 (healthcheck_failures)
  buffer[cursor++] = flags2;
  // Offset 12: lora_rssi (valor absoluto)
  buffer[cursor++] = lora_rssi;
  // Offset 13: lora_snr (int8)
  buffer[cursor++] = (uint8_t)lora_snr;
  // Offset 14: nb_rsrp — encoding: (-rsrp_dBm)-44, 0xFF=N/A
  buffer[cursor++] = nb_rsrp;
  // Offset 15: nb_consecutiveFailures
  buffer[cursor++] = nb_failures;
  // Offset 16: flags3
  buffer[cursor++] = flags3;
  // Offset 17: nb_snr — encoding: snr_dB+20, 0xFF=N/A
  buffer[cursor++] = nb_snr_encoded;
  // Offset 18: nb_ecl — 0/1/2 directo, 0xFF=N/A
  buffer[cursor++] = nb_ecl;
}

void PayloadConvert::addGPS(gpsStatus_t value) {
#if(HAS_GPS)
  buffer[cursor++] = (byte)((value.latitude & 0xFF000000) >> 24);
  buffer[cursor++] = (byte)((value.latitude & 0x00FF0000) >> 16);
  buffer[cursor++] = (byte)((value.latitude & 0x0000FF00) >> 8);
  buffer[cursor++] = (byte)((value.latitude & 0x000000FF));
  buffer[cursor++] = (byte)((value.longitude & 0xFF000000) >> 24);
  buffer[cursor++] = (byte)((value.longitude & 0x00FF0000) >> 16);
  buffer[cursor++] = (byte)((value.longitude & 0x0000FF00) >> 8);
  buffer[cursor++] = (byte)((value.longitude & 0x000000FF));
#if (!PAYLOAD_OPENSENSEBOX)
  buffer[cursor++] = value.satellites;
  buffer[cursor++] = highByte(value.hdop);
  buffer[cursor++] = lowByte(value.hdop);
  buffer[cursor++] = highByte(value.altitude);
  buffer[cursor++] = lowByte(value.altitude);
#endif
#endif
}

void PayloadConvert::addSensor(uint8_t buf[]) {
#if(HAS_SENSORS)
  uint8_t length = buf[0];
  memcpy(buffer, buf + 1, length);
  cursor += length;
#endif
}

void PayloadConvert::addBME(bmeStatus_t value) {
#if(HAS_BME)
  int16_t temperature = (int16_t)(value.temperature);
  uint16_t humidity = (uint16_t)(value.humidity);
  uint16_t pressure = (uint16_t)(value.pressure);
  uint16_t iaq = (uint16_t)(value.iaq);
  buffer[cursor++] = highByte(temperature);
  buffer[cursor++] = lowByte(temperature);
  buffer[cursor++] = highByte(pressure);
  buffer[cursor++] = lowByte(pressure);
  buffer[cursor++] = highByte(humidity);
  buffer[cursor++] = lowByte(humidity);
  buffer[cursor++] = highByte(iaq);
  buffer[cursor++] = lowByte(iaq);
#endif
}

void PayloadConvert::addButton(uint8_t value) {
#ifdef HAS_BUTTON
  buffer[cursor++] = value;
#endif
}

void PayloadConvert::addTime(time_t value) {
  uint32_t time = (uint32_t)value;
  buffer[cursor++] = (byte)((time & 0xFF000000) >> 24);
  buffer[cursor++] = (byte)((time & 0x00FF0000) >> 16);
  buffer[cursor++] = (byte)((time & 0x0000FF00) >> 8);
  buffer[cursor++] = (byte)((time & 0x000000FF));
}

/* ---------------- packed format with LoRa serialization Encoder ---------- */

#elif (PAYLOAD_ENCODER == 2)

void PayloadConvert::addByte(uint8_t value) { writeUint8(value); }

void PayloadConvert::addCount(uint16_t value, uint8_t snifftype) {
  writeUint16(value);
}

void PayloadConvert::addAlarm(int8_t rssi, uint8_t msg) {
  writeUint8(rssi);
  writeUint8(msg);
}

void PayloadConvert::addVoltage(uint16_t value) { writeUint16(value); }

void PayloadConvert::addConfig(configData_t value) {
  writeUint8(value.loradr);
  writeUint8(value.txpower);
  writeUint16(value.rssilimit);
  writeUint8(value.sendcycle);
  writeUint8(value.wifichancycle);
  writeUint8(value.blescantime);
  writeUint8(value.rgblum);
  writeBitmap(value.adrmode ? true : false, value.screensaver ? true : false,
              value.screenon ? true : false, value.countermode ? true : false,
              value.blescan ? true : false, value.wifiant ? true : false,
              value.vendorfilter ? true : false,
              value.monitormode ? true : false);
  writeBitmap(value.payloadmask && GPS_DATA ? true : false,
              value.payloadmask && ALARM_DATA ? true : false,
              value.payloadmask && MEMS_DATA ? true : false,
              value.payloadmask && COUNT_DATA ? true : false,
              value.payloadmask && SENSOR1_DATA ? true : false,
              value.payloadmask && SENSOR2_DATA ? true : false,
              value.payloadmask && SENSOR3_DATA ? true : false,
              value.payloadmask && BATT_DATA ? true : false);
  writeVersion(value.version);
}

// === ADEMUX: addStatus extendido a 19 bytes ===
void PayloadConvert::addStatus(uint32_t uptime, uint8_t cputemp,
                               uint16_t free_heap_div16, uint16_t min_heap_div16,
                               uint8_t reset_reason, uint8_t flags1, uint8_t flags2,
                               uint8_t lora_rssi, int8_t lora_snr,
                               uint8_t nb_rsrp, uint8_t nb_failures,
                               uint8_t flags3,
                               uint8_t nb_snr_encoded, uint8_t nb_ecl) {
  writeUint32(uptime);           // 0-3
  writeUint8(cputemp);           // 4
  writeUint16(free_heap_div16);  // 5-6
  writeUint16(min_heap_div16);   // 7-8
  writeUint8(reset_reason);      // 9
  writeUint8(flags1);            // 10
  writeUint8(flags2);            // 11
  writeUint8(lora_rssi);         // 12
  writeUint8((uint8_t)lora_snr); // 13
  writeUint8(nb_rsrp);           // 14 (antes CSQ, ahora RSRP)
  writeUint8(nb_failures);       // 15
  writeUint8(flags3);            // 16
  writeUint8(nb_snr_encoded);    // 17 nuevo
  writeUint8(nb_ecl);            // 18 nuevo
}

void PayloadConvert::addGPS(gpsStatus_t value) {
#if(HAS_GPS)
  writeLatLng(value.latitude, value.longitude);
#if (!PAYLOAD_OPENSENSEBOX)
  writeUint8(value.satellites);
  writeUint16(value.hdop);
  writeUint16(value.altitude);
#endif
#endif
}

void PayloadConvert::addSensor(uint8_t buf[]) {
#if(HAS_SENSORS)
  uint8_t length = buf[0];
  memcpy(buffer, buf + 1, length);
  cursor += length;
#endif
}

void PayloadConvert::addBME(bmeStatus_t value) {
#if(HAS_BME)
  writeFloat(value.temperature);
  writePressure(value.pressure);
  writeUFloat(value.humidity);
  writeUFloat(value.iaq);
#endif
}

void PayloadConvert::addButton(uint8_t value) {
#ifdef HAS_BUTTON
  writeUint8(value);
#endif
}

void PayloadConvert::addTime(time_t value) {
  uint32_t time = (uint32_t)value;
  writeUint32(time);
}

void PayloadConvert::uintToBytes(uint64_t value, uint8_t byteSize) {
  for (uint8_t x = 0; x < byteSize; x++) {
    byte next = 0;
    if (sizeof(value) > x) {
      next = static_cast<byte>((value >> (x * 8)) & 0xFF);
    }
    buffer[cursor] = next;
    ++cursor;
  }
}

void PayloadConvert::writeUptime(uint64_t uptime) { writeUint64(uptime); }

void PayloadConvert::writeVersion(char *version) {
  memcpy(buffer + cursor, version, 10);
  cursor += 10;
}

void PayloadConvert::writeLatLng(double latitude, double longitude) {
  writeUint32(latitude);
  writeUint32(longitude);
}

void PayloadConvert::writeUint64(uint64_t i) { uintToBytes(i, 8); }
void PayloadConvert::writeUint32(uint32_t i) { uintToBytes(i, 4); }
void PayloadConvert::writeUint16(uint16_t i) { uintToBytes(i, 2); }
void PayloadConvert::writeUint8(uint8_t i)   { uintToBytes(i, 1); }

void PayloadConvert::writeUFloat(float value) { writeUint16(value * 100); }
void PayloadConvert::writePressure(float value) { writeUint16(value * 10); }

void PayloadConvert::writeFloat(float value) {
  int16_t t = (int16_t)(value * 100);
  if (value < 0) {
    t = ~-t;
    t = t + 1;
  }
  buffer[cursor++] = (byte)((t >> 8) & 0xFF);
  buffer[cursor++] = (byte)t & 0xFF;
}

void PayloadConvert::writeBitmap(bool a, bool b, bool c, bool d, bool e, bool f,
                                 bool g, bool h) {
  uint8_t bitmap = 0;
  bitmap |= (a & 1) << 7;
  bitmap |= (b & 1) << 6;
  bitmap |= (c & 1) << 5;
  bitmap |= (d & 1) << 4;
  bitmap |= (e & 1) << 3;
  bitmap |= (f & 1) << 2;
  bitmap |= (g & 1) << 1;
  bitmap |= (h & 1) << 0;
  writeUint8(bitmap);
}

/* ---------------- Cayenne LPP 2.0 format ---------- */

#elif ((PAYLOAD_ENCODER == 3) || (PAYLOAD_ENCODER == 4))

void PayloadConvert::addByte(uint8_t value) { }

void PayloadConvert::addCount(uint16_t value, uint8_t snifftype) {
  switch (snifftype) {
  case MAC_SNIFF_WIFI:
#if (PAYLOAD_ENCODER == 3)
    buffer[cursor++] = LPP_COUNT_WIFI_CHANNEL;
#endif
    buffer[cursor++] = LPP_LUMINOSITY;
    buffer[cursor++] = highByte(value);
    buffer[cursor++] = lowByte(value);
    break;
  case MAC_SNIFF_BLE:
#if (PAYLOAD_ENCODER == 3)
    buffer[cursor++] = LPP_COUNT_BLE_CHANNEL;
#endif
    buffer[cursor++] = LPP_LUMINOSITY;
    buffer[cursor++] = highByte(value);
    buffer[cursor++] = lowByte(value);
    break;
  }
}

void PayloadConvert::addAlarm(int8_t rssi, uint8_t msg) {
#if (PAYLOAD_ENCODER == 3)
  buffer[cursor++] = LPP_ALARM_CHANNEL;
#endif
  buffer[cursor++] = LPP_PRESENCE;
  buffer[cursor++] = msg;
#if (PAYLOAD_ENCODER == 3)
  buffer[cursor++] = LPP_MSG_CHANNEL;
#endif
  buffer[cursor++] = LPP_ANALOG_INPUT;
  buffer[cursor++] = rssi;
}

void PayloadConvert::addVoltage(uint16_t value) {
  uint16_t volt = value / 10;
#if (PAYLOAD_ENCODER == 3)
  buffer[cursor++] = LPP_BATT_CHANNEL;
#endif
  buffer[cursor++] = LPP_ANALOG_INPUT;
  buffer[cursor++] = highByte(volt);
  buffer[cursor++] = lowByte(volt);
}

void PayloadConvert::addConfig(configData_t value) {
#if (PAYLOAD_ENCODER == 3)
  buffer[cursor++] = LPP_ADR_CHANNEL;
#endif
  buffer[cursor++] = LPP_DIGITAL_INPUT;
  buffer[cursor++] = value.adrmode;
}

// Cayenne LPP: firma actualizada para mantener compatibilidad de compilación
void PayloadConvert::addStatus(uint32_t uptime, uint8_t cputemp,
                               uint16_t free_heap_div16, uint16_t min_heap_div16,
                               uint8_t reset_reason, uint8_t flags1, uint8_t flags2,
                               uint8_t lora_rssi, int8_t lora_snr,
                               uint8_t nb_rsrp, uint8_t nb_failures,
                               uint8_t flags3,
                               uint8_t nb_snr_encoded, uint8_t nb_ecl) {
  // Cayenne LPP solo envía temperatura; datos completos van en raw por TELEMETRYPORT
  uint16_t temp = (uint16_t)cputemp * 10;
#if (PAYLOAD_ENCODER == 3)
  buffer[cursor++] = LPP_TEMPERATURE_CHANNEL;
#endif
  buffer[cursor++] = LPP_TEMPERATURE;
  buffer[cursor++] = highByte(temp);
  buffer[cursor++] = lowByte(temp);
}

void PayloadConvert::addGPS(gpsStatus_t value) {
#if(HAS_GPS)
  int32_t lat = value.latitude / 100;
  int32_t lon = value.longitude / 100;
  int32_t alt = value.altitude * 100;
#if (PAYLOAD_ENCODER == 3)
  buffer[cursor++] = LPP_GPS_CHANNEL;
#endif
  buffer[cursor++] = LPP_GPS;
  buffer[cursor++] = (byte)((lat & 0xFF0000) >> 16);
  buffer[cursor++] = (byte)((lat & 0x00FF00) >> 8);
  buffer[cursor++] = (byte)((lat & 0x0000FF));
  buffer[cursor++] = (byte)((lon & 0xFF0000) >> 16);
  buffer[cursor++] = (byte)((lon & 0x00FF00) >> 8);
  buffer[cursor++] = (byte)(lon & 0x0000FF);
  buffer[cursor++] = (byte)((alt & 0xFF0000) >> 16);
  buffer[cursor++] = (byte)((alt & 0x00FF00) >> 8);
  buffer[cursor++] = (byte)(alt & 0x0000FF);
#endif
}

void PayloadConvert::addSensor(uint8_t buf[]) { }

void PayloadConvert::addBME(bmeStatus_t value) {
#if(HAS_BME)
  int16_t temperature = (int16_t)(value.temperature * 10.0);
  uint16_t pressure = (uint16_t)(value.pressure * 10);
  uint8_t humidity = (uint8_t)(value.humidity * 2.0);
  int16_t iaq = (int16_t)(value.iaq);
#if (PAYLOAD_ENCODER == 3)
  buffer[cursor++] = LPP_TEMPERATURE_CHANNEL;
#endif
  buffer[cursor++] = LPP_TEMPERATURE;
  buffer[cursor++] = highByte(temperature);
  buffer[cursor++] = lowByte(temperature);
#if (PAYLOAD_ENCODER == 3)
  buffer[cursor++] = LPP_BAROMETER_CHANNEL;
#endif
  buffer[cursor++] = LPP_BAROMETER;
  buffer[cursor++] = highByte(pressure);
  buffer[cursor++] = lowByte(pressure);
#if (PAYLOAD_ENCODER == 3)
  buffer[cursor++] = LPP_HUMIDITY_CHANNEL;
#endif
  buffer[cursor++] = LPP_HUMIDITY;
  buffer[cursor++] = humidity;
#if (PAYLOAD_ENCODER == 3)
  buffer[cursor++] = LPP_AIR_CHANNEL;
#endif
  buffer[cursor++] = LPP_LUMINOSITY;
  buffer[cursor++] = highByte(iaq);
  buffer[cursor++] = lowByte(iaq);
#endif
}

void PayloadConvert::addButton(uint8_t value) {
#ifdef HAS_BUTTON
#if (PAYLOAD_ENCODER == 3)
  buffer[cursor++] = LPP_BUTTON_CHANNEL;
#endif
  buffer[cursor++] = LPP_DIGITAL_INPUT;
  buffer[cursor++] = value;
#endif
}

void PayloadConvert::addTime(time_t value) {
#if (PAYLOAD_ENCODER == 4)
  uint32_t t = (uint32_t)value;
  uint32_t tx_period = (uint32_t)SENDCYCLE * 2;
  buffer[cursor++] = 0x03;
  buffer[cursor++] = (byte)((t & 0xFF000000) >> 24);
  buffer[cursor++] = (byte)((t & 0x00FF0000) >> 16);
  buffer[cursor++] = (byte)((t & 0x0000FF00) >> 8);
  buffer[cursor++] = (byte)((t & 0x000000FF));
  buffer[cursor++] = (byte)((tx_period & 0xFF000000) >> 24);
  buffer[cursor++] = (byte)((tx_period & 0x00FF0000) >> 16);
  buffer[cursor++] = (byte)((tx_period & 0x0000FF00) >> 8);
  buffer[cursor++] = (byte)((tx_period & 0x000000FF));
#endif
}

#endif