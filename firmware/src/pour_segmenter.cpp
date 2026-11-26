#include "pour_segmenter.h"

namespace pourseg {

void Segmenter::reset() {
  state_     = State::Idle;
  restW_     = 0.0f;
  baseW_     = 0.0f;
  startMs_   = 0;
  endMs_     = 0;
  lastEndMs_ = 0;
  haveRest_  = false;
  pours_     = 0;
  taps_      = 0;
}

uint8_t Segmenter::livePours(float netW) const {
  if (state_ == State::Idle) return pours_;
  const bool merges = (lastEndMs_ != 0 && startMs_ - lastEndMs_ < MERGE_GAP_MS);
  if (!merges && netW - baseW_ >= MIN_GAIN_G && pours_ < 255) return static_cast<uint8_t>(pours_ + 1);
  return pours_;
}

Event Segmenter::add(uint32_t ms, float netW, float flowGps) {
  switch (state_) {
    case State::Idle:
      if (!haveRest_) { restW_ = netW; haveRest_ = true; }
      if (flowGps >= START_GPS) {
        state_   = State::Pouring;
        baseW_   = restW_;
        startMs_ = ms;
        return Event::PourStart;
      }
      // En reposo el peso de referencia sigue al real (evaporación, el agua que gotea...)
      if (flowGps <= END_GPS) restW_ = netW;
      return Event::None;

    case State::Pouring:
      if (flowGps <= END_GPS) {
        state_ = State::Settling;
        endMs_ = ms;
      }
      return Event::None;

    case State::Settling:
      if (flowGps >= START_GPS) {   // sigue vertiendo: el mismo candidato
        state_ = State::Pouring;
        return Event::None;
      }
      if (ms - endMs_ < SETTLE_MS) return Event::None;
      {
        state_ = State::Idle;
        restW_ = netW;
        const float gain   = netW - baseW_;
        const uint32_t dur = endMs_ - startMs_;
        if (gain < MIN_GAIN_G || dur < MIN_DUR_MS) {
          if (taps_ < 255) taps_++;
          return Event::TapRejected;
        }
        const bool merges = (lastEndMs_ != 0 && startMs_ - lastEndMs_ < MERGE_GAP_MS);
        lastEndMs_ = endMs_;
        if (merges) return Event::PourMerged;
        if (pours_ < 255) pours_++;
        return Event::PourAccepted;
      }
  }
  return Event::None;
}

}  // namespace pourseg
