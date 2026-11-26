#include "taste_model.h"

#include <math.h>
#include <string.h>

#if defined(ESP_PLATFORM) && __has_include("dsps_dotprod.h")
#include "dsps_dotprod.h"
#define TASTE_USE_DSP 1
#endif

namespace taste {

namespace {
float dot(const float* a, const float* b, int n) {
#if defined(TASTE_USE_DSP)
  float r = 0.0f;
  dsps_dotprod_f32(a, b, &r, n);   // ESP-DSP: versión SIMD en el ESP32-S3
  return r;
#else
  float r = 0.0f;
  for (int i = 0; i < n; i++) r += a[i] * b[i];
  return r;
#endif
}

// Generador pseudoaleatorio fijo: los pesos iniciales son siempre los mismos
uint32_t g_seed = 12345;
float rnd() {
  g_seed = g_seed * 1664525u + 1013904223u;
  return ((g_seed >> 8) & 0xFFFF) / 65535.0f - 0.5f;
}

// Regla de partida: zona SCA en el centro; 2 puntos de EY o 0.15 de TDS (1.5 en espresso) = 1 nivel
Sample prior(bool espresso, float ey, float tdsV) {
  Sample s;
  s.espresso = espresso;
  s.ey       = ey;
  s.tds      = tdsV;
  s.ratio    = espresso ? 2.0f : 16.0f;
  s.bloom    = !espresso;
  const float ab = (ey - 20.0f) / 2.0f;
  const float st = espresso ? (tdsV - 10.0f) / 1.5f : (tdsV - 1.30f) / 0.15f;
  s.acidBitter = static_cast<int8_t>(lroundf(ab < -2 ? -2 : (ab > 2 ? 2 : ab)));
  s.strength   = static_cast<int8_t>(lroundf(st < -2 ? -2 : (st > 2 ? 2 : st)));
  return s;
}
}  // namespace

bool usesDsp() {
#if defined(TASTE_USE_DSP)
  return true;
#else
  return false;
#endif
}

void encode(const Sample& s, float* x) {
  x[0] = (s.ey - 20.0f) / 3.0f;
  x[1] = s.espresso ? (s.tds - 10.0f) / 2.0f : (s.tds - 1.30f) / 0.20f;
  x[2] = s.espresso ? (s.ratio - 2.0f) / 0.5f : (s.ratio - 16.0f) / 2.0f;
  x[3] = s.bloom ? 1.0f : 0.0f;
  x[4] = s.espresso ? 1.0f : 0.0f;
  x[5] = s.tempC > 0.0f ? (s.tempC - 92.0f) / 4.0f : 0.0f;
}

void Model::forward(const float* x, float* h, float* y) const {
  for (int j = 0; j < N_HID; j++) h[j] = tanhf(dot(w_.w1[j], x, N_IN) + w_.b1[j]);
  for (int k = 0; k < N_OUT; k++) y[k] = 2.0f * tanhf(dot(w_.w2[k], h, N_HID) + w_.b2[k]);
}

void Model::predict(const Sample& s, float* ab, float* st) const {
  float x[N_IN], h[N_HID], y[N_OUT];
  encode(s, x);
  forward(x, h, y);
  if (ab) *ab = y[0];
  if (st) *st = y[1];
}

// Un paso de descenso por gradiente (error cuadrático) con peso de muestra
void Model::sgd(const Sample& s, float weight, float lr) {
  float x[N_IN], h[N_HID], y[N_OUT];
  encode(s, x);
  forward(x, h, y);
  const float target[N_OUT] = {static_cast<float>(s.acidBitter), static_cast<float>(s.strength)};
  float dOut[N_OUT];
  for (int k = 0; k < N_OUT; k++) {
    const float t = y[k] / 2.0f;                  // tanh
    dOut[k] = weight * (y[k] - target[k]) * 2.0f * (1.0f - t * t);
  }
  float dHid[N_HID];
  for (int j = 0; j < N_HID; j++) {
    float g = 0.0f;
    for (int k = 0; k < N_OUT; k++) g += dOut[k] * w_.w2[k][j];
    dHid[j] = g * (1.0f - h[j] * h[j]);
  }
  for (int k = 0; k < N_OUT; k++) {
    for (int j = 0; j < N_HID; j++) w_.w2[k][j] -= lr * dOut[k] * h[j];
    w_.b2[k] -= lr * dOut[k];
  }
  for (int j = 0; j < N_HID; j++) {
    for (int i = 0; i < N_IN; i++) w_.w1[j][i] -= lr * dHid[j] * x[i];
    w_.b1[j] -= lr * dHid[j];
  }
}

void Model::init() {
  memset(&w_, 0, sizeof(w_));
  g_seed = 12345;
  for (int j = 0; j < N_HID; j++) {
    for (int i = 0; i < N_IN; i++) w_.w1[j][i] = rnd();
    w_.b1[j] = 0.1f * rnd();
  }
  for (int k = 0; k < N_OUT; k++)
    for (int j = 0; j < N_HID; j++) w_.w2[k][j] = rnd();
  // Preentrenamiento con la regla, en filtro y espresso
  for (int epoch = 0; epoch < 300; epoch++) {
    for (float e = 15.0f; e <= 25.0f; e += 1.25f) {
      for (int t = 0; t < 5; t++) {
        sgd(prior(false, e, 1.00f + 0.15f * t), 1.0f, 0.02f);
        sgd(prior(true, e, 7.0f + 1.5f * t), 1.0f, 0.02f);
      }
    }
  }
  w_.magic       = MAGIC;
  w_.samplesSeen = 0;
  w_.maeAb       = 0.0f;
  w_.maeSt       = 0.0f;
}

void Model::learn(const Sample& s, const Sample* history, int nHistory) {
  // Error al predecir ANTES de aprender: así se ve si el modelo acierta cafés que aún no conoce
  float pab, pst;
  predict(s, &pab, &pst);
  const float eab = fabsf(pab - s.acidBitter), est = fabsf(pst - s.strength);
  if (w_.samplesSeen == 0) { w_.maeAb = eab; w_.maeSt = est; }
  else { w_.maeAb = 0.8f * w_.maeAb + 0.2f * eab; w_.maeSt = 0.8f * w_.maeSt + 0.2f * est; }
  if (w_.samplesSeen < 0xFFFF) w_.samplesSeen++;

  // Repasos: la valoración nueva, las guardadas y la regla de fondo con poco peso
  for (int epoch = 0; epoch < 40; epoch++) {
    sgd(s, 1.0f, 0.03f);
    for (int i = 0; i < nHistory; i++) sgd(history[i], 0.7f, 0.03f);
    for (float e = 16.0f; e <= 24.0f; e += 2.0f) {
      sgd(prior(s.espresso, e, s.espresso ? 10.0f : 1.30f), 0.15f, 0.03f);
    }
  }
}

// La red puede predecir "equilibrado" en más de un sitio (lejos de los datos no sabe nada): se busca el
// más cercano a donde están los cafés valorados (hint), con una penalización suave por distancia.
void Model::sweetSpot(bool espresso, float ratio, float hintEy, float hintTds, float* ey, float* tdsOut) const {
  const float tScale = espresso ? 1.5f : 0.15f;
  float best = 1e9f, bEy = hintEy, bT = hintTds;
  for (float e = 15.0f; e <= 25.0f; e += 0.25f) {
    for (int t = 0; t <= 28; t++) {
      Sample s;
      s.espresso = espresso;
      s.bloom    = !espresso;
      s.ratio    = ratio > 0.0f ? ratio : (espresso ? 2.0f : 16.0f);
      s.ey       = e;
      s.tds      = espresso ? 6.0f + 0.25f * t : 0.95f + 0.025f * t;
      float ab, st;
      predict(s, &ab, &st);
      const float de = (e - hintEy) / 2.0f, dt = (s.tds - hintTds) / tScale;
      const float d = ab * ab + st * st + 0.05f * (de * de + dt * dt);
      if (d < best) { best = d; bEy = e; bT = s.tds; }
    }
  }
  if (ey) *ey = bEy;
  if (tdsOut) *tdsOut = bT;
}

float Model::bitterPerEy(bool espresso, float ey, float tdsV, float ratio) const {
  Sample a, b;
  a.espresso = b.espresso = espresso;
  a.bloom = b.bloom = !espresso;
  a.ratio = b.ratio = ratio > 0.0f ? ratio : (espresso ? 2.0f : 16.0f);
  a.tds = b.tds = tdsV;
  a.ey = ey - 1.0f;
  b.ey = ey + 1.0f;
  float abA, abB;
  predict(a, &abA, nullptr);
  predict(b, &abB, nullptr);
  return (abB - abA) / 2.0f;
}

}  // namespace taste
