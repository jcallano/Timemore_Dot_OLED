// ============================================================================
// PUENTE BOOKOO: EL PANEL SE ANUNCIA COMO BOOKOO THEMIS ULTRA
// La máquina (Wendougee Lita/Data-S con E-Bar o GeeFlow) solo acepta básculas "BOOKOO_SC_U XXXXXX".
// El panel sigue siendo central de la Dot y, a la vez, periférico de la máquina:
//   Dot --(0xFFF1, 10 Hz)--> panel --(0x0FFE/0xFF11, trama 03 0B de 20 bytes)--> máquina
//   máquina --(0xFF12, 03 0A cmd)--> panel: tara por software y cronómetro del panel
// Las tramas se envían desde una tarea propia en el núcleo 0 en cuanto llega cada peso de la Dot (no
// desde loop(), que puede tardar hasta 16 ms volcando la OLED), con relleno cada 250 ms si la Dot calla.
// Pestaña del sketch: el IDE la concatena tras firmware.ino (orden alfabético).
// ============================================================================
#include <esp_mac.h>

static constexpr uint16_t UUID16_BOOKOO_SVC_0FFE  = 0x0FFE;
static constexpr uint16_t UUID16_BOOKOO_CHR_FF11  = 0xFF11;  // notify: telemetría
static constexpr uint16_t UUID16_BOOKOO_CHR_FF12  = 0xFF12;  // write: comandos
static constexpr uint32_t BOOKOO_FILL_MS          = 250;     // relleno si la Dot no manda peso
static constexpr uint32_t BOOKOO_MIN_GAP_MS       = 15;      // separación mínima entre tramas
// Anuncio sin máquina conectada: 60-100 ms (unidades de 0.625 ms). Lo bastante rápido para que el
// escaneo de la máquina lo encuentre a la primera y sin gastar tanto como los 20-40 ms del puente.
static constexpr uint16_t BOOKOO_ADV_MIN          = 96;
static constexpr uint16_t BOOKOO_ADV_MAX          = 160;

NimBLEServer* pBookooServer             = nullptr;
NimBLECharacteristic* pBookooNotifyChar = nullptr;
volatile bool bookooServerReady         = false;   // false mientras se apaga la pila (punteros inválidos)
TaskHandle_t bookooTxTaskHandle         = nullptr;
uint16_t bookooConnHandle               = 0xFFFF;

// ---------------------------------------------------------------------------
// Diagnóstico para la pantalla "modo puente" (renderBookooBridgeOled en 80_pantalla.ino)
// ---------------------------------------------------------------------------
static constexpr uint8_t  BOOKOO_CMD_LOG_LEN    = 3;
static constexpr uint32_t BOOKOO_OVERSHOOT_MS   = 3000;  // peso final = peso 3 s después del STOP
static constexpr uint32_t BOOKOO_AGE_WARN_MS    = 150;   // muestra más vieja que esto: hueco de la Dot

struct BookooCmdLogEntry {
  uint8_t cmd;
  unsigned long ms;        // millis() al procesarlo
  float weight;            // peso neto en ese instante (para STOP: peso al que paró la máquina)
  float overshoot;         // STOP: peso final - peso al parar
  bool hasOvershoot;
};
BookooCmdLogEntry bookooCmdLog[BOOKOO_CMD_LOG_LEN];  // [0] = el más reciente
uint8_t bookooCmdLogCount            = 0;
volatile uint32_t bookooBadCmds      = 0;   // escrituras sin cabecera 03 0A, con XOR mal o código desconocido
volatile uint32_t bookooLastAgeMs    = 0;   // edad de la muestra de la Dot en la última trama enviada
volatile uint32_t bookooMaxAgeMs     = 0;   // máxima desde el último cálculo (cada 1 s)
uint32_t bookooAgeMaxShown           = 0;   // máxima del último segundo (lo que muestra la OLED)
float bookooTxRate                   = 0.0f; // tramas/s del último segundo
float bookooMachineItvlMs            = 0.0f;
int bookooMachineRssi                = 0;
int bookooDotRssi                    = 0;
static unsigned long bookooStopEvalMs = 0;  // != 0: pendiente medir el rebase del último STOP

// ---------------------------------------------------------------------------
// Nombre "BOOKOO_SC_U XXXXXX" con la MAC Bluetooth (la que usa NimBLE como dirección pública)
// ---------------------------------------------------------------------------
void bookooPrepareName() {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_BT);
  bkproto::formatAdvName(bookooAdvName, sizeof(bookooAdvName), mac);
}

// ---------------------------------------------------------------------------
// Envío de la trama 03 0B (tarea bookooTx, núcleo 0)
// ---------------------------------------------------------------------------
// Caudal propio del puente: el mismo algoritmo que la OLED (src/flow_engine) pero alimentado aquí, en
// la tarea bookooTx, con cada muestra según llega. Antes se enviaba el caudal de loop(), que todavía no
// había procesado esa muestra: la máquina recibía el caudal con 100 ms de retraso respecto al peso.
static FlowEngine bookooFlow;
volatile uint32_t bookooSampleSeq      = 0;
volatile unsigned long bookooSampleMs  = 0;

static void bookooSendFrame(float net, float flowGps) {
  if (!bookooServerReady || !bookooSubscribed || pBookooNotifyChar == nullptr) return;

  unsigned long nowMs = millis();
  unsigned long totalMs = timerAccumulatedMs;
  if (timerRunning) totalMs += (nowMs - timerStartMillis);

  uint8_t batt = scaleBatteryValid ? scaleBatteryPct : 100;

  uint8_t frame[bkproto::WEIGHT_FRAME_LEN];
  bkproto::buildWeightFrame(frame, totalMs, net, flowGps, batt);
  if (pBookooNotifyChar->notify(frame, sizeof(frame))) {
    bookooFramesSent = bookooFramesSent + 1;
    // Edad del peso enviado: lo que tarda el panel + huecos de la Dot (las tramas de relleno la delatan)
    uint32_t age = millis() - bookooSampleMs;
    bookooLastAgeMs = age;
    if (age > bookooMaxAgeMs) bookooMaxAgeMs = age;
  }
}

static void bookooTxTask(void* arg) {
  (void)arg;
  uint32_t lastTxMs     = 0;
  uint32_t lastSeq      = 0;
  float lastTareOffset  = 0.0f;
  unsigned long lastRx  = 0;
  bookooFlow.reset(0.0f);
  for (;;) {
    // Despierta con cada peso de la Dot (o un comando) y, si no llega nada, cada 250 ms
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(BOOKOO_FILL_MS));

    // Peso neto = último bruto de la Dot - tara por software del panel (lo mismo que muestra la OLED)
    const float tare = softwareTareOffset;
    const float net  = bookooLatestRawGrams - tare;
    const uint32_t seq = bookooSampleSeq;
    if (tare != lastTareOffset) {
      // Tara (de la máquina, del panel o anulada): reiniciar la ventana como hace loop()
      lastTareOffset = tare;
      bookooFlow.reset(net);
    }
    if (seq != lastSeq) {
      lastSeq = seq;
      lastRx  = bookooSampleMs;
      bookooFlow.addSample(lastRx, net, false);
    } else {
      bookooFlow.decayIfStale(millis(), lastRx);
    }

    if (!bookooServerReady || !bookooSubscribed) continue;
    uint32_t since = millis() - lastTxMs;
    if (since < BOOKOO_MIN_GAP_MS) vTaskDelay(pdMS_TO_TICKS(BOOKOO_MIN_GAP_MS - since));
    bookooSendFrame(net, bookooFlow.currentGps);
    lastTxMs = millis();
  }
}

// Llamada desde el callback de notificación de la Dot (tarea host de NimBLE): solo guarda y avisa
void bookooOnScaleSample(float rawGrams) {
  bookooLatestRawGrams = rawGrams;
  bookooSampleMs       = millis();
  bookooSampleSeq      = bookooSampleSeq + 1;
  // Siempre (aunque no haya máquina): así el caudal del puente ya está asentado cuando se suscriba
  if (bookooTxTaskHandle != nullptr) xTaskNotifyGive(bookooTxTaskHandle);
}

void bookooKickTx() {
  if (bookooTxTaskHandle != nullptr) xTaskNotifyGive(bookooTxTaskHandle);
}

// ---------------------------------------------------------------------------
// Callbacks del servidor GATT
// ---------------------------------------------------------------------------
class BookooServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
    bookooConnHandle       = connInfo.getConnHandle();
    bookooMachineConnected = true;
    // 15-30 ms: de sobra para 10 Hz y deja hueco en la radio para el enlace de 15 ms con la Dot
    pServer->updateConnParams(connInfo.getConnHandle(), 12, 24, 0, 400);
    uiNeedsImmediateRedraw = true;
    Serial.printf("[BOOKOO] Maquina/app conectada: %s\n", connInfo.getAddress().toString().c_str());
  }
  void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
    (void)pServer; (void)connInfo;
    bookooMachineConnected = false;
    bookooSubscribed       = false;
    bookooConnHandle       = 0xFFFF;
    uiNeedsImmediateRedraw = true;
    Serial.printf("[BOOKOO] Maquina/app desconectada (razon=%d). Anunciando de nuevo.\n", reason);
  }
  void onConnParamsUpdate(NimBLEConnInfo& connInfo) override {
    Serial.printf("[BOOKOO] Enlace con la maquina: %.1f ms, latencia %u\n",
                  connInfo.getConnInterval() * 1.25f, connInfo.getConnLatency());
  }
};

class BookooCharCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChr, NimBLEConnInfo& connInfo) override {
    (void)connInfo;
    NimBLEAttValue v = pChr->getValue();
    bkproto::Command c = bkproto::parseCommand(v.data(), v.length());
    bookooCmdsRx = bookooCmdsRx + 1;
    if (!c.valid) {
      bookooBadCmds = bookooBadCmds + 1;
      bookooKickTx();  // pulsos de 1-2 bytes de algunas apps: responder con una trama
      return;
    }
    const bool known = c.cmd == bkproto::CMD_TARE || c.cmd == bkproto::CMD_TIMER_START ||
                       c.cmd == bkproto::CMD_TIMER_STOP || c.cmd == bkproto::CMD_TIMER_RESET ||
                       c.cmd == bkproto::CMD_TARE_START || c.cmd == bkproto::CMD_SMOOTHING;
    if (!c.xorOk || !known) {
      bookooBadCmds = bookooBadCmds + 1;
      char hex[3 * 12 + 1] = {0};
      size_t n = v.length() < 12 ? v.length() : 12;
      for (size_t i = 0; i < n; i++) snprintf(hex + 3 * i, 4, "%02X ", v.data()[i]);
      Serial.printf("[BOOKOO] Comando raro: %s(%u bytes, xor %s)\n",
                    hex, (unsigned)v.length(), c.xorOk ? "ok" : "MAL");
    }
    if (c.cmd < 32) __atomic_fetch_or(&bookooPendingCmds, 1UL << c.cmd, __ATOMIC_RELAXED);
  }
  void onSubscribe(NimBLECharacteristic* pChr, NimBLEConnInfo& connInfo, uint16_t subValue) override {
    (void)connInfo;
    if (pChr == pBookooNotifyChar) {
      bookooSubscribed = (subValue & 0x0001) != 0;
      Serial.printf("[BOOKOO] Notificaciones 0xFF11 %s.\n", bookooSubscribed ? "ACTIVADAS" : "desactivadas");
      if (bookooSubscribed) bookooKickTx();
    }
  }
};

static BookooServerCallbacks bookooServerCallbacks;
static BookooCharCallbacks bookooCharCallbacks;

// ---------------------------------------------------------------------------
// Servidor y anuncio (se crean tras NimBLEDevice::init(); deinit los destruye)
// ---------------------------------------------------------------------------
void startBookooAdvertising() {
  NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
  pAdv->stop();

  uint8_t raw[31];
  NimBLEAdvertisementData advData;
  advData.addData(raw, bkproto::buildAdvData(raw, sizeof(raw), bookooAdvName));
  NimBLEAdvertisementData scanData;
  scanData.addData(raw, bkproto::buildScanResponse(raw, sizeof(raw), bookooAdvName));

  pAdv->setAdvertisementData(advData);
  pAdv->setScanResponseData(scanData);
  pAdv->enableScanResponse(true);
  pAdv->setMinInterval(BOOKOO_ADV_MIN);
  pAdv->setMaxInterval(BOOKOO_ADV_MAX);
  bool ok = pAdv->start();
  Serial.printf("[BOOKOO] Anunciando como '%s' (%s).\n", bookooAdvName, ok ? "OK" : "FALLO");
}

void setupBookooServer() {
  if (!bookooBridgeEnabled) return;

  pBookooServer = NimBLEDevice::createServer();
  pBookooServer->setCallbacks(&bookooServerCallbacks, false);
  pBookooServer->advertiseOnDisconnect(true);

  // Device Information: algunas apps comprueban fabricante y modelo
  NimBLEService* pInfo = pBookooServer->createService(NimBLEUUID((uint16_t)0x180A));
  pInfo->createCharacteristic(NimBLEUUID((uint16_t)0x2A24), NIMBLE_PROPERTY::READ)->setValue("Themis");
  pInfo->createCharacteristic(NimBLEUUID((uint16_t)0x2A29), NIMBLE_PROPERTY::READ)->setValue("BOOKOO");
  pInfo->createCharacteristic(NimBLEUUID((uint16_t)0x2A26), NIMBLE_PROPERTY::READ)->setValue("2.0.08");

  NimBLEService* pSvc = pBookooServer->createService(NimBLEUUID(UUID16_BOOKOO_SVC_0FFE));
  pBookooNotifyChar = pSvc->createCharacteristic(NimBLEUUID(UUID16_BOOKOO_CHR_FF11),
                                                 NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ);
  uint8_t initFrame[bkproto::WEIGHT_FRAME_LEN];
  bkproto::buildWeightFrame(initFrame, 0, 0.0f, 0.0f, 100);
  pBookooNotifyChar->setValue(initFrame, sizeof(initFrame));
  pBookooNotifyChar->setCallbacks(&bookooCharCallbacks);

  NimBLECharacteristic* pWrite = pSvc->createCharacteristic(
      NimBLEUUID(UUID16_BOOKOO_CHR_FF12),
      NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ);
  pWrite->setCallbacks(&bookooCharCallbacks);

  pBookooServer->start();

  if (bookooTxTaskHandle == nullptr) {
    xTaskCreatePinnedToCore(bookooTxTask, "bookooTx", 3072, nullptr, 5, &bookooTxTaskHandle, 0);
  }
  bookooServerReady = true;
  startBookooAdvertising();
}

// Antes de NimBLEDevice::deinit(): la tarea deja de usar los punteros que se van a borrar
void teardownBookooServer() {
  bookooServerReady = false;
  if (pBookooServer != nullptr) {
    NimBLEDevice::getAdvertising()->stop();
    if (bookooConnHandle != 0xFFFF) pBookooServer->disconnect(bookooConnHandle);
    delay(20);
  }
  pBookooServer          = nullptr;
  pBookooNotifyChar      = nullptr;
  bookooMachineConnected = false;
  bookooSubscribed       = false;
  bookooConnHandle       = 0xFFFF;
}

// ---------------------------------------------------------------------------
// Comandos de la máquina (procesados en loop(), dueño del estado del cronómetro y la tara)
// ---------------------------------------------------------------------------
static void bookooTimerStart() {
  if (timerRunning) return;
  if (timerAccumulatedMs == 0) resetLivePourRecording();
  timerStartMillis = millis();
  timerRunning     = true;
  autoTimerArmed   = false;
}

// La máquina para el cronómetro, pero el shot NO se guarda en el historial de Pour-Over
// (era un espresso; el rebase se registra aparte en bookooUpdateDiagnostics)
static void bookooTimerStop() {
  if (!timerRunning) return;
  timerAccumulatedMs += (millis() - timerStartMillis);
  timerRunning = false;
}

static void bookooLogCmd(uint8_t cmd) {
  for (int i = BOOKOO_CMD_LOG_LEN - 1; i > 0; i--) bookooCmdLog[i] = bookooCmdLog[i - 1];
  bookooCmdLog[0].cmd          = cmd;
  bookooCmdLog[0].ms           = millis();
  bookooCmdLog[0].weight       = currentWeightGrams;
  bookooCmdLog[0].overshoot    = 0.0f;
  bookooCmdLog[0].hasOvershoot = false;
  if (bookooCmdLogCount < BOOKOO_CMD_LOG_LEN) bookooCmdLogCount++;
}

// Cada vuelta de loop(): vista automática, estadísticas por segundo y rebase tras un STOP
void bookooUpdateDiagnostics() {
  static bool prevConnected       = false;
  static unsigned long lastSecMs  = 0;
  static uint32_t lastFrames      = 0;
  unsigned long now = millis();

  // Pantalla de puente al conectar la máquina; al desconectarla, vuelta a la vista principal
  const bool connected = bookooMachineConnected;
  if (connected != prevConnected) {
    prevConnected = connected;
    if (connected) {
      currentScreenView = BOOKOO_SCREEN_VIEW;
      bookooCmdLogCount = 0;
      bookooBadCmds     = 0;
      bookooStopEvalMs  = 0;
    } else if (currentScreenView == BOOKOO_SCREEN_VIEW) {
      currentScreenView = 0;
    }
    uiNeedsImmediateRedraw = true;
  }

  if (now - lastSecMs >= 1000UL) {
    uint32_t frames = bookooFramesSent;
    bookooTxRate = (lastSecMs == 0) ? 0.0f
                                    : (frames - lastFrames) * 1000.0f / (float)(now - lastSecMs);
    lastFrames = frames;
    lastSecMs  = now;
    bookooAgeMaxShown = bookooMaxAgeMs;
    bookooMaxAgeMs    = 0;

    bookooMachineRssi   = 0;
    bookooMachineItvlMs = 0.0f;
    if (connected && bookooServerReady && pBookooServer != nullptr && bookooConnHandle != 0xFFFF) {
      int8_t r = 0;
      if (ble_gap_conn_rssi(bookooConnHandle, &r) == 0) bookooMachineRssi = r;
      bookooMachineItvlMs = pBookooServer->getPeerInfoByHandle(bookooConnHandle).getConnInterval() * 1.25f;
    }
    bookooDotRssi = (scaleConnected && pScaleClient != nullptr && pScaleClient->isConnected())
                        ? pScaleClient->getRssi() : 0;
  }

  // Rebase: cuánto sigue cayendo en la taza después de que la máquina mande STOP
  if (bookooStopEvalMs != 0 && (now - bookooStopEvalMs) >= BOOKOO_OVERSHOOT_MS) {
    bookooStopEvalMs = 0;
    for (uint8_t i = 0; i < bookooCmdLogCount; i++) {
      if (bookooCmdLog[i].cmd != bkproto::CMD_TIMER_STOP) continue;
      bookooCmdLog[i].overshoot    = currentWeightGrams - bookooCmdLog[i].weight;
      bookooCmdLog[i].hasOvershoot = true;
      Serial.printf("[BOOKOO] Shot: STOP a %.1f g, final %.1f g, rebase %+.1f g, tiempo %.1f s\n",
                    bookooCmdLog[i].weight, currentWeightGrams, bookooCmdLog[i].overshoot,
                    timerAccumulatedMs / 1000.0f);
      break;
    }
    uiNeedsImmediateRedraw = true;
  }
}

static void bookooTimerReset() {
  pourSavedBannerUntilMs = 0;
  timerRunning           = false;
  timerAccumulatedMs     = 0;
  autoTimerArmed         = autoTimerEnabled;
  resetLivePourRecording();
}

static void bookooTare() {
  pendingScaleTare = false;
  executeTimemoreTare();   // tara por software (sin tara física en la Dot: evita su zero-lock)
}

void serviceBookooBridge() {
  if (!bookooBridgeEnabled) return;
  bookooUpdateDiagnostics();
  uint32_t cmds = __atomic_exchange_n(&bookooPendingCmds, 0UL, __ATOMIC_RELAXED);
  if (cmds == 0) return;

  auto has = [cmds](uint8_t c) { return (cmds & (1UL << c)) != 0; };
  if (has(bkproto::CMD_TARE_START)) {
    bookooLogCmd(bkproto::CMD_TARE_START);
    bookooStopEvalMs = 0;  // la tara invalida la medida del rebase pendiente
    bookooTimerReset();
    bookooTare();
    bookooTimerStart();
    Serial.println("[BOOKOO] Maquina: TARA + START.");
  }
  if (has(bkproto::CMD_TARE)) {
    bookooLogCmd(bkproto::CMD_TARE);
    bookooStopEvalMs = 0;
    bookooTare();
    Serial.println("[BOOKOO] Maquina: TARA.");
  }
  if (has(bkproto::CMD_TIMER_RESET)) {
    bookooLogCmd(bkproto::CMD_TIMER_RESET);
    bookooTimerReset();
    Serial.println("[BOOKOO] Maquina: RESET cronometro.");
  }
  if (has(bkproto::CMD_TIMER_START)) {
    bookooLogCmd(bkproto::CMD_TIMER_START);
    bookooTimerStart();
    Serial.println("[BOOKOO] Maquina: START cronometro.");
  }
  if (has(bkproto::CMD_TIMER_STOP)) {
    bookooLogCmd(bkproto::CMD_TIMER_STOP);
    bookooStopEvalMs = millis();
    bookooTimerStop();
    Serial.printf("[BOOKOO] Maquina: STOP cronometro a %.1f g (rebase en %lu s).\n",
                  currentWeightGrams, (unsigned long)(BOOKOO_OVERSHOOT_MS / 1000UL));
  }
  if (has(bkproto::CMD_SMOOTHING)) {
    bookooLogCmd(bkproto::CMD_SMOOTHING);
    Serial.println("[BOOKOO] Maquina: SMOOTHING (ignorado).");
  }
  registerUserOrScaleActivity();
  uiNeedsImmediateRedraw = true;
  bookooKickTx();  // la máquina ve el 0.00 g / el cronómetro sin esperar a la siguiente muestra
}

// Activar o desactivar el puente (menú / orden 'b'): se guarda en NVS y se reinicia la pila BLE para
// crear o quitar el servidor GATT (NimBLE no permite cambiar servicios con conexiones activas).
void setBookooBridgeEnabled(bool enable) {
  if (enable == bookooBridgeEnabled) return;
  bookooBridgeEnabled = enable;
  Preferences p;
  p.begin(NVS_POUR_NAMESPACE, false);
  p.putBool("bk_on", enable);
  p.end();
  Serial.printf("[BOOKOO] Puente %s. Reiniciando BLE...\n", enable ? "ACTIVADO" : "DESACTIVADO");
  shutdownBleStack();
  initBleStack();
  startTimemoreScan();
}
