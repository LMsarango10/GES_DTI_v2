#include <Arduino.h>
#include <globals.h>
#include <sdcard.h>
#include <CRC32.h>
#include <Update.h>

#define UPDATE_FOLDER "/update"


bool checkUpdateFile(char * filename, uint32_t crc);
bool downloadUpdates(std::string index);
bool updateFromFS(void);