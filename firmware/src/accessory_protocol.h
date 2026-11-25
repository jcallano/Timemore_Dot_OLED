// Protocolo de los accesorios BLE propios (sonda de pH y temperatura, y los que vengan).
// Cada accesorio emite sus lecturas en el anuncio BLE (Manufacturer Specific Data, AD 0xFF), sin
// conexión: el display solo escucha. Especificación completa: docs/ACCESORIOS_BLE.md.
// Módulo puro (sin Arduino ni NimBLE): lo comparten el display y el firmware de los accesorios, y lo
// prueba el autotest ('x' por el monitor serie).
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace accproto {

// Manufacturer Specific Data, tal como lo entrega NimBLE (getManufacturerData), con el Company ID:
//   [0..1]  Company ID 0xFFFF (reservado para pruebas por el Bluetooth SIG), little-endian
//   [2..3]  'T' 'D'  (TMDot)
//   [4]     versión del formato (1)
//   [5]     tipo de accesorio (TYPE_*)
//   [6]     secuencia: sube con cada medida nueva (el display descarta los anuncios repetidos)
//   [7]     flags (FLAG_*)
//   [8..9]  temperatura, int16 LE, centésimas de °C
//   [10..11] pH, uint16 LE, milésimas
//   [12..13] tensión de la sonda de pH, int16 LE, décimas de mV (para diagnóstico)
//   [14]    batería del accesorio, % (0xFF = desconocida)
//   [15]    días desde la última calibración del pH (0xFF = nunca o desconocido)
static constexpr size_t   ADV_LEN     = 16;
static constexpr uint16_t COMPANY_ID  = 0xFFFF;
static constexpr uint8_t  MAGIC0      = 'T';
static constexpr uint8_t  MAGIC1      = 'D';
static constexpr uint8_t  VERSION     = 1;

enum : uint8_t {
  TYPE_PH_TEMP = 1,   // sonda de pH + PT100 (MAX31865)
};

enum : uint8_t {
  FLAG_TEMP_VALID = 0x01,
  FLAG_PH_VALID   = 0x02,
  FLAG_PH_CAL     = 0x04,   // el pH sale de una calibración (sin ella es una estimación de fábrica)
  FLAG_STABLE     = 0x08,   // la lectura dejó de moverse (se puede guardar)
  FLAG_CAL_DUE    = 0x10,   // toca calibrar
  FLAG_LOW_BATT   = 0x20,
};

struct Reading {
  uint8_t type        = 0;
  uint8_t seq         = 0;
  uint8_t flags       = 0;
  float   tempC       = 0.0f;
  float   ph          = 0.0f;
  float   phMv        = 0.0f;
  uint8_t batteryPct  = 0xFF;
  uint8_t calAgeDays  = 0xFF;

  bool tempValid() const { return (flags & FLAG_TEMP_VALID) != 0; }
  bool phValid() const { return (flags & FLAG_PH_VALID) != 0; }
};

// Devuelve false si no es un anuncio de un accesorio (otro Company ID, otra firma, otra versión o corto).
bool parseAdvert(const uint8_t* data, size_t len, Reading& out);

// Para el firmware del accesorio (y el autotest). Devuelve ADV_LEN, o 0 si no cabe.
size_t buildAdvert(const Reading& r, uint8_t* out, size_t cap);

}  // namespace accproto
