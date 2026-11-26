// ============================================================================
// MEMORIA: NVS (báscula vinculada, dosis, historial) Y PSRAM
// MAC de la Dot, grabación en vivo en PSRAM y anillo de 10 pour-overs en NVS.
// Pestaña del sketch: el IDE la concatena tras firmware.ino (orden alfabético).
// Tipos, constantes y variables globales compartidas están en la pestaña principal.
// ============================================================================

// ============================================================================
// MEMORIA NO VOLÁTIL (NVS)
// ============================================================================
void loadSavedScaleFromNVS() {
  preferences.begin(NVS_NAMESPACE, true);
  String mac = preferences.getString(NVS_KEY_MAC, "");
  savedTimemoreAddrType = preferences.getUChar(NVS_KEY_TYPE, 0);
  preferences.end();

  if (mac.length() >= 17) {
    strncpy(savedTimemoreMac, mac.c_str(), sizeof(savedTimemoreMac) - 1);
    savedTimemoreMac[sizeof(savedTimemoreMac) - 1] = '\0';
    hasSavedMac = true;
    Serial.printf("[NVS] Timemore Dot guardada: %s (tipo=%u)\n", savedTimemoreMac, savedTimemoreAddrType);
  } else {
    // Placa nueva: sin báscula vinculada. Se vincula la primera Timemore Dot que se encuentre
    // (servicio 0xFFF0/0xFFE0 o nombre Timemore) y su MAC se guarda al recibir la primera trama válida.
    memset(savedTimemoreMac, 0, sizeof(savedTimemoreMac));
    savedTimemoreAddrType = 0;
    hasSavedMac = false;
    Serial.println("[NVS] Sin Timemore Dot vinculada -> se vinculara la primera que se encuentre.");
  }
}

void saveScaleToNVS(const char* mac, uint8_t addrType) {
  if (!mac || strlen(mac) < 17) return;
  if (hasSavedMac && strcasecmp(savedTimemoreMac, mac) == 0 && savedTimemoreAddrType == addrType) {
    return;
  }
  preferences.begin(NVS_NAMESPACE, false);
  preferences.putString(NVS_KEY_MAC, mac);
  preferences.putUChar(NVS_KEY_TYPE, addrType);
  preferences.end();

  strncpy(savedTimemoreMac, mac, sizeof(savedTimemoreMac) - 1);
  savedTimemoreAddrType = addrType;
  hasSavedMac = true;
  Serial.printf("[NVS] Nueva Timemore Dot guardada: %s (tipo=%u)\n", savedTimemoreMac, savedTimemoreAddrType);
}

void clearScaleFromNVS() {
  preferences.begin(NVS_NAMESPACE, false);
  preferences.clear();
  preferences.end();
  memset(savedTimemoreMac, 0, sizeof(savedTimemoreMac));
  hasSavedMac = false;
  clearRejectedMacs();   // una Dot que antes no mandó peso vuelve a ser candidata
  Serial.println("[NVS] Memoria NVS borrada. Buscando cualquier Timemore Dot...");

  for (int i = 0; i < 4; i++) {
    setRgbColor(0, LED_BRIGHTNESS * 2, LED_BRIGHTNESS * 2);
    delay(80);
    setRgbColor(0, 0, 0);
    delay(80);
  }
  if (scaleConnected && pScaleClient != nullptr) {
    pScaleClient->disconnect();
  }
}

// ============================================================================
// DETECCIÓN DE PSRAM / FLASH Y MOTOR DE ALMACENAMIENTO DE POUR-OVERS
// ============================================================================
static const char* NVS_POUR_NAMESPACE = "tmdot_pours";

void resetLivePourRecording() {
  activeRecordingPourId  = 0;
  liveSampleCount        = 0;
  lastLiveSampleMs       = 0;
  currentPourPulsesCount = 0;
  peakFlowRateGps        = 0.0f;
}

void initPsramAndLoadPours() {
  totalFlashBytes  = ESP.getFlashChipSize();
  hasPsramHardware = psramFound();
  totalPsramBytes  = hasPsramHardware ? ESP.getPsramSize() : 0;

  Serial.println("----------------------------------------------------------------");
  Serial.printf("[MEMORIA] Flash SPI:  %.2f MB (Modo=%u, Speed=%lu MHz)\n",
                totalFlashBytes / (1024.0f * 1024.0f),
                (unsigned)ESP.getFlashChipMode(),
                (unsigned long)(ESP.getFlashChipSpeed() / 1000000UL));
  if (hasPsramHardware && totalPsramBytes > 0) {
    Serial.printf("[MEMORIA] PSRAM:      ACTIVA -> %.2f MB totales (%.2f MB libres)\n",
                  totalPsramBytes / (1024.0f * 1024.0f),
                  ESP.getFreePsram() / (1024.0f * 1024.0f));
  } else {
    Serial.println("[MEMORIA] PSRAM:      No habilitada en FQBN (usando SRAM interna de 320 KB).");
    Serial.println("          Tip: Compila con PSRAM=opi (para N16R8/N8R8) o PSRAM=qspi (para N8R2).");
  }
  Serial.println("----------------------------------------------------------------");

  // Alojar buffers de Pour-Over preferentemente en PSRAM (MALLOC_CAP_SPIRAM)
  size_t liveBytes  = sizeof(LivePourSample) * MAX_LIVE_POUR_SAMPLES;
  size_t savedBytes = sizeof(PourOverRecord) * MAX_SAVED_POURS;

  if (hasPsramHardware) {
    psramLiveSamples = static_cast<LivePourSample*>(ps_malloc(liveBytes));
    psramSavedPours  = static_cast<PourOverRecord*>(ps_malloc(savedBytes));
  }
  if (psramLiveSamples == nullptr) {
    psramLiveSamples = static_cast<LivePourSample*>(malloc(liveBytes));
  }
  if (psramSavedPours == nullptr) {
    psramSavedPours = static_cast<PourOverRecord*>(malloc(savedBytes));
  }

  if (psramSavedPours != nullptr) {
    memset(psramSavedPours, 0, savedBytes);
  }
  resetLivePourRecording();

  // Cargar Pour-Overs previos y dosis desde Flash NVS hacia PSRAM.
  // Formato actual: un registro por clave "rec_<pourId % 10>" (anillo de 10). Al guardar se escribe
  // solo el registro nuevo (antes se reescribían los 10, ~2.7 KB, en cada guardado) y al cargar se
  // descartan registros corruptos o de otro formato (comprobación de magic e id).
  Preferences pourPrefs;
  pourPrefs.begin(NVS_POUR_NAMESPACE, false);
  coffeeDoseGrams    = pourPrefs.getFloat("dose_g", 15.0f);
  if (coffeeDoseGrams < DOSE_MIN_G || coffeeDoseGrams > DOSE_MAX_G) coffeeDoseGrams = 15.0f;
  nextPourSequenceId = pourPrefs.getUShort("next_id", 1);
  bookooBridgeEnabled = pourPrefs.getBool("bk_on", true);
  uiEnglish           = pourPrefs.getBool("lang_en", true);
  savedPoursCount    = 0;

  // (lambda y no función: el generador de prototipos del IDE la declararía antes de PourOverRecord)
  auto isValidPourRecord = [](const PourOverRecord& r) -> bool {
    return r.magic == 0x504F5552UL && r.pourId != 0 && r.durationSec < 36000U &&
           r.poursCount > 0 && !isnan(r.finalWeightGrams);
  };

  if (psramSavedPours != nullptr) {
    PourOverRecord tmp;
    // 1. Registros en formato anillo
    for (uint8_t slot = 0; slot < MAX_SAVED_POURS; slot++) {
      char key[12];
      snprintf(key, sizeof(key), "rec_%u", slot);
      if (pourPrefs.getBytesLength(key) != sizeof(PourOverRecord)) continue;
      pourPrefs.getBytes(key, &tmp, sizeof(PourOverRecord));
      if (isValidPourRecord(tmp) && (tmp.pourId % MAX_SAVED_POURS) == slot) {
        psramSavedPours[savedPoursCount++] = tmp;
      }
    }

    // 2. Migración única desde el formato anterior ("pour_0".."pour_9" por posición + "count")
    uint8_t oldCount = pourPrefs.getUChar("count", 0);
    if (savedPoursCount == 0 && oldCount > 0) {
      if (oldCount > MAX_SAVED_POURS) oldCount = MAX_SAVED_POURS;
      for (uint8_t i = 0; i < oldCount; i++) {
        char oldKey[12];
        snprintf(oldKey, sizeof(oldKey), "pour_%u", i);
        if (pourPrefs.getBytesLength(oldKey) == sizeof(PourOverRecord)) {
          pourPrefs.getBytes(oldKey, &tmp, sizeof(PourOverRecord));
          if (isValidPourRecord(tmp)) {
            char newKey[12];
            snprintf(newKey, sizeof(newKey), "rec_%u", tmp.pourId % MAX_SAVED_POURS);
            pourPrefs.putBytes(newKey, &tmp, sizeof(PourOverRecord));
            psramSavedPours[savedPoursCount++] = tmp;
          }
        }
        pourPrefs.remove(oldKey);
      }
      pourPrefs.remove("count");
      Serial.printf("[POUR-NVS] %u Pour-Overs migrados al formato en anillo.\n", savedPoursCount);
    }

    // Orden cronológico (id ascendente) y siguiente id coherente con lo guardado
    for (uint8_t i = 1; i < savedPoursCount; i++) {
      PourOverRecord key = psramSavedPours[i];
      int8_t j = static_cast<int8_t>(i) - 1;
      while (j >= 0 && psramSavedPours[j].pourId > key.pourId) {
        psramSavedPours[j + 1] = psramSavedPours[j];
        j--;
      }
      psramSavedPours[j + 1] = key;
    }
    if (savedPoursCount > 0) {
      uint16_t maxId = psramSavedPours[savedPoursCount - 1].pourId;
      if (nextPourSequenceId <= maxId) nextPourSequenceId = static_cast<uint16_t>(maxId + 1);
      selectedPourSlot = static_cast<int8_t>(savedPoursCount - 1);
      Serial.printf("[POUR-NVS] %u Pour-Overs restaurados en PSRAM desde Flash NVS.\n", savedPoursCount);
    }
  }
  pourPrefs.end();
}

void saveLanguageToNVS() {
  Preferences p;
  p.begin(NVS_POUR_NAMESPACE, false);
  p.putBool("lang_en", uiEnglish);
  p.end();
}

bool storePourRecord(unsigned long totalElapsedMs, bool persistToNvs);
void recordBrewContext(bool shot, uint16_t id);                       // 46_extraccion.ino

bool finalizeAndSaveCurrentPourOver(unsigned long totalElapsedMs) {
  return storePourRecord(totalElapsedMs, !demoLiveActive);  // la demo en vivo no se escribe en la NVS
}

// Crea el registro del Pour-Over a partir de las muestras en vivo. persistToNvs = false: solo en RAM
// (pour-over de demostración de la orden serie 'd', que no debe ensuciar el historial guardado).
bool storePourRecord(unsigned long totalElapsedMs, bool persistToNvs) {
  if (psramSavedPours == nullptr || psramLiveSamples == nullptr) return false;
  uint16_t durSec = static_cast<uint16_t>(totalElapsedMs / 1000UL);
  float finalW = currentWeightGrams;
  if (liveSampleCount > 0 && psramLiveSamples[liveSampleCount - 1].weightGrams > finalW) {
    finalW = psramLiveSamples[liveSampleCount - 1].weightGrams;
  }
  if (recordWaterOverride > 0.0f) finalW = recordWaterOverride;   // pour-over guiado: el agua marcada

  // Requerir al menos 4 segundos y 5.0 g para considerar una extracción válida
  if (durSec < 4 || finalW < 5.0f || liveSampleCount < 2) {
    return false;
  }

  // Si esta misma grabación ya se guardó (se pausó y se reanudó), actualizar ese registro en lugar
  // de crear uno nuevo: antes cada pausa añadía un Pour-Over duplicado al historial.
  bool overwriteSameRecording = (activeRecordingPourId != 0 && savedPoursCount > 0 &&
                                 psramSavedPours[savedPoursCount - 1].pourId == activeRecordingPourId);

  // Si la tabla de 10 Pour-Overs está llena, desplazar (FIFO) para conservar los 10 más recientes
  uint8_t targetIdx = savedPoursCount;
  if (overwriteSameRecording) {
    targetIdx = static_cast<uint8_t>(savedPoursCount - 1);
  } else if (targetIdx >= MAX_SAVED_POURS) {
    for (uint8_t i = 0; i < MAX_SAVED_POURS - 1; i++) {
      psramSavedPours[i] = psramSavedPours[i + 1];
    }
    targetIdx = MAX_SAVED_POURS - 1;
  } else {
    savedPoursCount++;
  }

  PourOverRecord& rec = psramSavedPours[targetIdx];
  memset(&rec, 0, sizeof(PourOverRecord));
  rec.magic            = 0x504F5552UL;
  rec.pourId           = overwriteSameRecording ? activeRecordingPourId : nextPourSequenceId++;
  activeRecordingPourId = rec.pourId;
  rec.durationSec      = durSec;
  rec.doseGrams        = coffeeDoseGrams;
  rec.finalWeightGrams = finalW;
  rec.finalRatio       = (coffeeDoseGrams > 0.1f) ? (finalW / coffeeDoseGrams) : 0.0f;
  rec.avgFlowGps       = (durSec > 0) ? (finalW / static_cast<float>(durSec)) : 0.0f;
  rec.peakFlowGps      = peakFlowRateGps;
  rec.poursCount       = (currentPourPulsesCount > 0) ? currentPourPulsesCount : 1;

  // Bloom: el primer vertido es pequeño (<= 35 % del total) y le sigue una espera de 15 s o más antes
  // del siguiente. Se busca en las muestras a 2 Hz con los mismos umbrales que el contador de vertidos
  // (empieza con >= 0.8 g/s, termina con <= 0.25 g/s).
  if (recordBloomOverride >= 0) {
    rec.hasBloom = static_cast<uint8_t>(recordBloomOverride);     // pour-over guiado: lo marcó el usuario
  } else if (rec.poursCount >= 2) {
    uint16_t i = 0;
    while (i < liveSampleCount && psramLiveSamples[i].flowGps < 0.8f) i++;    // empieza el 1.er vertido
    while (i < liveSampleCount && psramLiveSamples[i].flowGps > 0.25f) i++;   // termina
    if (i < liveSampleCount) {
      const float firstPourW = psramLiveSamples[i].weightGrams;
      uint16_t j = i;
      while (j < liveSampleCount && psramLiveSamples[j].flowGps < 0.8f) j++;  // empieza el 2.º
      const float pauseSec = (j - i) * 0.5f;
      if (j < liveSampleCount && pauseSec >= 15.0f && firstPourW > 0.0f && firstPourW <= 0.35f * finalW) {
        rec.hasBloom = 1;
      }
    }
  }

  // Normalizar las N muestras de PSRAM a las 120 columnas de la pantalla OLED (altura 0..38 px).
  // El caudal va en escala fija (POUR_GRAPH_MAX_GPS), la misma que el perfil en vivo, para poder comparar.
  float maxW = (finalW > 10.0f) ? finalW : 10.0f;
  float maxF = POUR_GRAPH_MAX_GPS;

  for (uint8_t x = 0; x < POUR_GRAPH_POINTS; x++) {
    uint16_t srcIdx = static_cast<uint16_t>((static_cast<uint32_t>(x) * (liveSampleCount - 1)) / (POUR_GRAPH_POINTS - 1));
    float sw = psramLiveSamples[srcIdx].weightGrams;
    float sf = psramLiveSamples[srcIdx].flowGps;
    if (sw < 0.0f) sw = 0.0f;
    if (sf < 0.0f) sf = 0.0f;

    int hW = static_cast<int>((sw / maxW) * 38.0f);
    int hF = static_cast<int>((sf / maxF) * 38.0f);
    if (hW > 38) hW = 38;
    if (hF > 38) hF = 38;
    rec.weightCurve[x] = static_cast<uint8_t>(hW);
    rec.flowCurve[x]   = static_cast<uint8_t>(hF);
  }

  // Persistir en NVS Flash para que no se pierda al apagar la batería
  // Solo se escribe este registro, en su hueco del anillo (pisa al de hace 10 Pour-Overs)
  if (persistToNvs) {
    Preferences pourPrefs;
    pourPrefs.begin(NVS_POUR_NAMESPACE, false);
    pourPrefs.putUShort("next_id", nextPourSequenceId);
    char key[12];
    snprintf(key, sizeof(key), "rec_%u", rec.pourId % MAX_SAVED_POURS);
    pourPrefs.putBytes(key, &rec, sizeof(PourOverRecord));
    pourPrefs.end();
    recordBrewContext(false, rec.pourId);                                 // molienda y bolsa
  }

  selectedPourSlot       = static_cast<int8_t>(targetIdx);
  lastSavedPourId        = rec.pourId;
  bannerIsShot           = false;
  pourSavedBannerUntilMs = millis() + 2200UL;

  Serial.printf("[POUR-SAVE] Pour-Over #%u guardado en %s: Peso=%.1fg | Dosis=%.1fg | Ratio=1:%.1f | Tiempo=%us | Vertidos=%s%u\n",
                rec.pourId, persistToNvs ? "PSRAM+NVS" : "RAM (demo)", rec.finalWeightGrams, rec.doseGrams,
                rec.finalRatio, rec.durationSec, rec.hasBloom ? "bloom+" : "",
                rec.hasBloom ? rec.poursCount - 1 : rec.poursCount);
  return true;
}

// ----------------------------------------------------------------------------
// POUR-OVER DE DEMOSTRACIÓN (orden serie 'd'), para capturas de pantalla.
// 15 g de café y 250 g de agua (1:16.7): bloom de 45 g con 30 s de espera, y 3 vertidos hasta 120, 190
// y 250 g, con bajada final hasta los 2:45. El peso se genera a 10 Hz con la resolución de la Dot (0.1 g)
// y pasa por el motor de caudal REAL (src/flow_engine), con el mismo muestreo a 2 Hz y la misma
// detección de vertidos que una preparación de verdad. Solo queda en RAM (no se escribe en la NVS).
// ----------------------------------------------------------------------------
// Receta de demostración de V60: peso (g) del agua vertida en el segundo t desde el primer vertido.
// Tramos: hasta el segundo t1 el peso va en línea recta hasta w1 (vertidos) o se mantiene (esperas).
static constexpr float DEMO_POUR_TOTAL_SEC = 165.0f;   // 2:45
float demoPourWeightAt(float t) {
  struct Seg { float t1; float w1; };
  static const Seg segs[] = {
    {10.0f, 45.0f},   // bloom: vertido de 45 g en 10 s
    {30.0f, 45.0f},   // espera del bloom hasta los 30 s
    {55.0f, 120.0f},  // vertido 1
    {65.0f, 120.0f},
    {90.0f, 190.0f},  // vertido 2
    {100.0f, 190.0f},
    {120.0f, 250.0f}, // vertido 3
    {165.0f, 250.0f}, // bajada (drawdown) hasta 2:45
  };
  if (t <= 0.0f) return 0.0f;
  float prevT = 0.0f, prevW = 0.0f;
  for (const Seg& s : segs) {
    if (t <= s.t1) {
      float w = prevW + (s.w1 - prevW) * ((t - prevT) / (s.t1 - prevT));
      return roundf(w * 10.0f) / 10.0f;  // resolución de la Dot (0.1 g)
    }
    prevT = s.t1; prevW = s.w1;
  }
  return prevW;
}

void injectDemoPourOver() {
  if (psramLiveSamples == nullptr) return;
  const float totalSec = DEMO_POUR_TOTAL_SEC;

  FlowEngine fe;
  fe.reset(0.0f);
  const float savedDose   = coffeeDoseGrams;
  const uint16_t savedNext = nextPourSequenceId;
  coffeeDoseGrams = 15.0f;
  resetLivePourRecording();

  const unsigned long base = 1000UL;
  float peak = 0.0f;
  pourseg::Segmenter seg;
  seg.reset();
  for (unsigned long ms = 0; ms <= (unsigned long)(totalSec * 1000.0f); ms += 100UL) {
    float w = demoPourWeightAt(ms / 1000.0f);
    fe.addSample(base + ms, w, true);
    float flow = fe.currentGps;
    if (flow > peak) peak = flow;
    seg.add(static_cast<uint32_t>(base + ms), w, flow);
    if ((ms % 500UL) == 0 && liveSampleCount < MAX_LIVE_POUR_SAMPLES) {
      psramLiveSamples[liveSampleCount].weightGrams = w;
      psramLiveSamples[liveSampleCount].flowGps     = (flow > 0.0f) ? flow : 0.0f;
      liveSampleCount++;
    }
  }
  peakFlowRateGps        = peak;
  currentPourPulsesCount = seg.pours();
  storePourRecord((unsigned long)(totalSec * 1000.0f), false);

  coffeeDoseGrams    = savedDose;
  nextPourSequenceId = savedNext;   // el siguiente pour-over real conserva su número
  resetLivePourRecording();
  uiNeedsImmediateRedraw = true;
}

// ----------------------------------------------------------------------------
// DEMOSTRACIÓN EN VIVO DE UN V60 GUIADO (orden serie 'v'), para grabar la pantalla y probar sin agua.
// Hace lo mismo que una persona con el pour-over guiado (41_guia.ino): pone el server (300 g) y gira,
// pone el V60 con el café (140 g) y gira, gira a Bloom, vierte la receta de demoPourWeightAt() en tiempo
// real con un toque de la tetera a los 20 s, gira al acabar el bloom, al acabar de verter y al acabar el
// drawdown, quita el V60 (queda el server con 215 g de bebida) y gira al resumen. El peso simulado entra en
// la cola de muestras a 10 Hz, como el de la Dot, y recorre el mismo camino. Mientras dura se ignora el peso
// de la Dot; el pour-over se guarda solo en RAM y su traza se marca como demo. Al terminar se restauran la
// receta y el número del siguiente pour-over.
// ----------------------------------------------------------------------------
void registerUserOrScaleActivity();   // 60_power

static constexpr float DEMO_LEAD_SEC  = 9.0f;     // antes del primer vertido: server, V60, bloom armado
static constexpr float DEMO_END_SEC   = 173.0f;   // fin de la demostración (en el resumen)
static const float demoTurnAt[]       = {-6.5f, -3.5f, -1.5f, 30.0f, 120.0f, 160.0f, 168.0f};
static uint8_t demoTurnIdx            = 0;
static unsigned long demoStartMs      = 0;
static unsigned long demoLastSampleMs = 0;
static float demoBaseGross            = 0.0f;
static float demoSavedDose            = 0.0f;
static float demoSavedRatio           = 0.0f;
static float demoSavedBloom           = 0.0f;
static uint16_t demoSavedNextId       = 1;
static bool demoV60Running            = false;

// Peso bruto de la Dot en el segundo t desde el primer vertido
static float demoGrossAt(float t) {
  float g = demoBaseGross;
  if (t >= -8.0f) g += 300.0f;                       // server
  if (t >= 163.0f) return g + 215.0f;                // V60 fuera: el server con la bebida
  if (t >= -5.0f) g += 140.0f;                       // V60 con el café
  g += demoPourWeightAt(t);
  if (t >= 20.0f && t < 20.3f) g += 12.0f;           // toque de la tetera durante la espera del bloom
  return roundf(g * 10.0f) / 10.0f;
}

void startDemoLivePour() {
  if (demoLiveActive) return;
  if (brewMode != BrewMode::PourOver) setBrewMode(BrewMode::PourOver);
  demoSavedDose      = coffeeDoseGrams;
  demoSavedRatio     = pourRatio;
  demoSavedBloom     = bloomFactor;
  demoSavedNextId    = nextPourSequenceId;
  coffeeDoseGrams    = 15.0f;
  pourRatio          = 16.7f;   // 250 g
  bloomFactor        = 3.0f;    // 45 g
  demoBaseGross      = rawWeightGrams;   // lo que haya ahora sobre la báscula
  uiState            = UiState::Main;
  currentScreenView  = 0;
  pourSavedBannerUntilMs = 0;
  if (weightQueue != nullptr) xQueueReset(weightQueue);
  guideReset();
  demoLiveActive     = true;
  demoV60Running     = true;
  demoTurnIdx        = 0;
  demoStartMs        = millis();
  demoLastSampleMs   = 0;
  registerUserOrScaleActivity();
  Serial.println("[DEMO] V60 guiado: server, V60, bloom de 45 g (30 s), 3 vertidos hasta 250 g, yield 215 g.");
}

void serviceDemoLivePour() {
  if (!demoV60Running) return;
  unsigned long now = millis();
  if (demoLastSampleMs != 0 && (now - demoLastSampleMs) < 100UL) return;
  demoLastSampleMs = now;

  const float t = (now - demoStartMs) / 1000.0f - DEMO_LEAD_SEC;   // segundos desde el primer vertido
  WeightSample s;
  s.ms     = now;
  s.grams  = demoGrossAt(t);
  s.stable = true;
  if (weightQueue != nullptr) xQueueSend(weightQueue, &s, 0);
  registerUserOrScaleActivity();

  if (demoTurnIdx < sizeof(demoTurnAt) / sizeof(demoTurnAt[0]) && t >= demoTurnAt[demoTurnIdx]) {
    demoTurnIdx++;
    guideRotate(1);   // "girar el encoder" al paso siguiente
  }
  if (t >= DEMO_END_SEC) {
    demoV60Running     = false;
    demoLiveActive     = false;
    coffeeDoseGrams    = demoSavedDose;
    pourRatio          = demoSavedRatio;
    bloomFactor        = demoSavedBloom;
    nextPourSequenceId = demoSavedNextId;
    Serial.println("[DEMO] Fin de la demostracion (girar para salir del resumen).");
  }
}
