// ============================================================================
// INTERFAZ: PANTALLAS
// Vistas de pesaje, menú, visor de pour-overs e icono de batería.
// Pestaña del sketch: el IDE la concatena tras firmware.ino (orden alfabético).
// Tipos, constantes y variables globales compartidas están en la pestaña principal.
// ============================================================================

// ============================================================================
// RENDERIZADO DEL VISOR DE POUR-OVERS Y MENÚ EN PANTALLA OLED (M154-4P SSD1309)
// ============================================================================
void renderPourOverViewerOled() {
  oledFillRect(0, 0, 128, 11, true);

  if (savedPoursCount == 0 || psramSavedPours == nullptr) {
    oledDrawText(4, 2, "HISTORIAL POUR-OVER", 1, true);
    oledDrawText(10, 24, "SIN POUR-OVERS", 1);
    oledDrawText(10, 36, "GUARDADOS EN PSRAM", 1);
    oledDrawText(10, 52, "CLICK: VOLVER", 1);
    return;
  }

  if (selectedPourSlot < 0) selectedPourSlot = 0;
  if (selectedPourSlot >= savedPoursCount) selectedPourSlot = static_cast<int8_t>(savedPoursCount - 1);

  const PourOverRecord& rec = psramSavedPours[selectedPourSlot];
  uint16_t minPart = rec.durationSec / 60U;
  uint16_t secPart = rec.durationSec % 60U;

  // Cabecera invertida: "#1 250G 1:16.7 02:45"
  char hdr[32];
  snprintf(hdr, sizeof(hdr), "#%u %3.0fG 1:%4.1f %02u:%02u",
           rec.pourId, rec.finalWeightGrams, rec.finalRatio, minPart, secPart);
  oledDrawText(2, 2, hdr, 1, true);

  if (!pourViewerShowStatsPage) {
    // PÁGINA A: GRÁFICA COMPLETA DEL POUR-OVER (Curva de Peso + Pulsos de Flow Rate)
    oledDrawRect(0, 13, 128, 42, true);

    for (uint8_t x = 0; x < POUR_GRAPH_POINTS; x++) {
      int px = 4 + x;
      // 1. Barras verticales punteadas del Caudal (Flow Rate) de cada vertido
      uint8_t hF = rec.flowCurve[x];
      if (hF > 38) hF = 38;
      for (uint8_t y = 0; y < hF; y += 2) {
        oledDrawPixel(px, 53 - y, true);
      }

      // 2. Línea continua gruesa (2 px) de la Curva de Peso Acumulado (Weight)
      uint8_t hW = rec.weightCurve[x];
      if (hW > 38) hW = 38;
      int py = 53 - hW;
      oledDrawPixel(px, py, true);
      if (py > 14) oledDrawPixel(px, py - 1, true);

      // Conectar verticalmente con el punto anterior para que la curva de peso sea continua
      if (x > 0) {
        int prevPy = 53 - rec.weightCurve[x - 1];
        int yMin = (py < prevPy) ? py : prevPy;
        int yMax = (py > prevPy) ? py : prevPy;
        for (int yy = yMin; yy <= yMax; yy++) {
          oledDrawPixel(px, yy, true);
        }
      }
    }

    // Pie de gráfica: Dosis, número de vertidos e índice (ej. "D:15.0G 4V [1/3] CLK:DAT")
    char foot[32];
    // Máximo 21 caracteres (128 px): "D:15.0 4V 10/10 +INFO". Antes "D:15.0G 4V [10/10] +INFO" (24) se cortaba.
    snprintf(foot, sizeof(foot), "D:%.1f %uV %d/%u +INFO",
             rec.doseGrams, rec.poursCount, selectedPourSlot + 1, savedPoursCount);
    oledDrawText(0, 56, foot, 1);

  } else {
    // PÁGINA B: FICHA DETALLADA DEL POUR-OVER (Peso, Dosis, Ratio, Caudal Medio/Pico, Vertidos)
    char line1[32], line2[32], line3[32], line4[32];
    snprintf(line1, sizeof(line1), "AGUA/BEBIDA: %5.1f G", rec.finalWeightGrams);
    snprintf(line2, sizeof(line2), "DOSIS:%4.1fG -> 1:%4.1f", rec.doseGrams, rec.finalRatio);
    snprintf(line3, sizeof(line3), "FLOW MED:%3.1f MAX:%3.1f", rec.avgFlowGps, rec.peakFlowGps);
    snprintf(line4, sizeof(line4), "VERTIDOS:%u 3CLK=SALIR", rec.poursCount); // <= 21 caracteres (128 px)

    oledDrawText(2, 15, line1, 1);
    oledDrawText(2, 27, line2, 1);
    oledDrawText(2, 39, line3, 1);
    oledDrawHLine(0, 51, 128, true);
    oledDrawText(2, 55, line4, 1);
  }
}

void renderSettingsMenuOled() {
  oledFillRect(0, 0, 128, 11, true);
  // Mostrar incluso dentro del menú el PESO en vivo en la barra superior
  char hdrBuf[22];
  float w = currentWeightGrams;
  if (fabsf(w) < 0.05f) w = 0.0f;
  snprintf(hdrBuf, sizeof(hdrBuf), "MENU  PESO:%5.1fG", w);
  oledDrawText(4, 2, hdrBuf, 1, true);

  char histLabel[24];
  snprintf(histLabel, sizeof(histLabel), "1.POUR-OVERS (%u/%u)", savedPoursCount, MAX_SAVED_POURS);

  const char* labels[MENU_ITEMS_COUNT] = {
    histLabel,
    autoTimerEnabled ? "2.AUTO-TMR: ON" : "2.AUTO-TMR: OFF",
    "3.DOSIS:",
    (oledBrightnessLevel == 2) ? "4.BRILLO: ALTO" : ((oledBrightnessLevel == 1) ? "4.BRILLO: MED" : "4.BRILLO: BAJO"),
    bookooBridgeEnabled ? "5.PUENTE BOOKOO: ON" : "5.PUENTE BOOKOO: OFF",
    "6.RE-VINCULAR DOT",
    "7.ACTUALIZAR (BLE)",
    "8.APAGAR (SLEEP)",
    "9.SALIR A PANTALLA"
  };

  // Mostrar ventana deslizante de 4 elementos visibles a la vez
  int8_t firstVis = menuSelectedIndex - 1;
  if (firstVis < 0) firstVis = 0;
  if (firstVis > MENU_ITEMS_COUNT - 4) firstVis = MENU_ITEMS_COUNT - 4;

  for (int8_t i = 0; i < 4; i++) {
    int8_t itemIdx = firstVis + i;
    int yPos = 14 + i * 12;
    bool selected = (itemIdx == menuSelectedIndex);

    if (selected) {
      oledFillRect(0, yPos - 1, 128, 10, true);
    }

    char lineBuf[24];
    if (itemIdx == 2) {
      snprintf(lineBuf, sizeof(lineBuf), "3.DOSIS: %4.1fG%s",
               coffeeDoseGrams, (uiState == UiState::MenuEditDose && selected) ? " <" : "");
      oledDrawText(4, yPos, lineBuf, 1, selected);
    } else {
      oledDrawText(4, yPos, labels[itemIdx], 1, selected);
    }
  }
}

// Icono de 13x7 px de la batería del propio display (18650 vía divisor en GPIO 4).
// - Relleno proporcional al % ; parpadea por debajo del 10 %.
// - Cargando (pin CHRG del TP4056 en LOW o > 4.21 V): relleno animado.
// - Sin divisor de batería soldado: no se dibuja.
void drawLocalBatteryIcon(int x, int y) {
  if (!localBattSensorPresent || !localBattHasReading) return;
  oledDrawRect(x, y, 11, 7, true);
  oledFillRect(x + 11, y + 2, 2, 3, true);  // borne
  unsigned long t = millis();
  int fill;
  if (localBattCharging) {
    fill = static_cast<int>((t / 350UL) % 10UL);  // 0..9 animado
  } else {
    fill = (static_cast<int>(localBattPct) * 9 + 50) / 100;
    if (localBattPct <= 10 && ((t / 500UL) % 2UL) == 0) fill = 0;  // aviso de batería baja
  }
  if (fill > 9) fill = 9;
  if (fill > 0) oledFillRect(x + 1, y + 1, fill, 5, true);
}

// ============================================================================
// VISTA "MODO PUENTE": la máquina (Wendougee Lita) manda la báscula por el puente BooKoo.
// La pantalla de pour-over no aporta nada aquí; se muestra el estado de los dos enlaces, el peso de
// respaldo y lo que hace la máquina (últimos comandos y rebase tras el STOP).
//   M15MS -58 D15MS -61      <- cabecera invertida: intervalo y RSSI de Máquina y Dot
//    36.4 G        2.1G/S
//                  >24.3S    <- '>' = cronómetro en marcha (lo manda la máquina)
//   TX10.0 AGE  4/12     ?1  <- tramas/s, edad del peso enviado (última/máx. 1 s) y comandos raros
//   STOP    36.2 +1.4   3S   <- STOP: peso al parar y rebase medido a los 3 s
//   T+START             28S
// ============================================================================
static const char* bookooCmdLabel(uint8_t c, char* tmp, size_t cap) {
  switch (c) {
    case bkproto::CMD_TARE:        return "TARE";
    case bkproto::CMD_TIMER_START: return "START";
    case bkproto::CMD_TIMER_STOP:  return "STOP";
    case bkproto::CMD_TIMER_RESET: return "RESET";
    case bkproto::CMD_TARE_START:  return "T+START";
    case bkproto::CMD_SMOOTHING:   return "SMOOTH";
    default: snprintf(tmp, cap, "CMD%02X", c); return tmp;
  }
}

// "M15MS -58" (9 caracteres); "--" si aún no hay dato
static void formatBookooLink(char* out, size_t cap, char tag, float itvlMs, int rssi) {
  char itvl[4], rs[5];
  if (itvlMs > 0.5f && itvlMs < 99.5f) snprintf(itvl, sizeof(itvl), "%2.0f", itvlMs);
  else snprintf(itvl, sizeof(itvl), "--");
  if (rssi < 0) snprintf(rs, sizeof(rs), "%4d", rssi);
  else snprintf(rs, sizeof(rs), "  --");
  snprintf(out, cap, "%c%sMS%s", tag, itvl, rs);
}

void renderBookooBridgeOled() {
  unsigned long now = millis();
  char buf[24];

  // 1. Cabecera invertida: enlace con la Máquina y con la Dot
  float dotItvl = 0.0f;
  if (scaleConnected && pScaleClient != nullptr && pScaleClient->isConnected()) {
    dotItvl = pScaleClient->getConnInfo().getConnInterval() * 1.25f;
  }
  char m[12], d[12];
  if (bookooSubscribed) formatBookooLink(m, sizeof(m), 'M', bookooMachineItvlMs, bookooMachineRssi);
  else snprintf(m, sizeof(m), "M:SIN SUB");
  if (scaleConnected) formatBookooLink(d, sizeof(d), 'D', dotItvl, bookooDotRssi);
  else snprintf(d, sizeof(d), "D:BUSCANDO");
  oledFillRect(0, 0, 128, 9, true);
  oledDrawText(1, 1, m, 1, true);
  oledDrawText(127 - 6 * static_cast<int>(strlen(d)), 1, d, 1, true);

  // 2. Peso de respaldo (2x), caudal y cronómetro
  float w = currentWeightGrams;
  if (fabsf(w) < 0.05f) w = 0.0f;
  snprintf(buf, sizeof(buf), "%6.1f", w);
  oledDrawText(0, 12, buf, 2);
  oledDrawText(74, 19, "G", 1);

  snprintf(buf, sizeof(buf), "%4.1fG/S", fabsf(currentFlowRateGps));
  oledDrawText(86, 12, buf, 1);

  unsigned long totalMs = timerAccumulatedMs;
  if (timerRunning) totalMs += (now - timerStartMillis);
  if (totalMs < 1000000UL) {
    snprintf(buf, sizeof(buf), "%c%5.1fS", timerRunning ? '>' : ' ', totalMs / 1000.0f);
  } else {
    snprintf(buf, sizeof(buf), "%c%5luS", timerRunning ? '>' : ' ', totalMs / 1000UL);
  }
  oledDrawText(86, 21, buf, 1);

  // 3. Envío a la máquina: tramas/s y edad del peso (última / máxima del último segundo)
  uint32_t ageLast = bookooLastAgeMs, ageMax = bookooAgeMaxShown;
  if (ageLast > 999) ageLast = 999;
  if (ageMax > 999) ageMax = 999;
  snprintf(buf, sizeof(buf), "TX%4.1f AGE%3lu/%lu", bookooTxRate, (unsigned long)ageLast, (unsigned long)ageMax);
  const bool ageWarn = bookooSubscribed && ageMax > BOOKOO_AGE_WARN_MS;
  if (ageWarn) oledFillRect(0, 29, 128, 9, true);   // hueco de la Dot o atasco: línea en negativo
  oledDrawText(0, 30, buf, 1, ageWarn);
  uint32_t bad = bookooBadCmds;
  if (bad > 0) {
    snprintf(buf, sizeof(buf), "?%lu", (unsigned long)(bad > 99 ? 99 : bad));
    oledDrawText(128 - 6 * static_cast<int>(strlen(buf)), 30, buf, 1, ageWarn);
  }
  oledDrawHLine(0, 38, 128, true);

  // 4. Últimos comandos de la máquina (el más reciente arriba) con su antigüedad
  if (bookooCmdLogCount == 0) {
    oledDrawText(0, 48, bookooSubscribed ? "ESPERANDO COMANDOS" : "SIN SUSCRIPCION", 1);
    return;
  }
  for (uint8_t i = 0; i < bookooCmdLogCount; i++) {
    const BookooCmdLogEntry& e = bookooCmdLog[i];
    const int y = 40 + 8 * i;
    char tmp[8];
    oledDrawText(0, y, bookooCmdLabel(e.cmd, tmp, sizeof(tmp)), 1);
    if (e.cmd == bkproto::CMD_TIMER_STOP) {
      snprintf(buf, sizeof(buf), "%5.1f", e.weight);
      oledDrawText(44, y, buf, 1);
      if (e.hasOvershoot) snprintf(buf, sizeof(buf), "%+4.1f", e.overshoot);
      else snprintf(buf, sizeof(buf), " ...");
      oledDrawText(78, y, buf, 1);
    }
    unsigned long ageS = (now - e.ms) / 1000UL;
    if (ageS < 100) snprintf(buf, sizeof(buf), "%2luS", ageS);
    else snprintf(buf, sizeof(buf), "%2luM", (ageS / 60UL) > 99 ? 99UL : ageS / 60UL);
    oledDrawText(110, y, buf, 1);
  }
}

void renderOledScreen() {
  if (!oledPresent) return;

  // Muestrear historial para la gráfica en vivo de caudal cada 250 ms
  unsigned long now = millis();
  if (now - lastFlowGraphSampleMs >= 250UL) {
    lastFlowGraphSampleMs = now;
    float clamped = fabsf(currentFlowRateGps);
    if (clamped > 5.0f) clamped = 5.0f;
    // Escalar de 0..5.0 g/s a 0..18 píxeles de altura
    flowGraphHistory[flowGraphHead] = static_cast<uint8_t>((clamped / 5.0f) * 18.0f);
    flowGraphHead = (flowGraphHead + 1) & 0x3F;
  }

  oledClear();

  // Banner flotante cuando se acaba de guardar un Pour-Over en PSRAM + Flash NVS
  if (now < pourSavedBannerUntilMs) {
    oledDrawRect(0, 0, 128, 64, true);
    oledFillRect(2, 2, 124, 14, true);
    char bHdr[22];
    snprintf(bHdr, sizeof(bHdr), "POUR #%u GUARDADO!", lastSavedPourId);
    oledDrawText(12, 5, bHdr, 1, true);

    if (savedPoursCount > 0 && psramSavedPours != nullptr) {
      const PourOverRecord& rec = psramSavedPours[savedPoursCount - 1];
      char l1[22], l2[22];
      snprintf(l1, sizeof(l1), "PESO:%5.1fG (1:%.1f)", rec.finalWeightGrams, rec.finalRatio);
      snprintf(l2, sizeof(l2), "DOSIS:%4.1fG T:%u S", rec.doseGrams, rec.durationSec);
      oledDrawText(4, 24, l1, 1);  // 20 caracteres: cabe entre los bordes del marco
      oledDrawText(4, 40, l2, 1);
    }
    oledFlush();
    return;
  }

  if (uiState == UiState::PourViewer) {
    renderPourOverViewerOled();
    oledFlush();
    return;
  }

  if (uiState == UiState::Menu || uiState == UiState::MenuEditDose) {
    renderSettingsMenuOled();
    oledFlush();
    return;
  }

  // Vista de diagnóstico del puente (solo con la máquina conectada; si se fue, vuelta a la principal)
  if (currentScreenView >= NUM_SCREEN_VIEWS) {
    if (bookooBridgeEnabled && bookooMachineConnected) {
      renderBookooBridgeOled();
      oledFlush();
      return;
    }
    currentScreenView = 0;
  }

  // 1. BARRA SUPERIOR (y = 0..9):
  //    - En Vista 0 (Pantalla Principal: Dosis + Ratio):
  //      * Si la Timemore Dot está conectada: Muestra a la izquierda el valor compacto "X.XG/S" (36 px)
  //        y a la derecha (x = 38..127, 90 px, en lugar de la batería) un Medidor Gráfico Barista de Pour
  //        (escala 0..10 g/s) con marcas de "Zona Ideal / Sweet Spot" entre 3.5 g/s y 7.5 g/s.
  //      * Si se desconecta: Oculta el flow/gráfico y muestra "BUSCANDO DOT...".
  if (scaleConnected) {
    if (currentScreenView == 0) {
      float shownFlow = fabsf(displayedFlowRateGps);
      if (shownFlow > 15.0f) shownFlow = 15.0f;

      char topFlowBuf[10];
      if (shownFlow < 10.0f) {
        snprintf(topFlowBuf, sizeof(topFlowBuf), "%3.1fG/S", shownFlow);
      } else {
        snprintf(topFlowBuf, sizeof(topFlowBuf), "%2.0f.G/S", shownFlow);
      }
      oledDrawText(0, 1, topFlowBuf, 1);

      // Medidor Gráfico Barista de Caudal (desde x = 38, escala 0.0 .. 10.0 g/s). Si hay divisor de
      // batería, el medidor se acorta (76 px) para dejar sitio al icono de batería en x = 115..127.
      // - Zona Ideal Pour-Over (Sweet Spot 3.5 .. 7.5 g/s): muescas + puntos guía
      // - Por encima de 7.5 g/s la barra pasa a rayada (vertido demasiado rápido)
      const int gaugeW = localBattSensorPresent ? 76 : 90;
      const int innerW = gaugeW - 4;                      // zona útil desde x = 40
      const int markLo = 40 + (innerW * 35) / 100;        // 3.5 g/s
      const int markHi = 40 + (innerW * 75) / 100;        // 7.5 g/s
      oledDrawRect(38, 1, gaugeW, 8, true);
      for (int m = 0; m < 2; m++) {
        int mx = (m == 0) ? markLo : markHi;
        oledDrawPixel(mx, 0, true);
        oledDrawPixel(mx, 2, true);
        oledDrawPixel(mx, 7, true);
        oledDrawPixel(mx, 9, true);
      }
      for (int gx = markLo + 4; gx <= markHi - 4; gx += 4) {
        oledDrawPixel(gx, 4, true);
      }

      // Barra proporcional de caudal en vivo (0.0 a 10.0 g/s -> 0 a innerW px)
      float barFlow = (shownFlow > 10.0f) ? 10.0f : shownFlow;
      int fillW = static_cast<int>((barFlow / 10.0f) * static_cast<float>(innerW) + 0.5f);
      if (fillW > innerW) fillW = innerW;
      const int solidMax = markHi - 40;

      if (fillW > 0) {
        int solidW = (fillW <= solidMax) ? fillW : solidMax;
        oledFillRect(40, 3, solidW, 4, true);
        if (fillW > solidMax) {
          for (int px = solidMax; px < fillW; px++) {
            if ((px & 1) == 0) {
              oledFillRect(40 + px, 3, 1, 4, true);
            }
          }
        }
      }
    } else {
      // "DOT" + indicador del puente BooKoo: "bk" = anunciando, "BK" en negativo = máquina conectada
      if (bookooBridgeEnabled) {
        oledDrawText(0, 1, "DOT", 1);
        if (bookooMachineConnected) {
          oledFillRect(20, 0, 15, 9, true);
          oledDrawText(22, 1, "BK", 1, true);
        } else {
          oledDrawText(22, 1, "bk", 1);
        }
      } else {
        oledDrawText(0, 1, "DOT:OK", 1);
      }
      if (fabsf(currentFlowRateGps) >= 0.15f) {
        oledDrawText(40, 1, "[FLOW]", 1);
      } else if (weightIsStable) {
        oledDrawText(40, 1, "[STBL]", 1);
      } else {
        oledDrawText(40, 1, "[MOVE]", 1);
      }

      // Batería de la báscula Dot (en texto); la del propio display va como icono a la derecha
      char battBuf[10];
      snprintf(battBuf, sizeof(battBuf), "%3u%%", scaleBatteryPct);
      oledDrawText(localBattSensorPresent ? 84 : 104, 1, battBuf, 1);
    }
  } else {
    oledDrawText(0, 1, "BUSCANDO DOT...", 1);
  }
  drawLocalBatteryIcon(115, 1);
  oledDrawHLine(0, 10, 128, true);

  // 2. PESO EN TIEMPO REAL (Protagonista en TODAS las vistas, con espacio limpio antes de "G")
  char weightBuf[12];
  float w = currentWeightGrams;
  if (fabsf(w) < 0.05f) w = 0.0f;
  snprintf(weightBuf, sizeof(weightBuf), "%5.1f", w);

  // Cronómetro común para las vistas
  unsigned long totalMs = timerAccumulatedMs;
  if (timerRunning) {
    totalMs += (now - timerStartMillis);
  }
  unsigned long totalSec = totalMs / 1000UL;
  uint8_t tenths = static_cast<uint8_t>((totalMs % 1000UL) / 100UL);
  char timeBuf[12];
  if (totalSec < 100) {
    snprintf(timeBuf, sizeof(timeBuf), "%2lu.%u S", totalSec, tenths);
  } else {
    snprintf(timeBuf, sizeof(timeBuf), "%02lu:%02lu", (totalSec / 60UL) % 100UL, totalSec % 60UL);
  }

  if (currentScreenView == 1) {
    // ========================================================================
    // VISTA 1: MODO BÁSCULA PURA (PESO GIGANTE EN CENTRO DE PANTALLA)
    // ========================================================================
    char bigWeightBuf[10];
    if (w > -10.0f && w < 100.0f) {
      // 4 caracteres a escala 4x (96 px de ancho total: x=4..96) + espacio amplio antes de "G" en x=114
      snprintf(bigWeightBuf, sizeof(bigWeightBuf), "%4.1f", w);
      oledDrawText(4, 18, bigWeightBuf, 4);
    } else {
      // Para pesos >= 100.0 g (5 caracteres: 90 px en escala 3x: x=8..95) manteniendo espacio limpio antes de "G"
      snprintf(bigWeightBuf, sizeof(bigWeightBuf), "%5.1f", w);
      oledDrawText(8, 21, bigWeightBuf, 3);
    }
    oledDrawText(114, 28, "G", 2);
    oledDrawHLine(0, 52, 128, true);
    oledDrawText(0, 55, "1C:TMR 2C:TARA", 1);
    oledDrawText(86, 55, timeBuf, 1);
    oledFlush();
    return;
  }

  // En las Vistas 0, 2 y 3: PESO GRANDE (5 chars en escala 3x = 90 px: x=4..91) + espacio limpio antes de "G" en x=110
  oledDrawText(4, 15, weightBuf, 3);
  oledDrawText(110, 22, "G", 2);

  // 3. ZONA INFERIOR COMPLEMENTARIA SEGÚN LA VISTA SELECCIONADA CON EL ENCODER:
  if (currentScreenView == 0) {
    // VISTA 0 (PANTALLA PRINCIPAL POR DEFECTO): PESO GRANDE + FLOW ARRIBA + RATIO BARISTA (1:X) + DOSIS + CRONÓMETRO
    oledDrawHLine(0, 41, 128, true);
    float ratio = (coffeeDoseGrams > 0.1f && w > 0.0f) ? (w / coffeeDoseGrams) : 0.0f;
    if (ratio > 99.9f) ratio = 99.9f;

    char doseBuf[16];
    snprintf(doseBuf, sizeof(doseBuf), "DOSE:%4.1fG", coffeeDoseGrams);
    oledDrawText(0, 45, doseBuf, 1);

    char ratioBuf[16];
    snprintf(ratioBuf, sizeof(ratioBuf), "1:%4.1f", ratio);
    oledDrawText(0, 54, ratioBuf, 1);

    for (int yy = 42; yy < 64; yy++) oledDrawPixel(70, yy, true);

    oledDrawText(74, 45, autoTimerArmed ? "TIMER(A)" : "TIMER", 1);
    oledDrawText(74, 55, timeBuf, 1);

  } else if (currentScreenView == 2) {
    // VISTA 2: PESO GRANDE + BARRA DE FLUJO + FLOW RATE (g/s) + CRONÓMETRO
    oledDrawRect(0, 39, 128, 5, true);
    float clampedFlow = fabsf(currentFlowRateGps);
    if (clampedFlow > 5.0f) clampedFlow = 5.0f;
    int barWidth = static_cast<int>((clampedFlow / 5.0f) * 124.0f);
    if (barWidth > 0) {
      oledFillRect(2, 41, barWidth, 1, true);
    }

    char flowBuf[12];
    snprintf(flowBuf, sizeof(flowBuf), "%4.1f", fabsf(currentFlowRateGps));
    oledDrawText(0, 49, flowBuf, 2);
    oledDrawText(50, 56, "G/S", 1);

    for (int yy = 45; yy < 64; yy++) oledDrawPixel(70, yy, true);

    oledDrawText(74, 46, autoTimerArmed ? "TIMER(A)" : "TIMER", 1);
    oledDrawText(74, 55, timeBuf, 1);

  } else {
    // VISTA 3: PESO GRANDE + CURVA GRÁFICA EN VIVO DE FLOW RATE (64 muestras x 18px) + CRONÓMETRO
    oledDrawRect(0, 42, 68, 22, true);
    for (uint8_t x = 0; x < 64; x++) {
      uint8_t idx = (flowGraphHead + x) & 0x3F;
      uint8_t h = flowGraphHistory[idx];
      if (h > 18) h = 18;
      for (uint8_t yy = 0; yy <= h; yy++) {
        oledDrawPixel(2 + x, 62 - yy, true);
      }
    }
    oledDrawText(74, 44, "FLOW GPH", 1);
    oledDrawText(74, 55, timeBuf, 1);
  }

  oledFlush();
}
