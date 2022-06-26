// routines for writing data to an SD-card, if present
#define HAS_SDCARD 1
#if (HAS_SDCARD)

// Local logging tag
static const char TAG[] = __FILE__;

#include "sdcard.h"

static bool useSDCard;

static void createFile(void);
void createTempFile(File &tempFileSDCard, char* filename);
void replaceCurrentFile(char* newFilename);

File fileSDCard;
int fileIndex;

bool sdcardInit() {
  ESP_LOGD(TAG, "looking for SD-card...");
  useSDCard = SD.begin(SDCARD_CS, SDCARD_MOSI, SDCARD_MISO, SDCARD_SCLK);
  if (useSDCard)
    createFile();
  return useSDCard;
}

void sdcardWriteData(uint16_t noWifi, uint16_t noBle) {
  static int counterWrites = 0;
  char tempBuffer[12 + 1];
  time_t t = now();

  if (!useSDCard)
    return;

  sprintf(tempBuffer, "%02d.%02d.%4d,", day(t), month(t), year(t));
  fileSDCard.print(tempBuffer);
  sprintf(tempBuffer, "%02d:%02d:%02d,", hour(t), minute(t), second(t));
  fileSDCard.print(tempBuffer);
  sprintf(tempBuffer, "%d,%d", noWifi, noBle);
  fileSDCard.println(tempBuffer);

  if (++counterWrites > 2) {
    // force writing to SD-card
    ESP_LOGV(TAG, "flushing data to card");
    fileSDCard.flush();
    counterWrites = 0;
  }
}

void printFile(File file)
{
  ESP_LOGD(TAG, "Print file contents");
  ESP_LOGD(TAG, "------START-----");
  file.seek(0);
  while(fileSDCard.available())
  {
    char tempString[128];
    file.readBytesUntil('\n', tempString, 128);
    ESP_LOGD(TAG, "%s", tempString);
  }
  Serial.println();
  ESP_LOGD(TAG, "-------END------");
}

void printSdFile()
{
  printFile(fileSDCard);
}

void sdcardWriteFrame(MessageBuffer_t *message) {
  static int counterWrites = 0;
  char tempBuffer[128];

  if (!useSDCard)
    return;

  int sz = message->MessageSize;
  int i = 1;
  sprintf(tempBuffer, "%02X", message->MessagePort);
  ESP_LOGD(TAG, "PORT: %02X", message->MessagePort);
  for (; i < sz; i++)
  {
    sprintf(tempBuffer + 2*i, "%02X", message->Message[i-1]);
  }

  ESP_LOGD(TAG, "writing BUFFER (lenght = %d) to SD-card: %s", i, tempBuffer);
  fileSDCard.println(tempBuffer);

  /*if (++counterWrites > 2) {
    // force writing to SD-card
    ESP_LOGD(TAG, "flushing data to card");
    counterWrites = 0;
  }*/
  fileSDCard.flush();
  //printFile(fileSDCard);
}

int sdReadLine(File file, int lineNumber, char* outBuffer)
{
  file.seek(0);
  int recNum = 0;
  while(file.available())
  {
    int lineLen = file.readBytesUntil('\r',outBuffer,128);
    recNum++; // Count the record

    if(recNum == lineNumber && lineLen > 0)
    {
      outBuffer[lineLen] = 0;
      return lineLen;
    }
  }
  return 0;
}

int sdRemoveFirstLines(File file, int N)
{
  ESP_LOGV(TAG, "removing %d lines from sd file", N);
  char newFilename[16];
  int newFileIndex = fileIndex + 1;
  if(newFileIndex >= 100) newFileIndex = 0;
  sprintf(newFilename, SDCARD_FILE_NAME, newFileIndex);
  File tempFile;
  createTempFile(tempFile, newFilename);
  char outBuffer[128];

  // get position for second line
  file.seek(0);
  for ( int i =0; i < N && file.available(); i++)
  {
    int len = file.readBytesUntil('\n',outBuffer,128);
    outBuffer[len] = 0;
    ESP_LOGV(TAG, "remove line: %s", outBuffer);
  }
  // set start position for both files
  tempFile.seek(0);
  uint8_t buf[64];
  int n;
  while ((n = file.read(buf, sizeof(buf))) > 0) {
    tempFile.write(buf, n);
  }
  tempFile.flush();
  replaceCurrentFile(newFilename);
  fileIndex = newFileIndex;
}

int parseFrame(char* inFrame, uint8_t* outFrame, int len)
{
  char tempPort[2];
  tempPort[0] = inFrame[0];
  tempPort[1] = 0;
  int port = strtoul(tempPort, nullptr, 16);
  for(int i = 1; i < len; i+=2)
  {
    char tempBuff[3];
    tempBuff[0] = inFrame[i];
    tempBuff[1] = inFrame[i+1];
    tempBuff[2] = 0;
    uint8_t value = strtoul(tempBuff, nullptr, 16);
    outFrame[(i-1)/2] = value;
  }
  return port;
}

void sdRemoveFirstLines(int N)
{
  sdRemoveFirstLines(fileSDCard, N);
}

int sdcardReadFrame(MessageBuffer_t *message, int N)
{
  if (!useSDCard)
    return -1;

  char tempBuffer[128];
  int lineLen = sdReadLine(fileSDCard, N, tempBuffer);
  if (lineLen <= 0) return -1;
  ESP_LOGV(TAG, "read line : %s", tempBuffer);
  uint8_t frameBuffer[64];
  int port = parseFrame(tempBuffer, frameBuffer, lineLen);
  message->MessageSize = lineLen/2;
  memcpy(message->Message, frameBuffer, message->MessageSize);
  message->MessagePort = port;
  return 0;
}

void createTempFile(File &tempFileSDCard, char* filename) {
  bool fileExists = SD.exists(filename);
  if (fileExists)
  {
    ESP_LOGV(TAG, "SD: temp file exists: removing");
    SD.remove(filename);
  }

  ESP_LOGV(TAG, "SD: creating temp file");
  tempFileSDCard = SD.open(filename, FILE_WRITE);
  if (!tempFileSDCard) {
    ESP_LOGW(TAG, "SD: could not create temp sd file %s", filename);
  }
}

void createFile(void) {
  char bufferFilename[8 + 1 + 3 + 1];

  useSDCard = false;

  sprintf(bufferFilename, SDCARD_FILE_NAME, 0);
  fileIndex = 0;
  ESP_LOGV(TAG, "SD: looking for file <%s>", bufferFilename);
  bool fileExists = SD.exists(bufferFilename);
  if (fileExists)
  {
    SD.remove(bufferFilename);
  }
  ESP_LOGV(TAG, "SD: file does not exist: opening");
  fileSDCard = SD.open(bufferFilename, FILE_WRITE);
  if (fileSDCard) {
    ESP_LOGV(TAG, "SD: name opended: <%s>", bufferFilename);
    //fileSDCard.println(SDCARD_FILE_HEADER);
    useSDCard = true;
  }
}

void replaceCurrentFile(char* newFilename)
{
  char bufferFilename[8 + 1 + 3 + 1];

  // remove previous file
  sprintf(bufferFilename, SDCARD_FILE_NAME, fileIndex);
  fileSDCard.close();
  ESP_LOGV(TAG, "SD: removing file: %s", bufferFilename);
  SD.remove(bufferFilename);

  // load new file
  fileSDCard = SD.open(newFilename, FILE_WRITE);
  if (fileSDCard) {
    ESP_LOGV(TAG, "SD: name opened: <%s>", newFilename);
    //fileSDCard.println(SDCARD_FILE_HEADER);
    useSDCard = true;
  }
}

void sdSaveNbConfig(ConfigBuffer_t *config){
  if(SD.exists("nb.cnf")) {
    SD.remove("nb.cnf");
  }
  File f = SD.open("nb.cnf", FILE_WRITE);
  const size_t capacity = JSON_OBJECT_SIZE(32);
  DynamicJsonDocument doc(capacity);

  doc["serverAddress"] = config->ServerAddress;
  doc["serverPassword"] = config->ServerPassword;
  doc["applicationId"] = config->ApplicationId;
  doc["applicationName"] = config->ApplicationName;
  doc["gatewayId"] = config->GatewayId;
  doc["port"] = config->port;

  serializeJson(doc, f);
  f.flush();
  f.close();
}

void saveDefaultNbConfig() {
  ConfigBuffer_t conf;
  conf.ServerAddress[0] = 0;
  conf.ServerPassword[0] = 0;
  conf.ApplicationId[0] = 0;
  conf.ApplicationName[0] = 0;
  conf.port = 0;
  conf.GatewayId[0] = 0;

  strcat(conf.ServerAddress, "gesinen.es");
  strcat(conf.ServerPassword, "gesinen2110");
  strcat(conf.ApplicationId, "1");
  strcat(conf.ApplicationName, "app");
  conf.port = 1882;
  strcat(conf.GatewayId, "TESTGATE");
  sdSaveNbConfig(&conf);
}

int sdLoadNbConfig(ConfigBuffer_t *config){
  // Use this to return to default SD.remove("nb.cnf");
  if(!SD.exists("nb.cnf")) {
    ESP_LOGI(TAG, "nb.cnf file does not exists, creating");
    saveDefaultNbConfig();
  }

  File f = SD.open("nb.cnf", FILE_READ);
  const size_t capacity = JSON_OBJECT_SIZE(12) + 512;
  StaticJsonDocument<512> doc;

  char buff[512];
  int i = 0;
  while(f.available()) {
    buff[i++] = f.read();
  }
  buff[i] = 0;

  ESP_LOGD(TAG, "%d Bytes read", i);

  if(i == 0) {
    ESP_LOGE(TAG, "FILE EMPTY");
    return -1;
  }
  ESP_LOGD(TAG, "File content: %s", buff);
  DeserializationError err = deserializeJson(doc, buff);
  if(err) {
    ESP_LOGI(TAG, "deserialization error: %s", err.c_str());
    return -2;
  }

  f.close();

  const char* serverAddress = doc["serverAddress"]; // "12345678912345678912345678912345678912345678"
  const char* serverPassword = doc["serverPassword"];
  config->port = doc["port"];

  const char* applicationId = doc["applicationId"];
  const char* applicationName = doc["applicationName"];
  const char* gatewayId = doc["gatewayId"];

  int serverAddressLen = strlen(serverAddress);
  strncpy(config->ServerAddress, serverAddress, serverAddressLen);
  config->ServerAddress[serverAddressLen] = '\0';

  int serverPasswordLen = strlen(serverPassword);
  strncpy(config->ServerPassword, serverPassword, serverPasswordLen);
  config->ServerPassword[serverPasswordLen] = '\0';

  int applicationIdLen = strlen(applicationId);
  strncpy(config->ApplicationId, applicationId, applicationIdLen);
  config->ApplicationId[applicationIdLen] = '\0';

  int applicationNameLen = strlen(applicationName);
  strncpy(config->ApplicationName, applicationName, applicationNameLen);
  config->ApplicationName[applicationNameLen] = '\0';

  int gatewayIdLen = strlen(gatewayId);
  strncpy(config->GatewayId, gatewayId, gatewayIdLen);
  config->GatewayId[gatewayIdLen] = '\0';

  return 0;
}

#endif // (HAS_SDCARD)