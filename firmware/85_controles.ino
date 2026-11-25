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
static uint8_t viewerCount() {
  if (viewerShowsLots) return fermentLotsCount;
  return viewerShowsShots ? savedShotsCount : savedPoursCount;
}
static int8_t& viewerSlot() {
  if (viewerShowsLots) return selectedLotSlot;
  return viewerShowsShots ? selectedShotSlot : selectedPourSlot;
}

// Abre el visor del historial del modo actual en el último registro
static void openHistoryViewer() {
  uiState                 = UiState::PourViewer;
  viewerShowsLots         = (brewMode == BrewMode::Fermento);
  viewerShowsShots        = (brewMode == BrewMode::Espresso);
  pourViewerShowStatsPage = false;
  if (viewerCount() > 0) viewerSlot() = static_cast<int8_t>(viewerCount() - 1);
}

// Al salir de la edición de un valor del menú, guardarlo en la NVS
static void commitMenuEdit() {
  if (uiState != UiState::MenuEdit) return;
  if (menuEditItem == MI_SALT_PCT) saveFermentSettings();
  else if (menuEditItem == MI_DOSE && brewMode == BrewMode::PourOver) saveDoseToNVS();
  else saveEspressoSettings();
}

void executeButtonMultiClickAction(uint8_t clicks) {
  if (clicks == 0) return;
  uiNeedsImmediateRedraw = true;
  registerUserOrScaleActivity();

  const bool inMenu = (uiState == UiState::Menu || uiState == UiState::MenuEdit);

  // 3 CLICKS: Abrir / Cerrar Menú (o salir del visor del historial)
  if (clicks >= 3) {
    if (uiState == UiState::PourViewer) {
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
    if (uiState == UiState::PourViewer) {
      if (viewerCount() == 0) {
        uiState = UiState::Main;
      } else if (viewerShowsLots) {
        fermentAddReading();   // lotes: 1 clic guarda la lectura actual de la sonda
      } else {
        // 1 Click dentro del visor: alternar entre GRÁFICA y FICHA DE DATOS
        pourViewerShowStatsPage = !pourViewerShowStatsPage;
      }
    } else if (inMenu) {
      uint8_t ids[MENU_MAX_ITEMS];
      const uint8_t n = buildMenuItems(ids);
      if (menuSelectedIndex >= n) menuSelectedIndex = 0;
      const uint8_t item = ids[menuSelectedIndex];
      switch (item) {
        case MI_MODE:
        {
          setBrewMode(nextBrewMode(brewMode));
          // Las opciones cambian con el modo: el cursor sigue sobre "Modo"
          uint8_t nids[MENU_MAX_ITEMS];
          const uint8_t nn = buildMenuItems(nids);
          for (uint8_t k = 0; k < nn; k++) if (nids[k] == MI_MODE) menuSelectedIndex = static_cast<int8_t>(k);
          break;
        }
        case MI_HISTORY:   // historial del modo actual
          openHistoryViewer();
          break;
        case MI_AUTOTIMER:
          autoTimerEnabled = !autoTimerEnabled;
          autoTimerArmed   = autoTimerEnabled;
          break;
        case MI_DOSE:          // valores editables: 1 clic entra, girar ajusta, 1 clic guarda
        case MI_RATIO:
        case MI_TARGET_TIME:
        case MI_SALT_PCT:
          if (uiState == UiState::MenuEdit) {
            commitMenuEdit();
            uiState = UiState::Menu;
          } else {
            menuEditItem = item;
            uiState = UiState::MenuEdit;
          }
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
    } else if (brewMode == BrewMode::Fermento) {
      fermentClick();          // salmuera: fija la base / guarda el lote
    } else if (brewMode == BrewMode::Espresso) {
      espressoClick();         // shot: arranca / corta
    } else {
      startOrStopStopwatch();  // pour-over: cronómetro start / stop
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

  if (delta != 0) {
    uiNeedsImmediateRedraw = true;
    registerUserOrScaleActivity();

    if (uiState == UiState::PourViewer) {
      const uint8_t n = viewerCount();
      if (n > 0) {
        int8_t& slot = viewerSlot();
        slot = static_cast<int8_t>(slot + (delta > 0 ? 1 : -1));
        while (slot < 0) slot += n;
        while (slot >= n) slot -= n;
      }
    } else if (uiState == UiState::MenuEdit) {
      const float d = static_cast<float>(delta);
      if (menuEditItem == MI_SALT_PCT) {
        fermentSaltPct = constrain(roundf((fermentSaltPct + d * 0.1f) * 10.0f) / 10.0f, SALT_PCT_MIN, SALT_PCT_MAX);
      } else if (menuEditItem == MI_DOSE && brewMode == BrewMode::PourOver) {
        coffeeDoseGrams = constrain(coffeeDoseGrams + d * 0.5f, DOSE_MIN_G, DOSE_MAX_G);
      } else if (menuEditItem == MI_DOSE) {
        espDoseGrams = constrain(roundf((espDoseGrams + d * 0.1f) * 10.0f) / 10.0f, ESP_DOSE_MIN_G, ESP_DOSE_MAX_G);
      } else if (menuEditItem == MI_RATIO) {
        espRatio = constrain(roundf((espRatio + d * 0.1f) * 10.0f) / 10.0f, ESP_RATIO_MIN, ESP_RATIO_MAX);
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

// Número decimal que sigue a una orden (espera hasta 300 ms entre dígitos)
static uint32_t readSerialNumber() {
  uint32_t v = 0;
  unsigned long t0 = millis();
  while ((millis() - t0) < 300UL) {
    if (Serial.available() <= 0) { delay(1); continue; }
    const int ch = Serial.peek();
    if (ch < 0x30 || ch > 0x39) break;
    Serial.read();
    v = v * 10UL + static_cast<uint32_t>(ch - 0x30);
    t0 = millis();
  }
  return v;
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
      // Siguiente modo (pour-over -> espresso -> fermento), como Menú -> Modo
      setBrewMode(nextBrewMode(brewMode));
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
      openHistoryViewer();
      pourViewerShowStatsPage = (c == 'i' || c == 'I');
      pourSavedBannerUntilMs = 0;
      uiNeedsImmediateRedraw = true;
    } else if (c == 'm' || c == 'M') {
      uiState = UiState::Menu;
      menuSelectedIndex = 0;
      pourSavedBannerUntilMs = 0;
      uiNeedsImmediateRedraw = true;
    } else if (c == 'f' || c == 'F') {
      // Lectura simulada de la sonda de pH y temperatura
      probeInjectSimulated();
    } else if (c == 'z' || c == 'Z') {
      // Poner el reloj: z<segundos desde 1970, UTC>  (p. ej. en PowerShell: [DateTimeOffset]::UtcNow.ToUnixTimeSeconds())
      fermentSetClock(readSerialNumber());
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
