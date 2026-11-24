// Motor de caudal (g/s) para pour-over. Módulo PURO (sin Arduino, sin estado global):
// recibe muestras {ms, peso neto} y calcula el caudal; se puede probar con vertidos
// simulados (ver selftest.cpp).
//
// 3 etapas:
//  1. Arranque instantáneo al detectar subida neta de peso tras reposo (ancla 180 ms atrás).
//  2. Ventana ágil ~350 ms + mediana de 3 (filtra el impacto del chorro y paquetes repetidos).
//  3. EMA adaptativa (alfa 0.75 / 0.55 / 0.35 / 0.20 según el cambio) + histéresis visual 0.18 g/s.
//  Corte por "marca de agua": sin ganancia neta >= 0.08 g en 300 ms -> el caudal cae a 0.
#pragma once
#include <stdint.h>

class FlowEngine {
 public:
  static constexpr uint8_t WIN_SIZE = 16;

  // Estado visible (lo leen la UI, el auto-timer, el LED...)
  float currentGps            = 0.0f;  // caudal filtrado
  float displayedGps          = 0.0f;  // caudal con histéresis visual (lo que se muestra)
  float peakGps               = 0.0f;  // pico (solo se actualiza con trackPeak = true)
  bool stable                 = true;  // sin vertido activo
  unsigned long lastStepMs    = 0;     // última subida neta >= 0.08 g (0 = reposo / tras reset)
  unsigned long lastCalcMs    = 0;     // última muestra procesada (0 = sin base)

  // Reinicia la ventana con un peso base (tara, colocar/retirar la jarra...)
  void reset(float baseWeight);

  // Procesa una muestra de peso NETO. trackPeak: actualizar peakGps (cronómetro en marcha).
  void addSample(unsigned long nowMs, float netWeight, bool trackPeak);

  // Si dejan de llegar paquetes (> 450 ms) el caudal decae a 0 (llamar periódicamente).
  void decayIfStale(unsigned long nowMs, unsigned long lastRxMs);

  // Fuerza caudal 0 (desconexión de la báscula)
  void forceZero();

 private:
  unsigned long winTimeMs_[WIN_SIZE] = {0};
  float winWeight_[WIN_SIZE]         = {0.0f};
  uint8_t winHead_                   = 0;
  uint8_t winCount_                  = 0;
  float medianBuf_[3]                = {0.0f, 0.0f, 0.0f};
  uint8_t medianIdx_                 = 0;
  bool startedFromRest_              = true;
  float prevWeight_                  = 0.0f;
  float highWater_                   = 0.0f;
};
