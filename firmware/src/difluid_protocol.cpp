#include "difluid_protocol.h"

#include <string.h>

namespace dfproto {

namespace {
int16_t be16(const uint8_t* p) { return static_cast<int16_t>(static_cast<uint16_t>((p[0] << 8) | p[1])); }
uint32_t be32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | p[3];
}
}  // namespace

uint8_t checksum(const uint8_t* data, size_t len) {
  uint32_t s = 0;
  for (size_t i = 0; i < len; i++) s += data[i];
  return static_cast<uint8_t>(s & 0xFF);
}

size_t buildCmd(uint8_t func, uint8_t cmd, const uint8_t* data, uint8_t len, uint8_t* out, size_t cap) {
  const size_t n = 6u + len;
  if (out == nullptr || cap < n || (len > 0 && data == nullptr)) return 0;
  out[0] = 0xDF;
  out[1] = 0xDF;
  out[2] = func;
  out[3] = cmd;
  out[4] = len;
  if (len > 0) memcpy(out + 5, data, len);
  out[5 + len] = checksum(out, 5u + len);
  return n;
}

Frame parseFrame(const uint8_t* raw, size_t n) {
  Frame f;
  if (raw == nullptr || n < 6 || raw[0] != 0xDF || raw[1] != 0xDF) return f;
  const uint8_t len = raw[4];
  if (len > MAX_FRAME || n < 6u + len) return f;
  if (checksum(raw, 5u + len) != raw[5 + len]) return f;
  f.func = raw[2];
  f.cmd  = raw[3];
  f.len  = len;
  memcpy(f.data, raw + 5, len);
  f.valid = true;
  return f;
}

TestEvent decodeAction(const Frame& f) {
  TestEvent e;
  if (!f.valid || f.func != FUNC_ACTION) return e;
  if (f.cmd == ACT_ERROR) {
    e.kind     = TestEvent::Error;
    e.errClass = f.len > 0 ? f.data[0] : 0;
    e.errCode  = f.len > 1 ? f.data[1] : 0;
    return e;
  }
  if (f.cmd == ACT_UNKNOWN_ERROR) {
    e.kind = TestEvent::Error;
    return e;
  }
  if (f.len < 1) return e;
  const uint8_t pkg = f.data[0];
  const uint8_t* d  = f.data;
  if (pkg == 0 && f.len >= 2) {
    e.kind   = TestEvent::Status;
    e.status = d[1];
  } else if ((pkg == 1 || pkg == 4) && f.len >= 5) {
    // 1: temperaturas de la prueba; 4: media de temperaturas de una prueba "average"
    e.kind    = TestEvent::Temperature;
    e.prismC  = be16(d + 1) / 10.0f;
    e.sampleC = be16(d + 3) / 10.0f;
    const bool fahrenheit = (pkg == 1 && f.len >= 6 && d[5] == 1);
    if (fahrenheit) {
      e.prismC  = (e.prismC - 32.0f) * 5.0f / 9.0f;
      e.sampleC = (e.sampleC - 32.0f) * 5.0f / 9.0f;
    }
  } else if ((pkg == 2 || pkg == 3) && f.len >= 3) {
    e.kind = (pkg == 2) ? TestEvent::Result : TestEvent::AvgResult;
    e.tds  = be16(d + 1) / 100.0f;   // con signo: el agua da valores negativos pequeños (FF F9 = -0.07)
    if (f.len >= 7) e.ri = be32(d + 3) / 100000.0;
  }
  return e;
}

}  // namespace dfproto
