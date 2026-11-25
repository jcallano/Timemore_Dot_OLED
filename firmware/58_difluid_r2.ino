// ============================================================================
// REFRACTÓMETRO DIFLUID R2: CONEXIÓN A DEMANDA Y MUESTREO INTELIGENTE DE TDS
// Pestaña del sketch: el IDE la concatena tras firmware.ino (orden alfabético).
//
// El R2 solo se conecta mientras se usa (Menú -> TDS (R2)): es la tercera conexión BLE, junto a la Dot y
// la cafetera por el puente (NimBLE admite 3). Tras R2_IDLE_MS sin usarlo se desconecta.
//   Off -> Scanning -> Connecting -> (LinkUp) -> Ready <-> Measuring
// La primera vez acepta un aparato cuyo nombre contenga "R2" y que tenga el servicio 0x00FF; cuando
// responde con su modelo ("DFT-R..."), guarda su MAC y desde entonces solo acepta esa.
// Medida: pruebas simples encadenadas; tds::Sampler (src/tds_sampler) decide cuándo es fiable (deriva de
// temperatura y evaporación). El resultado se guarda en el pour-over o el shot elegido, con su EY.
// Protocolo: src/difluid_protocol (documentación oficial de DiFluid).
// ============================================================================

static const char*        NVS_R2_NAMESPACE  = "tmdot_r2";
static constexpr uint32_t R2_SCAN_MS        = 20000;   // buscarlo como mucho 20 s
static constexpr uint32_t R2_CONNECT_MS     = 8000;
static constexpr uint32_t R2_IDLE_MS        = 180000;  // desconectar tras 3 min sin usarlo
static constexpr uint32_t R2_TEST_TIMEOUT   = 15000;   // una prueba simple tarda unos segundos
static constexpr uint32_t R2_TEST_GAP_MS    = 400;     // pausa entre pruebas

// R2State: en firmware.ino
R2State r2State                 = R2State::Off;
char r2StatusMsg[22]            = "";                 // último aviso (no encontrado, sin muestra...)

static char r2SavedMac[18]      = "";
static uint8_t r2SavedType      = 0;
static char r2CandMac[18]       = "";
static uint8_t r2CandType       = 0;
static volatile bool r2CandFound   = false;
static volatile bool r2EvLinkUp    = false;
static volatile bool r2EvLinkFail  = false;
static volatile bool r2EvDisconn   = false;
static bool r2ScanByUs          = false;
static bool r2Triedalt          = false;
static unsigned long r2StateSinceMs = 0;
unsigned long r2LastUseMs       = 0;
static NimBLEClient* pR2Client  = nullptr;
static NimBLERemoteCharacteristic* pR2Char = nullptr;
char r2Model[12]                = "";

struct R2Raw { uint32_t ms; uint8_t len; uint8_t b[dfproto::MAX_FRAME + 6]; };
static QueueHandle_t r2Queue    = nullptr;
static bool r2LogFrames         = true;   // traza de tramas en el monitor serie (diagnóstico)

// Medida en curso
tds::Sampler r2Sampler;
static bool r2TestInFlight      = false;
static unsigned long r2TestSentMs = 0, r2NextTestAtMs = 0;
float r2LivePrismC = 0.0f, r2LiveSampleC = 0.0f, r2LiveTds = 0.0f;
bool r2LiveHaveTemp = false, r2LiveHaveTds = false;
// Cero (calibración con agua destilada): pedido desde el menú; se lanza al estar conectado
static bool r2PendingZero       = false;
static constexpr uint32_t R2_ZERO_TIMEOUT = 120000;   // el R2 espera a que la muestra se estabilice (~50 s)
// Registro destino: pour-over o shot, por su id
bool r2TargetIsShot             = false;
uint16_t r2TargetId             = 0;

static void r2SetState(R2State s) {
  r2State        = s;
  r2StateSinceMs = millis();
  uiNeedsImmediateRedraw = true;
}

static void r2Fail(const char* msg) {
  strncpy(r2StatusMsg, msg, sizeof(r2StatusMsg) - 1);
  r2StatusMsg[sizeof(r2StatusMsg) - 1] = '\0';
  Serial.printf("[R2] %s\n", msg);
  r2TestInFlight = false;
  if (pR2Client != nullptr && pR2Client->isConnected()) r2SetState(R2State::Ready);
  else                                                  r2SetState(R2State::Failed);
}

void loadR2Settings() {
  Preferences p;
  p.begin(NVS_R2_NAMESPACE, true);
  String mac  = p.getString("mac", "");
  r2SavedType = p.getUChar("type", 0);
  p.end();
  strncpy(r2SavedMac, mac.c_str(), sizeof(r2SavedMac) - 1);
  if (r2Queue == nullptr) r2Queue = xQueueCreate(16, sizeof(R2Raw));
  Serial.printf("[R2] %s\n", r2SavedMac[0] ? r2SavedMac : "sin R2 vinculado (se vincula el primero que aparezca)");
}

static void saveR2Mac(const char* mac, uint8_t type) {
  if (strcasecmp(mac, r2SavedMac) == 0) return;
  strncpy(r2SavedMac, mac, sizeof(r2SavedMac) - 1);
  r2SavedType = type;
  Preferences p;
  p.begin(NVS_R2_NAMESPACE, false);
  p.putString("mac", r2SavedMac);
  p.putUChar("type", type);
  p.end();
  Serial.printf("[R2] Vinculado: %s\n", r2SavedMac);
}

// ---------------------------------------------------------------------------
// Escaneo: lo llama handleScannedDevice() (50_ble_scale.ino) con cada anuncio
// ---------------------------------------------------------------------------
void handleR2Advert(const NimBLEAdvertisedDevice* dev) {
  if (dev == nullptr || r2State != R2State::Scanning || r2CandFound || !dev->isConnectable()) return;
  const std::string mac = dev->getAddress().toString();
  bool match;
  if (r2SavedMac[0] != '\0') {
    match = (strcasecmp(mac.c_str(), r2SavedMac) == 0);
  } else {
    if (!dev->haveName()) return;
    String name = String(dev->getName().c_str());
    name.toUpperCase();
    match = (name.indexOf("R2") >= 0) || dev->isAdvertisingService(NimBLEUUID(dfproto::SERVICE_UUID16));
  }
  if (!match) return;
  strncpy(r2CandMac, mac.c_str(), sizeof(r2CandMac) - 1);
  r2CandType  = dev->getAddress().getType();
  r2CandFound = true;
}

#if defined(NIMBLE_V2)
class R2ClientCallbacks : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient* c) override { (void)c; r2EvLinkUp = true; }
  void onConnectFail(NimBLEClient* c, int reason) override { (void)c; (void)reason; r2EvLinkFail = true; }
  void onDisconnect(NimBLEClient* c, int reason) override { (void)c; (void)reason; r2EvDisconn = true; }
};
static R2ClientCallbacks r2ClientCallbacks;

static void r2NotifyCb(NimBLERemoteCharacteristic* c, uint8_t* data, size_t len, bool isNotify) {
  (void)c; (void)isNotify;
  if (r2Queue == nullptr || data == nullptr) return;
  R2Raw r;
  r.ms = millis();
  r.len = static_cast<uint8_t>(len > sizeof(r.b) ? sizeof(r.b) : len);
  memcpy(r.b, data, r.len);
  xQueueSend(r2Queue, &r, 0);
}
#endif

static bool r2Write(uint8_t func, uint8_t cmd, const uint8_t* data, uint8_t len) {
  if (pR2Char == nullptr || pR2Client == nullptr || !pR2Client->isConnected()) return false;
  uint8_t buf[dfproto::MAX_FRAME];
  const size_t n = dfproto::buildCmd(func, cmd, data, len, buf, sizeof(buf));
  if (n == 0) return false;
  if (r2LogFrames) Serial.printf("[R2] %lu ms >> func %u cmd %u\n", millis(), func, cmd);
  return pR2Char->writeValue(buf, n, pR2Char->canWrite());
}

// ---------------------------------------------------------------------------
// API para la interfaz
// ---------------------------------------------------------------------------
// Pide el R2 (al entrar en la pantalla TDS o al pulsar): si no está conectado, lo busca
void r2Request() {
  r2LastUseMs = millis();
  if (r2State == R2State::Off || r2State == R2State::Failed) {
    r2StatusMsg[0] = '\0';
    r2CandFound    = false;
    r2SetState(R2State::Scanning);
  }
}

bool r2Connected() { return r2State == R2State::Ready || r2State == R2State::Measuring || r2State == R2State::Calibrating; }

void r2StartMeasure() {
  r2LastUseMs = millis();
  if (r2State != R2State::Ready) { r2Request(); return; }
  r2Sampler.reset();
  r2LiveHaveTemp = false;
  r2LiveHaveTds  = false;
  r2StatusMsg[0] = '\0';
  r2TestInFlight = false;
  r2NextTestAtMs = millis();
  r2SetState(R2State::Measuring);
  Serial.printf("[R2] Medida para %s #%u\n", r2TargetIsShot ? "shot" : "pour-over", r2TargetId);
}

// Menú -> Cero R2: con agua destilada en el R2
void r2RequestZero() {
  r2PendingZero = true;
  r2StatusMsg[0] = '\0';
  r2Request();
}

void r2CancelMeasure() {
  if (r2State == R2State::Measuring) {
    r2TestInFlight = false;
    r2SetState(R2State::Ready);
  }
}

// Definida en 30_storage / 45_espresso: guarda el TDS en el registro destino
void storeTdsResult(bool isShot, uint16_t id, uint16_t packed);
void beginTasteInput();   // 85_controles.ino

static void r2OnTestResult(float tdsVal) {
  r2TestInFlight = false;
  r2LiveTds      = tdsVal;
  r2LiveHaveTds  = true;
  if (r2State != R2State::Measuring) return;
  tds::Sample s;
  s.tds     = tdsVal;
  s.prismC  = r2LivePrismC;
  s.sampleC = r2LiveSampleC;
  const tds::Phase ph = r2Sampler.add(s);
  Serial.printf("[R2] Prueba %u: TDS %.2f %% | prisma %.1f C muestra %.1f C | %s\n", r2Sampler.count(), tdsVal,
                r2LivePrismC, r2LiveSampleC,
                ph == tds::Phase::Temperature ? "temperatura" : (ph == tds::Phase::Converging ? "estabilizando" : "listo"));
  if (ph == tds::Phase::Done) {
    const uint16_t packed = tds::pack(r2Sampler.result(), r2Sampler.flags());
    storeTdsResult(r2TargetIsShot, r2TargetId, packed);
    Serial.printf("[R2] Resultado: TDS %.2f %%%s%s%s\n", r2Sampler.result(),
                  (r2Sampler.flags() & tds::FLAG_EVAP) ? " (evaporacion)" : "",
                  (r2Sampler.flags() & tds::FLAG_DOUBTFUL) ? " (dudosa)" : "",
                  (r2Sampler.flags() & tds::FLAG_TEMP) ? " (temperatura inestable)" : "");
    r2SetState(R2State::Ready);
    // Primero probar: con la pantalla TDS abierta, pasa a "¿cómo sabe?" antes de enseñar el gráfico
    if (uiState == UiState::TdsMeasure) beginTasteInput();
  } else {
    r2NextTestAtMs = millis() + R2_TEST_GAP_MS;
  }
}

static uint32_t r2FrameRxMs = 0;
static void r2HandleFrame(const uint8_t* raw, size_t n) {
  if (r2LogFrames) {
    char hex[3 * (dfproto::MAX_FRAME + 6) + 1];
    size_t k = 0;
    for (size_t i = 0; i < n && k + 4 < sizeof(hex); i++) k += snprintf(hex + k, sizeof(hex) - k, "%02X ", raw[i]);
    hex[k] = 0;
    Serial.printf("[R2] %lu ms (rx %lu) << %s\n", millis(), static_cast<unsigned long>(r2FrameRxMs), hex);
  }
  const dfproto::Frame f = dfproto::parseFrame(raw, n);
  if (!f.valid) return;
  if (f.func == dfproto::FUNC_INFO && f.cmd == dfproto::INFO_MODEL) {
    const size_t m = f.len < sizeof(r2Model) - 1 ? f.len : sizeof(r2Model) - 1;
    memcpy(r2Model, f.data, m);
    r2Model[m] = '\0';
    Serial.printf("[R2] Modelo %s\n", r2Model);
    if (strncmp(r2Model, "DFT-R", 5) == 0 && pR2Client != nullptr) {
      saveR2Mac(pR2Client->getPeerAddress().toString().c_str(), pR2Client->getPeerAddress().getType());
    }
    return;
  }
  if (f.func != dfproto::FUNC_ACTION) return;
  const dfproto::TestEvent e = dfproto::decodeAction(f);
  if (f.cmd == dfproto::ACT_CALIBRATE) {   // cero: el R2 responde estado 'calibración terminada' y concentración 0
    if (e.kind == dfproto::TestEvent::Temperature) { r2LivePrismC = e.prismC; r2LiveSampleC = e.sampleC; r2LiveHaveTemp = true; }
    if (e.kind == dfproto::TestEvent::Status && e.status == dfproto::ST_CAL_FINISHED) {
      strncpy(r2StatusMsg, "CERO OK", sizeof(r2StatusMsg) - 1);
      Serial.println("[R2] Cero hecho.");
      r2SetState(r2State == R2State::Measuring ? R2State::Measuring : R2State::Ready);
    }
    if (e.kind != dfproto::TestEvent::Error) return;
  }
  switch (e.kind) {
    case dfproto::TestEvent::Temperature:
      r2LivePrismC   = e.prismC;
      r2LiveSampleC  = e.sampleC;
      r2LiveHaveTemp = true;
      uiNeedsImmediateRedraw = true;
      break;
    case dfproto::TestEvent::Result:
      r2OnTestResult(e.tds);
      break;
    case dfproto::TestEvent::Error:
      if (e.errClass == 2 && e.errCode == dfproto::ERR_CAL_FAILED)        r2Fail("CERO FALLIDO");
      else if (e.errClass == 2 && e.errCode == dfproto::ERR_NO_LIQUID)    r2Fail("SIN MUESTRA");
      else if (e.errClass == 2 && e.errCode == dfproto::ERR_BEYOND_RANGE) r2Fail("FUERA DE RANGO");
      else if (e.errClass == 3)                                           r2Fail("ERROR DEL R2");
      else                                                                r2Fail("PRUEBA FALLIDA");
      break;
    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// Máquina de estados (cada vuelta de loop())
// ---------------------------------------------------------------------------
static void r2Disconnect() {
  if (pR2Client != nullptr && pR2Client->isConnected()) pR2Client->disconnect();
  pR2Char = nullptr;
}

// La pila BLE se apaga (reposo): el cliente se borra con ella
void r2OnStackShutdown() {
  pR2Client = nullptr;
  pR2Char   = nullptr;
  r2State   = R2State::Off;
  if (r2Queue != nullptr) xQueueReset(r2Queue);
}

void serviceR2() {
#if defined(NIMBLE_V2)
  const unsigned long now = millis();
  NimBLEScan* pScan = NimBLEDevice::getScan();

  if (r2EvDisconn) {
    r2EvDisconn = false;
    pR2Char = nullptr;
    if (r2State == R2State::Ready || r2State == R2State::Measuring || r2State == R2State::Calibrating) {
      Serial.println("[R2] Desconectado.");
      if (r2State == R2State::Measuring) r2Fail("R2 DESCONECTADO");
      r2SetState(R2State::Off);
    }
  }

  switch (r2State) {
    case R2State::Off:
    case R2State::Failed:
      break;

    case R2State::Scanning: {
      if (r2CandFound) {
        // La Dot tiene prioridad: no lanzar otra conexión mientras ella conecta
        if (bleConnectPhase != BLE_IDLE || doConnectToScale) break;
        if (pScan->isScanning()) pScan->stop();
        r2ScanByUs = false;
        isScanning = false;
        if (pR2Client == nullptr) {
          pR2Client = NimBLEDevice::createClient();
          if (pR2Client == nullptr) { r2Fail("SIN CONEXION LIBRE"); break; }
          pR2Client->setClientCallbacks(&r2ClientCallbacks, false);
          pR2Client->setConnectionParams(24, 40, 0, 400);   // 30-50 ms: deja radio a la Dot (10-15 ms)
        }
        r2Triedalt   = false;
        r2EvLinkUp   = false;
        r2EvLinkFail = false;
        pR2Client->setConnectTimeout(R2_CONNECT_MS);
        Serial.printf("[R2] Conectando a %s...\n", r2CandMac);
        if (!pR2Client->connect(NimBLEAddress(std::string(r2CandMac), r2CandType), true, true, false)) {
          r2Fail("NO CONECTA");
          break;
        }
        r2SetState(R2State::Connecting);
        break;
      }
      if ((long)(millis() - r2StateSinceMs) > (long)R2_SCAN_MS) {
        if (r2ScanByUs && pScan->isScanning()) pScan->stop();
        r2ScanByUs = false;
        r2Fail("R2 NO ENCONTRADO");
        break;
      }
      // Sin la Dot conectada ya hay un escaneo periódico que también ve al R2; con ella, uno propio
      if (!pScan->isScanning() && bleConnectPhase == BLE_IDLE && !doConnectToScale) {
        pScan->clearResults();
        pScan->setScanCallbacks(&scaleScanCallbacks, true);
        pScan->setActiveScan(true);    // el nombre suele ir en la respuesta al escaneo
        pScan->setInterval(100);
        pScan->setWindow(40);
        r2ScanByUs = pScan->start(R2_SCAN_MS, false, true);
      }
      break;
    }

    case R2State::Connecting:
      if (r2EvLinkFail || (long)(millis() - r2StateSinceMs) > (long)R2_CONNECT_MS) {
        r2EvLinkFail = false;
        if (!r2Triedalt) {   // otro tipo de dirección (pública <-> aleatoria)
          r2Triedalt = true;
          r2StateSinceMs = now;
          pR2Client->connect(NimBLEAddress(std::string(r2CandMac), r2CandType ? 0 : 1), true, true, false);
          break;
        }
        r2Fail("NO CONECTA");
        break;
      }
      if (r2EvLinkUp) {
        r2EvLinkUp = false;
        NimBLERemoteService* svc = pR2Client->getService(NimBLEUUID(dfproto::SERVICE_UUID16));
        pR2Char = (svc != nullptr) ? svc->getCharacteristic(NimBLEUUID(dfproto::CHAR_UUID16)) : nullptr;
        if (pR2Char == nullptr || !pR2Char->canNotify() || !pR2Char->subscribe(true, r2NotifyCb)) {
          Serial.println("[R2] No tiene el servicio del DiFluid R2.");
          r2Disconnect();
          r2Fail("NO ES UN R2");
          break;
        }
        if (r2Queue != nullptr) xQueueReset(r2Queue);
        const uint8_t celsius = 0;
        r2Write(dfproto::FUNC_SETTINGS, dfproto::SET_TEMP_UNIT, &celsius, 1);
        r2Write(dfproto::FUNC_INFO, dfproto::INFO_MODEL, nullptr, 0);
        r2LastUseMs = now;
        Serial.println("[R2] Conectado.");
        r2SetState(R2State::Ready);
      }
      break;

    case R2State::Ready:
    case R2State::Measuring:
    case R2State::Calibrating: {
      R2Raw r;
      while (r2Queue != nullptr && xQueueReceive(r2Queue, &r, 0) == pdTRUE) { r2FrameRxMs = r.ms; r2HandleFrame(r.b, r.len); }
      if (r2State == R2State::Ready && r2PendingZero) {
        r2PendingZero = false;
        if (r2Write(dfproto::FUNC_ACTION, dfproto::ACT_CALIBRATE, nullptr, 0)) {
          Serial.println("[R2] Haciendo el cero...");
          r2SetState(R2State::Calibrating);
        } else {
          r2Fail("ERROR AL ESCRIBIR");
        }
      }
      if (r2State == R2State::Calibrating) {
        r2LastUseMs = now;
        if ((long)(millis() - r2StateSinceMs) > (long)R2_ZERO_TIMEOUT) r2Fail("CERO SIN RESPUESTA");
        break;
      }
      if (r2State == R2State::Measuring) {
        r2LastUseMs = now;
        if (!r2TestInFlight && (long)(now - r2NextTestAtMs) >= 0) {
          if (r2Write(dfproto::FUNC_ACTION, dfproto::ACT_SINGLE, nullptr, 0)) {
            r2TestInFlight = true;
            r2TestSentMs   = now;
          } else {
            r2Fail("ERROR AL ESCRIBIR");
          }
        } else if (r2TestInFlight && (long)(now - r2TestSentMs) > (long)R2_TEST_TIMEOUT) {
          r2Fail("EL R2 NO RESPONDE");
        }
      } else if (uiState != UiState::TdsMeasure && (long)(now - r2LastUseMs) > (long)R2_IDLE_MS) {
        Serial.println("[R2] Sin uso: desconectando.");
        r2Disconnect();
        r2SetState(R2State::Off);
      }
      break;
    }
  }
#endif
}
