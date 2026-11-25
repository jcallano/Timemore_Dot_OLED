#include "accessory_protocol.h"

#include <math.h>

namespace accproto {

namespace {
int16_t rdI16(const uint8_t* p) { return static_cast<int16_t>(static_cast<uint16_t>(p[0] | (p[1] << 8))); }
uint16_t rdU16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
void wr16(uint8_t* p, uint16_t v) { p[0] = static_cast<uint8_t>(v & 0xFF); p[1] = static_cast<uint8_t>(v >> 8); }
long clampRound(float v, long lo, long hi) {
  long x = lroundf(v);
  return x < lo ? lo : (x > hi ? hi : x);
}
}  // namespace

bool parseAdvert(const uint8_t* d, size_t len, Reading& out) {
  if (d == nullptr || len < ADV_LEN) return false;
  if (rdU16(d) != COMPANY_ID || d[2] != MAGIC0 || d[3] != MAGIC1 || d[4] != VERSION) return false;
  out.type       = d[5];
  out.seq        = d[6];
  out.flags      = d[7];
  out.tempC      = rdI16(d + 8) / 100.0f;
  out.ph         = rdU16(d + 10) / 1000.0f;
  out.phMv       = rdI16(d + 12) / 10.0f;
  out.batteryPct = d[14];
  out.calAgeDays = d[15];
  // Valores imposibles: se marcan como no válidos en lugar de mostrarlos
  if (out.ph > 14.0f) out.flags &= static_cast<uint8_t>(~FLAG_PH_VALID);
  if (out.tempC < -50.0f || out.tempC > 200.0f) out.flags &= static_cast<uint8_t>(~FLAG_TEMP_VALID);
  return true;
}

size_t buildAdvert(const Reading& r, uint8_t* out, size_t cap) {
  if (out == nullptr || cap < ADV_LEN) return 0;
  wr16(out, COMPANY_ID);
  out[2] = MAGIC0;
  out[3] = MAGIC1;
  out[4] = VERSION;
  out[5] = r.type;
  out[6] = r.seq;
  out[7] = r.flags;
  wr16(out + 8, static_cast<uint16_t>(static_cast<int16_t>(clampRound(r.tempC * 100.0f, -32768, 32767))));
  wr16(out + 10, static_cast<uint16_t>(clampRound(r.ph * 1000.0f, 0, 65535)));
  wr16(out + 12, static_cast<uint16_t>(static_cast<int16_t>(clampRound(r.phMv * 10.0f, -32768, 32767))));
  out[14] = r.batteryPct;
  out[15] = r.calAgeDays;
  return ADV_LEN;
}

}  // namespace accproto
