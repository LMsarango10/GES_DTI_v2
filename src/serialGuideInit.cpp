#include "serialGuideInit.h"
#include <EEPROM.h>

static const char TAG[] = __FILE__;

uint8_t __DEVEUI[8] = {0x00, 0x00, 0x00, 0x00,
                    0x00, 0x00, 0x00, 0x00}; // 3079e129d522e14f

void convert(const char *s) {
  for (int j = 0, i = 0; j < strlen(s); j++) {
    i = round((j - 0.01) / 2);
    if (j % 2 == 0 or j == 0) {
      __DEVEUI[i] = __DEVEUI[i] + 16 * convertChar(s[j]);
    } else {
      __DEVEUI[i] = __DEVEUI[i] + convertChar(s[j]);
    }
  }
}

uint8_t convertChar(char s) {
  switch (s) {
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

  default:
    Serial.print("NOT VALID HEX");
    break;
  }
}

void initEeprom() { EEPROM.begin(1024); }

void saveInitialConfig() {
  uint32_t eepAddr = 200;
  uint8_t configured = 7;

  EEPROM.put(eepAddr, configured);
  eepAddr += sizeof(configured);

  for (int i = 0; i < 8; i++) {
    EEPROM.put(eepAddr, DEVEUI[i]);
    eepAddr += sizeof(DEVEUI[i]);
  }

  EEPROM.commit();

  Serial.printf("Guardando la configuración en: %d\n", configured);
  Serial.printf("Guardado\n");
}

bool readConfig() {
  ESP_LOGI(TAG, "Leyendo la configuración existente");

  uint32_t eepAddr = 200;
  uint8_t configured = 0;

  EEPROM.get(eepAddr, configured);
  eepAddr += sizeof(configured);

  if (configured != 7) {
    printf("No existe Configuración Inicial... CONFIGURED: %d \n", configured);
    return false;
  }
  if (configured == 7) {
    printf("SI existe Configuración Inicial... CONFIGURED: %d \n", configured);
  }
  Serial.print("DEVEUI: ");

  for (int i = 0; i < 8; i++) {
    EEPROM.get(eepAddr, DEVEUI[i]);
    eepAddr += sizeof(DEVEUI[i]);
    Serial.print(DEVEUI[i], HEX);
  }
  Serial.println();

  return true;
}

void initConfig() {
  Serial.println("Bienvenido a la guía de STA\n");

  Serial.println("Introduzca la deveui: ");
  String devEui = "";
  char character;
  while (strlen(devEui.c_str()) < 16) {
    if (Serial.available()) {
      character = Serial.read();
      devEui.concat(character);
      devEui.trim();
    }
  }

  convert(devEui.c_str());
  Serial.print(__DEVEUI[0], HEX);
  Serial.print(__DEVEUI[1], HEX);
  Serial.print(__DEVEUI[2], HEX);
  Serial.print(__DEVEUI[3], HEX);
  Serial.print(__DEVEUI[4], HEX);
  Serial.print(__DEVEUI[5], HEX);
  Serial.print(__DEVEUI[6], HEX);
  Serial.println(__DEVEUI[7], HEX);

  Serial.println("Nueva DevEUI Añadida: ");
  for (int i = 0; i < 8; i++) {
    DEVEUI[i] = __DEVEUI[i];
    Serial.print(DEVEUI[i], HEX);
  }

  Serial.print("\n");
  Serial.print("\n");
  Serial.print("DEVEUI OK");
  Serial.print("\n");
  Serial.print("\n");
  saveInitialConfig();
}
void checkConfig() {
  Serial.print("APPKEY: ");

  for (int i = 0; i < 16; i++) {
    Serial.print(APPKEY[i], HEX);
  }
  Serial.println();
  initEeprom();
  // Si no hay configuración
  if (!readConfig()) {
    // Iniciar lectura de configuración
    initConfig();
  }
  // Si, si que hay configuración inicial
  else {
    Serial.print("Quieres resetear la configuración? Teclee Y = SI / N = NO "
                 "...( Tienes 10 segundos para contestar )\n");
    double lastMillis = 0;
    lastMillis = millis();
    while (millis() - lastMillis < 10000) {
      if (Serial.available()) {
        char character = Serial.read();
        if (character == 'Y') {
          EEPROM.put(200, 5);
          EEPROM.commit();
          Serial.print("Reiniciando...");
          ESP.restart();
        }
        if (character == 'N') {
          break;
        }
      }
    }
  }
  delay(2000);
}