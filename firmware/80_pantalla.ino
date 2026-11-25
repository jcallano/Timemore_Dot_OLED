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

    // Pie de gráfica: dosis, vertidos (B+n = bloom + n vertidos) e índice. Máximo 21 caracteres (128 px):
    // "15.0G B+3 10/10 +INFO". Antes "D:15.0G 4V [10/10] +INFO" (24) se cortaba.
    char foot[32];
    char pours[8];
    if (rec.hasBloom) snprintf(pours, sizeof(pours), "B+%u", (unsigned)(rec.poursCount - 1));
    else              snprintf(pours, sizeof(pours), "%uV", (unsigned)rec.poursCount);
    snprintf(foot, sizeof(foot), "%.1fG %s %d/%u +INFO",
             rec.doseGrams, pours, selectedPourSlot + 1, savedPoursCount);
    oledDrawText(0, 56, foot, 1);

  } else {
    // PÁGINA B: FICHA DETALLADA DEL POUR-OVER (Peso, Dosis, Ratio, Caudal Medio/Pico, Vertidos)
    char line1[32], line2[32], line3[32], line4[32];
    snprintf(line1, sizeof(line1), "AGUA/BEBIDA: %5.1f G", rec.finalWeightGrams);
    snprintf(line2, sizeof(line2), "DOSIS:%4.1fG -> 1:%4.1f", rec.doseGrams, rec.finalRatio);
    snprintf(line3, sizeof(line3), "FLOW MED:%3.1f MAX:%3.1f", rec.avgFlowGps, rec.peakFlowGps);
    // <= 21 caracteres (128 px)
    if (rec.hasBloom) snprintf(line4, sizeof(line4), "BLOOM+%u 3CLK=SALIR", (unsigned)(rec.poursCount - 1));
    else              snprintf(line4, sizeof(line4), "VERTIDOS:%u 3CLK=SALIR", (unsigned)rec.poursCount);

    oledDrawText(2, 15, line1, 1);
    oledDrawText(2, 27, line2, 1);
    oledDrawText(2, 39, line3, 1);
    oledDrawHLine(0, 51, 128, true);
    oledDrawText(2, 55, line4, 1);
  }
}

// Opción del menú que se está editando (dosis, ratio o tiempo) en UiState::MenuEdit
uint8_t menuEditItem = MI_DOSE;

// Opciones del menú según el modo. Devuelve cuántas hay. Orden: lo de cada día arriba (receta,
// historial, modo) y lo técnico abajo.
uint8_t buildMenuItems(uint8_t* ids) {
  uint8_t n = 0;
  if (brewMode == BrewMode::Espresso) {
    ids[n++] = MI_DOSE;
    ids[n++] = MI_RATIO;
    ids[n++] = MI_TARGET_TIME;
  } else if (brewMode == BrewMode::Fermento) {
    ids[n++] = MI_SALT_PCT;
  } else {
    ids[n++] = MI_DOSE;
    ids[n++] = MI_AUTOTIMER;
  }
  ids[n++] = MI_HISTORY;
  ids[n++] = MI_MODE;
  ids[n++] = MI_BRIGHT;
  ids[n++] = MI_BRIDGE;
  ids[n++] = MI_RELINK;
  ids[n++] = MI_UPDATE;
  ids[n++] = MI_SLEEP;
  ids[n++] = MI_EXIT;
  return n;
}

// Texto de una opción con su número ("4.RATIO 1:2.0=36.0G<"). Máximo 21 caracteres (128 px).
static void menuItemLabel(uint8_t id, uint8_t num, bool editing, char* out, size_t cap) {
  const bool esp = (brewMode == BrewMode::Espresso);
  const char* mark = editing ? " <" : "";
  switch (id) {
    case MI_MODE:        snprintf(out, cap, "%u.MODO: %s", num, brewModeName(brewMode)); break;
    case MI_SALT_PCT:    snprintf(out, cap, "%u.SAL: %.1f%%%s", num, fermentSaltPct, mark); break;
    case MI_HISTORY:
      if (brewMode == BrewMode::Fermento) snprintf(out, cap, "%u.LOTES (%u/%u)", num, fermentLotsCount, MAX_FERMENT_LOTS);
      else if (esp) snprintf(out, cap, "%u.SHOTS (%u/%u)", num, savedShotsCount, MAX_SAVED_SHOTS);
      else     snprintf(out, cap, "%u.POUR-OVERS (%u/%u)", num, savedPoursCount, MAX_SAVED_POURS);
      break;
    case MI_AUTOTIMER:   snprintf(out, cap, "%u.AUTO-TMR: %s", num, autoTimerEnabled ? "ON" : "OFF"); break;
    case MI_DOSE:        snprintf(out, cap, "%u.DOSIS: %.1fG%s", num, esp ? espDoseGrams : coffeeDoseGrams, mark); break;
    case MI_RATIO:       snprintf(out, cap, "%u.RATIO 1:%.1f=%.1fG%s", num, espRatio, espressoTargetGrams(), editing ? "<" : ""); break;
    case MI_TARGET_TIME: snprintf(out, cap, "%u.TIEMPO: %u S%s", num, espTargetSec, mark); break;
    case MI_BRIGHT:
      snprintf(out, cap, "%u.BRILLO: %s", num, oledBrightnessLevel == 2 ? "ALTO" : (oledBrightnessLevel == 1 ? "MED" : "BAJO"));
      break;
    case MI_BRIDGE:      snprintf(out, cap, "%u.PUENTE BOOKOO: %s", num, bookooBridgeEnabled ? "ON" : "OFF"); break;
    case MI_RELINK:      snprintf(out, cap, "%u.RE-VINCULAR DOT", num); break;
    case MI_UPDATE:      snprintf(out, cap, "%u.ACTUALIZAR (BLE)", num); break;
    case MI_SLEEP:       snprintf(out, cap, "%u.APAGAR (SLEEP)", num); break;
    default:             snprintf(out, cap, "%u.SALIR A PANTALLA", num); break;
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

  uint8_t ids[MENU_MAX_ITEMS];
  const int8_t n = static_cast<int8_t>(buildMenuItems(ids));
  if (menuSelectedIndex >= n) menuSelectedIndex = 0;

  // Ventana deslizante de 4 opciones visibles a la vez
  int8_t firstVis = menuSelectedIndex - 1;
  if (firstVis > n - 4) firstVis = n - 4;
  if (firstVis < 0) firstVis = 0;

  for (int8_t i = 0; i < 4 && firstVis + i < n; i++) {
    const int8_t itemIdx = firstVis + i;
    const int yPos = 14 + i * 12;
    const bool selected = (itemIdx == menuSelectedIndex);
    if (selected) oledFillRect(0, yPos - 1, 128, 10, true);
    char lineBuf[32];
    menuItemLabel(ids[itemIdx], static_cast<uint8_t>(itemIdx + 1),
                  selected && uiState == UiState::MenuEdit, lineBuf, sizeof(lineBuf));
    oledDrawText(4, yPos, lineBuf, 1, selected);
  }
}

// ============================================================================
// MODO ESPRESSO: vista del shot y visor de shots
// ============================================================================
//   1.8G/S    18.0>36.0G 28S     caudal | receta (dosis > objetivo, tiempo objetivo)
//     24.6 G                     peso
//   [##########|    ]            progreso hacia el objetivo; la marca es el punto de corte
//   1:1.37     | SHOT            ratio actual | estado
//   PI 6.2S    | 18.4 S          preinfusión (hasta la primera gota) | cronómetro
void renderEspressoViewOled(const char* timeBuf) {
  const unsigned long now = millis();
  // Barra superior
  if (scaleConnected) {
    char fb[10];
    snprintf(fb, sizeof(fb), "%.1fG/S", fabsf(displayedFlowRateGps));
    oledDrawText(0, 1, fb, 1);
    char rb[22];
    snprintf(rb, sizeof(rb), "%.1f>%.1fG %uS", espDoseGrams, espressoTargetGrams(), espTargetSec);
    oledDrawText(128 - 6 * static_cast<int>(strlen(rb)), 1, rb, 1);
  } else {
    oledDrawText(0, 1, "BUSCANDO DOT...", 1);
  }
  oledDrawHLine(0, 10, 128, true);

  // Peso
  float w = currentWeightGrams;
  if (fabsf(w) < 0.05f) w = 0.0f;
  char wb[12];
  snprintf(wb, sizeof(wb), "%5.1f", w);
  oledDrawText(4, 14, wb, 3);
  oledDrawText(110, 21, "G", 2);

  // Progreso hacia el objetivo, con la marca del punto de corte (objetivo menos el goteo aprendido)
  const float target = espressoTargetGrams();
  const float cut    = espressoCutGrams();
  oledDrawRect(0, 37, 128, 7, true);
  int fill = (target > 0.1f) ? static_cast<int>((w / target) * 124.0f) : 0;
  if (fill < 0) fill = 0;
  if (fill > 124) fill = 124;
  const bool inCutZone = (shotPhase == ShotPhase::Running && w >= cut);
  if (fill > 0 && !(inCutZone && ((now / 200UL) % 2UL) == 0)) oledFillRect(2, 39, fill, 3, true);
  int cutX = (target > 0.1f) ? 2 + static_cast<int>((cut / target) * 124.0f) : 126;
  if (cutX < 2) cutX = 2;
  if (cutX > 125) cutX = 125;
  oledDrawPixel(cutX, 44, true); oledDrawPixel(cutX, 45, true);   // marca del corte (solo debajo: arriba toca la G)
  oledDrawPixel(cutX - 1, 45, true); oledDrawPixel(cutX + 1, 45, true);

  // Ratio y preinfusión | estado y cronómetro
  for (int yy = 47; yy < 64; yy++) oledDrawPixel(70, yy, true);
  char l[16];
  const float ratio = (espDoseGrams > 0.1f && w > 0.0f) ? w / espDoseGrams : 0.0f;
  snprintf(l, sizeof(l), "1:%.2f", ratio);
  oledDrawText(0, 48, l, 1);
  if (shotPhase != ShotPhase::Idle && shotPiKnown && shotFirstDropMs != 0) {
    snprintf(l, sizeof(l), "PI %.1fS", (shotFirstDropMs - shotStartMs) / 1000.0f);
  } else if (shotPhase == ShotPhase::Running && shotPiKnown) {
    snprintf(l, sizeof(l), "PI ...");
  } else {
    snprintf(l, sizeof(l), "PI --");
  }
  oledDrawText(0, 57, l, 1);

  const char* st;
  if (shotPhase == ShotPhase::Running)       st = inCutZone ? "CORTA!" : (w >= cut - 3.0f ? "CASI" : "SHOT");
  else if (shotPhase == ShotPhase::Settling) st = "GOTEO";
  else if (espressoArmed())                  st = "LISTO";
  else                                       st = "ESPRESSO";
  oledDrawText(74, 48, st, 1);
  oledDrawText(74, 57, timeBuf, 1);
}

// Visor de shots: gráfica (peso, caudal, objetivo y primera gota) o ficha
void renderShotViewerOled() {
  oledFillRect(0, 0, 128, 11, true);
  if (savedShotsCount == 0) {
    oledDrawText(4, 2, "HISTORIAL DE SHOTS", 1, true);
    oledDrawText(10, 24, "SIN SHOTS", 1);
    oledDrawText(10, 36, "GUARDADOS", 1);
    oledDrawText(10, 52, "CLICK: VOLVER", 1);
    return;
  }
  if (selectedShotSlot < 0) selectedShotSlot = 0;
  if (selectedShotSlot >= savedShotsCount) selectedShotSlot = static_cast<int8_t>(savedShotsCount - 1);
  const ShotRecord& r = savedShots[selectedShotSlot];
  const float ratio = (r.doseGrams > 0.1f) ? r.yieldGrams / r.doseGrams : 0.0f;
  const bool piKnown = (r.flags & SHOT_FLAG_PI_KNOWN) != 0;

  char hdr[32];
  snprintf(hdr, sizeof(hdr), "#%u %.1fG 1:%.1f %.1fS", r.shotId, r.yieldGrams, ratio, r.timeDs / 10.0f);
  oledDrawText(2, 2, hdr, 1, true);

  if (!pourViewerShowStatsPage) {
    oledDrawRect(0, 13, 128, 42, true);
    // Línea del objetivo (punteada), a la misma escala que la curva de peso
    float maxW = (r.yieldGrams > r.targetGrams ? r.yieldGrams : r.targetGrams) * 1.05f;
    if (maxW < 10.0f) maxW = 10.0f;
    int ty = 53 - static_cast<int>((r.targetGrams / maxW) * 38.0f);
    for (int x = 3; x < 125; x += 3) oledDrawPixel(x, ty, true);
    // Primera gota (vertical punteada)
    if (piKnown && r.timeDs > 0) {
      int px = 4 + static_cast<int>((static_cast<float>(r.piDs) / r.timeDs) * 118.0f);
      for (int y = 15; y < 54; y += 3) oledDrawPixel(px, y, true);
    }
    int prevPy = -1;
    for (uint8_t i = 0; i < SHOT_GRAPH_POINTS; i++) {
      const int px = 4 + i * 2;
      uint8_t hF = r.flowCurve[i] > 38 ? 38 : r.flowCurve[i];
      for (uint8_t y = 0; y < hF; y += 2) { oledDrawPixel(px, 53 - y, true); }
      uint8_t hW = r.weightCurve[i] > 38 ? 38 : r.weightCurve[i];
      const int py = 53 - hW;
      for (int dx = 0; dx < 2; dx++) {
        oledDrawPixel(px + dx, py, true);
        if (py > 14) oledDrawPixel(px + dx, py - 1, true);
      }
      if (prevPy >= 0) {
        const int y0 = py < prevPy ? py : prevPy, y1 = py > prevPy ? py : prevPy;
        for (int yy = y0; yy <= y1; yy++) oledDrawPixel(px, yy, true);
      }
      prevPy = py;
    }
    char foot[32];
    if (piKnown) snprintf(foot, sizeof(foot), "PI %.1fS %d/%u +INFO", r.piDs / 10.0f, selectedShotSlot + 1, savedShotsCount);
    else         snprintf(foot, sizeof(foot), "PI -- %d/%u +INFO", selectedShotSlot + 1, savedShotsCount);
    oledDrawText(0, 56, foot, 1);
  } else {
    char l1[32], l2[32], l3[32], l4[32];
    snprintf(l1, sizeof(l1), "BEBIDA:%.1fG OBJ:%.0f", r.yieldGrams, r.targetGrams);
    snprintf(l2, sizeof(l2), "DOSIS:%.1fG -> 1:%.2f", r.doseGrams, ratio);
    if (piKnown) snprintf(l3, sizeof(l3), "TIEMPO:%.1fS PI:%.1fS", r.timeDs / 10.0f, r.piDs / 10.0f);
    else         snprintf(l3, sizeof(l3), "TIEMPO:%.1fS PI:--", r.timeDs / 10.0f);
    const float reb = r.yieldGrams - r.weightAtStop;
    if (r.flags & SHOT_FLAG_AUTOSTOP)     snprintf(l4, sizeof(l4), "FIN AUTOMATICO");
    else if (r.flags & SHOT_FLAG_MACHINE) snprintf(l4, sizeof(l4), "CORTE:CAFETERA %+.1fG", reb);
    else                                  snprintf(l4, sizeof(l4), "CORTE:MANUAL %+.1fG", reb);
    oledDrawText(2, 15, l1, 1);
    oledDrawText(2, 27, l2, 1);
    oledDrawText(2, 39, l3, 1);
    oledDrawHLine(0, 51, 128, true);
    oledDrawText(2, 55, l4, 1);
  }
}

// ============================================================================
// MODO FERMENTO: calculadora de salmuera y visor de lotes
// ============================================================================
void drawLocalBatteryIcon(int x, int y);

// Línea superior: la sonda (pH y temperatura) si hay lectura reciente; si no, el % de sal
static void drawFermentTopBar() {
  accproto::Reading pr;
  char top[24];
  if (probeGetFresh(pr, nullptr) && (pr.phValid() || pr.tempValid())) {
    char ph[10] = "PH --", tc[10] = "";
    if (pr.phValid()) snprintf(ph, sizeof(ph), "PH %.2f", pr.ph);
    if (pr.tempValid()) snprintf(tc, sizeof(tc), "%.1fC", pr.tempC);
    snprintf(top, sizeof(top), "%s %s%s", ph, tc, (pr.flags & accproto::FLAG_STABLE) ? "" : "~");
  } else if (!scaleConnected) {
    snprintf(top, sizeof(top), "BUSCANDO DOT...");
  } else {
    snprintf(top, sizeof(top), "FERMENTO");
  }
  oledDrawText(0, 1, top, 1);
  char sp[10];
  snprintf(sp, sizeof(sp), "%.1f%%", fermentSaltPct);
  const int right = localBattSensorPresent ? 112 : 128;
  oledDrawText(right - 6 * static_cast<int>(strlen(sp)), 1, sp, 1);
  drawLocalBatteryIcon(115, 1);
  oledDrawHLine(0, 10, 128, true);
}

static void drawFermentNotice(unsigned long now) {
  if (now >= fermentNoticeUntilMs) return;
  oledFillRect(0, 52, 128, 12, true);
  const int len = static_cast<int>(strlen(fermentNotice));
  oledDrawText((128 - 6 * len) / 2, 54, fermentNotice, 1, true);
}

// Paso Base:
//   PH 3.82 21.4C      2.5%      sonda | % de sal
//   1000.0 G                     lo que hay en la báscula (verduras + agua)
//   SAL 2.5% = 25.0 G            sal que hará falta
//   1C:FIJAR    2C:TARA
// Paso Sal:
//     18.6 G                     sal echada
//   [#########     ]             progreso hasta el objetivo
//   OBJ 25.0G FALTA 6.4G
//   BASE 1000G  1C:GUARDA
void renderFermentViewOled() {
  const unsigned long now = millis();
  drawFermentTopBar();
  float w = currentWeightGrams;
  if (fabsf(w) < 0.05f) w = 0.0f;
  char wb[12];
  if (w >= 1000.0f) snprintf(wb, sizeof(wb), "%5.0f", w);
  else              snprintf(wb, sizeof(wb), "%5.1f", w);
  oledDrawText(4, 14, wb, 3);
  oledDrawText(110, 21, "G", 2);

  char l1[24], l2[24];
  if (fermentStep == FermentStep::Base) {
    oledDrawHLine(0, 41, 128, true);
    snprintf(l1, sizeof(l1), "SAL %.1f%% = %.1f G", fermentSaltPct, w > 0.0f ? fermentSaltForBase(w) : 0.0f);
    oledDrawText(0, 45, l1, 1);
    oledDrawText(0, 55, "1C:FIJAR    2C:TARA", 1);
  } else {
    const float target = fermentSaltTargetGrams;
    const float tol    = fermentSaltTolerance();
    oledDrawRect(0, 37, 128, 7, true);
    int fill = (target > 0.01f) ? static_cast<int>((w / target) * 124.0f) : 0;
    if (fill < 0) fill = 0;
    if (fill > 124) fill = 124;
    const bool over = (w > target + tol);
    if (fill > 0 && !(over && ((now / 200UL) % 2UL) == 0)) oledFillRect(2, 39, fill, 3, true);
    if (w < target - tol) snprintf(l1, sizeof(l1), "OBJ %.1fG FALTA %.1fG", target, target - w);
    else if (!over)       snprintf(l1, sizeof(l1), "OBJ %.1fG  LISTO!", target);
    else                  snprintf(l1, sizeof(l1), "OBJ %.1fG SOBRA %.1fG", target, w - target);
    oledDrawText(0, 47, l1, 1);
    snprintf(l2, sizeof(l2), "BASE %.0fG  1C:GUARDA", fermentBaseGrams);
    oledDrawText(0, 56, l2, 1);
  }
  drawFermentNotice(now);
}

// Visor de lotes: receta, inicio y las 2 últimas lecturas. 1 clic añade la lectura de la sonda.
void renderLotViewerOled() {
  const unsigned long now = millis();
  oledFillRect(0, 0, 128, 11, true);
  if (fermentLotsCount == 0) {
    oledDrawText(4, 2, "LOTES DE FERMENTO", 1, true);
    oledDrawText(10, 24, "SIN LOTES", 1);
    oledDrawText(10, 36, "GUARDADOS", 1);
    oledDrawText(10, 52, "CLICK: VOLVER", 1);
    return;
  }
  if (selectedLotSlot < 0) selectedLotSlot = 0;
  if (selectedLotSlot >= fermentLotsCount) selectedLotSlot = static_cast<int8_t>(fermentLotsCount - 1);
  const FermentLot& lot = fermentLots[selectedLotSlot];
  const float realPct = lot.baseGrams > 0.0f ? lot.saltGrams * 100.0f / lot.baseGrams : 0.0f;

  char l[28];
  snprintf(l, sizeof(l), "LOTE #%u  SAL %.2f%%", lot.lotId, realPct);
  oledDrawText(2, 2, l, 1, true);
  snprintf(l, sizeof(l), "BASE:%.0fG SAL:%.1fG", lot.baseGrams, lot.saltGrams);
  oledDrawText(0, 14, l, 1);
  char d[8];
  formatShortDate(lot.startEpoch, d, sizeof(d));
  if (lot.startEpoch != 0 && clockIsSet()) {
    snprintf(l, sizeof(l), "INICIO %s  DIA %lu", d,
             static_cast<unsigned long>((static_cast<uint32_t>(time(nullptr)) - lot.startEpoch) / 86400UL));
  } else {
    snprintf(l, sizeof(l), "INICIO %s", lot.startEpoch != 0 ? d : "(SIN HORA)");
  }
  oledDrawText(0, 24, l, 1);

  if (lot.readingsCount == 0) {
    oledDrawText(0, 36, "SIN LECTURAS", 1);
  } else {
    const uint8_t first = lot.readingsCount > 2 ? lot.readingsCount - 2 : 0;
    uint8_t row = 0;
    for (uint8_t i = first; i < lot.readingsCount; i++, row++) {
      const FermentReading& fr = lot.readings[i];
      char tag[8], ph[8] = "--", tc[8] = "";
      fermentReadingTag(lot, i, tag, sizeof(tag));
      if (fr.phX100 != FERM_NO_DATA) snprintf(ph, sizeof(ph), "%.2f", fr.phX100 / 100.0f);
      if (fr.tempX10 != FERM_NO_DATA) snprintf(tc, sizeof(tc), "%.1fC", fr.tempX10 / 10.0f);
      snprintf(l, sizeof(l), "%-4s PH %s %s", tag, ph, tc);
      oledDrawText(0, 34 + row * 9, l, 1);
    }
  }
  oledDrawHLine(0, 52, 128, true);
  snprintf(l, sizeof(l), "1C:+LECTURA  %d/%u", selectedLotSlot + 1, fermentLotsCount);
  oledDrawText(0, 55, l, 1);
  drawFermentNotice(now);
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
    if (bannerIsLot) {
      snprintf(bHdr, sizeof(bHdr), "LOTE #%u GUARDADO!", lastSavedLotId);
      oledDrawText(12, 5, bHdr, 1, true);
      if (fermentLotsCount > 0) {
        const FermentLot& lot = fermentLots[fermentLotsCount - 1];
        char l1[24], l2[24];
        snprintf(l1, sizeof(l1), "BASE:%.0fG SAL:%.1fG", lot.baseGrams, lot.saltGrams);
        snprintf(l2, sizeof(l2), "SALMUERA:%.2f%%", lot.baseGrams > 0.0f ? lot.saltGrams * 100.0f / lot.baseGrams : 0.0f);
        oledDrawText(4, 24, l1, 1);
        oledDrawText(4, 40, l2, 1);
      }
      oledFlush();
      return;
    }
    if (bannerIsShot) {
      snprintf(bHdr, sizeof(bHdr), "SHOT #%u GUARDADO!", lastSavedShotId);
      oledDrawText(12, 5, bHdr, 1, true);
      if (savedShotsCount > 0) {
        const ShotRecord& r = savedShots[savedShotsCount - 1];
        char l1[24], l2[24];
        snprintf(l1, sizeof(l1), "BEBIDA:%.1fG 1:%.2f", r.yieldGrams, r.doseGrams > 0.1f ? r.yieldGrams / r.doseGrams : 0.0f);
        if (r.flags & SHOT_FLAG_PI_KNOWN) snprintf(l2, sizeof(l2), "TIEMPO:%.1fS PI:%.1fS", r.timeDs / 10.0f, r.piDs / 10.0f);
        else                              snprintf(l2, sizeof(l2), "TIEMPO:%.1fS", r.timeDs / 10.0f);
        oledDrawText(4, 24, l1, 1);
        oledDrawText(4, 40, l2, 1);
      }
      oledFlush();
      return;
    }
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
    if (viewerShowsLots)       renderLotViewerOled();
    else if (viewerShowsShots) renderShotViewerOled();
    else                  renderPourOverViewerOled();
    oledFlush();
    return;
  }

  if (uiState == UiState::Menu || uiState == UiState::MenuEdit) {
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

  // Modo fermento: la vista 0 es la calculadora de salmuera
  if (brewMode == BrewMode::Fermento && currentScreenView == 0) {
    renderFermentViewOled();
    oledFlush();
    return;
  }

  // Modo espresso: la vista 0 es la del shot (las vistas 1 a 3 son comunes)
  if (brewMode == BrewMode::Espresso && currentScreenView == 0) {
    unsigned long ms = timerAccumulatedMs;
    if (timerRunning) ms += (now - timerStartMillis);
    char tb[12];
    snprintf(tb, sizeof(tb), "%lu.%lu S", ms / 1000UL, (ms % 1000UL) / 100UL);
    renderEspressoViewOled(tb);
    oledFlush();
    return;
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
