// ============================================================================
// EXTRACCIÓN: CAUDAL, CICLO DEL SHOT Y MUESTREO DEL POUR-OVER
// Usa el motor de caudal de src/flow_engine.* (estado propiedad de loop()).
// Pestaña del sketch: el IDE la concatena tras firmware.ino (orden alfabético).
// Tipos, constantes y variables globales compartidas están en la pestaña principal.
// ============================================================================

// ============================================================================
// CÁLCULO DE FLOW RATE (g/s) Y MUESTREO EN PSRAM
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

  // Espresso (45_espresso.ino): en modo espresso, o con un shot de la cafetera en curso, manda el ciclo
  // del shot; el auto-timer, el auto-stop y el muestreo del pour-over no actúan.
  serviceEspressoShot(nowMs);
  if (brewMode == BrewMode::Espresso || shotPhase != ShotPhase::Idle) return;

  // Pour-over guiado (41_guia.ino): taras de los pasos, arranque del cronómetro y medida del yield.
  // Los vertidos y los toques de la tetera los separa src/pour_segmenter con cada lectura de la Dot.
  serviceGuide(nowMs);

  // Muestreo en vivo hacia PSRAM cada 500 ms (2 Hz) mientras corre el cronómetro
  if (timerRunning && psramLiveSamples != nullptr) {
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
