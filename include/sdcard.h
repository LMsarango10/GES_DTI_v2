#ifndef _SDCARD_H
#define _SDCARD_H

#include <globals.h>
#include <stdio.h>

#include <SPI.h>
#include <mySD.h>

#define DEFAULT_GESINEN 1
//#define DEFAULT_DIPUTACION 1

#ifdef DEFAULT_GESINEN
#define DEFAULT_URL "gesinen.es"
#define DEFAULT_USERNAME "gesinen"
#define DEFAULT_PASS "gesinen2110"
#define DEFAULT_APPID "1"
#define DEFAULT_APPNAME "app"
#define DEFAULT_PORT 1882
#define DEFAULT_GATEWAY_ID "DIVALGATE"
#endif

#ifdef DEFAULT_DIPUTACION
#define DEFAULT_URL "connecta.dival.es"
#define DEFAULT_USERNAME "gesinen_app"
#define DEFAULT_PASS "gesinen2110_app"
#define DEFAULT_APPID "1"
#define DEFAULT_APPNAME "app"
#define DEFAULT_PORT 1883
#define DEFAULT_GATEWAY_ID "REMOTE"
#endif

#define SDCARD_FILE_NAME       "paxcount.%02d"
#define SDCARD_FILE_HEADER     "date, time, wifi, bluet"

bool sdcardInit( void );
void sdcardWriteData( uint16_t, uint16_t);
int sdcardReadFrame(MessageBuffer_t *message, int N);
void sdcardWriteFrame(MessageBuffer_t *message);
void sdRemoveFirstLines(int N);
void sdSaveNbConfig(ConfigBuffer_t *config);
int sdLoadNbConfig(ConfigBuffer_t *config);
void printSdFile();

bool createFile(std::string filename, FileMySD &file);
bool deleteFile(std::string filename);
bool openFile(std::string filename, FileMySD &file);
bool createFolder(std::string path);
bool folderExists(std::string path);

#endif