// ============================================================================
// MODO FERMENTO: CALCULADORA DE SALMUERA Y LOTES DE FERMENTACIÓN LÁCTICA
// Pestaña del sketch: el IDE la concatena tras firmware.ino (orden alfabético).
// Tipos, constantes y variables globales compartidas están en la pestaña principal.
//
// Calculadora (vista principal, fermentStep):
//   Base -> pesa el tarro con las verduras y el agua (tras una tara con 2 clics). La pantalla ya dice
//           cuánta sal hace falta: base x % de sal. 1 clic fija la base y tara.
//   Sal  -> echa la sal: barra de progreso hasta el objetivo, FALTA / LISTO / SOBRA. 2 clics taran sin
//           salir (sirve para pesar la sal en un cuenco). 1 clic guarda el lote; sin sal, vuelve a Base.
// Lotes (Menú -> Lotes): cada lote guarda la receta y hasta 16 lecturas de la sonda de pH y
// temperatura (57_accesorios.ino). En el visor, 1 clic añade la lectura actual de la sonda al lote.
// La hora es opcional: el display no tiene reloj con pila. Si alguien la puso (orden serie 'z'), las
// lecturas guardan la fecha y se muestran como "D+n" (días desde el inicio); si no, como "#n".
// ============================================================================

// Definidas en pestañas posteriores
bool probeGetFresh(accproto::Reading& r, uint32_t* ageMs);

// Tipos de los lotes (FermentLot, FermentReading) y sus constantes: en firmware.ino

enum class FermentStep : uint8_t { Base, Salt };
FermentStep fermentStep        = FermentStep::Base;
float fermentSaltPct           = 2.5f;
float fermentBaseGrams         = 0.0f;
float fermentSaltTargetGrams   = 0.0f;

FermentLot fermentLots[MAX_FERMENT_LOTS];
uint8_t fermentLotsCount       = 0;
uint16_t nextLotId             = 1;
int8_t selectedLotSlot         = 0;
uint16_t lastSavedLotId        = 0;

// Aviso corto en la parte baja de la pantalla ("SIN SONDA", "LECTURA GUARDADA"...)
char fermentNotice[22]         = "";
unsigned long fermentNoticeUntilMs = 0;

static void fermentShowNotice(const char* msg) {
  strncpy(fermentNotice, msg, sizeof(fermentNotice) - 1);
  fermentNotice[sizeof(fermentNotice) - 1] = '\0';
  fermentNoticeUntilMs = millis() + 2000UL;
  uiNeedsImmediateRedraw = true;
}

bool clockIsSet() { return time(nullptr) > static_cast<time_t>(CLOCK_VALID_EPOCH); }
static uint32_t nowEpochOrZero() { return clockIsSet() ? static_cast<uint32_t>(time(nullptr)) : 0; }

float fermentSaltForBase(float baseGrams) { return baseGrams * fermentSaltPct / 100.0f; }

// Tolerancia de "LISTO": 0.1 g o el 1 % del objetivo, lo que sea mayor
float fermentSaltTolerance() {
  const float t = fermentSaltTargetGrams * 0.01f;
  return t > 0.1f ? t : 0.1f;
}

// ---------------------------------------------------------------------------
// NVS
// ---------------------------------------------------------------------------
static void persistLot(const FermentLot& lot) {
  Preferences f;
  f.begin(NVS_FERM_NAMESPACE, false);
  f.putUShort("next_id", nextLotId);
  char key[8];
  snprintf(key, sizeof(key), "l_%u", lot.lotId % MAX_FERMENT_LOTS);
  f.putBytes(key, &lot, sizeof(FermentLot));
  f.end();
}

void saveFermentSettings() {
  Preferences p;
  p.begin(NVS_POUR_NAMESPACE, false);
  p.putFloat("f_salt", fermentSaltPct);
  p.end();
}

void loadFermentData() {
  Preferences p;
  p.begin(NVS_POUR_NAMESPACE, true);
  fermentSaltPct = p.getFloat("f_salt", 2.5f);
  p.end();
  if (isnan(fermentSaltPct) || fermentSaltPct < SALT_PCT_MIN || fermentSaltPct > SALT_PCT_MAX) fermentSaltPct = 2.5f;

  Preferences f;
  f.begin(NVS_FERM_NAMESPACE, true);
  nextLotId        = f.getUShort("next_id", 1);
  fermentLotsCount = 0;
  FermentLot tmp;
  for (uint8_t slot = 0; slot < MAX_FERMENT_LOTS; slot++) {
    char key[8];
    snprintf(key, sizeof(key), "l_%u", slot);
    if (f.getBytesLength(key) != sizeof(FermentLot)) continue;
    f.getBytes(key, &tmp, sizeof(FermentLot));
    if (tmp.magic == 0x4645524DUL && tmp.lotId != 0 && (tmp.lotId % MAX_FERMENT_LOTS) == slot &&
        !isnan(tmp.baseGrams) && tmp.readingsCount <= MAX_LOT_READINGS) {
      fermentLots[fermentLotsCount++] = tmp;
    }
  }
  f.end();
  for (uint8_t i = 1; i < fermentLotsCount; i++) {   // orden cronológico por id
    FermentLot key = fermentLots[i];
    int8_t j = static_cast<int8_t>(i) - 1;
    while (j >= 0 && fermentLots[j].lotId > key.lotId) { fermentLots[j + 1] = fermentLots[j]; j--; }
    fermentLots[j + 1] = key;
  }
  if (fermentLotsCount > 0) {
    const uint16_t maxId = fermentLots[fermentLotsCount - 1].lotId;
    if (nextLotId <= maxId) nextLotId = static_cast<uint16_t>(maxId + 1);
    selectedLotSlot = static_cast<int8_t>(fermentLotsCount - 1);
  }
  Serial.printf("[FERMENTO] Sal %.1f %% | %u lotes guardados | reloj %s\n",
                fermentSaltPct, fermentLotsCount, clockIsSet() ? "con hora" : "sin hora");
}

// ---------------------------------------------------------------------------
// Calculadora
// ---------------------------------------------------------------------------
void fermentResetStep() {
  fermentStep            = FermentStep::Base;
  fermentBaseGrams       = 0.0f;
  fermentSaltTargetGrams = 0.0f;
}

static void storeLot(float saltGrams) {
  uint8_t idx = fermentLotsCount;
  if (idx >= MAX_FERMENT_LOTS) {
    for (uint8_t i = 0; i < MAX_FERMENT_LOTS - 1; i++) fermentLots[i] = fermentLots[i + 1];
    idx = MAX_FERMENT_LOTS - 1;
  } else {
    fermentLotsCount++;
  }
  FermentLot& lot = fermentLots[idx];
  memset(&lot, 0, sizeof(FermentLot));
  lot.magic       = 0x4645524DUL;
  lot.lotId       = nextLotId++;
  lot.saltPctX100 = static_cast<uint16_t>(lroundf(fermentSaltPct * 100.0f));
  lot.baseGrams   = fermentBaseGrams;
  lot.saltGrams   = saltGrams;
  lot.startEpoch  = nowEpochOrZero();
  persistLot(lot);

  selectedLotSlot        = static_cast<int8_t>(idx);
  lastSavedLotId         = lot.lotId;
  bannerIsLot            = true;
  bannerIsShot           = false;
  pourSavedBannerUntilMs = millis() + 2500UL;
  uiNeedsImmediateRedraw = true;
  Serial.printf("[FERMENTO] Lote #%u guardado: base %.1f g, sal %.1f g (objetivo %.1f g = %.1f %%), real %.2f %%\n",
                lot.lotId, lot.baseGrams, saltGrams, fermentSaltTargetGrams, fermentSaltPct,
                lot.baseGrams > 0.0f ? saltGrams * 100.0f / lot.baseGrams : 0.0f);
}

// 1 clic en la vista principal en modo fermento
void fermentClick() {
  const float w = currentWeightGrams;
  if (fermentStep == FermentStep::Base) {
    if (w < FERMENT_MIN_BASE_G) {
      fermentShowNotice("PESA VERDURA+AGUA");
      return;
    }
    fermentBaseGrams       = w;
    fermentSaltTargetGrams = fermentSaltForBase(w);
    fermentStep            = FermentStep::Salt;
    executeTimemoreTare();   // a partir de aquí la báscula pesa solo la sal
    Serial.printf("[FERMENTO] Base %.1f g -> sal %.1f g (%.1f %%)\n", w, fermentSaltTargetGrams, fermentSaltPct);
    uiNeedsImmediateRedraw = true;
    return;
  }
  if (w < FERMENT_MIN_SALT_G) {   // sin sal: volver a pesar la base
    fermentResetStep();
    uiNeedsImmediateRedraw = true;
    return;
  }
  storeLot(w);
  fermentResetStep();
}

// ---------------------------------------------------------------------------
// Lecturas de los lotes
// ---------------------------------------------------------------------------
// 1 clic en el visor de lotes: añade la lectura actual de la sonda al lote seleccionado
void fermentAddReading() {
  if (fermentLotsCount == 0) return;
  if (selectedLotSlot < 0 || selectedLotSlot >= fermentLotsCount) selectedLotSlot = static_cast<int8_t>(fermentLotsCount - 1);
  accproto::Reading r;
  if (!probeGetFresh(r, nullptr) || (!r.phValid() && !r.tempValid())) {
    fermentShowNotice("SIN SONDA");
    return;
  }
  FermentLot& lot = fermentLots[selectedLotSlot];
  if (lot.readingsCount >= MAX_LOT_READINGS) {
    // Lleno: se conserva la primera (día 0) y se descarta la segunda
    for (uint8_t i = 1; i < MAX_LOT_READINGS - 1; i++) lot.readings[i] = lot.readings[i + 1];
    lot.readingsCount = MAX_LOT_READINGS - 1;
  }
  FermentReading& fr = lot.readings[lot.readingsCount++];
  memset(&fr, 0, sizeof(fr));
  fr.epoch   = nowEpochOrZero();
  fr.phX100  = r.phValid() ? static_cast<int16_t>(lroundf(r.ph * 100.0f)) : FERM_NO_DATA;
  fr.tempX10 = r.tempValid() ? static_cast<int16_t>(lroundf(r.tempC * 10.0f)) : FERM_NO_DATA;
  fr.brixX10 = 0xFFFF;
  persistLot(lot);
  fermentShowNotice((r.flags & accproto::FLAG_STABLE) ? "LECTURA GUARDADA" : "GUARDADA (INESTABLE)");
  Serial.printf("[FERMENTO] Lote #%u lectura %u: pH %.2f, %.1f C%s\n", lot.lotId, lot.readingsCount,
                r.ph, r.tempC, (r.flags & accproto::FLAG_STABLE) ? "" : " (inestable)");
}

// Etiqueta de una lectura: "D+5" con hora, "#3" sin ella
void fermentReadingTag(const FermentLot& lot, uint8_t i, char* out, size_t cap) {
  const FermentReading& fr = lot.readings[i];
  if (fr.epoch != 0 && lot.startEpoch != 0 && fr.epoch >= lot.startEpoch) {
    snprintf(out, cap, "D+%lu", static_cast<unsigned long>((fr.epoch - lot.startEpoch) / 86400UL));
  } else {
    snprintf(out, cap, "#%u", i + 1);
  }
}

// Fecha corta "25/11" (UTC) o "--"
void formatShortDate(uint32_t epoch, char* out, size_t cap) {
  if (epoch == 0) { snprintf(out, cap, "--"); return; }
  time_t t = static_cast<time_t>(epoch);
  struct tm tmv;
  gmtime_r(&t, &tmv);
  snprintf(out, cap, "%02d/%02d", tmv.tm_mday, tmv.tm_mon + 1);
}

// Orden serie 'z<epoch>': poner el reloj (sobrevive al reposo profundo, no al interruptor)
void fermentSetClock(uint32_t epoch) {
  if (epoch < CLOCK_VALID_EPOCH) {
    Serial.println("[RELOJ] Hora no valida (z seguido de segundos desde 1970, UTC).");
    return;
  }
  struct timeval tv;
  tv.tv_sec  = static_cast<time_t>(epoch);
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
  char d[8];
  formatShortDate(epoch, d, sizeof(d));
  Serial.printf("[RELOJ] Hora puesta: %lu (%s)\n", static_cast<unsigned long>(epoch), d);
}

// Estado del LED mientras se echa la sal: 0 nada, 1 cerca (90 %), 2 listo, 3 sobra
uint8_t fermentSaltLedState() {
  if (brewMode != BrewMode::Fermento || fermentStep != FermentStep::Salt || fermentSaltTargetGrams <= 0.0f) return 0;
  const float w = currentWeightGrams, t = fermentSaltTargetGrams, tol = fermentSaltTolerance();
  if (w > t + tol) return 3;
  if (w >= t - tol) return 2;
  if (w >= 0.9f * t) return 1;
  return 0;
}
