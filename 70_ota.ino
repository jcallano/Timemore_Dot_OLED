// ============================================================================
// ACTUALIZACIÓN OTA POR WI-FI
// Punto de acceso WPA2 con clave por placa, subida web del .bin y ArduinoOTA.
// Pestaña del sketch: el IDE la concatena tras basicoledtimemore_dot.ino (orden alfabético).
// Tipos, constantes y variables globales compartidas están en la pestaña principal.
// ============================================================================

// ============================================================================
// MODO DE ACTUALIZACIÓN DE FIRMWARE INALÁMBRICO (WIFI WEB OTA + ARDUINO OTA)
// ============================================================================
static const char OTA_HTML_PAGE[] PROGMEM =
  "<!DOCTYPE html><html><head><meta charset='utf-8'>"
  "<meta name='viewport' content='width=device-width,initial-scale=1'>"
  "<title>Timemore Dot OLED - OTA</title>"
  "<style>body{background:#111;color:#eee;font-family:sans-serif;text-align:center;padding:24px;}"
  ".card{max-width:400px;margin:auto;background:#1e1e1e;padding:24px;border-radius:12px;border:1px solid #333;}"
  "input[type=file]{margin:18px 0;color:#ccc;}button{background:#00b894;color:#000;border:none;padding:12px 24px;"
  "font-size:16px;font-weight:bold;border-radius:8px;cursor:pointer;width:100%;}</style></head><body>"
  "<div class='card'><h2>Timemore Dot OLED</h2><p>Actualizacion de Firmware OTA (ESP32-S3)</p>"
  "<form method='POST' action='/update' enctype='multipart/form-data'>"
  "<input type='file' name='update' accept='.bin' required><br>"
  "<button type='submit'>Subir y Flashear (.bin)</button></form></div></body></html>";

// Contraseña WPA2 del AP OTA y de ArduinoOTA, única por placa: "TM" + 3 últimos bytes de la MAC
// de fábrica del chip (eFuse). Se muestra en la OLED al entrar en modo OTA. Antes la red era abierta
// y ArduinoOTA no tenía contraseña: cualquiera en alcance podía cargar un firmware.
static char otaPassword[12] = {0};

static const char* getOtaPassword() {
  if (otaPassword[0] == '\0') {
    uint64_t mac = ESP.getEfuseMac();
    uint8_t b3 = static_cast<uint8_t>((mac >> 24) & 0xFF);
    uint8_t b4 = static_cast<uint8_t>((mac >> 32) & 0xFF);
    uint8_t b5 = static_cast<uint8_t>((mac >> 40) & 0xFF);
    snprintf(otaPassword, sizeof(otaPassword), "TM%02X%02X%02X", b3, b4, b5);
  }
  return otaPassword;
}

static void renderOtaScreenOled(const char* statusLine, int progressPct) {
  if (!oledPresent) return;
  oledClear();
  oledFillRect(0, 0, 128, 11, true);
  oledDrawText(10, 2, "ACTUALIZAR POR OTA", 1, true);
  char passBuf[22];
  snprintf(passBuf, sizeof(passBuf), "CLAVE: %s", getOtaPassword());
  oledDrawText(2, 14, "WIFI: TIMEMORE-OTA", 1);
  oledDrawText(2, 23, passBuf, 1);
  oledDrawText(2, 32, "WEB:  192.168.4.1", 1);

  if (progressPct >= 0) {
    char pBuf[32];
    snprintf(pBuf, sizeof(pBuf), "FLASHEANDO: %3d%%", progressPct);
    oledDrawText(2, 42, pBuf, 1);
    oledDrawRect(2, 52, 124, 10, true);
    int wBar = (progressPct * 120) / 100;
    if (wBar > 120) wBar = 120;
    if (wBar > 0) oledFillRect(4, 54, wBar, 6, true);
  } else {
    oledDrawText(2, 42, statusLine ? statusLine : "ESPERANDO ARCHIVO...", 1);
    oledDrawHLine(0, 52, 128, true);
    oledDrawText(2, 55, "1 CLICK: CANCELAR", 1);
  }
  oledFlush();
}

void enterWifiOtaMode() {
  Serial.println("\n[OTA] Iniciando Modo de Actualizacion Inalambrica (Wi-Fi AP + Web OTA + ArduinoOTA)...");

  // 1. Detener escaneo y desconectar BLE para dedicar la antena 2.4 GHz al Wi-Fi OTA
  NimBLEScan* pScan = NimBLEDevice::getScan();
  if (pScan != nullptr && pScan->isScanning()) {
    pScan->stop();
  }
  if (scaleConnected && pScaleClient != nullptr) {
    pScaleClient->disconnect();
    delay(60);
  }
  scaleConnected = false;

  setRgbColor(LED_BRIGHTNESS, 0, LED_BRIGHTNESS); // Magenta = Modo OTA activo
  renderOtaScreenOled("INICIANDO WIFI AP...", -1);

  // Esperar a que se suelte el botón antes de entrar al bucle OTA
  while (digitalRead(PIN_ENC_SW) == LOW || digitalRead(PIN_BOOT_BUTTON) == LOW) {
    delay(10);
  }

  WiFi.mode(WIFI_AP);
  WiFi.softAP("TIMEMORE-OTA", getOtaPassword()); // WPA2 con clave única por placa -> IP 192.168.4.1
  delay(150);

  WebServer otaServer(80);
  bool otaSuccess = false;
  bool otaWriteError = false;
  int lastPctShown = -1;

  otaServer.on("/", HTTP_GET, [&]() {
    otaServer.send_P(200, "text/html", OTA_HTML_PAGE);
  });

  otaServer.on("/update", HTTP_POST, [&]() {
    bool ok = !otaWriteError && !Update.hasError() && Update.isFinished();
    otaServer.sendHeader("Connection", "close");
    otaServer.send(200, "text/plain", ok ? "OTA OK! Reiniciando ESP32-S3..." : "ERROR EN OTA");
    if (ok) {
      otaSuccess = true;
      renderOtaScreenOled("OTA 100% -> REBOOT!", 100);
      delay(800);
      ESP.restart();
    }
  }, [&]() {
    HTTPUpload& upload = otaServer.upload();
    if (upload.status == UPLOAD_FILE_START) {
      Serial.printf("[OTA-WEB] Recibiendo firmware: %s\n", upload.filename.c_str());
      lastPctShown  = 0;
      otaWriteError = false;
      renderOtaScreenOled("SUBIENDO...", 0);
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        otaWriteError = true;
        Serial.printf("[OTA-WEB] Update.begin fallo: %s\n", Update.errorString());
        renderOtaScreenOled("ERROR: SIN ESPACIO", -1);
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (otaWriteError) return;
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        otaWriteError = true;
        Serial.printf("[OTA-WEB] Update.write fallo: %s\n", Update.errorString());
        Update.abort();
        renderOtaScreenOled("ERROR AL ESCRIBIR!", -1);
        return;
      }
      // Progreso real según el tamaño de la petición (antes se suponía un binario de 660 KB)
      size_t expected = otaServer.clientContentLength();
      if (expected == 0) expected = ESP.getSketchSize();
      int pct = static_cast<int>((static_cast<uint64_t>(upload.totalSize) * 100ULL) / expected);
      if (pct > 99) pct = 99;
      if (pct >= lastPctShown + 4) {
        lastPctShown = pct;
        renderOtaScreenOled("SUBIENDO...", pct);
      }
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
      otaWriteError = true;
      Update.abort();
      renderOtaScreenOled("SUBIDA CANCELADA", -1);
    } else if (upload.status == UPLOAD_FILE_END) {
      if (otaWriteError) {
        renderOtaScreenOled("ERROR EN ARCHIVO!", -1);
      } else if (Update.end(true)) {
        Serial.printf("[OTA-WEB] Firmware escrito exitosamente (%u bytes).\n", (unsigned int)upload.totalSize);
        renderOtaScreenOled("OTA 100% -> REBOOT!", 100);
      } else {
        renderOtaScreenOled("ERROR EN ARCHIVO!", -1);
      }
    }
  });

  otaServer.begin();

  ArduinoOTA.setHostname("timemore-dot");
  ArduinoOTA.setPassword(getOtaPassword());
  ArduinoOTA.onStart([&]() {
    lastPctShown = 0;
    renderOtaScreenOled("ARDUINO OTA...", 0);
  });
  ArduinoOTA.onProgress([&](unsigned int progress, unsigned int total) {
    int pct = (total > 0) ? static_cast<int>((progress * 100U) / total) : 0;
    if (pct >= lastPctShown + 4 || pct == 100) {
      lastPctShown = pct;
      renderOtaScreenOled("ARDUINO OTA...", pct);
    }
  });
  ArduinoOTA.onEnd([&]() {
    renderOtaScreenOled("OTA 100% -> REBOOT!", 100);
  });
  ArduinoOTA.begin();

  renderOtaScreenOled("SUBIR .BIN EN WEB", -1);
  Serial.printf("[OTA] Red Wi-Fi creada: SSID='TIMEMORE-OTA' clave='%s' | Abre http://192.168.4.1 en tu navegador.\n",
                getOtaPassword());

  unsigned long otaStartMs = millis();
  // Permanecer en modo OTA hasta que se actualice, se presione 1 clic para cancelar, o pasen 5 minutos
  while (!otaSuccess && (millis() - otaStartMs < 300000UL)) {
    otaServer.handleClient();
    ArduinoOTA.handle();

    if (digitalRead(PIN_ENC_SW) == LOW || digitalRead(PIN_BOOT_BUTTON) == LOW) {
      delay(30);
      if (digitalRead(PIN_ENC_SW) == LOW || digitalRead(PIN_BOOT_BUTTON) == LOW) {
        while (digitalRead(PIN_ENC_SW) == LOW || digitalRead(PIN_BOOT_BUTTON) == LOW) {
          delay(10);
        }
        break;
      }
    }
    delay(2);
  }

  Serial.println("[OTA] Saliendo de modo OTA -> Reiniciando a modo normal BLE...");
  otaServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  ESP.restart();
}
