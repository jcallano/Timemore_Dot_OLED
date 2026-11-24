// ============================================================================
// EXTRACCIÓN: AUTO-TIMER, AUTO-STOP Y MUESTREO
// Usa el motor de caudal de src/flow_engine.* (estado propiedad de loop()).
// Pestaña del sketch: el IDE la concatena tras firmware.ino (orden alfabético).
// Tipos, constantes y variables globales compartidas están en la pestaña principal.
// ============================================================================

// ============================================================================
// CÁLCULO DE FLOW RATE (g/s), MUESTREO EN PSRAM Y AUTO-TIMER DE EXTRACCIÓN
// ============================================================================
void updateFlowAndAutoTimer() {
  unsigned long nowMs   = millis();
  unsigned long lastRx  = lastBleWeightRxMs;
  unsigned long tareMs  = lastTareCommandMs;
  unsigned long sinceTareMs = (tareMs > 0 && nowMs >= tareMs) ? (nowMs - tareMs) : 99999UL;
  float w = currentWeightGrams;

  // Con Tara por Software instantánea, tareSettlingActive ya nace en false y tareZeroConfirmed en true;
  // dejamos este respaldo por seguridad.
  if (tareSettlingActive && sinceTareMs >= 250UL) {
    tareSettlingActive = false;
    tareZeroConfirmed  = true;
  }

  // Solo decae aquí si se interrumpen los paquetes BLE (> 450 ms sin notificaciones);
  // mientras llegan paquetes, el motor gestiona el corte en addSample().
  flowEngine.decayIfStale(nowMs, lastRx);

  // Lógica Auto-Timer blindada contra falsos arranques al hacer Tara:
  // Solo arranca cuando:
  // 1. Auto-Timer está habilitado en el menú y armado para esta extracción.
  // 2. La Tara ya confirmó el cero (!tareSettlingActive && tareZeroConfirmed) y pasaron >= 250 ms desde el doble clic.
  // 3. El peso subió de forma activa y genuina después de la Tara (w >= 0.5 g, flow >= 0.40 g/s y escalón de subida > lastTareCommandMs).
  // Con la máquina conectada por el puente, el cronómetro lo manda ella: sin auto-timer ni auto-stop
  if (autoTimerEnabled && autoTimerArmed && !timerRunning && !bookooMachineConnected &&
      !tareSettlingActive && tareZeroConfirmed && sinceTareMs >= 250UL &&
      w >= 0.5f && currentFlowRateGps >= 0.40f &&
      lastWeightStepChangeMs > tareMs) {
    timerAccumulatedMs = 0;
    timerStartMillis   = nowMs;
    timerRunning       = true;
    autoTimerArmed     = false;
    resetLivePourRecording();
    peakFlowRateGps    = currentFlowRateGps;
    autoStopSinceMs    = 0;
    Serial.println("[AUTO-TIMER] Vertido detectado post-tara -> Grabacion de Pour-Over en PSRAM INICIADA.");
  }

  // AUTO-STOP (con Auto-Brew activo): si el peso cae por debajo del 70 % del máximo de esta extracción
  // durante AUTO_STOP_MS seguidos (se retiró el dripper o todo el conjunto), parar el cronómetro en el
  // instante de la caída y guardar el Pour-Over. No se dispara con el bloom ni con pausas entre vertidos,
  // porque ahí el peso se mantiene; sí con levantar la jarra más de AUTO_STOP_MS (1 clic la reanuda y
  // actualiza el mismo registro).
  if (timerRunning && autoTimerEnabled && !bookooMachineConnected) {
    if (w > brewMaxWeightGrams) brewMaxWeightGrams = w;
    bool removed = (brewMaxWeightGrams >= 20.0f) && (w < 0.7f * brewMaxWeightGrams);
    if (!removed) {
      autoStopSinceMs = 0;
    } else if (autoStopSinceMs == 0) {
      autoStopSinceMs   = nowMs;
      autoStopSampleIdx = liveSampleCount;
    } else if ((uint32_t)(nowMs - autoStopSinceMs) >= AUTO_STOP_MS) {
      unsigned long runUntil = (autoStopSinceMs > timerStartMillis) ? autoStopSinceMs : timerStartMillis;
      timerAccumulatedMs += (runUntil - timerStartMillis);
      timerRunning = false;
      if (autoStopSampleIdx < liveSampleCount) liveSampleCount = autoStopSampleIdx; // sin la caída
      autoStopSinceMs = 0;
      Serial.printf("[AUTO-STOP] Peso retirado %lu s -> cronometro detenido en %.1f s.\n",
                    (unsigned long)(AUTO_STOP_MS / 1000UL), timerAccumulatedMs / 1000.0f);
      finalizeAndSaveCurrentPourOver(timerAccumulatedMs);
      uiNeedsImmediateRedraw = true;
    }
  }

  // Muestreo en vivo hacia PSRAM cada 500 ms (2 Hz) mientras corre el cronómetro
  if (timerRunning && psramLiveSamples != nullptr) {
    // Detectar pulsos/vertidos de agua en Pour-Over (histéresis: sube de 0.8 g/s y baja de 0.25 g/s)
    if (!wasPouringActive && currentFlowRateGps >= 0.8f) {
      wasPouringActive = true;
      currentPourPulsesCount++;
    } else if (wasPouringActive && currentFlowRateGps <= 0.25f) {
      wasPouringActive = false;
    }

    if (lastLiveSampleMs == 0 || (nowMs - lastLiveSampleMs >= 500UL)) {
      lastLiveSampleMs = nowMs;
      if (liveSampleCount < MAX_LIVE_POUR_SAMPLES) {
        psramLiveSamples[liveSampleCount].weightGrams = (w > 0.0f) ? w : 0.0f;
        psramLiveSamples[liveSampleCount].flowGps     = (currentFlowRateGps > 0.0f) ? currentFlowRateGps : 0.0f;
        liveSampleCount++;
      }
    }
  }
}
