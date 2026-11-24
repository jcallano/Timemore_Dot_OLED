#include "selftest.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "bookoo_protocol.h"
#include "flow_engine.h"
#include "timemore_protocol.h"

namespace {

void (*g_print)(const char*) = nullptr;
int g_fail = 0;
int g_pass = 0;

void check(bool ok, const char* name, const char* detailFmt = nullptr, float a = 0.0f, float b = 0.0f) {
  char line[160];
  if (ok) {
    g_pass++;
    snprintf(line, sizeof(line), "  OK    %s", name);
  } else {
    g_fail++;
    if (detailFmt != nullptr) {
      char detail[80];
      snprintf(detail, sizeof(detail), detailFmt, a, b);
      snprintf(line, sizeof(line), "  FALLO %s (%s)", name, detail);
    } else {
      snprintf(line, sizeof(line), "  FALLO %s", name);
    }
  }
  g_print(line);
}

bool near(float x, float y, float tol) { return fabsf(x - y) <= tol; }

// Trama Open Scale de peso: int32 big-endian en décimas de gramo (+ caudal int16 y byte de estado)
size_t weightFrame(uint8_t* out, size_t cap, int32_t tenths, int16_t flowTenths = 0, uint8_t overload = 0) {
  uint8_t p[9] = {
      static_cast<uint8_t>((tenths >> 24) & 0xFF), static_cast<uint8_t>((tenths >> 16) & 0xFF),
      static_cast<uint8_t>((tenths >> 8) & 0xFF),  static_cast<uint8_t>(tenths & 0xFF),
      static_cast<uint8_t>((flowTenths >> 8) & 0xFF), static_cast<uint8_t>(flowTenths & 0xFF),
      0, 0, overload};
  return tmproto::buildFrame(0x01, 0x01, p, sizeof(p), out, cap);
}

void testProtocol() {
  g_print("[TEST] Protocolo Timemore (src/timemore_protocol)");
  uint8_t buf[64];

  // CRC conocido del protocolo Modbus/Open Scale: "123456789" -> 0x4B37
  const uint8_t ref[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  check(tmproto::crc16(ref, sizeof(ref)) == 0x4B37, "CRC16 (0xA001, 0xFFFF) vector de referencia");

  // Petición de batería: A5 5A 02 05 00 00 + CRC
  size_t n = tmproto::buildFrame(0x02, 0x05, nullptr, 0, buf, sizeof(buf));
  check(n == 8 && buf[0] == 0xA5 && buf[1] == 0x5A && buf[2] == 0x02 && buf[3] == 0x05,
        "buildFrame: peticion de bateria de 8 bytes");
  uint16_t crc = tmproto::crc16(buf, 6);
  check(buf[6] == (crc >> 8) && buf[7] == (crc & 0xFF), "buildFrame: CRC big-endian al final");
  check(tmproto::buildFrame(0x02, 0x05, buf, 40, buf, sizeof(buf)) == 0, "buildFrame: rechaza payload > 32");

  // Peso positivo y negativo
  n = weightFrame(buf, sizeof(buf), 1234);
  tmproto::ParseResult r = tmproto::parsePayload(buf, n);
  check(r.hasWeight && near(r.grams, 123.4f, 0.01f), "Peso Open Scale 123.4 g", "obtenido %.2f", r.grams);
  check(r.sawOpenScaleFrame && r.stable, "Peso Open Scale: trama valida y estable");
  n = weightFrame(buf, sizeof(buf), -52);
  r = tmproto::parsePayload(buf, n);
  check(r.hasWeight && near(r.grams, -5.2f, 0.01f), "Peso Open Scale negativo -5.2 g", "obtenido %.2f", r.grams);

  // Caudal nativo y sobrecarga
  n = weightFrame(buf, sizeof(buf), 2000, 25, 1);
  r = tmproto::parsePayload(buf, n);
  check(r.hasNativeFlow && near(r.nativeFlowGps, 2.5f, 0.01f), "Caudal nativo 2.5 g/s", "obtenido %.2f", r.nativeFlowGps);
  check(!r.stable, "Byte de sobrecarga != 0 -> no estable");

  // Dos tramas en el mismo paquete: gana la última
  size_t n1 = weightFrame(buf, sizeof(buf), 500);
  size_t n2 = weightFrame(buf + n1, sizeof(buf) - n1, 512);
  r = tmproto::parsePayload(buf, n1 + n2);
  check(r.hasWeight && near(r.grams, 51.2f, 0.01f), "Paquete con 2 tramas -> la mas reciente (51.2 g)", "obtenido %.2f", r.grams);
  check(r.frameCount == 2, "Paquete con 2 tramas -> frameCount = 2", "obtenido %.0f", r.frameCount);

  // Cola de la Dot con MTU grande: respuesta a la sonda (02 08) + aviso de bateria + peso en un paquete
  const uint8_t battMix[2] = {3, 80};
  size_t m1 = tmproto::buildFrame(0x01, 0x08, nullptr, 0, buf, sizeof(buf));
  size_t m2 = tmproto::buildFrame(0x01, 0x05, battMix, 2, buf + m1, sizeof(buf) - m1);
  size_t m3 = weightFrame(buf + m1 + m2, sizeof(buf) - m1 - m2, 630);
  r = tmproto::parsePayload(buf, m1 + m2 + m3);
  check(r.frameCount == 3 && r.sawProbeReply && r.hasBattery && r.batteryPct == 80 &&
        r.hasWeight && near(r.grams, 63.0f, 0.01f),
        "Paquete juntado (sonda+bateria+peso): 3 tramas, 63.0 g", "tramas %.0f, %.2f g", r.frameCount, r.grams);

  // Solo 01 01 es peso: una trama 02 01 no debe leerse como peso
  uint8_t p21[9] = {0, 0, 0x04, 0xD2, 0, 0, 0, 0, 0};
  n = tmproto::buildFrame(0x02, 0x01, p21, sizeof(p21), buf, sizeof(buf));
  r = tmproto::parsePayload(buf, n);
  check(!r.hasWeight && r.sawOpenScaleFrame, "Trama 02 01 NO se lee como peso (solo 01 01)");

  // Bytes basura delante de la trama
  uint8_t pre[64] = {0x00, 0x11, 0x22};
  n = weightFrame(pre + 3, sizeof(pre) - 3, 184);
  r = tmproto::parsePayload(pre, n + 3);
  check(r.hasWeight && near(r.grams, 18.4f, 0.01f), "Trama con bytes previos (18.4 g)", "obtenido %.2f", r.grams);

  // Trama truncada: ni peso ni trama válida
  n = weightFrame(buf, sizeof(buf), 1234);
  r = tmproto::parsePayload(buf, n - 3);
  check(!r.hasWeight && !r.sawOpenScaleFrame, "Trama truncada -> descartada");

  // Batería: A5 5A 01 05 00 02 [barras] [%]
  const uint8_t batt[2] = {3, 76};
  n = tmproto::buildFrame(0x01, 0x05, batt, 2, buf, sizeof(buf));
  r = tmproto::parsePayload(buf, n);
  check(r.hasBattery && r.batteryPct == 76 && r.batteryBars == 3 && !r.hasWeight,
        "Bateria 76 % / 3 barras, sin peso");

  // REGRESIÓN (auditoría M7): ACK de 9 bytes (opcode 0x03, 1 byte de payload). Antes caía en el
  // caso C1 y se leía como ~328 g.
  const uint8_t one[1] = {0x00};
  n = tmproto::buildFrame(0x03, 0x05, one, 1, buf, sizeof(buf));
  r = tmproto::parsePayload(buf, n);
  check(n == 9 && !r.hasWeight && r.sawOpenScaleFrame, "ACK de 9 bytes NO se lee como peso (regresion M7)");

  // Otros formatos
  const uint8_t c1[9] = {0x00, 0xE0, 0x47, 0x00, 0x00, 0, 0, 0, 0};  // 18400 mg LE
  r = tmproto::parsePayload(c1, sizeof(c1));
  check(r.hasWeight && near(r.grams, 18.4f, 0.01f), "0x2A6D: int32 LE en mg (18.4 g)", "obtenido %.2f", r.grams);
  const char ascii[] = "  18.4 g";
  r = tmproto::parsePayload(reinterpret_cast<const uint8_t*>(ascii), strlen(ascii));
  check(r.hasWeight && near(r.grams, 18.4f, 0.01f), "ASCII \"  18.4 g\"", "obtenido %.2f", r.grams);
  const uint8_t uart[8] = {0xAA, 0x55, 0x00, 0x00, 0x00, 0xB8, 0x50, 0x00};  // 18.4 g, bateria 80 %
  r = tmproto::parsePayload(uart, sizeof(uart));
  check(r.hasWeight && near(r.grams, 18.4f, 0.01f) && r.hasBattery && r.batteryPct == 80,
        "UART AA 55: 18.4 g y bateria 80 %");
}

// Simula la Dot: paquetes cada 100 ms, peso con resolución 0.1 g que el ADC actualiza cada 200 ms.
struct PourSim {
  FlowEngine fe;
  unsigned long t = 1000;
  float trueWeight = 0.0f;
  float reported = 0.0f;
  int tick = 0;

  void run(float gps, int ms, bool trackPeak = false, float noise = 0.0f) {
    for (int i = 0; i < ms / 100; i++) {
      trueWeight += gps * 0.1f;
      if ((tick++ % 2) == 0) {
        float n = (noise > 0.0f) ? (((tick / 2) % 2) ? noise : -noise) : 0.0f;
        reported = roundf((trueWeight + n) * 10.0f) / 10.0f;
      }
      fe.addSample(t, reported, trackPeak);
      t += 100;
    }
  }
  void jump(float grams) {
    trueWeight += grams;
    reported = roundf(trueWeight * 10.0f) / 10.0f;
    fe.addSample(t, reported, false);
    t += 100;
  }
};

void testFlowEngine() {
  g_print("[TEST] Motor de caudal (src/flow_engine)");

  PourSim s;
  s.run(0.0f, 1000);
  check(s.fe.currentGps == 0.0f && s.fe.stable, "Reposo -> 0 g/s y estable");

  s.run(5.0f, 3000, true);
  check(near(s.fe.currentGps, 5.0f, 0.8f), "Vertido constante 5 g/s -> ~5 g/s", "obtenido %.2f", s.fe.currentGps);
  check(near(s.fe.displayedGps, 5.0f, 0.8f), "Valor mostrado ~5 g/s", "obtenido %.2f", s.fe.displayedGps);
  check(!s.fe.stable, "Durante el vertido -> no estable");
  check(s.fe.peakGps >= 4.5f, "Pico registrado con trackPeak", "pico %.2f", s.fe.peakGps);

  s.run(0.0f, 800);
  check(s.fe.currentGps == 0.0f && s.fe.displayedGps == 0.0f, "Cortar el chorro -> 0 g/s en < 0.8 s",
        "obtenido %.2f", s.fe.currentGps);

  PourSim slow;
  slow.run(0.0f, 1000);
  slow.run(1.5f, 4000);
  check(near(slow.fe.currentGps, 1.5f, 0.5f), "Vertido lento 1.5 g/s -> ~1.5 g/s", "obtenido %.2f", slow.fe.currentGps);

  PourSim jitter;
  jitter.run(0.0f, 1000);
  jitter.run(0.0f, 3000, false, 0.1f);
  check(jitter.fe.currentGps == 0.0f, "Oscilacion +-0.1 g en reposo -> 0 g/s", "obtenido %.2f", jitter.fe.currentGps);

  PourSim carafe;
  carafe.run(0.0f, 1000);
  carafe.jump(300.0f);
  check(carafe.fe.currentGps == 0.0f && carafe.fe.lastStepMs == 0,
        "Colocar la jarra (+300 g) -> sin caudal y sin disparar auto-timer");
  carafe.run(4.0f, 2000);
  check(near(carafe.fe.currentGps, 4.0f, 0.8f), "Vertido tras colocar la jarra -> ~4 g/s", "obtenido %.2f", carafe.fe.currentGps);

  PourSim nopeak;
  nopeak.run(0.0f, 1000);
  nopeak.run(5.0f, 2000, false);
  check(nopeak.fe.peakGps == 0.0f, "Sin trackPeak (cronometro parado) no registra pico");

  PourSim stale;
  stale.run(0.0f, 1000);
  stale.run(5.0f, 2000);
  unsigned long lastRx = stale.t - 100;
  for (int i = 1; i <= 20; i++) stale.fe.decayIfStale(lastRx + 450 + i * 100, lastRx);
  check(stale.fe.currentGps == 0.0f, "Sin paquetes BLE -> el caudal decae a 0", "obtenido %.2f", stale.fe.currentGps);
}

void testBookoo() {
  uint8_t f[bkproto::WEIGHT_FRAME_LEN];
  // Trama inicial del puente (0.00 g, batería 90 %): vector conocido con XOR 0x37
  const uint8_t ref0[20] = {0x03, 0x0B, 0x00, 0x00, 0x00, 0x01, 0x2B, 0x00, 0x00, 0x00,
                            0x2B, 0x00, 0x00, 0x5A, 0x00, 0x64, 0x01, 0x01, 0x00, 0x37};
  size_t n = bkproto::buildWeightFrame(f, 0, 0.0f, 0.0f, 90);
  check(n == 20 && memcmp(f, ref0, 20) == 0, "BooKoo: trama 0 g identica a la del puente");

  bkproto::buildWeightFrame(f, 12345, 36.27f, 2.15f, 64);
  uint32_t cg = (static_cast<uint32_t>(f[7]) << 16) | (static_cast<uint32_t>(f[8]) << 8) | f[9];
  uint32_t ms = (static_cast<uint32_t>(f[2]) << 16) | (static_cast<uint32_t>(f[3]) << 8) | f[4];
  uint16_t cf = static_cast<uint16_t>((f[11] << 8) | f[12]);
  check(f[6] == 0x2B && cg == 3627, "BooKoo: peso +36.27 g -> 3627 centesimas", "obtenido %.0f", static_cast<float>(cg));
  check(ms == 12345 && cf == 215 && f[13] == 64, "BooKoo: cronometro, caudal y bateria");
  check(f[19] == bkproto::xorSum(f, 19), "BooKoo: XOR de los bytes 0..18");

  bkproto::buildWeightFrame(f, 0, -1.5f, -0.3f, 50);
  cg = (static_cast<uint32_t>(f[7]) << 16) | (static_cast<uint32_t>(f[8]) << 8) | f[9];
  check(f[6] == 0x2D && cg == 150 && f[10] == 0x2D, "BooKoo: peso y caudal negativos con signo '-'");
  bkproto::buildWeightFrame(f, 0, -0.001f, 0.0f, 50);
  check(f[6] == 0x2B, "BooKoo: -0.001 g se envia como +0.00");

  const uint8_t tare[6] = {0x03, 0x0A, 0x01, 0x00, 0x00, 0x08};
  bkproto::Command c = bkproto::parseCommand(tare, sizeof(tare));
  check(c.valid && c.xorOk && c.cmd == bkproto::CMD_TARE, "BooKoo: comando tara 03 0A 01 00 00 08");
  const uint8_t tareStart[6] = {0x03, 0x0A, 0x07, 0x00, 0x00, 0x0E};
  c = bkproto::parseCommand(tareStart, sizeof(tareStart));
  check(c.valid && c.xorOk && c.cmd == bkproto::CMD_TARE_START, "BooKoo: comando tara+start");
  const uint8_t junk[3] = {0x02, 0x00, 0x00};
  check(!bkproto::parseCommand(junk, sizeof(junk)).valid, "BooKoo: trama ajena ignorada");

  const uint8_t mac[6] = {0xA4, 0x6D, 0x33, 0x89, 0x60, 0x58};
  char name[24];
  bkproto::formatAdvName(name, sizeof(name), mac);
  check(strcmp(name, "BOOKOO_SC_U 896058") == 0, "BooKoo: nombre de anuncio de 18 caracteres");
  uint8_t adv[31];
  size_t an = bkproto::buildAdvData(adv, sizeof(adv), name);
  check(an == 27 && adv[3] == 0x03 && adv[5] == 0xFE && adv[6] == 0x0F && adv[7] == 19 && adv[8] == 0x09,
        "BooKoo: anuncio de 27 bytes (flags + 0x0FFE + nombre)");
  check(bkproto::buildScanResponse(adv, sizeof(adv), name) == 24, "BooKoo: respuesta al escaneo de 24 bytes");
}

}  // namespace

int runSelfTests(void (*print)(const char* line)) {
  g_print = print;
  g_fail  = 0;
  g_pass  = 0;
  testProtocol();
  testFlowEngine();
  testBookoo();
  char line[80];
  snprintf(line, sizeof(line), "[TEST] Resultado: %d OK, %d FALLOS", g_pass, g_fail);
  g_print(line);
  return g_fail;
}
