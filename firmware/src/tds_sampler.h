// Muestreo inteligente de TDS con el DiFluid R2: encadena pruebas sueltas y decide cuándo la lectura es
// fiable, evitando las dos derivas típicas:
//   - Temperatura: la muestra (sobre todo un espresso a 70-90 °C) tarda en igualarse con el prisma; el
//     TDS se mueve mientras tanto. Se ignoran las pruebas hasta que la muestra y el prisma están a menos
//     de TEMP_GAP_C y la muestra deja de cambiar (< TEMP_STEP_C entre pruebas).
//   - Evaporación: una muestra pequeña pierde agua y el TDS SUBE siempre en la misma dirección. Si las
//     últimas EVAP_RUN pruebas estables suben sin parar y en total más de la tolerancia, es evaporación:
//     se da la primera prueba estable y se marca.
// Resultado normal: la mediana de las CONVERGE_N últimas pruebas estables cuando su rango es menor que la
// tolerancia (0.02 puntos de TDS o el 0.4 % del valor, lo mayor). Si se llega a MAX_TESTS sin converger,
// la mediana de las últimas y la marca de "dudosa".
// Módulo puro (sin Arduino), con autotest.
#pragma once

#include <stdint.h>

namespace tds {

static constexpr uint8_t MAX_TESTS   = 12;
static constexpr uint8_t CONVERGE_N  = 3;
static constexpr uint8_t EVAP_RUN    = 4;
static constexpr float   TEMP_GAP_C  = 1.5f;
static constexpr float   TEMP_STEP_C = 0.4f;

// Marcas del resultado (también se guardan en los 4 bits altos del TDS empaquetado)
enum : uint8_t {
  FLAG_VALID    = 0x1,   // hay resultado
  FLAG_EVAP     = 0x2,   // se detectó evaporación (la muestra debía estar tapada)
  FLAG_DOUBTFUL = 0x4,   // no convergió en MAX_TESTS
  FLAG_TEMP     = 0x8,   // la temperatura no llegó a estabilizarse
};

enum class Phase : uint8_t { Temperature, Converging, Done };

struct Sample {
  float tds;
  float prismC;
  float sampleC;
};

class Sampler {
 public:
  void reset();
  // Añade una prueba y devuelve la fase. Con Phase::Done, result() y flags() tienen el resultado.
  Phase add(const Sample& s);
  Phase phase() const { return phase_; }
  uint8_t count() const { return n_; }
  uint8_t stableCount() const { return n_ - firstStable_; }
  float result() const { return result_; }
  uint8_t flags() const { return flags_; }
  float tolerance(float tds) const;

 private:
  Sample s_[MAX_TESTS] = {};
  uint8_t n_           = 0;
  uint8_t firstStable_ = 0xFF;   // índice de la primera prueba con la temperatura estable
  Phase phase_         = Phase::Temperature;
  float result_        = 0.0f;
  uint8_t flags_       = 0;

  float medianOfLast(uint8_t k, uint8_t from) const;
  void finish(float v, uint8_t extraFlags);
};

// TDS empaquetado en 16 bits para los registros: bits 0-11 centésimas de % (hasta 40.95 %), 12-15 marcas.
// 0 = sin medida.
uint16_t pack(float tds, uint8_t flags);
float unpackTds(uint16_t p);
uint8_t unpackFlags(uint16_t p);

// Extracción (EY, %).
//   Espresso: TDS x bebida / dosis.
//   Pour-over: la báscula pesa el agua vertida, no la bebida; se descuenta la que retiene el café
//   (retentionRatio g de agua por g de café, ~2.0) para estimar la bebida.
float eyEspresso(float tds, float beverageG, float doseG);
float eyPourOver(float tds, float waterG, float doseG, float retentionRatio = 2.0f);

}  // namespace tds
