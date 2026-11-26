// ============================================================================
// PANTALLA OLED 128x64 SSD1309 (I2C)
// Driver propio: inicialización, primitivas de dibujo, fuente 5x7 y volcado por páginas.
// Pestaña del sketch: el IDE la concatena tras firmware.ino (orden alfabético).
// Tipos, constantes y variables globales compartidas están en la pestaña principal.
// ============================================================================

// ============================================================================
// DRIVER GRÁFICO NATIVO PARA OLED 128x64 I2C (SSD1306 / SH1106)
// ============================================================================
static uint8_t oledAddr = 0x3C;
static bool oledPresent = false;
static uint8_t oledBuffer[128 * 8]; // 1024 bytes (128x64 monocromo)

// Fuente ASCII 5x7 compacta (caracteres 32..95: espacio hasta '_')
static const uint8_t FONT_5X7[][5] PROGMEM = {
  {0x00,0x00,0x00,0x00,0x00}, // ' ' (32)
  {0x00,0x00,0x5F,0x00,0x00}, // '!' (33)
  {0x00,0x07,0x00,0x07,0x00}, // '"' (34)
  {0x14,0x7F,0x14,0x7F,0x14}, // '#' (35)
  {0x24,0x2A,0x7F,0x2A,0x12}, // '$' (36)
  {0x23,0x13,0x08,0x64,0x62}, // '%' (37)
  {0x36,0x49,0x55,0x22,0x50}, // '&' (38)
  {0x00,0x05,0x03,0x00,0x00}, // ''' (39)
  {0x00,0x1C,0x22,0x41,0x00}, // '(' (40)
  {0x00,0x41,0x22,0x1C,0x00}, // ')' (41)
  {0x14,0x08,0x3E,0x08,0x14}, // '*' (42)
  {0x08,0x08,0x3E,0x08,0x08}, // '+' (43)
  {0x00,0x50,0x30,0x00,0x00}, // ',' (44)
  {0x08,0x08,0x08,0x08,0x08}, // '-' (45)
  {0x00,0x60,0x60,0x00,0x00}, // '.' (46)
  {0x20,0x10,0x08,0x04,0x02}, // '/' (47)
  {0x3E,0x51,0x49,0x45,0x3E}, // '0' (48)
  {0x00,0x42,0x7F,0x40,0x00}, // '1' (49)
  {0x42,0x61,0x51,0x49,0x46}, // '2' (50)
  {0x21,0x41,0x45,0x4B,0x31}, // '3' (51)
  {0x18,0x14,0x12,0x7F,0x10}, // '4' (52)
  {0x27,0x45,0x45,0x45,0x39}, // '5' (53)
  {0x3C,0x4A,0x49,0x49,0x30}, // '6' (54)
  {0x01,0x71,0x09,0x05,0x03}, // '7' (55)
  {0x36,0x49,0x49,0x49,0x36}, // '8' (56)
  {0x06,0x49,0x49,0x29,0x1E}, // '9' (57)
  {0x00,0x36,0x36,0x00,0x00}, // ':' (58)
  {0x00,0x56,0x36,0x00,0x00}, // ';' (59)
  {0x08,0x14,0x22,0x41,0x00}, // '<' (60)
  {0x14,0x14,0x14,0x14,0x14}, // '=' (61)
  {0x00,0x41,0x22,0x14,0x08}, // '>' (62)
  {0x02,0x01,0x51,0x09,0x06}, // '?' (63)
  {0x32,0x49,0x79,0x41,0x3E}, // '@' (64)
  {0x7E,0x11,0x11,0x11,0x7E}, // 'A' (65)
  {0x7F,0x49,0x49,0x49,0x36}, // 'B' (66)
  {0x3E,0x41,0x41,0x41,0x22}, // 'C' (67)
  {0x7F,0x41,0x41,0x22,0x1C}, // 'D' (68)
  {0x7F,0x49,0x49,0x49,0x41}, // 'E' (69)
  {0x7F,0x09,0x09,0x09,0x01}, // 'F' (70)
  {0x3E,0x41,0x49,0x49,0x7A}, // 'G' (71)
  {0x7F,0x08,0x08,0x08,0x7F}, // 'H' (72)
  {0x00,0x41,0x7F,0x41,0x00}, // 'I' (73)
  {0x20,0x40,0x41,0x3F,0x01}, // 'J' (74)
  {0x7F,0x08,0x14,0x22,0x41}, // 'K' (75)
  {0x7F,0x40,0x40,0x40,0x40}, // 'L' (76)
  {0x7F,0x02,0x0C,0x02,0x7F}, // 'M' (77)
  {0x7F,0x04,0x08,0x10,0x7F}, // 'N' (78)
  {0x3E,0x41,0x41,0x41,0x3E}, // 'O' (79)
  {0x7F,0x09,0x09,0x09,0x06}, // 'P' (80)
  {0x3E,0x41,0x51,0x21,0x5E}, // 'Q' (81)
  {0x7F,0x09,0x19,0x29,0x46}, // 'R' (82)
  {0x46,0x49,0x49,0x49,0x31}, // 'S' (83)
  {0x01,0x01,0x7F,0x01,0x01}, // 'T' (84)
  {0x3F,0x40,0x40,0x40,0x3F}, // 'U' (85)
  {0x1F,0x20,0x40,0x20,0x1F}, // 'V' (86)
  {0x3F,0x40,0x38,0x40,0x3F}, // 'W' (87)
  {0x63,0x14,0x08,0x14,0x63}, // 'X' (88)
  {0x07,0x08,0x70,0x08,0x07}, // 'Y' (89)
  {0x61,0x51,0x49,0x45,0x43}, // 'Z' (90)
  {0x00,0x7F,0x41,0x41,0x00}, // '[' (91)
  {0x02,0x04,0x08,0x10,0x20}, // '\' (92)
  {0x00,0x41,0x41,0x7F,0x00}, // ']' (93)
  {0x04,0x02,0x01,0x02,0x04}, // '^' (94)
  {0x40,0x40,0x40,0x40,0x40}  // '_' (95)
};

void oledSendCmd(uint8_t cmd) {
  if (!oledPresent) return;
  Wire.beginTransmission(oledAddr);
  Wire.write(0x00); // Control byte: Command
  Wire.write(cmd);
  Wire.endTransmission();
}

void oledSendCmdList(const uint8_t* cmds, size_t len) {
  if (!oledPresent || cmds == nullptr || len == 0) return;
  Wire.beginTransmission(oledAddr);
  Wire.write(0x00); // Control byte: Command stream (Co=0, D/C#=0)
  Wire.write(cmds, len);
  Wire.endTransmission();
}

void sendSsd1309InitSequence() {
  // Secuencia de inicialización oficial para SSD1309 (M154-4P 1.54" 128x64)
  // Compatible también con SSD1306 / SSD1315
  const uint8_t initSeq[] = {
    0xFD, 0x12, // Command Lock: 0x12 = Unlock OLED driver IC (indispensable en SSD1309)
    0xAE,       // Display OFF (sleep mode)
    0xD5, 0xA0, // Set Display Clock Divide Ratio / Oscillator Frequency (0xA0 óptimo para SSD1309)
    0xA8, 0x3F, // Multiplex Ratio = 1/64 duty (0x3F = 63)
    0xD3, 0x00, // Display Offset = 0
    0x40,       // Set Display Start Line = 0
    0x8D, 0x14, // Charge Pump Enable (para SSD1306/SSD1315; ignorado en SSD1309)
    0x20, 0x02, // Memory Addressing Mode = Page Addressing Mode (0x02)
    0xA1,       // Segment Re-map: column 127 mapped to SEG0
    0xC8,       // COM Output Scan Direction: remapped (COM[N-1] to COM0)
    0xDA, 0x12, // COM Pins Hardware Configuration (Alternative COM pin config)
    0x81, 0xCF, // Contrast Control (0xCF = alto contraste en panel 1.54" SSD1309)
    0xD9, 0x25, // Pre-charge Period (Phase 1 = 5 DCLK, Phase 2 = 2 DCLK para SSD1309)
    0xDB, 0x34, // VCOMH Deselect Level (~0.78 x VCC en SSD1309)
    0xA4,       // Entire Display ON (resume to RAM content)
    0xA6,       // Normal Display (0xA6 = normal, 0xA7 = invertido)
    0xAF        // Display ON
  };

  oledSendCmdList(initSeq, sizeof(initSeq));
}

bool initOledDisplay() {
  // Retardo breve al encender para que el convertidor boost DC-DC del módulo M154-4P (SSD1309) estabilice VCC
  delay(80);

  // Inicializar periférico I2C por hardware del ESP32-S3 (800 kHz Fast-Mode Plus en SDA=GPIO8, SCL=GPIO9)
  // Ampliar buffer I2C a 256 bytes para transferir cada página de 128 bytes en una sola ráfaga I2C (~1.4 ms total)
  Wire.setBufferSize(256);
  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL, OLED_I2C_FREQ_HZ);

  const uint8_t candidateAddrs[2] = { OLED_I2C_ADDR_7BIT, 0x3D };
  bool ackReceived = false;
  oledAddr = OLED_I2C_ADDR_7BIT; // 0x3C (serigrafía 0x78)

  for (uint8_t i = 0; i < 2; i++) {
    Wire.beginTransmission(candidateAddrs[i]);
    if (Wire.endTransmission() == 0) {
      oledAddr    = candidateAddrs[i];
      ackReceived = true;
      break;
    }
  }

  oledPresent = true;
  if (ackReceived) {
    Serial.printf("[OLED] SSD1309 (1.54\" / 2.42\" TZT 40307) detectada con ACK en 0x%02X (serigrafia 0x%02X) | SDA=GPIO%d, SCL=GPIO%d (HW I2C 800kHz Fast-Mode Plus)\n",
                  oledAddr, static_cast<uint8_t>(oledAddr << 1), PIN_OLED_SDA, PIN_OLED_SCL);
  } else {
    Serial.printf("[OLED] Aviso: Sin ACK en 0x%02X (serigrafia 0x%02X) en SDA=GPIO%d, SCL=GPIO%d (HW I2C 800kHz Fast-Mode Plus)\n",
                  oledAddr, static_cast<uint8_t>(oledAddr << 1), PIN_OLED_SDA, PIN_OLED_SCL);
  }

  sendSsd1309InitSequence();
  memset(oledBuffer, 0, sizeof(oledBuffer));
  oledInvalidateShadow(); // la GDDRAM tiene contenido desconocido tras el arranque
  return true;
}

void oledClear() {
  memset(oledBuffer, 0, sizeof(oledBuffer));
}

void oledDrawPixel(int x, int y, bool on = true) {
  if (x < 0 || x >= 128 || y < 0 || y >= 64) return;
  uint16_t idx = static_cast<uint16_t>((y >> 3) * 128 + x);
  uint8_t bitMask = static_cast<uint8_t>(1U << (y & 7));
  if (on) oledBuffer[idx] |= bitMask;
  else    oledBuffer[idx] &= ~bitMask;
}

void oledFillRect(int x, int y, int w, int h, bool on = true) {
  for (int yy = y; yy < y + h; yy++) {
    for (int xx = x; xx < x + w; xx++) {
      oledDrawPixel(xx, yy, on);
    }
  }
}

void oledDrawHLine(int x, int y, int w, bool on = true) {
  for (int xx = x; xx < x + w; xx++) {
    oledDrawPixel(xx, y, on);
  }
}

void oledDrawRect(int x, int y, int w, int h, bool on = true) {
  oledDrawHLine(x, y, w, on);
  oledDrawHLine(x, y + h - 1, w, on);
  for (int yy = y; yy < y + h; yy++) {
    oledDrawPixel(x, yy, on);
    oledDrawPixel(x + w - 1, yy, on);
  }
}

// Minúsculas propias: solo las de las unidades (g, g/s). El resto se dibuja en mayúscula.
static const uint8_t FONT_LOWER_G[5] PROGMEM = {0x0C, 0x52, 0x52, 0x52, 0x3E};   // 'g' (cola en la fila 6)
static const uint8_t FONT_LOWER_S[5] PROGMEM = {0x48, 0x54, 0x54, 0x54, 0x20};   // 's'

void oledDrawChar(int x, int y, char c, uint8_t scale = 1, bool invert = false) {
  const uint8_t* glyph = nullptr;
  if (c == 'g') {
    glyph = FONT_LOWER_G;
  } else if (c == 's') {
    glyph = FONT_LOWER_S;
  } else {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - ('a' - 'A'));
    if (c < 32 || c > 95) c = '?';
    glyph = FONT_5X7[c - 32];
  }

  for (uint8_t col = 0; col < 5; col++) {
    uint8_t line = pgm_read_byte(&glyph[col]);
    for (uint8_t row = 0; row < 7; row++) {
      bool bitOn = ((line >> row) & 0x01) != 0;
      if (invert) bitOn = !bitOn;
      if (scale == 1) {
        oledDrawPixel(x + col, y + row, bitOn);
      } else {
        oledFillRect(x + col * scale, y + row * scale, scale, scale, bitOn);
      }
    }
  }
}

void oledDrawText(int x, int y, const char* str, uint8_t scale = 1, bool invert = false) {
  if (!str) return;
  int curX = x;
  while (*str) {
    oledDrawChar(curX, y, *str, scale, invert);
    curX += 6 * scale;
    str++;
  }
}

// Copia de lo que hay realmente en la GDDRAM de la OLED: oledFlush() solo envía las páginas (8 filas
// de 128 bytes) que cambian. Con el peso y el cronómetro cambiando suelen ser 1-3 de 8 páginas, y un
// frame idéntico no transmite nada (antes cada refresco enviaba ~1 KB = ~12 ms de bus I2C a 800 kHz).
static uint8_t oledShadow[128 * 8];
static bool oledShadowValid       = false;
static uint32_t oledFlushLastUs   = 0;   // duración del último volcado con páginas
static uint32_t oledFlushMaxUs    = 0;   // máximo desde el último [STATUS]
static uint32_t oledPagesSent     = 0;   // páginas enviadas desde el último [STATUS]

void oledInvalidateShadow() {
  oledShadowValid = false;
}

void oledFlush() {
  if (!oledPresent) return;
  const uint8_t colOffset = OLED_IS_SH1106 ? 2 : 0;
  uint32_t t0 = micros();
  uint8_t sentNow = 0;

  for (uint8_t page = 0; page < 8; page++) {
    uint8_t* src = &oledBuffer[page * 128];
    uint8_t* shd = &oledShadow[page * 128];
    if (oledShadowValid && memcmp(src, shd, 128) == 0) continue;
    memcpy(shd, src, 128);
    sentNow++;
    const uint8_t pageCmds[3] = {
      static_cast<uint8_t>(0xB0 | page),
      static_cast<uint8_t>(0x00 | (colOffset & 0x0F)),
      static_cast<uint8_t>(0x10 | ((colOffset >> 4) & 0x0F))
    };
    oledSendCmdList(pageCmds, sizeof(pageCmds));

    // Enviar los 128 bytes de la página en una sola transacción I2C por hardware
    Wire.beginTransmission(oledAddr);
    Wire.write(0x40); // Control byte: Data stream
    Wire.write(src, 128);
    Wire.endTransmission();
  }
  oledShadowValid = true;

  if (sentNow > 0) {
    oledFlushLastUs = micros() - t0;
    if (oledFlushLastUs > oledFlushMaxUs) oledFlushMaxUs = oledFlushLastUs;
    oledPagesSent += sentNow;
  }
}

void oledSetContrast(uint8_t contrast) {
  if (!oledPresent) return;
  const uint8_t cmdPair[2] = { 0x81, contrast };
  oledSendCmdList(cmdPair, sizeof(cmdPair));
}

void oledPowerOff() {
  if (!oledPresent) return;
  oledSendCmd(0xAE); // Display OFF (Sleep mode en SSD1309 < 10 uA)
}

void oledPowerOn() {
  if (!oledPresent) return;
  oledInvalidateShadow(); // por seguridad, el siguiente volcado reescribe la pantalla completa
  oledSendCmd(0xAF); // Display ON
}
