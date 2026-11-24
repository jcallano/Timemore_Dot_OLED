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

La caja imprimible y sus scripts de FreeCAD están en [`cad/`](../cad/LEEME_caja.md).

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
- El firmware ocupa unos 694 KB (20 % de la app). No usa Wi-Fi.
- Para publicar una versión que se pueda cargar por Bluetooth: *Sketch → Export Compiled Binary* y sube **`firmware.ino.bin`** (no el `merged` ni el `bootloader`) como archivo de una *release* de GitHub.

---

## 3. Cómo consigue la baja latencia

1. **Intervalo BLE de 15 ms.** La Dot no pide cambiar el intervalo, pero el enlace quedaba en 60 ms. Si el intervalo supera 20 ms, el display vuelve a pedir 10-15 ms, y la Dot lo acepta a la primera (medido: **60 ms → 15 ms**).
2. **Tara por software** en el ESP32. No se envía la tara a la Dot, para evitar su *zero-lock*, que retrasa las lecturas tras tarar. Si se levanta la jarra para remover o servir, la tara se conserva. Si se pulsa el botón de tara de la propia Dot, se detecta (el peso sube poco a poco desde 0) y la tara del display se anula.
3. **MTU grande: la cola de la Dot no crece.** La Dot envía como máximo una notificación cada 100 ms. Con el MTU por defecto (23 bytes) solo cabe una trama de peso por notificación, y cada respuesta a un comando o aviso de batería retrasaba el peso 100 ms para siempre (hasta ~2,4 s tras unos minutos). Ahora se negocia MTU 247 un segundo después de suscribirse: la Dot junta varias tramas por notificación y vacía su cola. De cada notificación se usa la trama de peso (`A5 5A 01 01`) más reciente. Detalles en [`COLA_DOT_MTU.md`](./COLA_DOT_MTU.md).
4. **Motor de caudal** ([`src/flow_engine.*`](../firmware/src/flow_engine.h)). Combina una ventana de unos 350 ms, una mediana de 3, una EMA adaptativa (α 0.75/0.55/0.35/0.20), histéresis visual de 0.18 g/s y corte por "marca de agua": sin ganancia neta en 300 ms, el caudal pasa a 0.
5. **Sin carreras entre núcleos.** La tarea BLE (núcleo 0) solo decodifica y encola cada muestra con su hora; `loop()` (núcleo 1) es el único que toca el peso, la tara y el caudal.
6. **Conexión no bloqueante.** El `connect()` es asíncrono y el descubrimiento se completa en `loop()`. El encoder y la pantalla no se congelan mientras busca la báscula.
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
| `x` | **Autotest** de los módulos (protocolo, motor de caudal y puente BooKoo), con resultado OK/FALLO por caso |
| `q` | **Sonda de la cola de la Dot**: envía `02 08` y mide cuánto tarda la respuesta. Con la cola vacía, unos 100 ms |
| `s` | **Captura de pantalla**: vuelca el framebuffer de la OLED en hexadecimal (`[FB] BEGIN` … `[FB] END`) |
| `d` | **Pour-over de demostración**: 15 g / 250 g, bloom de 30 s y 3 vertidos, generado a 10 Hz y pasado por el motor de caudal real. Solo en RAM: no se escribe en la NVS ni cambia el número del siguiente pour-over |
| `0`-`3` | Vista de pesaje 0 a 3 |
| `g` / `i` | Historial en el último pour-over: gráfica / ficha |
| `m` | Abrir el menú |

**Capturas en PNG:** guarda la salida del monitor serie en un archivo (tras una o varias órdenes `s`) y ejecuta

```text
node tools/oled_capture.js captura.log docs/img/pantallas nombre1 nombre2 ...
```

Cada bloque `[FB]` se convierte en un PNG con aspecto de OLED. El script no tiene dependencias, solo usa Node.js. Las capturas del manual se hicieron así, con la secuencia `d s g s i s m s 0 s 1 s 2 s 3 s`.

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

## 7. Estructura del código

Todo el firmware está en [`firmware/`](../firmware/); la carpeta y la pestaña principal se llaman igual porque el IDE de Arduino lo exige.

| Archivo (en `firmware/`) | Contenido |
| :--- | :--- |
| `firmware.ino` | Configuración, pines, tipos y estado global compartido (`UiState`, `PourOverRecord`, `WeightSample`, motor de caudal...) |
| `10_encoder.ino` | Encoder por PCNT |
| `20_oled.ino` | Driver de la OLED: dibujo, fuente 5×7 y volcado por páginas |
| `25_led.ino` | LED RGB de estado |
| `30_storage.ino` | NVS (báscula vinculada, dosis, anillo de pour-overs) y PSRAM |
| `40_brew.ino` | Auto-timer, auto-stop y muestreo de la extracción |
| `50_ble_scale.ino` | Bluetooth: tramas, tara, cola de muestras, vinculación, escaneo y conexión no bloqueante |
| `55_bookoo_bridge.ino` | Puente BooKoo: servidor GATT, anuncio, envío a la cafetera y sus comandos |
| `60_power.ino` | Batería, reposo (light sleep) y corte (deep sleep) |
| `70_ota.ino` | Actualización por Bluetooth (servicio OTA y protocolo) |
| `80_pantalla.ino` | Vistas, menú, visor e icono de batería |
| `85_controles.ino` | Encoder, multiclic, BOOT y órdenes por serie |
| `90_app.ino` | `setup()` y `loop()` |
| `src/timemore_protocol.*` | **Módulo puro**: decodificación de notificaciones y construcción de tramas Open Scale (CRC16) |
| `src/flow_engine.*` | **Módulo puro**: motor de caudal |
| `src/bookoo_protocol.*` | **Módulo puro**: tramas BooKoo (peso, comandos de la cafetera, anuncio) |
| `src/selftest.*` | Autotest de los módulos puros (orden `x`) |
| `partitions.csv` | Tabla de particiones de 8 MB |
| `../tools/oled_capture.js` | (fuera del sketch) Convierte los volcados de pantalla de la orden `s` en PNG |

El IDE de Arduino concatena las pestañas `.ino` detrás de la principal en orden alfabético, de ahí los prefijos numéricos. Los módulos de `src/` no dependen de Arduino ni de variables globales, así que se pueden probar con tramas o vertidos simulados (el autotest los ejecuta en la propia placa).

---

## 8. Otros documentos

| Documento | Contenido |
| :--- | :--- |
| [`COLA_DOT_MTU.md`](./COLA_DOT_MTU.md) | Por qué el retraso del peso crecía con el tiempo y cómo se corrigió en este firmware |
| [`DOT_COLA_BLE.md`](./DOT_COLA_BLE.md) | La investigación completa de la cola de salida de la Dot (hecha en el proyecto hermano del panel de 7") |
| [`PLAN_PUENTE_BOOKOO.md`](./PLAN_PUENTE_BOOKOO.md) | Análisis previo del puente BooKoo |
| [`AUDITORIA_FIRMWARE.md`](./AUDITORIA_FIRMWARE.md) | Auditoría del firmware y las fases de cambios aplicadas |
| [`AUDITORIA_LATENCIAS.md`](./AUDITORIA_LATENCIAS.md) | Auditoría de latencias |
