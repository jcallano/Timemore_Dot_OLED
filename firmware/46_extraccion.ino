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
//   - Estimada: agua - retención (g de agua por g de café, aprendida; 2.0 al principio). EY~ en pantalla.
// Espresso: la taza está en la báscula; la bebida es el peso final del shot.
// ============================================================================

// BrewExtra: en firmware.ino (los tipos de las firmas deben ir antes de los prototipos del IDE)
static BrewExtra pourExtras[MAX_SAVED_POURS];
static BrewExtra shotExtras[MAX_SAVED_SHOTS];
float serverJarGrams       = 0.0f;    // peso de la jarra vacía (0 = sin registrar)
float pourRetentionRatio   = 2.0f;    // g de agua retenida por g de café (aprendida)

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
  if (p.getBytesLength("px") == sizeof(pourExtras)) p.getBytes("px", pourExtras, sizeof(pourExtras));
  serverJarGrams     = p.getFloat("jar", 0.0f);
  pourRetentionRatio = p.getFloat("ret", 2.0f);
  p.end();
  Preferences s;
  s.begin(NVS_SHOT_NAMESPACE, true);
  if (s.getBytesLength("sx") == sizeof(shotExtras)) s.getBytes("sx", shotExtras, sizeof(shotExtras));
  s.end();
  if (isnan(serverJarGrams) || serverJarGrams < 0.0f || serverJarGrams > 1500.0f) serverJarGrams = 0.0f;
  if (isnan(pourRetentionRatio) || pourRetentionRatio < 1.0f || pourRetentionRatio > 3.5f) pourRetentionRatio = 2.0f;
  Serial.printf("[EXTRACCION] Jarra %.1f g | retencion %.2f g/g\n", serverJarGrams, pourRetentionRatio);
}

static void saveExtras(bool shot) {
  Preferences p;
  if (shot) {
    p.begin(NVS_SHOT_NAMESPACE, false);
    p.putBytes("sx", shotExtras, sizeof(shotExtras));
  } else {
    p.begin(NVS_POUR_NAMESPACE, false);
    p.putBytes("px", pourExtras, sizeof(pourExtras));
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
  t[pick].id = id;
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
