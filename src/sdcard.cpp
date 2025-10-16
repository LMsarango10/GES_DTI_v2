// routines for writing data to an SD-card, if present
#define HAS_SDCARD 1
#if (HAS_SDCARD)

// Local logging tag
static const char TAG[] = __FILE__;

#include "sdcard.h"
#include "esp_system.h"
#if __has_include("esp_mac.h")
  #include "esp_mac.h"
#endif
extern "C" {
  #include "mbedtls/aes.h"
  #include "mbedtls/sha256.h"
}
static bool useSDCard;

static void createFile(void);
void createTempFile(FileMySD &tempFileSDCard, char* filename);
void replaceCurrentFile(char* newFilename);

FileMySD fileSDCard;
int fileIndex;

// seccion agragada para guardar con nivel de cifrado
// ====== Cripto: parámetros y utilidades ======
static const uint32_t NB_MAGIC  = 0x4E424331; // "NBC1"
static const size_t   NB_IV_LEN = 16;
static const size_t   NB_TAG_LEN= 32;         // SHA-256
static const size_t   NB_KEY_LEN= 32;         // AES-256

static void trng_fill(uint8_t* out, size_t n) {
  for (size_t i = 0; i < n; i += 4) {
    uint32_t r = esp_random();
    size_t c = (n - i) >= 4 ? 4 : (n - i);
    memcpy(out + i, &r, c);
  }
}

// Deriva una clave de 32B desde el eFuse/MAC del chip: key = SHA256("NBKDF"||MAC||"v1")
static void derive_key_from_chip(uint8_t key[NB_KEY_LEN]) {
  uint8_t mac6[6] = {0};
  esp_efuse_mac_get_default(mac6);
  const char* prefix = "NBKDF";
  const char* suffix = "v1";
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts_ret(&ctx, 0);
  mbedtls_sha256_update_ret(&ctx, (const unsigned char*)prefix, strlen(prefix));
  mbedtls_sha256_update_ret(&ctx, mac6, sizeof(mac6));
  mbedtls_sha256_update_ret(&ctx, (const unsigned char*)suffix, strlen(suffix));
  mbedtls_sha256_finish_ret(&ctx, key);
  mbedtls_sha256_free(&ctx);
}

// PKCS#7
static size_t pkcs7_pad(const uint8_t* in, size_t len, uint8_t* out, size_t cap) {
  const size_t blk = 16;
  size_t pad = blk - (len % blk);
  size_t need = len + pad;
  if (need > cap) return 0;
  memcpy(out, in, len);
  memset(out + len, (int)pad, pad);
  return need;
}
static size_t pkcs7_unpad(uint8_t* buf, size_t len) {
  if (!len) return 0;
  uint8_t pad = buf[len - 1];
  if (!pad || pad > 16 || pad > len) return 0;
  for (size_t i = 0; i < pad; ++i) if (buf[len - 1 - i] != pad) return 0;
  return len - pad;
}

static void sha256(const uint8_t* in, size_t len, uint8_t out[32]) {
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts_ret(&ctx, 0);
  mbedtls_sha256_update_ret(&ctx, in, len);
  mbedtls_sha256_finish_ret(&ctx, out);
  mbedtls_sha256_free(&ctx);
}

// Formato de archivo: [MAGIC(4)][IV(16)][LEN(4)][CIPHER(len)][TAG(32)]
static bool nb_encrypt_and_write(const char* path, const uint8_t* plain, size_t plen) {
  uint8_t key[NB_KEY_LEN]; derive_key_from_chip(key);

  const size_t blk = 16;
  size_t maxCipher = ((plen / blk) + 2) * blk;
  std::unique_ptr<uint8_t[]> padded(new uint8_t[maxCipher]);
  size_t encLen = pkcs7_pad(plain, plen, padded.get(), maxCipher);
  if (!encLen) return false;

  uint8_t iv[NB_IV_LEN]; trng_fill(iv, NB_IV_LEN);

  mbedtls_aes_context aes; mbedtls_aes_init(&aes);
  if (mbedtls_aes_setkey_enc(&aes, key, NB_KEY_LEN * 8) != 0) { mbedtls_aes_free(&aes); return false; }
  std::unique_ptr<uint8_t[]> cipher(new uint8_t[encLen]);
  uint8_t iv_cbc[NB_IV_LEN]; memcpy(iv_cbc, iv, NB_IV_LEN);
  if (mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, encLen, iv_cbc, padded.get(), cipher.get()) != 0) {
    mbedtls_aes_free(&aes); return false;
  }
  mbedtls_aes_free(&aes);

  std::unique_ptr<uint8_t[]> iv_cipher(new uint8_t[NB_IV_LEN + encLen]);
  memcpy(iv_cipher.get(), iv, NB_IV_LEN);
  memcpy(iv_cipher.get() + NB_IV_LEN, cipher.get(), encLen);
  uint8_t tag[NB_TAG_LEN]; sha256(iv_cipher.get(), NB_IV_LEN + encLen, tag);

  FileMySD f = mySD.open(path, FILE_WRITE);
  if (!f) return false;

  uint32_t magic = NB_MAGIC, clen = (uint32_t)encLen;
  bool ok = true;
  ok &= f.write((uint8_t*)&magic, sizeof(magic)) == sizeof(magic);
  ok &= f.write(iv, NB_IV_LEN) == NB_IV_LEN;
  ok &= f.write((uint8_t*)&clen, sizeof(clen)) == sizeof(clen);
  ok &= f.write(cipher.get(), encLen) == (int)encLen;
  ok &= f.write(tag, NB_TAG_LEN) == NB_TAG_LEN;
  f.flush(); f.close();
  return ok;
}

static bool nb_read_and_decrypt(const char* path, std::string& outJson) {
  FileMySD f = mySD.open(path, FILE_READ);
  if (!f) return false;

  uint32_t magic = 0, clen = 0;
  uint8_t iv[NB_IV_LEN], tag[NB_TAG_LEN];
  bool ok = true;
  ok &= f.read((uint8_t*)&magic, sizeof(magic)) == sizeof(magic);
  ok &= f.read(iv, NB_IV_LEN) == NB_IV_LEN;
  ok &= f.read((uint8_t*)&clen, sizeof(clen)) == sizeof(clen);
  if (!ok || magic != NB_MAGIC || clen == 0 || clen > 4096) { f.close(); return false; }

  std::unique_ptr<uint8_t[]> cipher(new uint8_t[clen]);
  ok &= f.read(cipher.get(), clen) == (int)clen;
  ok &= f.read(tag, NB_TAG_LEN) == NB_TAG_LEN;
  f.close();
  if (!ok) return false;

  std::unique_ptr<uint8_t[]> iv_cipher(new uint8_t[NB_IV_LEN + clen]);
  memcpy(iv_cipher.get(), iv, NB_IV_LEN);
  memcpy(iv_cipher.get() + NB_IV_LEN, cipher.get(), clen);
  uint8_t calc[NB_TAG_LEN]; sha256(iv_cipher.get(), NB_IV_LEN + clen, calc);
  if (memcmp(calc, tag, NB_TAG_LEN) != 0) return false;

  uint8_t key[NB_KEY_LEN]; derive_key_from_chip(key);
  mbedtls_aes_context aes; mbedtls_aes_init(&aes);
  if (mbedtls_aes_setkey_dec(&aes, key, NB_KEY_LEN * 8) != 0) { mbedtls_aes_free(&aes); return false; }
  std::unique_ptr<uint8_t[]> plain(new uint8_t[clen]);
  uint8_t iv_cbc[NB_IV_LEN]; memcpy(iv_cbc, iv, NB_IV_LEN);
  if (mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, clen, iv_cbc, cipher.get(), plain.get()) != 0) {
    mbedtls_aes_free(&aes); return false;
  }
  mbedtls_aes_free(&aes);

  size_t realLen = pkcs7_unpad(plain.get(), clen);
  if (!realLen) return false;

  outJson.assign((char*)plain.get(), (char*)plain.get() + realLen);
  return true;
}



//funcion modificada para evitar bloqueo si no hay SD
bool sdcardInit() {
  ESP_LOGD(TAG, "looking for SD-card...");
  useSDCard = mySD.begin(SDCARD_CS, SDCARD_MOSI, SDCARD_MISO, SDCARD_SCLK);

  if (!useSDCard) {
    ESP_LOGW(TAG, "⚠️ SD card not detected.");
    return false;   // <- evita bloqueo y no crea archivo
  }

  createFile();     // solo si se detecta
  return useSDCard;
}


bool createFile(std::string filename, FileMySD &file)
{
  if (!useSDCard)
    return false;

  char* path_c = (char*)filename.c_str();
  if (mySD.exists(path_c)) {
    mySD.remove(path_c);
  }
  file = mySD.open(filename.c_str(), FILE_WRITE);
  if (!fileSDCard) {
    ESP_LOGE(TAG, "Failed to open file for writing");
    return false;
  }
  return true;
}

bool deleteFile(std::string filename)
{
  if (!useSDCard)
    return false;

  char* path_c = (char*)filename.c_str();
  if (mySD.exists(path_c)) {
    mySD.remove(path_c);

  }
  return true;
}

bool openFile(std::string filename, FileMySD &file)
{
  if (!useSDCard)
    return false;
  char* filename_c = (char*)filename.c_str();
  if (!mySD.exists(filename_c))
  {
    return false;
  }
  file = mySD.open(filename.c_str());
  if (!fileSDCard) {
    ESP_LOGE(TAG, "Failed to open file for reading");
    return false;
  }
  return true;
}

bool createFolder(std::string path)
{
  if (!useSDCard)
    return false;
  char* path_c = (char*)path.c_str();
  return mySD.mkdir(path_c);
}

bool folderExists(std::string path)
{
  if (!useSDCard)
    return false;
  char* path_c = (char*)path.c_str();
  return mySD.exists(path_c);
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

void printFile(FileMySD file)
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

int sdReadLine(FileMySD file, int lineNumber, char* outBuffer)
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

int sdRemoveFirstLines(FileMySD file, int N)
{
  ESP_LOGV(TAG, "removing %d lines from sd file", N);
  char newFilename[16];
  int newFileIndex = fileIndex + 1;
  if(newFileIndex >= 100) newFileIndex = 0;
  sprintf(newFilename, SDCARD_FILE_NAME, newFileIndex);
  FileMySD tempFile;
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

void createTempFile(FileMySD &tempFileSDCard, char* filename) {
  bool fileExists = mySD.exists(filename);
  if (fileExists)
  {
    ESP_LOGV(TAG, "SD: temp file exists: removing");
    mySD.remove(filename);
  }

  ESP_LOGV(TAG, "SD: creating temp file");
  tempFileSDCard = mySD.open(filename, FILE_WRITE);
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
  bool fileExists = mySD.exists(bufferFilename);
  if (fileExists)
  {
    mySD.remove(bufferFilename);
  }
  ESP_LOGV(TAG, "SD: file does not exist: opening");
  fileSDCard = mySD.open(bufferFilename, FILE_WRITE);
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
  mySD.remove(bufferFilename);

  // load new file
  fileSDCard = mySD.open(newFilename, FILE_WRITE);
  if (fileSDCard) {
    ESP_LOGV(TAG, "SD: name opened: <%s>", newFilename);
    //fileSDCard.println(SDCARD_FILE_HEADER);
    useSDCard = true;
  }
}

void sdSaveNbConfig(ConfigBuffer_t *config){
  char path[] = "nb.cnf";
  if (mySD.exists(path)) {
      mySD.remove(path);
  }

  StaticJsonDocument<512> doc;
  doc["serverAddress"]  = config->ServerAddress;
  doc["serverUsername"] = config->ServerUsername;
  doc["serverPassword"] = config->ServerPassword;
  doc["applicationId"]  = config->ApplicationId;
  doc["applicationName"]= config->ApplicationName;
  doc["gatewayId"]      = config->GatewayId;
  doc["port"]           = config->port;

  char buff[512];
  size_t n = serializeJson(doc, buff, sizeof(buff));
  if (!n) {
    ESP_LOGE(TAG, "nb.cnf: serializeJson failed");
    return;
  }
  if (!nb_encrypt_and_write(path, (const uint8_t*)buff, n)) {
    ESP_LOGE(TAG, "nb.cnf: encrypted save failed");
    return;
  }
  ESP_LOGI(TAG, "🔐 nb.cnf encrypted and saved.");
}


void saveDefaultNbConfig() {
  ConfigBuffer_t conf;
  conf.ServerAddress[0] = 0;
  conf.ServerPassword[0] = 0;
  conf.ServerUsername[0] = 0;
  conf.ApplicationId[0] = 0;
  conf.ApplicationName[0] = 0;
  conf.port = 0;
  conf.GatewayId[0] = 0;

  strcat(conf.ServerAddress, DEFAULT_URL);
  strcat(conf.ServerUsername, DEFAULT_USERNAME);
  strcat(conf.ServerPassword, DEFAULT_PASS);
  strcat(conf.ApplicationId, DEFAULT_APPID);
  strcat(conf.ApplicationName, DEFAULT_APPNAME);
  conf.port = DEFAULT_PORT;
  strcat(conf.GatewayId, DEFAULT_GATEWAY_ID);
  sdSaveNbConfig(&conf);
}

int sdLoadNbConfig(ConfigBuffer_t *config){
  if (!useSDCard) {
    ESP_LOGW(TAG, "SD card not detected, cannot load nb.cnf");
    return -10;
  }

char path[] = "nb.cnf";
  if (!mySD.exists(path)) {
    ESP_LOGI(TAG, "nb.cnf not found, creating default (encrypted)...");
    saveDefaultNbConfig(); // crea cifrado con DEFAULT_*
  }

  std::string json;
  if (!nb_read_and_decrypt(path, json)) {
    ESP_LOGE(TAG, "nb.cnf: decrypt/verify failed, recreating defaults...");
    deleteFile(path);
    saveDefaultNbConfig();
    if (!nb_read_and_decrypt(path, json)) {
      ESP_LOGE(TAG, "nb.cnf: decrypt failed after regeneration");
      return -13;
    }
  }

  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, json);
  if (err) {
    ESP_LOGE(TAG, "nb.cnf: JSON parse error after decrypt: %s", err.c_str());
    deleteFile(path);
    return -14;
  }

  const char* serverAddress   = doc["serverAddress"];
  const char* serverPassword  = doc["serverPassword"];
  const char* serverUsername  = doc["serverUsername"];
  const char* applicationId   = doc["applicationId"];
  const char* applicationName = doc["applicationName"];
  const char* gatewayId       = doc["gatewayId"];
  config->port                = doc["port"] | 0;

  if (!serverAddress || !serverPassword || !serverUsername ||
      !applicationId || !applicationName || !gatewayId) {
    deleteFile(path);
    return -15;
  }

  strncpy(config->ServerAddress,   serverAddress,   sizeof(config->ServerAddress));
  strncpy(config->ServerUsername,  serverUsername,  sizeof(config->ServerUsername));
  strncpy(config->ServerPassword,  serverPassword,  sizeof(config->ServerPassword));
  strncpy(config->ApplicationId,   applicationId,   sizeof(config->ApplicationId));
  strncpy(config->ApplicationName, applicationName, sizeof(config->ApplicationName));
  strncpy(config->GatewayId,       gatewayId,       sizeof(config->GatewayId));

  ESP_LOGI(TAG, "✅ nb.cnf loaded (encrypted at rest).");
  return 0;
}


bool isSDCardAvailable() {
  return useSDCard;
}


#endif // (HAS_SDCARD)