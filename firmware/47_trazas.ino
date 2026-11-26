// ============================================================================
// TRAZAS DE CADA PREPARACIÓN (10 Hz)
// Todas las lecturas de la Dot (bruto, neto, caudal) y los eventos (pasos, taras, vertidos, toques,
// yield) de cada pour-over guiado, para ajustar los filtros con datos reales de la cocina.
//   - Se graban en PSRAM desde el paso Server hasta volver a Listo (máx. 10 min).
//   - Al pasar a Resumen se guardan en la flash (LittleFS, partición "spiffs" de 1.5 MB): las 8 últimas.
//   - Por el monitor serie: 'l' lista las trazas y 'f<n>' vuelca la n en CSV ('f' sola: la última).
//     tools/descargar_trazas.ps1 las descarga todas a archivos .csv.
// Pestaña del sketch: el IDE la concatena tras firmware.ino (orden alfabético).
// ============================================================================

static constexpr uint16_t TRACE_MAX_SAMPLES = 6000;          // 10 min a 10 Hz (72 KB en PSRAM)
static constexpr uint8_t  TRACE_MAX_EVENTS  = 160;
static constexpr uint8_t  TRACE_MAX_FILES   = 8;
static constexpr uint32_t TRACE_MAGIC       = 0x315A5254UL;  // "TRZ1"
static const char*        TRACE_DIR         = "/trz";

// Cabecera de cada archivo (le siguen las muestras y los eventos)
struct TraceHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t pourId;        // 0 = no se guardó el pour-over
  uint32_t seq;           // número de la traza
  uint32_t epoch;         // fecha y hora (0 = sin hora)
  uint16_t nSamples;
  uint16_t nEvents;
  float    dose;
  float    ratio;
  float    bloomFactor;
  float    serverRaw;
  uint8_t  demo;          // 1 = demostración ('v'), no una preparación real
  uint8_t  pad[3];
};

static TraceSample* traceBuf      = nullptr;
static TraceEvent traceEvents[TRACE_MAX_EVENTS];
static uint16_t traceCount        = 0;
static uint8_t traceEventCount    = 0;
static bool traceActive           = false;
static unsigned long traceStartMs = 0;
static uint32_t traceSavedSeq     = 0;   // ya guardada con este número (volver a guardar la pisa)
static uint32_t traceNextSeq      = 1;
static bool traceFsReady          = false;

// Monta la flash de las trazas (la primera vez la formatea: unos segundos, una sola vez)
void traceInit() {
  traceFsReady = LittleFS.begin(true, "/littlefs", 4, "spiffs");
  if (!traceFsReady) {
    Serial.println("[TRAZA] No se pudo montar LittleFS: las trazas no se guardaran.");
    return;
  }
  if (!LittleFS.exists(TRACE_DIR)) LittleFS.mkdir(TRACE_DIR);
  Preferences p;
  p.begin(NVS_POUR_NAMESPACE, true);
  traceNextSeq = p.getULong("trz_seq", 1);
  p.end();
  Serial.printf("[TRAZA] LittleFS: %u KB usados de %u KB.\n",
                (unsigned)(LittleFS.usedBytes() / 1024), (unsigned)(LittleFS.totalBytes() / 1024));
}

void traceBegin() {
  if (traceBuf == nullptr) {
    traceBuf = static_cast<TraceSample*>(ps_malloc(sizeof(TraceSample) * TRACE_MAX_SAMPLES));
    if (traceBuf == nullptr) return;
  }
  traceCount      = 0;
  traceEventCount = 0;
  traceSavedSeq   = 0;
  traceStartMs    = millis();
  traceActive     = true;
}

void traceStop() { traceActive = false; }

static int16_t traceClamp16(float v) {
  if (v > 32767.0f) return 32767;
  if (v < -32768.0f) return -32768;
  return static_cast<int16_t>(lroundf(v));
}

void traceSample(unsigned long ms, float raw, float net, float flow) {
  if (!traceActive || traceBuf == nullptr || traceCount >= TRACE_MAX_SAMPLES) return;
  TraceSample& s = traceBuf[traceCount++];
  s.ms       = static_cast<uint32_t>(ms - traceStartMs);
  s.rawX10   = traceClamp16(raw * 10.0f);
  s.netX10   = traceClamp16(net * 10.0f);
  s.flowX100 = traceClamp16(flow * 100.0f);
  s.stable   = weightIsStable ? 1 : 0;
  s.pad      = 0;
}

void traceEvent(uint8_t type, uint8_t arg, float val) {
  if (!traceActive || traceEventCount >= TRACE_MAX_EVENTS) return;
  TraceEvent& e = traceEvents[traceEventCount++];
  e.ms     = static_cast<uint32_t>(millis() - traceStartMs);
  e.type   = type;
  e.arg    = arg;
  e.valX10 = traceClamp16(val * 10.0f);
}

static void tracePath(uint32_t seq, char* out, size_t cap) {
  snprintf(out, cap, "%s/%05lu.bin", TRACE_DIR, (unsigned long)seq);
}

// Números de las trazas guardadas (orden del directorio). Devuelve cuántas hay.
static uint8_t traceListSeqs(uint32_t* seqs, uint8_t cap) {
  uint8_t n = 0;
  File dir = LittleFS.open(TRACE_DIR);
  if (!dir || !dir.isDirectory()) return 0;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    const char* name = f.name();
    const char* slash = strrchr(name, '/');
    if (slash) name = slash + 1;
    const uint32_t seq = strtoul(name, nullptr, 10);
    if (seq > 0 && n < cap) seqs[n++] = seq;
  }
  return n;
}

// Guarda la traza en curso (al pasar a Resumen). Devuelve el número de la traza (0 = no se guardó).
uint32_t traceSave(uint16_t pourId) {
  if (!traceFsReady || traceBuf == nullptr || traceCount == 0) return 0;
  const unsigned long t0 = millis();
  if (traceSavedSeq == 0) {
    traceSavedSeq = traceNextSeq++;
    Preferences p;
    p.begin(NVS_POUR_NAMESPACE, false);
    p.putULong("trz_seq", traceNextSeq);
    p.end();
  }
  TraceHeader h;
  memset(&h, 0, sizeof(h));
  h.magic       = TRACE_MAGIC;
  h.version     = 1;
  h.pourId      = pourId;
  h.seq         = traceSavedSeq;
  h.epoch       = nowEpochOrZero();
  h.nSamples    = traceCount;
  h.nEvents     = traceEventCount;
  h.dose        = coffeeDoseGrams;
  h.ratio       = pourRatio;
  h.bloomFactor = bloomFactor;
  h.serverRaw   = serverRawGrams;
  h.demo        = demoLiveActive ? 1 : 0;

  char path[32];
  tracePath(traceSavedSeq, path, sizeof(path));
  File f = LittleFS.open(path, "w");
  if (!f) {
    Serial.printf("[TRAZA] No se pudo crear %s.\n", path);
    return 0;
  }
  f.write(reinterpret_cast<const uint8_t*>(&h), sizeof(h));
  f.write(reinterpret_cast<const uint8_t*>(traceBuf), sizeof(TraceSample) * traceCount);
  f.write(reinterpret_cast<const uint8_t*>(traceEvents), sizeof(TraceEvent) * traceEventCount);
  f.close();

  // Solo las TRACE_MAX_FILES más recientes
  uint32_t seqs[32];
  uint8_t n = traceListSeqs(seqs, 32);
  while (n > TRACE_MAX_FILES) {
    uint8_t oldest = 0;
    for (uint8_t i = 1; i < n; i++) if (seqs[i] < seqs[oldest]) oldest = i;
    tracePath(seqs[oldest], path, sizeof(path));
    LittleFS.remove(path);
    seqs[oldest] = seqs[--n];
  }
  Serial.printf("[TRAZA] #%lu guardada: %u muestras, %u eventos, pour-over #%u (%lu ms).\n",
                (unsigned long)traceSavedSeq, traceCount, traceEventCount, pourId,
                (unsigned long)(millis() - t0));
  return traceSavedSeq;
}

// ---------------------------------------------------------------------------
// Descarga por el monitor serie
// ---------------------------------------------------------------------------
// El Serial no bloqueante descarta lo que no cabe: esperar hueco antes de cada línea
static void traceWaitRoom() {
  const unsigned long t0 = millis();
  while (!SafeUsbSerial.canWrite() && (millis() - t0) < 500UL) delay(1);
}

static const char* traceEventName(uint8_t type) {
  switch (type) {
    case TE_STEP:        return "paso";
    case TE_TARE:        return "tara";
    case TE_TIMER_START: return "cronometro_inicio";
    case TE_TIMER_STOP:  return "cronometro_fin";
    case TE_POUR_START:  return "vertido_inicio";
    case TE_POUR_OK:     return "vertido_ok";
    case TE_POUR_MERGED: return "vertido_unido";
    case TE_TAP:         return "toque";
    case TE_YIELD:       return "yield";
    case TE_CLICK:       return "vista";
  }
  return "?";
}

// Lee la cabecera de una traza (lambda y no función: el generador de prototipos del IDE la declararía antes
// de TraceHeader)
static auto traceReadHeader = [](File& f, TraceHeader& h) -> bool {
  return f.read(reinterpret_cast<uint8_t*>(&h), sizeof(h)) == sizeof(h) && h.magic == TRACE_MAGIC;
};

// 'l': lista de trazas guardadas
void traceListSerial() {
  traceWaitRoom();
  if (!traceFsReady) { Serial.println("[TRZ] Sin LittleFS."); return; }
  uint32_t seqs[32];
  const uint8_t n = traceListSeqs(seqs, 32);
  // Orden ascendente
  for (uint8_t i = 1; i < n; i++) {
    const uint32_t k = seqs[i];
    int8_t j = static_cast<int8_t>(i) - 1;
    while (j >= 0 && seqs[j] > k) { seqs[j + 1] = seqs[j]; j--; }
    seqs[j + 1] = k;
  }
  Serial.printf("[TRZ] LISTA %u\n", n);
  for (uint8_t i = 0; i < n; i++) {
    char path[32];
    tracePath(seqs[i], path, sizeof(path));
    File f = LittleFS.open(path, "r");
    TraceHeader h;
    if (!f || !traceReadHeader(f, h)) continue;
    f.close();
    char dt[20] = "-";
    if (h.epoch != 0) formatDateTime(h.epoch, dt, sizeof(dt));
    traceWaitRoom();
    Serial.printf("[TRZ] %lu pour=%u muestras=%u eventos=%u dosis=%.1f ratio=%.1f fecha=%s%s\n",
                  (unsigned long)h.seq, h.pourId, h.nSamples, h.nEvents, h.dose, h.ratio, dt,
                  h.demo ? " demo" : "");
  }
  traceWaitRoom();
  Serial.println("[TRZ] FIN");
}

// 'f<n>': vuelca la traza n en CSV (0 = la última)
void traceDumpSerial(uint32_t seq) {
  traceWaitRoom();
  if (!traceFsReady) { Serial.println("[CSV] ERROR sin LittleFS"); return; }
  if (seq == 0) {
    uint32_t seqs[32];
    const uint8_t n = traceListSeqs(seqs, 32);
    for (uint8_t i = 0; i < n; i++) if (seqs[i] > seq) seq = seqs[i];
  }
  char path[32];
  tracePath(seq, path, sizeof(path));
  File f = LittleFS.open(path, "r");
  TraceHeader h;
  if (!f || !traceReadHeader(f, h)) {
    Serial.printf("[CSV] ERROR no existe la traza %lu\n", (unsigned long)seq);
    return;
  }
  char dt[20] = "-";
  if (h.epoch != 0) formatDateTime(h.epoch, dt, sizeof(dt));
  Serial.printf("[CSV] BEGIN %lu\n", (unsigned long)h.seq);
  traceWaitRoom();
  Serial.printf("# traza=%lu pour=%u fecha=%s dosis=%.1f ratio=%.1f bloom_x=%.1f server_bruto=%.1f demo=%u\n",
                (unsigned long)h.seq, h.pourId, dt, h.dose, h.ratio, h.bloomFactor, h.serverRaw, h.demo);
  traceWaitRoom();
  Serial.println("t_ms,bruto_g,neto_g,caudal_gps,estable");
  TraceSample buf[32];
  uint16_t left = h.nSamples;
  while (left > 0) {
    const uint16_t k = left > 32 ? 32 : left;
    if (f.read(reinterpret_cast<uint8_t*>(buf), sizeof(TraceSample) * k) != sizeof(TraceSample) * k) break;
    for (uint16_t i = 0; i < k; i++) {
      traceWaitRoom();
      Serial.printf("%lu,%.1f,%.1f,%.2f,%u\n", (unsigned long)buf[i].ms, buf[i].rawX10 / 10.0f,
                    buf[i].netX10 / 10.0f, buf[i].flowX100 / 100.0f, buf[i].stable);
    }
    left -= k;
  }
  traceWaitRoom();
  Serial.println("# eventos: t_ms,evento,arg,valor");
  for (uint16_t i = 0; i < h.nEvents; i++) {
    TraceEvent e;
    if (f.read(reinterpret_cast<uint8_t*>(&e), sizeof(e)) != sizeof(e)) break;
    traceWaitRoom();
    Serial.printf("#E,%lu,%s,%u,%.1f\n", (unsigned long)e.ms, traceEventName(e.type), e.arg, e.valX10 / 10.0f);
  }
  f.close();
  traceWaitRoom();
  Serial.println("[CSV] END");
}
