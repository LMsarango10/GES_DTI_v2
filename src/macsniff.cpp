
// Basic Config
#include "globals.h"

#if (VENDORFILTER)
#include "vendor_array.h"
#endif

// Local logging tag
static const char TAG[] = __FILE__;

char salt[9];

char *get_salt(void) {
  snprintf(salt, sizeof(salt), "%08X", cfg.salt);
  return salt;
}

int8_t isBeacon(uint64_t mac) {
  it = std::find(beacons.begin(), beacons.end(), mac);
  if (it != beacons.end())
    return std::distance(beacons.begin(), it);
  else
    return -1;
}

// Display a key
void printKey(const char *name, const uint8_t *key, uint8_t len, bool lsb) {
  const uint8_t *p;
  char keystring[len + 1] = "", keybyte[3];
  for (uint8_t i = 0; i < len; i++) {
    p = lsb ? key + len - i - 1 : key + i;
    snprintf(keybyte, 3, "%02X", *p);
    strncat(keystring, keybyte, 2);
  }
  ESP_LOGI(TAG, "%s: %s", name, keystring);
}

uint64_t macConvert(uint8_t *paddr) {
  uint64_t *mac;
  mac = (uint64_t *)paddr;
  return (__builtin_bswap64(*mac) >> 16);
}
bool mac_add(uint8_t *paddr, int8_t rssi, uint8_t sniff_type) {
  if (!salt) // ensure we have salt (appears after radio is turned on)
    return false;

  char buff[32]; // temporary buffer for printf
  bool added = false;
  int8_t beaconID;    // beacon number in test monitor mode
  uint32_t hashedmac; // temporary buffer for generated hash value

  // if it is NOT a locally administered ("random") mac, we don't count it
  if (!(paddr[0] & 0b10)) return false;

#if (VENDORFILTER)
  uint32_t *oui; // temporary buffer for vendor OUI
  oui = (uint32_t *)paddr;

  // use OUI vendor filter list only on Wifi, not on BLE
  if ((sniff_type == MAC_SNIFF_BLE) || (sniff_type == MAC_SNIFF_BT) ||
      std::find(vendors_list.begin(), vendors_list.end(),
                __builtin_bswap32(*oui) >> 8) != vendors_list.end()) {
#endif

    // salt and hash MAC, and if new unique one, store identifier in container
    // and increment counter on display
    // https://en.wikipedia.org/wiki/MAC_Address_Anonymization

    /*  ESP_LOGI(TAG, "MAC is: %02X%02X%02X%02X%02X%02X",
      paddr[0],paddr[1],paddr[2],paddr[3],paddr[4],paddr[5]);*/

    snprintf(buff, sizeof(buff), "%02X:%02X:%02X:%02X:%02X:%02X", paddr[0],
             paddr[1], paddr[2], paddr[3], paddr[4], paddr[5]);
    //  convert unsigned 32-bit salted MAC
    // to 8 digit hex string
    // hashedmac = rokkit(&buff[0], 5);      // hash MAC 8 digit -> 5 digit
    char out[METIS_OUTPUT_HASH_LENGTH];
    // ESP_LOGV(TAG, "salted MAC %d", in);
    metis_enable_printing(true);

    /**
     * Determinar si una dirección MAC corresponde de manera probable a un
     * dispositivo móvil.
     * Esta función realiza una consulta a un listado interno de la biblioteca
     * para determinar si, por medio del fabricante indicado en la dirección
     * MAC, el dispositivo por el que se ha consultado es susceptible de ser un
     * Smartphone, Tablet, u otro dispositivo personal con funcionalidad WiFi
     */

    bool *isDev = false;
    // metis_is_device((char *)in, isDev);
    // metis_digest_mac_salt(in, salt, out); // Last version
    #ifdef DEBUG_METIS
    ESP_LOGI(TAG, "Content of buff is: %s", buff);
    #endif
    if (metis_digest_mac_from_str_salt(buff, salt, out) ==
        metis_failure_reason_none) {
          #ifdef DEBUG_METIS
      ESP_LOGD(TAG, "(METIS) OK!\n");
      ESP_LOGD(TAG, "(METIS) Digest Mac: %s\n", out);
      #endif
    } else {
      ESP_LOGD(TAG, "(METIS) FAILED\n");
    }

    hashedmac = (uint32_t) strtoul(out, NULL, 16);

    char hashedmacbuff[20];
    snprintf(hashedmacbuff, sizeof(hashedmacbuff), "%08X", hashedmac);

    switch (sniff_type) {
    case MAC_SNIFF_WIFI: {
      auto newmac =
          macs_list_wifi.insert(hashedmac); // add hashed MAC, if new unique
      added = newmac.second
                  ? true
                  : false; // true if hashed MAC is unique in container
      if (added) {
        macs_wifi++; // increment Wifi MACs counter
#if (HAS_LED != NOT_A_PIN) || defined(HAS_RGB_LED)
        blink_LED(COLOR_GREEN, 50);
#endif
      }

      break;
    }
    case MAC_SNIFF_BLE: {
      auto newmac =
          macs_list_ble.insert(hashedmac); // add hashed MAC, if new unique
      added = newmac.second
                  ? true
                  : false; // true if hashed MAC is unique in container
      if (added) {
        macs_ble++; // increment Wifi MACs counter
#if (HAS_LED != NOT_A_PIN) || defined(HAS_RGB_LED)
        blink_LED(COLOR_MAGENTA, 50);
#endif
      }
      break;
    }
    case MAC_SNIFF_BT: {
      auto newmac =
          macs_list_bt.insert(hashedmac); // add hashed MAC, if new unique
      added = newmac.second
                  ? true
                  : false; // true if hashed MAC is unique in container
      if (added) {
        macs_bt++; // increment Wifi MACs counter
#if (HAS_LED != NOT_A_PIN) || defined(HAS_RGB_LED)
        blink_LED(COLOR_BLUE_MAGENTA, 50);
#endif
      }
      break;
    }
    }

    // in beacon monitor mode check if seen MAC is a known beacon
    if (cfg.monitormode) {

      beaconID = isBeacon(macConvert(paddr)); /// MIRAR PARA CAMBIAR TODO:

      if (beaconID >= 0) {

        ESP_LOGI(TAG, "Beacon ID#%d detected", beaconID);
#if (HAS_LED != NOT_A_PIN) || defined(HAS_RGB_LED)
        blink_LED(COLOR_WHITE, 2000);
#endif
        payload.reset();
        payload.addAlarm(rssi, beaconID);
        SendPayload(BEACONPORT, prio_high);
      }
    }

    // added

    // Log scan result
    if (added) { // DESCOMENTAR PARA LOG CAMBIAR TODO:
      ESP_LOGD(TAG, "%s salt", salt);

      ESP_LOGD(TAG,
               "%s %s RSSI %ddBi -> salted MAC %s -> Hash %s -> WiFi:%d  "
               "BLE:%d -> BLTH:%d -> "
               "%d Bytes left",
               added ? "new  " : "known",
               sniff_type == MAC_SNIFF_WIFI  ? "WiFi"
               : sniff_type == MAC_SNIFF_BLE ? "BLE"
                                             : "BLTH",
               rssi, out, hashedmacbuff, macs_wifi, macs_ble, macs_bt,
               getFreeRAM());
      ESP_LOGV(TAG, "MAC is: %02X%02X%02X%02X%02X%02X",
      paddr[0],paddr[1],paddr[2],paddr[3],paddr[4],paddr[5]);
    }

#if (VENDORFILTER)
  } else {
    // Very noisy
    // ESP_LOGD(TAG, "Filtered MAC %02X:%02X:%02X:%02X:%02X:%02X",
    // paddr[0],paddr[1],paddr[2],paddr[3],paddr[5],paddr[5]);
  }
#endif

  // True if MAC WiFi/BLE was new
  return added; // function returns bool if a new and unique Wifi or BLE mac was
                // counted (true) or not (false)
}
