#ifndef _SDCARD_H
#define _SDCARD_H

#include <globals.h>
#include <stdio.h>

#include <SPI.h>
#include <mySD.h>

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
bool openFile(std::string filename, FileMySD &file);
bool createFolder(std::string path);
bool folderExists(std::string path);

#endif