// ============================================================================
// POUR-OVER GUIADO
//   Listo -> Granos -> Server -> V60 -> Café -> Bloom -> Vertidos -> Drawdown -> Yield -> Resumen
// Los pasos avanzan GIRANDO el encoder y se deshacen girando hacia atrás. El peso nunca cambia de paso
// por su cuenta (el montaje se toca sin querer): solo mide y avisa.
//
// Tara en dos capas, con la tara por software de siempre (softwareTareOffset):
//   - Granos: tara con el vaso dosificador; los granos son el último peso quieto (se puede quitar el vaso
//     para moler antes de girar). Sin granos, el paso se salta girando.
//   - Server: se guarda su peso bruto (serverRawGrams) y la pantalla pasa a 0.
//   - V60: se vuelve a poner a 0 con el V60 (dripperEmptyRawGrams), con el filtro ya enjuagado.
//   - Café: el café molido que se echa en el V60. Al girar es la dosis real (el agua objetivo y el bloom
//     se recalculan con ella) y vuelve a tarar (dripperRawGrams). Granos - café = retención del molino.
//     Si el café ya estaba en el V60 al tarar, marca 0 y la dosis es la de los granos o la de la receta.
//   - Yield: al quitar el V60, la bebida es el bruto de la Dot menos el server.
// El cronómetro arranca solo con el primer chorro que deja agua (no con un toque de la tetera) y para al
// pasar a Yield, que es cuando se guarda el pour-over.
// Pestaña del sketch: el IDE la concatena tras firmware.ino (orden alfabético).
// ============================================================================

static constexpr uint32_t GUIDE_STEP_LOCK_MS  = 350;    // un giro rápido no salta varios pasos
static constexpr float    GUIDE_START_GAIN_G  = 2.0f;   // agua que arranca el cronómetro
static constexpr float    GUIDE_STABLE_BAND_G = 0.3f;   // "estable": el bruto no se mueve más que esto
static constexpr uint32_t GUIDE_STABLE_MS     = 700;    // ... durante este tiempo
static constexpr uint32_t GUIDE_COMMIT_MAX_MS = 3000;   // tara pendiente: se fija igualmente pasado esto

float guidePrevTareOffset   = 0.0f;   // tara de antes del server (para deshacer y para volver a Listo)
float serverRawGrams        = 0.0f;   // bruto con el server vacío
float dripperRawGrams       = 0.0f;   // bruto con server + V60 + café
float dripperEmptyRawGrams  = 0.0f;   // bruto con server + V60 sin el café
float cupRawGrams           = 0.0f;   // bruto con el vaso dosificador vacío
float guideBeansG           = 0.0f;   // granos pesados en este café (0 = no se pesaron)
float guideGroundG          = 0.0f;   // café molido pesado en el V60 (0 = no se pesó)
float guideDoseG            = 0.0f;   // dosis medida de este café (0 = la de la receta)
float guideBeansCandidate   = 0.0f;   // último peso quieto de los granos en el paso Granos
bool guideBeansLocked       = false;  // el vaso ya se quitó: los granos no cambian
unsigned long guideNoticeUntilMs = 0; // aviso "2 clics no borran el café" en el paso Café molido
bool guidePendingTare       = false;  // Server o V60 esperan peso estable para fijar su tara
unsigned long guidePendingSinceMs = 0;
unsigned long brewStartMs   = 0;      // inicio del cronómetro (0 = aún no ha caído agua)
uint32_t bloomEndMs         = 0;      // ms desde el inicio (0 = sin bloom)
uint32_t pourEndMs          = 0;      // ms desde el inicio en que acabó de verter (0 = aún no)
float bloomWaterG           = 0.0f;
float waterTotalG           = 0.0f;
uint8_t poursAtBloomEnd     = 0;
float yieldGrams            = 0.0f;
bool yieldMeasured          = false;
uint16_t guidePourId        = 0;      // pour-over guardado al pasar a Yield (0 = no se guardó)
float guidePeakGps          = 0.0f;   // caudal máximo de los vertidos aceptados (sin los toques)
static float guideCandPeakGps = 0.0f; // ... del candidato en curso
static unsigned long guideLastStepMs = 0;
static float guideStableRef = 0.0f;
static unsigned long guideStableSinceMs = 0;
static float guideYieldCandidate = 0.0f;
static unsigned long guideYieldSinceMs = 0;

static GuideTiming guideTimings[MAX_SAVED_POURS];

// ---------------------------------------------------------------------------
// Datos derivados (también los usan la pantalla y el LED)
// ---------------------------------------------------------------------------
// Dosis de este café: la medida (café molido o, si no, granos) o la de la receta
float brewDoseGrams()     { return guideDoseG > 0.1f ? guideDoseG : coffeeDoseGrams; }
float targetWaterGrams()  { return brewDoseGrams() * pourRatio; }
float bloomTargetGrams()  { return brewDoseGrams() * bloomFactor; }

// Retención media del molino (granos - café molido) en los pour-overs guardados que pesaron las dos cosas.
// Devuelve cuántos hay (0 = ninguno; *avg sin tocar).
uint8_t grinderRetentionAvg(float* avg) {
  float s = 0.0f;
  uint8_t n = 0;
  for (uint8_t i = 0; i < MAX_SAVED_POURS; i++) {
    const GuideTiming& g = guideTimings[i];
    if (g.id == 0 || g.beansX10 == 0 || g.groundX10 == 0) continue;
    const float r = (static_cast<int>(g.beansX10) - static_cast<int>(g.groundX10)) / 10.0f;
    if (fabsf(r) > 3.0f) continue;   // otro café o un error al pesar, no retención
    s += r;
    n++;
  }
  if (n && avg) *avg = s / n;
  return n;
}

// Granos objetivo: la dosis de la receta más lo que suele quedarse en el molino
float beansTargetGrams() {
  float r = 0.0f;
  if (grinderRetentionAvg(&r) && r > 0.0f) return coffeeDoseGrams + r;
  return coffeeDoseGrams;
}
bool guideBrewStarted()   { return brewStartMs != 0; }

uint32_t brewElapsedMs() {
  if (brewStartMs == 0) return 0;
  return timerRunning ? (millis() - timerStartMillis) + timerAccumulatedMs : timerAccumulatedMs;
}

// Peso bruto quieto desde hace cuánto (ms)
uint32_t guideStableForMs() {
  return guideStableSinceMs == 0 ? 0 : static_cast<uint32_t>(millis() - guideStableSinceMs);
}

// Agua vertida: la marcada al acabar de verter, o la que hay ahora
float guideWaterGrams() {
  if (pourEndMs != 0) return waterTotalG;
  return currentWeightGrams > 0.0f ? currentWeightGrams : 0.0f;
}

// En Yield: ya no está el V60 (el peso cayó claramente por debajo del agua vertida)
bool guideV60Removed() {
  return brewStartMs != 0 && waterTotalG > 20.0f && currentWeightGrams < waterTotalG - 20.0f;
}

// LED: 0 nada, 1 cerca del agua objetivo (amarillo), 2 objetivo alcanzado (rojo)
uint8_t guideLedLevel() {
  if (brewMode != BrewMode::PourOver || guideStep != GuideStep::Brew || !timerRunning) return 0;
  const float target = targetWaterGrams();
  if (currentWeightGrams >= target) return 2;
  if (currentWeightGrams >= target - 10.0f) return 1;
  return 0;
}

const char* guideStepName(GuideStep s) {
  switch (s) {
    case GuideStep::Ready:    return TR("LISTO", "READY");
    case GuideStep::Beans:    return TR("GRANOS", "BEANS");
    case GuideStep::Server:   return "SERVER";
    case GuideStep::Dripper:  return "V60";
    case GuideStep::Ground:   return TR("CAFE MOLIDO", "GROUND COFFEE");
    case GuideStep::Bloom:    return "BLOOM";
    case GuideStep::Brew:     return TR("VERTIDOS", "BREW");
    case GuideStep::Drawdown: return "DRAWDOWN";
    case GuideStep::Yield:    return "YIELD";
    case GuideStep::Summary:  return TR("RESUMEN", "SUMMARY");
  }
  return "";
}

// Número del paso en pantalla (1..9; Listo = 0)
uint8_t guideStepNumber(GuideStep s) {
  switch (s) {
    case GuideStep::Ready:    return 0;
    case GuideStep::Beans:    return 1;
    case GuideStep::Server:   return 2;
    case GuideStep::Dripper:  return 3;
    case GuideStep::Ground:   return 4;
    case GuideStep::Bloom:    return 5;
    case GuideStep::Brew:     return 6;
    case GuideStep::Drawdown: return 7;
    case GuideStep::Yield:    return 8;
    case GuideStep::Summary:  return 9;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Tiempos de cada pour-over en la NVS (bloom, fin de vertido, toques, server, granos y café molido)
// ---------------------------------------------------------------------------
void loadGuideTimings() {
  memset(guideTimings, 0, sizeof(guideTimings));
  Preferences p;
  p.begin(NVS_POUR_NAMESPACE, true);
  if (p.getBytesLength("gt2") == sizeof(guideTimings)) {
    p.getBytes("gt2", guideTimings, sizeof(guideTimings));
  } else {
    // Tabla anterior (12 B por café, sin granos ni café molido): se lee y se pasa a la nueva al guardar
    struct GuideTimingV1 { uint16_t id, bloomEndDs, pourEndDs, bloomWaterX10; uint8_t taps, flags; uint16_t serverX10; };
    static_assert(sizeof(GuideTimingV1) == 12, "GuideTimingV1");
    GuideTimingV1 old[MAX_SAVED_POURS];
    if (p.getBytesLength("gt1") == sizeof(old) && p.getBytes("gt1", old, sizeof(old)) == sizeof(old)) {
      for (uint8_t i = 0; i < MAX_SAVED_POURS; i++) {
        guideTimings[i].id            = old[i].id;
        guideTimings[i].bloomEndDs    = old[i].bloomEndDs;
        guideTimings[i].pourEndDs     = old[i].pourEndDs;
        guideTimings[i].bloomWaterX10 = old[i].bloomWaterX10;
        guideTimings[i].taps          = old[i].taps;
        guideTimings[i].serverX10     = old[i].serverX10;
      }
    }
  }
  pourRatio   = p.getFloat("pr_ratio", 16.0f);
  bloomFactor = p.getFloat("bloom_x", 2.0f);
  waterTempC  = p.getUChar("w_temp", 93);
  p.end();
  if (waterTempC < WATER_TEMP_MIN || waterTempC > WATER_TEMP_MAX) waterTempC = 93;
  if (isnan(pourRatio) || pourRatio < POUR_RATIO_MIN || pourRatio > POUR_RATIO_MAX) pourRatio = 16.0f;
  if (isnan(bloomFactor) || bloomFactor < BLOOM_FACTOR_MIN || bloomFactor > BLOOM_FACTOR_MAX) bloomFactor = 2.0f;
}

void savePourRecipeToNVS() {
  if (demoLiveActive) return;   // la demostración usa su propia receta y al acabar restaura la de siempre
  Preferences p;
  p.begin(NVS_POUR_NAMESPACE, false);
  p.putFloat("dose_g", coffeeDoseGrams);
  p.putFloat("pr_ratio", pourRatio);
  p.putFloat("bloom_x", bloomFactor);
  p.putUChar("w_temp", waterTempC);
  p.end();
  saveGrindSettings();
}

// Tiempos de un pour-over (nullptr si no los tiene: hecho antes del modo guiado)
const GuideTiming* findGuideTiming(uint16_t id) {
  if (id == 0) return nullptr;
  for (uint8_t i = 0; i < MAX_SAVED_POURS; i++) if (guideTimings[i].id == id) return &guideTimings[i];
  return nullptr;
}

static void storeGuideTiming(uint16_t id) {
  if (id == 0 || demoLiveActive) return;
  uint8_t pick = 0;
  for (uint8_t i = 0; i < MAX_SAVED_POURS; i++) {
    if (guideTimings[i].id == id) { pick = i; break; }
    if (guideTimings[i].id < guideTimings[pick].id) pick = i;   // libre (0) o el más antiguo
  }
  GuideTiming& g = guideTimings[pick];
  memset(&g, 0, sizeof(g));
  g.id            = id;
  g.bloomEndDs    = static_cast<uint16_t>(bloomEndMs / 100UL);
  g.pourEndDs     = static_cast<uint16_t>(pourEndMs / 100UL);
  g.bloomWaterX10 = static_cast<uint16_t>(lroundf(bloomWaterG * 10.0f));
  g.taps          = pourSegmenter.taps();
  g.serverX10     = static_cast<uint16_t>(constrain(lroundf(serverRawGrams * 10.0f), 0L, 65535L));
  g.waterTempC    = waterTempC;
  g.beansX10      = static_cast<uint16_t>(constrain(lroundf(guideBeansG * 10.0f), 0L, 65535L));
  g.groundX10     = static_cast<uint16_t>(constrain(lroundf(guideGroundG * 10.0f), 0L, 65535L));
  Preferences p;
  p.begin(NVS_POUR_NAMESPACE, false);
  p.putBytes("gt2", guideTimings, sizeof(guideTimings));
  if (p.isKey("gt1")) p.remove("gt1");
  p.end();
}

// ---------------------------------------------------------------------------
// Tara y cronómetro
// ---------------------------------------------------------------------------
// Fija la tara por software sin tocar la Dot (como executeTimemoreTare) y reinicia el motor de caudal
void setTareOffset(float offset) {
  const unsigned long now = millis();
  softwareTareOffset      = offset;
  tareZeroAmbiguous       = false;
  lastTareCommandMs       = now;
  currentWeightGrams      = rawWeightGrams - offset;
  prevActivityWeightGrams = currentWeightGrams;
  flowEngine.reset(currentWeightGrams);
  prevFlowCalcMs          = now;
  traceEvent(TE_TARE, 0, offset);
  uiNeedsImmediateRedraw  = true;
}

static void guideResetBrewMarks() {
  brewStartMs     = 0;
  bloomEndMs      = 0;
  pourEndMs       = 0;
  bloomWaterG     = 0.0f;
  waterTotalG     = 0.0f;
  poursAtBloomEnd = 0;
  yieldGrams      = 0.0f;
  yieldMeasured   = false;
  guidePourId     = 0;
  guidePeakGps    = 0.0f;
  guideCandPeakGps = 0.0f;
}

// Bloom armado: el cronómetro espera al primer chorro
static void guideArmBrew() {
  timerRunning       = false;
  timerAccumulatedMs = 0;
  resetLivePourRecording();
  pourSegmenter.reset();
  guideResetBrewMarks();
}

static void guideStartTimer(unsigned long atMs) {
  brewStartMs        = atMs;
  timerStartMillis   = atMs;
  timerAccumulatedMs = 0;
  timerRunning       = true;
  lastLiveSampleMs   = 0;
  traceEvent(TE_TIMER_START, 0, 0.0f);
  Serial.printf("[GUIA] Cronometro en marcha (primer chorro, %lu ms antes).\n", (unsigned long)(millis() - atMs));
}

static void guideCancelBrew() {
  guideArmBrew();
  Serial.println("[GUIA] Preparacion cancelada.");
}

// Para el cronómetro y guarda el pour-over (al pasar a Yield)
static void guideStopAndSave(unsigned long now) {
  yieldMeasured     = false;
  guideYieldSinceMs = 0;
  if (brewStartMs == 0) { guidePourId = 0; return; }
  if (timerRunning) {
    timerAccumulatedMs = now - brewStartMs;
    timerRunning       = false;
    traceEvent(TE_TIMER_STOP, 0, timerAccumulatedMs / 100.0f);
  }
  if (pourEndMs == 0) {
    pourEndMs   = timerAccumulatedMs;
    waterTotalG = currentWeightGrams;
  }
  const uint8_t pours  = pourSegmenter.livePours(currentWeightGrams);
  const bool bloom     = bloomEndMs > 0 && bloomWaterG >= 1.0f;
  const uint8_t inBrew = pours > poursAtBloomEnd ? static_cast<uint8_t>(pours - poursAtBloomEnd) : 0;
  currentPourPulsesCount = bloom ? static_cast<uint8_t>(inBrew + 1) : (pours > 0 ? pours : 1);
  recordBloomOverride    = bloom ? 1 : 0;
  peakFlowRateGps        = guidePeakGps;   // sin los picos de los toques de la tetera
  recordWaterOverride    = waterTotalG;
  const bool ok          = finalizeAndSaveCurrentPourOver(timerAccumulatedMs);
  recordBloomOverride    = -1;
  recordWaterOverride    = -1.0f;
  pourSavedBannerUntilMs = 0;   // el resumen se ve en su paso, no en un aviso de 2 s
  guidePourId = ok ? lastSavedPourId : 0;
  if (ok) storeGuideTiming(guidePourId);
}

// Dosis medida de este café: ninguna (vuelve la de la receta)
static void guideClearDose() {
  guideBeansG         = 0.0f;
  guideGroundG        = 0.0f;
  guideDoseG          = 0.0f;
  guideBeansCandidate = 0.0f;
  guideBeansLocked    = false;
}

// Vuelta a Listo: la tara de antes de empezar; el pour-over terminado queda en el historial
static void guideFinish() {
  guidePendingTare = false;
  setTareOffset(guidePrevTareOffset);
  traceStop();
  timerRunning       = false;
  timerAccumulatedMs = 0;
  guideResetBrewMarks();
  guideClearDose();
}

// A Listo sin tocar la tara (demostración 'v')
void guideReset() {
  guideClearDose();
  guidePendingTare   = false;
  traceStop();
  timerRunning       = false;
  timerAccumulatedMs = 0;
  guideResetBrewMarks();
  guideStep          = GuideStep::Ready;
  guideLastStepMs    = 0;
}

static void guideGoTo(GuideStep s) {
  guideStep       = s;
  guideLastStepMs = millis();
  traceEvent(TE_STEP, static_cast<uint8_t>(s), 0.0f);
  triggerLedFlash();
  uiNeedsImmediateRedraw = true;
  Serial.printf("[GUIA] Paso: %s\n", guideStepName(s));
}

static void guideStartPendingTare() {
  guidePendingTare    = true;
  guidePendingSinceMs = millis();
}

// ---------------------------------------------------------------------------
// Receta en la pantalla Listo: 1 clic edita la dosis, clic -> ratio, clic -> bloom, clic -> molienda,
// clic -> temperatura del agua, clic -> guarda. La molienda es la del molino para filtrado (46_extraccion).
// Girar ajusta el valor resaltado. 2 o 3 clics guardan y salen. Se guarda en la NVS al salir.
// ---------------------------------------------------------------------------
int8_t guideRecipeField = -1;   // -1 = sin editar, 0 = dosis, 1 = ratio, 2 = bloom, 3 = molienda, 4 = agua (°C)
static constexpr int8_t GUIDE_RECIPE_LAST = 4;

bool guideRecipeEditing() { return guideRecipeField >= 0; }

void guideRecipeDone() {
  if (guideRecipeField < 0) return;
  guideRecipeField = -1;
  savePourRecipeToNVS();
  uiNeedsImmediateRedraw = true;
  Serial.printf("[GUIA] Receta: %.1f g, 1:%.1f (%.0f g), bloom x%.1f (%.0f g), molienda %.1f, agua %u C.\n",
                coffeeDoseGrams, pourRatio, targetWaterGrams(), bloomFactor, bloomTargetGrams(), grindForMode(),
                (unsigned)waterTempC);
}

void guideRecipeClick() {
  if (guideRecipeField < 0)      guideRecipeField = 0;
  else if (guideRecipeField < GUIDE_RECIPE_LAST) guideRecipeField++;
  else                           guideRecipeDone();
  uiNeedsImmediateRedraw = true;
}

void guideRecipeAdjust(int32_t delta) {
  const float d = static_cast<float>(delta);
  if (guideRecipeField == 0) {
    coffeeDoseGrams = constrain(roundf((coffeeDoseGrams + d * 0.5f) * 10.0f) / 10.0f, DOSE_MIN_G, DOSE_MAX_G);
  } else if (guideRecipeField == 1) {
    pourRatio = constrain(roundf((pourRatio + d * 0.1f) * 10.0f) / 10.0f, POUR_RATIO_MIN, POUR_RATIO_MAX);
  } else if (guideRecipeField == 2) {
    bloomFactor = constrain(roundf((bloomFactor + d * 0.5f) * 10.0f) / 10.0f, BLOOM_FACTOR_MIN, BLOOM_FACTOR_MAX);
  } else if (guideRecipeField == 3) {
    nudgeGrind(delta);
  } else if (guideRecipeField == 4) {
    waterTempC = static_cast<uint8_t>(constrain(static_cast<int>(waterTempC) + static_cast<int>(delta),
                                                static_cast<int>(WATER_TEMP_MIN), static_cast<int>(WATER_TEMP_MAX)));
  }
  uiNeedsImmediateRedraw = true;
}

// ---------------------------------------------------------------------------
// Encoder y clics
// ---------------------------------------------------------------------------
// Giro en pour-over: siguiente / anterior paso. Devuelve true si hay que abrir la medida de TDS
// (al salir del resumen con un R2 vinculado).
bool guideRotate(int dir) {
  const unsigned long now = millis();
  if ((uint32_t)(now - guideLastStepMs) < GUIDE_STEP_LOCK_MS) return false;
  registerUserOrScaleActivity();

  if (dir > 0) {
    switch (guideStep) {
      case GuideStep::Ready:
        guideRecipeDone();
        guidePrevTareOffset = softwareTareOffset;
        guideClearDose();
        traceBegin();
        guideStartPendingTare();          // el vaso dosificador: su peso se fija al estar quieto
        guideGoTo(GuideStep::Beans);
        break;
      case GuideStep::Beans:
        if (guidePendingTare) return false;
        guideBeansG = guideBeansCandidate;   // 0 = no se pesaron
        guideDoseG  = guideBeansG;
        if (guideBeansG > 0.0f) {
          traceEvent(TE_BEANS, 0, guideBeansG);
          Serial.printf("[GUIA] Granos: %.1f g.\n", guideBeansG);
        }
        guideStartPendingTare();          // el server
        guideGoTo(GuideStep::Server);
        break;
      case GuideStep::Server:
        if (guidePendingTare) return false;
        guideStartPendingTare();          // el V60 (sin el café)
        guideGoTo(GuideStep::Dripper);
        break;
      case GuideStep::Dripper:
        if (guidePendingTare) return false;
        guideGoTo(GuideStep::Ground);
        break;
      case GuideStep::Ground: {
        // El café que hay en el V60 es la dosis real; vuelve a tarar para el bloom
        const float raw = rawWeightGrams;
        const float net = raw - dripperEmptyRawGrams;
        if (net >= 1.0f && net <= 100.0f) {
          guideGroundG    = net;
          guideDoseG      = net;
          dripperRawGrams = raw;
          setTareOffset(raw);
          traceEvent(TE_GROUND, 0, net);
          if (guideBeansG > 0.0f) Serial.printf("[GUIA] Cafe molido: %.1f g (retencion %+.1f g).\n", net, guideBeansG - net);
          else                    Serial.printf("[GUIA] Cafe molido: %.1f g.\n", net);
        } else {
          guideGroundG    = 0.0f;         // el café ya estaba en el V60 al tarar
          guideDoseG      = guideBeansG;
          dripperRawGrams = dripperEmptyRawGrams;
        }
        guideArmBrew();
        guideGoTo(GuideStep::Bloom);
        break;
      }
      case GuideStep::Bloom:
        if (brewStartMs != 0) {
          bloomEndMs      = brewElapsedMs();
          bloomWaterG     = currentWeightGrams;
          poursAtBloomEnd = pourSegmenter.livePours(currentWeightGrams);
        }
        guideGoTo(GuideStep::Brew);
        break;
      case GuideStep::Brew:
        if (brewStartMs != 0) {
          pourEndMs   = brewElapsedMs();
          waterTotalG = currentWeightGrams;
        }
        guideGoTo(GuideStep::Drawdown);
        break;
      case GuideStep::Drawdown:
        guideStopAndSave(now);
        guideGoTo(GuideStep::Yield);
        break;
      case GuideStep::Yield:
        traceSave(guidePourId);
        guideGoTo(GuideStep::Summary);
        break;
      case GuideStep::Summary: {
        const bool tds = (guidePourId != 0 && r2Paired());
        guideFinish();
        guideGoTo(GuideStep::Ready);
        return tds;
      }
    }
    return false;
  }

  switch (guideStep) {
    case GuideStep::Ready:
      break;
    case GuideStep::Beans:
      guidePendingTare = false;
      setTareOffset(guidePrevTareOffset);
      traceStop();
      guideClearDose();
      guideGoTo(GuideStep::Ready);
      break;
    case GuideStep::Server:
      guidePendingTare = false;
      setTareOffset(cupRawGrams);         // los granos que se pesaron siguen como candidatos
      guideBeansG = 0.0f;
      guideDoseG  = 0.0f;
      guideGoTo(GuideStep::Beans);
      break;
    case GuideStep::Dripper:
      guidePendingTare = false;
      setTareOffset(serverRawGrams);
      guideGoTo(GuideStep::Server);
      break;
    case GuideStep::Ground:
      setTareOffset(dripperEmptyRawGrams);
      guideGoTo(GuideStep::Dripper);
      break;
    case GuideStep::Bloom:
      guideCancelBrew();
      setTareOffset(dripperEmptyRawGrams);   // el café vuelve a verse
      guideGroundG = 0.0f;
      guideDoseG   = guideBeansG;
      guideGoTo(GuideStep::Ground);
      break;
    case GuideStep::Brew:
      bloomEndMs = 0;
      bloomWaterG = 0.0f;
      guideGoTo(GuideStep::Bloom);
      break;
    case GuideStep::Drawdown:
      pourEndMs = 0;
      guideGoTo(GuideStep::Brew);
      break;
    case GuideStep::Yield:
      if (brewStartMs != 0) {             // sigue contando desde el mismo inicio
        timerStartMillis   = brewStartMs;
        timerAccumulatedMs = 0;
        timerRunning       = true;
        traceEvent(TE_TIMER_START, 1, 0.0f);
      }
      yieldMeasured = false;
      guideGoTo(GuideStep::Drawdown);
      break;
    case GuideStep::Summary:
      guideGoTo(GuideStep::Yield);
      break;
  }
  return false;
}

// 2 clics en pour-over: tara del paso actual (sin perder el server)
void guideTare() {
  const float raw = rawWeightGrams;
  guidePendingTare = false;
  switch (guideStep) {
    case GuideStep::Ready:
      setTareOffset(raw);
      break;
    case GuideStep::Beans:
      cupRawGrams         = raw;
      guideBeansCandidate = 0.0f;
      guideBeansLocked    = false;
      setTareOffset(raw);
      break;
    case GuideStep::Server:
      serverRawGrams = raw;
      setTareOffset(raw);
      break;
    case GuideStep::Ground:
      // Con el café ya en el V60, tarar lo borraría (y el filtro mojado no deja repetir la medida):
      // 2 clics no hacen nada y la pantalla recuerda que se sigue girando
      if (raw - dripperEmptyRawGrams >= 1.0f) {
        guideNoticeUntilMs = millis() + 2500UL;
        uiNeedsImmediateRedraw = true;
        Serial.printf("[GUIA] 2 clics ignorados: hay %.1f g de cafe en el V60.\n", raw - dripperEmptyRawGrams);
        return;
      }
      [[fallthrough]];                    // V60 vacío: otra vez a 0
    case GuideStep::Dripper:              // el V60 otra vez a 0 (sin el café)
      dripperEmptyRawGrams = raw;
      dripperRawGrams      = raw;
      setTareOffset(raw);
      break;
    case GuideStep::Bloom:
    case GuideStep::Brew:
    case GuideStep::Drawdown:
      // Volver a empezar los vertidos (con el V60 tal como está)
      guideCancelBrew();
      dripperRawGrams = raw;
      setTareOffset(raw);
      if (guideStep != GuideStep::Bloom) guideGoTo(GuideStep::Bloom);
      break;
    case GuideStep::Yield:
    case GuideStep::Summary:
      if (guideStep == GuideStep::Yield) traceSave(guidePourId);
      guideFinish();
      guideGoTo(GuideStep::Ready);
      break;
  }
  triggerLedFlash();
  Serial.printf("[GUIA] 2 clics: tara en %s.\n", guideStepName(guideStep));
}

// 1 clic en pour-over: siguiente vista (guiada, báscula, perfil y, con la cafetera, la del puente)
void guideNextView() {
  uint8_t v = currentScreenView;
  if (v == 0)      v = 1;
  else if (v == 1) v = 2;
  else if (v == 2) v = bookooMachineConnected ? BOOKOO_SCREEN_VIEW : 0;
  else             v = 0;
  currentScreenView = v;
  traceEvent(TE_CLICK, v, 0.0f);
}

// ---------------------------------------------------------------------------
// Cada lectura de la Dot (desde processWeightSample, ~10 Hz)
// ---------------------------------------------------------------------------
void guideOnWeightSample(unsigned long ms, float raw, float net) {
  traceSample(ms, raw, net, currentFlowRateGps);

  if (fabsf(raw - guideStableRef) > GUIDE_STABLE_BAND_G || guideStableSinceMs == 0) {
    guideStableRef     = raw;
    guideStableSinceMs = ms;
  }

  if (brewMode != BrewMode::PourOver || shotPhase != ShotPhase::Idle) return;
  if (guideStep != GuideStep::Bloom && guideStep != GuideStep::Brew && guideStep != GuideStep::Drawdown) return;

  const pourseg::Event ev = pourSegmenter.add(static_cast<uint32_t>(ms), net, currentFlowRateGps);
  if (pourSegmenter.active() && currentFlowRateGps > guideCandPeakGps) guideCandPeakGps = currentFlowRateGps;
  switch (ev) {
    case pourseg::Event::PourStart:
      guideCandPeakGps = currentFlowRateGps;
      traceEvent(TE_POUR_START, 0, net);
      break;
    case pourseg::Event::PourAccepted:
    case pourseg::Event::PourMerged:
      if (guideCandPeakGps > guidePeakGps) guidePeakGps = guideCandPeakGps;
      traceEvent(ev == pourseg::Event::PourAccepted ? TE_POUR_OK : TE_POUR_MERGED, pourSegmenter.pours(), net);
      break;
    case pourseg::Event::TapRejected:
      traceEvent(TE_TAP, pourSegmenter.taps(), net);
      break;
    default: break;
  }

  // Arranque del cronómetro: el chorro ya deja agua de verdad (un toque de la tetera no)
  if (brewStartMs == 0 && guideStep != GuideStep::Drawdown) {
    if (pourSegmenter.active() && pourSegmenter.candidateGain(net) >= GUIDE_START_GAIN_G) {
      guideStartTimer(pourSegmenter.candidateStartMs());
    } else if (net >= 5.0f && currentFlowRateGps >= 0.3f) {
      guideStartTimer(ms);   // vertido muy lento que no llega al umbral del segmentador
    }
  }
}

// ---------------------------------------------------------------------------
// Servicio periódico (desde updateFlowAndAutoTimer)
// ---------------------------------------------------------------------------
void serviceGuide(unsigned long now) {
  // Taras pendientes de Server y V60: en cuanto el peso está quieto
  if (guidePendingTare) {
    const bool still   = guideStableForMs() >= GUIDE_STABLE_MS;
    const bool timeout = (uint32_t)(now - guidePendingSinceMs) >= GUIDE_COMMIT_MAX_MS;
    if (still || timeout) {
      guidePendingTare = false;
      const float raw = rawWeightGrams;
      if (guideStep == GuideStep::Beans) {
        cupRawGrams = raw;
        Serial.printf("[GUIA] Vaso: %.1f g (bruto).\n", raw);
      } else if (guideStep == GuideStep::Server) {
        serverRawGrams = raw;
        Serial.printf("[GUIA] Server: %.1f g (bruto).\n", raw);
      } else if (guideStep == GuideStep::Dripper) {
        dripperEmptyRawGrams = raw;
        dripperRawGrams      = raw;
        Serial.printf("[GUIA] V60: %.1f g.\n", raw - serverRawGrams);
      }
      setTareOffset(raw);
      uiNeedsImmediateRedraw = true;
    }
  }

  // Granos: el último peso quieto con algo en el vaso. Al quitar el vaso (para moler) quedan fijados: lo
  // que se ponga después (el server) ya no son granos. 2 clics vuelven a empezar.
  if (guideStep == GuideStep::Beans && !guidePendingTare) {
    const float net = rawWeightGrams - softwareTareOffset;
    if (guideBeansCandidate > 0.0f && net < -0.5f) guideBeansLocked = true;
    if (!guideBeansLocked && guideStableForMs() >= GUIDE_STABLE_MS &&
        net >= 1.0f && net <= 100.0f && fabsf(net - guideBeansCandidate) >= 0.05f) {
      guideBeansCandidate    = roundf(net * 10.0f) / 10.0f;
      uiNeedsImmediateRedraw = true;
    }
  }

  // Yield: al quitar el V60 queda el server con la bebida. Bebida = bruto - server.
  if (guideStep == GuideStep::Yield && !yieldMeasured && brewStartMs != 0) {
    const float water   = waterTotalG;
    const float bev     = rawWeightGrams - serverRawGrams;
    const bool sane     = water > 20.0f && bev > 0.4f * water && bev <= water + 2.0f;
    if (!guideV60Removed() || !sane || fabsf(bev - guideYieldCandidate) > GUIDE_STABLE_BAND_G) {
      guideYieldCandidate = bev;
      guideYieldSinceMs   = 0;
    } else if (guideYieldSinceMs == 0) {
      guideYieldSinceMs = now;
    } else if ((uint32_t)(now - guideYieldSinceMs) >= 1500UL) {
      yieldGrams    = bev;
      yieldMeasured = true;
      traceEvent(TE_YIELD, 0, bev);
      if (guidePourId != 0 && !demoLiveActive) storeMeasuredBeverage(guidePourId, bev, water, brewDoseGrams());
      Serial.printf("[GUIA] Yield: %.1f g (agua %.1f g).\n", bev, water);
      uiNeedsImmediateRedraw = true;
    }
  }
}
