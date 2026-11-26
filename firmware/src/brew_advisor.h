// Sugerencias de ajuste a partir del SABOR (primero) y de los números (TDS, EY) del gráfico de control.
// El sabor manda: si el café gusta, no se cambia nada aunque los números estén fuera de la zona, y ese
// punto alimenta el punto dulce personal. Cada eje del gráfico se corrige con lo suyo:
//   - Extracción (agrio <-> amargo, EY): molienda; después temperatura y bloom.
//   - Intensidad (aguado <-> intenso, TDS): ratio (agua o bebida por gramo de café).
// Si el sabor contradice los números (agrio con EY alta, amargo con EY baja) no se toca la molienda:
// suele ser canalización, agitación o el tueste, y se sugiere la temperatura.
// Módulo puro (sin Arduino), con autotest. Textos de 21 caracteres como máximo (OLED de 128 px).
#pragma once

#include <stdint.h>

namespace advisor {

static constexpr uint8_t MAX_LINES = 4;
static constexpr uint8_t LINE_LEN  = 22;   // 21 caracteres + '\0'

struct Input {
  bool    espresso         = false;
  float   ey               = 0.0f;
  float   tds              = 0.0f;
  float   targetEy         = 20.0f;   // centro del punto dulce
  float   targetTds        = 1.30f;
  float   halfEy           = 2.0f;    // semiancho de la zona
  float   halfTds          = 0.15f;
  int8_t  acidBitter       = 0;       // -2 muy agrio .. 0 equilibrado .. +2 muy amargo/seco
  int8_t  strength         = 0;       // -2 muy aguado .. 0 bien .. +2 muy intenso
  float   grind            = -1.0f;   // dial del molino del café (<0 = desconocido)
  bool    grindUpCoarser   = true;    // subir el número = más grueso
  float   grindStep        = 0.1f;
  bool    hasBloom         = true;    // pour-over: hubo bloom
  float   ratio            = 0.0f;    // filtro: agua/dosis; espresso: bebida/dosis
  float   bestGrindThisBag = -1.0f;   // molienda del café que más gustó de esta bolsa (<0 = ninguno)
  bool    english          = false;   // textos en inglés
};

struct Advice {
  bool    liked          = false;     // sabor equilibrado: es tu punto dulce
  bool    inZone         = false;     // los números caen dentro de la zona
  float   suggestedGrind = -1.0f;     // <0 = no cambiar
  float   suggestedRatio = -1.0f;     // <0 = no cambiar
  uint8_t nLines         = 0;
  char    lines[MAX_LINES][LINE_LEN] = {};
};

Advice advise(const Input& in);

// Mueve el punto dulce hacia un café que gustó (media exponencial: 20 % del nuevo punto)
void learnSweetSpot(float& targetEy, float& targetTds, float ey, float tds);

}  // namespace advisor
