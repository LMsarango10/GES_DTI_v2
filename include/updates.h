#include <Arduino.h>
#include <globals.h>
#include <sdcard.h>
#include <CRC32.h>

#define UPDATE_FOLDER "/update"


bool checkUpdateFile(char * filename, uint32_t crc);
bool downloadUpdates(std::string index);