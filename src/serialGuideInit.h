#include "globals.h"
#ifndef __SERIAL_GUIDE
#define __SERIAL_GUIDE


void convert(const char *s);
u1_t convertChar(char s);
void checkConfig();
void saveInitialConfig();
bool readConfig();
#endif