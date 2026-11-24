# -*- coding: utf-8 -*-
"""
Renders y animación del ensamble con el visor 3D de FreeCAD (iluminación y sombreado reales).

Ejecutar DESPUÉS de generar_ensamble.py, con la interfaz gráfica (no con freecadcmd):
    "C:\\Program Files\\FreeCAD 1.1\\bin\\freecad.exe" generar_renders.py

Abre una ventana de FreeCAD, renderiza y se cierra sola. Crea en renders/:
    raw/montado.png, raw/explosionado.png, raw/interior.png, raw/trasera.png   (1600 x 1200)
    frames/f_000.png ...                                       (fotogramas de la animación)
y deja "renders/LISTO.txt" al terminar. La animación se monta con ffmpeg (ver LEEME_caja.md):
    giro de 360° del aparato montado, explosionado, giro explosionado y vuelta a montar.
"""
import math
import re
import os
import traceback

import FreeCAD as App
import FreeCADGui as Gui

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "renders")
FRAMES = os.path.join(OUT, "frames")
RAW = os.path.join(OUT, "raw")            # vistas fijas sin recortar (montar_renders.js las recorta)
os.makedirs(FRAMES, exist_ok=True)
os.makedirs(RAW, exist_ok=True)
LOG = os.path.join(OUT, "LISTO.txt")

W, H = 1600, 1200            # resolución de los renders fijos
FW, FH = 1600, 1200          # resolución de los fotogramas (ffmpeg los recorta y reduce)
BG = "White"

# Qué sube y qué baja en el explosionado (como en generar_ensamble.py)
ARRIBA = ("Carcasa", "PisadorOLED", "OLED", "Encoder", "Mando", "Interruptor")
DZ_ARRIBA, DZ_ABAJO = 45.0, -15.0


def pump():
    Gui.updateGui()


def main():
    # Antialiasing (MSAA 8x) antes de crear la vista
    App.ParamGet("User parameter:BaseApp/Preferences/View").SetInt("AntiAliasing", 4)

    doc = App.openDocument(os.path.join(HERE, "ensamble_display_timemore.FCStd"))
    Gui.activateWorkbench("PartWorkbench")
    pump()
    gdoc = Gui.getDocument(doc.Name)
    view = gdoc.activeView()

    # Colores (freecadcmd no los guarda: vienen de ensamble_colores.txt)
    colors = {}
    with open(os.path.join(HERE, "ensamble_colores.txt")) as f:
        for line in f:
            p = line.split()
            if len(p) == 4:
                colors[p[0]] = (float(p[1]), float(p[2]), float(p[3]))

    mont = doc.getObject("Montado").Group
    for o in doc.Objects:
        vo = o.ViewObject
        if vo is None:
            continue
        if o.TypeId == "App::DocumentObjectGroup":
            continue
        vo.Visibility = o in mont
        if o.Name in colors:
            vo.ShapeColor = colors[o.Name]
        if hasattr(vo, "DisplayMode"):
            vo.DisplayMode = "Flat Lines"
            vo.LineColor = (0.35, 0.35, 0.38)
            vo.LineWidth = 1.0
        # La carcasa blanca con aristas grises finas; el resto con aristas oscuras discretas
        if o.Name.startswith("Carcasa"):
            vo.LineColor = (0.55, 0.55, 0.58)
    doc.getObject("Explosionado").ViewObject.Visibility = False
    doc.getObject("Montado").ViewObject.Visibility = True

    view.setCameraType("Perspective")
    pump()

    base = {o.Name: o.Placement.copy() for o in mont}
    bb = None
    for o in mont:
        bb = o.Shape.BoundBox if bb is None else bb.united(o.Shape.BoundBox)
    center = bb.Center

    def pose(angle_deg=0.0, explode=0.0, hide=()):
        rot = App.Placement(App.Vector(0, 0, 0), App.Rotation(App.Vector(0, 0, 1), angle_deg), center)
        for o in mont:
            up = any(o.Name.startswith(a) for a in ARRIBA)
            dz = (DZ_ARRIBA if up else DZ_ABAJO) * explode
            o.Placement = rot.multiply(App.Placement(App.Vector(0, 0, dz), App.Rotation())).multiply(base[o.Name])
            o.ViewObject.Visibility = not any(o.Name.startswith(h) for h in hide)
        pump()

    def camera(az, elev):
        # La cámara por defecto mira hacia -Z con +Y arriba. Inclinar (90 - elev) sobre X la pone delante
        # (en -Y) mirando hacia +Y con +Z arriba; luego se gira az grados alrededor de Z.
        r = App.Rotation(App.Vector(0, 0, 1), az).multiply(App.Rotation(App.Vector(1, 0, 0), 90 - elev))
        view.setCameraOrientation(r.Q)
        pump()

    def fit(margin=1.08):
        # Encuadre con margen: se amplía el ángulo de la cámara en perspectiva. Se edita la cámara como
        # texto de Inventor (getCameraNode() necesita pivy, que no siempre está disponible).
        view.fitAll()
        pump()
        cam = view.getCamera()
        m = re.search(r"heightAngle\s+([0-9.eE+-]+)", cam)
        if m:
            ang = float(m.group(1)) * margin
            view.setCamera(cam[:m.start(1)] + repr(ang) + cam[m.end(1):])
        pump()

    def shot(name, w=W, h=H):
        pump()
        view.saveImage(name, w, h, BG)

    # ---------------- Renders fijos ----------------
    pose(0, 0)
    camera(-35, 24); fit()
    shot(os.path.join(RAW, "montado.png"))

    pose(0, 1)
    camera(-35, 20); fit()
    shot(os.path.join(RAW, "explosionado.png"))

    pose(0, 0, hide=("Carcasa",))
    camera(-35, 38); fit()
    shot(os.path.join(RAW, "interior.png"))

    pose(0, 0)
    camera(145, 22); fit()
    shot(os.path.join(RAW, "trasera.png"))

    # ---------------- Animación ----------------
    # Encuadre fijo para todos los fotogramas: el del explosionado (lo más grande), con margen para el giro
    camera(-35, 22)
    pose(0, 1); fit(1.0)
    pose(90, 1); view.fitAll(); pump()
    pose(0, 1); fit(1.45)   # margen amplio: al girar, la tapa asoma; se recorta después

    seq = []
    for i in range(72):                       # giro de 360° montado (5° por fotograma)
        seq.append((i * 5.0, 0.0))
    for i in range(1, 25):                    # explosionado (suavizado)
        t = i / 24.0
        seq.append((0.0, 0.5 - 0.5 * math.cos(math.pi * t)))
    for i in range(12):                       # pausa explosionado
        seq.append((0.0, 1.0))
    for i in range(1, 73):                    # giro de 360° explosionado
        seq.append((i * 5.0, 1.0))
    for i in range(1, 25):                    # vuelta a montar
        t = i / 24.0
        seq.append((0.0, 0.5 + 0.5 * math.cos(math.pi * t)))
    for i in range(12):                       # pausa montado
        seq.append((0.0, 0.0))

    for n, (ang, ex) in enumerate(seq):
        pose(ang, ex)
        shot(os.path.join(FRAMES, "f_%03d.png" % n), FW, FH)

    with open(LOG, "w") as f:
        f.write("OK %d fotogramas\n" % len(seq))


try:
    main()
except Exception:
    with open(LOG, "w") as f:
        f.write("ERROR\n" + traceback.format_exc())
finally:
    os._exit(0)
