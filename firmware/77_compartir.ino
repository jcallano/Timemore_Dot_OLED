// ============================================================================
// ENVIAR CAFÉS A BEANCONQUEROR (POR WIFI, CON EL CAFÉ YA HECHO)
// Pestaña del sketch: el IDE la concatena tras firmware.ino (orden alfabético).
//
// Desde el historial de pour-overs (página "ENVIAR A BEANCONQUEROR"): el display apaga el Bluetooth (la
// Dot no hace falta con el café terminado y así el WiFi no comparte la radio), se conecta a la red guardada
// (o, si no hay o no conecta, crea la suya) y sirve una página. En la OLED sale un QR con la dirección; el
// móvil la abre con la cámara, elige los cafés y los manda a Beanconqueror con su enlace de importación
// (beanconqueror://ADD_BREW / ADD_BREWS, docs/brew-import-api.md de Beanconqueror). El display entrega cada
// café ya en ese formato (sobre "handoff" v1); la página solo pone los nombres del usuario, comprime y abre.
// Se sale con un clic, a los 3 minutos sin uso o con 'x' por el monitor serie. Al salir vuelve el Bluetooth
// y la Dot se reconecta sola.
// ============================================================================
#include <WebServer.h>
#include <qrcode.h>            // esp_qrcode (componente de ESP-IDF incluido en el core)
#include "src/share_page.h"

static constexpr uint32_t SHARE_IDLE_MS      = 180000;   // sin peticiones ni botón: se cierra
static constexpr uint32_t SHARE_WIFI_MS      = 10000;    // espera para conectar a la red guardada
static constexpr uint16_t SHARE_FLOW_STEP_MS = 500;      // curva para Beanconqueror a 2 Hz
static constexpr uint8_t  SHARE_MAX_IDS      = MAX_SAVED_POURS;

static WebServer* shareServer       = nullptr;
static bool shareApMode             = false;
static char shareUrl[48]            = "";
static char shareIp[16]             = "";
static char shareApSsid[20]         = "";
static char shareApPass[12]         = "";
static char shareDevice[20]         = "TMDOT";
static uint16_t sharePreselectId    = 0;
static unsigned long shareLastHitMs = 0;
static uint8_t shareSentCount       = 0;
static bool shareDirty              = true;

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------
static void jsonStr(String& o, const char* s) {
  o += '"';
  for (const char* p = s; *p; p++) {
    const char c = *p;
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if (static_cast<uint8_t>(c) < 0x20) o += ' ';
    else o += c;
  }
  o += '"';
}

static void jsonNum(String& o, float v, uint8_t dec) {
  char b[24];
  if (!isfinite(v)) v = 0.0f;
  snprintf(b, sizeof(b), "%.*f", dec, v);
  o += b;
}

// "2026-11-26T18:30:00.000Z" (el reloj guarda UTC)
static void isoDate(uint32_t epoch, char* out, size_t cap) {
  time_t t = static_cast<time_t>(epoch);
  struct tm tmv;
  gmtime_r(&t, &tmv);
  strftime(out, cap, "%Y-%m-%dT%H:%M:%S.000Z", &tmv);
}

// ---------------------------------------------------------------------------
// Curva del café: de la traza a 10 Hz si sigue en la flash; si no, de la gráfica de 120 puntos
// ---------------------------------------------------------------------------
struct ShareFlow {
  String t, water, weight;
  uint16_t n = 0;
  float brewTimeS = 0.0f;
  bool full = false;
};

// Funciones con tipos de esta pestaña (o de 47_trazas) en sus parámetros: lambdas, no funciones (el
// generador de prototipos del IDE las declararía antes de esos tipos).
// Busca la traza de un pour-over. Devuelve su número (0 = no hay).
static auto shareFindTrace = [](uint16_t pourId, TraceHeader& out) -> uint32_t {
  if (!traceFsReady) return 0;
  uint32_t seqs[32];
  const uint8_t n = traceListSeqs(seqs, 32);
  uint32_t best = 0;
  for (uint8_t i = 0; i < n; i++) {
    char path[32];
    tracePath(seqs[i], path, sizeof(path));
    File f = LittleFS.open(path, "r");
    TraceHeader h;
    if (f && traceReadHeader(f, h) && h.pourId == pourId && h.seq > best) { best = h.seq; out = h; }
    if (f) f.close();
  }
  return best;
};

static auto shareFlowFromTrace = [](uint16_t pourId, ShareFlow& fl) -> bool {
  TraceHeader h;
  const uint32_t seq = shareFindTrace(pourId, h);
  if (seq == 0 || h.nSamples == 0) return false;
  char path[32];
  tracePath(seq, path, sizeof(path));
  File f = LittleFS.open(path, "r");
  if (!f) return false;

  // Eventos: inicio y fin del cronómetro (sin ellos, el primer vertido y el último dato)
  uint32_t startMs = UINT32_MAX, stopMs = 0, firstPourMs = UINT32_MAX;
  f.seek(sizeof(TraceHeader) + static_cast<uint32_t>(h.nSamples) * sizeof(TraceSample));
  for (uint16_t i = 0; i < h.nEvents; i++) {
    TraceEvent e;
    if (f.read(reinterpret_cast<uint8_t*>(&e), sizeof(e)) != sizeof(e)) break;
    if (e.type == TE_TIMER_START && startMs == UINT32_MAX) startMs = e.ms;
    if (e.type == TE_TIMER_STOP) stopMs = e.ms;
    if (e.type == TE_POUR_START && firstPourMs == UINT32_MAX) firstPourMs = e.ms;
  }
  if (startMs == UINT32_MAX) startMs = firstPourMs;
  if (startMs == UINT32_MAX) { f.close(); return false; }

  f.seek(sizeof(TraceHeader));
  TraceSample buf[32];
  uint16_t left = h.nSamples;
  uint32_t nextMs = startMs, lastKeptMs = startMs;
  int32_t prevW = 0;
  while (left > 0) {
    const uint16_t k = left > 32 ? 32 : left;
    if (f.read(reinterpret_cast<uint8_t*>(buf), sizeof(TraceSample) * k) != sizeof(TraceSample) * k) break;
    left -= k;
    for (uint16_t i = 0; i < k; i++) {
      const TraceSample& s = buf[i];
      if (s.ms < nextMs) continue;
      if (stopMs > startMs && s.ms > stopMs) { left = 0; break; }
      int32_t w = s.netX10 > 0 ? s.netX10 : 0;   // décimas de g
      if (fl.n) { fl.t += ','; fl.water += ','; fl.weight += ','; }
      fl.t += String(s.ms - lastKeptMs);
      fl.water += '0';
      fl.weight += String(w - prevW);
      prevW = w;
      lastKeptMs = s.ms;
      nextMs = s.ms + SHARE_FLOW_STEP_MS;
      fl.n++;
    }
  }
  f.close();
  if (fl.n < 2) return false;
  fl.brewTimeS = ((stopMs > startMs) ? (stopMs - startMs) : (lastKeptMs - startMs)) / 1000.0f;
  fl.full = true;
  return true;
};

static auto shareFlowFromCurve = [](const PourOverRecord& r, ShareFlow& fl) {
  const float maxW = r.finalWeightGrams > 10.0f ? r.finalWeightGrams : 10.0f;
  const float stepMs = (r.durationSec * 1000.0f) / (POUR_GRAPH_POINTS - 1);
  int32_t prevW = 0;
  uint32_t prevT = 0;
  for (uint8_t i = 0; i < POUR_GRAPH_POINTS; i++) {
    const uint32_t tMs = static_cast<uint32_t>(lroundf(i * stepMs));
    const int32_t w = static_cast<int32_t>(lroundf(r.weightCurve[i] / 38.0f * maxW * 10.0f));
    if (i) { fl.t += ','; fl.water += ','; fl.weight += ','; }
    fl.t += String(tMs - prevT);
    fl.water += '0';
    fl.weight += String(w - prevW);
    prevW = w;
    prevT = tMs;
  }
  fl.n = POUR_GRAPH_POINTS;
  fl.brewTimeS = r.durationSec;
  fl.full = false;
};

// ---------------------------------------------------------------------------
// Un café en el formato de importación de Beanconqueror
// ---------------------------------------------------------------------------
static void shareAppendEnvelope(String& o, const PourOverRecord& r) {
  ShareFlow fl;
  if (!shareFlowFromTrace(r.pourId, fl)) shareFlowFromCurve(r, fl);

  bool bevMeasured = false;
  const float bev = pourBeverageGrams(r, &bevMeasured);
  float tdsV = 0.0f;
  uint8_t tdsFlags = 0;
  const bool hasTds = recordTds(false, r.pourId, &tdsV, &tdsFlags);
  bool eyEst = false;
  const float ey = hasTds ? recordEy(false, r.pourId, &eyEst) : 0.0f;
  const GuideTiming* gt = findGuideTiming(r.pourId);
  const float grind = recordGrind(false, r.pourId);
  const BrewExtra* ex = findExtra(false, r.pourId, false);
  const uint32_t epoch = recordEpoch(false, r.pourId);
  int8_t ab = 0, st = 0;
  const bool hasTaste = recordTaste(false, r.pourId, &ab, &st);

  // Nota: lo que Beanconqueror no tiene como campo (el TDS, entre otros)
  char note[400];
  size_t k = 0;
  auto add = [&](const char* fmt, auto... args) {
    if (k < sizeof(note)) k += snprintf(note + k, sizeof(note) - k, fmt, args...);
  };
  add("Timemore Dot OLED #%u", r.pourId);
  if (hasTds) add(" | TDS %.2f %% | EY %.1f %%%s", tdsV, ey, eyEst ? TR(" (bebida estimada)", " (estimated beverage)") : "");
  if (r.hasBloom && gt && gt->bloomWaterX10) add(" | bloom %.0f g", gt->bloomWaterX10 / 10.0f);
  else if (r.hasBloom)                         add(" | bloom");
  if (r.hasBloom) add(TR(" + %u vertidos", " + %u pours"), (unsigned)(r.poursCount - 1));
  else            add(TR(" | %u vertidos", " | %u pours"), (unsigned)r.poursCount);
  if (gt && gt->taps) add(TR(" (%u toques descartados)", " (%u taps ignored)"), (unsigned)gt->taps);
  add(TR(" | caudal medio %.1f g/s, max %.1f g/s", " | flow avg %.1f g/s, max %.1f g/s"), r.avgFlowGps, r.peakFlowGps);
  if (gt && gt->beansX10) {
    add(TR(" | granos %.1f g", " | beans %.1f g"), gt->beansX10 / 10.0f);
    if (gt->groundX10) add(TR(", retencion del molino %.1f g", ", grinder retention %.1f g"),
                           (static_cast<int>(gt->beansX10) - static_cast<int>(gt->groundX10)) / 10.0f);
  }
  if (!bevMeasured) add(TR(" | bebida estimada", " | beverage estimated"));
  if (ex && ex->bagId) add(TR(" | bolsa #%u", " | bag #%u"), (unsigned)ex->bagId);
  if (hasTaste) {
    static const char* AB_ES[] = {"muy agrio", "agrio", "equilibrado", "amargo", "muy amargo"};
    static const char* AB_EN[] = {"very sour", "sour", "balanced", "bitter", "very bitter"};
    static const char* ST_ES[] = {"muy aguado", "aguado", "en su punto", "intenso", "muy intenso"};
    static const char* ST_EN[] = {"very weak", "weak", "just right", "strong", "very strong"};
    add(TR(" | sabor: %s, %s", " | taste: %s, %s"), (uiEnglish ? AB_EN : AB_ES)[ab + 2], (uiEnglish ? ST_EN : ST_ES)[st + 2]);
  }

  o += F("{\"v\":1,\"app\":{\"name\":\"Timemore Dot OLED\"},\"brew\":{\"date\":");
  if (epoch != 0) { char d[32]; isoDate(epoch, d, sizeof(d)); jsonStr(o, d); }
  else o += F("null");   // sin hora: la página pone la del móvil
  o += F(",\"doseIn\":{\"value\":");          jsonNum(o, constrain(r.doseGrams, 0.0f, 200.0f), 1);
  o += F(",\"unit\":\"g\"},\"waterIn\":{\"value\":"); jsonNum(o, r.finalWeightGrams > 0 ? r.finalWeightGrams : 0.0f, 1);
  o += F(",\"unit\":\"ml\"},\"beverageOut\":{\"value\":"); jsonNum(o, bev, 1);
  o += F(",\"unit\":\"g\"},\"brewTime\":");    jsonNum(o, fl.brewTimeS, 1);
  o += F(",\"ratio\":");                       jsonNum(o, r.finalRatio > 0 ? r.finalRatio : 0.0f, 1);
  if (gt && gt->waterTempC) { o += F(",\"temperature\":"); o += String(gt->waterTempC); }
  if (grind >= 0.0f) { o += F(",\"grindSize\":"); char g[12]; snprintf(g, sizeof(g), "%.1f", grind); jsonStr(o, g); }
  o += F(",\"preparationMethod\":\"V60\",\"preparationType\":\"V60\"");
  if (r.hasBloom && gt && gt->bloomEndDs) { o += F(",\"bloomTime\":"); jsonNum(o, gt->bloomEndDs / 10.0f, 1); }
  o += F(",\"note\":");                        jsonStr(o, note);
  o += F("},\"flow\":{\"fidelity\":\"downsampled\",\"t\":[");              o += fl.t;
  o += F("],\"waterDispensed\":["); o += fl.water;
  o += F("],\"weight\":[");        o += fl.weight;
  o += F("]},\"imported\":{\"source\":\"timemore-dot-oled\",\"sourceName\":\"Timemore Dot OLED\",\"device\":");
  jsonStr(o, shareDevice);
  o += F(",\"schema\":1,\"params\":{\"pourId\":");  o += String(r.pourId);
  if (hasTds) { o += F(",\"tds\":"); jsonNum(o, tdsV, 2); o += F(",\"ey\":"); jsonNum(o, ey, 1); }
  if (gt && gt->beansX10)  { o += F(",\"beans\":");  jsonNum(o, gt->beansX10 / 10.0f, 1); }
  if (gt && gt->groundX10) { o += F(",\"ground\":"); jsonNum(o, gt->groundX10 / 10.0f, 1); }
  if (gt && gt->beansX10 && gt->groundX10) {
    o += F(",\"grinderRetention\":");
    jsonNum(o, (static_cast<int>(gt->beansX10) - static_cast<int>(gt->groundX10)) / 10.0f, 1);
  }
  o += F(",\"beverageMeasured\":"); o += bevMeasured ? F("true") : F("false");
  o += F(",\"trace\":");            o += fl.full ? F("true") : F("false");
  o += F("}}}");
}

// ---------------------------------------------------------------------------
// Servidor
// ---------------------------------------------------------------------------
static void shareTouch() {
  shareLastHitMs = millis();
  registerUserOrScaleActivity();
}

static uint8_t shareParseIds(const String& s, uint16_t* ids) {
  uint8_t n = 0;
  const char* p = s.c_str();
  while (*p && n < SHARE_MAX_IDS) {
    char* end;
    const unsigned long v = strtoul(p, &end, 10);
    if (end == p) { p++; continue; }
    if (v > 0 && v < 65536 && findPourById(static_cast<uint16_t>(v)) != nullptr) ids[n++] = static_cast<uint16_t>(v);
    p = end;
  }
  return n;
}

static void shareHandlePage() {
  shareTouch();
  shareServer->send_P(200, "text/html; charset=utf-8", SHARE_PAGE_HTML);
}

static void shareHandleBrews() {
  shareTouch();
  String o;
  o.reserve(160 + savedPoursCount * 140);
  o += F("{\"device\":");
  jsonStr(o, shareDevice);
  o += F(",\"brews\":[");
  for (uint8_t i = 0; i < savedPoursCount; i++) {
    const PourOverRecord& r = psramSavedPours[i];
    bool measured = false;
    const float bev = pourBeverageGrams(r, &measured);
    float tdsV = 0.0f;
    const bool hasTds = recordTds(false, r.pourId, &tdsV, nullptr);
    if (i) o += ',';
    o += F("{\"id\":");        o += String(r.pourId);
    o += F(",\"date\":");      o += String(recordEpoch(false, r.pourId));
    o += F(",\"dose\":");      jsonNum(o, r.doseGrams, 1);
    o += F(",\"water\":");     jsonNum(o, r.finalWeightGrams, 1);
    o += F(",\"bev\":");       jsonNum(o, bev, 1);
    o += F(",\"bevMeasured\":"); o += measured ? F("true") : F("false");
    o += F(",\"time\":");      o += String(r.durationSec);
    o += F(",\"tds\":");       jsonNum(o, hasTds ? tdsV : 0.0f, 2);
    o += F(",\"sent\":");      o += recordSentToBc(r.pourId) ? F("true") : F("false");
    o += '}';
  }
  o += F("]}");
  shareServer->send(200, "application/json", o);
}

static void shareHandleHandoff() {
  shareTouch();
  uint16_t ids[SHARE_MAX_IDS];
  const uint8_t n = shareParseIds(shareServer->arg("ids"), ids);
  String o;
  o.reserve(2048 + n * 6144);
  o += F("{\"brews\":[");
  for (uint8_t i = 0; i < n; i++) {
    if (i) o += ',';
    shareAppendEnvelope(o, *findPourById(ids[i]));
  }
  o += F("]}");
  Serial.printf("[COMPARTIR] %u cafe(s) entregados a la pagina (%u bytes).\n", n, o.length());
  shareServer->send(200, "application/json", o);
}

static void shareHandleSent() {
  shareTouch();
  uint16_t ids[SHARE_MAX_IDS];
  const uint8_t n = shareParseIds(shareServer->arg("ids"), ids);
  for (uint8_t i = 0; i < n; i++) markSentToBc(ids[i]);
  shareSentCount += n;
  shareDirty = true;
  Serial.printf("[COMPARTIR] %u cafe(s) marcados como enviados.\n", n);
  shareServer->send(200, "application/json", "{\"ok\":true}");
}

static void shareHandleOther() {
  // /P/<id> (la dirección del QR, en mayúsculas para que el QR sea más pequeño) y cualquier otra: la página
  const String u = shareServer->uri();
  if (u.startsWith("/api/")) { shareServer->send(404, "text/plain", "not found"); return; }
  shareHandlePage();
}

// ---------------------------------------------------------------------------
// Pantalla
// ---------------------------------------------------------------------------
static int shareQrY0 = 0;

// Módulos oscuros sobre fondo claro (como en papel) y 1 módulo de margen; a 2 px si cabe
static auto shareQrDraw = [](esp_qrcode_handle_t q) {
  const int n = esp_qrcode_get_size(q);
  int s = 62 / (n + 2);
  if (s < 1) s = 1;
  const int side = (n + 2) * s;
  const int x0 = (64 - side) / 2, y0 = (64 - side) / 2;
  oledFillRect(x0, y0, side, side, true);
  for (int y = 0; y < n; y++)
    for (int x = 0; x < n; x++)
      if (esp_qrcode_get_module(q, x, y)) oledFillRect(x0 + (x + 1) * s, y0 + (y + 1) * s, s, s, false);
  shareQrY0 = y0;
};

static void shareDrawQr(const char* text) {
  esp_qrcode_config_t cfg = {};
  cfg.display_func       = shareQrDraw;
  cfg.max_qrcode_version = 4;
  cfg.qrcode_ecc_level   = ESP_QRCODE_ECC_LOW;
  esp_qrcode_generate(&cfg, text);
}

static void shareScreenMsg(const char* l1, const char* l2) {
  if (!oledPresent) return;
  oledClear();
  oledFillRect(0, 0, 128, 11, true);
  oledDrawText(2, 2, "BEANCONQUEROR", 1, true);
  oledDrawText(2, 22, l1, 1);
  if (l2) oledDrawText(2, 36, l2, 1);
  oledDrawText(2, 56, TR("CLIC: SALIR", "CLICK: EXIT"), 1);
  oledFlush();
}

static void shareRender() {
  if (!oledPresent) return;
  oledClear();
  const bool joinFirst = shareApMode && WiFi.softAPgetStationNum() == 0;
  if (joinFirst) {
    char wifiQr[64];
    snprintf(wifiQr, sizeof(wifiQr), "WIFI:T:WPA;S:%s;P:%s;;", shareApSsid, shareApPass);
    shareDrawQr(wifiQr);
    oledDrawText(68, 0, TR("1.UNETE", "1.JOIN"), 1);
    oledDrawText(68, 9, TR("A LA RED", "WIFI"), 1);
    oledDrawText(68, 22, shareApSsid, 1);       // "TMDOT-XXXX": 10 caracteres, justo los que caben
    oledDrawText(68, 34, shareApPass, 1);
  } else {
    shareDrawQr(shareUrl);
    oledDrawText(68, 0, TR("ESCANEA", "SCAN WITH"), 1);
    oledDrawText(68, 9, TR("CON LA", "THE PHONE"), 1);
    oledDrawText(68, 18, TR("CAMARA", "CAMERA"), 1);
    char line[16];
    snprintf(line, sizeof(line), TR("ENVIADOS:%u", "SENT:%u"), shareSentCount);
    oledDrawText(68, 34, line, 1);
  }
  oledDrawText(68, 47, TR("CLIC:", "CLICK:"), 1);
  oledDrawText(68, 56, TR("SALIR", "EXIT"), 1);
  oledFlush();
}

// ---------------------------------------------------------------------------
// Sesión
// ---------------------------------------------------------------------------
static bool shareExitRequested() {
  if (otaButtonPressed()) return true;
  while (Serial.available() > 0) {
    const int c = Serial.read();
    if (c == 'x' || c == 'X') return true;
    if (c == 's' || c == 'S') dumpOledFramebuffer();   // captura de pantalla (tools/oled_capture.js)
  }
  return false;
}

// forceOwnNetwork: sin intentar la red guardada (orden serie 'W', para probar ese camino)
void enterShareMode(uint16_t preselectId, bool forceOwnNetwork) {
  sharePreselectId = preselectId;
  shareSentCount   = 0;
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(shareDevice, sizeof(shareDevice), "TMDOT %02X%02X", mac[4], mac[5]);
  Serial.printf("\n[COMPARTIR] Enviar a Beanconqueror (cafe #%u). Bluetooth apagado mientras tanto.\n", preselectId);

  setRgbColor(0, 0, LED_BRIGHTNESS);   // azul = enviando por WiFi (el cian es "vertido en curso")
  shareScreenMsg(TR("PREPARANDO...", "PREPARING..."), nullptr);
  while (digitalRead(PIN_ENC_SW) == LOW || digitalRead(PIN_BOOT_BUTTON) == LOW) delay(10);
  shutdownBleStack();

  // 1) La red guardada (la de la hora); 2) si no hay o no conecta, una red propia
  bool sta = false, aborted = false;
  WiFi.persistent(false);
  if (wifiSsid[0] != '\0' && !forceOwnNetwork) {
    char l2[24];
    snprintf(l2, sizeof(l2), "%.21s", wifiSsid);
    shareScreenMsg(TR("CONECTANDO A:", "CONNECTING TO:"), l2);
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiSsid, wifiPass);
    const unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - t0) < SHARE_WIFI_MS) {
      if (shareExitRequested()) { aborted = true; break; }
      delay(50);
    }
    sta = !aborted && WiFi.status() == WL_CONNECTED;
    if (!sta) { WiFi.disconnect(true, false); delay(50); }
  }
  if (!aborted) {
    if (sta) {
      shareApMode = false;
      WiFi.setSleep(false);   // respuesta rápida de la página
      strncpy(shareIp, WiFi.localIP().toString().c_str(), sizeof(shareIp) - 1);
    } else {
      shareApMode = true;
      snprintf(shareApSsid, sizeof(shareApSsid), "TMDOT-%02X%02X", mac[4], mac[5]);
      snprintf(shareApPass, sizeof(shareApPass), "%08lu", (unsigned long)(esp_random() % 100000000UL));
      WiFi.mode(WIFI_AP);
      WiFi.softAP(shareApSsid, shareApPass);
      delay(100);
      strncpy(shareIp, WiFi.softAPIP().toString().c_str(), sizeof(shareIp) - 1);
    }
    // En mayúsculas y sin '?': el QR usa el modo alfanumérico y sale más pequeño (módulos más grandes)
    snprintf(shareUrl, sizeof(shareUrl), "HTTP://%s/P/%u", shareIp, preselectId);
    Serial.printf("[COMPARTIR] %s: http://%s/  (QR: %s)\n", sta ? "En la red" : "Red propia", shareIp, shareUrl);
    if (shareApMode) Serial.printf("[COMPARTIR] Red '%s', clave %s\n", shareApSsid, shareApPass);

    shareServer = new WebServer(80);
    shareServer->on("/", HTTP_GET, shareHandlePage);
    shareServer->on("/api/brews", HTTP_GET, shareHandleBrews);
    shareServer->on("/api/handoff", HTTP_GET, shareHandleHandoff);
    shareServer->on("/api/sent", HTTP_ANY, shareHandleSent);
    shareServer->onNotFound(shareHandleOther);
    shareServer->begin();

    shareLastHitMs = millis();
    shareDirty     = true;
    int lastStations = -1;
    for (;;) {
      shareServer->handleClient();
      if (shareExitRequested()) break;
      if ((millis() - shareLastHitMs) > SHARE_IDLE_MS) { Serial.println("[COMPARTIR] Sin uso: cerrando."); break; }
      const int st = shareApMode ? WiFi.softAPgetStationNum() : 0;
      if (st != lastStations) { lastStations = st; shareDirty = true; if (st > 0) shareTouch(); }
      if (shareDirty) { shareDirty = false; shareRender(); }
      delay(2);
    }
    shareServer->stop();
    delete shareServer;
    shareServer = nullptr;
  }

  if (shareApMode) WiFi.softAPdisconnect(true);
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);
  Serial.printf("[COMPARTIR] Fin: %u cafe(s) enviados. Vuelve el Bluetooth.\n", shareSentCount);
  initBleStack();
  startTimemoreScan();
  registerUserOrScaleActivity();
  uiNeedsImmediateRedraw = true;
}
