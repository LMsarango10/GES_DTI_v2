#include "globals.h"
#ifndef __SERIAL_GUIDE
#define __SERIAL_GUIDE

uint8_t convertChar(char s);
void convert(const char *s);
uint8_t convertChar(char s);
void checkConfig();
void saveInitialConfig();
bool readConfig();
#endif