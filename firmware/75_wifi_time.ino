// ============================================================================
// HORA: WIFI AL ARRANCAR (NTP) Y DESDE LA PÁGINA WEB DE ACTUALIZACIÓN
// Pestaña del sketch: el IDE la concatena tras firmware.ino (orden alfabético).
//
// El WiFi NO se usa durante el funcionamiento: solo al arrancar, ANTES de iniciar el Bluetooth (así no
// comparte la radio con la Dot ni con el puente), y solo si el reloj no está en hora o pasaron más de
// WIFI_RESYNC_S desde la última vez. Se conecta, pide la hora por NTP y apaga la radio (unos 3-6 s).
// La red, la clave y la zona horaria se guardan desde docs/ota/ (Bluetooth, con el PIN de la pantalla);
// esa página también pone la hora del teléfono o el ordenador directamente, sin WiFi.
// El reloj del ESP32 sigue en el reposo profundo, pero no si se corta la alimentación (interruptor).
// ============================================================================
#include <WiFi.h>
#include <time.h>
#include <sys/time.h>

static const char*        NVS_WIFI_NAMESPACE = "tmdot_wifi";
static constexpr uint32_t WIFI_CONNECT_MS    = 8000;
static constexpr uint32_t NTP_WAIT_MS        = 5000;
static constexpr uint32_t WIFI_RESYNC_S      = 12UL * 3600UL;
// CLOCK_VALID_EPOCH: en firmware.ino (también lo usa el fermento, 47_fermento.ino)

static char wifiSsid[33]   = "";
static char wifiPass[65]   = "";
int16_t tzOffsetMin        = 0;          // minutos respecto a UTC (Omán: +240)
RTC_DATA_ATTR static time_t lastClockSyncEpoch = 0;   // sobrevive al reposo profundo
bool wifiSettingsChanged   = false;      // al salir del modo OTA se reinicia para probar la red

bool clockIsSet() { return time(nullptr) > CLOCK_VALID_EPOCH; }

static void applyTimezone() {
  // POSIX invierte el signo: UTC+4 se escribe "UTC-04:00"
  char tz[24];
  const int m = tzOffsetMin < 0 ? -tzOffsetMin : tzOffsetMin;
  snprintf(tz, sizeof(tz), "UTC%c%02d:%02d", tzOffsetMin >= 0 ? '-' : '+', m / 60, m % 60);
  setenv("TZ", tz, 1);
  tzset();
}

void loadWifiSettings() {
  Preferences p;
  p.begin(NVS_WIFI_NAMESPACE, true);
  String s = p.getString("ssid", "");
  String k = p.getString("pass", "");
  tzOffsetMin = p.getShort("tz", 0);
  p.end();
  strncpy(wifiSsid, s.c_str(), sizeof(wifiSsid) - 1);
  strncpy(wifiPass, k.c_str(), sizeof(wifiPass) - 1);
  applyTimezone();
}

// Llamadas desde el modo OTA (70_ota.ino) con los datos de la página web
void saveWifiCredentials(const char* ssid, const char* pass) {
  strncpy(wifiSsid, ssid, sizeof(wifiSsid) - 1);
  wifiSsid[sizeof(wifiSsid) - 1] = '\0';
  strncpy(wifiPass, pass, sizeof(wifiPass) - 1);
  wifiPass[sizeof(wifiPass) - 1] = '\0';
  Preferences p;
  p.begin(NVS_WIFI_NAMESPACE, false);
  p.putString("ssid", wifiSsid);
  p.putString("pass", wifiPass);
  p.end();
  wifiSettingsChanged = true;
  Serial.printf("[WIFI] Red guardada: \"%s\"\n", wifiSsid);
}

void setClockFromEpoch(uint32_t epoch, int16_t tzMin) {
  if (epoch < static_cast<uint32_t>(CLOCK_VALID_EPOCH)) return;
  struct timeval tv;
  tv.tv_sec  = static_cast<time_t>(epoch);
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
  lastClockSyncEpoch = static_cast<time_t>(epoch);
  if (tzMin != tzOffsetMin) {
    tzOffsetMin = tzMin;
    Preferences p;
    p.begin(NVS_WIFI_NAMESPACE, false);
    p.putShort("tz", tzOffsetMin);
    p.end();
  }
  applyTimezone();
  Serial.printf("[RELOJ] Hora puesta desde la pagina web (UTC%+d min).\n", tzOffsetMin);
}

// "16:42" o "--:--"; "25/11" o "--/--"
void formatClockHM(char* out, size_t cap) {
  if (!clockIsSet()) { snprintf(out, cap, "--:--"); return; }
  time_t t = time(nullptr);
  struct tm tmv;
  localtime_r(&t, &tmv);
  snprintf(out, cap, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
}

void formatDateTime(uint32_t epoch, char* out, size_t cap) {
  if (epoch < static_cast<uint32_t>(CLOCK_VALID_EPOCH)) { snprintf(out, cap, "--"); return; }
  time_t t = static_cast<time_t>(epoch);
  struct tm tmv;
  localtime_r(&t, &tmv);
  snprintf(out, cap, "%02d/%02d %02d:%02d", tmv.tm_mday, tmv.tm_mon + 1, tmv.tm_hour, tmv.tm_min);
}

uint32_t nowEpochOrZero() { return clockIsSet() ? static_cast<uint32_t>(time(nullptr)) : 0; }

static void wifiScreen(const char* l1, const char* l2) {
  if (!oledPresent) return;
  oledClear();
  oledFillRect(0, 0, 128, 11, true);
  oledDrawText(2, 2, TR("HORA POR WIFI", "TIME OVER WIFI"), 1, true);
  oledDrawText(2, 22, l1, 1);
  if (l2) oledDrawText(2, 36, l2, 1);
  oledFlush();
}

// En setup(), antes de initBleStack()
void wifiTimeSyncAtBoot() {
  if (wifiSsid[0] == '\0') return;
  const time_t now = time(nullptr);
  if (clockIsSet() && lastClockSyncEpoch > 0 && (now - lastClockSyncEpoch) < static_cast<time_t>(WIFI_RESYNC_S)) return;

  const unsigned long t0 = millis();
  char l2[24];
  snprintf(l2, sizeof(l2), "%.21s", wifiSsid);
  wifiScreen(TR("CONECTANDO A:", "CONNECTING TO:"), l2);
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid, wifiPass);
  while (WiFi.status() != WL_CONNECTED && (millis() - t0) < WIFI_CONNECT_MS) delay(100);

  bool ok = false;
  if (WiFi.status() == WL_CONNECTED) {
    wifiScreen(TR("PIDIENDO LA HORA...", "GETTING THE TIME..."), l2);
    configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
    applyTimezone();   // configTime cambia TZ: se vuelve a poner la nuestra
    const unsigned long t1 = millis();
    while (!clockIsSet() && (millis() - t1) < NTP_WAIT_MS) delay(100);
    ok = clockIsSet();
  }
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);

  if (ok) {
    lastClockSyncEpoch = time(nullptr);
    char hm[8], dt[20];
    formatClockHM(hm, sizeof(hm));
    formatDateTime(static_cast<uint32_t>(lastClockSyncEpoch), dt, sizeof(dt));
    wifiScreen(TR("HORA OK", "TIME OK"), dt);
    Serial.printf("[WIFI] Hora por NTP: %s (UTC%+d min) en %lu ms\n", dt, tzOffsetMin, millis() - t0);
  } else {
    wifiScreen(TR("SIN HORA", "NO TIME"), TR("RED NO DISPONIBLE", "NETWORK UNAVAILABLE"));
    Serial.printf("[WIFI] Sin hora: no se pudo conectar a \"%s\" (%lu ms)\n", wifiSsid, millis() - t0);
  }
  delay(700);
}
