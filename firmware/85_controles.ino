// ============================================================================
// INTERFAZ: CONTROLES
// Encoder (giro con histéresis), multiclic, botón BOOT y órdenes por el monitor serie.
// Pestaña del sketch: el IDE la concatena tras firmware.ino (orden alfabético).
// Tipos, constantes y variables globales compartidas están en la pestaña principal.
// ============================================================================

// ============================================================================
// LECTURA DEL ENCODER ROTATORIO (GPIO 10/11/12), BOTÓN BOOT (GPIO 0) Y SERIE
// ============================================================================
// Controles por clics del botón (Encoder SW GPIO 12 y Botón BOOT GPIO 0):
// - 1 Click  (Single Click): Cronómetro START / STOP (Iniciar / Pausar)
// - 2 Clicks (Double Click): TARA (0.0 g) + Reseteo del Cronómetro (0.0 s)
// - 3 Clicks (Triple Click): Abrir / Cerrar el Menú de Configuración
void startOrStopStopwatch() {
  unsigned long now = millis();
  if (timerRunning) {
    timerAccumulatedMs += (now - timerStartMillis);
    timerRunning = false;
    Serial.println("[TIMER] 1-Click: Cronometro PAUSADO.");
    // Al pausar/terminar la extracción, guardar automáticamente el Pour-Over en PSRAM + NVS
    // Con la máquina conectada por el puente es un espresso: no va al historial de Pour-Over
    if (!bookooMachineConnected) finalizeAndSaveCurrentPourOver(timerAccumulatedMs);
  } else {
    if (timerAccumulatedMs == 0) {
      resetLivePourRecording();
    }
    timerStartMillis = now;
    timerRunning     = true;
    autoTimerArmed   = false;
    Serial.println("[TIMER] 1-Click: Cronometro INICIADO.");
  }
}

void tareAndResetStopwatch() {
  // Doble clic: Tara + Reset inmediato en 0 ms (NO guardar en Flash NVS ni mostrar banner de 2.2s;
  // el guardado a Flash NVS se realiza únicamente al detener la extracción con 1 Clic).
  pourSavedBannerUntilMs = 0;
  timerRunning           = false;
  timerAccumulatedMs     = 0;
  autoTimerArmed         = autoTimerEnabled;
  resetLivePourRecording();
  pendingScaleTare       = false;
  espressoCancelShot();  // espresso: cancela el shot y arma el arranque con la primera gota
  executeTimemoreTare(); // Ejecutar TARA por Software (0 ms) sin activar el Zero-Lock interno de la Dot
  Serial.println("[BOTON] 2-Clicks: TARA Instantanea (0.0g) + Cronometro REINICIADO a 0.0s.");
}

// Historial que muestra el visor: shots o pour-overs
static uint8_t viewerCount() { return viewerShowsShots ? savedShotsCount : savedPoursCount; }
static int8_t& viewerSlot()   { return viewerShowsShots ? selectedShotSlot : selectedPourSlot; }

// Pantalla TDS (R2): registro destino = el último del modo actual; girar recorre el historial
static void tdsSelectRecord(int8_t step) {
  const bool shot = (brewMode == BrewMode::Espresso);
  const uint8_t n = shot ? savedShotsCount : savedPoursCount;
  r2TargetIsShot = shot;
  if (n == 0) { r2TargetId = 0; return; }
  int idx = n - 1;
  for (uint8_t i = 0; i < n; i++) {
    const uint16_t id = shot ? savedShots[i].shotId : psramSavedPours[i].pourId;
    if (id == r2TargetId) idx = i;
  }
  if (step == 0 && r2TargetId == 0) idx = n - 1;
  idx = ((idx + step) % n + n) % n;
  r2TargetId = shot ? savedShots[idx].shotId : psramSavedPours[idx].pourId;
}

// "¿Cómo sabe?" (antes de ver el gráfico) y gráfico de control con la sugerencia
static bool chartFromViewer = false;

void beginTasteInput() {
  tasteStep = 0;
  tasteAB   = 0;
  tasteST   = 0;
  int8_t ab, st;
  if (recordTaste(r2TargetIsShot, r2TargetId, &ab, &st)) { tasteAB = ab; tasteST = st; }
  uiState = UiState::Taste;
  uiNeedsImmediateRedraw = true;
}

static void openChart(bool shot, uint16_t id, bool fromViewer) {
  r2TargetIsShot  = shot;
  r2TargetId      = id;
  chartFromViewer = fromViewer;
  chartShowAdvice = false;
  uiState         = UiState::Chart;
}

static void openTdsScreen() {
  r2TargetId = 0;
  tdsSelectRecord(0);
  uiState = UiState::TdsMeasure;
  r2Request();
}

// Al salir de la edición de un valor del menú, guardarlo en la NVS
static void commitMenuEdit() {
  if (uiState != UiState::MenuEdit) return;
  if (menuEditItem == MI_GRIND) { saveGrindSettings(); return; }
  if (menuEditItem == MI_DOSE && brewMode == BrewMode::PourOver) saveDoseToNVS();
  else saveEspressoSettings();
}

void executeButtonMultiClickAction(uint8_t clicks) {
  if (clicks == 0) return;
  uiNeedsImmediateRedraw = true;
  registerUserOrScaleActivity();

  const bool inMenu = (uiState == UiState::Menu || uiState == UiState::MenuEdit);

  // 3 CLICKS: Abrir / Cerrar Menú (o salir del visor del historial)
  if (clicks >= 3) {
    if (uiState == UiState::PourViewer || uiState == UiState::TdsMeasure) {
      r2CancelMeasure();
      uiState = UiState::Main;
    } else if (inMenu) {
      commitMenuEdit();
      uiState = UiState::Main;
    } else {
      uiState = UiState::Menu;
    }
    triggerLedFlash();
    Serial.printf("[BOTON] 3-Clicks: Menu %s.\n", (uiState == UiState::Menu) ? "ABIERTO" : "CERRADO");
    return;
  }

  // 2 CLICKS: En pantalla principal -> TARA + Reset Cronómetro; en Menú/Visor -> Salir a pantalla principal
  if (clicks == 2) {
    if (uiState != UiState::Main) {
      r2CancelMeasure();
      commitMenuEdit();
      uiState = UiState::Main;
      triggerLedFlash();
    } else {
      tareAndResetStopwatch();
    }
    return;
  }

  // 1 CLICK: En pantalla principal -> Cronómetro / shot; en Menú -> Confirmar opción; en Visor -> Cambiar página
  if (clicks == 1) {
    if (uiState == UiState::Taste) {
      if (tasteStep == 0) {
        tasteStep = 1;
      } else {
        storeTaste(r2TargetIsShot, r2TargetId, tasteAB, tasteST);
        openChart(r2TargetIsShot, r2TargetId, false);
      }
      return;
    }
    if (uiState == UiState::Chart) {
      int8_t ab, st;
      if (chartShowAdvice && !recordTaste(r2TargetIsShot, r2TargetId, &ab, &st)) beginTasteInput();
      else if (chartShowAdvice && chartFromViewer) uiState = UiState::PourViewer;
      else if (chartShowAdvice) uiState = UiState::Main;
      else chartShowAdvice = true;
      return;
    }
    if (uiState == UiState::TdsMeasure) {
      if (r2State == R2State::Calibrating)                  return;
      if (r2State == R2State::Measuring)                    r2CancelMeasure();
      else if (r2State == R2State::Ready && r2TargetId != 0) r2StartMeasure();
      else                                                  r2Request();
      return;
    }
    if (uiState == UiState::PourViewer) {
      if (viewerCount() == 0) {
        uiState = UiState::Main;
      } else {
        // 1 Click dentro del visor: GRÁFICA -> FICHA -> (con TDS) GRÁFICO DE CONTROL -> GRÁFICA
        const bool shot = viewerShowsShots;
        const uint16_t id = shot ? savedShots[selectedShotSlot].shotId : psramSavedPours[selectedPourSlot].pourId;
        if (pourViewerShowStatsPage && recordTds(shot, id, nullptr, nullptr)) {
          pourViewerShowStatsPage = false;
          openChart(shot, id, true);
        } else {
          pourViewerShowStatsPage = !pourViewerShowStatsPage;
        }
      }
    } else if (inMenu) {
      uint8_t ids[MENU_MAX_ITEMS];
      const uint8_t n = buildMenuItems(ids);
      if (menuSelectedIndex >= n) menuSelectedIndex = 0;
      const uint8_t item = ids[menuSelectedIndex];
      switch (item) {
        case MI_MODE:
        {
          setBrewMode(brewMode == BrewMode::Espresso ? BrewMode::PourOver : BrewMode::Espresso);
          // Las opciones cambian con el modo: el cursor sigue sobre "Modo"
          uint8_t nids[MENU_MAX_ITEMS];
          const uint8_t nn = buildMenuItems(nids);
          for (uint8_t k = 0; k < nn; k++) if (nids[k] == MI_MODE) menuSelectedIndex = static_cast<int8_t>(k);
          break;
        }
        case MI_HISTORY:   // historial del modo actual
          uiState                 = UiState::PourViewer;
          viewerShowsShots        = (brewMode == BrewMode::Espresso);
          pourViewerShowStatsPage = false;
          if (viewerCount() > 0) viewerSlot() = static_cast<int8_t>(viewerCount() - 1);
          break;
        case MI_AUTOTIMER:
          autoTimerEnabled = !autoTimerEnabled;
          autoTimerArmed   = autoTimerEnabled;
          break;
        case MI_DOSE:          // valores editables: 1 clic entra, girar ajusta, 1 clic guarda
        case MI_RATIO:
        case MI_TARGET_TIME:
        case MI_GRIND:
          if (uiState == UiState::MenuEdit) {
            commitMenuEdit();
            uiState = UiState::Menu;
          } else {
            menuEditItem = item;
            uiState = UiState::MenuEdit;
          }
          break;
        case MI_TDS:           // medida de TDS con el DiFluid R2
          openTdsScreen();
          break;
        case MI_R2ZERO:        // cero del R2 con agua destilada
          openTdsScreen();
          r2RequestZero();
          break;
        case MI_BAG:           // bolsa de café nueva: historial de molienda limpio
          startNewBag();
          break;
        case MI_GRIND_DIR:     // sentido del molino
          grindUpCoarser = !grindUpCoarser;
          saveGrindSettings();
          break;
        case MI_JAR:           // jarra vacía sobre la báscula: su peso (sin nada, la borra)
          saveServerJar(rawWeightGrams > 20.0f ? static_cast<float>(rawWeightGrams) : 0.0f);
          break;
        case MI_BRIGHT:
          oledBrightnessLevel = (oledBrightnessLevel + 1) % 3;
          oledSetContrast(getActiveOledContrast());
          break;
        case MI_BRIDGE:        // reinicia la pila BLE: la Dot se reconecta en ~2 s
          setBookooBridgeEnabled(!bookooBridgeEnabled);
          break;
        case MI_RELINK:        // olvidar la Dot (borra la NVS de la báscula)
          uiState = UiState::Main;
          clearScaleFromNVS();
          break;
        case MI_UPDATE:        // actualización por Bluetooth (página Web Bluetooth de docs/ota, PIN en pantalla)
          uiState = UiState::Main;
          enterBleOtaMode();
          break;
        case MI_SLEEP:
          uiState = UiState::Main;
          enterDeepSleepMode("APAGADO MANUAL");
          break;
        case MI_EXIT:
        default:
          commitMenuEdit();
          uiState = UiState::Main;
          break;
      }
    } else if (brewMode == BrewMode::Espresso) {
      espressoClick();         // shot: arranca / corta
    } else {
      startOrStopStopwatch();  // pour-over: cronómetro start / stop
    }
  }
}

void applyEncoderDelta(int32_t delta) {
  {
    uiNeedsImmediateRedraw = true;
    registerUserOrScaleActivity();

    if (uiState == UiState::Taste) {
      int8_t& v = (tasteStep == 0) ? tasteAB : tasteST;
      v = static_cast<int8_t>(constrain(static_cast<int>(v) + (delta > 0 ? 1 : -1), -2, 2));
    } else if (uiState == UiState::Chart) {
      chartShowAdvice = !chartShowAdvice;
    } else if (uiState == UiState::TdsMeasure) {
      if (r2State != R2State::Measuring) tdsSelectRecord(delta > 0 ? 1 : -1);
    } else if (uiState == UiState::PourViewer) {
      const uint8_t n = viewerCount();
      if (n > 0) {
        int8_t& slot = viewerSlot();
        slot = static_cast<int8_t>(slot + (delta > 0 ? 1 : -1));
        while (slot < 0) slot += n;
        while (slot >= n) slot -= n;
      }
    } else if (uiState == UiState::MenuEdit) {
      const float d = static_cast<float>(delta);
      if (menuEditItem == MI_DOSE && brewMode == BrewMode::PourOver) {
        coffeeDoseGrams = constrain(coffeeDoseGrams + d * 0.5f, DOSE_MIN_G, DOSE_MAX_G);
      } else if (menuEditItem == MI_DOSE) {
        espDoseGrams = constrain(roundf((espDoseGrams + d * 0.1f) * 10.0f) / 10.0f, ESP_DOSE_MIN_G, ESP_DOSE_MAX_G);
      } else if (menuEditItem == MI_RATIO) {
        espRatio = constrain(roundf((espRatio + d * 0.1f) * 10.0f) / 10.0f, ESP_RATIO_MIN, ESP_RATIO_MAX);
      } else if (menuEditItem == MI_GRIND) {
        float& g = grindForMode();
        g = constrain(roundf((g + d * 0.1f) * 10.0f) / 10.0f, GRIND_MIN, GRIND_MAX);
      } else if (menuEditItem == MI_TARGET_TIME) {
        espTargetSec = static_cast<uint8_t>(constrain(static_cast<int>(espTargetSec) + static_cast<int>(delta),
                                                      static_cast<int>(ESP_TIME_MIN_S), static_cast<int>(ESP_TIME_MAX_S)));
      }
    } else if (uiState == UiState::Menu) {
      uint8_t ids[MENU_MAX_ITEMS];
      const int8_t n = static_cast<int8_t>(buildMenuItems(ids));
      menuSelectedIndex = static_cast<int8_t>(menuSelectedIndex + delta);
      while (menuSelectedIndex < 0) menuSelectedIndex += n;
      while (menuSelectedIndex >= n) menuSelectedIndex -= n;
    } else {
      // En pantalla principal: girar cambia entre las 4 vistas (todas con el PESO como protagonista) y,
      // con la máquina conectada por el puente, una 5ª vista de diagnóstico del puente
      const int nViews = NUM_SCREEN_VIEWS + (bookooMachineConnected ? 1 : 0);
      int cur = (currentScreenView < nViews) ? static_cast<int>(currentScreenView) : 0;
      int nextView = (cur + (delta > 0 ? 1 : -1)) % nViews;
      if (nextView < 0) nextView += nViews;
      currentScreenView = static_cast<uint8_t>(nextView);
      Serial.printf("[ENCODER] Vista de pantalla cambiada a: %u\n", currentScreenView);
    }
  }

}

void handleRotaryEncoder() {
  // 1. Procesar giro en cuadratura desde el periférico PCNT por hardware del ESP32-S3.
  //    Acumulador con histéresis: una muesca cuenta al acumular 3 de sus 4 pulsos, y el resto (-1) queda
  //    pendiente. Así el umbral cae a media transición, NO en la posición de reposo: un rebote de un
  //    pulso (4->3->4) ya no hace oscilar la muesca (antes: count/4 truncado, umbral justo en reposo).
  int32_t encCount = getEncoderPulseCount();
  int32_t encStep  = encCount - lastEncPulseCount;
  lastEncPulseCount = encCount;
  if (encStep > 4000 || encStep < -4000) encStep = 0;  // salto anómalo (desbordamiento): ignorar
  encPulseAccum += encStep;
  int32_t delta = 0;
  while (encPulseAccum >= ENC_STEPS_PER_DETENT - 1) {
    encPulseAccum -= ENC_STEPS_PER_DETENT;
    delta++;
  }
  while (encPulseAccum <= -(ENC_STEPS_PER_DETENT - 1)) {
    encPulseAccum += ENC_STEPS_PER_DETENT;
    delta--;
  }

  if (delta != 0) applyEncoderDelta(delta);

  // 2. Procesar botón Push del Encoder (PIN_ENC_SW = GPIO 12, activo en LOW)
  //    - 1 Click:  Cronómetro Start / Stop (o confirmar en menú)
  //    - 2 Clicks: TARA (0.0 g) + Reset Cronómetro (0.0 s)
  //    - 3 Clicks: Abrir / Cerrar Menú
  bool swDown = (digitalRead(PIN_ENC_SW) == LOW);
  unsigned long now = millis();

  if (swDown && !encSwPressed) {
    encSwPressed     = true;
    encSwLongHandled = false;
    encSwDownMs      = now;
    registerUserOrScaleActivity();
  } else if (swDown && encSwPressed) {
    // Respaldo de pulsación larga (>= 1500 ms) para abrir/cerrar menú si se mantiene presionado
    if (!encSwLongHandled && encClickCount == 0 && (now - encSwDownMs >= 1500UL)) {
      encSwLongHandled = true;
      executeButtonMultiClickAction(3);
    }
  } else if (!swDown && encSwPressed) {
    unsigned long heldMs = now - encSwDownMs;
    encSwPressed = false;

    if (!encSwLongHandled && heldMs >= 25UL) {
      encClickCount++;
      encLastReleaseMs = now;
      // Si ya llegó al 3er clic (Triple Click), ejecutar de inmediato sin esperar timeout
      if (encClickCount >= 3) {
        uint8_t c = encClickCount;
        encClickCount = 0;
        executeButtonMultiClickAction(c);
      }
    }
  }

  // Si hay 1 o 2 clics acumulados y ya expiró la ventana multi-clic, ejecutar acción
  if (!encSwPressed && encClickCount > 0 && (now - encLastReleaseMs >= MULTI_CLICK_GAP_MS)) {
    uint8_t c = encClickCount;
    encClickCount = 0;
    executeButtonMultiClickAction(c);
  }
}

void handleBootButton() {
  bool isDown = (digitalRead(PIN_BOOT_BUTTON) == LOW);
  unsigned long now = millis();

  if (isDown && !bootBtnPressed) {
    bootBtnPressed     = true;
    bootBtnLongHandled = false;
    bootBtnDownMs      = now;
    registerUserOrScaleActivity();
  } else if (isDown && bootBtnPressed) {
    if (!bootBtnLongHandled && bootClickCount == 0 && (now - bootBtnDownMs >= BTN_NVS_RESET_MS)) {
      bootBtnLongHandled = true;
      clearScaleFromNVS();
    }
  } else if (!isDown && bootBtnPressed) {
    unsigned long heldMs = now - bootBtnDownMs;
    bootBtnPressed = false;

    if (!bootBtnLongHandled && heldMs >= 25UL) {
      bootClickCount++;
      bootLastReleaseMs = now;
      if (bootClickCount >= 3) {
        uint8_t c = bootClickCount;
        bootClickCount = 0;
        executeButtonMultiClickAction(c);
      }
    }
  }

  if (!bootBtnPressed && bootClickCount > 0 && (now - bootLastReleaseMs >= MULTI_CLICK_GAP_MS)) {
    uint8_t c = bootClickCount;
    bootClickCount = 0;
    executeButtonMultiClickAction(c);
  }
}

// Salida del autotest: espera (hasta 300 ms) a que haya hueco en el FIFO USB para no perder líneas,
// porque el Serial no bloqueante descarta lo que no cabe.
static void selfTestPrint(const char* line) {
  unsigned long t0 = millis();
  while (!SafeUsbSerial.canWrite() && (millis() - t0) < 300UL) {
    delay(2);
  }
  Serial.println(line);
}

// Captura de pantalla (orden 's'): vuelca el framebuffer de la OLED (128x64, 8 páginas de 128 bytes;
// bit 0 de cada byte = fila superior de la página) en hexadecimal. tools/oled_capture.js lo convierte
// en PNG.
// El Serial no bloqueante solo garantiza 64 bytes libres: cada línea (263 caracteres) se envía en trozos
// de 32 bytes del framebuffer (64 caracteres), esperando hueco antes de cada uno.
static void waitSerialRoom() {
  unsigned long t0 = millis();
  while (!SafeUsbSerial.canWrite() && (millis() - t0) < 300UL) delay(2);
}

static void dumpOledFramebuffer() {
  char part[8 + 64 + 1];
  selfTestPrint("[FB] BEGIN 128x64");
  for (uint8_t page = 0; page < 8; page++) {
    const uint8_t* src = &oledBuffer[page * 128];
    snprintf(part, sizeof(part), "[FB] %u ", page);
    waitSerialRoom();
    Serial.print(part);
    for (uint8_t x = 0; x < 128; x += 32) {
      int n = 0;
      for (uint8_t i = 0; i < 32; i++) n += snprintf(part + n, sizeof(part) - n, "%02X", src[x + i]);
      waitSerialRoom();
      Serial.print(part);
    }
    waitSerialRoom();
    Serial.println();
  }
  selfTestPrint("[FB] END");
}

void handleSerialCommands() {
  while (Serial.available() > 0) {
    char c = static_cast<char>(Serial.read());
    if (c == '\r' || c == '\n') continue;
    if (c == 'x' || c == 'X') {
      // Autotest de los módulos puros: protocolo Timemore y motor de caudal (src/selftest.cpp)
      runSelfTests(selfTestPrint);
    } else if (c == 's' || c == 'S') {
      dumpOledFramebuffer();
    } else if (c == 'd' || c == 'D') {
      // Pour-over de demostración (solo RAM) y su banner de guardado
      injectDemoPourOver();
    } else if (c == 'k' || c == 'K') {
      // Cambiar de modo (pour-over <-> espresso), como Menú -> 1. Modo
      setBrewMode(brewMode == BrewMode::Espresso ? BrewMode::PourOver : BrewMode::Espresso);
      uiNeedsImmediateRedraw = true;
    } else if (c == 'e' || c == 'E') {
      // Demostración en vivo de un espresso con la receta actual
      startDemoEspresso();
    } else if (c == 'v' || c == 'V') {
      // Demostración en vivo de un V60 (2:45 en tiempo real), para grabar la pantalla
      startDemoLivePour();
    } else if (c >= '0' && c <= '3') {
      uiState = UiState::Main;
      currentScreenView = static_cast<uint8_t>(c - '0');
      pourSavedBannerUntilMs = 0;
      uiNeedsImmediateRedraw = true;
    } else if (c == 'g' || c == 'G' || c == 'i' || c == 'I') {
      // Visor del historial en el último pour-over: 'g' gráfica, 'i' ficha
      uiState = UiState::PourViewer;
      viewerShowsShots = (brewMode == BrewMode::Espresso);
      pourViewerShowStatsPage = (c == 'i' || c == 'I');
      if (viewerCount() > 0) viewerSlot() = static_cast<int8_t>(viewerCount() - 1);
      pourSavedBannerUntilMs = 0;
      uiNeedsImmediateRedraw = true;
    } else if (c == 'm' || c == 'M') {
      uiState = UiState::Menu;
      menuSelectedIndex = 0;
      pourSavedBannerUntilMs = 0;
      uiNeedsImmediateRedraw = true;
    } else if (c == 'o' || c == 'O') {
      // Cero del R2 (agua destilada en el prisma)
      openTdsScreen();
      r2RequestZero();
      uiNeedsImmediateRedraw = true;
    } else if (c == 'u' || c == 'U') {
      // Medir TDS (como 1 clic en la pantalla TDS): abre la pantalla si hace falta
      if (uiState != UiState::TdsMeasure) openTdsScreen();
      if (r2State == R2State::Ready) r2StartMeasure(); else r2Request();
      uiNeedsImmediateRedraw = true;
    } else if (c == '<' || c == '>') {
      applyEncoderDelta(c == '>' ? 1 : -1);   // giro simulado
    } else if (c == '.') {
      executeButtonMultiClickAction(1);        // clic simulado
    } else if (c == 'j' || c == 'J') {
      // TDS simulado (1.61 %) para el registro de la pantalla TDS, y a "¿cómo sabe?"
      if (uiState != UiState::TdsMeasure) openTdsScreen();
      if (r2TargetId != 0) {
        storeTdsResult(r2TargetIsShot, r2TargetId, tds::pack(1.61f, tds::FLAG_VALID));
        beginTasteInput();
      }
      pourSavedBannerUntilMs = 0;
    } else if (c == 'y' || c == 'Y') {
      // Pantalla TDS (R2) del último registro del modo actual
      openTdsScreen();
      pourSavedBannerUntilMs = 0;
      uiNeedsImmediateRedraw = true;
    } else if (c == 'q' || c == 'Q') {
      // Sonda de la cola de la Dot: el tiempo hasta la respuesta es el tamaño de su cola (DOT_COLA_BLE.md)
      sendQueueProbe();
    } else if (c == 't' || c == 'T') {
      pendingScaleTare = true;
    } else if (c == 'b' || c == 'B') {
      setBookooBridgeEnabled(!bookooBridgeEnabled);
    } else if (c == 'r' || c == 'R') {
      clearScaleFromNVS();
    } else if (c == 'c' || c == 'C') {
      timerRunning       = false;
      timerAccumulatedMs = 0;
      autoTimerArmed     = true;
      Serial.println("[SERIAL] Cronometro reiniciado a 0.0s.");
    }
  }
}
