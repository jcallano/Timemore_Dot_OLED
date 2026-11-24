// Protocolo BooKoo Themis (Mini/Ultra): trama de peso de 20 bytes y comandos de 6 bytes.
// Módulo puro (sin Arduino ni NimBLE) para poder probarlo con el autotest ('x' por el monitor serie).
// Referencia: BooKooCode/OpenSource bookoo_ultra_scale/protocols.md y docs/PROTOCOLS.md del puente.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace bkproto {

static constexpr size_t WEIGHT_FRAME_LEN = 20;

// Comandos que envía la app o la máquina por 0xFF12: 03 0A cmd d1 d2 xor
enum : uint8_t {
  CMD_TARE        = 0x01,
  CMD_TIMER_START = 0x04,
  CMD_TIMER_STOP  = 0x05,
  CMD_TIMER_RESET = 0x06,
  CMD_TARE_START  = 0x07,
  CMD_SMOOTHING   = 0x08,
};

struct Command {
  bool valid    = false;  // cabecera 03 0A y al menos 3 bytes
  bool xorOk    = false;  // checksum correcto (se acepta igualmente: hay clientes que no lo calculan)
  uint8_t cmd   = 0;
  uint8_t d1    = 0;
  uint8_t d2    = 0;
};

uint8_t xorSum(const uint8_t* data, size_t len);

// Trama de telemetría 03 0B: cronómetro (ms, uint24), peso (centésimas con signo), caudal (centésimas
// de g/s con signo), batería (%). Devuelve WEIGHT_FRAME_LEN.
size_t buildWeightFrame(uint8_t* out, uint32_t timerMs, float grams, float flowGps, uint8_t batteryPct);

Command parseCommand(const uint8_t* data, size_t len);

// "BOOKOO_SC_U XXXXXX" (18 caracteres + '\0') con los 3 últimos bytes de la MAC BLE.
// La máquina Wendougee solo acepta escalas cuyo nombre empiece por "BOOKOO" y tenga este formato.
void formatAdvName(char* out, size_t cap, const uint8_t mac[6]);

// Bloque de anuncio legacy: flags + UUID 0x0FFE + nombre completo (27 bytes con el nombre de 18).
size_t buildAdvData(uint8_t* out, size_t cap, const char* name);
// Respuesta al escaneo: nombre completo + UUID 0x0FFE.
size_t buildScanResponse(uint8_t* out, size_t cap, const char* name);

}  // namespace bkproto
