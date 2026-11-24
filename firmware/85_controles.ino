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
  executeTimemoreTare(); // Ejecutar TARA por Software (0 ms) sin activar el Zero-Lock interno de la Dot
  Serial.println("[BOTON] 2-Clicks: TARA Instantanea (0.0g) + Cronometro REINICIADO a 0.0s.");
}

void executeButtonMultiClickAction(uint8_t clicks) {
  if (clicks == 0) return;
  uiNeedsImmediateRedraw = true;
  registerUserOrScaleActivity();

  const bool inMenu = (uiState == UiState::Menu || uiState == UiState::MenuEditDose);

  // 3 CLICKS: Abrir / Cerrar Menú (o salir del visor de Pour-Over)
  if (clicks >= 3) {
    if (uiState == UiState::PourViewer) {
      uiState = UiState::Main;
    } else if (inMenu) {
      if (uiState == UiState::MenuEditDose) saveDoseToNVS();
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
      if (uiState == UiState::MenuEditDose) saveDoseToNVS();
      uiState = UiState::Main;
      triggerLedFlash();
    } else {
      tareAndResetStopwatch();
    }
    return;
  }

  // 1 CLICK: En pantalla principal -> Cronómetro Start/Stop; en Menú -> Confirmar opción; en Visor -> Cambiar página
  if (clicks == 1) {
    if (uiState == UiState::PourViewer) {
      if (savedPoursCount == 0) {
        uiState = UiState::Main;
      } else {
        // 1 Click dentro del visor de Pour-Over: alternar entre GRÁFICA y FICHA DE DATOS
        pourViewerShowStatsPage = !pourViewerShowStatsPage;
      }
    } else if (inMenu) {
      // Ejecutar opción seleccionada del menú
      switch (menuSelectedIndex) {
        case 0: // ABRIR HISTORIAL Y GRÁFICAS DE POUR-OVERS EN PSRAM
          uiState                 = UiState::PourViewer;
          pourViewerShowStatsPage = false;
          if (savedPoursCount > 0) {
            selectedPourSlot = static_cast<int8_t>(savedPoursCount - 1);
          }
          break;
        case 1: // AUTO-TIMER ON/OFF
          autoTimerEnabled = !autoTimerEnabled;
          autoTimerArmed   = autoTimerEnabled;
          break;
        case 2: // EDITAR DOSIS DE CAFÉ (entrar / confirmar y guardar en NVS)
          if (uiState == UiState::MenuEditDose) {
            saveDoseToNVS();
            uiState = UiState::Menu;
          } else {
            uiState = UiState::MenuEditDose;
          }
          break;
        case 3: // BRILLO OLED
          oledBrightnessLevel = (oledBrightnessLevel + 1) % 3;
          oledSetContrast(getActiveOledContrast());
          break;
        case 4: // PUENTE BOOKOO ON/OFF (reinicia la pila BLE: la Dot se reconecta en ~2 s)
          setBookooBridgeEnabled(!bookooBridgeEnabled);
          break;
        case 5: // RE-VINCULAR TIMEMORE DOT (Borrar NVS)
          uiState = UiState::Main;
          clearScaleFromNVS();
          break;
        case 6: // ACTUALIZAR FIRMWARE POR BLUETOOTH (página Web Bluetooth de docs/ota, PIN en pantalla)
          uiState = UiState::Main;
          enterBleOtaMode();
          break;
        case 7: // APAGAR (reposo)
          uiState = UiState::Main;
          enterDeepSleepMode("APAGADO MANUAL");
          break;
        case 8: // SALIR
        default:
          if (uiState == UiState::MenuEditDose) saveDoseToNVS();
          uiState = UiState::Main;
          break;
      }
    } else {
      // En pantalla principal: 1 Click = Cronómetro Start / Stop
      startOrStopStopwatch();
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
      if (savedPoursCount > 0) {
        selectedPourSlot = static_cast<int8_t>(selectedPourSlot + (delta > 0 ? 1 : -1));
        while (selectedPourSlot < 0) selectedPourSlot += savedPoursCount;
        while (selectedPourSlot >= savedPoursCount) selectedPourSlot -= savedPoursCount;
      }
    } else if (uiState == UiState::MenuEditDose) {
      coffeeDoseGrams += static_cast<float>(delta) * 0.5f;
      if (coffeeDoseGrams < DOSE_MIN_G) coffeeDoseGrams = DOSE_MIN_G;
      if (coffeeDoseGrams > DOSE_MAX_G) coffeeDoseGrams = DOSE_MAX_G;
      Serial.printf("[ENCODER] Dosis de cafe ajustada: %.1f g\n", coffeeDoseGrams);
    } else if (uiState == UiState::Menu) {
      menuSelectedIndex = static_cast<int8_t>(menuSelectedIndex + delta);
      while (menuSelectedIndex < 0) menuSelectedIndex += MENU_ITEMS_COUNT;
      while (menuSelectedIndex >= MENU_ITEMS_COUNT) menuSelectedIndex -= MENU_ITEMS_COUNT;
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

void handleSerialCommands() {
  while (Serial.available() > 0) {
    char c = static_cast<char>(Serial.read());
    if (c == '\r' || c == '\n') continue;
    if (c == 'x' || c == 'X') {
      // Autotest de los módulos puros: protocolo Timemore y motor de caudal (src/selftest.cpp)
      runSelfTests(selfTestPrint);
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
