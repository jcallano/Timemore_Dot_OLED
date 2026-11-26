#include "selftest.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "bookoo_protocol.h"
#include "difluid_protocol.h"
#include "tds_sampler.h"
#include "brew_advisor.h"
#include "taste_model.h"
#include "flow_engine.h"
#include "timemore_protocol.h"
#include "pour_segmenter.h"

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

// Vertidos y toques de la tetera: el peso pasa por el motor de caudal real y luego por el segmentador
struct SegSim {
  FlowEngine fe;
  pourseg::Segmenter seg;
  unsigned long t = 1000;
  float water = 0.0f;      // agua de verdad en la báscula
  float push = 0.0f;       // fuerza del toque (g aparentes, sin agua)
  int tick = 0;
  float reported = 0.0f;
  int accepted = 0, merged = 0, rejected = 0;

  void step(float gps) {
    water += gps * 0.1f;
    if ((tick++ % 2) == 0) reported = roundf((water + push) * 10.0f) / 10.0f;
    fe.addSample(t, reported, true);
    const pourseg::Event e = seg.add(static_cast<uint32_t>(t), reported, fe.currentGps);
    if (e == pourseg::Event::PourAccepted) accepted++;
    if (e == pourseg::Event::PourMerged) merged++;
    if (e == pourseg::Event::TapRejected) rejected++;
    t += 100;
  }
  void run(float gps, int ms) { for (int i = 0; i < ms / 100; i++) step(gps); }
  // Toque: empuja `grams` durante `ms` y suelta
  void tap(float grams, int ms) {
    push = grams;
    run(0.0f, ms);
    push = 0.0f;
  }
};

void testPourSegmenter() {
  g_print("[TEST] Vertidos y toques (src/pour_segmenter)");

  SegSim s;
  s.run(0.0f, 1000);
  s.run(5.0f, 8000);          // bloom: 40 g
  s.run(0.0f, 12000);
  s.tap(12.0f, 300);          // toque corto con el pico de la tetera
  s.run(0.0f, 15000);
  check(s.seg.pours() == 1 && s.seg.taps() == 1, "Bloom + toque corto -> 1 vertido y 1 toque",
        "vertidos %.0f toques %.0f", s.seg.pours(), s.seg.taps());

  s.run(4.0f, 20000);         // vertido 1: 80 g
  s.run(0.0f, 1500);          // la mano se recoloca
  s.run(4.0f, 5000);          // sigue la misma tanda
  s.run(0.0f, 10000);
  check(s.seg.pours() == 2 && s.merged == 1, "Pausa de 1.5 s dentro de una tanda -> se une",
        "vertidos %.0f uniones %.0f", s.seg.pours(), static_cast<float>(s.merged));

  s.tap(8.0f, 1500);          // apoyo largo (1.5 s) sin agua: dura, pero no deja peso
  s.run(0.0f, 5000);
  check(s.seg.pours() == 2 && s.seg.taps() == 2, "Apoyo largo sin agua -> toque, no vertido",
        "vertidos %.0f toques %.0f", s.seg.pours(), s.seg.taps());

  s.run(3.0f, 20000);         // vertido 2: 60 g
  check(s.seg.livePours(s.reported) == 3, "En pleno vertido ya se cuenta en vivo",
        "en vivo %.0f", s.seg.livePours(s.reported));
  s.run(0.0f, 5000);
  check(s.seg.pours() == 3, "Bloom + 2 tandas -> 3 vertidos", "vertidos %.0f", s.seg.pours());

  SegSim slowTap;
  slowTap.run(0.0f, 1000);
  slowTap.run(1.0f, 2000);    // 2 g: un chorrito, no un vertido
  slowTap.run(0.0f, 3000);
  check(slowTap.seg.pours() == 0, "Chorrito de 2 g -> no es vertido", "vertidos %.0f", slowTap.seg.pours());
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

// Vectores de la documentación oficial de DiFluid (docs/protocolR2.md)
void testDifluid() {
  uint8_t c[16];
  size_t n = dfproto::buildCmd(dfproto::FUNC_INFO, dfproto::INFO_SN, nullptr, 0, c, sizeof(c));
  const uint8_t getSn[6] = {0xDF, 0xDF, 0x00, 0x00, 0x00, 0xBE};
  check(n == 6 && memcmp(c, getSn, 6) == 0, "R2: orden Get SN = DF DF 00 00 00 BE");
  const uint8_t unitC = 0;
  n = dfproto::buildCmd(dfproto::FUNC_SETTINGS, dfproto::SET_TEMP_UNIT, &unitC, 1, c, sizeof(c));
  const uint8_t setC[7] = {0xDF, 0xDF, 0x01, 0x00, 0x01, 0x00, 0xC0};
  check(n == 7 && memcmp(c, setC, 7) == 0, "R2: orden unidad de temperatura en C");
  n = dfproto::buildCmd(dfproto::FUNC_ACTION, dfproto::ACT_SINGLE, nullptr, 0, c, sizeof(c));
  const uint8_t single[6] = {0xDF, 0xDF, 0x03, 0x00, 0x00, 0xC1};
  check(n == 6 && memcmp(c, single, 6) == 0, "R2: orden prueba simple = DF DF 03 00 00 C1");

  const uint8_t model[14] = {0xDF, 0xDF, 0x00, 0x01, 0x08, 0x44, 0x46, 0x54, 0x2D, 0x52, 0x31, 0x30, 0x32, 0xB7};
  dfproto::Frame f = dfproto::parseFrame(model, sizeof(model));
  check(f.valid && f.func == 0 && f.cmd == 1 && f.len == 8 && memcmp(f.data, "DFT-R102", 8) == 0,
        "R2: respuesta de modelo DFT-R102");
  uint8_t bad[14];
  memcpy(bad, model, sizeof(bad));
  bad[13] ^= 1;
  check(!dfproto::parseFrame(bad, sizeof(bad)).valid, "R2: suma incorrecta rechazada");

  const uint8_t st[9] = {0xDF, 0xDF, 0x03, 0x00, 0x03, 0x00, 0x0B, 0x00, 0xCF};
  dfproto::TestEvent e = dfproto::decodeAction(dfproto::parseFrame(st, sizeof(st)));
  check(e.kind == dfproto::TestEvent::Status && e.status == dfproto::ST_TEST_START, "R2: estado 'prueba iniciada'");
  const uint8_t temp[12] = {0xDF, 0xDF, 0x03, 0x00, 0x06, 0x01, 0x03, 0x17, 0x03, 0x14, 0x01, 0xFA};
  e = dfproto::decodeAction(dfproto::parseFrame(temp, sizeof(temp)));
  check(e.kind == dfproto::TestEvent::Temperature && near(e.prismC, 26.17f, 0.02f) && near(e.sampleC, 26.0f, 0.02f),
        "R2: temperaturas 79.1/78.8 F -> 26.2/26.0 C", "prisma %.2f", e.prismC);
  const uint8_t res[13] = {0xDF, 0xDF, 0x03, 0x00, 0x07, 0x02, 0x00, 0x4F, 0x00, 0x02, 0x09, 0x3D, 0x61};
  e = dfproto::decodeAction(dfproto::parseFrame(res, sizeof(res)));
  check(e.kind == dfproto::TestEvent::Result && near(e.tds, 0.79f, 0.001f) && e.ri > 1.33436 && e.ri < 1.33438,
        "R2: resultado TDS 0.79 %, IR 1.33437", "TDS %.2f", e.tds);
  const uint8_t neg[13] = {0xDF, 0xDF, 0x03, 0x01, 0x07, 0x02, 0xFF, 0xF9, 0x00, 0x02, 0x08, 0xA6, 0x73};
  e = dfproto::decodeAction(dfproto::parseFrame(neg, sizeof(neg)));
  check(e.kind == dfproto::TestEvent::Result && near(e.tds, -0.07f, 0.001f), "R2: TDS negativo (agua) -0.07 %");
  // Trama real del R2 sin muestra en el prisma
  const uint8_t noLiq[9] = {0xDF, 0xDF, 0x03, 0xFE, 0x03, 0x00, 0x02, 0x03, 0xC7};
  e = dfproto::decodeAction(dfproto::parseFrame(noLiq, sizeof(noLiq)));
  check(e.kind == dfproto::TestEvent::Error && e.errClass == 2 && e.errCode == dfproto::ERR_NO_LIQUID,
        "R2: error real 'sin liquido' (DF DF 03 FE 03 00 02 03 C7)");

}

void testTdsSampler() {
  tds::Sampler s;
  // Espresso caliente: la muestra se enfría hacia el prisma (25 C) y luego tres lecturas estables
  const tds::Sample hot[] = {{9.20f, 25.0f, 60.0f}, {9.45f, 25.2f, 38.0f}, {9.58f, 25.3f, 27.5f},
                             {9.61f, 25.3f, 26.0f}, {9.63f, 25.3f, 25.8f}, {9.62f, 25.3f, 25.7f},
                             {9.62f, 25.3f, 25.6f}};
  s.reset();
  tds::Phase p = tds::Phase::Temperature;
  for (const auto& x : hot) p = s.add(x);
  check(p == tds::Phase::Done && s.flags() == tds::FLAG_VALID && near(s.result(), 9.62f, 0.005f),
        "TDS: espresso caliente -> ignora el enfriamiento, mediana 9.62", "obtenido %.2f", s.result());

  // Evaporación: temperatura estable desde el principio y el TDS sube sin parar
  s.reset();
  float v = 9.50f;
  for (int i = 0; i < 8 && s.phase() != tds::Phase::Done; i++, v += 0.05f) s.add({v, 25.0f, 25.2f});
  check(s.phase() == tds::Phase::Done && (s.flags() & tds::FLAG_EVAP) && near(s.result(), 9.50f, 0.005f),
        "TDS: evaporacion -> primera lectura estable y marca EVAP", "obtenido %.2f", s.result());

  // Filtrado (1.38 %) con ruido pequeño: converge enseguida
  s.reset();
  const float filt[] = {1.37f, 1.39f, 1.38f, 1.38f};
  for (float x : filt) s.add({x, 24.0f, 24.3f});
  check(s.phase() == tds::Phase::Done && near(s.result(), 1.38f, 0.005f) && s.flags() == tds::FLAG_VALID,
        "TDS: filtrado con ruido de +-0.01 converge en 1.38", "obtenido %.2f", s.result());

  // Sin estabilizar nunca la temperatura: resultado dudoso al llegar al máximo
  s.reset();
  for (int i = 0; i < tds::MAX_TESTS; i++) s.add({1.40f + 0.03f * (i % 2), 24.0f, 40.0f - i});
  check(s.phase() == tds::Phase::Done && (s.flags() & tds::FLAG_DOUBTFUL) && (s.flags() & tds::FLAG_TEMP),
        "TDS: temperatura sin estabilizar -> dudosa");

  const uint16_t pk = tds::pack(9.62f, tds::FLAG_VALID | tds::FLAG_EVAP);
  check(near(tds::unpackTds(pk), 9.62f, 0.001f) && tds::unpackFlags(pk) == (tds::FLAG_VALID | tds::FLAG_EVAP) &&
        tds::pack(1.0f, 0) == 0, "TDS: empaquetado en 16 bits con marcas");
  check(near(tds::eyEspresso(9.62f, 36.0f, 18.0f), 19.24f, 0.01f), "EY espresso: 9.62 % x 36 g / 18 g = 19.24 %");
  check(near(tds::eyPourOver(1.38f, 250.0f, 15.0f), 20.24f, 0.01f), "EY pour-over: 1.38 % x (250 - 2x15) / 15 = 20.24 %");
}

void testAdvisor() {
  advisor::Input in;
  in.ey = 23.6f; in.tds = 1.61f; in.grind = 1.5f; in.ratio = 17.0f;
  in.acidBitter = 1; in.strength = 1;
  advisor::Advice a = advisor::advise(in);
  check(!a.liked && !a.inZone && near(a.suggestedGrind, 1.6f, 0.001f) && near(a.suggestedRatio, 17.5f, 0.001f),
        "Consejo: EY 23.6 amargo e intenso -> molienda 1.6 y 1:17.5", "molienda %.2f", a.suggestedGrind);
  in.grindUpCoarser = false;
  a = advisor::advise(in);
  check(near(a.suggestedGrind, 1.4f, 0.001f), "Consejo: molino al reves (subir = mas fino) -> 1.4", "obtenido %.2f", a.suggestedGrind);

  advisor::Input s;
  s.ey = 17.0f; s.tds = 1.20f; s.grind = 2.0f; s.acidBitter = -2; s.hasBloom = false; s.ratio = 16.0f;
  a = advisor::advise(s);
  check(near(a.suggestedGrind, 1.8f, 0.001f) && a.suggestedRatio < 0.0f && a.nLines == 2,
        "Consejo: muy agrio sin bloom -> 2 pasos mas fino y bloom", "molienda %.2f", a.suggestedGrind);

  advisor::Input c;   // agrio con EY alta: no se toca la molienda
  c.ey = 23.5f; c.tds = 1.35f; c.grind = 1.5f; c.acidBitter = -1;
  a = advisor::advise(c);
  check(a.suggestedGrind < 0.0f && a.nLines >= 2, "Consejo: agrio con EY alta -> temperatura, no molienda");

  advisor::Input g;   // gusta fuera de la zona: no cambiar
  g.ey = 23.6f; g.tds = 1.61f; g.grind = 1.5f;
  a = advisor::advise(g);
  check(a.liked && !a.inZone && a.suggestedGrind < 0.0f && a.suggestedRatio < 0.0f,
        "Consejo: si gusta, no se cambia nada aunque este fuera de la zona");

  float tEy = 20.0f, tTds = 1.30f;
  advisor::learnSweetSpot(tEy, tTds, 23.0f, 1.50f);
  check(near(tEy, 20.6f, 0.001f) && near(tTds, 1.34f, 0.001f), "Punto dulce: se mueve un 20 % hacia el cafe que gusto");
}

void testTasteModel() {
  static taste::Model m;   // static: los pesos no caben cómodos en la pila del loop
  m.init();
  taste::Sample s;
  s.ey = 23.6f; s.tds = 1.61f; s.ratio = 17.0f; s.bloom = true;
  float ab, st;
  m.predict(s, &ab, &st);
  check(ab > 0.8f && st > 0.8f, "Red: preentrenada, EY 23.6/TDS 1.61 -> amargo e intenso", "agrio/amargo %.2f", ab);
  float ey, tv;
  m.sweetSpot(false, 16.0f, 20.0f, 1.30f, &ey, &tv);
  check(fabsf(ey - 20.0f) <= 1.0f && fabsf(tv - 1.30f) <= 0.1f, "Red: sin datos, punto dulce en la zona SCA", "EY %.2f", ey);

  // Una persona a la que le gustan las extracciones altas
  taste::Sample hist[6];
  int nh = 0;
  const float likedEy[4] = {22.5f, 22.8f, 22.2f, 22.6f};
  for (int i = 0; i < 4; i++) {
    taste::Sample l;
    l.ey = likedEy[i]; l.tds = 1.42f; l.ratio = 16.5f; l.bloom = true;
    l.acidBitter = 0; l.strength = 0;
    m.learn(l, hist, nh);
    hist[nh++] = l;
  }
  taste::Sample sour;
  sour.ey = 20.0f; sour.tds = 1.30f; sour.ratio = 16.5f; sour.bloom = true; sour.acidBitter = -1;
  m.learn(sour, hist, nh);
  m.sweetSpot(false, 16.5f, 22.0f, 1.40f, &ey, &tv);   // media de los cafés valorados
  check(ey >= 21.5f && m.weights().samplesSeen == 5, "Red: aprende que le gustan extracciones altas", "punto dulce EY %.2f", ey);
  m.predict(sour, &ab, nullptr);
  check(ab < -0.3f, "Red: ahora EY 20 le sabe agrio", "agrio/amargo %.2f", ab);
}

}  // namespace

int runSelfTests(void (*print)(const char* line)) {
  g_print = print;
  g_fail  = 0;
  g_pass  = 0;
  testProtocol();
  testFlowEngine();
  testPourSegmenter();
  testBookoo();
  testDifluid();
  testTdsSampler();
  testAdvisor();
  testTasteModel();
  char line[80];
  snprintf(line, sizeof(line), "[TEST] Resultado: %d OK, %d FALLOS", g_pass, g_fail);
  g_print(line);
  return g_fail;
}
