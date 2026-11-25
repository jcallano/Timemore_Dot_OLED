// ============================================================================
// EXTRACCIÓN: TDS (DiFluid R2), BEBIDA Y EY DE CADA POUR-OVER Y SHOT
// Pestaña del sketch: el IDE la concatena tras firmware.ino (orden alfabético).
//
// Los registros de pour-over (272 B) y de shot (148 B) no tienen sitio libre, así que estos datos van en
// una tabla aparte por historial, unida por el id del registro (un blob por tabla en la NVS).
//
// Bebida del pour-over: la Dot pesa el agua vertida; parte se queda en el café.
//   - Medida: con la jarra registrada (Menú -> Jarra, pesada vacía), al quitar el dripper tras guardar el
//     pour-over queda en la báscula la jarra con la bebida. Como la tara es por software, el peso absoluto
//     de la Dot (rawWeightGrams) menos la jarra es la bebida. Cada medida afina la retención aprendida.
//   - Estimada: agua - retención (g de agua por g de café, aprendida; 2.0 al principio). EY* en pantalla.
// Espresso: la taza está en la báscula; la bebida es el peso final del shot.
// ============================================================================

// BrewExtra: en firmware.ino (los tipos de las firmas deben ir antes de los prototipos del IDE)
static BrewExtra pourExtras[MAX_SAVED_POURS];
static BrewExtra shotExtras[MAX_SAVED_SHOTS];
float serverJarGrams       = 0.0f;    // peso de la jarra vacía (0 = sin registrar)
float pourRetentionRatio   = 2.0f;    // g de agua retenida por g de café (aprendida)

// Molino y bolsa: cada registro guarda la molienda y la bolsa con las que se hizo
float grindPour            = 1.5f;    // dial del molino para filtrado
float grindEsp             = 1.5f;    // dial del molino para espresso
bool grindUpCoarser        = true;    // subir el número = más grueso
uint16_t currentBagId      = 1;
static constexpr float GRIND_MIN = 0.0f, GRIND_MAX = 60.0f;
// Punto dulce personal (centro de la zona del gráfico), por modo. Empieza en la zona SCA.
float sweetEyPour = 20.0f, sweetTdsPour = 1.30f, sweetEyEsp = 20.0f, sweetTdsEsp = 10.0f;
static constexpr float HALF_EY = 2.0f, HALF_TDS_POUR = 0.15f, HALF_TDS_ESP = 1.5f;

// Bebida pendiente de medir tras guardar un pour-over
static uint16_t bevPendingPourId   = 0;
static float bevPendingWater       = 0.0f;
static float bevPendingDose        = 0.0f;
static unsigned long bevPendingSinceMs = 0, bevStableSinceMs = 0;
static float bevLastCandidate      = 0.0f;

void loadBrewExtras() {
  memset(pourExtras, 0, sizeof(pourExtras));
  memset(shotExtras, 0, sizeof(shotExtras));
  Preferences p;
  p.begin(NVS_POUR_NAMESPACE, true);
  if (p.getBytesLength("px2") == sizeof(pourExtras)) p.getBytes("px2", pourExtras, sizeof(pourExtras));
  grindPour      = p.getFloat("grind_f", 1.5f);
  grindEsp       = p.getFloat("grind_e", 1.5f);
  grindUpCoarser = p.getBool("grind_up", true);
  currentBagId   = p.getUShort("bag", 1);
  sweetEyPour    = p.getFloat("sw_ey_f", 20.0f);
  sweetTdsPour   = p.getFloat("sw_tds_f", 1.30f);
  sweetEyEsp     = p.getFloat("sw_ey_e", 20.0f);
  sweetTdsEsp    = p.getFloat("sw_tds_e", 10.0f);
  serverJarGrams     = p.getFloat("jar", 0.0f);
  pourRetentionRatio = p.getFloat("ret", 2.0f);
  p.end();
  Preferences s;
  s.begin(NVS_SHOT_NAMESPACE, true);
  if (s.getBytesLength("sx2") == sizeof(shotExtras)) s.getBytes("sx2", shotExtras, sizeof(shotExtras));
  s.end();
  if (isnan(serverJarGrams) || serverJarGrams < 0.0f || serverJarGrams > 1500.0f) serverJarGrams = 0.0f;
  if (isnan(pourRetentionRatio) || pourRetentionRatio < 1.0f || pourRetentionRatio > 3.5f) pourRetentionRatio = 2.0f;
  Serial.printf("[EXTRACCION] Jarra %.1f g | retencion %.2f g/g\n", serverJarGrams, pourRetentionRatio);
}

static void saveExtras(bool shot) {
  Preferences p;
  if (shot) {
    p.begin(NVS_SHOT_NAMESPACE, false);
    p.putBytes("sx2", shotExtras, sizeof(shotExtras));
  } else {
    p.begin(NVS_POUR_NAMESPACE, false);
    p.putBytes("px2", pourExtras, sizeof(pourExtras));
    p.putFloat("ret", pourRetentionRatio);
  }
  p.end();
}

// Entrada de un registro; con create, reutiliza la libre o la del id más antiguo
static BrewExtra* findExtra(bool shot, uint16_t id, bool create) {
  BrewExtra* t = shot ? shotExtras : pourExtras;
  const uint8_t n = shot ? MAX_SAVED_SHOTS : MAX_SAVED_POURS;
  for (uint8_t i = 0; i < n; i++) if (t[i].id == id && id != 0) return &t[i];
  if (!create || id == 0) return nullptr;
  uint8_t pick = 0;
  for (uint8_t i = 0; i < n; i++) {
    if (t[i].id == 0) { pick = i; break; }
    if (t[i].id < t[pick].id) pick = i;
  }
  memset(&t[pick], 0, sizeof(BrewExtra));
  t[pick].id       = id;
  t[pick].grindX10 = 0xFFFF;
  t[pick].taste    = TASTE_NONE;
  return &t[pick];
}

void saveServerJar(float grams) {
  serverJarGrams = grams;
  Preferences p;
  p.begin(NVS_POUR_NAMESPACE, false);
  p.putFloat("jar", serverJarGrams);
  p.end();
  Serial.printf("[EXTRACCION] Jarra: %.1f g\n", serverJarGrams);
}

// Resultado de la medida del R2 (58_difluid_r2.ino)
void storeTdsResult(bool isShot, uint16_t id, uint16_t packed) {
  BrewExtra* e = findExtra(isShot, id, true);
  if (e == nullptr) return;
  e->tdsPacked = packed;
  saveExtras(isShot);
}

// ---------------------------------------------------------------------------
// Consultas para la pantalla
// ---------------------------------------------------------------------------
const PourOverRecord* findPourById(uint16_t id) {
  if (psramSavedPours == nullptr) return nullptr;
  for (uint8_t i = 0; i < savedPoursCount; i++) if (psramSavedPours[i].pourId == id) return &psramSavedPours[i];
  return nullptr;
}

int8_t shotIndexById(uint16_t id) {
  for (uint8_t i = 0; i < savedShotsCount; i++) if (savedShots[i].shotId == id) return static_cast<int8_t>(i);
  return -1;
}

bool recordTds(bool shot, uint16_t id, float* tdsOut, uint8_t* flagsOut) {
  const BrewExtra* e = findExtra(shot, id, false);
  if (e == nullptr || e->tdsPacked == 0) return false;
  if (tdsOut) *tdsOut = tds::unpackTds(e->tdsPacked);
  if (flagsOut) *flagsOut = tds::unpackFlags(e->tdsPacked);
  return true;
}

// Bebida del pour-over: medida (true) o estimada (false)
float pourBeverageGrams(const PourOverRecord& r, bool* measured) {
  const BrewExtra* e = findExtra(false, r.pourId, false);
  if (e != nullptr && e->beverageX10 > 0) {
    if (measured) *measured = true;
    return e->beverageX10 / 10.0f;
  }
  if (measured) *measured = false;
  const float b = r.finalWeightGrams - pourRetentionRatio * r.doseGrams;
  return b > 0.0f ? b : 0.0f;
}

// EY del registro (0 si no hay TDS). estimated = la bebida del pour-over es estimada.
float recordEy(bool shot, uint16_t id, bool* estimated) {
  float v;
  if (estimated) *estimated = false;
  if (!recordTds(shot, id, &v, nullptr)) return 0.0f;
  if (shot) {
    const int8_t k = shotIndexById(id);
    return k >= 0 ? tds::eyEspresso(v, savedShots[k].yieldGrams, savedShots[k].doseGrams) : 0.0f;
  }
  const PourOverRecord* r = findPourById(id);
  if (r == nullptr || r->doseGrams <= 0.1f) return 0.0f;
  bool measured = false;
  const float bev = pourBeverageGrams(*r, &measured);
  if (estimated) *estimated = !measured;
  return v * bev / r->doseGrams;
}

// ---------------------------------------------------------------------------
// Medida de la bebida al quitar el dripper
// ---------------------------------------------------------------------------
// Lo llama storePourRecord() (30_storage.ino) al guardar un pour-over real
void armBeverageCapture(uint16_t pourId, float waterG, float doseG) {
  if (serverJarGrams <= 0.0f || waterG < 50.0f) return;
  bevPendingPourId   = pourId;
  bevPendingWater    = waterG;
  bevPendingDose     = doseG;
  bevPendingSinceMs  = millis();
  bevStableSinceMs   = 0;
}

void serviceBeverageCapture() {
  if (bevPendingPourId == 0) return;
  const unsigned long now = millis();
  if ((now - bevPendingSinceMs) > 180000UL) {   // 3 min sin quitar el dripper: se olvida
    bevPendingPourId = 0;
    return;
  }
  // Dripper fuera: el peso neto cae claramente por debajo del agua vertida
  const float bev = rawWeightGrams - serverJarGrams;
  const bool plausible = (currentWeightGrams < 0.9f * bevPendingWater) &&
                         bev > 0.5f * bevPendingWater && bev <= bevPendingWater + 2.0f;
  if (!plausible || !weightIsStable || fabsf(bev - bevLastCandidate) > 0.3f) {
    bevLastCandidate = bev;
    bevStableSinceMs = 0;
    return;
  }
  if (bevStableSinceMs == 0) { bevStableSinceMs = now; return; }
  if ((now - bevStableSinceMs) < 1500UL) return;

  BrewExtra* e = findExtra(false, bevPendingPourId, true);
  if (e != nullptr) {
    e->beverageX10 = static_cast<uint16_t>(lroundf(bev * 10.0f));
    // Retención de este pour-over: afina la estimación de los siguientes
    if (bevPendingDose > 1.0f) {
      const float r = (bevPendingWater - bev) / bevPendingDose;
      if (r >= 1.0f && r <= 3.5f) pourRetentionRatio = 0.7f * pourRetentionRatio + 0.3f * r;
    }
    saveExtras(false);
    Serial.printf("[EXTRACCION] Pour-over #%u: bebida %.1f g (agua %.1f g) | retencion %.2f g/g\n",
                  bevPendingPourId, bev, bevPendingWater, pourRetentionRatio);
    uiNeedsImmediateRedraw = true;
  }
  bevPendingPourId = 0;
}

// ---------------------------------------------------------------------------
// Molino, bolsa, sabor y punto dulce
// ---------------------------------------------------------------------------
float& grindForMode() { return (brewMode == BrewMode::Espresso) ? grindEsp : grindPour; }

void saveGrindSettings() {
  Preferences p;
  p.begin(NVS_POUR_NAMESPACE, false);
  p.putFloat("grind_f", grindPour);
  p.putFloat("grind_e", grindEsp);
  p.putBool("grind_up", grindUpCoarser);
  p.putUShort("bag", currentBagId);
  p.end();
}

static void saveSweetSpots() {
  Preferences p;
  p.begin(NVS_POUR_NAMESPACE, false);
  p.putFloat("sw_ey_f", sweetEyPour);
  p.putFloat("sw_tds_f", sweetTdsPour);
  p.putFloat("sw_ey_e", sweetEyEsp);
  p.putFloat("sw_tds_e", sweetTdsEsp);
  p.end();
}

// Menú -> Bolsa nueva: la molienda que funcionaba con la anterior ya no sirve de referencia
void startNewBag() {
  currentBagId++;
  saveGrindSettings();
  Serial.printf("[EXTRACCION] Bolsa nueva: #%u\n", currentBagId);
}

// Al guardar un pour-over o un shot real: molienda y bolsa del momento
void recordBrewContext(bool shot, uint16_t id) {
  BrewExtra* e = findExtra(shot, id, true);
  if (e == nullptr) return;
  // Café nuevo: nada de un registro anterior con el mismo número (el anillo reutiliza los ids)
  memset(e, 0, sizeof(BrewExtra));
  e->id    = id;
  e->taste = TASTE_NONE;
  const float g = shot ? grindEsp : grindPour;
  e->grindX10 = static_cast<uint16_t>(lroundf(g * 10.0f));
  e->bagId    = currentBagId;
  saveExtras(shot);
}

bool recordTaste(bool shot, uint16_t id, int8_t* acidBitter, int8_t* strength) {
  const BrewExtra* e = findExtra(shot, id, false);
  if (e == nullptr || e->taste == TASTE_NONE) return false;
  if (acidBitter) *acidBitter = static_cast<int8_t>(e->taste & 0x0F) - 2;
  if (strength)   *strength   = static_cast<int8_t>(e->taste >> 4) - 2;
  return true;
}

// Molienda del registro (<0 = desconocida)
float recordGrind(bool shot, uint16_t id) {
  const BrewExtra* e = findExtra(shot, id, false);
  return (e == nullptr || e->grindX10 == 0xFFFF) ? -1.0f : e->grindX10 / 10.0f;
}

// Molienda del último café de la bolsa actual que gustó (equilibrado en los dos ejes)
float bestGrindThisBag(bool shot) {
  const BrewExtra* t = shot ? shotExtras : pourExtras;
  const uint8_t n = shot ? MAX_SAVED_SHOTS : MAX_SAVED_POURS;
  uint16_t bestId = 0;
  float g = -1.0f;
  for (uint8_t i = 0; i < n; i++) {
    if (t[i].id != 0 && t[i].bagId == currentBagId && t[i].taste == 0x22 && t[i].grindX10 != 0xFFFF && t[i].id > bestId) {
      bestId = t[i].id;
      g = t[i].grindX10 / 10.0f;
    }
  }
  return g;
}

void sweetSpot(bool shot, float* ey, float* tdsV, float* halfEy, float* halfTds) {
  if (ey)      *ey      = shot ? sweetEyEsp : sweetEyPour;
  if (tdsV)    *tdsV    = shot ? sweetTdsEsp : sweetTdsPour;
  if (halfEy)  *halfEy  = HALF_EY;
  if (halfTds) *halfTds = shot ? HALF_TDS_ESP : HALF_TDS_POUR;
}

// Sugerencia para un registro con TDS y sabor
bool buildAdvice(bool shot, uint16_t id, advisor::Advice& out) {
  float v;
  int8_t ab, st;
  if (!recordTds(shot, id, &v, nullptr) || !recordTaste(shot, id, &ab, &st)) return false;
  advisor::Input in;
  in.espresso       = shot;
  in.tds            = v;
  in.ey             = recordEy(shot, id, nullptr);
  sweetSpot(shot, &in.targetEy, &in.targetTds, &in.halfEy, &in.halfTds);
  in.acidBitter     = ab;
  in.strength       = st;
  in.grind          = recordGrind(shot, id);
  in.grindUpCoarser = grindUpCoarser;
  in.english        = uiEnglish;
  in.bestGrindThisBag = bestGrindThisBag(shot);
  if (shot) {
    const int8_t k = shotIndexById(id);
    if (k >= 0 && savedShots[k].doseGrams > 0.1f) in.ratio = savedShots[k].yieldGrams / savedShots[k].doseGrams;
  } else {
    const PourOverRecord* r = findPourById(id);
    if (r != nullptr) {
      in.ratio    = r->finalRatio;
      in.hasBloom = r->hasBloom != 0;
    }
  }
  out = advisor::advise(in);
  return true;
}

static void tasteModelLearn(bool shot, uint16_t id);

// Respuesta a "¿cómo sabe?" (-2..2 en cada eje). Si gusta, mueve el punto dulce hacia ese café.
void storeTaste(bool shot, uint16_t id, int8_t acidBitter, int8_t strength) {
  BrewExtra* e = findExtra(shot, id, true);
  if (e == nullptr) return;
  e->taste = static_cast<uint8_t>(((strength + 2) << 4) | (acidBitter + 2));
  saveExtras(shot);
  float v;
  if (acidBitter == 0 && strength == 0 && recordTds(shot, id, &v, nullptr)) {
    const float ey = recordEy(shot, id, nullptr);
    if (ey > 0.0f) {
      if (shot) advisor::learnSweetSpot(sweetEyEsp, sweetTdsEsp, ey, v);
      else      advisor::learnSweetSpot(sweetEyPour, sweetTdsPour, ey, v);
      saveSweetSpots();
    }
  }
  tasteModelLearn(shot, id);   // la red neuronal aprende de esta valoración
  Serial.printf("[SABOR] %s #%u: agrio/amargo %+d, aguado/intenso %+d\n", shot ? "Shot" : "Pour", id, acidBitter, strength);
}

// ---------------------------------------------------------------------------
// Modelo de gusto (red neuronal, src/taste_model): aprende con cada valoración
// ---------------------------------------------------------------------------
taste::Model tasteModel;
static const char* NVS_NN_NAMESPACE = "tmdot_nn";

static void saveTasteModel() {
  Preferences p;
  p.begin(NVS_NN_NAMESPACE, false);
  p.putBytes("w", &tasteModel.weights(), sizeof(taste::Weights));
  p.end();
}

void loadTasteModel() {
  Preferences p;
  p.begin(NVS_NN_NAMESPACE, true);
  bool ok = false;
  if (p.getBytesLength("w") == sizeof(taste::Weights)) {
    p.getBytes("w", &tasteModel.weights(), sizeof(taste::Weights));
    ok = tasteModel.valid();
  }
  p.end();
  if (!ok) {
    const unsigned long t0 = millis();
    tasteModel.init();   // preentrenamiento con la regla (una sola vez)
    saveTasteModel();
    Serial.printf("[GUSTO] Red neuronal nueva, preentrenada en %lu ms\n", millis() - t0);
  }
  Serial.printf("[GUSTO] Red neuronal 6-8-2%s: %u valoraciones aprendidas\n",
                taste::usesDsp() ? " (ESP-DSP)" : "", tasteModel.weights().samplesSeen);
}

// Orden serie '#': la red vuelve a empezar (olvida lo aprendido)
void resetTasteModel() {
  tasteModel.init();
  saveTasteModel();
  Serial.println("[GUSTO] Red neuronal reiniciada.");
}

// Muestra de entrenamiento de un registro con TDS y sabor
static bool recordSample(bool shot, uint16_t id, taste::Sample& s) {
  float v;
  int8_t ab, st;
  if (!recordTds(shot, id, &v, nullptr) || !recordTaste(shot, id, &ab, &st)) return false;
  const float ey = recordEy(shot, id, nullptr);
  if (ey <= 0.0f) return false;
  s.espresso   = shot;
  s.ey         = ey;
  s.tds        = v;
  s.acidBitter = ab;
  s.strength   = st;
  if (shot) {
    const int8_t k = shotIndexById(id);
    if (k < 0 || savedShots[k].doseGrams <= 0.1f) return false;
    s.ratio = savedShots[k].yieldGrams / savedShots[k].doseGrams;
    s.bloom = false;
  } else {
    const PourOverRecord* r = findPourById(id);
    if (r == nullptr) return false;
    s.ratio = r->finalRatio;
    s.bloom = r->hasBloom != 0;
  }
  return true;
}

// Entrena la red con la valoración nueva y repasa las demás guardadas
static void tasteModelLearn(bool shot, uint16_t id) {
  taste::Sample s;
  if (!recordSample(shot, id, s)) return;
  static taste::Sample hist[MAX_SAVED_POURS + MAX_SAVED_SHOTS];
  int nh = 0;
  for (uint8_t i = 0; i < MAX_SAVED_POURS; i++)
    if (pourExtras[i].id != 0 && !(!shot && pourExtras[i].id == id) && recordSample(false, pourExtras[i].id, hist[nh])) nh++;
  for (uint8_t i = 0; i < MAX_SAVED_SHOTS; i++)
    if (shotExtras[i].id != 0 && !(shot && shotExtras[i].id == id) && recordSample(true, shotExtras[i].id, hist[nh])) nh++;
  float pab, pst;
  tasteModel.predict(s, &pab, &pst);
  const unsigned long t0 = millis();
  tasteModel.learn(s, hist, nh);
  saveTasteModel();
  Serial.printf("[GUSTO] Prevision %+.1f/%+.1f, valoracion %+d/%+d | aprendido en %lu ms con %d cafes mas\n",
                pab, pst, s.acidBitter, s.strength, millis() - t0, nh);
}

// Descripción del gusto aprendido (hasta 5 líneas de 21 caracteres)
uint8_t describeTaste(bool shot, char lines[][22]) {
  const taste::Weights& w = tasteModel.weights();
  const float ratio = shot ? espRatio : 16.0f;
  // Pista: la media de los cafés valorados de este modo (o la zona SCA si no hay)
  float sEy = 0.0f, sT = 0.0f;
  int ns = 0;
  const BrewExtra* tb = shot ? shotExtras : pourExtras;
  const uint8_t nt = shot ? MAX_SAVED_SHOTS : MAX_SAVED_POURS;
  for (uint8_t i = 0; i < nt; i++) {
    taste::Sample smp;
    if (tb[i].id != 0 && recordSample(shot, tb[i].id, smp)) { sEy += smp.ey; sT += smp.tds; ns++; }
  }
  const float hEy = ns ? sEy / ns : 20.0f, hT = ns ? sT / ns : (shot ? 10.0f : 1.30f);
  float ey, tv;
  tasteModel.sweetSpot(shot, ratio, hEy, hT, &ey, &tv);
  uint8_t n = 0;
  snprintf(lines[n++], 22, shot ? "EY %.1f%%  TDS %.1f%%" : "EY %.1f%%  TDS %.2f%%", ey, tv);
  if (w.samplesSeen == 0) {
    snprintf(lines[n++], 22, TR("AUN NO TE CONOZCO:", "NOT ENOUGH DATA YET:"));
    snprintf(lines[n++], 22, TR("PARTO DE LA ZONA SCA", "STARTING AT SCA ZONE"));
    snprintf(lines[n++], 22, TR("VALORA TUS CAFES", "RATE YOUR COFFEES"));
    return n;
  }
  snprintf(lines[n++], 22, TR("EXTRACCION %s", "EXTRACTION %s"), ey < 19.0f ? TR("BAJA", "LOW") : (ey > 21.0f ? TR("ALTA", "HIGH") : TR("MEDIA", "MEDIUM")));
  const float rel = shot ? (tv - 10.0f) / 1.5f : (tv - 1.30f) / 0.15f;
  snprintf(lines[n++], 22, TR("CUERPO %s", "BODY %s"), rel < -0.5f ? TR("LIGERO", "LIGHT") : (rel > 0.5f ? TR("INTENSO", "STRONG") : TR("MEDIO", "MEDIUM")));
  const float slope = tasteModel.bitterPerEy(shot, ey, tv, ratio);
  snprintf(lines[n++], 22, "%s", slope > 0.7f ? TR("SENSIBLE AL AMARGOR", "BITTERNESS-SENSITIVE") : (slope < 0.3f ? TR("TOLERAS EL AMARGOR", "BITTERNESS-TOLERANT") : TR("AMARGOR: NORMAL", "BITTERNESS: NORMAL")));
  return n;
}
