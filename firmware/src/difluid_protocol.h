// Protocolo BLE del refractómetro DiFluid R2 Extract.
// Fuente: documentación oficial de DiFluid (github.com/DiFluid/difluid-sdk-demo, docs/protocolR2.md y
// docs/difluid-protocol.md), y su uso en Beanconqueror (src/classes/devices/difluidR2Refractometer.ts).
// Servicio 0x00FF, característica 0xAA01 (escritura de órdenes y notificaciones de respuesta).
// Trama (órdenes y respuestas): DF DF func cmd len data0..dataN suma   (suma = byte bajo de la suma
// de todos los bytes anteriores). Los números van en big-endian.
// Módulo puro (sin Arduino ni NimBLE) para poder probarlo con el autotest ('x' por el monitor serie).
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace dfproto {

static constexpr uint16_t SERVICE_UUID16 = 0x00FF;
static constexpr uint16_t CHAR_UUID16    = 0xAA01;
static constexpr size_t   MAX_FRAME      = 32;

enum : uint8_t { FUNC_INFO = 0, FUNC_SETTINGS = 1, FUNC_ACTION = 3 };
enum : uint8_t { INFO_SN = 0, INFO_MODEL = 1, INFO_FIRMWARE = 2 };
enum : uint8_t { SET_TEMP_UNIT = 0, SET_AUTO_TEST = 1, SET_BRIGHTNESS = 2, SET_TEST_COUNT = 3 };
enum : uint8_t { ACT_SINGLE = 0, ACT_AVERAGE = 1, ACT_CALIBRATE = 2, ACT_ERROR = 254, ACT_UNKNOWN_ERROR = 255 };

// Estado del aparato (paquete 0 de una prueba)
enum : uint8_t {
  ST_TEST_FINISHED = 0, ST_CAL_FINISHED = 1, ST_AVG_START = 4, ST_AVG_ONGOING = 5, ST_AVG_FINISHED = 6,
  ST_LOOP_START = 7, ST_LOOP_ONGOING = 8, ST_LOOP_FINISHED = 9, ST_AVG_TIMEOUT = 10, ST_TEST_START = 11,
  ST_CAL_START = 12,
};

// Errores generales (orden 254, data0 = 2)
enum : uint8_t { ERR_TEST = 1, ERR_CAL_FAILED = 2, ERR_NO_LIQUID = 3, ERR_BEYOND_RANGE = 4 };

uint8_t checksum(const uint8_t* data, size_t len);

// Construye una orden. Devuelve su longitud (6 + len), o 0 si no cabe.
size_t buildCmd(uint8_t func, uint8_t cmd, const uint8_t* data, uint8_t len, uint8_t* out, size_t cap);

struct Frame {
  bool    valid   = false;   // cabecera, longitud y suma correctas
  uint8_t func    = 0;
  uint8_t cmd     = 0;
  uint8_t len     = 0;
  uint8_t data[MAX_FRAME] = {0};
};

Frame parseFrame(const uint8_t* raw, size_t n);

// Paquetes de una prueba (single o average). data0 = número de paquete.
struct TestEvent {
  enum Kind : uint8_t { None, Status, Temperature, Result, AvgResult, Error } kind = None;
  uint8_t status   = 0;       // Status
  float   prismC   = 0.0f;    // Temperature (convertida a °C si el aparato está en °F)
  float   sampleC  = 0.0f;
  float   tds      = 0.0f;    // Result / AvgResult: concentración en %
  double  ri       = 0.0;     // índice de refracción
  uint8_t errClass = 0;       // Error: 2 general, 3 hardware
  uint8_t errCode  = 0;
};

TestEvent decodeAction(const Frame& f);

}  // namespace dfproto
