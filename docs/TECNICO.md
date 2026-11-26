# Documentación técnica

Para quien quiera montar, compilar o modificar el dispositivo. El uso diario está en el [manual](../MANUAL.md).

---

## 1. Hardware

| Componente | Modelo | Función |
| :--- | :--- | :--- |
| Microcontrolador | **WeAct Studio ESP32-S3-A** (`ESP32-S3R8`, 8 MB de flash y 8 MB de PSRAM) | BLE con la báscula y la cafetera, interfaz y almacenamiento |
| Pantalla | **OLED 2.42" I2C 128×64** (`2.42OLED-IIC v1.1`, SSD1309) | Dirección `0x3C` (serigrafía `0x78`) |
| Control | **Módulo encoder EC11** con pulsador (PCB redondo de Ø23.6 mm, 5 pines) | Giro y 1/2/3 clics |
| Carga | **TP4056 USB-C con protección** (DW01A) | Carga de la celda y protección |
| Batería | **Celda 18650** en portapilas | Alimentación |
| Interruptor | **MTS-103** (palanca, rosca M6) | ON/OFF en la línea OUT+ → 5V |
| Báscula | **Timemore Black Mirror Dot** (`Phyplus PHY6222`) | Peso por BLE |

### 1.1 Conexiones

| Señal | GPIO WeAct | Notas |
| :--- | :--- | :--- |
| OLED SDA / SCL | **8 / 9** | I2C configurado a 800 kHz; OLED alimentada desde 3V3 |
| Encoder CLK (A) / DT (B) | **10 / 11** | Contador de pulsos PCNT por hardware |
| Encoder SW (pulsador) | **12** | Activo en LOW; también despierta del reposo |
| Botón BOOT de la placa | **0** | Mismos clics que el encoder; mantenido ≥ 4 s, re-vincular |
| LED RGB WS2812B de la placa | **48** | Estado (ver el manual) |
| Divisor de batería (100 kΩ + 100 kΩ) | **4** | **Opcional.** B+ (u OUT+) → 100k → GPIO 4 → 100k → GND. Activa el icono de batería y el corte por batería baja |
| CHRG del TP4056 | **5** | **Opcional.** LOW mientras carga (animación del icono) |
| Alimentación | 5V / GND | OUT+ del TP4056 → interruptor → pin 5V de la WeAct |

> El firmware detecta solo si el divisor está montado. Para ello compara la lectura del GPIO 4 con los pull-up y pull-down internos; la línea `[STATUS] Divisor bat:` del monitor serie muestra el resultado. Sin divisor, no hay icono ni corte por batería.
>
> El LED WS2812B de la placa consume del orden de 0.5-1 mA incluso apagado.

La caja imprimible y sus scripts de FreeCAD están en [`cad/`](../cad/README.md).

---

## 2. Compilación

### 2.1 Versiones usadas

Probado con estas versiones (noviembre de 2026):

| Herramienta / librería | Versión | Notas |
| :--- | :--- | :--- |
| Arduino IDE | **2.3.10** | Incluye arduino-cli 1.5.1 |
| Core **esp32** de Espressif (`esp32:esp32`) | **3.3.12** | Basado en **ESP-IDF 5.5.5**. URL del gestor de placas: `https://espressif.github.io/arduino-esp32/package_esp32_index.json` |
| esptool | 5.3.1 | Viene con el core |
| **NimBLE-Arduino** (h2zero) | **2.5.2** | **Única librería externa.** Instálala desde el gestor de librerías. Hace falta la serie 2.x (la 1.x tiene otra API) |
| Wire, Preferences, Update | 3.3.12 | Vienen con el core; no hay que instalarlas |

No hace falta ninguna librería de pantalla ni de LED: el driver de la OLED (fuente 5×7 incluida) está en `20_oled.ino` y el LED usa la función del core.

### 2.2 Ajustes de la placa

- Abre **`firmware/firmware.ino`** en el IDE; el resto de pestañas se cargan solas.
- Placa: *ESP32S3 Dev Module*, USB CDC On Boot **Enabled**, USB Mode **Hardware CDC and JTAG**, Flash Size **8MB**, PSRAM **OPI PSRAM**, Partition Scheme **"8M with spiffs (3MB APP/1.5MB SPIFFS)"**.
- FQBN equivalente para arduino-cli:
  ```text
  esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,FlashSize=8M,PSRAM=opi,PartitionScheme=default_8MB
  ```
- [`firmware/partitions.csv`](../firmware/partitions.csv) (incluido en el sketch) fija la tabla de 8 MB con dos apps de 3.2 MB. La NVS se mantiene en `0x9000`, así que se conservan la báscula vinculada, la dosis y el historial. **Al cambiar de tabla, la primera carga debe hacerse por USB.**
- El firmware ocupa unos 694 KB (20 % de la app). Usa el Wi-Fi solo al arrancar (la hora, 75_wifi_time.ino) y al enviar cafés a Beanconqueror (8.4), siempre con el Bluetooth apagado.
- Para publicar una versión que se pueda cargar por Bluetooth: *Sketch → Export Compiled Binary* y sube **`firmware.ino.bin`** (no el `merged` ni el `bootloader`) como archivo de una *release* de GitHub.

---

## 3. Cómo consigue la baja latencia

1. **Intervalo BLE de 15 ms.** La Dot no pide cambiar el intervalo, pero el enlace quedaba en 60 ms. Si el intervalo supera 20 ms, el display vuelve a pedir 10-15 ms, y la Dot lo acepta a la primera (medido: **60 ms → 15 ms**).
2. **Tara por software** en el ESP32. No se envía la tara a la Dot, para evitar su *zero-lock*, que retrasa las lecturas tras tarar. Si se levanta la jarra para remover o servir, la tara se conserva. Si se pulsa el botón de tara de la propia Dot, se detecta (el peso sube poco a poco desde 0) y la tara del display se anula.
3. **MTU grande: la cola de la Dot no crece.** La Dot envía como máximo una notificación cada 100 ms. Con el MTU por defecto (23 bytes) solo cabe una trama de peso por notificación, y cada respuesta a un comando o aviso de batería retrasaba el peso 100 ms para siempre (hasta ~2,4 s tras unos minutos). Ahora se negocia MTU 247 un segundo después de suscribirse: la Dot junta varias tramas por notificación y vacía su cola. De cada notificación se usa la trama de peso (`A5 5A 01 01`) más reciente. Detalles en [`COLA_DOT_MTU.md`](./COLA_DOT_MTU.md).
4. **Motor de caudal** ([`src/flow_engine.*`](../firmware/src/flow_engine.h)). Combina una ventana de unos 350 ms, una mediana de 3, una EMA adaptativa (α 0.75/0.55/0.35/0.20), histéresis visual de 0.18 g/s y corte por "marca de agua": sin ganancia neta en 300 ms, el caudal pasa a 0.
5. **Sin carreras entre núcleos.** La tarea BLE (núcleo 0) solo decodifica y encola cada muestra con su hora; `loop()` (núcleo 1) es el único que toca el peso, la tara y el caudal.
6. **Conexión no bloqueante.** El `connect()` es asíncrono y el descubrimiento se completa en `loop()`. El encoder y la pantalla no se congelan mientras busca la báscula. Una báscula nueva que conecta pero no manda peso (ni ninguna trama Open Scale) en 10 s, o en la que no se puede suscribir, se desconecta y se aparta **60 s** (`rejectMac`, hasta 4 a la vez) para no reconectar en bucle. Antes se apartaba hasta el siguiente reinicio, y una Dot de verdad que tardaba en mandar peso (recién encendida, o con la app del móvil conectada) se quedaba bloqueada incluso tras *Re-vincular*. *Re-vincular* vacía la lista (`clearRejectedMacs`).
7. **OLED por páginas.** Solo se envían las franjas de 8 filas que cambian. Un volcado completo tarda unos **14-16 ms** (medido); con la pantalla quieta no se transmite nada.
8. **Encoder por hardware (PCNT)** con filtro de glitches y acumulador con histéresis: una muesca cuenta a los 3 de sus 4 pulsos, sin rebotes en la posición de reposo.
9. **`Serial` no bloqueante.** Sin monitor abierto, las trazas no frenan la CPU.
10. **PSRAM y NVS.** Cada extracción se graba en PSRAM a 2 Hz; al terminar se guarda un resumen con la curva en la NVS, en un anillo de 10 registros donde solo se escribe el nuevo.

---

## 4. Puente BooKoo por dentro

```text
Dot --(0xFFF1, 10 Hz)--> ESP32-S3 --(0x0FFE/0xFF11, trama 03 0B de 20 bytes)--> Wendougee
                         ESP32-S3 <--(0xFF12, comandos 03 0A cmd)-------------- Wendougee
```

- El ESP32 es a la vez **central** de la Dot y **periférico** de la cafetera. Se anuncia como `BOOKOO_SC_U XXXXXX` (los 6 caracteres salen de la MAC Bluetooth de la placa), con el servicio `0x0FFE` y un Device Information que dice "BOOKOO / Themis".
- Cada peso de la Dot se reenvía en cuanto llega, desde una tarea propia en el núcleo 0 (no espera al refresco de la OLED, que tarda hasta 16 ms). Si la Dot calla, se reenvía el último peso cada 250 ms. Separación mínima entre tramas: 15 ms.
- El caudal que se envía lo calcula el mismo motor que la OLED, alimentado en esa tarea con cada muestra según llega.
- Comandos de la cafetera: tara `01`, inicio `04`, parada `05`, reinicio `06`, tara + inicio `07`, suavizado `08` (ignorado). La tara es por software, igual que la del display.
- Inicio y parada arrancan y cortan un **shot** de `45_espresso.ino` en cualquier modo: se guarda en el historial de shots con la preinfusión medida desde el inicio y el goteo de los 3 s siguientes a la parada.
- Enlace con la cafetera: 15-30 ms. Anuncio sin cafetera conectada: 60-100 ms.
- El rebase se mide como el peso 3 s después del `STOP` menos el peso en el instante del `STOP`.
- Protocolo en [`src/bookoo_protocol.*`](../firmware/src/bookoo_protocol.h); análisis previo en [`PLAN_PUENTE_BOOKOO.md`](./PLAN_PUENTE_BOOKOO.md).

---

## 5. Monitor serie (115200)

| Tecla | Acción |
| :--- | :--- |
| `t` | Tara |
| `c` | Cronómetro a 0 |
| `r` | Olvidar la báscula vinculada |
| `b` | Activar / desactivar el puente BooKoo |
| `x` | **Autotest** de los módulos puros (protocolo, motor de caudal, vertidos y toques, puente BooKoo, R2, consejo y red del gusto), con resultado OK/FALLO por caso |
| `q` | **Sonda de la cola de la Dot**: envía `02 08` y mide cuánto tarda la respuesta. Con la cola vacía, unos 100 ms |
| `s` | **Captura de pantalla**: vuelca el framebuffer de la OLED en hexadecimal (`[FB] BEGIN` … `[FB] END`) |
| `d` | **Pour-over de demostración**: 15 g / 250 g, bloom de 30 s y 3 vertidos, generado a 10 Hz y pasado por el motor de caudal real. Solo en RAM: no se escribe en la NVS ni cambia el número del siguiente pour-over |
| `0`-`3` | Vista de pesaje 0 a 3 |
| `g` / `i` | Historial en el último pour-over: gráfica / ficha |
| `n` | Historial en la página "Enviar a Beanconqueror" del último pour-over |
| `h` | Cambiar el idioma de la pantalla (como *Menú → Idioma*) |
| `w` / `W` | **Enviar a Beanconqueror** el último pour-over (8.4): con la red guardada / forzando la red propia. Durante la sesión, `s` captura la pantalla y `x` la cierra |
| `m` | Abrir el menú |
| `e` | **Espresso en vivo** con la receta actual: tara, clic, 7 s de preinfusión, flujo de hasta 2 g/s, clic al llegar al punto de corte y goteo. El shot se guarda solo en RAM; al acabar se restauran el modo, el número del siguiente shot y el goteo aprendido |
| `k` | Cambiar de modo (pour-over ↔ espresso), como *Menú → Modo* |
| `v` | **V60 guiado en vivo** (unos 3 min): pone un vaso de 50 g y gira, echa 15,3 g de granos y quita el vaso, pone un server de 300 g y gira, pone el V60 (125 g) y gira, echa 15,1 g de café molido y gira a Bloom, vierte la receta de `d` en tiempo real con un toque de la tetera a los 20 s, gira al acabar el bloom, al acabar de verter y al acabar el drawdown, quita el V60 (yield 215 g) y gira al resumen. El peso simulado entra en la cola de muestras a 10 Hz en lugar del de la Dot y recorre todo el firmware. El pour-over se guarda solo en RAM y su traza, marcada como demo; al acabar se restauran la receta y el número del siguiente pour-over |
| `<` / `>` / `.` | Giro del encoder hacia atrás / adelante y clic simulados |
| `l` | **Lista de trazas** guardadas (número, pour-over, muestras, eventos, receta y fecha) |
| `f<n>` | **Vuelca la traza n en CSV** (`[CSV] BEGIN` … `[CSV] END`); `f` sola, la última. Ver 8.3 |

**Capturas en PNG:** guarda la salida del monitor serie en un archivo (tras una o varias órdenes `s`) y ejecuta

```text
node tools/oled_capture.js captura.log docs/img/pantallas nombre1 nombre2 ...
```

Cada bloque `[FB]` se convierte en un PNG con aspecto de OLED. El script no tiene dependencias, solo usa Node.js. Las capturas del manual se hicieron así, con la secuencia `d s g s i s m s 0 s 1 s 2 s 3 s`.

**GIF animado:** `node tools/oled_gif.js captura.log salida.gif [ms por fotograma]` convierte todos los bloques `[FB]` del log en un GIF. Una línea `[GIF] DELAY <ms>` antes de un bloque alarga ese fotograma. El GIF del README se hizo con `v` y una orden `s` por segundo durante 2:52, más `g s` e `i s` con `[GIF] DELAY` de 4 s. A 100 ms por fotograma queda acelerado 10 veces. Solo guarda la zona que cambia en cada fotograma: 171 fotogramas en 170 KB.

Con un ordenador conectado por USB, el aparato no entra en reposo, porque el reposo apaga el USB y no se podría cargar firmware ni usar el monitor serie. Lo detecta con `HWCDC::isPlugged()`; con un cargador sin datos sí duerme.

Cada 5 s se imprime `[STATUS]` con la conexión, el peso, el caudal, las baterías, el intervalo BLE, los paquetes recibidos, el tiempo de volcado de la OLED y el diagnóstico del divisor. Con la Dot conectada se añade `[LINK]`: el MTU negociado, las notificaciones con varias tramas (`tramas juntadas N/M`) y el resultado de la última sonda. Con el puente activo, `[BOOKOO]` indica el nombre anunciado, si la cafetera está conectada y suscrita, y las tramas y comandos intercambiados.

La vinculación de la Dot solo acepta un dispositivo con servicio Timemore (`0xFFF0`/`0xFFE0`) o nombre Timemore; su MAC se guarda en la NVS al recibir la primera trama válida.

---

## 6. Actualización por Bluetooth por dentro

- En modo actualización, el aparato apaga la Dot y el puente, y se anuncia como **`TMDot-OTA`** con el servicio `c0de0001-7d2e-4b5a-9c1e-0a7e5d0f0a01`. Una característica de control (`…0002`, escritura + notificación) y otra de datos (`…0003`, escritura sin respuesta).
- El cliente es la página [`docs/ota/index.html`](./ota/index.html), publicada con GitHub Pages. Usa Web Bluetooth: Chrome/Edge de escritorio, Chrome de Android o Bluefy en iPhone.
- **Protocolo:** `START` con el tamaño y el **PIN de 6 cifras** que muestra la pantalla (aleatorio en cada sesión; 5 fallos y sale del modo). Después, paquetes `[offset u32][datos]` del tamaño que permite el MTU. El cliente no deja más de 8 KB sin confirmar y el aparato confirma cada bloque de 4 KB ya escrito en la flash. Al final, `END`: `Update.end()` comprueba la imagen (incluido su SHA-256) antes de activarla. La especificación completa está en la cabecera de [`firmware/70_ota.ino`](../firmware/70_ota.ino).
- El callback de NimBLE solo copia los datos a un `StreamBuffer` de 16 KB; la escritura en flash se hace en el bucle del modo OTA, fuera de la tarea Bluetooth.
- El primer bloque debe ser una imagen de aplicación ESP32 (magic `0xE9` y `esp_app_desc_t` en el offset `0x20`). Así se rechazan el `.bin` *merged*, el del bootloader o cualquier otro archivo.
- Si la subida se corta, se cancela y el aparato sigue con el firmware anterior (la app nueva va a la otra partición OTA y solo se activa si se verifica completa).

---

## 7. Modo espresso por dentro

- **Ciclo** (`shotPhase`): `Idle` → `Running` (1 clic, la cafetera o la primera gota) → `Settling` (3 s tras un corte manual o de la cafetera, para medir el goteo; 0,3 s si terminó solo) → guarda y vuelve a `Idle`.
- **Primera gota**: 0,3 g netos en la taza. La preinfusión es el tiempo desde el inicio hasta ese momento; solo es válida si el shot arrancó con clic o con la cafetera.
- **Arranque solo**: peso ≥ 0,5 g, caudal ≥ 0,3 g/s y una subida de peso posterior a la tara. Solo se arma con una tara (2 clics), para que poner la taza no lo dispare.
- **Fin solo**: tras la primera gota, 4 s sin que el peso suba (y al menos 3 g). El cronómetro se para en el instante de la última subida. Tope de seguridad: 120 s.
- **Punto de corte** = dosis × ratio − goteo aprendido. El goteo (`espRebaseAvgGrams`, 2 g de partida) se actualiza con una media móvil (0,7 / 0,3) en cada shot cortado a mano o por la cafetera, y se guarda en la NVS. El LED pasa a amarillo 3 g antes y a rojo en el punto de corte (no en los shots de la cafetera, que corta sola).
- **Muestreo** a 5 Hz en el mismo búfer de PSRAM que el pour-over; el registro guarda 60 puntos de peso y caudal.
- **`ShotRecord`** (148 bytes, `static_assert`): id, duración y preinfusión en décimas, tiempo objetivo, banderas (corte de la cafetera, fin solo, preinfusión válida), dosis, bebida, objetivo, peso al cortar y las dos curvas. Anillo de 10 en el espacio NVS `tmdot_shots` (`s_0`…`s_9`, `next_id`), como los pour-overs.
- **Ajustes** en `tmdot_pours`: `mode`, `esp_dose`, `esp_ratio`, `esp_tsec`, `esp_reb`.
- **Menú**: las opciones se construyen según el modo (`buildMenuItems()` con identificadores `MI_*`), así que los números cambian entre los dos modos.

---

## 8. Pour-over guiado por dentro

### 8.1 Pasos y tara en dos capas

[`41_guia.ino`](../firmware/41_guia.ino): `Listo → Granos → Server → V60 → Café molido → Bloom → Vertidos → Drawdown → Yield → Resumen` (`GuideStep`; `Beans` y `Ground` van al final del enum para no cambiar el número de los pasos en las trazas ya guardadas). **Solo el giro del encoder cambia de paso**: el peso mide y avisa, pero nunca avanza solo, porque en la encimera el montaje se toca sin querer. Una muesca es un paso; tras cada cambio se ignoran los giros durante 350 ms para que un giro rápido no salte varios. Girar hacia atrás deshace lo que hizo el paso.

| Paso al que se entra | Qué hace |
| :--- | :--- |
| Granos | Guarda la tara anterior (para deshacer y para volver a Listo) y empieza la traza. En cuanto el bruto está quieto (±0,3 g durante 0,7 s; como mucho 3 s) guarda `cupRawGrams` (el vaso) y pone la tara ahí. Los granos son el último neto quieto entre 1 y 100 g; cuando el neto baja de −0,5 g (se quitó el vaso) quedan fijados, para que el server puesto después no cuente. 2 clics vuelven a tarar y a empezar |
| Server | Fija los granos (`guideBeansG`, evento `granos`; 0 = no se pesaron) y, con el peso quieto, guarda `serverRawGrams` y pone la tara ahí |
| V60 | Igual con el V60 vacío: `dripperEmptyRawGrams` y tara. El server sigue guardado |
| Café molido | Muestra el neto (el café que se echa). Al girar, si está entre 1 y 100 g es la dosis (`guideGroundG`, evento `molido`) y tara ahí (`dripperRawGrams`); si no (el café ya estaba al tarar el V60), no toca la tara |
| Bloom | Arma la preparación: muestras, segmentador y marcas a cero. El cronómetro arranca con el primer chorro que deja ≥ 2 g (con la hora del inicio del chorro), o con ≥ 5 g y ≥ 0,3 g/s si el chorro es muy lento |
| Vertidos | Marca el fin del bloom: tiempo, agua y vertidos hasta ahí |
| Drawdown | Marca el fin del vertido: tiempo y agua total |
| Yield | Para el cronómetro y guarda el pour-over (`storePourRecord` con el agua marcada y el bloom que marcó el usuario). La bebida se mide al quitar el V60: neto < agua − 20 g, bebida = bruto − server entre el 40 % del agua y el agua + 2 g, quieta 1,5 s. Afina la retención aprendida, como antes la jarra |
| Resumen | Guarda la traza en la flash. Girar otra vez vuelve a Listo (con la tara de antes del server) y, si hay un R2 vinculado, abre la medida de TDS de este pour-over |

**Dosis:** `brewDoseGrams()` es el café molido pesado, si no los granos, si no la de la receta; de ella salen el agua objetivo, el bloom, la dosis y el ratio del registro y la de la traza. La receta guardada no cambia. **Retención del molino** = granos − café molido; `grinderRetentionAvg()` es la media de los guardados que pesaron las dos cosas (descarta |r| > 3 g) y el paso Granos la suma a la dosis de la barra.

**2 clics** hacen la tara del paso actual sin perder el server; en Bloom, Vertidos o Drawdown reinician los vertidos (vuelve a Bloom armado) y en Yield o Resumen terminan (vuelta a Listo).

Los tiempos que no caben en `PourOverRecord` (272 B sin hueco) van en una tabla aparte de 10 `GuideTiming` de 16 B, unida por el id, en la clave `gt2` de `tmdot_pours`: fin del bloom y fin del vertido en décimas de segundo, agua del bloom, toques descartados, peso del server, granos y café molido (décimas de g, 0 = no se pesaron). La tabla anterior (`gt1`, 12 B) se lee al arrancar y se borra al guardar la nueva. La receta se edita en el paso Listo (1 clic: dosis → ratio → bloom → guardar; el giro ajusta) y no en el menú, y se guarda al salir de la edición: `dose_g`, `pr_ratio` (agua = dosis × ratio) y `bloom_x` (agua del bloom de referencia = dosis × factor). La demostración `v` no la guarda.

### 8.2 Vertidos y toques de la tetera

[`src/pour_segmenter.*`](../firmware/src/pour_segmenter.h), módulo puro con autotest. Recibe cada lectura (peso neto y caudal del motor de caudal, ~10 Hz). Un candidato empieza con ≥ 0,8 g/s y termina con ≤ 0,25 g/s; se juzga 0,8 s después, con el peso asentado:

- **Vertido** si deja ≥ 4 g de agua y duró ≥ 1 s. Si empieza menos de 3 s después del anterior, es la misma tanda (la mano se recoloca).
- **Toque** si no: un golpe del pico contra el filtro sube el peso y lo devuelve a donde estaba. No cuenta como vertido; se cuenta aparte (`TAP` en el perfil) y su pico de caudal no entra en el máximo del registro.

Los umbrales son una primera aproximación; las trazas (8.3) sirven para ajustarlos con datos reales.

### 8.3 Trazas

[`47_trazas.ino`](../firmware/47_trazas.ino). Desde el paso Granos hasta volver a Listo se graban en PSRAM **todas** las lecturas de la Dot (`TraceSample`, 12 B: ms, bruto, neto, caudal, estable; tope 6000 = 10 min) y los eventos (`TraceEvent`, 8 B: paso, tara, granos, café molido, cronómetro, vertido, toque, yield, vista; tope 160). Al pasar a Resumen se escriben en `/trz/NNNNN.bin` (LittleFS en la partición `spiffs` de 1,5 MB; la primera vez se formatea), con una cabecera (número, pour-over, fecha, receta, server, marca de demo). Se conservan las 8 últimas; el número sigue en la NVS (`trz_seq`). Escribir una traza de 3 min tarda unos 150 ms.

**Descarga** con el display conectado por USB y el monitor serie cerrado:

```text
powershell -ExecutionPolicy Bypass -File tools\descargar_trazas.ps1 -Port COM14
```

Crea `trazas/traza_NNNN.csv` con `t_ms,bruto_g,neto_g,caudal_gps,estable` y, al final, los eventos (`#E,t_ms,evento,arg,valor`).

### 8.4 Envío a Beanconqueror

[`77_compartir.ino`](../firmware/77_compartir.ino). Beanconqueror (8.8.2 o posterior) importa preparaciones terminadas con un enlace `beanconqueror://ADD_BREW` (una) o `ADD_BREWS` (hasta 50): un sobre JSON ("handoff" v1) comprimido con gzip, en base64url sin relleno y repartido en parámetros `shareBrew0`, `shareBrew1`… de 400 caracteres, con `len` = longitud total. La especificación está en [`docs/brew-import-api.md`](https://github.com/graphefruit/Beanconqueror/blob/main/docs/brew-import-api.md) de Beanconqueror.

**Sesión** (desde la última página del historial, o `w` por serie): bucle propio, como el modo OTA. Apaga la pila BLE (`shutdownBleStack`) para que el WiFi no comparta la radio, se conecta a la red guardada (10 s como mucho) o crea una red propia `TMDOT-XXXX` con clave aleatoria de 8 cifras, y arranca un `WebServer` en el puerto 80. La OLED muestra un QR (`esp_qrcode`, incluido en el core) con `HTTP://<ip>/P/<id>`: en mayúsculas y sin `?` cabe en modo alfanumérico, versión 2, a 2 px por módulo. Con la red propia, primero sale el QR `WIFI:T:WPA;S:…;P:…;;` y, al unirse una estación, el de la dirección. Se sale con un clic, `x` por serie o 3 min sin peticiones; entonces vuelven `initBleStack` y el escaneo de la Dot.

**API:**

| Ruta | Respuesta |
| :--- | :--- |
| `/`, `/P/<id>` | La página (`src/share_page.h`) |
| `/api/brews` | Resumen de los pour-overs guardados (id, fecha, dosis, agua, bebida, tiempo, TDS, enviado) |
| `/api/handoff?ids=a,b` | `{"brews":[sobre, …]}`, cada uno ya en el formato de importación |
| `/api/sent?ids=a,b` | Los marca como enviados (bit 0 de `BrewExtra.flags`) |

**Cada café:** dosis, agua (`waterIn`), bebida medida o estimada (`beverageOut`), tiempo, ratio, molienda (`grindSize`), bloom (`bloomTime`, de `GuideTiming`) y fecha en UTC. Sin hora en el aparato, `date` va a `null` y la página pone la del móvil. La curva (`flow.weight`, decigramos de diferencia, a 2 Hz) sale de la traza a 10 Hz si sigue en la flash (8.3), entre el inicio y el fin del cronómetro; si no, de la gráfica de 120 puntos del registro. `waterDispensed` va a cero. `doseIn` es el café molido pesado (o los granos, o la receta). Lo que no tiene campo (TDS, EY, vertidos, toques, caudal, granos y retención del molino, bolsa, sabor) va en `note` y en `imported.params` (`beans`, `ground` y `grinderRetention` si se pesaron).

**La página** pone los nombres del usuario (método, tipo de método, molino y café; los guarda en `localStorage`), comprime con `CompressionStream('gzip')`, arma el enlace, marca los cafés como enviados y lo abre. Probado en Android (Chrome) con la red de casa y con la red propia.

---

## 9. Estructura del código

Todo el firmware está en [`firmware/`](../firmware/); la carpeta y la pestaña principal se llaman igual porque el IDE de Arduino lo exige.

| Archivo (en `firmware/`) | Contenido |
| :--- | :--- |
| `firmware.ino` | Configuración, pines, tipos y estado global compartido (`UiState`, `PourOverRecord`, `WeightSample`, motor de caudal...) |
| `10_encoder.ino` | Encoder por PCNT |
| `20_oled.ino` | Driver de la OLED: dibujo, fuente 5×7 y volcado por páginas |
| `25_led.ino` | LED RGB de estado |
| `30_storage.ino` | NVS (báscula vinculada, dosis, anillo de pour-overs) y PSRAM |
| `40_brew.ino` | Caudal, ciclo del shot y muestreo del pour-over a 2 Hz |
| `41_guia.ino` | Pour-over guiado: pasos con el encoder, tara en dos capas, arranque con el primer chorro, yield |
| `47_trazas.ino` | Trazas a 10 Hz: PSRAM, LittleFS y volcado CSV por serie |
| `46_extraccion.ino` | TDS, bebida, EY, molino, bolsa, sabor y red del gusto |
| `45_espresso.ino` | Modo espresso: receta, ciclo del shot (preinfusión, corte, goteo), historial de shots en la NVS y demo `e` |
| `50_ble_scale.ino` | Bluetooth: tramas, tara, cola de muestras, vinculación, escaneo y conexión no bloqueante |
| `55_bookoo_bridge.ino` | Puente BooKoo: servidor GATT, anuncio, envío a la cafetera y sus comandos |
| `77_compartir.ino` | Enviar pour-overs a Beanconqueror: WiFi, QR en la OLED, servidor web y cada café en el formato de importación |
| `src/share_page.h` | La página que sirve el aparato para el envío (lista, nombres del usuario, gzip y enlace) |
| `60_power.ino` | Batería, reposo (light sleep) y corte (deep sleep) |
| `70_ota.ino` | Actualización por Bluetooth (servicio OTA y protocolo) |
| `80_pantalla.ino` | Vistas, menú, visor e icono de batería |
| `82_pantalla_guia.ino` | Pantallas del pour-over guiado: paso, resumen y perfil en vivo |
| `85_controles.ino` | Encoder, multiclic, BOOT y órdenes por serie |
| `90_app.ino` | `setup()` y `loop()` |
| `src/timemore_protocol.*` | **Módulo puro**: decodificación de notificaciones y construcción de tramas Open Scale (CRC16) |
| `src/flow_engine.*` | **Módulo puro**: motor de caudal |
| `src/pour_segmenter.*` | **Módulo puro**: vertidos y toques de la tetera |
| `src/bookoo_protocol.*` | **Módulo puro**: tramas BooKoo (peso, comandos de la cafetera, anuncio) |
| `src/selftest.*` | Autotest de los módulos puros (orden `x`) |
| `partitions.csv` | Tabla de particiones de 8 MB |
| `../tools/oled_capture.js` | (fuera del sketch) Convierte los volcados de pantalla de la orden `s` en PNG |
| `../tools/oled_gif.js` | (fuera del sketch) Convierte una serie de volcados en un GIF animado |
| `../tools/descargar_trazas.ps1` | (fuera del sketch) Descarga las trazas por USB a archivos CSV |

El IDE de Arduino concatena las pestañas `.ino` detrás de la principal en orden alfabético, de ahí los prefijos numéricos. Los módulos de `src/` no dependen de Arduino ni de variables globales, así que se pueden probar con tramas o vertidos simulados (el autotest los ejecuta en la propia placa).

---

## 10. Otros documentos

| Documento | Contenido |
| :--- | :--- |
| [`COLA_DOT_MTU.md`](./COLA_DOT_MTU.md) | Por qué el retraso del peso crecía con el tiempo y cómo se corrigió en este firmware |
| [`DOT_COLA_BLE.md`](./DOT_COLA_BLE.md) | La investigación completa de la cola de salida de la Dot (hecha en el proyecto hermano del panel de 7") |
| [`PLAN_PUENTE_BOOKOO.md`](./PLAN_PUENTE_BOOKOO.md) | Análisis previo del puente BooKoo |
| [`AUDITORIA_FIRMWARE.md`](./AUDITORIA_FIRMWARE.md) | Auditoría del firmware y las fases de cambios aplicadas |
| [`AUDITORIA_LATENCIAS.md`](./AUDITORIA_LATENCIAS.md) | Auditoría de latencias |
