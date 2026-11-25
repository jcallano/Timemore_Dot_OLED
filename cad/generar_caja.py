# -*- coding: utf-8 -*-
"""
Genera la caja (carcasa + tapa + pisadores de la OLED) del display para la
bascula Timemore Black Mirror Dot.

Ejecutar con:  "C:\\Program Files\\FreeCAD 1.1\\bin\\freecadcmd.exe" generar_caja.py
Crea caja_display_timemore.FCStd y exporta STL/STEP en la carpeta cad/.

Todo cuelga de la hoja de calculo "Parametros": cambia un valor alli, pulsa
Recompute en FreeCAD y vuelve a exportar las piezas.

Ejes: X = ancho (izquierda->derecha mirando la pantalla), Y = fondo (Y=0 frente),
Z = altura (Z=0 apoyo de la caja). El panel superior esta inclinado A grados y
sube hacia atras. Coordenadas "de panel": u = X, s = distancia a lo largo de la
pendiente desde la arista frontal superior, d = profundidad hacia dentro desde
la cara exterior del panel.

v2:
- La tapa va encajada DENTRO de la carcasa (galce en la base de las paredes +
  labio de centrado), enrasada por debajo.
- OLED encajonada: ventana del tamano del marco metalico (63 x 40) y el modulo
  dentro de ella. Se sujeta con dos pisadores impresos atornillados (M2.5 a
  insertos de laton M2.5 x 3 x 3.5) que aprietan el borde del PCB por detras.
- ESP32 WeAct elevado (pines + dupont hacia abajo): bastidor central, horquilla
  en el lado de la antena y dos ganchos de clip en el lado del USB.
- TP4056 con pines hacia arriba: pivotes en sus muescas, carriles de apoyo y
  dos ganchos de clip.
"""
import os
import FreeCAD as App
import Part
import Mesh

HERE = os.path.dirname(os.path.abspath(__file__))
FCSTD = os.path.join(HERE, "caja_display_timemore.FCStd")

doc = App.newDocument("CajaTimemore")
doc.Label = "Caja display Timemore Dot"

# --------------------------------------------------------------------------
# Hoja de parametros
# --------------------------------------------------------------------------
PARAMS = [
    # alias, valor, descripcion
    ("Wc", "124", "Ancho exterior (X)"),
    ("D", "78", "Fondo exterior (Y)"),
    ("Hf", "28", "Altura frontal total incl. tapa"),
    ("ang", "=25deg", "Inclinacion del panel"),
    ("Hb", "=Hf + D*tan(ang)", "Altura trasera total (calculada)"),
    ("tw", "2.4", "Grosor de paredes laterales"),
    ("tp", "2.0", "Grosor del panel superior (tuerca M7 del EC11 <= 2.5)"),
    ("Lt", "3.0", "Grosor de la tapa inferior"),
    ("clr", "0.25", "Holgura de impresion por lado"),
    # Encaje tapa-carcasa (cuna)
    ("reb_w", "1.2", "Galce: cuanto se mete la tapa en la pared (queda pared de tw-reb_w)"),
    ("lip_t", "1.2", "Labio de centrado de la tapa: grosor"),
    ("lip_h", "3", "Labio de centrado de la tapa: altura sobre la tapa"),
    # Postes / tornilleria del cierre
    ("post_d", "8", "Diametro postes de tornillo"),
    ("Hp", "10", "Altura postes"),
    ("ins_d", "4.0", "Agujero inserto laton M3 x 5 x D4.2"),
    ("ins_h", "7", "Profundidad agujero inserto M3 (inserto de 5 + sitio para M3x8)"),
    ("post_yl", "44", "Y del poste intermedio izquierdo"),
    ("post_yr", "33", "Y del poste intermedio derecho (entre portapilas y ESP32)"),
    ("screw_d", "3.4", "Paso tornillo M3 en la tapa"),
    ("cb_d", "6.5", "Avellanado/cajeado cabeza M3"),
    ("cb_h", "1.8", "Profundidad cajeado cabeza"),
    # OLED 2.42" encajonada
    ("oled_x", "78", "Centro X de la OLED"),
    ("oled_s", "37", "Centro del PCB OLED a lo largo de la pendiente"),
    ("oled_w", "70", "PCB OLED ancho (STEP 2.42OLED-IIC v1.1)"),
    ("oled_h", "48", "PCB OLED alto (STEP)"),
    ("oled_pcb_t", "0.96", "Grosor PCB OLED (STEP)"),
    ("mod_w", "63", "Modulo de cristal con marco metalico: ancho (medido)"),
    ("mod_h", "40", "Modulo de cristal con marco metalico: alto (medido)"),
    ("mod_t", "3.1", "Grosor del modulo sobre el PCB (STEP)"),
    ("glass_off", "-0.42", "Centro del modulo vs centro PCB (+ = hacia el header/atras)"),
    ("oled_rec", "0.3", "Hundimiento del cristal respecto a la cara exterior del panel"),
    ("oled_d0", "=oled_rec + mod_t", "Profundidad de la cara delantera del PCB (calculada)"),
    ("stop_w", "2.4", "Ancho de los topes donde apoya la cara del PCB"),
    ("hdr_front", "2.1", "Pines del header que asoman por delante del PCB (STEP 2.01)"),
    ("ins25_d", "3.4", "Agujero inserto laton M2.5 (D ext 3.5; guia de 3.0 y el FDM encoge el agujero ~0.2)"),
    ("ins25_h", "3.5", "Profundidad agujero inserto M2.5 x 3 x D3.5 (tornillo M2.5x6)"),
    ("scr25_d", "2.8", "Paso tornillo M2.5 en el pisador"),
    ("boss_d", "7", "Diametro torretas de los pisadores"),
    ("boss_u", "=oled_w/2 + 0.4 + boss_d/2", "Torretas: distancia horizontal al centro OLED"),
    ("boss_s", "15", "Torretas: semi-distancia a lo largo de la pendiente"),
    ("pis_t", "2.5", "Grosor del pisador"),
    ("pis_rib", "1.0", "Nervio del pisador que apoya en el PCB"),
    ("pis_in", "2.6", "Cuanto pisa el nervio dentro del borde del PCB"),
    ("pis_pre", "0.15", "Precarga del pisador sobre el PCB"),
    ("boss_top", "=oled_d0 + oled_pcb_t + pis_rib - pis_pre", "Profundidad de la cara de apoyo de las torretas"),
    # Encoder EC11
    ("enc_x", "24", "Centro X del encoder (izquierda, mano derecha libre)"),
    ("enc_s", "36", "Centro encoder a lo largo de la pendiente"),
    ("enc_hole", "7.4", "Agujero casquillo M7 del EC11"),
    ("enc_pcb_d", "23.6", "Diametro PCB redondo del encoder (STEP ec11 v2)"),
    ("enc_body_h", "6.5", "Altura cuerpo EC11 (STEP): el PCB queda a esta distancia del panel"),
    # Placas
    ("pcb_t", "1.6", "Grosor PCB del ESP32/TP4056"),
    ("corral_rw", "1.2", "Grosor carriles del portapilas"),
    ("hook_t", "1.6", "Ganchos de clip: grosor (flexion)"),
    ("hook_lw", "0.8", "Ganchos de clip: cuanto vuela la una sobre el hueco de la placa"),
    ("hook_lh", "1.2", "Ganchos de clip: alto de la una (chaflan de entrada)"),
    # ESP32 WeAct (pines y dupont hacia ABAJO)
    ("esp_L", "62.7", "WeAct ESP32-S3-A largo (STEP oficial)"),
    ("esp_W", "31.2", "WeAct ESP32-S3-A ancho (STEP oficial)"),
    ("esp_lift", "22", "Hueco bajo el PCB del ESP32 (pines 15 + dupont = 20, +2)"),
    ("esp_z", "=Lt + esp_lift", "Z de la cara inferior del PCB del ESP32 (calculada)"),
    ("esp_cy", "=D - tw - clr - lip_t - 0.4 - hook_t - clr - esp_W/2", "Centro Y del ESP32"),
    ("esp_gap", "1.0", "Hueco placa-pared en el lado del USB (el USB-C sobresale 0.77)"),
    ("esp_usb_sep", "11.2", "Separacion entre los 2 USB-C del WeAct (0 = solo uno)"),
    ("esp_usb_z", "2.7", "Centro de los USB-C sobre la cara inferior del PCB"),
    ("esp_ant", "6.2", "Voladizo del modulo WROOM mas alla del PCB (lado antena)"),
    ("esp_fork", "2.5", "Cuanto entra el PCB en la horquilla del lado de la antena"),
    # TP4056 (pines hacia ARRIBA)
    ("tp_L", "28.3", "TP4056 largo (STEP)"),
    ("tp_W", "17.4", "TP4056 ancho (STEP)"),
    ("tp_cx", "41", "Centro X del TP4056 (USB-C sale por detras)"),
    ("tp_gap", "1.2", "Hueco placa-pared en el lado del USB (el USB-C sobresale 1.0)"),
    ("tp_pad", "6", "Altura de la cara inferior del TP4056 sobre la tapa"),
    ("tp_z", "=Lt + tp_pad", "Z de la cara inferior del PCB del TP4056 (calculada)"),
    ("tp_notch", "4.7", "Muescas del TP4056: distancia al eje (STEP, D4)"),
    ("tp_peg_d", "3.4", "Pivotes que entran en las muescas"),
    ("usb_w", "12.5", "Recorte USB-C ancho (cabe el moldeado del cable)"),
    ("usb_h", "7.0", "Recorte USB-C alto"),
    ("usb_zc", "=tp_z + 2.75", "Altura del centro del USB-C del TP4056 (STEP: 2.75 sobre cara inferior)"),
    # Bateria 18650 en portapilas (IGES: 76 x 21.2 x 18.2, agujero central, pines 5.5 en los extremos)
    ("bat_d", "18.6", "Diametro celda 18650"),
    ("bat_len", "65", "Largo celda"),
    ("bh_L", "76", "Portapilas largo (sin pines)"),
    ("bh_W", "21.2", "Portapilas ancho"),
    ("bh_H", "18.2", "Portapilas alto"),
    ("bh_pin", "5.5", "Pines de soldadura que sobresalen por cada extremo"),
    ("bh_x0", "16", "X del extremo izquierdo del portapilas (deja sitio al pin y al poste)"),
    ("bh_cy", "=tw + clr + lip_t + 0.3 + corral_rw + clr + bh_W/2", "Centro Y del portapilas (delante)"),
    ("bh_hole", "38", "Posicion del agujero de fijacion desde el extremo izquierdo"),
    ("bh_rail_h", "4", "Altura carriles laterales del portapilas"),
    # Interruptor ON/OFF
    ("sw_d", "6.4", "Agujero interruptor de palanca MTS-103 (rosca M6)"),
    ("sw_body", "18", "Fondo del cuerpo + terminales del MTS-103 hacia dentro (referencia)"),
    ("sw_cx", "20", "Centro X del interruptor (pared trasera)"),
    ("sw_cz", "40", "Centro Z del interruptor"),
]

sh = doc.addObject("Spreadsheet::Sheet", "Parametros")
sh.set("A1", "Parametro")
sh.set("B1", "Valor")
sh.set("C1", "Descripcion")
for i, (alias, val, desc) in enumerate(PARAMS, start=2):
    sh.set("A%d" % i, alias)
    sh.set("B%d" % i, val)
    sh.set("C%d" % i, desc)
    sh.setAlias("B%d" % i, alias)
doc.recompute()

P = "Parametros."


def p(name):
    return P + name


def E(o, prop, expr):
    o.setExpression(prop, expr)


def hide(*objs):
    for o in objs:
        o.Visibility = False


def box(name, L, Wd, H, x, y, z, label=None):
    b = doc.addObject("Part::Box", name)
    E(b, "Length", L)
    E(b, "Width", Wd)
    E(b, "Height", H)
    E(b, "Placement.Base.x", x)
    E(b, "Placement.Base.y", y)
    E(b, "Placement.Base.z", z)
    if label:
        b.Label = label
    return b


def cyl(name, r, h, x, y, z, label=None):
    c = doc.addObject("Part::Cylinder", name)
    E(c, "Radius", r)
    E(c, "Height", h)
    E(c, "Placement.Base.x", x)
    E(c, "Placement.Base.y", y)
    E(c, "Placement.Base.z", z)
    if label:
        c.Label = label
    return c


def panel(o, u, s, d):
    """Coloca o en el sistema del panel inclinado (u, s, d). El +Z local apunta hacia fuera."""
    E(o, "Placement.Base.x", "(%s)" % u)
    E(o, "Placement.Base.y", "(%s)*cos(%s) + (%s)*sin(%s)" % (s, p("ang"), d, p("ang")))
    E(o, "Placement.Base.z", "%s + (%s)*sin(%s) - (%s)*cos(%s)" % (p("Hf"), s, p("ang"), d, p("ang")))
    o.Placement.Rotation = App.Rotation(App.Vector(1, 0, 0), 25)
    E(o, "Placement.Rotation.Angle", p("ang"))
    return o


def pbox(name, L, Wd, H, u, s, d, label=None):
    """Caja en coordenadas de panel: u..u+L, s..s+Wd, profundidad d-H..d."""
    b = doc.addObject("Part::Box", name)
    E(b, "Length", L)
    E(b, "Width", Wd)
    E(b, "Height", H)
    if label:
        b.Label = label
    return panel(b, u, s, d)


def pcyl(name, r, h, u, s, d, label=None):
    """Cilindro en coordenadas de panel: profundidad d-h..d."""
    c = doc.addObject("Part::Cylinder", name)
    E(c, "Radius", r)
    E(c, "Height", h)
    if label:
        c.Label = label
    return panel(c, u, s, d)


def cut(name, base, tool, label=None):
    c = doc.addObject("Part::Cut", name)
    c.Base = base
    c.Tool = tool
    hide(base, tool)
    if label:
        c.Label = label
    return c


def fuse(name, shapes, label=None):
    f = doc.addObject("Part::MultiFuse", name)
    f.Shapes = shapes
    f.Refine = True
    hide(*shapes)
    if label:
        f.Label = label
    return f


# Direccion de la una de un gancho -> imagen de los ejes locales del Part::Wedge
# (X local = extrusion, Y local = +Z global, Z local = direccion de la una)
_HOOK_ROT = {
    "-y": ((1, 0, 0), (0, -1, 0)),
    "+y": ((-1, 0, 0), (0, 1, 0)),
    "+x": ((0, 1, 0), (1, 0, 0)),
    "-x": ((0, -1, 0), (-1, 0, 0)),
}


def hook(name, col, lip_dir, a0, a1, face, zlip):
    """Gancho de clip impreso con la tapa.
    col: (x, y, z, L, W, H) expresiones de la columna (caja).
    lip_dir: '+x','-x','+y','-y' hacia donde vuela la una (hacia la placa).
    a0..a1: tramo a lo largo de la arista de la placa; face: coordenada de la
    cara interior de la columna; zlip: Z de la cara inferior de la una."""
    c = box(name + "_col", col[3], col[4], col[5], col[0], col[1], col[2])
    w = doc.addObject("Part::Wedge", name + "_una")
    L = "(%s) - (%s)" % (a1, a0)
    for prop, val in (("Xmin", "0"), ("Xmax", L), ("X2min", "0"), ("X2max", L),
                      ("Ymin", "0"), ("Ymax", p("hook_lh")),
                      ("Zmin", "0"), ("Zmax", p("hook_lw")), ("Z2min", "0"), ("Z2max", "0.01")):
        E(w, prop, val)
    xi, zi = _HOOK_ROT[lip_dir]
    m = App.Matrix(xi[0], 0, zi[0], 0,
                   xi[1], 0, zi[1], 0,
                   xi[2], 1, zi[2], 0,
                   0, 0, 0, 1)
    w.Placement = App.Placement(m)
    start = a0 if (xi[0] + xi[1]) > 0 else a1
    if lip_dir in ("+y", "-y"):
        E(w, "Placement.Base.x", start)
        E(w, "Placement.Base.y", face)
    else:
        E(w, "Placement.Base.x", face)
        E(w, "Placement.Base.y", start)
    E(w, "Placement.Base.z", zlip)
    return [c, w]


SLOPE_L = "%s/cos(%s) + 20" % (p("D"), p("ang"))
rw = p("corral_rw")
cl = p("clr")

# --------------------------------------------------------------------------
# CARCASA (cuerpo superior en cuna). Baja hasta Z=0: la tapa va dentro.
# --------------------------------------------------------------------------
outer_box = box("ExtBox", p("Wc"), p("D"), "%s + 10" % p("Hb"), "0", "0", "0")
cutter_o = pbox("CuchillaExt", "%s + 2" % p("Wc"), SLOPE_L, "200", "-1", "-5", "0")
outer = cut("Exterior", outer_box, cutter_o)

inner_box = box("IntBox", "%s - 2*%s" % (p("Wc"), p("tw")), "%s - 2*%s" % (p("D"), p("tw")),
                "%s + 20" % p("Hb"), p("tw"), p("tw"), "-1")
cutter_i = pbox("CuchillaInt", "%s + 2" % p("Wc"), SLOPE_L, "200", "-1", "-5", p("tp"))
inner = cut("Interior", inner_box, cutter_i)

hull = cut("Cascara", outer, inner)

# Postes de tornillo (insertos M3 desde abajo)
pr = "(%s/2 - 1)" % p("post_d")  # centro a radio-1 de la pared: el poste se funde con ella
prr = "(%s/2)" % p("post_d")
POSTS = [
    ("%s + %s" % (p("tw"), pr), "%s + %s" % (p("tw"), pr)),                          # frontal izq.
    ("%s + %s" % (p("tw"), pr), p("post_yl")),                                     # lateral izq.
    ("%s + %s" % (p("tw"), pr), "%s - %s - %s" % (p("D"), p("tw"), pr)),            # trasero izq.
    ("%s - %s - %s" % (p("Wc"), p("tw"), pr), "%s + %s" % (p("tw"), pr)),           # frontal der.
    ("%s - %s - %s" % (p("Wc"), p("tw"), pr), p("post_yr")),                        # lateral der.
]
# (la esquina trasera derecha la ocupa el ESP32 con sus USB-C)
posts = []
inserts = []
for i, (px, py) in enumerate(POSTS, start=1):
    posts.append(cyl("Poste%d" % i, prr, p("Hp"), px, py, p("Lt")))
    inserts.append(cyl("Inserto%d" % i, "%s/2" % p("ins_d"), "%s + 1" % p("ins_h"),
                       px, py, "%s - 1" % p("Lt")))

# OLED encajonada ---------------------------------------------------------
# Topes: la cara del PCB apoya en dos regletas a izquierda y derecha de la ventana.
win_w = "(%s + 2*%s)" % (p("mod_w"), cl)
win_h = "(%s + 2*%s)" % (p("mod_h"), cl)
win_s0 = "(%s + %s - %s/2)" % (p("oled_s"), p("glass_off"), win_h)
stop_u0 = "(%s/2 + 0.3)" % win_w             # desde el centro OLED
stop_h = "(%s - %s + 0.3)" % (p("oled_d0"), p("tp"))
stops = []
for k, sg in ((1, -1), (2, 1)):
    u0 = ("%s + %s" % (p("oled_x"), stop_u0)) if sg > 0 else \
         ("%s - %s - %s" % (p("oled_x"), stop_u0, p("stop_w")))
    stops.append(pbox("TopeOLED%d" % k, p("stop_w"), "%s - 4" % p("oled_h"), stop_h,
                      u0, "%s - %s/2 + 2" % (p("oled_s"), p("oled_h")), p("oled_d0")))
# Torretas de los pisadores (fuera del PCB), con inserto M2.5 desde dentro
bosses = []
bholes = []
k = 0
for su in ("-", "+"):
    for ss in ("-", "+"):
        k += 1
        bu = "%s %s %s" % (p("oled_x"), su, p("boss_u"))
        bs = "%s %s %s" % (p("oled_s"), ss, p("boss_s"))
        bosses.append(pcyl("TorretaOLED%d" % k, "%s/2" % p("boss_d"),
                           "%s - %s + 0.5" % (p("boss_top"), p("tp")), bu, bs, p("boss_top")))
        bholes.append(pcyl("InsertoOLED%d" % k, "%s/2" % p("ins25_d"), "%s + 1" % p("ins25_h"),
                           bu, bs, "%s + 1" % p("boss_top")))

body = fuse("CascaraPostes", [hull] + posts + stops + bosses)

# Recortes
window = pbox("VentanaOLED", win_w, win_h, "%s + 2" % p("tp"),
              "%s - %s/2" % (p("oled_x"), win_w), win_s0, "%s + 1" % p("tp"))
# Rebaje para los pines del header que asoman por delante del PCB (STEP: x +-5.08, s 20.93..23.47)
hdr = pbox("RebajeHeader", "13", "4.6", "%s + 1" % p("tp"),
           "%s - 6.5" % p("oled_x"), "%s + 20.4" % p("oled_s"),
           "%s + 1" % p("tp"))
E(hdr, "Height", "%s + 1 - (%s - %s - 0.4)" % (p("tp"), p("oled_d0"), p("hdr_front")))
enc_hole = pcyl("AgujeroEncoder", "%s/2" % p("enc_hole"), "%s + 2" % p("tp"),
                p("enc_x"), p("enc_s"), "%s + 1" % p("tp"))
usb_tp = box("USB_TP4056", p("usb_w"), "%s + 2" % p("tw"), p("usb_h"),
             "%s - %s/2" % (p("tp_cx"), p("usb_w")), "%s - %s - 1" % (p("D"), p("tw")),
             "%s - %s/2" % (p("usb_zc"), p("usb_h")))
esp_slot = "(%s + %s)" % (p("usb_w"), p("esp_usb_sep"))
usb_esp = box("USB_ESP32", "%s + 2" % p("tw"), esp_slot, p("usb_h"),
              "%s - %s - 1" % (p("Wc"), p("tw")), "%s - %s/2" % (p("esp_cy"), esp_slot),
              "%s + %s - %s/2" % (p("esp_z"), p("esp_usb_z"), p("usb_h")))
switch = cyl("Interruptor", "%s/2" % p("sw_d"), "%s + 2" % p("tw"),
             p("sw_cx"), "%s - %s - 1" % (p("D"), p("tw")), p("sw_cz"))
switch.Placement.Rotation = App.Rotation(App.Vector(1, 0, 0), -90)  # eje +Y
# Galce de la base: la tapa entra en la pared reb_w y queda enrasada abajo
gal_o = "(%s - %s)" % (p("tw"), p("reb_w"))
galce = box("Galce", "%s - 2*%s" % (p("Wc"), gal_o), "%s - 2*%s" % (p("D"), gal_o), "%s + 1" % p("Lt"),
            gal_o, gal_o, "-1")
cutters = fuse("Recortes", inserts + bholes + [window, hdr, enc_hole, usb_tp, usb_esp, switch, galce])
carcasa = cut("Carcasa", body, cutters, label="Carcasa")

# --------------------------------------------------------------------------
# PISADORES de la OLED (2 piezas): apoyan en las torretas y aprietan el borde del PCB
# --------------------------------------------------------------------------
pis_len = "(2*%s + %s + 2)" % (p("boss_s"), p("boss_d"))
pis_s0 = "(%s - %s/2)" % (p("oled_s"), pis_len)
pis_w = "(%s - %s/2 + %s + %s/2)" % (p("boss_u"), p("oled_w"), p("pis_in"), p("boss_d"))
pisadores = []
for nm, sg in (("PisadorOLED_izq", -1), ("PisadorOLED_der", 1)):
    if sg > 0:
        u0 = "%s + %s/2 - %s" % (p("oled_x"), p("oled_w"), p("pis_in"))
        r0 = u0
    else:
        u0 = "%s - %s/2 + %s - %s" % (p("oled_x"), p("oled_w"), p("pis_in"), pis_w)
        r0 = "%s - %s/2" % (p("oled_x"), p("oled_w"))
    plate_ = pbox(nm + "_placa", pis_w, pis_len, p("pis_t"), u0, pis_s0,
                  "%s + %s" % (p("boss_top"), p("pis_t")))
    rib_ = pbox(nm + "_nervio", "%s - 0.2" % p("pis_in"), "%s - 8" % p("oled_h"), p("pis_rib"),
                ("%s + 0.2" % r0) if sg < 0 else r0, "%s - %s/2 + 4" % (p("oled_s"), p("oled_h")),
                p("boss_top"))
    if sg > 0:
        E(rib_, "Placement.Base.x", "%s + %s/2 - %s" % (p("oled_x"), p("oled_w"), p("pis_in")))
    holes_ = []
    for ss in ("-", "+"):
        holes_.append(pcyl(nm + "_paso%s" % ("a" if ss == "-" else "b"), "%s/2" % p("scr25_d"),
                           "%s + 2" % p("pis_t"),
                           "%s %s %s" % (p("oled_x"), "+" if sg > 0 else "-", p("boss_u")),
                           "%s %s %s" % (p("oled_s"), ss, p("boss_s")),
                           "%s + %s + 1" % (p("boss_top"), p("pis_t"))))
    pf = fuse(nm + "_bruto", [plate_, rib_])
    pisadores.append(cut(nm, pf, fuse(nm + "_pasos", holes_), label=nm))

# --------------------------------------------------------------------------
# TAPA (base encajada en el galce, con labio de centrado, portapilas y soportes)
# --------------------------------------------------------------------------
lid_o = "(%s + %s)" % (gal_o, cl)
plate = box("Placa", "%s - 2*%s" % (p("Wc"), lid_o), "%s - 2*%s" % (p("D"), lid_o), p("Lt"),
            lid_o, lid_o, "0")
# Labio de centrado ("cuna"): anillo pegado por dentro a las paredes
lo = "(%s + %s)" % (p("tw"), cl)
lip_ext = box("Labio_ext", "%s - 2*%s" % (p("Wc"), lo), "%s - 2*%s" % (p("D"), lo),
              "%s + 0.5" % p("lip_h"), lo, lo, "%s - 0.5" % p("Lt"))
li = "(%s + %s)" % (lo, p("lip_t"))
lip_int = box("Labio_int", "%s - 2*%s" % (p("Wc"), li), "%s - 2*%s" % (p("D"), li),
              "%s + 2" % p("lip_h"), li, li, "%s - 1" % p("Lt"))
lip_post_cuts = []
for i, (px, py) in enumerate(POSTS, start=1):
    lip_post_cuts.append(cyl("Labio_hueco%d" % i, "%s/2 + %s + 0.2" % (p("post_d"), cl),
                             "%s + 2" % p("lip_h"), px, py, "%s - 1" % p("Lt")))
lip_ring = cut("Labio_anillo", lip_ext, lip_int)
lip = cut("Labio", lip_ring, fuse("Labio_huecos", lip_post_cuts))

# Portapilas 18650 a lo largo de X, delante. Dos carriles laterales lo centran.
bh_y0 = "(%s - %s/2 - %s)" % (p("bh_cy"), p("bh_W"), cl)
bh_y1 = "(%s + %s/2 + %s)" % (p("bh_cy"), p("bh_W"), cl)
rail_l = "(%s - 10)" % p("bh_L")
rail1 = box("Portapilas_Carril1", rail_l, rw, p("bh_rail_h"),
            "%s + 5" % p("bh_x0"), "%s - %s" % (bh_y0, rw), p("Lt"))
rail2 = box("Portapilas_Carril2", rail_l, rw, p("bh_rail_h"),
            "%s + 5" % p("bh_x0"), bh_y1, p("Lt"))
cradle = fuse("PortapilasCarriles", [rail1, rail2])

# ESP32 ------------------------------------------------------------------
# Hueco de la placa (con holgura): x e_x0..e_x1, y e_y0..e_y1. USB-C a la derecha.
e_x1 = "(%s - %s - %s + %s)" % (p("Wc"), p("tw"), p("esp_gap"), cl)
e_x0 = "(%s - %s - 2*%s)" % (e_x1, p("esp_L"), cl)
e_y0 = "(%s - %s/2 - %s)" % (p("esp_cy"), p("esp_W"), cl)
e_y1 = "(%s + %s/2 + %s)" % (p("esp_cy"), p("esp_W"), cl)
ez = p("esp_z")
esp_parts = []
# Bastidor central bajo el PCB (entre las dos filas de dupont, a 13.97 del eje)
bst_x0 = "(%s + 7)" % e_x0
bst_l = "(%s - %s - 14)" % (e_x1, e_x0)
bst_y0 = "(%s - 9)" % p("esp_cy")
bst_o = box("ESP_Bastidor_ext", bst_l, "18", "%s - %s" % (ez, p("Lt")), bst_x0, bst_y0, p("Lt"))
bst_i = box("ESP_Bastidor_int", "%s - 4" % bst_l, "14", "%s + 2" % ez, "%s + 2" % bst_x0,
            "%s + 2" % bst_y0, "%s - 1" % p("Lt"))
esp_parts.append(cut("ESP_Bastidor", bst_o, bst_i))
# Horquilla en el lado de la antena (dos esquinas, fuera del modulo WROOM)
for k, (ya, yb) in enumerate(((("%s - 1.5" % e_y0), ("%s + 2.6" % e_y0)),
                              (("%s - 2.6" % e_y1), ("%s + 1.5" % e_y1))), start=1):
    fo = box("ESP_Horquilla%d_bloque" % k, "3 + %s" % p("esp_fork"), "(%s) - (%s)" % (yb, ya),
             "%s - %s + %s + 0.3 + 1.5" % (ez, p("Lt"), p("pcb_t")),
             "%s - 3" % e_x0, ya, p("Lt"))
    fs = box("ESP_Horquilla%d_ranura" % k, "%s + 1" % p("esp_fork"), "(%s) - (%s) + 2" % (yb, ya),
             "%s + 0.3" % p("pcb_t"), e_x0, "%s - 1" % ya, ez)
    esp_parts.append(cut("ESP_Horquilla%d" % k, fo, fs))
# Ganchos de clip en el lado del USB (los pines empiezan a 3.3 mm del borde)
hz = "%s + %s + 0.15" % (ez, p("pcb_t"))
hx0 = "(%s - 4.0)" % e_x1
hx1 = "(%s - 1.2)" % e_x1
hcol_h = "%s + %s - %s" % (hz, p("hook_lh"), p("Lt"))
esp_parts += hook("ESP_Gancho1", (hx0, "%s - %s" % (e_y0, p("hook_t")), p("Lt"),
                                  "%s - %s" % (hx1, hx0), p("hook_t"), hcol_h),
                  "+y", hx0, hx1, e_y0, hz)
esp_parts += hook("ESP_Gancho2", (hx0, e_y1, p("Lt"), "%s - %s" % (hx1, hx0), p("hook_t"), hcol_h),
                  "-y", hx0, hx1, e_y1, hz)

# TP4056 -----------------------------------------------------------------
# A lo largo de Y, USB-C hacia la pared trasera; muescas en el extremo delantero.
t_y1 = "(%s - %s - %s + %s)" % (p("D"), p("tw"), p("tp_gap"), cl)
t_y0 = "(%s - %s - 2*%s)" % (t_y1, p("tp_L"), cl)
t_x0 = "(%s - %s/2 - %s)" % (p("tp_cx"), p("tp_W"), cl)
t_x1 = "(%s + %s/2 + %s)" % (p("tp_cx"), p("tp_W"), cl)
t_yc = "(%s + %s/2 + %s)" % (t_y0, p("tp_L"), cl)    # centro de la placa
tz = p("tp_z")
tp_parts = []
# Carriles de apoyo bajo los bordes largos, en la zona central (sin soldaduras)
for k, xa in ((1, "%s + %s" % (t_x0, cl)), (2, "%s - %s - 2.2" % (t_x1, cl))):
    tp_parts.append(box("TP_Apoyo%d" % k, "2.2", "14", "%s - %s" % (tz, p("Lt")),
                        xa, "%s - 8" % t_yc, p("Lt")))
# Pivotes en las muescas (sujetan en X e Y; la pared trasera hace de tope del USB)
for k, sg in ((1, "-"), (2, "+")):
    tp_parts.append(cyl("TP_Pivote%d" % k, "%s/2" % p("tp_peg_d"),
                        "%s - %s + %s + 0.8" % (tz, p("Lt"), p("pcb_t")),
                        "%s %s %s" % (p("tp_cx"), sg, p("tp_notch")), "%s + %s" % (t_y0, cl), p("Lt")))
# Ganchos de clip en los bordes largos (STEP x -4.0..-1.8: libre de componentes)
thz = "%s + %s + 0.15" % (tz, p("pcb_t"))
ty0 = "(%s - 4.0)" % t_yc
ty1 = "(%s - 1.8)" % t_yc
tcol_h = "%s + %s - %s" % (thz, p("hook_lh"), p("Lt"))
tp_parts += hook("TP_Gancho1", ("%s - %s" % (t_x0, p("hook_t")), ty0, p("Lt"),
                                p("hook_t"), "%s - %s" % (ty1, ty0), tcol_h),
                 "+x", ty0, ty1, t_x0, thz)
tp_parts += hook("TP_Gancho2", (t_x1, ty0, p("Lt"), p("hook_t"), "%s - %s" % (ty1, ty0), tcol_h),
                 "-x", ty0, ty1, t_x1, thz)

lid_body = fuse("TapaCuerpo", [plate, lip, cradle] + esp_parts + tp_parts)
holes = []
for i, (px, py) in enumerate(POSTS, start=1):
    holes.append(cyl("PasoM3_%d" % i, "%s/2" % p("screw_d"), "%s + 2" % p("Lt"), px, py, "-1"))
    holes.append(cyl("Cajeado_%d" % i, "%s/2" % p("cb_d"), "%s + 1" % p("cb_h"), px, py, "-1"))
bhx = "%s + %s" % (p("bh_x0"), p("bh_hole"))
holes.append(cyl("PasoM3_Portapilas", "%s/2" % p("screw_d"), "%s + 2" % p("Lt"), bhx, p("bh_cy"), "-1"))
holes.append(cyl("Cajeado_Portapilas", "%s/2" % p("cb_d"), "%s + 1" % p("cb_h"), bhx, p("bh_cy"), "-1"))
# Rebaje de 0.8 mm bajo la celda: asoma unas decimas por la ventana del fondo del portapilas
holes.append(box("RebajeCelda", "%s + 2" % p("bat_len"), "10", "1.8",
                 "%s - (%s + 2)/2" % (bhx, p("bat_len")), "%s - 5" % p("bh_cy"),
                 "%s - 0.8" % p("Lt")))
lid_holes = fuse("TapaAgujeros", holes)
tapa = cut("Tapa", lid_body, lid_holes, label="Tapa")

# --------------------------------------------------------------------------
# Componentes de referencia (NO se imprimen; sirven para comprobar choques)
# --------------------------------------------------------------------------
import Import  # noqa: E402

ref = doc.addObject("App::DocumentObjectGroup", "Componentes_referencia")
refs = []


def load_step_compound(path, matrix=None):
    tmp = App.newDocument("tmp_load")
    Import.insert(path, tmp.Name)
    shapes = []
    for o in tmp.Objects:
        if o.TypeId == "Part::Feature" and o.Shape.Solids:
            s_ = o.Shape.copy()
            s_.Placement = o.getGlobalPlacement()
            shapes.append(s_)
    App.closeDocument(tmp.Name)
    comp = Part.makeCompound(shapes)
    if matrix is not None:
        comp = comp.transformGeometry(matrix)
    return comp


OLED_STEP = os.path.join(HERE, "ref_oled", "OLED_2.42_IIC_v1.1.step")
if os.path.exists(OLED_STEP):
    # STEP: PCB en el plano XZ con la cara frontal en y=0, cristal hacia +y, header en +z.
    # Se lleva a ejes de panel (u=-x, s=z, normal exterior=+y) y luego se coloca con panel().
    oled_ref = doc.addObject("Part::Feature", "Ref_OLED")
    oled_ref.Shape = load_step_compound(
        OLED_STEP, App.Matrix(-1, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 1))
    refs.append(panel(oled_ref, p("oled_x"), p("oled_s"), p("oled_d0")))
else:
    refs.append(pbox("Ref_OLED", p("oled_w"), p("oled_h"), "%s + %s" % (p("mod_t"), p("oled_pcb_t")),
                     "%s - %s/2" % (p("oled_x"), p("oled_w")), "%s - %s/2" % (p("oled_s"), p("oled_h")),
                     "%s + %s" % (p("oled_d0"), p("oled_pcb_t"))))
# Dupont del header de 4 pines (borde superior, s 20.93..23.47 del centro). Los pines
# van DOBLADOS 90 grados hacia +s (hacia la pared trasera) justo sobre el plastico del
# header (que acaba a 3.46 de la cara del PCB): el dupont queda paralelo al panel.
# Caja: 4 dupont (10.2 de ancho, 2.54 de alto) + 14 de carcasa + 4 de curva de los cables.
refs.append(pbox("Ref_OLED_dupont", "12", "18", "3.0",
                 "%s - 6" % p("oled_x"), "%s + 23.5" % p("oled_s"),
                 "%s + 3.46 + 3.0" % p("oled_d0")))
ENC_STEP = os.path.join(HERE, "ref_encoder", "ec11_v2.step")
if os.path.exists(ENC_STEP):
    # STEP: eje en +z, cara superior del PCB en z=0, pines en y=-8.1.
    # Girado 180 grados en z para que los pines miren a +s (hacia atras).
    enc_ref = doc.addObject("Part::Feature", "Ref_Encoder")
    enc_ref.Shape = load_step_compound(
        ENC_STEP, App.Matrix(-1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1))
    refs.append(panel(enc_ref, p("enc_x"), p("enc_s"), "%s + %s" % (p("tp"), p("enc_body_h"))))
else:
    refs.append(pbox("Ref_Encoder_cuerpo", "12", "11.8", p("enc_body_h"),
                     "%s - 6" % p("enc_x"), "%s - 5.9" % p("enc_s"),
                     "%s + %s" % (p("tp"), p("enc_body_h"))))
refs.append(pbox("Ref_Encoder_dupont", "14", "8", "20",
                 "%s - 7" % p("enc_x"), "%s + 8.1 - 4" % p("enc_s"),
                 "%s + %s + 4.1 + 20" % (p("tp"), p("enc_body_h"))))
refs.append(pcyl("Ref_Mando", "12", "16", p("enc_x"), p("enc_s"), "-1"))
# ESP32 + header y dupont por DEBAJO (2x22 pines a +-13.97 del centro, de 3.3 a 59.2 mm del borde USB)
refs.append(box("Ref_ESP32", p("esp_L"), p("esp_W"), p("pcb_t"),
                "%s + %s" % (e_x0, cl), "%s + %s" % (e_y0, cl), ez))
for k, sg in ((1, "-"), (2, "+")):
    refs.append(box("Ref_ESP32_dupont%d" % k, "55.9", "2.6", "20",
                    "%s - %s - 59.2" % (e_x1, cl), "%s %s 13.97 - 1.3" % (p("esp_cy"), sg),
                    "%s - 20" % ez))
refs.append(box("Ref_ESP32_WROOM", "25.5", "18", "3.1",
                "%s + %s - %s" % (e_x0, cl, p("esp_ant")), "%s - 9" % p("esp_cy"),
                "%s + %s" % (ez, p("pcb_t"))))
sw_ref = box("Ref_Interruptor", "8", p("sw_body"), "13",
             "%s - 4" % p("sw_cx"), "%s - %s - %s" % (p("D"), p("tw"), p("sw_body")),
             "%s - 6.5" % p("sw_cz"))
refs.append(sw_ref)
# TP4056 + dupont ARRIBA en las pastillas del extremo de las muescas (a 3.5 mm del borde)
refs.append(box("Ref_TP4056", p("tp_W"), p("tp_L"), p("pcb_t"),
                "%s + %s" % (t_x0, cl), "%s + %s" % (t_y0, cl), tz))
refs.append(box("Ref_TP4056_dupont", p("tp_W"), "3", "17",
                "%s + %s" % (t_x0, cl), "%s + %s + 2.2" % (t_y0, cl), "%s + %s" % (tz, p("pcb_t"))))
bh_zc = "(%s + %s/2)" % (p("Lt"), p("bh_H"))
bat = cyl("Ref_18650", "%s/2" % p("bat_d"), p("bat_len"),
          "%s + %s - %s/2" % (p("bh_x0"), p("bh_hole"), p("bat_len")), p("bh_cy"), bh_zc)
bat.Placement.Rotation = App.Rotation(App.Vector(0, 1, 0), 90)
refs.append(bat)
BH_IGES = os.path.join(HERE, "ref_18650", "portapilas_18650.iges")
if os.path.exists(BH_IGES):
    bh = doc.addObject("Part::Feature", "Ref_Portapilas")
    bh.Shape = Part.read(BH_IGES)
    bh.Placement.Rotation = App.Rotation(App.Vector(0, 0, 1), -90)
    E(bh, "Placement.Base.x", p("bh_x0"))
    E(bh, "Placement.Base.y", p("bh_cy"))
    E(bh, "Placement.Base.z", bh_zc)
    refs.append(bh)
else:
    refs.append(box("Ref_Portapilas", p("bh_L"), p("bh_W"), p("bh_H"),
                    p("bh_x0"), "%s - %s/2" % (p("bh_cy"), p("bh_W")), p("Lt")))
for r in refs:
    ref.addObject(r)

doc.recompute()

# --------------------------------------------------------------------------
# Comprobaciones
# --------------------------------------------------------------------------
bad = [o.Name for o in doc.Objects if "Invalid" in o.State]
print("OBJETOS_INVALIDOS:", bad)
PIEZAS = [carcasa, tapa] + pisadores
for o in PIEZAS:
    s = o.Shape
    print("%s: valido=%s solidos=%d vol=%.0f bbox=%s" % (o.Label, s.isValid(), len(s.Solids), s.Volume, s.BoundBox))
print("Hb = %.2f" % sh.get("Hb"))


def group(n):
    for g_ in ("Ref_Encoder", "Ref_OLED", "Ref_ESP32", "Ref_TP4056"):
        if n.startswith(g_):
            return g_
    return n


parts = PIEZAS + refs
print("CHOQUES (volumen comun > 0.5 mm3):")
for i in range(len(parts)):
    for j in range(i + 1, len(parts)):
        a, b = parts[i], parts[j]
        if group(a.Name) == group(b.Name):
            continue
        if {a.Name, b.Name} == {"Ref_18650", "Ref_Portapilas"}:
            continue
        try:
            v = a.Shape.common(b.Shape).Volume
        except Exception:  # noqa
            v = -1
        if v > 0.5 or v < 0:
            print("  %s x %s : %.1f mm3" % (a.Name, b.Name, v))

doc.saveAs(FCSTD)

for o in PIEZAS:
    Mesh.export([o], os.path.join(HERE, "%s.stl" % o.Label))
Part.export(PIEZAS, os.path.join(HERE, "caja_display_timemore.step"))
print("GUARDADO:", FCSTD)

# --------------------------------------------------------------------------
# Comprobacion con los STEP reales de las placas (si estan en ref_*/)
# --------------------------------------------------------------------------
g = lambda a: float(sh.get(a))  # noqa: E731
others = [(o.Label, o.Shape) for o in PIEZAS] + \
    [(r.Name, r.Shape) for r in refs if not r.Name.startswith(("Ref_ESP32", "Ref_TP4056"))]
REAL = [
    ("WeAct", os.path.join(HERE, "ref_weact", "WeAct_ESP32S3-A.step"),
     # STEP: USB en x=0, centro de la placa en y=15.24 -> girado 180 grados
     App.Placement(App.Vector(g("Wc") - g("tw") - g("esp_gap"), g("esp_cy") + 15.24, g("esp_z")),
                   App.Rotation(App.Vector(0, 0, 1), 180))),
    ("TP4056", os.path.join(HERE, "ref_tp4056", "TP4056_Charging_Module_Type_C.step"),
     # STEP: centrado en el origen, USB hacia +X -> girado 90 grados (USB hacia atras)
     App.Placement(App.Vector(g("tp_cx"), g("D") - g("tw") - g("tp_gap") - g("tp_L") / 2, g("tp_z")),
                   App.Rotation(App.Vector(0, 0, 1), 90))),
]
for nm, path, pl in REAL:
    if not os.path.exists(path):
        continue
    tmp = App.newDocument("tmp_" + nm)
    Import.insert(path, tmp.Name)
    hits = 0
    for o in tmp.Objects:
        if o.TypeId != "Part::Feature" or not o.Shape.Solids:
            continue
        s = o.Shape.copy()
        s.Placement = o.getGlobalPlacement()
        s = s.transformed(pl.toMatrix())
        for on, osh in others:
            v = s.common(osh).Volume
            if v > 0.05:
                hits += 1
                print("  CHOQUE REAL %s/%s x %s : %.2f mm3" % (nm, o.Label[:25], on, v))
    print("MODELO REAL %s: %d choques" % (nm, hits))
    App.closeDocument(tmp.Name)
