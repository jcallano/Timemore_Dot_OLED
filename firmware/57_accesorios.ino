// ============================================================================
// ACCESORIOS BLE: SONDA DE pH Y TEMPERATURA (y los que vengan)
// Los accesorios emiten sus lecturas en el anuncio BLE (src/accessory_protocol.*, docs/ACCESORIOS_BLE.md);
// el display solo escucha, sin conectarse.
//   - Sin la Dot conectada, el escaneo periódico que la busca también recoge los anuncios.
//   - Con la Dot conectada, en modo fermento, un escaneo pasivo de bajo ciclo (32 ms cada 160 ms) los
//     sigue recogiendo. Fuera de ese modo no se escanea: no compite con la Dot ni con el puente.
// Pestaña del sketch: el IDE la concatena tras firmware.ino (orden alfabético).
// ============================================================================

static constexpr uint32_t PROBE_FRESH_MS      = 15000;  // una lectura más vieja ya no se muestra ni se guarda
static constexpr uint32_t ACC_SCAN_MS         = 30000;  // duración de cada escaneo (se encadenan)
static constexpr uint16_t ACC_SCAN_INTERVAL   = 160;
static constexpr uint16_t ACC_SCAN_WINDOW     = 32;

static portMUX_TYPE probeMux        = portMUX_INITIALIZER_UNLOCKED;
static accproto::Reading probeLast;
static unsigned long probeRxMs      = 0;
static bool probeHave               = false;
static int probeRssi                = 0;
static char probeMac[18]            = "";
static bool accScanActive           = false;

// Llamada desde el callback del escaneo (tarea de NimBLE) con cada anuncio
void handleAccessoryAdvert(const NimBLEAdvertisedDevice* dev) {
  if (dev == nullptr || !dev->haveManufacturerData()) return;
  const std::string md = dev->getManufacturerData();
  accproto::Reading r;
  if (!accproto::parseAdvert(reinterpret_cast<const uint8_t*>(md.data()), md.size(), r)) return;
  if (r.type != accproto::TYPE_PH_TEMP) return;
  const std::string mac = dev->getAddress().toString();
  const int rssi = dev->getRSSI();
  portENTER_CRITICAL(&probeMux);
  probeLast = r;
  probeRxMs = millis();
  probeHave = true;
  probeRssi = rssi;
  strncpy(probeMac, mac.c_str(), sizeof(probeMac) - 1);
  probeMac[sizeof(probeMac) - 1] = '\0';
  portEXIT_CRITICAL(&probeMux);
}

// Última lectura de la sonda si tiene menos de PROBE_FRESH_MS
bool probeGetFresh(accproto::Reading& r, uint32_t* ageMs) {
  portENTER_CRITICAL(&probeMux);
  const bool have = probeHave;
  const unsigned long rx = probeRxMs;
  r = probeLast;
  portEXIT_CRITICAL(&probeMux);
  if (!have) return false;
  const uint32_t age = static_cast<uint32_t>(millis() - rx);
  if (ageMs != nullptr) *ageMs = age;
  return age < PROBE_FRESH_MS;
}

// Lectura simulada (orden serie 'f'): para probar la pantalla y los lotes sin la sonda
void probeInjectSimulated() {
  static uint8_t seq = 0;
  accproto::Reading r;
  r.type       = accproto::TYPE_PH_TEMP;
  r.seq        = ++seq;
  r.flags      = accproto::FLAG_TEMP_VALID | accproto::FLAG_PH_VALID | accproto::FLAG_PH_CAL | accproto::FLAG_STABLE;
  r.tempC      = 21.4f;
  r.ph         = 3.82f + 0.01f * static_cast<float>(seq % 5);
  r.phMv       = 180.0f;
  r.batteryPct = 87;
  r.calAgeDays = 3;
  portENTER_CRITICAL(&probeMux);
  probeLast = r;
  probeRxMs = millis();
  probeHave = true;
  probeRssi = -50;
  strncpy(probeMac, "simulada", sizeof(probeMac) - 1);
  portEXIT_CRITICAL(&probeMux);
  uiNeedsImmediateRedraw = true;
  Serial.printf("[SONDA] Lectura simulada: pH %.2f, %.1f C\n", r.ph, r.tempC);
}

// Escaneo de accesorios con la Dot conectada (llamada en cada vuelta de loop())
void serviceAccessoryScan() {
  NimBLEScan* pScan = NimBLEDevice::getScan();
  if (pScan == nullptr) return;
  const bool want = (brewMode == BrewMode::Fermento) && scaleConnected && bleConnectPhase == BLE_IDLE;

  if (accScanActive && !pScan->isScanning()) accScanActive = false;   // terminó solo
  if (!want) {
    if (accScanActive) {
      pScan->stop();
      accScanActive = false;
      isScanning    = false;
    }
    return;
  }
  if (accScanActive || pScan->isScanning()) return;

  pScan->clearResults();
#if defined(NIMBLE_V2)
  pScan->setScanCallbacks(&scaleScanCallbacks, true);   // 50_ble_scale.ino: reparte a la Dot y a los accesorios
  pScan->setActiveScan(false);
  pScan->setDuplicateFilter(0);   // cada anuncio cuenta: la lectura cambia con la secuencia
  pScan->setInterval(ACC_SCAN_INTERVAL);
  pScan->setWindow(ACC_SCAN_WINDOW);
  accScanActive = pScan->start(ACC_SCAN_MS, false, true);
#else
  pScan->setAdvertisedDeviceCallbacks(&scaleScanCallbacks, true);
  pScan->setActiveScan(false);
  pScan->setDuplicateFilter(false);
  pScan->setInterval(ACC_SCAN_INTERVAL);
  pScan->setWindow(ACC_SCAN_WINDOW);
  accScanActive = pScan->start(ACC_SCAN_MS / 1000, nullptr, false);
#endif
}
