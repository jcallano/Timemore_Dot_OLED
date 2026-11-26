// ============================================================================
// PANTALLAS DEL POUR-OVER GUIADO (41_guia.ino)
//   Vista 0: el paso en curso (qué hacer, el peso y el siguiente giro). En Resumen, la ficha completa.
//   Vista 2: perfil en vivo, la misma gráfica del historial dibujándose mientras se prepara.
// Pantalla de 128x64; fuente de 6 px por carácter: 21 caracteres por línea.
// Pestaña del sketch: el IDE la concatena tras firmware.ino (orden alfabético).
// ============================================================================

static void fmtMinSec(uint32_t ms, char* out, size_t cap) {
  const uint32_t s = ms / 1000UL;
  snprintf(out, cap, "%lu:%02lu", (unsigned long)(s / 60UL), (unsigned long)(s % 60UL));
}

// Vertidos para la pantalla: "B+2" (bloom y 2 tandas) o "3V" / "3P"
static void guidePoursLabel(char* out, size_t cap) {
  const uint8_t live = pourSegmenter.livePours(currentWeightGrams);
  if (bloomEndMs > 0 && bloomWaterG >= 1.0f) {
    snprintf(out, cap, "B+%u", (unsigned)(live > poursAtBloomEnd ? live - poursAtBloomEnd : 0));
  } else {
    snprintf(out, cap, TR("%uV", "%uP"), (unsigned)live);
  }
}

// Texto con un trozo resaltado (en negativo): antes + resaltado + después, en una línea
static void drawTextHighlight(int x, int y, const char* before, const char* hl, bool on, const char* after) {
  oledDrawText(x, y, before, 1);
  x += static_cast<int>(strlen(before)) * 6;
  const int w = static_cast<int>(strlen(hl)) * 6;
  if (on) oledFillRect(x - 1, y - 1, w + 1, 8, true);   // filas y-1..y+6: no pisa las líneas vecinas
  oledDrawText(x, y, hl, 1, on);
  oledDrawText(x + w, y, after, 1);
}

// Barra de progreso hacia un objetivo (x 0..69) y etiqueta a la derecha (hasta 9 caracteres)
static void drawGuideBar(int y, float value, float target, const char* label) {
  oledDrawRect(0, y, 70, 7, true);
  if (target > 0.1f && value > 0.0f) {
    int w = static_cast<int>((value / target) * 66.0f + 0.5f);
    if (w > 66) w = 66;
    oledFillRect(2, y + 2, w, 3, true);
  }
  oledDrawText(74, y, label, 1);
}

// Medidor de caudal del barista: el número a la izquierda y una barra de 0 a 10 g/s con tres zonas:
// lento (< 3,5 g/s), zona ideal del pour-over (3,5-7,5 g/s, entre muescas y con puntos guía) y demasiado
// rápido (> 7,5 g/s, la barra pasa a rayada). Ocupa de y - 1 a y + 8.
void drawFlowGauge(int y, float gps) {
  float shown = fabsf(gps);
  if (shown > 15.0f) shown = 15.0f;
  char buf[10];
  if (shown < 10.0f) snprintf(buf, sizeof(buf), "%3.1fg/s", shown);
  else               snprintf(buf, sizeof(buf), "%2.0f.g/s", shown);
  oledDrawText(0, y, buf, 1);

  const int x0 = 38, gaugeW = 90, innerW = gaugeW - 4;   // zona útil desde x = 40
  const int markLo = x0 + 2 + (innerW * 35) / 100;       // 3.5 g/s
  const int markHi = x0 + 2 + (innerW * 75) / 100;       // 7.5 g/s
  oledDrawRect(x0, y, gaugeW, 8, true);
  for (int m = 0; m < 2; m++) {
    const int mx = (m == 0) ? markLo : markHi;
    oledDrawPixel(mx, y - 1, true);
    oledDrawPixel(mx, y + 1, true);
    oledDrawPixel(mx, y + 6, true);
    oledDrawPixel(mx, y + 8, true);
  }
  for (int gx = markLo + 4; gx <= markHi - 4; gx += 4) oledDrawPixel(gx, y + 3, true);

  const float barFlow = (shown > 10.0f) ? 10.0f : shown;
  int fillW = static_cast<int>((barFlow / 10.0f) * static_cast<float>(innerW) + 0.5f);
  if (fillW > innerW) fillW = innerW;
  const int solidMax = markHi - (x0 + 2);
  if (fillW > 0) {
    oledFillRect(x0 + 2, y + 2, (fillW <= solidMax) ? fillW : solidMax, 4, true);
    for (int px = solidMax; px < fillW; px++) {
      if ((px & 1) == 0) oledFillRect(x0 + 2 + px, y + 2, 1, 4, true);
    }
  }
}

// Gráfica de un pour-over (historial y perfil en vivo): caja de 128x42 desde y = 13, columnas desde x = 4,
// línea de peso (2 px) y barras punteadas de caudal, alturas 0..38 px.
// targetH: altura de la línea punteada del agua objetivo (-1 = sin ella). markX: columna del fin del bloom.
void drawPourCurves(const uint8_t* wc, const uint8_t* fc, uint8_t n, int targetH, int markX) {
  oledDrawRect(0, 13, 128, 42, true);
  if (targetH >= 0 && targetH <= 38) {
    for (int x = 4; x < 124; x += 3) oledDrawPixel(x, 53 - targetH, true);
  }
  if (markX >= 0 && markX < POUR_GRAPH_POINTS) {
    for (int y = 15; y < 54; y += 3) oledDrawPixel(4 + markX, y, true);
  }
  for (uint8_t x = 0; x < n && x < POUR_GRAPH_POINTS; x++) {
    const int px = 4 + x;
    uint8_t hF = fc[x];
    if (hF > 38) hF = 38;
    for (uint8_t y = 0; y < hF; y += 2) oledDrawPixel(px, 53 - y, true);

    uint8_t hW = wc[x];
    if (hW > 38) hW = 38;
    const int py = 53 - hW;
    oledDrawPixel(px, py, true);
    if (py > 14) oledDrawPixel(px, py - 1, true);
    if (x > 0) {
      uint8_t prev = wc[x - 1];
      if (prev > 38) prev = 38;
      const int prevPy = 53 - prev;
      const int yMin = (py < prevPy) ? py : prevPy;
      const int yMax = (py > prevPy) ? py : prevPy;
      for (int yy = yMin; yy <= yMax; yy++) oledDrawPixel(px, yy, true);
    }
  }
}

// ---------------------------------------------------------------------------
// Vista 2: perfil en vivo
// ---------------------------------------------------------------------------
// Escala: Y fija al agua objetivo de la receta (si se pasa, crece); X empieza en 3:00 y se estira x1.5
// de golpe cuando el café dura más (sin reescalar a cada segundo, para que la curva no tiemble).
void renderLiveProfileOled() {
  static uint8_t wc[POUR_GRAPH_POINTS], fc[POUR_GRAPH_POINTS];
  const uint32_t el = brewElapsedMs();
  const float target = targetWaterGrams();

  float maxW = 0.0f;
  for (uint16_t i = 0; i < liveSampleCount; i++) {
    if (psramLiveSamples[i].weightGrams > maxW) maxW = psramLiveSamples[i].weightGrams;
  }
  float yMax = (maxW > target ? maxW : target) * 1.05f;
  if (yMax < 10.0f) yMax = 10.0f;
  float windowS = 180.0f;
  while (el / 1000.0f > windowS) windowS *= 1.5f;

  uint8_t cols = 0;
  if (brewStartMs != 0 && liveSampleCount > 0) {
    cols = static_cast<uint8_t>(constrain(lroundf((el / 1000.0f) / windowS * POUR_GRAPH_POINTS), 1L,
                                          static_cast<long>(POUR_GRAPH_POINTS)));
    for (uint8_t x = 0; x < cols; x++) {
      uint32_t idx = static_cast<uint32_t>((x * windowS / POUR_GRAPH_POINTS) * 2.0f);   // muestras a 2 Hz
      if (idx >= liveSampleCount) idx = liveSampleCount - 1;
      const float w = psramLiveSamples[idx].weightGrams;
      const float f = psramLiveSamples[idx].flowGps;
      wc[x] = static_cast<uint8_t>(constrain(lroundf(w / yMax * 38.0f), 0L, 38L));
      fc[x] = static_cast<uint8_t>(constrain(lroundf(f / POUR_GRAPH_MAX_GPS * 38.0f), 0L, 38L));
    }
  }
  const int targetH = static_cast<int>(lroundf(target / yMax * 38.0f));
  const int markX = (bloomEndMs > 0) ? static_cast<int>((bloomEndMs / 1000.0f) / windowS * POUR_GRAPH_POINTS) : -1;

  // Cabecera: agua, ratio, vertidos y tiempo ("245G 1:16.3 B+2 2:14")
  oledFillRect(0, 0, 128, 11, true);
  const float water = guideWaterGrams();
  char pours[8], tm[10], hdr[28];
  guidePoursLabel(pours, sizeof(pours));
  fmtMinSec(el, tm, sizeof(tm));
  snprintf(hdr, sizeof(hdr), "%3.0fg 1:%4.1f %s %s", water,
           water / brewDoseGrams(), pours, tm);
  oledDrawText(2, 2, hdr, 1, true);

  drawPourCurves(wc, fc, cols, targetH, markX);
  if (cols == 0) {
    oledDrawText(10, 24, TR("EL PERFIL SALE", "THE PROFILE STARTS"), 1);
    oledDrawText(10, 34, TR("CON EL 1ER VERTIDO", "WITH THE 1ST POUR"), 1);
  }

  // Pie: caudal ahora, máximo y toques de la tetera descartados
  char foot[28];
  snprintf(foot, sizeof(foot), TR("F%.1f MAX%.1f TOQ%u", "F%.1f MAX%.1f TAP%u"),
           fabsf(displayedFlowRateGps), guidePeakGps, (unsigned)pourSegmenter.taps());
  oledDrawText(0, 56, foot, 1);
}

// ---------------------------------------------------------------------------
// Vista 0: resumen (último paso)
// ---------------------------------------------------------------------------
static void renderGuideSummaryOled() {
  const PourOverRecord* rec = findPourById(guidePourId);
  const uint32_t total = timerAccumulatedMs;
  char tm[10], line[28];

  oledFillRect(0, 0, 128, 11, true);
  if (guidePourId != 0) snprintf(line, sizeof(line), TR("RESUMEN #%u", "SUMMARY #%u"), guidePourId);
  else                  snprintf(line, sizeof(line), "%s", TR("SIN GUARDAR", "NOT SAVED"));
  oledDrawText(2, 2, line, 1, true);
  fmtMinSec(total, tm, sizeof(tm));
  oledDrawText(126 - static_cast<int>(strlen(tm)) * 6, 2, tm, 1, true);

  const float water = waterTotalG;
  const float dose  = rec ? rec->doseGrams : brewDoseGrams();
  snprintf(line, sizeof(line), TR("AGUA %.0fg 1:%.1f %uC", "WATER %.0fg 1:%.1f %uC"), water,
           dose > 0.1f ? water / dose : 0.0f, (unsigned)waterTempC);
  oledDrawText(2, 14, line, 1);

  char pours[8];
  guidePoursLabel(pours, sizeof(pours));
  bool measured = yieldMeasured;
  float bev = yieldGrams;
  if (!measured && rec != nullptr) bev = pourBeverageGrams(*rec, &measured);
  if (measured) snprintf(line, sizeof(line), "YIELD %.1fg  %s", bev, pours);
  else          snprintf(line, sizeof(line), TR("YIELD %.0fg* %s", "YIELD %.0fg* %s"), bev, pours);
  oledDrawText(2, 24, line, 1);

  char b[10], d[10];
  if (bloomEndMs > 0) fmtMinSec(bloomEndMs, b, sizeof(b)); else snprintf(b, sizeof(b), "--");
  if (pourEndMs > 0 && total >= pourEndMs) fmtMinSec(total - pourEndMs, d, sizeof(d)); else snprintf(d, sizeof(d), "--");
  snprintf(line, sizeof(line), "BLOOM %s  DD %s", b, d);
  oledDrawText(2, 34, line, 1);

  // Con la dosis pesada, la dosis y la retención del molino; si no, el caudal
  if (guideBeansG > 0.0f && guideGroundG > 0.0f)
    snprintf(line, sizeof(line), TR("DOSIS %.1fg RET %+.1fg", "DOSE %.1fg RET %+.1fg"), guideGroundG, guideBeansG - guideGroundG);
  else if (guideDoseG > 0.0f)
    snprintf(line, sizeof(line), TR("DOSIS %.1fg (%s)", "DOSE %.1fg (%s)"), guideDoseG,
             guideGroundG > 0.0f ? TR("MOLIDO", "GROUND") : TR("GRANOS", "BEANS"));
  else
    snprintf(line, sizeof(line), TR("CAUDAL %.1f MAX %.1f", "FLOW %.1f MAX %.1f"),
             rec ? rec->avgFlowGps : 0.0f, rec ? rec->peakFlowGps : 0.0f);
  oledDrawText(2, 44, line, 1);

  oledDrawHLine(0, 53, 128, true);
  oledDrawText(2, 56, (guidePourId != 0 && r2Paired()) ? TR("GIRA > MEDIR TDS", "TURN > MEASURE TDS")
                                                       : TR("GIRA > OTRO CAFE", "TURN > NEXT BREW"), 1);
}

// ---------------------------------------------------------------------------
// Vista 0: el paso en curso
// ---------------------------------------------------------------------------
//   [VERTIDOS       1:42]   paso y cronómetro (antes de empezar: número de paso)
//     132.4 g               peso
//   4.1g/s [####:###  ]     línea A: caudal con el medidor de 3 zonas (o un dato)
//   [######    ] -118 B+2   línea B: barra hacia el agua objetivo (o un dato)
//   AL ACABAR GIRA >        línea C: qué hace el siguiente giro
void renderGuideOled() {
  if (guideStep == GuideStep::Summary) {
    renderGuideSummaryOled();
    return;
  }
  char buf[28], tm[10];

  // Barra superior
  oledFillRect(0, 0, 128, 11, true);
  oledDrawText(2, 2, guideStepName(guideStep), 1, true);
  if (brewStartMs != 0) fmtMinSec(brewElapsedMs(), tm, sizeof(tm));
  else                  snprintf(tm, sizeof(tm), "%u/9", static_cast<unsigned>(guideStepNumber(guideStep)));
  if (!scaleConnected && !demoLiveActive) snprintf(tm, sizeof(tm), "%s", TR("SIN DOT", "NO DOT"));
  oledDrawText(126 - static_cast<int>(strlen(tm)) * 6, 2, tm, 1, true);

  // Peso: en Yield, con el V60 fuera, la bebida (bruto - server); una vez medida, la medida
  float w = currentWeightGrams;
  if (guideStep == GuideStep::Yield && yieldMeasured)       w = yieldGrams;
  else if (guideStep == GuideStep::Yield && guideV60Removed()) w = rawWeightGrams - serverRawGrams;
  // Granos: con el vaso fuera (a moler), los granos que se pesaron
  else if (guideStep == GuideStep::Beans && guideBeansLocked) w = guideBeansCandidate;
  if (fabsf(w) < 0.05f) w = 0.0f;
  snprintf(buf, sizeof(buf), "%5.1f", w);
  if (guideStep == GuideStep::Ready) {
    // En Listo manda la receta: el peso, más pequeño
    oledDrawText(4, 14, buf, 2);
    oledDrawText(66, 21, "g", 1);
  } else {
    oledDrawText(4, 14, buf, 3);
    oledDrawText(110, 21, "g", 2);
  }

  const char* a = nullptr;
  const char* c = nullptr;
  char lineA[28] = "", lineB[28] = "";
  const float target = targetWaterGrams();

  switch (guideStep) {
    case GuideStep::Ready: {
      // Receta: dosis y ratio (agua objetivo), bloom, molienda y temperatura del agua; con 1 clic se
      // edita, el campo activo en negativo
      char dose[10], ratio[10], water[12], bloom[10], bloomG[12], grind[10], temp[8];
      snprintf(dose, sizeof(dose), "%.1fg", coffeeDoseGrams);
      snprintf(ratio, sizeof(ratio), "1:%.1f", pourRatio);
      snprintf(water, sizeof(water), " >%.0fg", target);
      snprintf(bloom, sizeof(bloom), "X%.1f", bloomFactor);
      snprintf(bloomG, sizeof(bloomG), " >%.0fg", bloomTargetGrams());
      snprintf(grind, sizeof(grind), "%.1f", grindPour);
      snprintf(temp, sizeof(temp), "%uC", (unsigned)waterTempC);
      drawTextHighlight(0, 31, "", dose, guideRecipeField == 0, " ");
      drawTextHighlight(6 * (static_cast<int>(strlen(dose)) + 1), 31, "", ratio, guideRecipeField == 1, water);
      drawTextHighlight(0, 39, "BLOOM ", bloom, guideRecipeField == 2, bloomG);
      const char* gl = TR("MOLIENDA ", "GRIND ");
      drawTextHighlight(0, 47, gl, grind, guideRecipeField == 3, "  ");
      drawTextHighlight(6 * static_cast<int>(strlen(gl) + strlen(grind) + 2), 47, "", temp, guideRecipeField == 4, "");
      c = guideRecipeEditing() ? TR("GIRA:AJUSTA CLIC:SIG", "TURN:ADJUST CLICK:NXT")
                               : TR("CLIC:RECETA  GIRA >", "CLICK:RECIPE TURN >");
      break;
    }
    case GuideStep::Beans: {
      // Tara con el vaso; barra hacia la dosis más la retención media del molino
      float ret = 0.0f;
      const uint8_t nRet = grinderRetentionAvg(&ret);
      if (guidePendingTare)  a = TR("ESPERA PESO QUIETO...", "HOLD STILL...");
      else if (guideBeansLocked) a = TR("A MOLER 2CLIC:REPETIR", "GRIND  2CLICK:REDO");
      else if (nRet)         snprintf(lineA, sizeof(lineA), TR("RETENCION MEDIA %+.1fg", "AVG RETENTION %+.1fg"), ret);
      else                   snprintf(lineA, sizeof(lineA), TR("VASO %.1fg", "CUP %.1fg"), cupRawGrams - guidePrevTareOffset);
      const float bt = beansTargetGrams();
      const float shown = guideBeansLocked ? guideBeansCandidate : currentWeightGrams;
      snprintf(buf, sizeof(buf), "%.1fg", bt);
      drawGuideBar(48, shown, bt, buf);
      c = TR("PESA LOS GRANOS GIRA>", "WEIGH BEANS  TURN >");
      break;
    }
    case GuideStep::Server:
      if (guidePendingTare) a = TR("ESPERA PESO QUIETO...", "HOLD STILL...");
      else snprintf(lineA, sizeof(lineA), "SERVER %.1fg", serverRawGrams);
      if (guideBeansG > 0.0f) snprintf(lineB, sizeof(lineB), TR("GRANOS %.1fg", "BEANS %.1fg"), guideBeansG);
      c = TR("PON EL V60  GIRA >", "ADD THE V60  TURN >");
      break;
    case GuideStep::Dripper:
      if (guidePendingTare) a = TR("ESPERA PESO QUIETO...", "HOLD STILL...");
      else snprintf(lineA, sizeof(lineA), "V60 %.1fg", dripperEmptyRawGrams - serverRawGrams);
      snprintf(lineB, sizeof(lineB), "%.1fg 1:%.1f > %.0fg", brewDoseGrams(), pourRatio, target);
      c = TR("GIRA > CAFE MOLIDO", "TURN > GROUND COFFEE");
      break;
    case GuideStep::Ground: {
      // El café molido que entra en el V60; con los granos pesados, la retención del molino en vivo
      const float net = currentWeightGrams;
      if (guideBeansG > 0.0f && net >= 1.0f)
        snprintf(lineA, sizeof(lineA), TR("GRANOS %.1f RET %+.1f", "BEANS %.1f RET %+.1f"), guideBeansG, guideBeansG - net);
      else
        a = TR("ECHA EL CAFE MOLIDO", "ADD THE GROUND COFFEE");
      // 2 clics con el café dentro no lo borran: se avisa de que para seguir se gira
      if (millis() < guideNoticeUntilMs) a = TR("CAFE GUARDADO: GIRA >", "COFFEE KEPT: TURN >");
      const float dt = guideBeansG > 0.0f ? guideBeansG : coffeeDoseGrams;
      snprintf(buf, sizeof(buf), "%.1fg", dt);
      drawGuideBar(48, net, dt, buf);
      c = TR("GIRA > BLOOM", "TURN > BLOOM");
      break;
    }
    case GuideStep::Bloom: {
      // Caudal mientras se vierte; en la espera del bloom, el tiempo de espera
      if (brewStartMs == 0) {
        a = TR("VIERTE EL BLOOM", "POUR THE BLOOM");
      } else if (!pourSegmenter.active() && pourSegmenter.lastPourEndMs() != 0) {
        fmtMinSec(millis() - pourSegmenter.lastPourEndMs(), tm, sizeof(tm));
        snprintf(lineA, sizeof(lineA), TR("ESPERA %s", "WAIT %s"), tm);
      } else {
        drawFlowGauge(39, displayedFlowRateGps);
      }
      snprintf(buf, sizeof(buf), "%.0fg", bloomTargetGrams());
      drawGuideBar(48, currentWeightGrams, bloomTargetGrams(), buf);
      c = TR("GIRA > VERTIDOS", "TURN > BREW");
      break;
    }
    case GuideStep::Brew: {
      drawFlowGauge(39, displayedFlowRateGps);
      const float left = target - currentWeightGrams;
      char pours[8];
      guidePoursLabel(pours, sizeof(pours));
      if (left >= 0.5f) snprintf(buf, sizeof(buf), "-%.0f %s", left, pours);
      else              snprintf(buf, sizeof(buf), "+%.0f %s", -left, pours);
      drawGuideBar(48, currentWeightGrams, target, buf);
      c = TR("AL ACABAR GIRA >", "DONE POURING TURN >");
      break;
    }
    case GuideStep::Drawdown:
      snprintf(lineA, sizeof(lineA), TR("AGUA %.1fg 1:%.1f", "WATER %.1fg 1:%.1f"), waterTotalG,
               waterTotalG / brewDoseGrams());
      if (brewStartMs != 0 && pourEndMs != 0) {
        fmtMinSec(brewElapsedMs() - pourEndMs, tm, sizeof(tm));
        snprintf(lineB, sizeof(lineB), "DRAWDOWN %s", tm);
      }
      c = TR("AL GOTEAR GIRA >", "DRIPPED? TURN >");
      break;
    case GuideStep::Yield:
      if (brewStartMs == 0)      a = TR("NO HUBO VERTIDO", "NO WATER POURED");
      else if (!yieldMeasured)   a = TR("QUITA EL V60", "REMOVE THE V60");
      else                       a = TR("YIELD MEDIDO", "YIELD MEASURED");
      if (guidePourId != 0) snprintf(lineB, sizeof(lineB), TR("GUARDADO #%u", "SAVED #%u"), guidePourId);
      c = TR("GIRA > RESUMEN", "TURN > SUMMARY");
      break;
    case GuideStep::Summary:
      break;
  }

  if (a != nullptr) oledDrawText(0, 40, a, 1);
  else if (lineA[0]) oledDrawText(0, 40, lineA, 1);
  if (lineB[0]) oledDrawText(0, 48, lineB, 1);
  if (c != nullptr) {
    oledDrawHLine(0, 55, 128, true);
    oledDrawText(0, 57, c, 1);
  }
}
