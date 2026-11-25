#include "brew_advisor.h"

#include <math.h>
#include <stdio.h>

namespace advisor {

namespace {
void addLine(Advice& a, const char* fmt, float v = 0.0f) {
  if (a.nLines >= MAX_LINES) return;
  snprintf(a.lines[a.nLines], LINE_LEN, fmt, v);
  a.nLines++;
}
float roundStep(float v, float step) { return roundf(v / step) * step; }
}  // namespace

Advice advise(const Input& in) {
  Advice a;
  a.inZone = fabsf(in.ey - in.targetEy) <= in.halfEy && fabsf(in.tds - in.targetTds) <= in.halfTds;
  const bool eyHigh = in.ey > in.targetEy + in.halfEy;
  const bool eyLow  = in.ey < in.targetEy - in.halfEy;

  if (in.acidBitter == 0 && in.strength == 0) {
    a.liked = true;
    addLine(a, "TE GUSTA: ES TU");
    addLine(a, "PUNTO DULCE. REPITE");
    if (a.inZone) addLine(a, "(DENTRO DE LA ZONA)");
    else          addLine(a, "(FUERA DE LA ZONA,");
    if (!a.inZone) addLine(a, "PERO MANDA TU GUSTO)");
    return a;
  }

  // Extracción: agrio pide extraer más; amargo o seco, menos
  if (in.acidBitter != 0) {
    const int n = in.acidBitter < 0 ? -in.acidBitter : in.acidBitter;
    const bool sour = in.acidBitter < 0;
    if (sour && eyHigh) {
      addLine(a, "AGRIO CON EY ALTA:");
      addLine(a, "SUBE TEMP 2C, REVISA");
      addLine(a, "CANALES/AGITACION");
    } else if (!sour && eyLow) {
      addLine(a, "AMARGO CON EY BAJA:");
      addLine(a, "BAJA TEMP 2-3C");
    } else {
      // finer = número menor si subir es más grueso
      const float dir = (sour ? -1.0f : 1.0f) * (in.grindUpCoarser ? 1.0f : -1.0f);
      if (in.grind >= 0.0f) {
        float g = roundStep(in.grind + dir * n * in.grindStep, in.grindStep);
        if (g < 0.0f) g = 0.0f;
        a.suggestedGrind = g;
        addLine(a, sour ? "MUELE MAS FINO: %.1f" : "MUELE MAS GRUESO:%.1f", g);
      } else {
        addLine(a, sour ? "MUELE MAS FINO" : "MUELE MAS GRUESO");
      }
      if (sour && !in.espresso && !in.hasBloom) addLine(a, "Y HAZ BLOOM 30-45 S");
      else if (n >= 2)                         addLine(a, sour ? "O SUBE LA TEMP 2C" : "O BAJA LA TEMP 2C");
    }
  }

  // Intensidad: aguado pide menos agua por gramo (ratio más corto); intenso, más
  if (in.strength != 0 && in.ratio > 0.0f) {
    const int n = in.strength < 0 ? -in.strength : in.strength;
    const float step = in.espresso ? 0.2f : 0.5f;
    const float r = in.ratio + (in.strength < 0 ? -1.0f : 1.0f) * n * step;
    a.suggestedRatio = r;
    if (in.espresso) addLine(a, in.strength < 0 ? "RATIO MAS CORTO 1:%.1f" : "RATIO MAS LARGO 1:%.1f", r);
    else             addLine(a, in.strength < 0 ? "MENOS AGUA: 1:%.1f" : "MAS AGUA: 1:%.1f", r);
  }

  if (in.bestGrindThisBag >= 0.0f) addLine(a, "MEJOR DE LA BOLSA:%.1f", in.bestGrindThisBag);
  return a;
}

void learnSweetSpot(float& targetEy, float& targetTds, float ey, float tds) {
  targetEy  = 0.8f * targetEy + 0.2f * ey;
  targetTds = 0.8f * targetTds + 0.2f * tds;
}

}  // namespace advisor
