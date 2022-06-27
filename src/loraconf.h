#if(HAS_LORA)

/************************************************************
 * LMIC LoRaWAN configuration
 *
 * Read the values from TTN console (or whatever applies), insert them here,
 * and rename this file to src/loraconf.h
 *
 * Note that DEVEUI, APPEUI and APPKEY should all be specified in MSB format.
 * (This is different from standard LMIC-Arduino which expects DEVEUI and APPEUI
 * in LSB format.)

 * Set your DEVEUI here, if you have one. You can leave this untouched,
 * then the DEVEUI will be generated during runtime from device's MAC adress
 * and will be displayed on device's screen as well as on serial console.
 *
 * NOTE: Use MSB format (as displayed in TTN console, so you can cut & paste
 * from there)
 * For TTN, APPEUI in MSB format always starts with 0x70, 0xB3, 0xD5
 *
 * Note: If using a board with Microchip 24AA02E64 Uinique ID for deveui,
 * the DEVEUI will be overwriten by the one contained in the Microchip module
 *
 ************************************************************/

//static const u1_t DEVEUI[8] = {0x29, 0xe6, 0x22, 0xd5, 0x29, 0xe1, 0x79, 0x00};
static const u1_t DEVEUI[8] = {0x20, 0x79, 0xe1, 0x29, 0xd5, 0x22, 0xe1, 0x40};

static const u1_t APPEUI[8] = {0xDB, 0x16, 0x01, 0xD0, 0x7E, 0xD5, 0xB3, 0x70};

static const u1_t APPKEY[16] = {0x06, 0x26, 0x35, 0x77, 0xC3, 0xBB, 0xC9, 0xEB, 0x2F, 0xEF, 0x99, 0x13, 0x5E, 0xF0, 0xF6, 0xA5};

#endif // HAS_LORA