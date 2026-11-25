// ============================================================================
// MODO ESPRESSO: RECETA, SHOT E HISTORIAL DE SHOTS
// Pestaña del sketch: el IDE la concatena tras firmware.ino (orden alfabético).
// Tipos, constantes y variables globales compartidas están en la pestaña principal.
//
// Ciclo del shot (shotPhase):
//   Idle     -> 1 clic: arranca el cronómetro (Running) y espera la primera gota (preinfusión).
//               Sin clic, arranca solo al caer la primera gota (sin preinfusión medible).
//   Running  -> marca la primera gota; muestrea peso y caudal a 5 Hz. Termina con 1 clic, solo cuando
//               deja de caer café (4 s sin subir el peso) o a los 120 s.
//   Settling -> 3 s tras un corte manual o de la cafetera para medir lo que sigue goteando (rebase);
//               después guarda el shot.
// La cafetera (puente BooKoo) arranca y para los shots con sus comandos en cualquier modo; esos shots
// también se guardan aquí.
// ============================================================================

// Definidas en pestañas posteriores
void registerUserOrScaleActivity();
void fermentResetStep();

static constexpr uint8_t  MAX_SAVED_SHOTS       = 10;
static constexpr uint8_t  SHOT_GRAPH_POINTS     = 60;     // 2 px por punto en la OLED
static constexpr uint32_t SHOT_SAMPLE_MS        = 200;    // 5 Hz
static constexpr uint32_t SHOT_SETTLE_MS        = 3000;   // medida del rebase tras cortar
static constexpr uint32_t SHOT_FLOW_END_MS      = 4000;   // sin subir el peso -> el shot terminó
static constexpr uint32_t SHOT_MAX_MS           = 120000;
static constexpr float    SHOT_FIRST_DROP_G     = 0.3f;   // peso neto que cuenta como primera gota
static const char*        NVS_SHOT_NAMESPACE    = "tmdot_shots";

static constexpr uint8_t SHOT_FLAG_MACHINE  = 0x01;  // lo cortó la cafetera
static constexpr uint8_t SHOT_FLAG_AUTOSTOP = 0x02;  // terminó solo (dejó de caer café)
static constexpr uint8_t SHOT_FLAG_PI_KNOWN = 0x04;  // la preinfusión es válida (arrancó con clic o la cafetera)

struct ShotRecord {
  uint32_t magic;            // 0x53484F54 ("SHOT")
  uint16_t shotId;
  uint16_t timeDs;           // duración en décimas de segundo
  uint16_t piDs;             // tiempo hasta la primera gota (décimas); válido con SHOT_FLAG_PI_KNOWN
  uint8_t  targetSec;
  uint8_t  flags;
  float    doseGrams;
  float    yieldGrams;       // peso final en la taza (tras el goteo)
  float    targetGrams;
  float    weightAtStop;     // peso en el momento del corte
  uint8_t  weightCurve[SHOT_GRAPH_POINTS];   // 0..38 px
  uint8_t  flowCurve[SHOT_GRAPH_POINTS];     // 0..38 px
};
static_assert(sizeof(ShotRecord) == 148, "ShotRecord cambió de tamaño: invalidaría los shots guardados");

ShotRecord savedShots[MAX_SAVED_SHOTS];
uint8_t savedShotsCount        = 0;
uint16_t nextShotId            = 1;
int8_t selectedShotSlot        = 0;
uint16_t lastSavedShotId       = 0;

static unsigned long shotLastSampleMs  = 0;
static unsigned long shotSettleUntilMs = 0;
static float shotPeakFlow              = 0.0f;
static bool espArmed                   = false;  // arranque con la primera gota armado por una tara (2 clics)

const char* brewModeName(BrewMode m) {
  return m == BrewMode::Espresso ? "ESPRESSO" : (m == BrewMode::Fermento ? "FERMENTO" : "POUR-OVER");
}

// Siguiente modo del menú: pour-over -> espresso -> fermento -> pour-over
BrewMode nextBrewMode(BrewMode m) {
  if (m == BrewMode::PourOver) return BrewMode::Espresso;
  if (m == BrewMode::Espresso) return BrewMode::Fermento;
  return BrewMode::PourOver;
}

float espressoTargetGrams() { return espDoseGrams * espRatio; }
bool espressoArmed() { return espArmed; }

// Peso al que conviene cortar: objetivo menos lo que suele caer después (aprendido de los shots)
float espressoCutGrams() {
  float reb = espRebaseAvgGrams;
  if (reb < 0.0f) reb = 0.0f;
  if (reb > 8.0f) reb = 8.0f;
  return espressoTargetGrams() - reb;
}

// ---------------------------------------------------------------------------
// Ajustes y shots en la NVS
// ---------------------------------------------------------------------------
void loadEspressoSettingsAndShots() {
  Preferences p;
  p.begin(NVS_POUR_NAMESPACE, true);
  const uint8_t m   = p.getUChar("mode", 0);
  brewMode          = (m == 1) ? BrewMode::Espresso : (m == 2 ? BrewMode::Fermento : BrewMode::PourOver);
  espDoseGrams      = p.getFloat("esp_dose", 18.0f);
  espRatio          = p.getFloat("esp_ratio", 2.0f);
  espTargetSec      = p.getUChar("esp_tsec", 28);
  espRebaseAvgGrams = p.getFloat("esp_reb", 2.0f);
  p.end();
  if (isnan(espDoseGrams) || espDoseGrams < ESP_DOSE_MIN_G || espDoseGrams > ESP_DOSE_MAX_G) espDoseGrams = 18.0f;
  if (isnan(espRatio) || espRatio < ESP_RATIO_MIN || espRatio > ESP_RATIO_MAX) espRatio = 2.0f;
  if (espTargetSec < ESP_TIME_MIN_S || espTargetSec > ESP_TIME_MAX_S) espTargetSec = 28;
  if (isnan(espRebaseAvgGrams) || espRebaseAvgGrams < 0.0f || espRebaseAvgGrams > 8.0f) espRebaseAvgGrams = 2.0f;

  Preferences s;
  s.begin(NVS_SHOT_NAMESPACE, true);
  nextShotId      = s.getUShort("next_id", 1);
  savedShotsCount = 0;
  ShotRecord tmp;
  for (uint8_t slot = 0; slot < MAX_SAVED_SHOTS; slot++) {
    char key[8];
    snprintf(key, sizeof(key), "s_%u", slot);
    if (s.getBytesLength(key) != sizeof(ShotRecord)) continue;
    s.getBytes(key, &tmp, sizeof(ShotRecord));
    if (tmp.magic == 0x53484F54UL && tmp.shotId != 0 && (tmp.shotId % MAX_SAVED_SHOTS) == slot &&
        !isnan(tmp.yieldGrams)) {
      savedShots[savedShotsCount++] = tmp;
    }
  }
  s.end();
  // Orden cronológico por id
  for (uint8_t i = 1; i < savedShotsCount; i++) {
    ShotRecord key = savedShots[i];
    int8_t j = static_cast<int8_t>(i) - 1;
    while (j >= 0 && savedShots[j].shotId > key.shotId) { savedShots[j + 1] = savedShots[j]; j--; }
    savedShots[j + 1] = key;
  }
  if (savedShotsCount > 0) {
    uint16_t maxId = savedShots[savedShotsCount - 1].shotId;
    if (nextShotId <= maxId) nextShotId = static_cast<uint16_t>(maxId + 1);
    selectedShotSlot = static_cast<int8_t>(savedShotsCount - 1);
  }
  Serial.printf("[ESPRESSO] Modo %s | receta %.1f g x 1:%.1f = %.1f g en %u s | %u shots guardados\n",
                brewModeName(brewMode), espDoseGrams, espRatio,
                espressoTargetGrams(), espTargetSec, savedShotsCount);
}

void saveEspressoSettings() {
  Preferences p;
  p.begin(NVS_POUR_NAMESPACE, false);
  p.putUChar("mode", static_cast<uint8_t>(brewMode));
  p.putFloat("esp_dose", espDoseGrams);
  p.putFloat("esp_ratio", espRatio);
  p.putUChar("esp_tsec", espTargetSec);
  p.putFloat("esp_reb", espRebaseAvgGrams);
  p.end();
}

void setBrewMode(BrewMode m) {
  if (m == brewMode) return;
  brewMode = m;
  // Cambiar de modo deja el cronómetro y la grabación en limpio
  timerRunning       = false;
  timerAccumulatedMs = 0;
  autoTimerArmed     = (m == BrewMode::PourOver) ? autoTimerEnabled : false;
  shotPhase          = ShotPhase::Idle;
  espArmed           = false;
  resetLivePourRecording();
  saveEspressoSettings();
  fermentResetStep();
  Serial.printf("[MODO] %s\n", brewModeName(m));
}

// ---------------------------------------------------------------------------
// Guardado del shot
// ---------------------------------------------------------------------------
static void storeShotRecord(bool persistToNvs) {
  const unsigned long durMs = timerAccumulatedMs;
  if (durMs < 3000UL || currentWeightGrams < 1.0f || liveSampleCount < 2) {
    Serial.println("[SHOT] Demasiado corto o sin peso: no se guarda.");
    return;
  }
  uint8_t targetIdx = savedShotsCount;
  if (targetIdx >= MAX_SAVED_SHOTS) {
    for (uint8_t i = 0; i < MAX_SAVED_SHOTS - 1; i++) savedShots[i] = savedShots[i + 1];
    targetIdx = MAX_SAVED_SHOTS - 1;
  } else {
    savedShotsCount++;
  }
  ShotRecord& r = savedShots[targetIdx];
  memset(&r, 0, sizeof(ShotRecord));
  r.magic        = 0x53484F54UL;
  r.shotId       = nextShotId++;
  r.timeDs       = static_cast<uint16_t>(durMs / 100UL);
  r.targetSec    = espTargetSec;
  r.doseGrams    = espDoseGrams;
  r.yieldGrams   = currentWeightGrams;
  r.targetGrams  = espressoTargetGrams();
  r.weightAtStop = shotWeightAtStop;
  if (shotByMachine)   r.flags |= SHOT_FLAG_MACHINE;
  if (shotAutoStopped) r.flags |= SHOT_FLAG_AUTOSTOP;
  if (shotPiKnown && shotFirstDropMs >= shotStartMs) {
    r.flags |= SHOT_FLAG_PI_KNOWN;
    r.piDs = static_cast<uint16_t>((shotFirstDropMs - shotStartMs) / 100UL);
  }

  // Curvas: las muestras a 5 Hz, reducidas a 60 columnas
  float maxW = r.yieldGrams;
  if (r.targetGrams > maxW) maxW = r.targetGrams;
  maxW *= 1.05f;
  if (maxW < 10.0f) maxW = 10.0f;
  float maxF = (shotPeakFlow > 3.0f) ? shotPeakFlow : 3.0f;
  for (uint8_t x = 0; x < SHOT_GRAPH_POINTS; x++) {
    uint16_t idx = static_cast<uint16_t>((static_cast<uint32_t>(x) * (liveSampleCount - 1)) / (SHOT_GRAPH_POINTS - 1));
    float sw = psramLiveSamples[idx].weightGrams, sf = psramLiveSamples[idx].flowGps;
    int hW = static_cast<int>((sw > 0.0f ? sw : 0.0f) / maxW * 38.0f);
    int hF = static_cast<int>((sf > 0.0f ? sf : 0.0f) / maxF * 38.0f);
    r.weightCurve[x] = static_cast<uint8_t>(hW > 38 ? 38 : hW);
    r.flowCurve[x]   = static_cast<uint8_t>(hF > 38 ? 38 : hF);
  }

  // Aprender el rebase de los cortes manuales y de la cafetera (no de los que terminan solos)
  if (!shotAutoStopped && shotWeightAtStop > 1.0f) {
    float reb = r.yieldGrams - shotWeightAtStop;
    if (reb >= 0.0f && reb < 8.0f) espRebaseAvgGrams = 0.7f * espRebaseAvgGrams + 0.3f * reb;
  }

  if (persistToNvs) {
    Preferences s;
    s.begin(NVS_SHOT_NAMESPACE, false);
    s.putUShort("next_id", nextShotId);
    char key[8];
    snprintf(key, sizeof(key), "s_%u", r.shotId % MAX_SAVED_SHOTS);
    s.putBytes(key, &r, sizeof(ShotRecord));
    s.end();
    saveEspressoSettings();  // rebase aprendido
  }

  selectedShotSlot       = static_cast<int8_t>(targetIdx);
  lastSavedShotId        = r.shotId;
  bannerIsShot           = true;
  bannerIsLot            = false;
  pourSavedBannerUntilMs = millis() + 2500UL;
  uiNeedsImmediateRedraw = true;
  Serial.printf("[SHOT] #%u guardado%s: %.1f g (obj %.1f) | 1:%.2f | %.1f s | PI %s%.1f s | rebase %+.1f g | %s\n",
                r.shotId, persistToNvs ? "" : " (solo RAM)", r.yieldGrams, r.targetGrams,
                r.doseGrams > 0.1f ? r.yieldGrams / r.doseGrams : 0.0f, r.timeDs / 10.0f,
                (r.flags & SHOT_FLAG_PI_KNOWN) ? "" : "? ", r.piDs / 10.0f, r.yieldGrams - r.weightAtStop,
                shotByMachine ? "corte cafetera" : (shotAutoStopped ? "fin automatico" : "corte manual"));
}

// ---------------------------------------------------------------------------
// Ciclo del shot
// ---------------------------------------------------------------------------
static void shotBegin(unsigned long now, bool piKnown, bool byMachine) {
  resetLivePourRecording();
  timerAccumulatedMs = 0;
  timerStartMillis   = now;
  timerRunning       = true;
  autoTimerArmed     = false;
  espArmed           = false;
  shotPhase          = ShotPhase::Running;
  shotStartMs        = now;
  shotFirstDropMs    = piKnown ? 0 : now;
  shotPiKnown        = piKnown;
  shotByMachine      = byMachine;
  shotAutoStopped    = false;
  shotWeightAtStop   = 0.0f;
  shotPeakFlow       = 0.0f;
  shotLastSampleMs   = 0;
  pourSavedBannerUntilMs = 0;
  uiNeedsImmediateRedraw = true;
  Serial.printf("[SHOT] Inicio (%s).\n", byMachine ? "cafetera" : (piKnown ? "clic" : "primera gota"));
}

// Para el cronómetro en stopAtMs y pasa a medir el goteo (o guarda ya si terminó solo)
static void shotEnd(unsigned long stopAtMs, bool autoStopped) {
  if (shotPhase != ShotPhase::Running) return;
  if (timerRunning) {
    unsigned long until = (stopAtMs > timerStartMillis) ? stopAtMs : timerStartMillis;
    timerAccumulatedMs += (until - timerStartMillis);
    timerRunning = false;
  }
  shotAutoStopped   = autoStopped;
  shotWeightAtStop  = currentWeightGrams;
  shotPhase         = ShotPhase::Settling;
  shotSettleUntilMs = millis() + (autoStopped ? 300UL : SHOT_SETTLE_MS);
  uiNeedsImmediateRedraw = true;
}

// 2 clics (tara y cronómetro a 0): cancela el shot y arma el arranque con la primera gota
void espressoCancelShot() {
  shotPhase    = ShotPhase::Idle;
  bannerIsShot = false;
  espArmed     = true;
}

// 1 clic en la pantalla de pesaje en modo espresso
void espressoClick() {
  unsigned long now = millis();
  if (shotPhase == ShotPhase::Idle) {
    shotBegin(now, true, false);
  } else if (shotPhase == ShotPhase::Running) {
    shotEnd(now, false);
  }
  // Settling: se ignora (está midiendo el goteo; termina en 3 s)
}

// Comandos de la cafetera por el puente (en cualquier modo)
void espressoMachineShotStart() {
  if (shotPhase == ShotPhase::Running) return;
  shotBegin(millis(), true, true);
}
void espressoMachineShotStop() {
  if (shotPhase == ShotPhase::Running) shotEnd(millis(), false);
}

// Llamada desde updateFlowAndAutoTimer() (40_brew.ino)
void serviceEspressoShot(unsigned long now) {
  const float w    = currentWeightGrams;
  const float flow = currentFlowRateGps;

  if (shotPhase == ShotPhase::Idle) {
    // Sin clic: arranca solo con la primera gota (en modo espresso, tras una tara y sin la cafetera mandando)
    if (brewMode == BrewMode::Espresso && espArmed && !bookooMachineConnected && !tareSettlingActive &&
        w >= 0.5f && flow >= 0.3f && lastWeightStepChangeMs > lastTareCommandMs &&
        (now - lastTareCommandMs) >= 250UL && pourSavedBannerUntilMs < now) {
      shotBegin(now, false, false);
    }
    return;
  }

  if (shotPhase == ShotPhase::Running) {
    if (shotFirstDropMs == 0 && w >= SHOT_FIRST_DROP_G) {
      shotFirstDropMs = now;
      Serial.printf("[SHOT] Primera gota a %.1f s.\n", (now - shotStartMs) / 1000.0f);
    }
    if (flow > shotPeakFlow) shotPeakFlow = flow;
    if (psramLiveSamples != nullptr && (shotLastSampleMs == 0 || now - shotLastSampleMs >= SHOT_SAMPLE_MS)) {
      shotLastSampleMs = now;
      if (liveSampleCount < MAX_LIVE_POUR_SAMPLES) {
        psramLiveSamples[liveSampleCount].weightGrams = (w > 0.0f) ? w : 0.0f;
        psramLiveSamples[liveSampleCount].flowGps     = (flow > 0.0f) ? flow : 0.0f;
        liveSampleCount++;
      }
    }
    // Fin solo: tras la primera gota, 4 s sin que suba el peso (la cafetera ya no echa café).
    // Con la cafetera conectada manda ella.
    const unsigned long lastStep = lastWeightStepChangeMs;
    if (!shotByMachine && shotFirstDropMs != 0 && w >= 3.0f && lastStep > shotFirstDropMs &&
        (now - lastStep) >= SHOT_FLOW_END_MS) {
      shotEnd(lastStep, true);
    } else if ((now - shotStartMs) >= SHOT_MAX_MS) {
      shotEnd(now, false);
    }
    return;
  }

  // Settling: guardar tras medir el goteo
  if ((long)(now - shotSettleUntilMs) >= 0) {
    shotPhase = ShotPhase::Idle;
    storeShotRecord(!demoLiveActive);
  }
}

// ----------------------------------------------------------------------------
// DEMOSTRACIÓN EN VIVO DE UN ESPRESSO (orden serie 'e'), para probar el modo y grabar la pantalla.
// Hace lo mismo que una persona con la receta actual: tara (2 clics), 1 clic al pulsar la cafetera,
// 7 s de preinfusión, flujo de hasta 2 g/s, 1 clic al llegar al punto de corte que marca la barra y el
// goteo final. El peso simulado entra en la cola de muestras a 10 Hz en lugar del de la Dot. El shot se
// guarda solo en RAM; al acabar se restauran el modo, el número del siguiente shot y el goteo aprendido.
// ----------------------------------------------------------------------------
void tareAndResetStopwatch();

static bool demoEspRunning          = false;
static uint8_t demoEspStage         = 0;     // 0 tara, 1 preinfusión, 2 flujo, 3 goteo
static unsigned long demoEspStartMs = 0, demoEspLastMs = 0, demoEspMarkMs = 0;
static float demoEspBase            = 0.0f, demoEspW = 0.0f;
static BrewMode demoEspSavedMode    = BrewMode::PourOver;
static uint16_t demoEspSavedNextId  = 1;
static float demoEspSavedRebase     = 2.0f;

void startDemoEspresso() {
  if (demoLiveActive) return;
  demoEspSavedMode   = brewMode;
  demoEspSavedNextId = nextShotId;
  demoEspSavedRebase = espRebaseAvgGrams;
  brewMode           = BrewMode::Espresso;   // sin guardar en la NVS
  uiState            = UiState::Main;
  currentScreenView  = 0;
  pourSavedBannerUntilMs = 0;
  demoEspBase        = rawWeightGrams;       // taza sobre la báscula
  demoEspW           = 0.0f;
  demoEspStage       = 0;
  if (weightQueue != nullptr) xQueueReset(weightQueue);
  demoLiveActive     = true;
  demoEspRunning     = true;
  tareAndResetStopwatch();                   // "2 clics": tara y arranque armado
  demoEspStartMs     = millis();
  demoEspLastMs      = 0;
  registerUserOrScaleActivity();
  Serial.printf("[DEMO] Espresso en vivo: %.1f g -> %.1f g (corte a %.1f g).\n",
                espDoseGrams, espressoTargetGrams(), espressoCutGrams());
}

void serviceDemoEspresso() {
  if (!demoEspRunning) return;
  const unsigned long now = millis();
  if (demoEspLastMs != 0 && (now - demoEspLastMs) < 100UL) return;
  demoEspLastMs = now;
  const float t = (now - demoEspStartMs) / 1000.0f;
  float flow = 0.0f;

  if (demoEspStage == 0) {
    if (t >= 2.0f) { espressoClick(); demoEspStage = 1; demoEspMarkMs = now; }   // pulsa la cafetera
  } else if (demoEspStage == 1) {
    if ((now - demoEspMarkMs) >= 7000UL) { demoEspStage = 2; demoEspMarkMs = now; }  // preinfusión
  } else if (demoEspStage == 2) {
    const float td = (now - demoEspMarkMs) / 1000.0f;
    flow = (td < 4.0f) ? 0.5f * td : 2.0f - 0.02f * (td - 4.0f);
    if (demoEspW >= espressoCutGrams()) {     // llega a la marca de corte: 1 clic
      espressoClick();
      demoEspStage = 3;
      demoEspMarkMs = now;
    }
  } else {
    const float td = (now - demoEspMarkMs) / 1000.0f;
    flow = 1.9f * expf(-td / 1.0f);            // goteo tras cortar (~1.9 g)
    if (td > 6.0f && shotPhase == ShotPhase::Idle) {
      demoEspRunning     = false;
      demoLiveActive     = false;
      brewMode           = demoEspSavedMode;
      nextShotId         = demoEspSavedNextId;
      espRebaseAvgGrams  = demoEspSavedRebase;
      Serial.println("[DEMO] Fin del espresso de demostracion.");
      return;
    }
  }
  demoEspW += flow * 0.1f;
  WeightSample s;
  s.ms     = now;
  s.grams  = demoEspBase + roundf(demoEspW * 10.0f) / 10.0f;
  s.stable = true;
  if (weightQueue != nullptr) xQueueSend(weightQueue, &s, 0);
  registerUserOrScaleActivity();
}
