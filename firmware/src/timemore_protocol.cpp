#include "timemore_protocol.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace tmproto {

uint16_t crc16(const uint8_t* data, size_t length) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < length; i++) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; bit++) {
      if (crc & 0x0001) {
        crc = (crc >> 1) ^ 0xA001;
      } else {
        crc >>= 1;
      }
    }
  }
  return crc;
}

size_t buildFrame(uint8_t opcode, uint8_t cmd, const uint8_t* payload, uint16_t payloadLen,
                  uint8_t* out, size_t outCap) {
  if (out == nullptr || payloadLen > 32) return 0;
  size_t total = 6 + payloadLen + 2;
  if (outCap < total) return 0;
  out[0] = 0xA5;
  out[1] = 0x5A;
  out[2] = opcode;
  out[3] = cmd;
  out[4] = static_cast<uint8_t>((payloadLen >> 8) & 0xFF);
  out[5] = static_cast<uint8_t>(payloadLen & 0xFF);
  if (payloadLen > 0 && payload != nullptr) {
    memcpy(&out[6], payload, payloadLen);
  }
  uint16_t crc = crc16(out, 6 + payloadLen);
  out[6 + payloadLen]     = static_cast<uint8_t>((crc >> 8) & 0xFF);
  out[6 + payloadLen + 1] = static_cast<uint8_t>(crc & 0xFF);
  return total;
}

ParseResult parsePayload(const uint8_t* data, size_t len) {
  ParseResult r;
  if (data == nullptr || len < 4) return r;

  // CASO A: Timemore Open Scale Protocol v1.0.3 (A5 5A) sobre 0xFFF0 -> 0xFFF1 (Timemore Dot TES017).
  // Recorre todo el paquete (puede traer bytes previos o varias tramas) y se queda con la lectura de
  // peso más reciente.
  if (len >= 8) {
    size_t offset = 0;
    while (offset + 8 <= len) {
      if (data[offset] != 0xA5 || data[offset + 1] != 0x5A) {
        offset++;
        continue;
      }
      uint8_t opcode      = data[offset + 2];
      uint8_t cmd         = data[offset + 3];
      uint16_t payloadLen = static_cast<uint16_t>((static_cast<uint16_t>(data[offset + 4]) << 8) | data[offset + 5]);
      size_t frameTotalLen = 6 + payloadLen + 2;

      if (payloadLen > 32 || offset + frameTotalLen > len) {
        offset++;
        continue;
      }
      r.sawOpenScaleFrame = true;
      if (r.frameCount < 255) r.frameCount++;
      if (cmd == 0x08) r.sawProbeReply = true;

      // Tramas de confirmación (ACK, opcode 0x03) tras enviar comandos por 0xFFF2
      if (opcode == 0x03) {
        offset += frameTotalLen;
        continue;
      }

      // Solo A5 5A 01 01 es una lectura de peso (DOT_COLA_BLE.md, regla 4)
      if (opcode == 0x01 && cmd == 0x01 && (offset + 10 <= len)) {
        int32_t rawVal = 0;
        if (payloadLen >= 4 && (offset + 12 <= len)) {
          rawVal = (static_cast<int32_t>(static_cast<int8_t>(data[offset + 6])) << 24) |
                   (static_cast<int32_t>(data[offset + 7]) << 16) |
                   (static_cast<int32_t>(data[offset + 8]) << 8) |
                   static_cast<int32_t>(data[offset + 9]);
        } else {
          int16_t raw16 = static_cast<int16_t>((static_cast<uint16_t>(data[offset + 6]) << 8) | data[offset + 7]);
          rawVal = raw16;
        }
        r.grams = static_cast<float>(rawVal) / 10.0f;

        if (payloadLen >= 6 && (offset + 12 <= len)) {
          int16_t rawFlow16 = static_cast<int16_t>((static_cast<uint16_t>(data[offset + 10]) << 8) | data[offset + 11]);
          r.nativeFlowGps = static_cast<float>(rawFlow16) / 10.0f;
          r.hasNativeFlow = true;
        }

        if (payloadLen >= 9) {
          r.stable = (data[offset + 14] == 0x00);  // byte de sobrecarga
        } else {
          r.stable = true;
        }
        r.hasWeight = true;
      } else if (cmd == 0x05 && payloadLen >= 1) {
        // Batería (sección 5.3 del protocolo v1.0.3): A5 5A [01|02] 05 00 02 [Barras] [Porcentaje] [CRC]
        uint8_t b0   = data[offset + 6];
        uint8_t b1   = (payloadLen >= 2) ? data[offset + 7] : b0;
        uint8_t bars = (payloadLen >= 2) ? b0 : 0;
        uint8_t batt = (b1 > 0 && b1 <= 100) ? b1 : ((b0 > 4 && b0 <= 100) ? b0 : b1);
        if (batt <= 100) {
          r.hasBattery       = true;
          r.batteryPct       = batt;
          r.batteryBars      = bars;
          r.batteryBarsValid = true;
        }
      }

      offset += frameTotalLen;
    }
    // Tramas Open Scale válidas sin peso (ACK, batería...): no seguir con las heurísticas. Antes una
    // trama de 9 bytes (ACK con 1 byte de payload) acababa en el CASO C1 y se leía como ~328 g.
    if (r.sawOpenScaleFrame) return r;
  }

  // CASO B: Protocolo UART 0xFFE0 / 0xFFE1 con cabecera 0xAA o 0x55
  if (data[0] == 0xAA || data[0] == 0x55) {
    if (len >= 7 && ((data[0] == 0xAA && data[1] == 0x55) || (data[0] == 0x55 && data[1] == 0xAA))) {
      uint8_t statusByte = data[3];
      bool isNeg = ((statusByte & 0x80) != 0) || (statusByte == 0x01) || (statusByte == '-');
      r.stable = ((statusByte & 0x40) == 0);
      int16_t rawMag = static_cast<int16_t>((static_cast<uint16_t>(data[4]) << 8) | data[5]);
      if (rawMag < 0) {
        r.grams = static_cast<float>(rawMag) / 10.0f;
      } else {
        r.grams = (isNeg ? -1.0f : 1.0f) * (static_cast<float>(rawMag) / 10.0f);
      }
      if (len >= 8 && data[6] > 0 && data[6] <= 100) {
        r.hasBattery = true;
        r.batteryPct = data[6];
      }
      r.hasWeight = true;
      return r;
    }

    if (len >= 5) {
      uint8_t signByte = data[1];
      bool isNeg = ((signByte & 0x80) != 0) || (signByte == 0x01) || (signByte == '-');
      int16_t raw16 = static_cast<int16_t>((static_cast<uint16_t>(data[2]) << 8) | data[3]);
      if (raw16 < 0) {
        r.grams = static_cast<float>(raw16) / 10.0f;
      } else {
        r.grams = (isNeg ? -1.0f : 1.0f) * (static_cast<float>(raw16) / 10.0f);
      }
      r.stable = true;
      if (len >= 6 && data[4] > 0 && data[4] <= 100) {
        r.hasBattery = true;
        r.batteryPct = data[4];
      }
      r.hasWeight = true;
      return r;
    }
  }

  // CASO C1: 0x181A -> 0x2A6D (9 bytes, int32 LE en miligramos)
  if (len == 9) {
    int32_t weightMg = static_cast<int32_t>(
        static_cast<uint32_t>(data[1]) |
        (static_cast<uint32_t>(data[2]) << 8) |
        (static_cast<uint32_t>(data[3]) << 16) |
        (static_cast<uint32_t>(data[4]) << 24));
    float grams = static_cast<float>(weightMg) / 1000.0f;
    if (!isnan(grams) && fabsf(grams) <= 5000.0f) {
      r.grams     = roundf(grams * 10.0f) / 10.0f;
      r.stable    = (data[0] == 0x00);
      r.hasWeight = true;
      return r;
    }
  }

  // CASO C2: 0x181D -> 0x2A9D (4 a 6 bytes, int16 LE en décimas de gramo)
  if (len >= 4 && len <= 6 && data[0] <= 0x1F) {
    int16_t rawLe16 = static_cast<int16_t>(static_cast<uint16_t>(data[1]) |
                                          (static_cast<uint16_t>(data[2]) << 8));
    if (rawLe16 >= -20000) {  // el máximo de int16 (3276.7 g) ya está dentro del rango útil
      r.grams     = static_cast<float>(rawLe16) / 10.0f;
      r.stable    = true;
      r.hasWeight = true;
      return r;
    }
  }

  // CASO D: Trama ASCII en UART transparente
  bool looksAscii = true;
  bool hasDigit   = false;
  for (size_t i = 0; i < len; i++) {
    char c = static_cast<char>(data[i]);
    if (c >= '0' && c <= '9') hasDigit = true;
    if (c != ' ' && c != '+' && c != '-' && c != '.' &&
        c != 'g' && c != 'G' && c != '\r' && c != '\n' && c != '\0' &&
        !(c >= '0' && c <= '9')) {
      looksAscii = false;
      break;
    }
  }
  if (looksAscii && hasDigit) {
    char buf[32];
    size_t copyLen = (len < sizeof(buf) - 1) ? len : (sizeof(buf) - 1);
    bool neg = false;
    size_t outIdx = 0;
    for (size_t i = 0; i < copyLen; i++) {
      char c = static_cast<char>(data[i]);
      if (c == '-') neg = true;
      if ((c >= '0' && c <= '9') || c == '.') buf[outIdx++] = c;
    }
    buf[outIdx] = '\0';
    if (outIdx > 0) {
      float val   = static_cast<float>(atof(buf));
      r.grams     = neg ? -val : val;
      r.stable    = true;
      r.hasWeight = true;
      return r;
    }
  }

  return r;
}

}  // namespace tmproto
