// routines for writing data to an SD-card, if present
#define HAS_SDCARD 1
#if (HAS_SDCARD)

#include "sdcard.h"
#include "esp_system.h"

#if __has_include("esp_mac.h")
#include "esp_mac.h"
#endif

// FreeRTOS
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

// C libs for crypto
extern "C" {
#include "mbedtls/aes.h"
#include "mbedtls/sha256.h"
}

// For AES-GCM line encryption utilities
extern "C" {
#include "mbedtls/gcm.h"
#include "mbedtls/base64.h"
}

// Local logging tag
static const char TAG[] = "sdcard";

// ----------------------- Globals -----------------------
static bool useSDCard = false;
FileMySD fileSDCard; // global active log file handle

// Log rotation
#define MAX_LOG_FILE_SIZE (1ULL * 1024 * 1024 * 1024) // 1 GB
static int currentFileIndex = 0;
int fileIndex = 0;

// ----------------------- Helpers forward -----------------------
static void createFile(void);
static void checkAndRotateLogFile(void);

// =======================================================
//                   NB.CNF ENCRYPTION
// =======================================================

static const uint32_t NB_MAGIC = 0x4E424331; // "NBC1"
static const size_t NB_IV_LEN  = 16;
static const size_t NB_TAG_LEN = 32; // SHA-256
static const size_t NB_KEY_LEN = 32; // AES-256

static void trng_fill(uint8_t* out, size_t n) {
  for (size_t i = 0; i < n; i += 4) {
    uint32_t r = esp_random();
    size_t c = (n - i) >= 4 ? 4 : (n - i);
    memcpy(out + i, &r, c);
  }
}

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
  for (size_t i = 0; i < pad; ++i)
    if (buf[len - 1 - i] != pad) return 0;
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

static bool nb_encrypt_and_write(const char* path, const uint8_t* plain, size_t plen) {
  uint8_t key[NB_KEY_LEN];
  derive_key_from_chip(key);

  const size_t blk = 16;
  size_t maxCipher = ((plen / blk) + 2) * blk;
  std::unique_ptr<uint8_t[]> padded(new uint8_t[maxCipher]);
  size_t encLen = pkcs7_pad(plain, plen, padded.get(), maxCipher);
  if (!encLen) return false;

  uint8_t iv[NB_IV_LEN];
  trng_fill(iv, NB_IV_LEN);

  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  if (mbedtls_aes_setkey_enc(&aes, key, NB_KEY_LEN * 8) != 0) {
    mbedtls_aes_free(&aes);
    return false;
  }

  std::unique_ptr<uint8_t[]> cipher(new uint8_t[encLen]);
  uint8_t iv_cbc[NB_IV_LEN];
  memcpy(iv_cbc, iv, NB_IV_LEN);

  if (mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, encLen, iv_cbc,
                            padded.get(), cipher.get()) != 0) {
    mbedtls_aes_free(&aes);
    return false;
  }
  mbedtls_aes_free(&aes);

  std::unique_ptr<uint8_t[]> iv_cipher(new uint8_t[NB_IV_LEN + encLen]);
  memcpy(iv_cipher.get(), iv, NB_IV_LEN);
  memcpy(iv_cipher.get() + NB_IV_LEN, cipher.get(), encLen);

  uint8_t tag[NB_TAG_LEN];
  sha256(iv_cipher.get(), NB_IV_LEN + encLen, tag);

  // CORREGIDO: Casting explícito a (char*) para mySD.open
  FileMySD f = mySD.open((char*)path, FILE_WRITE); 
  if (!f) return false;

  uint32_t magic = NB_MAGIC, clen = (uint32_t)encLen;
  bool ok = true;
  ok &= f.write((uint8_t*)&magic, sizeof(magic)) == sizeof(magic);
  ok &= f.write(iv, NB_IV_LEN) == (int)NB_IV_LEN;
  ok &= f.write((uint8_t*)&clen, sizeof(clen)) == sizeof(clen);
  ok &= f.write(cipher.get(), encLen) == (int)encLen;
  ok &= f.write(tag, NB_TAG_LEN) == (int)NB_TAG_LEN;
  f.flush();
  f.close();

  if (ok) {
    ESP_LOGI("SD_CNF", "🔐 Configuración NB-IoT guardada y encriptada en '%s' (%d bytes)", path, clen);
  } else {
    ESP_LOGE("SD_CNF", "❌ Error escribiendo configuración en SD");
  }

  return ok;
}

static bool nb_read_and_decrypt(const char* path, std::string& outJson) {
  FileMySD f = mySD.open((char*)path, FILE_READ);
  if (!f) return false;

  uint32_t magic = 0, clen = 0;
  uint8_t iv[NB_IV_LEN], tag[NB_TAG_LEN];

  bool ok = true;
  ok &= f.read((uint8_t*)&magic, sizeof(magic)) == (int)sizeof(magic);
  ok &= f.read(iv, NB_IV_LEN) == (int)NB_IV_LEN;
  ok &= f.read((uint8_t*)&clen, sizeof(clen)) == (int)sizeof(clen);

  if (!ok || magic != NB_MAGIC || clen == 0 || clen > 4096) {
    f.close();
    return false;
  }

  std::unique_ptr<uint8_t[]> cipher(new uint8_t[clen]);
  ok &= f.read(cipher.get(), clen) == (int)clen;
  ok &= f.read(tag, NB_TAG_LEN) == (int)NB_TAG_LEN;
  f.close();
  if (!ok) return false;

  std::unique_ptr<uint8_t[]> iv_cipher(new uint8_t[NB_IV_LEN + clen]);
  memcpy(iv_cipher.get(), iv, NB_IV_LEN);
  memcpy(iv_cipher.get() + NB_IV_LEN, cipher.get(), clen);

  uint8_t calc[NB_TAG_LEN];
  sha256(iv_cipher.get(), NB_IV_LEN + clen, calc);
  if (memcmp(calc, tag, NB_TAG_LEN) != 0) return false;

  uint8_t key[NB_KEY_LEN];
  derive_key_from_chip(key);

  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  if (mbedtls_aes_setkey_dec(&aes, key, NB_KEY_LEN * 8) != 0) {
    mbedtls_aes_free(&aes);
    return false;
  }

  std::unique_ptr<uint8_t[]> plain(new uint8_t[clen]);
  uint8_t iv_cbc[NB_IV_LEN];
  memcpy(iv_cbc, iv, NB_IV_LEN);

  if (mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, clen, iv_cbc,
                            cipher.get(), plain.get()) != 0) {
    mbedtls_aes_free(&aes);
    return false;
  }
  mbedtls_aes_free(&aes);

  size_t realLen = pkcs7_unpad(plain.get(), clen);
  if (!realLen) return false;

  outJson.assign((char*)plain.get(), (char*)plain.get() + realLen);
  return true;
}

// =======================================================
//             SD PERSISTENT FIFO QUEUE: paxqueue.q
// =======================================================

// IMPORTANT: define as char[] to satisfy mySD APIs requiring char*
static char PAXQUEUE_FILE[] = "/paxqueue.q"; // Changed to absolute path just in case
static char PAXQUEUE_TMP[]  = "/paxqueue.tmp";

static SemaphoreHandle_t sdqMutex = NULL;

static const uint32_t PAXQ_MAGIC = 0x31515850; // 'P''X''Q''1' little-endian
static const uint8_t  PAXQ_VER   = 1;

#pragma pack(push, 1)
struct PaxQHeader {
  uint32_t magic;     // PAXQ_MAGIC
  uint8_t  version;   // PAXQ_VER
  uint8_t  reserved[3];
  uint32_t head;      // offset of first record
  uint32_t tail;      // offset to append
  uint32_t count;     // number of records
  uint16_t hdrCrc;    // CRC16 of header except hdrCrc/pad
  uint16_t pad;
};
#pragma pack(pop)

#pragma pack(push, 1)
struct PaxQRecHdr {
  uint16_t len;
  uint8_t  port;
  uint8_t  prio;
  uint32_t ts;
  uint16_t crc;  // CRC16 over (len,port,prio,ts,payload)
};
#pragma pack(pop)

static uint16_t crc16_ccitt(const uint8_t *data, size_t len, uint16_t crc = 0xFFFF) {
  while (len--) {
    crc ^= (uint16_t)(*data++) << 8;
    for (int i = 0; i < 8; i++) {
      if (crc & 0x8000) crc = (crc << 1) ^ 0x1021;
      else crc <<= 1;
    }
  }
  return crc;
}

static bool sdq_lock() {
  if (!sdqMutex) sdqMutex = xSemaphoreCreateMutex();
  return sdqMutex && (xSemaphoreTake(sdqMutex, pdMS_TO_TICKS(2000)) == pdTRUE);
}
static void sdq_unlock() {
  if (sdqMutex) xSemaphoreGive(sdqMutex);
}

static uint16_t header_crc(const PaxQHeader &h) {
  return crc16_ccitt((const uint8_t*)&h, sizeof(PaxQHeader) - sizeof(uint16_t) - sizeof(uint16_t));
}

static bool readHeader(FileMySD &f, PaxQHeader &h) {
  f.seek(0);
  if (f.read((uint8_t*)&h, sizeof(h)) != (int)sizeof(h)) return false;
  if (h.magic != PAXQ_MAGIC || h.version != PAXQ_VER) return false;
  return (header_crc(h) == h.hdrCrc);
}

static bool writeHeader(FileMySD &f, PaxQHeader &h) {
  h.magic = PAXQ_MAGIC;
  h.version = PAXQ_VER;
  h.hdrCrc = header_crc(h);
  f.seek(0);
  if (f.write((uint8_t*)&h, sizeof(h)) != (int)sizeof(h)) return false;
  f.flush();
  return true;
}

// Manual "rename": copy tmp -> dst, remove tmp
static bool copyFileAndRemove(char* srcPath, char* dstPath) {
  FileMySD src = mySD.open(srcPath, FILE_READ);
  if (!src) return false;

  // Remove destination if exists
  if (mySD.exists(dstPath)) mySD.remove(dstPath);

  FileMySD dst = mySD.open(dstPath, FILE_WRITE);
  if (!dst) { src.close(); return false; }

  uint8_t buf[256];
  int n;
  while ((n = src.read(buf, sizeof(buf))) > 0) {
    dst.write(buf, n);
  }
  dst.flush();
  dst.close();
  src.close();

  mySD.remove(srcPath);
  return true;
}

// Compact queue: rewrite remaining [head..tail) into new file
static bool sdq_compact_locked() {
  if (!useSDCard) return false;
  if (!mySD.exists(PAXQUEUE_FILE)) return true;

  FileMySD in = mySD.open(PAXQUEUE_FILE, FILE_READ);
  if (!in) return false;

  PaxQHeader h;
  if (!readHeader(in, h)) { in.close(); return false; }

  if (h.count == 0) {
    in.close();
    mySD.remove(PAXQUEUE_FILE);
    FileMySD nf = mySD.open(PAXQUEUE_FILE, FILE_WRITE);
    if (!nf) return false;
    PaxQHeader nh{};
    nh.head  = sizeof(PaxQHeader);
    nh.tail  = sizeof(PaxQHeader);
    nh.count = 0;
    bool ok = writeHeader(nf, nh);
    nf.close();
    return ok;
  }

  if (mySD.exists(PAXQUEUE_TMP)) mySD.remove(PAXQUEUE_TMP);
  FileMySD out = mySD.open(PAXQUEUE_TMP, FILE_WRITE);
  if (!out) { in.close(); return false; }

  PaxQHeader nh{};
  nh.head  = sizeof(PaxQHeader);
  nh.tail  = sizeof(PaxQHeader);
  nh.count = h.count;

  if (!writeHeader(out, nh)) { in.close(); out.close(); return false; }

  // Copy payload region
  const size_t BUFSZ = 256;
  uint8_t buf[BUFSZ];

  uint32_t remaining = (h.tail > h.head) ? (h.tail - h.head) : 0;
  in.seek(h.head);

  while (remaining > 0) {
    size_t want = (remaining > BUFSZ) ? BUFSZ : remaining;
    int r = in.read(buf, want);
    if (r <= 0) break;
    out.write(buf, r);
    nh.tail += r;
    remaining -= r;
  }

  // Rewrite header with new tail
  writeHeader(out, nh);

  in.close();
  out.flush();
  out.close();

  // Replace old with tmp (manual rename)
  mySD.remove(PAXQUEUE_FILE);
  return copyFileAndRemove(PAXQUEUE_TMP, PAXQUEUE_FILE);
}

bool sdqueueInit() {
  if (!useSDCard) return false;
  if (!sdqMutex) sdqMutex = xSemaphoreCreateMutex();
  if (!sdqMutex) return false;

  if (!mySD.exists(PAXQUEUE_FILE)) {
    FileMySD f = mySD.open(PAXQUEUE_FILE, FILE_WRITE);
    if (!f) return false;
    PaxQHeader h{};
    h.head  = sizeof(PaxQHeader);
    h.tail  = sizeof(PaxQHeader);
    h.count = 0;
    bool ok = writeHeader(f, h);
    f.close();
    return ok;
  }

  FileMySD f = mySD.open(PAXQUEUE_FILE, FILE_READ);
  if (!f) return false;

  PaxQHeader h;
  bool ok = readHeader(f, h);
  f.close();

  if (!ok) {
    ESP_LOGW(TAG, "paxqueue.q corrupted -> rebuilding");
    mySD.remove(PAXQUEUE_FILE);
    return sdqueueInit();
  }
  return true;
}

uint32_t sdqueueCount() {
  if (!useSDCard) return 0;
  if (!sdq_lock()) return 0;

  FileMySD f = mySD.open(PAXQUEUE_FILE, FILE_READ);
  if (!f) { sdq_unlock(); return 0; }

  PaxQHeader h;
  bool ok = readHeader(f, h);
  f.close();
  sdq_unlock();
  return ok ? h.count : 0;
}

static bool sdq_read_record_at(FileMySD &f, uint32_t offset, MessageBuffer_t *msg, uint32_t &nextOffset) {
  PaxQRecHdr rh{};
  f.seek(offset);
  if (f.read((uint8_t*)&rh, sizeof(rh)) != (int)sizeof(rh)) return false;

  const size_t MSGCAP = sizeof(msg->Message);
  if (rh.len == 0 || rh.len > MSGCAP) return false;

  uint8_t payload[MSGCAP];
  if (f.read(payload, rh.len) != (int)rh.len) return false;

  uint16_t crc = 0xFFFF;
  crc = crc16_ccitt((uint8_t*)&rh, sizeof(PaxQRecHdr) - sizeof(uint16_t), crc);
  crc = crc16_ccitt(payload, rh.len, crc);
  if (crc != rh.crc) return false;

  msg->MessageSize = rh.len;
  msg->MessagePort = rh.port;
  msg->MessagePrio = (sendprio_t)rh.prio;
  memcpy(msg->Message, payload, rh.len);

  nextOffset = offset + sizeof(PaxQRecHdr) + rh.len;
  return true;
}

bool sdqueueDequeue(MessageBuffer_t *msg) {
  if (!useSDCard || !msg) return false;
  if (!sdq_lock()) return false;

  FileMySD f = mySD.open(PAXQUEUE_FILE, FILE_READ);
  if (!f) { sdq_unlock(); return false; }

  PaxQHeader h;
  if (!readHeader(f, h)) { f.close(); sdq_unlock(); return false; }

  if (h.count == 0) { f.close(); sdq_unlock(); return false; }

  uint32_t nextOffset = 0;
  bool ok = sdq_read_record_at(f, h.head, msg, nextOffset);
  f.close();

  if (ok) {
    h.head = nextOffset;
    h.count--;
    
    // Reopen for write header update
    f = mySD.open(PAXQUEUE_FILE, FILE_WRITE); // r+ simulation
    if (f) {
      writeHeader(f, h);
      f.close();
      
      // LOG DE SALIDA (Para ver cuando vacía la cola)
      ESP_LOGI("SD_QUEUE", "🚀 Recuperado de SD y enviado. Pendientes: %d", h.count);
    }

    if (h.count == 0 || h.head > 128000) {
      sdq_compact_locked();
    }
  }

  sdq_unlock();
  return ok;
}


// =================================================================================
// sdqueueEnqueue: escribe un MessageBuffer_t en paxqueue.q usando PaxQHeader/PaxQRecHdr
// =================================================================================
bool sdqueueEnqueue(MessageBuffer_t *message) {
#ifdef HAS_SDCARD
    if (!useSDCard || !message)
        return false;

    // Mutex para acceso concurrente
    if (!sdq_lock())
        return false;

    // 1. Leer cabecera actual
    FileMySD f = mySD.open(PAXQUEUE_FILE, FILE_READ);
    PaxQHeader h{};
    bool ok = f && readHeader(f, h);
    if (f) f.close();

    if (!ok) {
        // Si hay corrupción, reconstruimos la cola
        ESP_LOGW(TAG, "paxqueue.q corrupta en enqueue -> rebuilding");
        mySD.remove(PAXQUEUE_FILE);
        if (!sdqueueInit()) {
            sdq_unlock();
            return false;
        }
        // Reabrimos ya inicializada
        f = mySD.open(PAXQUEUE_FILE, FILE_READ);
        if (!f || !readHeader(f, h)) {
            if (f) f.close();
            sdq_unlock();
            return false;
        }
        f.close();
    }

    // 2. Abrir para escritura (modo "r+")
    FileMySD wf = mySD.open(PAXQUEUE_FILE, FILE_WRITE);
    if (!wf) {
        sdq_unlock();
        return false;
    }

    // Ir al final lógico (tail)
    wf.seek(h.tail);

    // 3. Construir cabecera de registro
    PaxQRecHdr rh{};
    rh.len  = message->MessageSize;
    rh.port = message->MessagePort;
    rh.prio = (uint8_t)message->MessagePrio;
    rh.ts   = (uint32_t)now();  // timestamp actual (si no quieres usarlo, puedes poner 0)

    // CRC sobre (len,port,prio,ts,payload)
    uint16_t crc = 0xFFFF;
    crc = crc16_ccitt((const uint8_t *)&rh, sizeof(PaxQRecHdr) - sizeof(uint16_t), crc);
    crc = crc16_ccitt(message->Message, rh.len, crc);
    rh.crc = crc;

    // 4. Escribir registro: header + payload
    bool okWrite = true;
    okWrite &= wf.write((uint8_t *)&rh, sizeof(rh)) == (int)sizeof(rh);
    okWrite &= wf.write(message->Message, rh.len)      == (int)rh.len;

    if (!okWrite) {
        wf.close();
        sdq_unlock();
        ESP_LOGE("SD_QUEUE", "⚠️ Error escribiendo registro en paxqueue.q");
        return false;
    }

    // 5. Actualizar cabecera en RAM
    h.tail += sizeof(PaxQRecHdr) + rh.len;
    h.count++;

    // 6. Reescribir cabecera en el fichero
    okWrite = writeHeader(wf, h);
    wf.flush();
    wf.close();
    sdq_unlock();

    if (okWrite) {
        ESP_LOGI("SD_QUEUE",
                 "📦 Paquete salvado en cola SD (port %u, %u bytes, count=%u)",
                 message->MessagePort, message->MessageSize, h.count);
        return true;
    } else {
        ESP_LOGE("SD_QUEUE", "⚠️ Error actualizando cabecera paxqueue.q");
        return false;
    }
#else
    return false;
#endif
}

bool sdqueuePeek(MessageBuffer_t *msg) {
  if (!useSDCard) return false;
  if (!sdq_lock()) return false;

  FileMySD f = mySD.open(PAXQUEUE_FILE, FILE_READ);
  if (!f) { sdq_unlock(); return false; }

  PaxQHeader h;
  bool ok = readHeader(f, h);
  if (!ok || h.count == 0) { f.close(); sdq_unlock(); return false; }

  uint32_t nextOffset = 0;
  ok = sdq_read_record_at(f, h.head, msg, nextOffset);

  f.close();
  sdq_unlock();
  return ok;
}


// Optional wrapper for old calls: "write frame to SD"
void sdcardWriteFrame(MessageBuffer_t *message) {
  if (!useSDCard) return;
  sdqueueEnqueue(message);
}

// =======================================================
//                 SD BASE FUNCTIONS (LOG)
// =======================================================

// Mount + open log file paxcount.xx
bool sdcardInit() {
  ESP_LOGI("SD", "🔍 Checking SD-card status...");

  if (useSDCard) {
    ESP_LOGI("SD", "Unmounting previous SD instance...");
    if (fileSDCard) {
      fileSDCard.flush();
      fileSDCard.close();
    }
    useSDCard = false;
    delay(100);
  }

  useSDCard = mySD.begin(SDCARD_CS, SDCARD_MOSI, SDCARD_MISO, SDCARD_SCLK);
  if (!useSDCard) {
    ESP_LOGW("SD", "⚠️ SD card not detected or failed to initialize.");
    return false;
  }

  // Detect last log index
  for (int i = 0; i < 100; i++) {
    char testname[16];
    sprintf(testname, SDCARD_FILE_NAME, i);
    if (!mySD.exists(testname)) {
      currentFileIndex = (i == 0) ? 0 : i - 1;
      break;
    }
  }

  ESP_LOGI("SD", "📂 Current log index set to %d", currentFileIndex);

  // Open or create current log
  char filename[16];
  sprintf(filename, SDCARD_FILE_NAME, currentFileIndex);

  if (mySD.exists(filename)) {
    ESP_LOGI("SD", "📂 Existing log file found: %s", filename);
    fileSDCard = mySD.open(filename, FILE_WRITE);
    fileSDCard.seek(fileSDCard.size());
  } else {
    ESP_LOGI("SD", "🆕 Creating new log file: %s", filename);
    fileSDCard = mySD.open(filename, FILE_WRITE);
    if (fileSDCard) {
      fileSDCard.println(SDCARD_FILE_HEADER);
      fileSDCard.flush();
    }
  }

  if (!fileSDCard) {
    ESP_LOGE("SD", "❌ Failed to open file on SD.");
    useSDCard = false;
    return false;
  }

  // Init persistent queue (paxqueue.q)
  sdqueueInit();

  // Start flusher
  sdqueueStartFlusher();

  ESP_LOGI("SD", "✅ SD-card initialized and ready for logging + persistent queue.");
  return true;
}

bool isSDCardAvailable() { return useSDCard; }

// PLAIN log line write
void sdcardWriteLine(const char *line) {
  if (!useSDCard) return;
  if (!fileSDCard) {
    ESP_LOGW("SD", "File closed, recreating...");
    createFile();
  }
  checkAndRotateLogFile();
  fileSDCard.println(line);
  fileSDCard.flush();
}

void sdcardWriteData(uint16_t noWifi, uint16_t noBle) {
  if (!useSDCard) return;

  if (!fileSDCard) createFile();

  // Construcción del string CSV
  String dataLine = String(noWifi) + "," + String(noBle);
  
  #if (defined BAT_MEASURE_ADC || defined HAS_PMU)
    float voltage = read_voltage() / 1000.0;
    dataLine += "," + String(voltage, 2);
  #endif

  fileSDCard.println(dataLine);
  
  ESP_LOGI("SD_CSV", "📝 Dato escrito en SD: WiFi=%d BLE=%d", noWifi, noBle);

  static int writeCntr = 0;
  if (++writeCntr > 5) {
    fileSDCard.flush();
    writeCntr = 0;
  }
}

// =======================================================
//             NB CONFIG SAVE/LOAD (SD independent)
// =======================================================

void sdSaveNbConfig(ConfigBuffer_t *config) {
  char path[] = "nb.cnf";

  if (mySD.exists(path)) mySD.remove(path);

  StaticJsonDocument<512> doc;
  doc["serverAddress"]   = config->ServerAddress;
  doc["serverUsername"]  = config->ServerUsername;
  doc["serverPassword"]  = config->ServerPassword;
  doc["applicationId"]   = config->ApplicationId;
  doc["applicationName"] = config->ApplicationName;
  doc["gatewayId"]       = config->GatewayId;
  doc["port"]            = config->port;

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

  ESP_LOGI(TAG, "🔒 nb.cnf encrypted and saved.");
}

static void saveDefaultNbConfig() {
  ConfigBuffer_t conf;
  memset(&conf, 0, sizeof(conf));

  strncpy(conf.ServerAddress,   DEFAULT_URL,      sizeof(conf.ServerAddress) - 1);
  strncpy(conf.ServerUsername,  DEFAULT_USERNAME, sizeof(conf.ServerUsername) - 1);
  strncpy(conf.ServerPassword,  DEFAULT_PASS,     sizeof(conf.ServerPassword) - 1);
  strncpy(conf.ApplicationId,   DEFAULT_APPID,    sizeof(conf.ApplicationId) - 1);
  strncpy(conf.ApplicationName, DEFAULT_APPNAME,  sizeof(conf.ApplicationName) - 1);
  strncpy(conf.GatewayId,       DEFAULT_GATEWAY_ID, sizeof(conf.GatewayId) - 1);
  conf.port = DEFAULT_PORT;

  sdSaveNbConfig(&conf);
}

int sdLoadNbConfig(ConfigBuffer_t *config) {
  if (!useSDCard) {
    ESP_LOGW(TAG, "SD not detected -> using DEFAULT NB config (SD independent mode).");
    memset(config, 0, sizeof(ConfigBuffer_t));
    strncpy(config->ServerAddress,   DEFAULT_URL,        sizeof(config->ServerAddress) - 1);
    strncpy(config->ServerUsername,  DEFAULT_USERNAME,   sizeof(config->ServerUsername) - 1);
    strncpy(config->ServerPassword,  DEFAULT_PASS,       sizeof(config->ServerPassword) - 1);
    strncpy(config->ApplicationId,   DEFAULT_APPID,      sizeof(config->ApplicationId) - 1);
    strncpy(config->ApplicationName, DEFAULT_APPNAME,    sizeof(config->ApplicationName) - 1);
    strncpy(config->GatewayId,       DEFAULT_GATEWAY_ID, sizeof(config->GatewayId) - 1);
    config->port = DEFAULT_PORT;
    ESP_LOGI(TAG, "✅ NB defaults loaded: server=%s port=%d appId=%s gw=%s",
             config->ServerAddress, config->port, config->ApplicationId, config->GatewayId);
    return 0;
  }

  char path[] = "nb.cnf";
  if (!mySD.exists(path)) {
    ESP_LOGI(TAG, "nb.cnf not found, creating default (encrypted)...");
    saveDefaultNbConfig();
  }

  std::string json;
  if (!nb_read_and_decrypt(path, json)) {
    ESP_LOGE(TAG, "nb.cnf: decrypt/verify failed, recreating defaults...");
    mySD.remove(path);
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
    mySD.remove(path);
    return -14;
  }

  const char *serverAddress  = doc["serverAddress"];
  const char *serverPassword = doc["serverPassword"];
  const char *serverUsername = doc["serverUsername"];
  const char *applicationId  = doc["applicationId"];
  const char *applicationName= doc["applicationName"];
  const char *gatewayId      = doc["gatewayId"];
  config->port = doc["port"] | 0;

  if (!serverAddress || !serverPassword || !serverUsername ||
      !applicationId || !applicationName || !gatewayId) {
    mySD.remove(path);
    return -15;
  }

  strncpy(config->ServerAddress,   serverAddress,  sizeof(config->ServerAddress) - 1);
  strncpy(config->ServerUsername,  serverUsername, sizeof(config->ServerUsername) - 1);
  strncpy(config->ServerPassword,  serverPassword, sizeof(config->ServerPassword) - 1);
  strncpy(config->ApplicationId,   applicationId,  sizeof(config->ApplicationId) - 1);
  strncpy(config->ApplicationName, applicationName,sizeof(config->ApplicationName) - 1);
  strncpy(config->GatewayId,       gatewayId,      sizeof(config->GatewayId) - 1);

  ESP_LOGI(TAG, "✅ nb.cnf loaded (encrypted at rest).");
  return 0;
}

// =======================================================
//                 LOG FILE HELPERS
// =======================================================

static void createFile(void) {
  char bufferFilename[16];
  sprintf(bufferFilename, SDCARD_FILE_NAME, 0);
  fileIndex = 0;

  if (mySD.exists(bufferFilename)) mySD.remove(bufferFilename);

  fileSDCard = mySD.open(bufferFilename, FILE_WRITE);
  if (fileSDCard) {
    fileSDCard.println(SDCARD_FILE_HEADER);
    fileSDCard.flush();
    useSDCard = true;
  }
}

static void checkAndRotateLogFile(void) {
  if (!fileSDCard) return;
  
  size_t size = fileSDCard.size();
  if (size < MAX_LOG_FILE_SIZE) return;

  ESP_LOGW("SD_ROT", "🔄 Archivo lleno (%.2f MB). Rotando log...", size / (1024.0 * 1024.0));

  fileSDCard.flush();
  fileSDCard.close();
  
  currentFileIndex++;
  createFile(); 
}

// =======================================================
//                 SD QUEUE FLUSHER TASK
// =======================================================

static TaskHandle_t sdqFlusherTask = NULL;

#if (HAS_LORA)
extern bool check_queue_available();
extern bool lora_enqueuedata(MessageBuffer_t *message);
extern "C" {
  extern struct lmic_t LMIC;
}
#endif

#if (HAS_NBIOT)
extern void nb_enable(bool temporary);
extern bool nb_enqueuedata(MessageBuffer_t *message);
#endif

static void sdqueueFlusher(void *param) {
  (void)param;
  const int MAX_PER_CYCLE = 16; // era 8 

  ESP_LOGI("SD_FLUSH", "🔄 SD Queue Flusher task started");

  while (true) {
    if (!useSDCard) {
      vTaskDelay(pdMS_TO_TICKS(2000));
      continue;
    }

// DESPUÉS:
    uint32_t pending = sdqueueCount();
    if (pending == 0) {
      vTaskDelay(pdMS_TO_TICKS(2000));
      continue;
    }
    
    ESP_LOGI("SD_FLUSH", "📤 Starting flush cycle: %u messages pending", pending);

    for (int i = 0; i < MAX_PER_CYCLE; i++) {
      MessageBuffer_t msg;

      if (!sdqueuePeek(&msg)) break;

      bool delivered = false;

      #if (HAS_LORA)
            // Prefer LoRa if joined and queue has space
            if (LMIC.devaddr && check_queue_available()) {
              delivered = lora_enqueuedata(&msg);
            }
      #endif

      #if (HAS_NBIOT)
            // Use NB only if LoRa not available
            if (!delivered) {
              nb_enable(true);
              delivered = nb_enqueuedata(&msg);
            }
      #endif

      if (delivered) {
            MessageBuffer_t dumped;
            sdqueueDequeue(&dumped);
            
            // AÑADIR ESTE LOG:
            ESP_LOGI("SD_FLUSH", "✅ Message delivered from SD (port %u, %u bytes, %u remaining)", 
                    msg.MessagePort, msg.MessageSize, sdqueueCount());
            
            vTaskDelay(pdMS_TO_TICKS(20));
          } else {
            // AÑADIR ESTE LOG:
            ESP_LOGW("SD_FLUSH", "⏸️ Cannot deliver - queues full, will retry");
            break;
          }
    }

          uint32_t delay = (sdqueueCount() > 10) ? 100 : 300;
          vTaskDelay(pdMS_TO_TICKS(delay));
  }
}

void sdqueueStartFlusher() {
  if (sdqFlusherTask) return;
  xTaskCreatePinnedToCore(sdqueueFlusher, "sdqFlusher", 4096, NULL, 1, &sdqFlusherTask, 1);
}


// ==========================================================
//  COMPATIBILIDAD CON updates.cpp (NO BORRAR)
// ==========================================================

bool createFolder(std::string path) {
#ifdef HAS_SDCARD
    if (!useSDCard) return false;
    return mySD.mkdir((char*)path.c_str());
#else
    return false;
#endif
}

bool folderExists(std::string path) {
#ifdef HAS_SDCARD
    if (!useSDCard) return false;
    return mySD.exists((char*)path.c_str());
#else
    return false;
#endif
}

bool deleteFile(std::string path) {
#ifdef HAS_SDCARD
    if (!useSDCard) return false;
    if (mySD.exists((char*)path.c_str())) {
        return mySD.remove((char*)path.c_str());
    }
    return true;
#else
    return false;
#endif
}

bool createFile(std::string filename, FileMySD &file) {
#ifdef HAS_SDCARD
    if (!useSDCard) return false;

    // Remove previous if exists
    if (mySD.exists((char*)filename.c_str())) {
        mySD.remove((char*)filename.c_str());
    }

    file = mySD.open((char*)filename.c_str(), FILE_WRITE);
    return file ? true : false;
#else
    return false;
#endif
}

bool openFile(std::string filename, FileMySD &file) {
#ifdef HAS_SDCARD
    if (!useSDCard) return false;

    if (!mySD.exists((char*)filename.c_str()))
        return false;

    file = mySD.open((char*)filename.c_str(), FILE_READ);
    return file ? true : false;
#else
    return false;
#endif
}

#endif // HAS_SDCARD