#include "tds_sampler.h"

#include <math.h>

namespace tds {

void Sampler::reset() {
  n_           = 0;
  firstStable_ = 0xFF;
  phase_       = Phase::Temperature;
  result_      = 0.0f;
  flags_       = 0;
}

float Sampler::tolerance(float v) const {
  const float rel = fabsf(v) * 0.004f;
  return rel > 0.02f ? rel : 0.02f;
}

float Sampler::medianOfLast(uint8_t k, uint8_t from) const {
  if (n_ <= from) return 0.0f;
  if (k > n_ - from) k = n_ - from;
  float v[MAX_TESTS];
  for (uint8_t i = 0; i < k; i++) v[i] = s_[n_ - k + i].tds;
  for (uint8_t i = 1; i < k; i++) {   // inserción
    float x = v[i];
    int j = i - 1;
    while (j >= 0 && v[j] > x) { v[j + 1] = v[j]; j--; }
    v[j + 1] = x;
  }
  return (k % 2) ? v[k / 2] : 0.5f * (v[k / 2 - 1] + v[k / 2]);
}

void Sampler::finish(float v, uint8_t extraFlags) {
  result_ = v;
  flags_  = static_cast<uint8_t>(FLAG_VALID | extraFlags);
  phase_  = Phase::Done;
}

Phase Sampler::add(const Sample& s) {
  if (phase_ == Phase::Done || n_ >= MAX_TESTS) return phase_;
  s_[n_++] = s;
  const uint8_t i = n_ - 1;

  // ¿Temperatura estable? Muestra y prisma cerca, y la muestra casi no cambió desde la prueba anterior
  if (firstStable_ == 0xFF) {
    const bool close  = fabsf(s.sampleC - s.prismC) <= TEMP_GAP_C;
    const bool steady = (i > 0) && fabsf(s.sampleC - s_[i - 1].sampleC) <= TEMP_STEP_C;
    if (close && steady) firstStable_ = i;
  }

  if (firstStable_ != 0xFF) {
    phase_ = Phase::Converging;
    const uint8_t st = n_ - firstStable_;
    // Convergencia: las CONVERGE_N últimas estables dentro de la tolerancia
    if (st >= CONVERGE_N) {
      float lo = s_[n_ - CONVERGE_N].tds, hi = lo;
      for (uint8_t k = n_ - CONVERGE_N; k < n_; k++) {
        if (s_[k].tds < lo) lo = s_[k].tds;
        if (s_[k].tds > hi) hi = s_[k].tds;
      }
      const float med = medianOfLast(CONVERGE_N, firstStable_);
      if ((hi - lo) <= tolerance(med)) {
        finish(med, 0);
        return phase_;
      }
    }
    // Evaporación: EVAP_RUN estables que suben siempre, y en total más que la tolerancia
    if (st >= EVAP_RUN) {
      bool rising = true;
      for (uint8_t k = n_ - EVAP_RUN + 1; k < n_; k++) {
        if (!(s_[k].tds > s_[k - 1].tds)) { rising = false; break; }
      }
      const float first = s_[n_ - EVAP_RUN].tds;
      if (rising && (s_[n_ - 1].tds - first) > tolerance(first)) {
        finish(s_[firstStable_].tds, FLAG_EVAP);
        return phase_;
      }
    }
  }

  if (n_ >= MAX_TESTS) {
    if (firstStable_ == 0xFF) finish(medianOfLast(CONVERGE_N, 0), FLAG_DOUBTFUL | FLAG_TEMP);
    else                      finish(medianOfLast(CONVERGE_N, firstStable_), FLAG_DOUBTFUL);
  }
  return phase_;
}

uint16_t pack(float v, uint8_t flags) {
  if (!(flags & FLAG_VALID)) return 0;
  long c = lroundf(v * 100.0f);
  if (c < 0) c = 0;
  if (c > 4095) c = 4095;
  return static_cast<uint16_t>(c | (static_cast<uint16_t>(flags & 0x0F) << 12));
}

float unpackTds(uint16_t p) { return (p & 0x0FFF) / 100.0f; }
uint8_t unpackFlags(uint16_t p) { return static_cast<uint8_t>(p >> 12); }

float eyEspresso(float v, float beverageG, float doseG) {
  if (doseG <= 0.1f || beverageG <= 0.0f) return 0.0f;
  return v * beverageG / doseG;
}

float eyPourOver(float v, float waterG, float doseG, float retentionRatio) {
  if (doseG <= 0.1f) return 0.0f;
  const float bev = waterG - retentionRatio * doseG;
  return bev > 0.0f ? v * bev / doseG : 0.0f;
}

}  // namespace tds
