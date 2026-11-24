// ============================================================================
// ENERGÍA: BATERÍA, REPOSO (LIGHT SLEEP) Y CORTE
// Medida del divisor, icono, light sleep con despertar por GPIO y deep sleep por batería baja.
// Pestaña del sketch: el IDE la concatena tras basicoledtimemore_dot.ino (orden alfabético).
// Tipos, constantes y variables globales compartidas están en la pestaña principal.
// ============================================================================

// ============================================================================
// GESTIÓN DE BATERÍA HW-107 (TP4056 + DW01A) Y AHORRO DE ENERGÍA (DEEP SLEEP)
// ============================================================================
uint8_t lipoVoltsToPercent(float v) {
  if (v >= 4.18f) return 100;
  if (v <= 3.25f) return 0;
  // Curva de descarga típica por tramos para celda Li-Ion / LiPo 1S (3.7V nominal)
  if (v >= 4.00f) return static_cast<uint8_t>(80.0f + (v - 4.00f) * (20.0f / 0.18f));
  if (v >= 3.80f) return static_cast<uint8_t>(50.0f + (v - 3.80f) * (30.0f / 0.20f));
  if (v >= 3.65f) return static_cast<uint8_t>(20.0f + (v - 3.65f) * (30.0f / 0.15f));
  return static_cast<uint8_t>((v - 3.25f) * (20.0f / 0.40f));
}

uint8_t getActiveOledContrast() {
  if (oledBrightnessLevel == 0) return 0x15; // Bajo
  if (oledBrightnessLevel == 1) return 0x6F; // Medio
  return 0xCF;                               // Alto
}

void enterDeepSleepMode(const char* reasonMsg) {
  Serial.printf("[POWER] Entrando en Modo Reposo (%s). Presiona o gira el Encoder (GPIO %d/%d/%d) o BOOT (GPIO %d) para despertar.\n",
                reasonMsg, PIN_ENC_SW, PIN_ENC_CLK, PIN_ENC_DT, PIN_BOOT_BUTTON);

  if (oledPresent) {
    oledClear();
    oledDrawRect(0, 0, 128, 64, true);
    oledDrawText(16, 16, reasonMsg, 1);
    oledDrawText(8, 36, "CLICK/GIRO = WAKE", 1);
    oledFlush();
    delay(700);
    oledPowerOff();
  }

  // Desconectar la báscula y apagar por completo la pila BLE (la radio activa impide el light sleep)
  shutdownBleStack();

  // Apagar LED RGB WS2812B
  setRgbColor(0, 0, 0);

  // REPOSO = LIGHT SLEEP del ESP32-S3 (antes era un bucle activo a 80 MHz que consumía ~15-25 mA).
  // - Light sleep NO reinicia el chip: se conservan RAM, PSRAM y estado, y el GPIO 0 (strapping) puede
  //   usarse para despertar sin riesgo de entrar en modo bootloader.
  // - gpio_sleep_sel_dis() mantiene los pull-ups normales de los pines durante el sueño.
  // - Despierta al pulsar el encoder (GPIO 12) o BOOT (GPIO 0), al girar el encoder (GPIO 10/11),
  //   y cada BATT_SLEEP_CHECK_MS para vigilar la batería (corte si baja de BATT_CUTOFF_VOLTS).
  pinMode(PIN_ENC_SW,      INPUT_PULLUP);
  pinMode(PIN_BOOT_BUTTON, INPUT_PULLUP);
  pinMode(PIN_ENC_CLK,     INPUT_PULLUP);
  pinMode(PIN_ENC_DT,      INPUT_PULLUP);
  gpio_pullup_en(static_cast<gpio_num_t>(PIN_ENC_SW));
  gpio_pullup_en(static_cast<gpio_num_t>(PIN_BOOT_BUTTON));
  gpio_pullup_en(static_cast<gpio_num_t>(PIN_ENC_CLK));
  gpio_pullup_en(static_cast<gpio_num_t>(PIN_ENC_DT));

  // Esperar a que ambos botones estén sueltos de forma estable antes de armar el despertar
  unsigned long releaseStartMs = millis();
  while (millis() - releaseStartMs < 60UL) {
    if (digitalRead(PIN_BOOT_BUTTON) == LOW || digitalRead(PIN_ENC_SW) == LOW) {
      releaseStartMs = millis();
    }
    delay(5);
  }

  const gpio_num_t wakePins[4] = {
    static_cast<gpio_num_t>(PIN_ENC_SW), static_cast<gpio_num_t>(PIN_BOOT_BUTTON),
    static_cast<gpio_num_t>(PIN_ENC_CLK), static_cast<gpio_num_t>(PIN_ENC_DT)
  };
  for (uint8_t i = 0; i < 4; i++) {
    gpio_sleep_sel_dis(wakePins[i]);
  }

  while (true) {
    // Armar el despertar en el nivel CONTRARIO al actual de cada pin: botones (sueltos = HIGH) despiertan
    // al ponerse en LOW; las fases del encoder despiertan con cualquier cambio, esté en la posición que esté.
    int levelsBefore[4];
    for (uint8_t i = 0; i < 4; i++) {
      levelsBefore[i] = gpio_get_level(wakePins[i]);
      gpio_wakeup_enable(wakePins[i], levelsBefore[i] ? GPIO_INTR_LOW_LEVEL : GPIO_INTR_HIGH_LEVEL);
    }
    esp_sleep_enable_gpio_wakeup();
    esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(BATT_SLEEP_CHECK_MS) * 1000ULL);

    esp_err_t slept = esp_light_sleep_start();
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

    for (uint8_t i = 0; i < 4; i++) {
      gpio_wakeup_disable(wakePins[i]);
    }
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

    if (slept == ESP_OK && cause == ESP_SLEEP_WAKEUP_TIMER) {
      // Despertar programado: solo vigilar la batería y volver a dormir
      if (readLocalBatteryNow() && localBattVolts < BATT_CUTOFF_VOLTS && !localBattCharging) {
        enterBatteryCutoff();
      }
      continue;
    }

    // Despertar por GPIO (o sueño rechazado porque un pin ya estaba activo): confirmar
    delay(18); // anti-rebote
    bool btnLow = (digitalRead(PIN_ENC_SW) == LOW) || (digitalRead(PIN_BOOT_BUTTON) == LOW);
    bool rotated = (gpio_get_level(wakePins[2]) != levelsBefore[2]) ||
                   (gpio_get_level(wakePins[3]) != levelsBefore[3]);
    if (btnLow || rotated) {
      break;
    }
  }

  // Esperar a que el usuario suelte el botón para que el clic de despertar no dispare el cronómetro
  unsigned long wakeReleaseMs = millis();
  while (millis() - wakeReleaseMs < 50UL) {
    if (digitalRead(PIN_BOOT_BUTTON) == LOW || digitalRead(PIN_ENC_SW) == LOW) {
      wakeReleaseMs = millis();
    }
    delay(5);
  }

  // Limpiar contadores de clics y sincronizar posición del encoder
  encSwPressed           = false;
  encSwLongHandled       = false;
  encClickCount          = 0;
  bootBtnPressed         = false;
  bootBtnLongHandled     = false;
  bootClickCount         = 0;
  lastEncPulseCount      = getEncoderPulseCount();
  encPulseAccum          = 0;

  // Reactivar pantalla OLED, LED de estado y escaneo BLE
  oledPowerOn();
  oledIsDimmed           = false;
  oledSetContrast(getActiveOledContrast());
  lastActivityMs         = millis();
  uiNeedsImmediateRedraw = true;
  lastScanAttemptMs      = 0;
  triggerLedFlash();

  Serial.println("[POWER] Sistema DESPIERTO -> Pantalla OLED y busqueda BLE reactivadas.");
  initBleStack();
  startTimemoreScan();
}


void registerUserOrScaleActivity() {
  lastActivityMs = millis();
  if (oledIsDimmed) {
    oledIsDimmed = false;
    oledSetContrast(getActiveOledContrast());
  }
}

// Lee el divisor de batería (GPIO 4). Devuelve true si la lectura es válida y actualiza
// localBattVolts / localBattPct / localBattCharging. La primera lectura válida se toma tal cual
// (antes el filtro partía de 4.0 V y tardaba ~30 s en reflejar una batería baja).
bool readLocalBatteryNow() {
  if (!localBattSensorPresent) return false;
  uint32_t sumMv = 0;
  for (int i = 0; i < 4; i++) {
    sumMv += analogReadMilliVolts(PIN_BATT_ADC);
  }
  float adcMv = static_cast<float>(sumMv) / 4.0f;
  if (adcMv < 1500.0f || adcMv > 2500.0f) return false;
  float measuredV = (adcMv / 1000.0f) * BATT_DIVIDER_RATIO;
  if (!localBattHasReading) {
    localBattVolts      = measuredV;
    localBattHasReading = true;
  } else {
    localBattVolts = (0.25f * measuredV) + (0.75f * localBattVolts);
  }
  localBattPct      = lipoVoltsToPercent(localBattVolts);
  localBattCharging = (digitalRead(PIN_TP4056_CHRG) == LOW) || (localBattVolts >= 4.21f);
  return true;
}

// Batería agotada: avisar y entrar en DEEP SLEEP (el mínimo consumo posible). Solo despierta el botón
// del encoder (GPIO 12, pin RTC con su pull-up RTC activo); al despertar el chip reinicia y vuelve a
// medir: si sigue baja, vuelve a apagarse. Cargando por USB-C la tensión sube y arranca normal.
void enterBatteryCutoff() {
  Serial.printf("[POWER] Bateria agotada (%.2f V < %.2f V) -> apagado de proteccion.\n",
                localBattVolts, BATT_CUTOFF_VOLTS);
  if (oledPresent) {
    oledPowerOn();
    oledSetContrast(getActiveOledContrast());
    oledClear();
    oledDrawRect(0, 0, 128, 64, true);
    oledDrawText(22, 12, "BATERIA BAJA", 1);
    char vBuf[16];
    snprintf(vBuf, sizeof(vBuf), "%.2f V", localBattVolts);
    oledDrawText(40, 26, vBuf, 1);
    oledDrawText(16, 42, "CARGA POR USB-C", 1);
    oledFlush();
    delay(2500);
    oledPowerOff();
  }
  shutdownBleStack();
  setRgbColor(0, 0, 0);

  // Esperar a que el botón esté suelto para no despertar en el acto
  while (digitalRead(PIN_ENC_SW) == LOW) {
    delay(10);
  }
  rtc_gpio_pullup_en(static_cast<gpio_num_t>(PIN_ENC_SW));
  rtc_gpio_pulldown_dis(static_cast<gpio_num_t>(PIN_ENC_SW));
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
  esp_sleep_enable_ext1_wakeup(1ULL << PIN_ENC_SW, ESP_EXT1_WAKEUP_ANY_LOW);
  esp_deep_sleep_start();
}

void updateBatteryAndPowerManagement() {
  // Detectar actividad por cualquier cambio de peso (>= 0.1 g), flujo activo o cronómetro corriendo
  if (fabsf(currentWeightGrams - prevActivityWeightGrams) >= 0.1f || fabsf(currentFlowRateGps) >= 0.10f || timerRunning) {
    prevActivityWeightGrams = currentWeightGrams;
    registerUserOrScaleActivity();
  }

  unsigned long now = millis();

  // Leer ADC de batería cada 5 segundos solo si hay divisor resistivo detectado al arrancar
  // (evita reconfigurar el pin ADC en cada ciclo cuando el pin GPIO 4 está al aire)
  if (!battAdcProbedAtBoot || (localBattSensorPresent && (uint32_t)(now - lastBattReadMs) >= 5000UL)) {
    lastBattReadMs = now;
    if (!battAdcProbedAtBoot) {
      battAdcProbedAtBoot = true;
      // Detección del divisor 100k/100k (Thevenin ~50 kΩ a Vbat/2) comparando la lectura con el
      // pull-down y con el pull-up internos (~45 kΩ): un pin al aire salta de ~0 V a ~3.1 V
      // (diferencia > 2.7 V); con divisor la diferencia ronda 1.6-1.8 V. Antes se exigían >= 900 mV
      // solo con pull-down y, con la celda a ~3.8 V, el divisor quedaba justo en el umbral y se daba
      // por ausente (sin icono ni corte por batería).
      pinMode(PIN_BATT_ADC, INPUT_PULLDOWN);
      delay(2);
      uint32_t pullDownMv = analogReadMilliVolts(PIN_BATT_ADC);
      pinMode(PIN_BATT_ADC, INPUT_PULLUP);
      delay(2);
      uint32_t pullUpMv = analogReadMilliVolts(PIN_BATT_ADC);
      pinMode(PIN_BATT_ADC, INPUT);
      delay(2);
      battProbePullDownMv = pullDownMv;
      battProbePullUpMv   = pullUpMv;
      uint32_t diffMv = (pullUpMv > pullDownMv) ? (pullUpMv - pullDownMv) : 0;
      localBattSensorPresent = (pullDownMv >= 400) && (diffMv < 2300);
      Serial.printf("[BATT] Sonda GPIO %d: pull-down=%lu mV, pull-up=%lu mV -> divisor %s\n",
                    PIN_BATT_ADC, (unsigned long)pullDownMv, (unsigned long)pullUpMv,
                    localBattSensorPresent ? "DETECTADO" : "no detectado");
    }

    // Corte por batería baja: 2 lecturas seguidas (~5-10 s) por debajo de BATT_CUTOFF_VOLTS sin cargar.
    // Protege la 18650: antes se descargaba hasta los 2.4 V del DW01A.
    if (readLocalBatteryNow()) {
      if (localBattVolts < BATT_CUTOFF_VOLTS && !localBattCharging) {
        if (++lowBattReadings >= 2) {
          enterBatteryCutoff();
        }
      } else {
        lowBattReadings = 0;
      }
      uiNeedsImmediateRedraw = true;
    }
  }

  // CRÍTICO CONTRA MODO REPOSO PREMATURO:
  // Refrescar `now = millis()` DESPUÉS de llamar a `registerUserOrScaleActivity()` y del sondeo ADC,
  // y proteger la resta `(now >= lastAct) ? (now - lastAct) : 0UL`.
  // Antes, `now` se leía ANTES de `registerUserOrScaleActivity()`; cuando el tick de 1 ms avanzaba justo
  // al registrar actividad (al verter agua o tras hacer Tara/Power-Up), `lastActivityMs` quedaba en `now + 1`
  // y la resta sin signo `now - lastActivityMs` hacía underflow a 4294967295 ms (>= 600000 ms), disparando
  // `enterDeepSleepMode("MODO REPOSO")` en pleno vertido.
  now = millis();
  unsigned long lastAct    = lastActivityMs;
  unsigned long inactiveMs = (now >= lastAct) ? (now - lastAct) : 0UL;

  if (timerRunning || fabsf(currentFlowRateGps) >= 0.10f) {
    return;
  }

  // Auto-Dimming del OLED tras 2 minutos sin uso
  if (!oledIsDimmed && (inactiveMs >= AUTO_DIM_TIMEOUT_MS)) {
    oledIsDimmed = true;
    oledSetContrast(0x05); // Contraste mínimo para cuidar el panel OLED y ahorrar batería
    Serial.println("[POWER] Pantalla OLED atenuada por inactividad (Auto-Dim).");
  }

  // Auto Deep-Sleep tras 10 minutos sin uso
  // (no con la máquina conectada por el puente BooKoo: dormir apaga la radio y la máquina pierde la báscula)
  if (inactiveMs >= AUTO_SLEEP_TIMEOUT_MS && !bookooMachineConnected) {
    enterDeepSleepMode("MODO REPOSO");
  }
}
