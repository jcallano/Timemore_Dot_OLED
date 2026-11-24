# -*- coding: utf-8 -*-
"""
Ensamble visual del display Timemore Dot: carcasa + tapa + modelos reales de los
componentes (STEP/IGES de ref_*/) colocados en su sitio, y una vista explosionada.

Ejecutar DESPUES de generar_caja.py:
    "C:\\Program Files\\FreeCAD 1.1\\bin\\freecadcmd.exe" generar_ensamble.py

Crea:
    ensamble_display_timemore.FCStd   (grupos "Montado" y "Explosionado")
    ensamble_display_timemore.step    (montado, para ver en cualquier visor CAD)
Es una copia estatica: si cambias Parametros en la caja, vuelve a ejecutar ambos scripts.
Los renders y la animacion: generar_renders.py (con freecad.exe) y montar_renders.js (ffmpeg).
"""
import os
import FreeCAD as App
import Part
import Import

HERE = os.path.dirname(os.path.abspath(__file__))
caja = App.openDocument(os.path.join(HERE, "caja_display_timemore.FCStd"))
sh = caja.getObject("Parametros")
g = lambda a: float(sh.get(a))  # noqa: E731


def load_step(path):
    tmp = App.newDocument("tmp_load")
    Import.insert(path, tmp.Name)
    shapes = []
    for o in tmp.Objects:
        if o.TypeId == "Part::Feature" and o.Shape.Solids:
            s = o.Shape.copy()
            s.Placement = o.getGlobalPlacement()
            shapes.append(s)
    App.closeDocument(tmp.Name)
    return Part.makeCompound(shapes)


def placed(shape, pl):
    return shape.transformed(pl.toMatrix())


# --------------------------------------------------------------------------
# Piezas (forma global ya colocada) y color RGB 0..1
# --------------------------------------------------------------------------
P = []  # (nombre, forma, color, grupo_explosion)
P.append(("Carcasa", caja.getObject("Carcasa").Shape.copy(), (0.92, 0.92, 0.90), "arriba"))
P.append(("Tapa", caja.getObject("Tapa").Shape.copy(), (0.22, 0.22, 0.25), "abajo"))
for _pn in ("PisadorOLED_izq", "PisadorOLED_der"):
    P.append((_pn, caja.getObject(_pn).Shape.copy(), (0.85, 0.45, 0.12), "arriba"))
P.append(("OLED_2.42", caja.getObject("Ref_OLED").Shape.copy(), (0.10, 0.25, 0.55), "arriba"))
P.append(("Encoder_EC11", caja.getObject("Ref_Encoder").Shape.copy(), (0.15, 0.35, 0.75), "arriba"))
P.append(("Mando", caja.getObject("Ref_Mando").Shape.copy(), (0.85, 0.45, 0.12), "arriba"))
P.append(("Portapilas_18650", caja.getObject("Ref_Portapilas").Shape.copy(), (0.12, 0.12, 0.12), "abajo"))
P.append(("Celda_18650", caja.getObject("Ref_18650").Shape.copy(), (0.20, 0.60, 0.30), "abajo"))

weact = load_step(os.path.join(HERE, "ref_weact", "WeAct_ESP32S3-A.step"))
P.append(("WeAct_ESP32-S3-A", placed(weact, App.Placement(
    App.Vector(g("Wc") - g("tw") - g("esp_gap"), g("esp_cy") + 15.24, g("esp_z")),
    App.Rotation(App.Vector(0, 0, 1), 180))), (0.05, 0.45, 0.20), "abajo"))
tp = load_step(os.path.join(HERE, "ref_tp4056", "TP4056_Charging_Module_Type_C.step"))
P.append(("TP4056", placed(tp, App.Placement(
    App.Vector(g("tp_cx"), g("D") - g("tw") - g("tp_gap") - g("tp_L") / 2, g("tp_z")),
    App.Rotation(App.Vector(0, 0, 1), 90))), (0.10, 0.30, 0.65), "abajo"))

# Interruptor MTS-103 simplificado: cuerpo dentro, rosca M6 y palanca fuera (pared trasera)
yb = g("D")
sw_body = Part.makeBox(8, 13, 13, App.Vector(g("sw_cx") - 4, yb - g("tw") - 13, g("sw_cz") - 6.5))
sw_thr = Part.makeCylinder(3.0, 9, App.Vector(g("sw_cx"), yb - g("tw"), g("sw_cz")), App.Vector(0, 1, 0))
sw_lev = Part.makeCylinder(1.2, 11, App.Vector(g("sw_cx"), yb - g("tw") + 9, g("sw_cz")),
                           App.Vector(0, 0.94, 0.34))
P.append(("Interruptor_MTS-103", Part.makeCompound([sw_body, sw_thr, sw_lev]), (0.75, 0.75, 0.78), "arriba"))

# --------------------------------------------------------------------------
# Documento de ensamble
# --------------------------------------------------------------------------
ens = App.newDocument("EnsambleTimemore")
ens.Label = "Ensamble display Timemore Dot"
g_mont = ens.addObject("App::DocumentObjectGroup", "Montado")
g_expl = ens.addObject("App::DocumentObjectGroup", "Explosionado")
DZ_ARRIBA, DZ_ABAJO = 45.0, -15.0
colors = {}
for name, shp, col, grp in P:
    o = ens.addObject("Part::Feature", name)
    o.Shape = shp
    g_mont.addObject(o)
    colors[o.Name] = col
    e = ens.addObject("Part::Feature", name + "_expl")
    e.Shape = shp.copy()
    e.Placement = App.Placement(App.Vector(0, 0, DZ_ARRIBA if grp == "arriba" else DZ_ABAJO), App.Rotation())
    e.Visibility = False
    g_expl.addObject(e)
    colors[e.Name] = col
g_expl.Visibility = False
ens.recompute()
# SOLO_RENDER=1 regenera solo las imagenes (no pisa el .FCStd, que guarda los colores de la GUI)
if not os.environ.get("SOLO_RENDER"):
    ens.saveAs(os.path.join(HERE, "ensamble_display_timemore.FCStd"))
    Part.export([o for o in g_mont.Group], os.path.join(HERE, "ensamble_display_timemore.step"))
    # colores para aplicarlos luego en la GUI (freecadcmd no guarda colores)
    with open(os.path.join(HERE, "ensamble_colores.txt"), "w") as f:
        for k, v in colors.items():
            f.write("%s %.2f %.2f %.2f\n" % (k, v[0], v[1], v[2]))
    print("ENSAMBLE GUARDADO")

# Los renders se hacen aparte, con el visor de FreeCAD: generar_renders.py y montar_renders.js
