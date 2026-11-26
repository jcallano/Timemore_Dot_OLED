// Separa los vertidos de un pour-over y descarta los toques de la tetera.
//
// Un toque del pico de la tetera contra el filtro (o apoyar la mano) da un pico de caudal igual que un
// vertido, pero el peso vuelve a donde estaba. Un vertido de verdad deja agua: el peso se queda arriba.
// Por eso cada candidato (caudal >= START_GPS) se juzga cuando termina y el peso se ha asentado
// (SETTLE_MS sin caudal), con dos condiciones:
//   - ganancia neta (peso asentado - peso de antes) >= MIN_GAIN_G
//   - duración >= MIN_DUR_MS
// Si no las cumple es un toque: no cuenta como vertido (se cuenta aparte, para el perfil).
// Dos vertidos separados por menos de MERGE_GAP_MS son la misma tanda (la mano se recoloca).
//
// Recibe el peso neto y el caudal ya filtrado por el motor de caudal (src/flow_engine), a ~10 Hz.
// Módulo puro (sin Arduino), con autotest.
#pragma once

#include <stdint.h>

namespace pourseg {

static constexpr float    START_GPS    = 0.8f;    // empieza un candidato
static constexpr float    END_GPS      = 0.25f;   // termina
static constexpr uint32_t SETTLE_MS    = 800;     // espera sin caudal antes de juzgar
static constexpr float    MIN_GAIN_G   = 4.0f;    // agua mínima que deja un vertido
static constexpr uint32_t MIN_DUR_MS   = 1000;    // duración mínima de un vertido
static constexpr uint32_t MERGE_GAP_MS = 3000;    // pausa máxima dentro de una tanda

// Qué pasó en la última llamada a add()
enum class Event : uint8_t { None, PourStart, PourAccepted, PourMerged, TapRejected };

class Segmenter {
 public:
  void reset();

  // ms: marca de tiempo de la muestra; netW: peso neto (g); flowGps: caudal filtrado (g/s)
  Event add(uint32_t ms, float netW, float flowGps);

  uint8_t pours() const { return pours_; }        // vertidos confirmados (tandas)
  uint8_t taps() const { return taps_; }          // toques descartados
  bool active() const { return state_ != State::Idle; }
  uint32_t candidateStartMs() const { return startMs_; }
  // Agua que lleva el candidato en curso (0 sin candidato)
  float candidateGain(float netW) const { return active() ? netW - baseW_ : 0.0f; }
  // Vertidos a mostrar en vivo: los confirmados y el candidato en curso si ya deja agua de verdad
  uint8_t livePours(float netW) const;
  uint32_t lastPourEndMs() const { return lastEndMs_; }   // 0 = ninguno

 private:
  enum class State : uint8_t { Idle, Pouring, Settling };
  State state_       = State::Idle;
  float restW_       = 0.0f;   // peso en reposo (base del siguiente candidato)
  float baseW_       = 0.0f;
  uint32_t startMs_  = 0;
  uint32_t endMs_    = 0;      // fin del caudal del candidato (al pasar a Settling)
  uint32_t lastEndMs_ = 0;     // fin del último vertido aceptado
  bool haveRest_     = false;
  uint8_t pours_     = 0;
  uint8_t taps_      = 0;
};

}  // namespace pourseg
