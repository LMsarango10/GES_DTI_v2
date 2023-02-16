#include "serialGuideInit.h"
#include <EEPROM.h>
#include <lora_credentials.hpp>

uint8_t __DEVEUI[8] = {0x00, 0x00, 0x00, 0x00,
                       0x00, 0x00, 0x00, 0x00}; // 3079e129d522e14f

void convert(const char *s)
{
  for (int j = 0, i = 0; j < strlen(s); j++)
  {
    i = round((j - 0.01) / 2);
    if (j % 2 == 0 or j == 0)
    {
      __DEVEUI[i] = (uint8_t)__DEVEUI[i] + 16 * convertChar(s[j]);
    }
    else
    {
      __DEVEUI[i] = (uint8_t)__DEVEUI[i] + convertChar(s[j]);
    }
  }
  uint8_t temporal;
  for (int x = 0; x < 8 / 2; x++) {
    temporal = __DEVEUI[x];
    __DEVEUI[x] = __DEVEUI[8 - x - 1];
    __DEVEUI[8 - x - 1] = temporal;
  }
}
uint8_t convertChar(char s)
{
  switch (s)
  {
  case '0':
    return 0;
  case '1':
    return 1;
  case '2':
    return 2;
  case '3':
    return 3;
  case '4':
    return 4;
  case '5':
    return 5;
  case '6':
    return 6;
  case '7':
    return 7;
  case '8':
    return 8;
  case '9':
    return 9;
  case 'a':
    return 10;
  case 'b':
    return 11;
  case 'c':
    return 12;
  case 'd':
    return 13;
  case 'e':
    return 14;
  case 'f':
    return 15;
  case 'A':
    return 10;
  case 'B':
    return 11;
  case 'C':
    return 12;
  case 'D':
    return 13;
  case 'E':
    return 14;
  case 'F':
    return 15;

  default:
    Serial.print("NOT VALID HEX");
    break;
  }
}
#define DEBUG_DEVEUI 
void initConfigSerialGuide(String devEui,uint8_t actualConfig)
{
  convert(devEui.c_str());
#ifdef DEBUG_DEVEUI
  Serial.println(devEui);
  Serial.println("SIZEOF");
  Serial.println(sizeof(devEui));
  Serial.print(__DEVEUI[0], HEX);
  Serial.print(__DEVEUI[1], HEX);
  Serial.print(__DEVEUI[2], HEX);
  Serial.print(__DEVEUI[3], HEX);
  Serial.print(__DEVEUI[4], HEX);
  Serial.print(__DEVEUI[5], HEX);
  Serial.print(__DEVEUI[6], HEX);
  Serial.println(__DEVEUI[7], HEX);

  Serial.println("Nueva DevEUI Añadida: ");

  for (int i = 0; i < 8; i++)
  {
    DEVEUI_DEF[i] = __DEVEUI[i];
    Serial.print(DEVEUI_DEF[i], HEX);
  }

  Serial.print("\n");
  Serial.print("\n");
#endif
  // memcpy(DEVEUI_DEF, __DEVEUI, 8);
  uint32_t eepAddr = 0;
  // config
  EEPROM.put(eepAddr, actualConfig);
  eepAddr += sizeof(actualConfig);

  // Deveui
  Serial.printf("Setting DEVEUI to eeprom: ");
  for (int i = 0; i < 8; i++)
  {
    EEPROM.put(eepAddr, DEVEUI_DEF[i]);
    eepAddr += sizeof(DEVEUI_DEF[i]);
    Serial.print(DEVEUI_DEF[i], HEX);
  }
  Serial.printf("\n      * Ok\n");
  EEPROM.commit();
  printf("Saved config eeprom\n");
  // saveInitialConfig();
}

void saveDeveuiEeprom(){

}