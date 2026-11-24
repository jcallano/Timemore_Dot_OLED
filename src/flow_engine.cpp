#include "flow_engine.h"

#include <math.h>

static inline float median3(float a, float b, float c) {
  if (a > b) { float t = a; a = b; b = t; }
  if (b > c) { float t = b; b = c; c = t; }
  if (a > b) { float t = a; a = b; b = t; }
  return b;
}

void FlowEngine::reset(float baseWeight) {
  winHead_         = 0;
  winCount_        = 0;
  currentGps       = 0.0f;
  displayedGps     = 0.0f;
  prevWeight_      = baseWeight;
  highWater_       = baseWeight;
  lastCalcMs       = 0;
  lastStepMs       = 0;
  medianBuf_[0]    = 0.0f;
  medianBuf_[1]    = 0.0f;
  medianBuf_[2]    = 0.0f;
  medianIdx_       = 0;
  startedFromRest_ = true;
}

void FlowEngine::forceZero() {
  currentGps   = 0.0f;
  displayedGps = 0.0f;
}

void FlowEngine::decayIfStale(unsigned long nowMs, unsigned long lastRxMs) {
  unsigned long rxAgeMs = (nowMs >= lastRxMs) ? (nowMs - lastRxMs) : 0UL;
  if (lastRxMs == 0 || rxAgeMs > 450UL) {
    currentGps *= 0.60f;
    if (fabsf(currentGps) < 0.15f) {
      currentGps   = 0.0f;
      displayedGps = 0.0f;
      stable       = true;
    } else {
      displayedGps = currentGps;
    }
  }
}

void FlowEngine::addSample(unsigned long nowMs, float netWeight, bool trackPeak) {
  float stepDelta = netWeight - prevWeight_;
  unsigned long stepAgeMs = (lastStepMs > 0 && nowMs >= lastStepMs) ? (nowMs - lastStepMs) : 9999UL;

  if (lastCalcMs == 0 || fabsf(stepDelta) > 25.0f) {
    // Primera muestra o salto > 25 g (se colocó/retiró la jarra): nueva base. lastStepMs = 0 para
    // que colocar la jarra no dispare el auto-timer.
    reset(netWeight);
    winTimeMs_[0] = nowMs;
    winWeight_[0] = netWeight;
    winHead_      = 1;
    winCount_     = 1;
    stable        = true;
    lastCalcMs    = nowMs;
    return;
  }

  if (stepAgeMs > 480UL || winCount_ == 0) {
    // Estaba en reposo: anclar la muestra base 180 ms atrás con el peso previo
    float base = prevWeight_;
    reset(base);
    winTimeMs_[0]    = (nowMs > 180UL) ? (nowMs - 180UL) : 0;
    winWeight_[0]    = base;
    winHead_         = 1;
    winCount_        = 1;
    startedFromRest_ = true;
  }

  winTimeMs_[winHead_] = nowMs;
  winWeight_[winHead_] = netWeight;
  winHead_ = static_cast<uint8_t>((winHead_ + 1) % WIN_SIZE);
  if (winCount_ < WIN_SIZE) winCount_++;

  // Marca de agua: solo sube con ganancia neta >= 0.08 g; se reajusta si se retiró > 1.5 g
  if (netWeight >= highWater_ + 0.08f) {
    highWater_ = netWeight;
    lastStepMs = nowMs;
    stepAgeMs  = 0;
  } else if (netWeight < highWater_ - 1.5f) {
    highWater_ = netWeight;
  }

  if (stepAgeMs > 300UL) {
    // > 300 ms sin superar la marca de agua: se dejó de verter
    currentGps *= 0.28f;
    if (currentGps < 0.25f) {
      currentGps       = 0.0f;
      displayedGps     = 0.0f;
      medianBuf_[0]    = 0.0f;
      medianBuf_[1]    = 0.0f;
      medianBuf_[2]    = 0.0f;
      startedFromRest_ = true;
      stable           = true;
    } else {
      displayedGps = currentGps;
    }
  } else {
    // Muestra de referencia hacia atrás: ~320-400 ms en régimen, >= 160 ms al arrancar
    int bestIdx = -1;
    unsigned long bestDt = 0;
    for (uint8_t i = 1; i < winCount_; i++) {
      uint8_t idx = static_cast<uint8_t>((winHead_ + WIN_SIZE - 1 - i) % WIN_SIZE);
      unsigned long sampleAge = (nowMs >= winTimeMs_[idx]) ? (nowMs - winTimeMs_[idx]) : 0UL;
      if (sampleAge >= 160UL && sampleAge <= 600UL) {
        bestIdx = idx;
        bestDt  = sampleAge;
        if (sampleAge >= 320UL) break;
      }
    }

    if (bestIdx >= 0 && bestDt >= 160UL) {
      float rawSlope = ((netWeight - winWeight_[bestIdx]) * 1000.0f) / static_cast<float>(bestDt);
      if (rawSlope < 0.0f)  rawSlope = 0.0f;
      if (rawSlope > 11.5f) rawSlope = 11.5f;

      // Mediana de 3 (al arrancar desde reposo se siembran las 3 casillas: 0 ms de retardo)
      if (startedFromRest_ && rawSlope >= 0.4f) {
        medianBuf_[0]    = rawSlope;
        medianBuf_[1]    = rawSlope;
        medianBuf_[2]    = rawSlope;
        medianIdx_       = 0;
        startedFromRest_ = false;
      } else {
        medianBuf_[medianIdx_] = rawSlope;
        medianIdx_ = static_cast<uint8_t>((medianIdx_ + 1) % 3);
      }
      float medFlow = median3(medianBuf_[0], medianBuf_[1], medianBuf_[2]);

      // EMA adaptativa
      float diff  = fabsf(medFlow - currentGps);
      float alpha = 0.20f;
      if (currentGps < 0.4f) {
        alpha = 0.75f;
      } else if (diff >= 0.8f) {
        alpha = 0.55f;
      } else if (diff >= 0.4f) {
        alpha = 0.35f;
      }
      currentGps = (alpha * medFlow) + ((1.0f - alpha) * currentGps);

      if (currentGps < 0.20f && medFlow < 0.20f) {
        currentGps   = 0.0f;
        displayedGps = 0.0f;
      } else if (displayedGps == 0.0f || fabsf(currentGps - displayedGps) >= 0.18f) {
        displayedGps = currentGps;  // histéresis visual de 0.18 g/s
      }
    }

    stable = (currentGps < 0.20f);
    if (trackPeak && currentGps > peakGps) {
      peakGps = currentGps;
    }
  }

  prevWeight_ = netWeight;
  lastCalcMs  = nowMs;
}
