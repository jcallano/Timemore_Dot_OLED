#include "bookoo_protocol.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace bkproto {

uint8_t xorSum(const uint8_t* data, size_t len) {
  uint8_t x = 0;
  for (size_t i = 0; i < len; i++) x ^= data[i];
  return x;
}

size_t buildWeightFrame(uint8_t* out, uint32_t timerMs, float grams, float flowGps, uint8_t batteryPct) {
  if (isnan(grams)) grams = 0.0f;
  if (isnan(flowGps)) flowGps = 0.0f;

  float absW = fabsf(grams);
  if (absW > 167772.15f) absW = 167772.15f;           // uint24 en centésimas
  float absF = fabsf(flowGps);
  if (absF > 655.35f) absF = 655.35f;                 // uint16 en centésimas
  if (timerMs > 0xFFFFFFUL) timerMs = 0xFFFFFFUL;
  if (batteryPct > 100) batteryPct = 100;

  const uint32_t cg = static_cast<uint32_t>(lroundf(absW * 100.0f));
  const uint16_t cf = static_cast<uint16_t>(lroundf(absF * 100.0f));
  // Signo solo si el valor redondeado no es cero (evita "-0.00")
  const bool negW = (grams < 0.0f) && cg != 0;
  const bool negF = (flowGps < 0.0f) && cf != 0;

  out[0]  = 0x03;
  out[1]  = 0x0B;
  out[2]  = static_cast<uint8_t>((timerMs >> 16) & 0xFF);
  out[3]  = static_cast<uint8_t>((timerMs >> 8) & 0xFF);
  out[4]  = static_cast<uint8_t>(timerMs & 0xFF);
  out[5]  = 0x01;                     // unidad: gramos
  out[6]  = negW ? 0x2D : 0x2B;       // '-' / '+'
  out[7]  = static_cast<uint8_t>((cg >> 16) & 0xFF);
  out[8]  = static_cast<uint8_t>((cg >> 8) & 0xFF);
  out[9]  = static_cast<uint8_t>(cg & 0xFF);
  out[10] = negF ? 0x2D : 0x2B;
  out[11] = static_cast<uint8_t>((cf >> 8) & 0xFF);
  out[12] = static_cast<uint8_t>(cf & 0xFF);
  out[13] = batteryPct;
  out[14] = 0x00;                     // tiempo de standby: 100 min
  out[15] = 0x64;
  out[16] = 0x01;                     // nivel del zumbador
  out[17] = 0x01;                     // suavizado de caudal activado
  out[18] = 0x00;
  out[19] = xorSum(out, 19);
  return WEIGHT_FRAME_LEN;
}

Command parseCommand(const uint8_t* data, size_t len) {
  Command c;
  if (data == nullptr || len < 3 || data[0] != 0x03 || data[1] != 0x0A) return c;
  c.valid = true;
  c.cmd   = data[2];
  c.d1    = (len > 3) ? data[3] : 0;
  c.d2    = (len > 4) ? data[4] : 0;
  c.xorOk = (len >= 6) && (xorSum(data, 5) == data[5]);
  return c;
}

void formatAdvName(char* out, size_t cap, const uint8_t mac[6]) {
  snprintf(out, cap, "BOOKOO_SC_U %02X%02X%02X", mac[3], mac[4], mac[5]);
}

static size_t putName(uint8_t* out, size_t cap, size_t idx, const char* name) {
  size_t n = strlen(name);
  if (idx + 2 + n > cap) n = (cap > idx + 2) ? cap - idx - 2 : 0;
  out[idx++] = static_cast<uint8_t>(1 + n);
  out[idx++] = 0x09;                  // Complete Local Name
  memcpy(&out[idx], name, n);
  return idx + n;
}

static size_t putUuid0FFE(uint8_t* out, size_t cap, size_t idx) {
  if (idx + 4 > cap) return idx;
  out[idx++] = 0x03;
  out[idx++] = 0x03;                  // Complete List of 16-bit UUIDs
  out[idx++] = 0xFE;
  out[idx++] = 0x0F;
  return idx;
}

size_t buildAdvData(uint8_t* out, size_t cap, const char* name) {
  if (cap < 7) return 0;
  size_t idx = 0;
  out[idx++] = 0x02;
  out[idx++] = 0x01;
  out[idx++] = 0x06;                  // LE General Discoverable + BR/EDR no soportado
  idx = putUuid0FFE(out, cap, idx);
  return putName(out, cap, idx, name);
}

size_t buildScanResponse(uint8_t* out, size_t cap, const char* name) {
  size_t idx = putName(out, cap, 0, name);
  return putUuid0FFE(out, cap, idx);
}

}  // namespace bkproto
