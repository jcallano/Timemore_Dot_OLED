// ============================================================================
// ARRANQUE Y BUCLE PRINCIPAL
// setup() y loop().
// Pestaña del sketch: el IDE la concatena tras firmware.ino (orden alfabético).
// Tipos, constantes y variables globales compartidas están en la pestaña principal.
// ============================================================================

// ============================================================================
// SETUP Y LOOP PRINCIPAL
// ============================================================================
void setup() {
  Serial.begin(115200);
  pinMode(PIN_BOOT_BUTTON, INPUT_PULLUP);
  pinMode(PIN_TP4056_CHRG, INPUT_PULLUP);

  // Si venimos del apagado por batería baja (deep sleep con despertar EXT1), el GPIO 12 quedó en modo
  // RTC: devolverlo a GPIO digital antes de configurar el encoder.
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1) {
    rtc_gpio_deinit(static_cast<gpio_num_t>(PIN_ENC_SW));
  }

  // Inicializar Encoder Rotatorio usando el periférico PCNT por hardware del ESP32-S3 (GPIO 10, 11, 12)
  initHardwareEncoder();

  analogReadResolution(12);
  analogSetPinAttenuation(PIN_BATT_ADC, ADC_11db);

  lastActivityMs = millis();
  setRgbColor(0, 0, LED_BRIGHTNESS);

  delay(400);
  Serial.println("\n================================================================");
  Serial.println("  MONITOR OLED TIMEMORE DOT - WEACT STUDIO ESP32-S3-A");
  Serial.printf("  LED RGB WS2812B: GPIO %u | Boton BOOT: GPIO %u\n", WEACT_RGB_LED_PIN, PIN_BOOT_BUTTON);
  Serial.printf("  Encoder Rot.:    CLK=GPIO %d, DT=GPIO %d, SW=GPIO %d\n",
                PIN_ENC_CLK, PIN_ENC_DT, PIN_ENC_SW);
  Serial.printf("  OLED M154-4P:    SDA=GPIO %d, SCL=GPIO %d (SSD1309 0x78/0x3C)\n",
                PIN_OLED_SDA, PIN_OLED_SCL);

  initPsramAndLoadPours();
  initOledDisplay();
  updateBatteryAndPowerManagement();
  renderOledScreen();

  Serial.printf("  Bateria HW-107:  ADC=GPIO %d, CHRG=GPIO %d (%s)\n",
                PIN_BATT_ADC, PIN_TP4056_CHRG,
                localBattSensorPresent ? "Divisor detectado" : "Sin divisor ADC (pin al aire)");
  Serial.println("================================================================");

  loadSavedScaleFromNVS();

  weightQueue = xQueueCreate(WEIGHT_QUEUE_LEN, sizeof(WeightSample));
  initBleStack();
  startTimemoreScan();
}

void loop() {
  // 0. Procesar las muestras de peso que encoló la tarea BLE y los eventos de desconexión
  serviceDemoLivePour();   // demo en vivo (orden serie 'v'): inyecta su muestra antes de vaciar la cola
  drainWeightQueue();
  if (bleDisconnectEvent) {
    bleDisconnectEvent   = false;
    currentFlowRateGps   = 0.0f;
    displayedFlowRateGps = 0.0f;
    uiNeedsImmediateRedraw = true;
  }

  unsigned long now = millis();

  // 1. Encoder Rotatorio (GPIO 10/11/12), Botón BOOT (GPIO 0), comandos serie y LED RGB (GPIO 48)
  handleRotaryEncoder();
  handleBootButton();
  handleSerialCommands();
  updateStatusLed();

  // 2. Detener escaneo si se encontró candidata
  if (pendingStopScan) {
    pendingStopScan = false;
    NimBLEScan* pScan = NimBLEDevice::getScan();
    if (pScan->isScanning()) {
      pScan->stop();
    }
    isScanning = false;
  }

  // 3. Conectar a la Timemore Dot cuando fue detectada en el escaneo (no bloqueante) y avanzar
  //    la máquina de estados de la conexión
  if (doConnectToScale && !scaleConnected && bleConnectPhase == BLE_IDLE) {
    doConnectToScale = false;
    connectToTimemoreScale();
    lastScanAttemptMs = millis();
  }
  serviceBleConnection();

  // 3b. Confirmar la báscula recién conectada: guardar su MAC en NVS al llegar el primer peso válido;
  //     si en 10 s no llega ninguno y no es la báscula ya vinculada, desconectar y descartarla.
  if (pendingScaleSave) {
    if (!scaleConnected) {
      pendingScaleSave = false;
    } else if (weightPacketsRx > 0) {
      pendingScaleSave = false;
      saveScaleToNVS(pendingSaveMac, pendingSaveAddrType);
    } else if (!isSavedScaleMac(pendingSaveMac) &&
               (uint32_t)(millis() - scaleConnectedAtMs) >= NEW_SCALE_PROOF_MS) {
      pendingScaleSave = false;
      Serial.printf("[BLE] %s no envia peso en %lu s -> desconectando.\n",
                    pendingSaveMac, (unsigned long)(NEW_SCALE_PROOF_MS / 1000UL));
      rejectMac(pendingSaveMac);
      if (pScaleClient != nullptr) pScaleClient->disconnect();
    }
  }

  // 4. Reintentar escaneo periódico si la báscula está apagada o desconectada.
  //    Se usa millis() actual (no `now`): tras una conexión bloqueante `now` queda atrasado y la
  //    resta sin signo daba un valor enorme que disparaba un re-escaneo inmediato.
  if (!scaleConnected && !doConnectToScale && bleConnectPhase == BLE_IDLE) {
    NimBLEScan* pScan = NimBLEDevice::getScan();
    if (!pScan->isScanning()) {
      isScanning = false;
      if ((uint32_t)(millis() - lastScanAttemptMs) >= SCAN_RETRY_MS) {
        startTimemoreScan();
      }
    }
  }

  // 5. Ejecutar Tara o comandos de Timer diferidos
  if (pendingScaleTare) {
    pendingScaleTare = false;
    executeTimemoreTare();
  }
  // 5b. Comandos recibidos de la máquina por el puente BooKoo (tara, cronómetro)
  serviceBookooBridge();

  // 6. Sondeo inicial diferido de batería (1.2 s tras conectar) y Keep-Alive cada 25 segundos
  if (scaleConnected && pendingBatteryQueryAtMs > 0 && (int32_t)(millis() - pendingBatteryQueryAtMs) >= 0) {
    pendingBatteryQueryAtMs = 0;
    if (!scaleBatteryValid && batteryQueryTries < 2) {
      batteryQueryTries++;
      sendTimemoreKeepAlive();
      pendingBatteryQueryAtMs = millis() + 1200UL; // segundo intento si no respondió
    }
  }
  if (scaleConnected && ((uint32_t)(millis() - lastKeepAliveMs) >= KEEP_ALIVE_MS)) {
    lastKeepAliveMs = millis();
    unsigned long lastStep = lastWeightStepChangeMs;
    unsigned long stepAge  = (now >= lastStep) ? (now - lastStep) : 0UL;
    // Se evita sondear en pleno vertido, pero nunca más de KEEP_ALIVE_FORCE_MS sin ningún comando:
    // sin comandos la Dot se apaga sola a los ~70 s (p. ej. cronómetro olvidado en marcha y báscula quieta).
    bool quiet  = !timerRunning && (lastStep == 0 || stepAge > 3000UL);
    bool forced = (uint32_t)(millis() - lastKeepAliveSentMs) >= KEEP_ALIVE_FORCE_MS;
    if (quiet || forced) {
      sendTimemoreKeepAlive();
    }
  }

  // 6a. Intercambio de MTU con la Dot, 1 s después de suscribirse (ver COLA_DOT_MTU.md). Se comprueba a
  //     los 2 s y se reintenta hasta 3 veces si el MTU sigue en 23.
  if (scaleConnected && pScaleClient != nullptr && pendingMtuExchangeAtMs > 0 &&
      (int32_t)(millis() - pendingMtuExchangeAtMs) >= 0) {
    pendingMtuExchangeAtMs = 0;
    uint16_t mtu = pScaleClient->getMTU();
    if (mtu > 23) {
      scaleLinkMtu = mtu;
      Serial.printf("[BLE] MTU con la Dot = %u: la Dot puede juntar tramas y vaciar su cola.\n", mtu);
    } else if (mtuExchangeTries < 3) {
      mtuExchangeTries++;
#if defined(NIMBLE_V2)
      bool sent = pScaleClient->exchangeMTU();
#else
      bool sent = false;
#endif
      Serial.printf("[BLE] Pidiendo MTU %u a la Dot (intento %u, %s).\n",
                    DOT_PREFERRED_MTU, mtuExchangeTries, sent ? "enviado" : "rechazado");
      pendingMtuExchangeAtMs = millis() + 2000UL;
    } else {
      Serial.println("[BLE] AVISO: la Dot sigue con MTU 23. El retraso del peso crecera con el tiempo.");
    }
  }

  // 6c. Resultado de la sonda de cola (comando serie 'q')
  if (queueProbeResultReady) {
    queueProbeResultReady = false;
    Serial.printf("[COLA] Sonda 02 08 -> respuesta en %lu ms (MTU %u, tramas juntadas %lu/%lu). %s\n",
                  (unsigned long)queueProbeLastMs, (unsigned)scaleLinkMtu,
                  (unsigned long)multiFramePackets, (unsigned long)openScalePackets,
                  queueProbeLastMs <= 200UL ? "Cola vacia." : "COLA ACUMULADA.");
  } else if (queueProbeSentMs != 0 && (uint32_t)(millis() - queueProbeSentMs) > 4000UL) {
    queueProbeSentMs = 0;
    Serial.println("[COLA] La Dot no respondio a la sonda 02 08 en 4 s.");
  }

  // 6b. Imponer intervalo de conexión rápido. Diagnóstico: la Dot NO envía peticiones de parámetros que
  //     pasen por onConnParamsUpdateRequest(), pero el enlace acaba en 60 ms (el controlador lo acepta a
  //     nivel LL). Si el intervalo supera 20 ms, el central vuelve a pedir 10-15 ms (hasta 5 veces).
  //     Primera comprobación a los 1.5 s de conectar y después cada 3 s (antes cada 8 s: los primeros
  //     ~8 s tras conectar iban a 60 ms).
  const uint32_t itvlCheckMs = (connItvlFixAttempts == 0) ? 1500UL : 3000UL;
  if (scaleConnected && pScaleClient != nullptr && connItvlFixAttempts < 5 &&
      (uint32_t)(millis() - lastConnItvlCheckMs) >= itvlCheckMs) {
    lastConnItvlCheckMs = millis();
    uint16_t itvl = pScaleClient->getConnInfo().getConnInterval();
    if (itvl > 16) {
      connItvlFixAttempts++;
      bool sent = pScaleClient->updateConnParams(8, 12, 0, 400);
      Serial.printf("[BLE] Intervalo %.1f ms -> pidiendo 10-15 ms (intento %u, %s).\n",
                    itvl * 1.25f, connItvlFixAttempts, sent ? "enviado" : "rechazado");
    }
  }

  // 7. Actualizar cálculo de Flow Rate (g/s), Auto-Timer, batería HW-107 y pantalla OLED:
  //    - Si llegó un paquete BLE con nuevo peso/flow o se tocó el encoder: redibujar al instante (0 ms de espera).
  //    - Si corre el cronómetro: refrescar cada 100 ms (décimas de segundo).
  //    - En reposo: refrescar cada 250 ms para dejar el bus I2C y la CPU 100% libres al llegar paquetes BLE.
  unsigned long refreshIntervalMs = timerRunning ? 100UL : 250UL;
  if (uiNeedsImmediateRedraw || (now - lastOledUpdateMs >= refreshIntervalMs)) {
    uiNeedsImmediateRedraw = false;
    lastOledUpdateMs = now;
    updateFlowAndAutoTimer();
    updateBatteryAndPowerManagement();
    renderOledScreen();
  }

  // 8. Reporte periódico por Monitor Serie cada 5 segundos
  if (now - lastSerialStatusMs >= SERIAL_STATUS_MS) {
    lastSerialStatusMs = now;
    float connItvlMs = 0.0f;
    uint16_t connLat = 0;
    if (scaleConnected && pScaleClient != nullptr && pScaleClient->isConnected()) {
      connItvlMs = pScaleClient->getConnInfo().getConnInterval() * 1.25f;
      connLat    = pScaleClient->getConnInfo().getConnLatency();
    }
    if (localBattSensorPresent) {
      Serial.printf("[STATUS] Dot=%s (%s) | Peso=%+6.1fg | Flow=%4.1fg/s (Dot:%4.1f) | BatDot=%u%% | BatHW107=%.2fV (%u%%%s) | BLE=%.1fms/lat%u | Rx=%lu\n",
                    scaleConnected ? "CONECTADA" : "BUSCANDO",
                    hasSavedMac ? savedTimemoreMac : "Auto",
                    currentWeightGrams,
                    currentFlowRateGps,
                    scaleNativeFlowGps,
                    scaleBatteryPct,
                    localBattVolts,
                    localBattPct,
                    localBattCharging ? " CHG" : "",
                    connItvlMs,
                    connLat,
                    (unsigned long)packetsRxCount);
    } else {
      Serial.printf("[STATUS] Dot=%s (%s) | Peso=%+6.1fg | Flow=%4.1fg/s (Dot:%4.1f) | BatDot=%u%% | BLE=%.1fms/lat%u | Rx=%lu\n",
                    scaleConnected ? "CONECTADA" : "BUSCANDO",
                    hasSavedMac ? savedTimemoreMac : "Auto",
                    currentWeightGrams,
                    currentFlowRateGps,
                    scaleNativeFlowGps,
                    scaleBatteryPct,
                    connItvlMs,
                    connLat,
                    (unsigned long)packetsRxCount);
    }
    // Rendimiento real del bus I2C de la OLED (volcado por páginas modificadas) y de la cola BLE
    Serial.printf("[STATUS] OLED: ultimo=%.1fms max=%.1fms paginas/5s=%lu | Muestras BLE perdidas=%lu\n",
                  oledFlushLastUs / 1000.0f, oledFlushMaxUs / 1000.0f,
                  (unsigned long)oledPagesSent, (unsigned long)droppedWeightSamples);
    // Cola de salida de la Dot: con MTU > 23 las "tramas juntadas" crecen de vez en cuando (la Dot
    // vaciando su cola). Sonda manual con el comando serie 'q'.
    if (scaleConnected) {
      Serial.printf("[LINK] MTU=%u | tramas juntadas %lu/%lu | ultima sonda %lu ms\n",
                    (unsigned)scaleLinkMtu, (unsigned long)multiFramePackets,
                    (unsigned long)openScalePackets, (unsigned long)queueProbeLastMs);
    }
    // Diagnóstico (los mensajes del arranque se pierden mientras el USB se reconecta tras un reset)
    Serial.printf("[STATUS] Divisor bat: %s (pd=%lu mV, pu=%lu mV, %.2f V) | Peticiones intervalo Dot: %lu (ultima %u..%u lat %u)\n",
                  localBattSensorPresent ? "SI" : "NO",
                  (unsigned long)battProbePullDownMv, (unsigned long)battProbePullUpMv, localBattVolts,
                  (unsigned long)connParamReqCount, lastConnReqMin, lastConnReqMax, lastConnReqLat);
    if (bookooBridgeEnabled) {
      Serial.printf("[BOOKOO] '%s' | maquina=%s | notif=%s | tramas=%lu | comandos=%lu\n",
                    bookooAdvName, bookooMachineConnected ? "CONECTADA" : "no",
                    bookooSubscribed ? "SI" : "no",
                    (unsigned long)bookooFramesSent, (unsigned long)bookooCmdsRx);
    }
    oledFlushMaxUs = 0;
    oledPagesSent  = 0;
  }
}
