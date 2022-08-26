#include <updates.h>

static const char TAG[] = __FILE__;

bool savePartUpdateFile(int fileNumber, char *buff, int size) {
  if (!folderExists(UPDATE_FOLDER)) {
    if (!createFolder(UPDATE_FOLDER)) {
      ESP_LOGE(TAG, "Failed to create %s folder", UPDATE_FOLDER);
      return false;
    }
  }

  char filename[20];
  sprintf(filename, "%s/%d.bin", UPDATE_FOLDER, fileNumber);

  FileMySD file;
  if (!createFile(filename, file)) {
    ESP_LOGE(TAG, "Failed to create %s file", filename);
    return false;
  }
  file.write((uint8_t*)buff, size);
  file.close();
  return true;
}

bool generateChecksumFile(int fileNumber)
{
  char filename[20];
  sprintf(filename, "%s/%d.bin", UPDATE_FOLDER, fileNumber);

  char checksumFilename[20];
  sprintf(checksumFilename, "%s/%d.sum", UPDATE_FOLDER, fileNumber);

  std::string fileName = std::string(filename);
  FileMySD updateFile;
  if (!openFile(fileName, updateFile)) {
    ESP_LOGE(TAG, "Failed to open file: %s", fileName.c_str());
    return false;
  }

  CRC32 crcFile;
  crcFile.reset();

  char buff[2048];
  size_t res = updateFile.readBytes(buff, sizeof(buff));
  size_t fileSize = updateFile.size();
  updateFile.close();

  if (res <= 0) {
    ESP_LOGE(TAG, "Failed to read file (empty): %s", fileName.c_str());
    return false;
  }

  ESP_LOGV(TAG, "File size: %d", fileSize);
  // Here we add each byte to the checksum, caclulating the checksum as we go.
  for (size_t i = 0; i < fileSize; i++) {
    crcFile.update(buff[i]);
  }

  // Once we have added all of the data, generate the final CRC32 checksum.
  uint32_t checksum = crcFile.finalize();

  ESP_LOGV(TAG, "calculated checksum for file %s is %08x", filename, checksum);

  std::string checksumFilenameString = std::string(checksumFilename);
  FileMySD checksumFile;
  if (!createFile(checksumFilenameString, checksumFile)) {
    ESP_LOGE(TAG, "Failed to create file: %s", checksumFilenameString.c_str());
    return false;
  }
  checksumFile.write((uint8_t*)&checksum, sizeof(checksum));
  checksumFile.close();

  return true;
}

bool getChecksumFromFile(int fileNumber, uint32_t* checksum)
{
  char filename[20];
  sprintf(filename, "%s/%d.sum", UPDATE_FOLDER, fileNumber);
  std::string fileName = std::string(filename);
  FileMySD checksumFile;
  if (!openFile(fileName, checksumFile)) {
    ESP_LOGE(TAG, "Failed to open file: %s", fileName.c_str());
    return false;
  }
  checksumFile.read((uint8_t*)checksum, sizeof(checksum));
  checksumFile.close();
  ESP_LOGV(TAG, "Value read from checksum file: %08X", *checksum);
  return true;
}

bool deleteChecksumFile(int fileNumber)
{
  char filename[20];
  sprintf(filename, "%s/%d.sum", UPDATE_FOLDER, fileNumber);
  std::string fileName = std::string(filename);

  return deleteFile(fileName);
}

bool checkUpdateFile(int fileNumber, uint32_t crc) {
  ESP_LOGV(TAG, "Checking update file");

  if (!folderExists(UPDATE_FOLDER)) {
    if (!createFolder(UPDATE_FOLDER)) {
      ESP_LOGE(TAG, "Failed to create %s folder", UPDATE_FOLDER);
      return false;
    }
  }
  char filename[20];
  sprintf(filename, "%s/%d.bin", UPDATE_FOLDER, fileNumber);

  uint32_t checksum = 0;
  if (!getChecksumFromFile(fileNumber, &checksum)) {
    ESP_LOGV(TAG, "Failed to get checksum from file, calculating");
    if(!generateChecksumFile(fileNumber)) {
      ESP_LOGE(TAG, "Failed to generate checksum file");
      return false;
    }
    if (!getChecksumFromFile(fileNumber, &checksum)) {
      ESP_LOGE(TAG, "Failed to get checksum from generated file");
      return false;
    }
  }

  if (checksum != crc) {
    ESP_LOGW(TAG, "Update file CRC mismatch, FILE: %08x, EXPECTED CRC: %08x",
             checksum, crc);

    deleteChecksumFile(fileNumber);
    return false;
  }

  return true;
}

bool unifyUpdates(int parts) {
  char finalFilename[20];
  sprintf(finalFilename, "%s/final.gz", UPDATE_FOLDER);

  FileMySD finalFile;
  if (!createFile(finalFilename, finalFile)) {
    ESP_LOGE(TAG, "Failed to create final file");
    return false;
  }
  uint8_t buff[2048];

  for (int i = 1; i <= parts; i++) {
    int cursor = 0;
    FileMySD file;
    char filename[20];
    sprintf(filename, "%s/%d.bin", UPDATE_FOLDER, i);

    if (!openFile(filename, file)) {
      ESP_LOGE(TAG, "Failed to open file number: %d", i);
      return false;
    }
    ESP_LOGD(TAG, "Reading file number: %d", i);
    uint32_t filesize = file.size();
    file.read(buff, filesize);
    file.close();

    finalFile.write(buff, filesize);
  }
  finalFile.close();
  return true;
}

bool downloadChecksumFile(int i, uint32_t *crcBuffer, int bufferSize,
                          int checksumsPerFile) {
  char filename[20];
  sprintf(filename, "%d.chk", i);
  ESP_LOGD(TAG, "Downloading %s", filename);

  char buff[2048];
  int responseSize = 0;

  if (getData(UPDATES_SERVER_IP, UPDATES_SERVER_PORT, filename, buff,
              sizeof(buff), &responseSize) >= 0) {
    if (responseSize > 0) {
      for (int i = 0; i < checksumsPerFile; i++) {
        if (i * 4 >= responseSize) {
          break;
        }
        uint32_t crc = *((uint32_t *)&buff[i * sizeof(uint32_t)]);
        if (i >= bufferSize) {
          ESP_LOGE(TAG, "Buffer overflow");
          return false;
        }
        crcBuffer[i] = crc;
        //ESP_LOGD(TAG, "CRC: %08x", crc);
      }
      return true;
    }
  }
  return false;
}

bool downloadFile(int i, uint32_t crc) {
  char filename[20];
  sprintf(filename, "%d.bin", i);
  ESP_LOGI(TAG, "Downloading %s", filename);

  char buff[2048];
  int responseSize = 0;
  if (getData(UPDATES_SERVER_IP, UPDATES_SERVER_PORT, filename, buff,
              sizeof(buff), &responseSize) >= 0) {
    if (responseSize > 0) {
      if (!savePartUpdateFile(i, buff, responseSize)) {
        ESP_LOGE(TAG, "Failed to save file number: %d", i);
        return false;
      }
    }
  }
  if (!checkUpdateFile(i, crc)) {
    ESP_LOGE(TAG, "Checksum fail for file: %d", i);
    return false;
  }
  ESP_LOGI(TAG, "File number: %d downloaded", i);
  return true;
}

bool removeUpdateFiles(std::string index)
{
  std::size_t found = index.find("\r\n");
  if (found != std::string::npos) {
    std::string version = index.substr(0, found);

    const char *partsStr = index.substr(found + 2).c_str();
    char *end;
    long parts = strtol(partsStr, &end, 10);

    if (end == partsStr || errno == ERANGE) {
      ESP_LOGE(TAG, "Invalid number of parts: %s", partsStr);
      return false;
    }
    ESP_LOGD(TAG, "Number of parts to remove: %d", parts);
    for (int i = 1; i <= parts; i++) {
      ESP_LOGD(TAG, "Removing part: %d/%d", i, parts);
      char filename[20];
      sprintf(filename, "%s/%d.bin", UPDATE_FOLDER, i);
      ESP_LOGV(TAG, "Removing file: %s", filename);
      if (!deleteFile(filename)) {
        ESP_LOGE(TAG, "Failed to remove file: %s", filename);
        return false;
      }
      char checksumFilename[20];
      sprintf(checksumFilename, "%s/%d.sum", UPDATE_FOLDER, i);
      ESP_LOGV(TAG, "Removing file: %s", checksumFilename);
      if (!deleteFile(checksumFilename)) {
        ESP_LOGE(TAG, "Failed to remove file: %s", checksumFilename);
        return false;
      }
    }
  }
  return true;
}

bool downloadUpdates(std::string index) {
  unsigned long startTime = millis();
  std::size_t found = index.find("\r\n");
  if (found != std::string::npos) {
    std::string version = index.substr(0, found);
    ESP_LOGI(TAG, "Latest Version: %s", version.c_str());

    const char *partsStr = index.substr(found + 2).c_str();
    char *end;
    long parts = strtol(partsStr, &end, 10);

    if (end == partsStr || errno == ERANGE) {
      ESP_LOGE(TAG, "Invalid number of parts: %s", partsStr);
      return false;
    }

    char *checksumsPerFileStr = new char[strlen(end) + 1];
    strcpy(checksumsPerFileStr, end);
    long checksumsPerFile = strtol(checksumsPerFileStr, &end, 10);

    if (end == checksumsPerFileStr || errno == ERANGE) {
      ESP_LOGE(TAG, "Invalid number of checksums per part: %s",
               checksumsPerFileStr);
      return false;
    }

    ESP_LOGD(TAG, "Number of parts: %d", parts);
    ESP_LOGD(TAG, "Number of checksums per file: %d", checksumsPerFile);

    uint32_t *crcBuffer = new uint32_t[parts];
    for (int i = 1; i <= parts; i += checksumsPerFile) {
      uint32_t *tempBuffer = new uint32_t[checksumsPerFile];
      downloadChecksumFile(i, tempBuffer, checksumsPerFile, checksumsPerFile);
      for (int j = 0; j < checksumsPerFile; j++) {
        if (i - 1 + j >= parts) {
          break;
        }
        crcBuffer[i - 1 + j] = tempBuffer[j];
      }
      delete tempBuffer;
    }

    int retries = 0;
    for (int i = 1; i <= parts; i++) {
        ESP_LOGI(TAG, "Downloading part: %d/%d", i, parts);
      retries = 0;
      while (retries < MAX_DOWNLOAD_RETRIES) {
        if (checkUpdateFile(i, crcBuffer[i - 1])) {
            ESP_LOGI(TAG, "File %d already downloaded, skipping", i);
            break;
        }
        if (!downloadFile(i, crcBuffer[i - 1])) {
          ESP_LOGE(TAG, "Failed to download file number: %d", i);
          retries += 1;
        }
        else {
          break;
        }
        ESP_LOGW(TAG, "Retrying download file number: %d", i);
      }
      if (retries == MAX_DOWNLOAD_RETRIES) {
        ESP_LOGE(TAG, "Failed to download file number: %d", i);
        delete crcBuffer;
        return false;
      }
      if (startTime + MAX_DOWNLOAD_TIME < millis()) {
        ESP_LOGI(TAG, "Download time exceeded, continuing normal operation and retrying");
        delete crcBuffer;
        return false;
      }
    }
    delete crcBuffer;
    return unifyUpdates(parts);
  }
}

/*bool performUpdate(Stream &updateSource, size_t updateSize) {
  if (Update.begin(updateSize)) {
    size_t written = Update.writeStream(updateSource);
    if (written == updateSize) {
      ESP_LOGI(TAG, "Written : %d bytes successfully", written);
    } else {
      ESP_LOGW(TAG, "Written only : %d/%d. Retry?", written, updateSize);
    }
    if (Update.end()) {
      ESP_LOGI(TAG, "OTA done!");
      if (Update.isFinished()) {
        ESP_LOGI(TAG, "Update successfully completed. Rebooting.");
        return true;
      } else {
        ESP_LOGE(TAG, "Update not finished? Something went wrong!");
      }
    } else {
      ESP_LOGE(TAG, "Error Occurred. Error #: %d", Update.getError());
    }

  } else {
    ESP_LOGW(TAG, "Not enough space to begin OTA");
  }
  return false;
}*/

bool uncompressFileAndFlash(std::string filename) {

  FileMySD inputFile;
  if (!openFile(filename, inputFile))
  {
    ESP_LOGE(TAG, "Failed to open file: %s", filename.c_str());
    return false;
  }

  I2C_MUTEX_LOCK();
  tarGzFS.begin();
  GzUnpacker *GZUnpacker = new GzUnpacker();

  GZUnpacker->haltOnError( true ); // stop on fail (manual restart/reset required)
  GZUnpacker->setupFSCallbacks( targzTotalBytesFn, targzFreeBytesFn ); // prevent the partition from exploding, recommended
  GZUnpacker->setGzProgressCallback( BaseUnpacker::defaultProgressCallback ); // targzNullProgressCallback or defaultProgressCallback
  GZUnpacker->setLoggerCallback( BaseUnpacker::targzPrintLoggerCallback  );    // gz log verbosity

  if( !GZUnpacker->gzStreamUpdater( (Stream *)&inputFile, UPDATE_SIZE_UNKNOWN ) ) {
    Serial.printf("gzStreamUpdater failed with return code #%d\n", GZUnpacker->tarGzGetError() );
    I2C_MUTEX_UNLOCK();
    return false;
  }

  I2C_MUTEX_UNLOCK();
  return true;
}


// check given FS for valid update.bin and perform update if available
bool updateFromFS() {
  bool result = false;

  char compressedFilename[20];
  sprintf(compressedFilename, "%s/final.gz", UPDATE_FOLDER);

  if(!uncompressFileAndFlash(compressedFilename))
  {
    ESP_LOGE(TAG, "Failed to uncompress file and flash");
    return false;
  }

  /*
  File updateBin;
  if (!openFile(finalFilename, updateBin)) {
    ESP_LOGE(TAG, "Failed to open final update file");
    return false;
  }
  if (updateBin) {
    if (updateBin.isDirectory()) {
      ESP_LOGW(TAG, "Error, update.bin is not a file");
      updateBin.close();
      return false;
    }

    size_t updateSize = updateBin.size();

    ESP_LOGD(TAG, "Update file: %s", updateBin.readString().c_str());

    if (updateSize > 0) {
      ESP_LOGD(TAG, "Try to start update");
      result = performUpdate(updateBin, updateSize);
    } else {
      ESP_LOGW(TAG, "Error, file is empty");
    }

    updateBin.close();
  } else {
    ESP_LOGW(TAG, "Could not load update.bin from sd root");
  }*/
  return true;
}