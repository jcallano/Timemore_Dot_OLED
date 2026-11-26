// Modelo de gusto personal: una red neuronal pequeña (perceptrón multicapa 6-8-2) que aprende en el
// propio aparato cómo le sabe un café a esta persona a partir de sus números.
//   Entradas (normalizadas): EY, TDS, ratio, bloom, espresso y temperatura del agua (0 si no se sabe).
//   Salidas (tanh, x2): sabor previsto en los dos ejes de "¿cómo sabe?": agrio(-2)..amargo(+2) y
//   aguado(-2)..intenso(+2).
// Arranca preentrenada con la regla de siempre (más EY -> más amargo, más TDS -> más intenso, centrada
// en la zona SCA), así que al principio predice lo mismo que las sugerencias por reglas. Cada valoración
// la entrena (descenso por gradiente, en el S3) con ese café, los anteriores y la regla de fondo con poco
// peso para que no olvide lo básico con pocos datos.
// Usos: describir el gusto en palabras (dónde predice "equilibrado") y decir cuánto acierta.
// Módulo puro (sin Arduino); en el ESP32 los productos escalares usan ESP-DSP (instrucciones SIMD del S3).
#pragma once

#include <stdint.h>

namespace taste {

static constexpr int N_IN  = 6;
static constexpr int N_HID = 8;
static constexpr int N_OUT = 2;

struct Sample {
  float ey      = 0.0f;
  float tds     = 0.0f;
  float ratio   = 0.0f;   // filtro: agua/dosis; espresso: bebida/dosis
  bool  bloom   = false;
  bool  espresso = false;
  float tempC   = 0.0f;   // 0 = desconocida
  int8_t acidBitter = 0;  // etiqueta (-2..2)
  int8_t strength   = 0;
};

struct Weights {
  uint32_t magic;                 // 'TNN1'
  uint16_t samplesSeen;           // valoraciones con las que ha aprendido
  uint16_t reserved;
  float    maeAb;                 // error medio (eje agrio/amargo) al predecir antes de aprender
  float    maeSt;
  float    w1[N_HID][N_IN];
  float    b1[N_HID];
  float    w2[N_OUT][N_HID];
  float    b2[N_OUT];
};

class Model {
 public:
  // Pesos iniciales (deterministas) + preentrenamiento con la regla
  void init();
  bool valid() const { return w_.magic == MAGIC; }
  // Sabor previsto (-2..2 en cada eje)
  void predict(const Sample& s, float* acidBitter, float* strength) const;
  // Aprende de una valoración nueva. history: otras valoraciones guardadas (se repasan).
  void learn(const Sample& s, const Sample* history, int nHistory);
  // Punto donde predice "equilibrado" en los dos ejes, el más cercano a hint (media de los cafés valorados)
  void sweetSpot(bool espresso, float ratio, float hintEy, float hintTds, float* ey, float* tds) const;
  // Pendiente del eje agrio/amargo por cada punto de EY (cuánto le cambia el sabor con la extracción)
  float bitterPerEy(bool espresso, float ey, float tds, float ratio) const;

  Weights& weights() { return w_; }
  const Weights& weights() const { return w_; }
  static constexpr uint32_t MAGIC = 0x314E4E54UL;   // "TNN1"

 private:
  Weights w_ = {};
  void forward(const float* x, float* h, float* y) const;
  void sgd(const Sample& s, float weight, float lr);
};

void encode(const Sample& s, float* x);

// true si los productos escalares usan ESP-DSP (SIMD del S3)
bool usesDsp();

}  // namespace taste
