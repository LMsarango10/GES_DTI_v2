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

  File file;
  if (!createFile(filename, file)) {
    ESP_LOGE(TAG, "Failed to create %s file", filename);
    return false;
  }
  file.write(buff, size);
  file.close();
  return true;
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

  std::string fileName = std::string(filename);
  File updateFile;
  if (!openFile(fileName, updateFile)) {
    ESP_LOGE(TAG, "Failed to open file: %s", fileName.c_str());
    return false;
  }

  char buff[2048];
  size_t res = updateFile.readBytes(buff, sizeof(buff));
  size_t fileSize = updateFile.size();
  updateFile.close();

  if (res <= 0) {
    ESP_LOGE(TAG, "Failed to read file (empty): %s", fileName.c_str());
    return false;
  }

  CRC32 crcFile;
  crcFile.reset();

  ESP_LOGD(TAG, "File size: %d", fileSize);
  // Here we add each byte to the checksum, caclulating the checksum as we go.
  for (size_t i = 0; i < fileSize; i++) {
    crcFile.update(buff[i]);
  }

  // Once we have added all of the data, generate the final CRC32 checksum.
  uint32_t checksum = crcFile.finalize();

  if (checksum != crc) {
    ESP_LOGD(TAG, "Update file CRC mismatch, FILE: %08x, EXPECTED CRC: %08x",
             checksum, crc);
    return false;
  }
  return true;
}

bool unifyUpdates(int parts) {
  char finalFilename[20];
  sprintf(finalFilename, "%s/final.bin", UPDATE_FOLDER);

  File finalFile;
  if (!createFile(finalFilename, finalFile)) {
    ESP_LOGE(TAG, "Failed to create final file");
    return false;
  }
  for (int i = 1; i <= parts; i++) {
    File file;
    char filename[20];
    sprintf(filename, "%s/%d.bin", UPDATE_FOLDER, i);

    if (!openFile(filename, file)) {
      ESP_LOGE(TAG, "Failed to open file number: %d", i);
      return false;
    }
    ESP_LOGD(TAG, "Reading file number: %d", i);
    while (file.available()) {
      finalFile.write(file.read());
    }
    file.close();
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
        ESP_LOGD(TAG, "CRC: %08x", crc);
      }
      return true;
    }
  }
  return false;
}

bool downloadFile(int i, uint32_t crc) {
  char filename[20];
  sprintf(filename, "%d.bin", i);
  ESP_LOGD(TAG, "Downloading %s", filename);

  char checksum[20];
  sprintf(checksum, "%d.sum", i);
  ESP_LOGD(TAG, "Downloading %s", checksum);

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
}

bool downloadUpdates(std::string index) {
  std::size_t found = index.find("\r\n");
  if (found != std::string::npos) {
    std::string version = index.substr(0, found);
    ESP_LOGD(TAG, "Latest Version: %s", version.c_str());

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

    for (int i = 1; i <= parts; i++) {
      if (!downloadFile(i, crcBuffer[i - 1])) {
        ESP_LOGE(TAG, "Failed to download file number: %d", i);
        delete crcBuffer;
        return false;
      }
    }
    delete crcBuffer;
    return unifyUpdates(parts);
  }
}

bool performUpdate(Stream &updateSource, size_t updateSize) {
  if (Update.begin(updateSize)) {
    size_t written = Update.writeStream(updateSource);
    if (written == updateSize) {
      ESP_LOGD(TAG, "Written : %d bytes successfully", written);
    } else {
      ESP_LOGD(TAG, "Written only : %d/%d. Retry?", written, updateSize);
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
}

// check given FS for valid update.bin and perform update if available
bool updateFromFS() {
  bool result = false;
  char finalFilename[20];
  sprintf(finalFilename, "%s/final.bin", UPDATE_FOLDER);
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
  }
  return result;
}