#ifndef UPDATES_H
#define UPDATES_H

#define DEST_FS_USES_SPIFFS
#include <Arduino.h>
#include <globals.h>
#include <sdcard.h>
#include <CRC32.h>
//#include <Update.h>
#include <SPIFFS.h>
#include <ESP32-targz.h>

#define UPDATE_FOLDER "update"
#define MAX_DOWNLOAD_RETRIES 3


bool checkUpdateFile(char * filename, uint32_t crc);
bool downloadUpdates(std::string index);
bool updateFromFS(void);
#endif