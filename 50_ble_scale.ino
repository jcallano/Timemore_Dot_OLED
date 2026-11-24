// ============================================================================
// BLUETOOTH: TIMEMORE DOT
// Tramas, tara, cola de muestras, vinculación, escaneo, conexión no bloqueante y pila NimBLE.
// Pestaña del sketch: el IDE la concatena tras basicoledtimemore_dot.ino (orden alfabético).
// Tipos, constantes y variables globales compartidas están en la pestaña principal.
// ============================================================================

// ============================================================================
// DECODIFICACIÓN Y COMANDOS DE LA TIMEMORE BLACK MIRROR DOT
// ============================================================================
// Envío de una trama Open Scale a la Dot (construcción y CRC en src/timemore_protocol.*)
bool sendTimemoreOpenScaleFrame(uint8_t opcode, uint8_t cmd, const uint8_t* payload, uint16_t payloadLen) {
  if (!scaleConnected || pScaleWriteChar == nullptr) return false;
  uint8_t frame[48];
  size_t n = tmproto::buildFrame(opcode, cmd, payload, payloadLen, frame, sizeof(frame));
  if (n == 0) return false;
  return pScaleWriteChar->writeValue(frame, n, false);
}

void registerUserOrScaleActivity();

void executeTimemoreTare() {
  unsigned long nowMs     = millis();
  lastTareCommandMs       = nowMs;
  tareSettlingActive      = false;
  tareZeroConfirmed       = true;

  // POR QUÉ TARA 100% EN SOFTWARE EN EL ESP32-S3 (SIN ENVIAR Cmd = 0x0D A LA DOT):
  // 1. La Timemore Black Mirror Dot NO tiene pantalla física; la única pantalla es el OLED del ESP32-S3.
  // 2. Cuando el registro interno de peso de la Timemore Dot está por encima de 0.0 g (como al encenderla y
  //    poner encima la jarra/V60 de +50..+250 g), el firmware interno de la Dot desactiva su "Zero-Tracking Lock"
  //    y transmite continuamente cada cambio de 0.1 g a 10 Hz sin ningún retardo de despegue.
  // 3. En cambio, al enviarle Cmd = 0x0D (Tara por hardware), la Dot vuelve internamente a 0.0 g, activa su
  //    filtro amortiguado de bloqueo en cero (Zero-Lock) y detiene las notificaciones hasta acumular suficiente peso,
  //    causando lag apreciable después de hacer Tara.
  // 4. Con softwareTareOffset = rawWeightGrams en el ESP32-S3, la tara ocurre en 0.000 ms, no hay paquetes
  //    residuales pre-tara en vuelo, y la Timemore Dot permanece en su modo activo de respuesta instantánea.
  softwareTareOffset      = rawWeightGrams;
  tareZeroAmbiguous       = false;
  currentWeightGrams      = 0.0f;
  prevActivityWeightGrams = 0.0f;
  flowEngine.reset(0.0f);
  prevFlowCalcMs          = nowMs;
  autoTimerArmed          = autoTimerEnabled; // Rearmar Auto-Timer para la siguiente extracción (si está activo en Menú)
  registerUserOrScaleActivity();
  triggerLedFlash();

  Serial.printf("[TARA] Tara instantanea por software aplicada (offset=%.1fg -> 0.0g, sin Zero-Lock en la Dot) + Auto-Timer %s.\n",
                softwareTareOffset, autoTimerArmed ? "ARMADO" : "OFF");
}

void sendTimemoreKeepAlive() {
  if (!scaleConnected || pScaleWriteChar == nullptr) return;

  if (pScaleWriteChar->getUUID() == NimBLEUUID(UUID16_TIMEMORE_CHR_2A6E)) {
    const uint8_t keepAliveCmd[2] = { 0x20, 0x00 };
    pScaleWriteChar->writeValue(keepAliveCmd, sizeof(keepAliveCmd), false);
  } else {
    // Solicitud ligera no bloqueante de batería (Opcode 0x02, Cmd 0x05)
    sendTimemoreOpenScaleFrame(0x02, 0x05, nullptr, 0);
  }
  lastKeepAliveSentMs = millis();
}

// Sonda de la cola de salida de la Dot (comando serie 'q'): envía el comando ligero 02 08 y mide cuánto
// tarda su respuesta. Ese tiempo ES el tamaño de la cola, sin tocar la báscula. Con MTU grande: ~0.1 s
// siempre; con MTU 23 crece 0.1 s por cada trama extra y no baja.
void sendQueueProbe() {
  if (!scaleConnected || pScaleWriteChar == nullptr ||
      pScaleWriteChar->getUUID() == NimBLEUUID(UUID16_TIMEMORE_CHR_2A6E)) {
    Serial.println("[COLA] Sin enlace Open Scale con la Dot: sonda no enviada.");
    return;
  }
  queueProbeResultReady = false;
  queueProbeSentMs      = millis();
  if (!sendTimemoreOpenScaleFrame(0x02, 0x08, nullptr, 0)) {
    queueProbeSentMs = 0;
    Serial.println("[COLA] Error al enviar la sonda 02 08.");
  }
}

// ============================================================================
// CALLBACKS BLE CENTRAL (TIMEMORE DOT - TIEMPO REAL CERO LATENCIA)
// ============================================================================

// Vacía la cola de muestras BLE en el contexto de loop() (dueño único del estado de peso y caudal)
void drainWeightQueue() {
  if (weightQueue == nullptr) return;
  WeightSample sample;
  while (xQueueReceive(weightQueue, &sample, 0) == pdTRUE) {
    processWeightSample(sample);
  }
}

void timemoreNotifyCallback(NimBLERemoteCharacteristic* pRemoteCharacteristic,
                            uint8_t* pData, size_t length, bool isNotify) {
  (void)pRemoteCharacteristic;
  (void)isNotify;

  packetsRxCount = packetsRxCount + 1;
  tmproto::ParseResult pr = tmproto::parsePayload(pData, length);

  // Diagnóstico de la cola de la Dot: paquetes con varias tramas y respuesta a la sonda 02 08
  if (pr.frameCount > 0) {
    openScalePackets = openScalePackets + 1;
    if (pr.frameCount > 1) multiFramePackets = multiFramePackets + 1;
  }
  if (pr.sawProbeReply && queueProbeSentMs != 0) {
    queueProbeLastMs      = (uint32_t)(millis() - queueProbeSentMs);
    queueProbeSentMs      = 0;
    queueProbeResultReady = true;   // loop() lo imprime (no imprimir desde la tarea de NimBLE)
  }

  // Prueba de que el periférico es una Timemore: peso o cualquier trama Open Scale válida (p. ej. la
  // respuesta de batería que se pide al conectar, útil si la báscula está en reposo a 0.0 g).
  if (pr.hasWeight || pr.sawOpenScaleFrame) {
    weightPacketsRx = weightPacketsRx + 1;
  }
  if (pr.hasNativeFlow) {
    scaleNativeFlowGps = pr.nativeFlowGps;
  }

  // Batería de la báscula (escrituras simples de un byte: seguras entre núcleos)
  if (pr.hasBattery && pr.batteryPct <= 100) {
    if (pr.batteryBarsValid) scaleBatteryBars = pr.batteryBars;
    if (pr.batteryPct != scaleBatteryPct || !scaleBatteryValid) {
      scaleBatteryPct        = pr.batteryPct;
      scaleBatteryValid      = true;
      uiNeedsImmediateRedraw = true;
    }
  }

  // El callback corre en la tarea host de NimBLE (núcleo 0) y loop() en el núcleo 1. Antes el motor de
  // caudal, la tara y el auto-timer modificaban el mismo estado desde ambos núcleos sin sincronización.
  // Ahora el callback SOLO parsea y encola la muestra con su marca de tiempo; loop() la procesa.
  if (pr.hasWeight) {
    // Puente BooKoo: la trama a la máquina sale ya desde la tarea bookooTx, sin pasar por loop()
    if (bookooBridgeEnabled) bookooOnScaleSample(pr.grams);

    WeightSample sample;
    sample.ms     = millis();
    sample.grams  = pr.grams;
    sample.stable = pr.stable;
    if (weightQueue == nullptr || xQueueSend(weightQueue, &sample, 0) != pdTRUE) {
      droppedWeightSamples = droppedWeightSamples + 1;
    }
  }
}

// Procesa una muestra de peso (llamada SOLO desde loop(), ver drainWeightQueue()).
void processWeightSample(const WeightSample& sample) {
  const float parsedWeight = sample.grams;
  {
    unsigned long nowMs = sample.ms;

    // El peso bruto cae de golpe a 0.0 g con una tara por software activa. Dos causas posibles:
    //  a) Se LEVANTÓ todo de la báscula (remover el V60, servir): al reponerlo vuelve de golpe
    //     cerca del offset -> la tara debe CONSERVARSE.
    //  b) Se pulsó el botón de Tara físico de la Dot: el peso seguirá SUBIENDO POCO A POCO desde 0
    //     -> la tara por software debe ANULARSE.
    // Antes se anulaba siempre en el acto, y levantar la jarra rompía el peso y el ratio en pleno vertido.
    if (fabsf(parsedWeight) <= 0.10f && fabsf(softwareTareOffset) > 2.0f &&
        fabsf(parsedWeight - rawWeightGrams) > 2.0f) {
      tareZeroAmbiguous = true;
    } else if (tareZeroAmbiguous && parsedWeight > 0.3f) {
      tareZeroAmbiguous = false;
      if (parsedWeight < 0.5f * softwareTareOffset) {
        // Subida gradual desde 0 -> fue la Tara física de la Dot
        softwareTareOffset = 0.0f;
        flowEngine.reset(0.0f);
        prevFlowCalcMs = nowMs;
        Serial.println("[TARA] Tara fisica de la Dot detectada -> tara por software anulada.");
      }
      // Si no, se repuso lo que se había levantado: la tara por software sigue vigente.
    }

    float newNetWeight      = parsedWeight - softwareTareOffset;
    float weightChanged     = fabsf(newNetWeight - currentWeightGrams);
    float prevDisplayedFlow = displayedFlowRateGps;

    // Motor de caudal (src/flow_engine.*): ventana ~350 ms + mediana de 3 + EMA adaptativa + corte
    // por marca de agua. El pico solo se registra con el cronómetro en marcha.
    flowEngine.addSample(nowMs, newNetWeight, timerRunning);

    rawWeightGrams     = parsedWeight;
    currentWeightGrams = newNetWeight;
    lastBleWeightRxMs  = nowMs;

    // Redibujar OLED en el acto cuando cambia el peso (>= 0.05 g) o cuando cambia el flow con histéresis
    if (weightChanged >= 0.05f || fabsf(displayedFlowRateGps - prevDisplayedFlow) >= 0.15f) {
      uiNeedsImmediateRedraw = true;
    }
  }
}

class ScaleClientCallbacks : public NimBLEClientCallbacks {
#if defined(NIMBLE_V2)
  void onConnect(NimBLEClient* pClient) override {
    float itvlMs = 0.0f;
    if (pClient != nullptr) {
      itvlMs = pClient->getConnInfo().getConnInterval() * 1.25f;
      // Solicitar intervalo nativo rápido soportado por la Timemore Dot: 10..15 ms (8..12 * 1.25ms, slave_latency = 0)
      pClient->updateConnParams(8, 12, 0, 400);
    }
    Serial.printf("[BLE] Conectado fisicamente a la Timemore Dot (intervalo inicial %.1f ms; pidiendo 10-15 ms).\n", itvlMs);
    bleConnectPhase = BLE_LINK_UP; // loop() completa descubrimiento y suscripción
  }
  void onConnectFail(NimBLEClient* pClient, int reason) override {
    (void)pClient;
    Serial.printf("[BLE] Fallo al conectar (razon=%d / 0x%03X).\n", reason, reason);
    bleConnectFailReason = reason;
    bleConnectPhase      = BLE_LINK_FAILED;
  }
  bool onConnParamsUpdateRequest(NimBLEClient* pClient, const ble_gap_upd_params* params) override {
    if (params != nullptr) {
      connParamReqCount = connParamReqCount + 1;
      lastConnReqMin    = params->itvl_min;
      lastConnReqMax    = params->itvl_max;
      lastConnReqLat    = params->latency;
      Serial.printf("[BLE] La Dot pide intervalo %u..%u (x1.25 ms), latencia %u -> %s\n",
                    params->itvl_min, params->itvl_max, params->latency,
                    (params->itvl_max > 16 || params->latency > 0) ? "RECHAZADO" : "aceptado");
      // CRÍTICO CONTRA LA DEGRADACIÓN TRAS EL POWER-UP:
      // Unos ~5 segundos después de conectar, el chip Phyplus PHY6222 de la Timemore Dot solicita
      // subir el intervalo BLE a 48 (60.0 ms) para ahorrar energía. Si se acepta, el enlace pasa de
      // 15.0 ms a 60.0 ms (4 veces más lento). Rechazamos cualquier intervalo > 20 ms (16) o latency > 0
      // y reafirmamos 8..12 (10.0..15.0 ms, que es el mínimo soportado por la Dot sin error L2CAP).
      if (params->itvl_max > 16 || params->latency > 0) {
        if (pClient != nullptr) {
          pClient->updateConnParams(8, 12, 0, 400);
        }
        return false;
      }
    }
    return true;
  }
  void onMTUChange(NimBLEClient* pClient, uint16_t mtu) override {
    (void)pClient;
    scaleLinkMtu = mtu;   // loop() lo informa al comprobar el intercambio
  }
  void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
    Serial.printf("[BLE] SMP Bonding completado (encrypted=%d, bonded=%d).\n",
                  connInfo.isEncrypted() ? 1 : 0, connInfo.isBonded() ? 1 : 0);
  }
  void onDisconnect(NimBLEClient* pClient, int reason) override {
    (void)pClient;
    scaleConnected       = false;
    bleConnectPhase      = BLE_IDLE;
    pScaleNotifyChar     = nullptr;
    pScaleWriteChar      = nullptr;
    candidateFound       = false;
    doConnectToScale     = false;
    bleDisconnectEvent   = true;   // loop() pone a cero el caudal (estado de loop, no de esta tarea)
    lastScanAttemptMs    = 0;
    Serial.printf("[BLE] Timemore Dot desconectada (razon=%d). Buscando reconexion inmediata...\n", reason);
  }
#else
  void onConnect(NimBLEClient* pClient) override {
    (void)pClient;
    Serial.println("[BLE] Conectado fisicamente a la Timemore Dot.");
  }
  void onDisconnect(NimBLEClient* pClient) override {
    (void)pClient;
    scaleConnected      = false;
    bleConnectPhase     = BLE_IDLE;
    pScaleNotifyChar    = nullptr;
    pScaleWriteChar     = nullptr;
    candidateFound      = false;
    doConnectToScale    = false;
    bleDisconnectEvent  = true;
    lastScanAttemptMs   = 0;
    Serial.println("[BLE] Timemore Dot desconectada.");
  }
#endif
};

static ScaleClientCallbacks scaleClientCallbacks;

// Lista corta de MACs rechazadas en esta sesión (conectaron pero no eran una Timemore):
// evita reconectar en bucle cada 1.5 s al mismo dispositivo ajeno.
static constexpr uint8_t MAX_REJECTED_MACS = 4;
char rejectedMacs[MAX_REJECTED_MACS][20] = {{0}};
uint8_t rejectedMacsHead = 0;

bool isMacRejected(const char* mac) {
  for (uint8_t i = 0; i < MAX_REJECTED_MACS; i++) {
    if (rejectedMacs[i][0] != '\0' && strcasecmp(rejectedMacs[i], mac) == 0) return true;
  }
  return false;
}

void rejectMac(const char* mac) {
  if (mac == nullptr || mac[0] == '\0' || isMacRejected(mac)) return;
  strncpy(rejectedMacs[rejectedMacsHead], mac, sizeof(rejectedMacs[0]) - 1);
  rejectedMacs[rejectedMacsHead][sizeof(rejectedMacs[0]) - 1] = '\0';
  rejectedMacsHead = (rejectedMacsHead + 1) % MAX_REJECTED_MACS;
  Serial.printf("[SCAN] %s descartada para esta sesion (no es una Timemore).\n", mac);
}

bool isSavedScaleMac(const char* mac) {
  return hasSavedMac && savedTimemoreMac[0] != '\0' && mac != nullptr &&
         strcasecmp(savedTimemoreMac, mac) == 0;
}

bool isTimemoreAdvertisedDevice(const NimBLEAdvertisedDevice* dev) {
  if (dev == nullptr) return false;

  // 1. Con una báscula ya vinculada (MAC en NVS o la MAC por defecto) SOLO se acepta esa MAC.
  //    Antes se aceptaba cualquier dispositivo con "DOT" en el nombre o con el servicio 0x181A
  //    (p. ej. termómetros Xiaomi con firmware pvvx/ATC) y su MAC sustituía a la buena en NVS.
  if (hasSavedMac && savedTimemoreMac[0] != '\0') {
    return isSavedScaleMac(dev->getAddress().toString().c_str());
  }

  // 2. Modo vinculación (tras "Re-vincular"): exigir una firma inequívoca de Timemore.
  //    Servicios propios de Timemore (0xFFF0 Open Scale, 0xFFE0 UART) anunciados o en Service Data.
  //    0x181A / 0x181D son servicios estándar del Bluetooth SIG y NO bastan por sí solos.
  if (dev->isAdvertisingService(NimBLEUUID(UUID16_TIMEMORE_SVC_FFF0)) ||
      dev->isAdvertisingService(NimBLEUUID(UUID16_TIMEMORE_SVC_FFE0))) {
    return true;
  }
  if (dev->haveServiceData()) {
    uint8_t svcDataCount = dev->getServiceDataCount();
    for (uint8_t i = 0; i < svcDataCount; i++) {
      NimBLEUUID sdUuid = dev->getServiceDataUUID(i);
      if (sdUuid == NimBLEUUID(UUID16_TIMEMORE_SVC_FFF0) ||
          sdUuid == NimBLEUUID(UUID16_TIMEMORE_SVC_FFE0)) {
        return true;
      }
    }
  }

  // 3. Nombre: solo marcas/modelos de Timemore (sin la subcadena genérica "DOT", que coincide
  //    con Echo Dot y similares).
  if (dev->haveName()) {
    String upperName = String(dev->getName().c_str());
    upperName.toUpperCase();
    if (upperName.indexOf("TIMEMORE") >= 0 ||
        upperName.indexOf("MACARON") >= 0 ||
        upperName.indexOf("BLACK MIRROR") >= 0) {
      return true;
    }
  }

  return false;
}

void handleScannedDevice(const NimBLEAdvertisedDevice* dev) {
  if (!dev || scaleConnected || doConnectToScale) return;
  if (!isTimemoreAdvertisedDevice(dev)) return;
  if (!dev->isConnectable()) return;
  if (isMacRejected(dev->getAddress().toString().c_str())) return;

  std::string macStr = dev->getAddress().toString();
  uint8_t addrType   = dev->getAddress().getType();
  int rssi           = dev->getRSSI();

  strncpy(candidateMac, macStr.c_str(), sizeof(candidateMac) - 1);
  candidateMac[sizeof(candidateMac) - 1] = '\0';
  candidateAddrType = addrType;
  candidateRSSI     = rssi;
  candidateFound    = true;
  doConnectToScale  = true;
  pendingStopScan   = true;
  Serial.printf("[SCAN] Timemore Dot detectada: MAC=%s (tipo=%u, RSSI=%d dBm)\n",
                candidateMac, candidateAddrType, candidateRSSI);
}

#if defined(NIMBLE_V2)
class ScaleScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override {
    handleScannedDevice(advertisedDevice);
  }
  void onScanEnd(const NimBLEScanResults& results, int reason) override {
    (void)results; (void)reason;
    isScanning = false;
    lastScanAttemptMs = millis();
    if (candidateFound && !scaleConnected) {
      doConnectToScale = true;
    }
  }
};
#else
class ScaleScanCallbacks : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice* advertisedDevice) override {
    handleScannedDevice(advertisedDevice);
  }
};
#endif

static ScaleScanCallbacks scaleScanCallbacks;

void startTimemoreScan() {
  if (scaleConnected || doConnectToScale || bleConnectPhase != BLE_IDLE) return;
  NimBLEScan* pScan = NimBLEDevice::getScan();
  if (pScan->isScanning()) {
    isScanning = true;
    return;
  }

  pScan->clearResults();
  candidateFound    = false;
  pendingStopScan   = false;
  isScanning        = true;
  lastScanAttemptMs = millis();

#if defined(NIMBLE_V2)
  pScan->setScanCallbacks(&scaleScanCallbacks, true);
  pScan->setScanResponseTimeout(150);
  pScan->setActiveScan(true);
  pScan->setInterval(85);
  pScan->setWindow(55);
  if (!pScan->start(SCAN_DURATION_SEC * 1000, false, true)) {
    isScanning = false;
  }
#else
  pScan->setAdvertisedDeviceCallbacks(&scaleScanCallbacks, true);
  pScan->setActiveScan(true);
  pScan->setInterval(85);
  pScan->setWindow(55);
  if (!pScan->start(SCAN_DURATION_SEC, nullptr, false)) {
    isScanning = false;
  }
#endif
}

bool discoverAndSubscribeTimemore(NimBLEClient* pClient) {
  pScaleNotifyChar = nullptr;
  pScaleWriteChar  = nullptr;

  // 1. Prioridad máxima: Servicio oficial Timemore Black Mirror Dot (TES017) Open Scale Protocol 0xFFF0 -> 0xFFF1 (Notify) / 0xFFF2 (Write)
  NimBLERemoteService* pSvc = pClient->getService(NimBLEUUID(UUID16_TIMEMORE_SVC_FFF0));
  if (pSvc != nullptr) {
    Serial.println("[BLE] Servicio Timemore Open Scale 0xFFF0 descubierto.");
    pScaleNotifyChar = pSvc->getCharacteristic(NimBLEUUID(UUID16_TIMEMORE_CHR_FFF1));
    pScaleWriteChar  = pSvc->getCharacteristic(NimBLEUUID(UUID16_TIMEMORE_CHR_FFF2));
  }

  // 2. Fallback Servicio Timemore 0xFFE0 -> Char 0xFFE1 / 0xFFE2
  if (pScaleNotifyChar == nullptr) {
    pSvc = pClient->getService(NimBLEUUID(UUID16_TIMEMORE_SVC_FFE0));
    if (pSvc != nullptr) {
      Serial.println("[BLE] Servicio Timemore 0xFFE0 descubierto.");
      pScaleNotifyChar = pSvc->getCharacteristic(NimBLEUUID(UUID16_TIMEMORE_CHR_FFE1));
      pScaleWriteChar  = pSvc->getCharacteristic(NimBLEUUID(UUID16_TIMEMORE_CHR_FFE2));
      if (pScaleWriteChar == nullptr && pScaleNotifyChar != nullptr &&
          (pScaleNotifyChar->canWrite() || pScaleNotifyChar->canWriteNoResponse())) {
        pScaleWriteChar = pScaleNotifyChar;
      }
    }
  }

  // 3. Fallback Servicio Weight Scale 0x181D -> 0x2A9D
  if (pScaleNotifyChar == nullptr) {
    pSvc = pClient->getService(NimBLEUUID(UUID16_TIMEMORE_SVC_181D));
    if (pSvc != nullptr) {
      Serial.println("[BLE] Servicio Weight Scale 0x181D descubierto.");
      pScaleNotifyChar = pSvc->getCharacteristic(NimBLEUUID(UUID16_TIMEMORE_CHR_2A9D));
      pScaleWriteChar  = pScaleNotifyChar;
    }
  }

  // 4. Fallback Servicio 0x181A -> 0x2A6D (Notify) / 0x2A6E (Write)
  if (pScaleNotifyChar == nullptr) {
    pSvc = pClient->getService(NimBLEUUID(UUID16_TIMEMORE_SVC_181A));
    if (pSvc != nullptr) {
      Serial.println("[BLE] Servicio Timemore 0x181A descubierto.");
      pScaleNotifyChar = pSvc->getCharacteristic(NimBLEUUID(UUID16_TIMEMORE_CHR_2A6D));
      pScaleWriteChar  = pSvc->getCharacteristic(NimBLEUUID(UUID16_TIMEMORE_CHR_2A6E));
    }
  }

  // Leer batería estándar 0x180F -> 0x2A19 solo si el servicio no es 0xFFF0 (en 0xFFF0 la batería va por Cmd 0x05)
  if (pScaleWriteChar == nullptr || pScaleWriteChar->getUUID() != NimBLEUUID(UUID16_TIMEMORE_CHR_FFF2)) {
    NimBLERemoteService* pBattSvc = pClient->getService(NimBLEUUID((uint16_t)0x180F));
    if (pBattSvc != nullptr) {
      NimBLERemoteCharacteristic* pBattChar = pBattSvc->getCharacteristic(NimBLEUUID((uint16_t)0x2A19));
      if (pBattChar != nullptr && pBattChar->canRead()) {
        uint8_t b = pBattChar->readValue<uint8_t>();
        if (b <= 100) scaleBatteryPct = b;
      }
    }
  }

  // 5. Descubrimiento completo de atributos GATT en caso de UUID alternativo.
  //    Solo para la báscula ya vinculada: con un dispositivo nuevo, aceptar "cualquier característica
  //    notify" es lo que permitía vincular por error termómetros u otros periféricos BLE.
  if (pScaleNotifyChar == nullptr && isSavedScaleMac(candidateMac)) {
    pClient->discoverAttributes();
    const auto& services = pClient->getServices(true);
    for (auto* s : services) {
      if (s == nullptr) continue;
      NimBLEUUID sUuid = s->getUUID();
      if (sUuid == NimBLEUUID((uint16_t)0x1800) ||
          sUuid == NimBLEUUID((uint16_t)0x1801) ||
          sUuid == NimBLEUUID((uint16_t)0x180A) ||
          sUuid == NimBLEUUID((uint16_t)0x180F)) {
        continue;
      }
      const auto& chars = s->getCharacteristics(true);
      for (auto* c : chars) {
        if (c == nullptr) continue;
        if (pScaleNotifyChar == nullptr && (c->canNotify() || c->canIndicate())) {
          pScaleNotifyChar = c;
          Serial.printf("[BLE] Caracteristica Notify descubierta dinamicamente: %s (Svc %s)\n",
                        c->getUUID().toString().c_str(), sUuid.toString().c_str());
        }
        if (pScaleWriteChar == nullptr && (c->canWrite() || c->canWriteNoResponse())) {
          pScaleWriteChar = c;
        }
      }
    }
  }

  if (pScaleNotifyChar == nullptr) {
    Serial.println("[BLE] ERROR: No se encontraron las caracteristicas de peso (0xFFE1 / 0xFFF1 / 0x2A9D / 0x2A6D).");
    return false;
  }

  if (pScaleNotifyChar->canNotify()) {
    if (!pScaleNotifyChar->subscribe(true, timemoreNotifyCallback)) {
      Serial.println("[BLE] ERROR: Fallo al suscribir notificaciones CCCD.");
      return false;
    }
    Serial.printf("[BLE] Suscrito exitosamente a notificaciones en %s.\n",
                  pScaleNotifyChar->getUUID().toString().c_str());
  } else if (pScaleNotifyChar->canIndicate()) {
    if (!pScaleNotifyChar->subscribe(false, timemoreNotifyCallback)) {
      Serial.println("[BLE] ERROR: Fallo al suscribir indicaciones.");
      return false;
    }
  }

  return true;
}

// ----------------------------------------------------------------------------
// CONEXIÓN NO BLOQUEANTE (máquina de estados)
// Antes connectToTimemoreScale() bloqueaba loop() hasta ~7 s (connect síncrono de 4.5 s + reintento de
// 2 s + delays): encoder, botones y OLED quedaban congelados. Ahora:
//   connectToTimemoreScale()  -> lanza connect() asíncrono y vuelve al instante (BLE_CONNECTING)
//   onConnect / onConnectFail -> (tarea NimBLE) solo cambian bleConnectPhase
//   serviceBleConnection()    -> (loop) reintenta con el otro tipo de dirección o completa el
//                                descubrimiento GATT y la suscripción (finishScaleConnection)
// ----------------------------------------------------------------------------
bool beginScaleConnection(uint8_t addrType, uint32_t timeoutMs) {
  if (pScaleClient == nullptr) {
    pScaleClient = NimBLEDevice::createClient();
    pScaleClient->setClientCallbacks(&scaleClientCallbacks, false);
    // Intervalo nativo óptimo de baja latencia (8..12 * 1.25ms = 10ms..15ms, slave_latency = 0)
    pScaleClient->setConnectionParams(8, 12, 0, 400, 16, 16);
#if defined(NIMBLE_V2) && defined(CONFIG_BT_NIMBLE_EXT_ADV)
    pScaleClient->setConnectPhy(BLE_GAP_LE_PHY_1M_MASK);
#endif
  }

  if (pScaleClient->isConnected()) {
    pScaleClient->disconnect();
  }

  bleConnectAddrType = addrType;
  NimBLEAddress addr(std::string(candidateMac), addrType);
  Serial.printf("[BLE] Conectando a Timemore Dot [%s] (tipo=%u)...\n", candidateMac, addrType);
  bleConnectStartMs = millis();
  bleConnectPhase   = BLE_CONNECTING;

#if defined(NIMBLE_V2)
  pScaleClient->setConnectTimeout(timeoutMs);
  if (!pScaleClient->connect(addr, true, true /* asíncrono */, false)) {
    bleConnectFailReason = pScaleClient->getLastError();
    bleConnectPhase      = BLE_LINK_FAILED;
    return false;
  }
#else
  pScaleClient->setConnectTimeout((timeoutMs + 999) / 1000);
  bool ok = pScaleClient->connect(addr, true); // NimBLE 1.x: síncrono
  if (ok) {
    bleConnectPhase = BLE_LINK_UP;
  } else {
    bleConnectFailReason = pScaleClient->getLastError();
    bleConnectPhase      = BLE_LINK_FAILED;
    return false;
  }
#endif
  return true;
}

// Inicia la conexión con la candidata encontrada en el escaneo (no bloquea con NimBLE 2.x)
bool connectToTimemoreScale() {
  if (!candidateFound || strlen(candidateMac) < 17) return false;

  pendingStopScan = false;
  NimBLEScan* pScan = NimBLEDevice::getScan();
  if (pScan->isScanning()) {
    pScan->stop();
  }
  isScanning = false;
  bleConnectTriedAlt = false;
  return beginScaleConnection(candidateAddrType, 4500);
}

// Avanza la máquina de estados de conexión (llamada en cada vuelta de loop())
void serviceBleConnection() {
  uint8_t phase = bleConnectPhase;

  // Red de seguridad por si NimBLE no notificara el fin del intento
  if (phase == BLE_CONNECTING && (uint32_t)(millis() - bleConnectStartMs) > 8000UL) {
#if defined(NIMBLE_V2)
    if (pScaleClient != nullptr) pScaleClient->cancelConnect();
#endif
    bleConnectFailReason = -1;
    phase = BLE_LINK_FAILED;
    bleConnectPhase = phase;
  }

  if (phase == BLE_LINK_FAILED) {
    if (!bleConnectTriedAlt) {
      // Reintento con el otro tipo de dirección (pública <-> aleatoria)
      bleConnectTriedAlt = true;
      beginScaleConnection(static_cast<uint8_t>(candidateAddrType == 0 ? 1 : 0), 2000);
      return;
    }
    Serial.printf("[BLE] Conexion no establecida (razon=%d).\n", bleConnectFailReason);
    bleConnectPhase   = BLE_IDLE;
    lastScanAttemptMs = millis();
    return;
  }

  if (phase == BLE_LINK_UP) {
    bleConnectPhase = BLE_IDLE;
    finishScaleConnection(bleConnectAddrType);
    lastScanAttemptMs = millis();
  }
}

// Enlace establecido: descubrir servicios, suscribirse y dejar la báscula operativa
bool finishScaleConnection(uint8_t connectedAddrType) {
  if (pScaleClient == nullptr || !pScaleClient->isConnected()) return false;

  if (!discoverAndSubscribeTimemore(pScaleClient)) {
    pScaleClient->disconnect();
    if (!isSavedScaleMac(candidateMac)) {
      rejectMac(candidateMac);
    }
    return false;
  }

  scaleConnected          = true;
  softwareTareOffset      = 0.0f;
  prevActivityWeightGrams = currentWeightGrams;
  lastKeepAliveMs         = millis();
  registerUserOrScaleActivity();

  // La MAC se guarda en NVS solo cuando llegue la primera trama de peso válida (ver loop()),
  // no al suscribirse: así un periférico ajeno que acepte la conexión nunca sustituye a la báscula.
  weightPacketsRx = 0;
  scaleConnectedAtMs = millis();
  strncpy(pendingSaveMac, candidateMac, sizeof(pendingSaveMac) - 1);
  pendingSaveMac[sizeof(pendingSaveMac) - 1] = '\0';
  pendingSaveAddrType = connectedAddrType;
  pendingScaleSave    = true;

  // Lectura inicial de batería (Opcode 0x02, Cmd 0x05) 150 ms después de suscribir el CCCD, con un
  // segundo intento a los 1.2 s si no hubo respuesta. Programado en loop(), sin delay().
  scaleBatteryValid       = false;
  batteryQueryTries       = 0;
  connItvlFixAttempts     = 0;
  lastConnItvlCheckMs     = millis();
  pendingBatteryQueryAtMs = millis() + 150UL;
  lastKeepAliveSentMs     = millis();

  // Intercambio de MTU DESPUÉS de suscribirse (no en connect(): ahí provocaba el error 0x3E/574 en
  // los primeros eventos de conexión). Sin él la cola de la Dot crece hasta ~2.4 s de retraso.
  scaleLinkMtu            = 23;
  mtuExchangeTries        = 0;
  multiFramePackets       = 0;
  openScalePackets        = 0;
  queueProbeSentMs        = 0;
  pendingMtuExchangeAtMs  = millis() + MTU_EXCHANGE_DELAY_MS;

  Serial.printf("[BLE] >>> CONEXION EXITOSA con Timemore Dot (%s, tipo=%u) <<<\n",
                candidateMac, connectedAddrType);
  return true;
}


// Inicializa la pila NimBLE (arranque y al despertar del reposo, que la apaga por completo)
static bool bleStackUp = false;

void initBleStack() {
  if (bleStackUp) return;
  bleStackUp = true;
  // Con el puente activo el nombre GAP (0x2A00) también es "BOOKOO_SC_U XXXXXX"
  bookooPrepareName();
  NimBLEDevice::init(bookooBridgeEnabled ? bookooAdvName : "TMDot-OLED");
  // MTU preferido grande: la Dot acepta 247 y así junta varias tramas por notificación (vacía su cola)
  NimBLEDevice::setMTU(DOT_PREFERRED_MTU);
  NimBLEDevice::setSecurityAuth(true, false, false);
#if defined(BLE_HS_IO_NO_INPUT_OUTPUT)
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
#endif
#if defined(BLE_SM_PAIR_KEY_DIST_ENC) && defined(BLE_SM_PAIR_KEY_DIST_ID)
  NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
  NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
#endif
  // +3 dBm basta para una báscula a menos de 1 m (antes +9 dBm, el máximo): menos consumo y menos
  // interferencia con el Wi-Fi de la cocina.
  NimBLEDevice::setPower(ESP_PWR_LVL_P3);

  // Puente BooKoo: servidor GATT 0x0FFE + anuncio (antes de cualquier conexión)
  setupBookooServer();
}

// Detiene escaneo, desconecta la báscula y desinicializa NimBLE para que la radio no impida dormir
void shutdownBleStack() {
  if (!bleStackUp) return; // p. ej. corte por batería en setup(), antes de iniciar BLE
  bleStackUp = false;
  teardownBookooServer();
  NimBLEScan* pScan = NimBLEDevice::getScan();
  if (pScan != nullptr && pScan->isScanning()) {
    pScan->stop();
  }
  if (scaleConnected && pScaleClient != nullptr) {
    pScaleClient->disconnect();
    delay(50);
  }
  scaleConnected   = false;
  isScanning       = false;
  doConnectToScale = false;
  pendingStopScan  = false;
  candidateFound   = false;
  pendingScaleSave = false;
  pScaleNotifyChar = nullptr;
  pScaleWriteChar  = nullptr;
  NimBLEDevice::deinit(true); // borra también el cliente
  pScaleClient     = nullptr;
  bleConnectPhase  = BLE_IDLE;
  if (weightQueue != nullptr) xQueueReset(weightQueue);
}
