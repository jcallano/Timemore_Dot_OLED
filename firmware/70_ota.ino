// ============================================================================
// ACTUALIZACIÓN DEL FIRMWARE POR BLUETOOTH (BLE OTA)
// El aparato se anuncia como "TMDot-OTA" con un servicio propio y recibe el .bin desde la página web
// Web Bluetooth de docs/ota/ (GitHub Pages). Solo acepta la subida con el PIN de 6 cifras que muestra
// la pantalla en ese momento.
// Pestaña del sketch: el IDE la concatena tras firmware.ino (orden alfabético).
// Tipos, constantes y variables globales compartidas están en la pestaña principal.
//
// PROTOCOLO (versión 1). Enteros little-endian.
//   Control 0x...02 (write con respuesta + notify):
//     -> 01 START  [size u32][pin 6 ASCII]   <- 81 [estado][mtu u16][versión]
//     -> 03 END                              <- 83 [estado]          (estado 0 = OK -> reinicio)
//     -> 04 ABORT                            <- 84 00
//                                            <- 82 [escritos u32]   progreso tras cada bloque de 4 KB
//                                            <- EE [código][recibidos u32]  error
//     Estados de START: 0 OK, 1 PIN incorrecto, 2 tamaño no válido, 3 Update.begin() falló.
//     Errores EE: 1 desfase de offset, 2 fallo de escritura, 3 búfer lleno, 4 no es un firmware de
//     aplicación ESP32, 5 tiempo agotado, 6 recibido de más.
//   Datos 0x...03 (write sin respuesta): [offset u32][bytes]. El cliente no debe tener más de
//     OTA_WINDOW bytes enviados sin confirmar por un progreso 82.
// La escritura en flash se hace en este bucle (núcleo 1), nunca en la tarea de NimBLE: el callback
// solo copia los datos a un StreamBuffer.
// ============================================================================
#include <freertos/stream_buffer.h>
#include <esp_random.h>

static const char* OTA_SVC_UUID  = "c0de0001-7d2e-4b5a-9c1e-0a7e5d0f0a01";
static const char* OTA_CTRL_UUID = "c0de0002-7d2e-4b5a-9c1e-0a7e5d0f0a01";
static const char* OTA_DATA_UUID = "c0de0003-7d2e-4b5a-9c1e-0a7e5d0f0a01";
static const char* OTA_DEV_NAME  = "TMDot-OTA";
static constexpr uint8_t  OTA_PROTO_VERSION   = 1;
static constexpr size_t   OTA_STREAM_BYTES    = 16384;   // el cliente usa una ventana de 8 KB
static constexpr size_t   OTA_BLOCK_BYTES     = 4096;    // escritura y confirmación por bloques de 4 KB
static constexpr uint32_t OTA_IDLE_EXIT_MS    = 300000UL; // 5 min sin actividad -> salir
static constexpr uint32_t OTA_STALL_MS        = 20000UL;  // 20 s sin datos durante una subida -> abortar
static constexpr uint8_t  OTA_MAX_PIN_TRIES   = 5;

enum OtaCtrlCmd : uint8_t { OTA_CMD_START = 0x01, OTA_CMD_END = 0x03, OTA_CMD_ABORT = 0x04 };

static StreamBufferHandle_t otaStream   = nullptr;
static NimBLECharacteristic* otaCtrlChr = nullptr;
static char otaPin[7]                   = {0};

// Estado compartido entre los callbacks de NimBLE (núcleo 0) y el bucle OTA (núcleo 1)
static volatile bool     otaReceiving    = false;   // START aceptado y datos en curso
static volatile uint32_t otaImageSize    = 0;
static volatile uint32_t otaRxBytes      = 0;       // bytes metidos en el StreamBuffer
static volatile uint8_t  otaRxError      = 0;       // error detectado en el callback (código EE)
static volatile bool     otaPeerConnected = false;
static volatile bool     otaPeerLost     = false;
static volatile uint16_t otaPeerMtu      = 23;
static volatile unsigned long otaLastRxMs = 0;

// Orden de control pendiente (la ejecuta el bucle, que es quien toca Update)
static volatile bool     otaCtrlPending  = false;
static uint8_t           otaCtrlBuf[16];
static volatile size_t   otaCtrlLen      = 0;

static void otaNotify(const uint8_t* data, size_t len) {
  if (otaCtrlChr == nullptr || !otaPeerConnected) return;
  otaCtrlChr->setValue(data, len);
  otaCtrlChr->notify();
}

static void otaNotifyError(uint8_t code, uint32_t value) {
  uint8_t m[6] = {0xEE, code, (uint8_t)value, (uint8_t)(value >> 8), (uint8_t)(value >> 16), (uint8_t)(value >> 24)};
  otaNotify(m, sizeof(m));
}

class OtaServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
    otaPeerConnected = true;
    otaPeerLost      = false;
    otaPeerMtu       = connInfo.getMTU();
    // Intervalo corto (7.5-15 ms) para que la subida vaya rápida
    pServer->updateConnParams(connInfo.getConnHandle(), 6, 12, 0, 400);
    Serial.printf("[OTA-BLE] Conectado: %s\n", connInfo.getAddress().toString().c_str());
  }
  void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
    (void)pServer; (void)connInfo;
    otaPeerConnected = false;
    otaPeerLost      = true;
    Serial.printf("[OTA-BLE] Desconectado (razon=%d).\n", reason);
  }
  void onMTUChange(uint16_t MTU, NimBLEConnInfo& connInfo) override {
    (void)connInfo;
    otaPeerMtu = MTU;
  }
};

class OtaCtrlCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChr, NimBLEConnInfo& connInfo) override {
    otaPeerMtu = connInfo.getMTU();
    NimBLEAttValue v = pChr->getValue();
    if (v.length() == 0 || v.length() > sizeof(otaCtrlBuf) || otaCtrlPending) return;
    memcpy(otaCtrlBuf, v.data(), v.length());
    otaCtrlLen     = v.length();
    otaCtrlPending = true;
  }
};

class OtaDataCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChr, NimBLEConnInfo& connInfo) override {
    (void)connInfo;
    if (!otaReceiving || otaRxError != 0) return;
    NimBLEAttValue v = pChr->getValue();
    if (v.length() <= 4) return;
    const uint8_t* d = v.data();
    uint32_t offset = (uint32_t)d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
    size_t n = v.length() - 4;
    if (offset != otaRxBytes)                { otaRxError = 1; return; }
    if (otaRxBytes + n > otaImageSize)       { otaRxError = 6; return; }
    if (xStreamBufferSend(otaStream, d + 4, n, 0) != n) { otaRxError = 3; return; }
    otaRxBytes  = otaRxBytes + n;
    otaLastRxMs = millis();
  }
};

static OtaServerCallbacks otaServerCallbacks;
static OtaCtrlCallbacks   otaCtrlCallbacks;
static OtaDataCallbacks   otaDataCallbacks;

// Pantalla: nombre Bluetooth, PIN grande, estado y barra de progreso
static void renderOtaScreenOled(const char* statusLine, int progressPct) {
  if (!oledPresent) return;
  oledClear();
  oledFillRect(0, 0, 128, 11, true);
  oledDrawText(10, 2, "ACTUALIZAR POR BLE", 1, true);
  oledDrawText(2, 14, "BLE: TMDot-OTA", 1);
  oledDrawText(2, 29, "PIN", 1);
  oledDrawText(28, 24, otaPin, 2);

  oledDrawText(2, 42, statusLine ? statusLine : "", 1);
  if (progressPct >= 0) {
    oledDrawRect(2, 52, 124, 10, true);
    int wBar = (progressPct * 120) / 100;
    if (wBar > 120) wBar = 120;
    if (wBar > 0) oledFillRect(4, 54, wBar, 6, true);
  } else {
    oledDrawHLine(0, 52, 128, true);
    oledDrawText(2, 55, "1 CLICK: CANCELAR", 1);
  }
  oledFlush();
}

static bool otaButtonPressed() {
  if (digitalRead(PIN_ENC_SW) == LOW || digitalRead(PIN_BOOT_BUTTON) == LOW) {
    delay(30);
    if (digitalRead(PIN_ENC_SW) == LOW || digitalRead(PIN_BOOT_BUTTON) == LOW) {
      while (digitalRead(PIN_ENC_SW) == LOW || digitalRead(PIN_BOOT_BUTTON) == LOW) delay(10);
      return true;
    }
  }
  return false;
}

// Cabecera de imagen de aplicación ESP32: magic 0xE9 y, en el offset 0x20, esp_app_desc_t (0xABCD5432).
// Rechaza el .bin "merged", el del bootloader o cualquier otro archivo.
static bool otaLooksLikeAppImage(const uint8_t* b, size_t n) {
  if (n < 0x24) return false;
  return b[0] == 0xE9 && b[0x20] == 0x32 && b[0x21] == 0x54 && b[0x22] == 0xCD && b[0x23] == 0xAB;
}

void enterBleOtaMode() {
  Serial.println("\n[OTA-BLE] Iniciando modo de actualizacion por Bluetooth...");

  // PIN de esta sesión (6 cifras)
  snprintf(otaPin, sizeof(otaPin), "%06lu", (unsigned long)(esp_random() % 1000000UL));

  setRgbColor(LED_BRIGHTNESS, 0, LED_BRIGHTNESS); // Magenta = modo actualización
  renderOtaScreenOled("PREPARANDO...", -1);
  while (digitalRead(PIN_ENC_SW) == LOW || digitalRead(PIN_BOOT_BUTTON) == LOW) delay(10);

  // Pila BLE limpia: fuera la Dot y el puente; solo el servicio OTA
  shutdownBleStack();
  otaStream = xStreamBufferCreate(OTA_STREAM_BYTES, 1);
  static uint8_t stage[OTA_BLOCK_BYTES];
  if (otaStream == nullptr) {
    renderOtaScreenOled("ERROR: SIN MEMORIA", -1);
    delay(3000);
    ESP.restart();
  }

  NimBLEDevice::init(OTA_DEV_NAME);
  NimBLEDevice::setMTU(DOT_PREFERRED_MTU);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);  // la subida tarda ~1 min: mejor enlace que ahorro
  NimBLEServer* pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(&otaServerCallbacks, false);
  pServer->advertiseOnDisconnect(true);
  NimBLEService* pSvc = pServer->createService(OTA_SVC_UUID);
  otaCtrlChr = pSvc->createCharacteristic(OTA_CTRL_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY);
  otaCtrlChr->setCallbacks(&otaCtrlCallbacks);
  NimBLECharacteristic* pData = pSvc->createCharacteristic(OTA_DATA_UUID, NIMBLE_PROPERTY::WRITE_NR, 512);
  pData->setCallbacks(&otaDataCallbacks);
  pServer->start();

  NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
  NimBLEAdvertisementData advData;
  advData.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  advData.addServiceUUID(NimBLEUUID(OTA_SVC_UUID));
  NimBLEAdvertisementData scanData;
  scanData.setName(OTA_DEV_NAME);
  pAdv->setAdvertisementData(advData);
  pAdv->setScanResponseData(scanData);
  pAdv->enableScanResponse(true);
  pAdv->start();

  Serial.printf("[OTA-BLE] Anunciando '%s'. PIN: %s\n", OTA_DEV_NAME, otaPin);
  renderOtaScreenOled("ESPERANDO CONEXION", -1);

  uint32_t written      = 0;
  size_t   stageLen     = 0;
  uint8_t  pinFails     = 0;
  bool     updateOpen   = false;
  int      lastPctShown = -1;
  bool     wasConnected = false;
  unsigned long lastActivityMs = millis();

  auto abortUpdate = [&](const char* msg, uint8_t code) {
    if (updateOpen) Update.abort();
    updateOpen    = false;
    otaReceiving  = false;
    if (code != 0) otaNotifyError(code, otaRxBytes);
    xStreamBufferReset(otaStream);
    stageLen      = 0;
    written       = 0;
    lastPctShown  = -1;
    Serial.printf("[OTA-BLE] Subida cancelada: %s\n", msg);
    renderOtaScreenOled(msg, -1);
  };

  for (;;) {
    unsigned long now = millis();

    if (otaButtonPressed()) break;
    if (!otaReceiving && (now - lastActivityMs) > OTA_IDLE_EXIT_MS) break;

    // Conexión / desconexión del navegador
    if (otaPeerConnected != wasConnected) {
      wasConnected   = otaPeerConnected;
      lastActivityMs = now;
      if (wasConnected) {
        renderOtaScreenOled("CONECTADO", -1);
      } else {
        if (otaReceiving || updateOpen) abortUpdate("CONEXION PERDIDA", 0);
        else renderOtaScreenOled("ESPERANDO CONEXION", -1);
      }
    }

    // Órdenes de control
    if (otaCtrlPending) {
      uint8_t cmd[16];
      size_t len = otaCtrlLen;
      memcpy(cmd, otaCtrlBuf, len);
      otaCtrlPending = false;
      lastActivityMs = now;

      if (cmd[0] == OTA_CMD_START && len >= 11) {
        uint32_t size = (uint32_t)cmd[1] | ((uint32_t)cmd[2] << 8) | ((uint32_t)cmd[3] << 16) | ((uint32_t)cmd[4] << 24);
        uint8_t status = 0;
        if (updateOpen) { Update.abort(); updateOpen = false; }
        otaReceiving = false;
        xStreamBufferReset(otaStream);
        stageLen = 0; written = 0; lastPctShown = -1;
        if (memcmp(&cmd[5], otaPin, 6) != 0) {
          status = 1;
          pinFails++;
          renderOtaScreenOled("PIN INCORRECTO", -1);
        } else if (size < 0x24) {
          status = 2;
        } else if (!Update.begin(size)) {
          status = 3;
          Serial.printf("[OTA-BLE] Update.begin(%lu) fallo: %s\n", (unsigned long)size, Update.errorString());
          renderOtaScreenOled("ERROR: NO CABE", -1);
        } else {
          updateOpen   = true;
          otaImageSize = size;
          otaRxBytes   = 0;
          otaRxError   = 0;
          otaLastRxMs  = now;
          otaReceiving = true;
          Serial.printf("[OTA-BLE] Recibiendo %lu bytes (MTU %u).\n", (unsigned long)size, (unsigned)otaPeerMtu);
          renderOtaScreenOled("RECIBIENDO...", 0);
        }
        uint16_t mtu = otaPeerMtu;
        uint8_t resp[5] = {0x81, status, (uint8_t)mtu, (uint8_t)(mtu >> 8), OTA_PROTO_VERSION};
        otaNotify(resp, sizeof(resp));
        if (pinFails >= OTA_MAX_PIN_TRIES) break;

      } else if (cmd[0] == OTA_CMD_END) {
        uint8_t status = 1;
        if (updateOpen && written == otaImageSize && Update.end(true)) {
          status = 0;
        } else if (updateOpen) {
          Serial.printf("[OTA-BLE] Update.end fallo (%lu/%lu): %s\n", (unsigned long)written,
                        (unsigned long)otaImageSize, Update.errorString());
          Update.abort();
        }
        updateOpen   = false;
        otaReceiving = false;
        uint8_t resp[2] = {0x83, status};
        otaNotify(resp, sizeof(resp));
        if (status == 0) {
          Serial.println("[OTA-BLE] Firmware verificado y escrito. Reiniciando...");
          renderOtaScreenOled("OK! REINICIANDO...", 100);
          delay(800);
          ESP.restart();
        }
        renderOtaScreenOled("ERROR: IMAGEN MALA", -1);

      } else if (cmd[0] == OTA_CMD_ABORT) {
        abortUpdate("CANCELADO", 0);
        uint8_t resp[2] = {0x84, 0x00};
        otaNotify(resp, sizeof(resp));
      }
    }

    // Datos: del StreamBuffer a la flash, en bloques de 4 KB
    if (otaReceiving) {
      if (otaRxError != 0) {
        uint8_t code = otaRxError;
        abortUpdate(code == 1 ? "ERROR: DESFASE" : (code == 3 ? "ERROR: DESBORDE" : "ERROR: DATOS"), code);
        continue;
      }
      size_t got = xStreamBufferReceive(otaStream, stage + stageLen, OTA_BLOCK_BYTES - stageLen, pdMS_TO_TICKS(5));
      stageLen += got;
      const bool lastBlock = (written + stageLen) == otaImageSize;
      if (stageLen == OTA_BLOCK_BYTES || (lastBlock && stageLen > 0)) {
        if (written == 0 && !otaLooksLikeAppImage(stage, stageLen)) {
          abortUpdate("ERROR: NO ES FIRMWARE", 4);
          continue;
        }
        if (Update.write(stage, stageLen) != stageLen) {
          Serial.printf("[OTA-BLE] Update.write fallo: %s\n", Update.errorString());
          abortUpdate("ERROR AL ESCRIBIR", 2);
          continue;
        }
        written += stageLen;
        stageLen = 0;
        uint8_t prog[5] = {0x82, (uint8_t)written, (uint8_t)(written >> 8), (uint8_t)(written >> 16), (uint8_t)(written >> 24)};
        otaNotify(prog, sizeof(prog));
        lastActivityMs = millis();
        int pct = (int)((uint64_t)written * 100ULL / otaImageSize);
        if (pct >= lastPctShown + 3 || pct == 100) {
          lastPctShown = pct;
          char line[22];
          snprintf(line, sizeof(line), "RECIBIENDO %3d%%", pct);
          renderOtaScreenOled(line, pct);
        }
      } else if ((millis() - otaLastRxMs) > OTA_STALL_MS) {
        abortUpdate("ERROR: SIN DATOS", 5);
      }
    } else {
      delay(5);
    }
  }

  Serial.println("[OTA-BLE] Saliendo del modo de actualizacion. Reiniciando...");
  if (updateOpen) Update.abort();
  renderOtaScreenOled("SALIENDO...", -1);
  delay(300);
  ESP.restart();
}
