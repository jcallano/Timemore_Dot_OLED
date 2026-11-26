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
#define L(es, en) (in.english ? (en) : (es))
  Advice a;
  a.inZone = fabsf(in.ey - in.targetEy) <= in.halfEy && fabsf(in.tds - in.targetTds) <= in.halfTds;
  const bool eyHigh = in.ey > in.targetEy + in.halfEy;
  const bool eyLow  = in.ey < in.targetEy - in.halfEy;

  if (in.acidBitter == 0 && in.strength == 0) {
    a.liked = true;
    addLine(a, L("TE GUSTA: ES TU", "YOU LIKE IT: THIS IS"));
    addLine(a, L("PUNTO DULCE. REPITE", "YOUR SWEET SPOT"));
    if (a.inZone) addLine(a, L("(DENTRO DE LA ZONA)", "(INSIDE THE ZONE)"));
    else          addLine(a, L("(FUERA DE LA ZONA,", "(OUTSIDE THE ZONE,"));
    if (!a.inZone) addLine(a, L("PERO MANDA TU GUSTO)", "BUT TASTE RULES)"));
    return a;
  }

  // Extracción: agrio pide extraer más; amargo o seco, menos
  if (in.acidBitter != 0) {
    const int n = in.acidBitter < 0 ? -in.acidBitter : in.acidBitter;
    const bool sour = in.acidBitter < 0;
    if (sour && eyHigh) {
      addLine(a, L("AGRIO CON EY ALTA:", "SOUR WITH HIGH EY:"));
      addLine(a, L("SUBE TEMP 2C, REVISA", "RAISE TEMP 2C, CHECK"));
      addLine(a, L("CANALES/AGITACION", "CHANNELING/AGITATION"));
    } else if (!sour && eyLow) {
      addLine(a, L("AMARGO CON EY BAJA:", "BITTER WITH LOW EY:"));
      addLine(a, L("BAJA TEMP 2-3C", "LOWER TEMP 2-3C"));
    } else {
      // finer = número menor si subir es más grueso
      const float dir = (sour ? -1.0f : 1.0f) * (in.grindUpCoarser ? 1.0f : -1.0f);
      if (in.grind >= 0.0f) {
        float g = roundStep(in.grind + dir * n * in.grindStep, in.grindStep);
        if (g < 0.0f) g = 0.0f;
        a.suggestedGrind = g;
        addLine(a, sour ? L("MUELE MAS FINO: %.1f", "GRIND FINER: %.1f") : L("MUELE MAS GRUESO:%.1f", "GRIND COARSER: %.1f"), g);
      } else {
        addLine(a, sour ? L("MUELE MAS FINO", "GRIND FINER") : L("MUELE MAS GRUESO", "GRIND COARSER"));
      }
      if (sour && !in.espresso && !in.hasBloom) addLine(a, L("Y HAZ BLOOM 30-45 S", "AND BLOOM 30-45 S"));
      else if (n >= 2)                         addLine(a, sour ? L("O SUBE LA TEMP 2C", "OR RAISE TEMP 2C") : L("O BAJA LA TEMP 2C", "OR LOWER TEMP 2C"));
    }
  }

  // Intensidad: aguado pide menos agua por gramo (ratio más corto); intenso, más
  if (in.strength != 0 && in.ratio > 0.0f) {
    const int n = in.strength < 0 ? -in.strength : in.strength;
    const float step = in.espresso ? 0.2f : 0.5f;
    const float r = in.ratio + (in.strength < 0 ? -1.0f : 1.0f) * n * step;
    a.suggestedRatio = r;
    if (in.espresso) addLine(a, in.strength < 0 ? L("RATIO MAS CORTO 1:%.1f", "SHORTER RATIO 1:%.1f") : L("RATIO MAS LARGO 1:%.1f", "LONGER RATIO 1:%.1f"), r);
    else             addLine(a, in.strength < 0 ? L("MENOS AGUA: 1:%.1f", "LESS WATER: 1:%.1f") : L("MAS AGUA: 1:%.1f", "MORE WATER: 1:%.1f"), r);
  }

  if (in.bestGrindThisBag >= 0.0f) addLine(a, L("MEJOR DE LA BOLSA:%.1f", "BEST IN THIS BAG:%.1f"), in.bestGrindThisBag);
  return a;
#undef L
}

void learnSweetSpot(float& targetEy, float& targetTds, float ey, float tds) {
  targetEy  = 0.8f * targetEy + 0.2f * ey;
  targetTds = 0.8f * targetTds + 0.2f * tds;
}

}  // namespace advisor
