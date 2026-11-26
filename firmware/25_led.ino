// ============================================================================
// LED RGB WS2812B DE LA WEACT (GPIO 48)
// Color de estado (buscando / conectado / máquina BooKoo conectada / vertiendo) y destello.
// Pestaña del sketch: el IDE la concatena tras firmware.ino (orden alfabético).
// Tipos, constantes y variables globales compartidas están en la pestaña principal.
// ============================================================================

// ============================================================================
// CONTROL DEL LED RGB WS2812B EN WEACT STUDIO ESP32-S3-A (GPIO 48)
// ============================================================================
void setRgbColor(uint8_t r, uint8_t g, uint8_t b) {
  if (r == cachedR && g == cachedG && b == cachedB) return;
  cachedR = r;
  cachedG = g;
  cachedB = b;
  rgbLedWrite(WEACT_RGB_LED_PIN, r, g, b);
}

void triggerLedFlash() {
  ledFlashUntilMs = millis() + 200;
}

float espressoCutGrams();   // 45_espresso.ino

void updateStatusLed() {
  unsigned long now = millis();
  if (now < ledFlashUntilMs) {
    setRgbColor(LED_BRIGHTNESS * 2, LED_BRIGHTNESS * 2, LED_BRIGHTNESS * 2);
    return;
  }

  // Shot de espresso con receta: amarillo en los 3 g antes del punto de corte, rojo al llegar (corta ya).
  // Con la cafetera cortando por el puente no hace falta avisar.
  if (scaleConnected && shotPhase == ShotPhase::Running && !shotByMachine) {
    const float cut = espressoCutGrams();
    if (currentWeightGrams >= cut) {
      setRgbColor(LED_BRIGHTNESS * 2, 0, 0);
      return;
    }
    if (currentWeightGrams >= cut - 3.0f) {
      setRgbColor(LED_BRIGHTNESS * 2, LED_BRIGHTNESS * 3 / 2, 0);
      return;
    }
  }

  // Pour-over guiado, paso Vertidos: amarillo en los 10 g antes del agua objetivo, rojo al llegar
  const uint8_t guideLevel = scaleConnected ? guideLedLevel() : 0;
  if (guideLevel == 2) {
    setRgbColor(LED_BRIGHTNESS * 2, 0, 0);
    return;
  }
  if (guideLevel == 1) {
    setRgbColor(LED_BRIGHTNESS * 2, LED_BRIGHTNESS * 3 / 2, 0);
    return;
  }

  if (scaleConnected) {
    if (fabsf(currentFlowRateGps) >= 0.2f) {
      // Cian durante flujo activo de extracción
      setRgbColor(0, LED_BRIGHTNESS, LED_BRIGHTNESS);
    } else if (bookooMachineConnected) {
      // Magenta fijo: la Wendougee (o una app) está conectada por el puente BooKoo y recibe el peso
      setRgbColor(LED_BRIGHTNESS, 0, LED_BRIGHTNESS);
    } else {
      // Verde fijo cuando la Timemore Dot está conectada y lista
      setRgbColor(0, LED_BRIGHTNESS, 0);
    }
  } else if (isScanning || doConnectToScale) {
    // Amarillo parpadeante buscando la báscula
    bool blink = ((now / 300) % 2) == 0;
    setRgbColor(blink ? LED_BRIGHTNESS : 0, blink ? (LED_BRIGHTNESS * 3 / 4) : 0, 0);
  } else {
    // Rojo parpadeante si está desconectada
    bool blink = ((now / 300) % 2) == 0;
    setRgbColor(blink ? LED_BRIGHTNESS : 0, 0, 0);
  }
}
