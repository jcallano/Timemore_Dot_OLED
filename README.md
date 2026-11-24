# Monitor Barista OLED y registro de pour-overs para la Timemore Black Mirror Dot

Display inalámbrico para la báscula **Timemore Black Mirror Dot**. Muestra el peso, el caudal (g/s), el ratio 1:X y el cronómetro, y guarda las curvas de cada pour-over. Funciona sobre una **WeAct Studio ESP32-S3-A** (`ESP32-S3R8`, 8 MB de flash y 8 MB de PSRAM).

- Firmware: pestaña principal [`basicoledtimemore_dot.ino`](./basicoledtimemore_dot.ino), más pestañas y módulos (ver la sección 4).
- Caja imprimible en 3D: [`cad/`](./cad/LEEME_caja.md).
- Auditoría del firmware y cambios aplicados: [`AUDITORIA_FIRMWARE.md`](./AUDITORIA_FIRMWARE.md).

---

## 1. Hardware

| Componente | Modelo | Función |
| :--- | :--- | :--- |
| Microcontrolador | **WeAct Studio ESP32-S3-A** | BLE con la báscula, interfaz y almacenamiento |
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
| LED RGB WS2812B de la placa | **48** | Estado (ver 3.6) |
| Divisor de batería (100 kΩ + 100 kΩ) | **4** | **Opcional.** B+ (u OUT+) → 100k → GPIO 4 → 100k → GND. Activa el icono de batería y el corte por batería baja |
| CHRG del TP4056 | **5** | **Opcional.** LOW mientras carga (animación del icono) |
| Alimentación | 5V / GND | OUT+ del TP4056 → interruptor → pin 5V de la WeAct |

> El firmware detecta solo si el divisor está montado. Para ello compara la lectura del GPIO 4 con los pull-up y pull-down internos; la línea `[STATUS] Divisor bat:` del monitor serie muestra el resultado. Sin divisor, no hay icono ni corte por batería.

### 1.2 Compilación

- Librería necesaria: **NimBLE-Arduino 2.x** (probado con la 2.5.2). Core **esp32 3.3.x**.
- FQBN:
  ```text
  esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,FlashSize=8M,PSRAM=opi,PartitionScheme=default_8MB
  ```
- En el Arduino IDE: *ESP32S3 Dev Module*, USB CDC On Boot **Enabled**, USB Mode **Hardware CDC and JTAG**, Flash Size **8MB**, PSRAM **OPI PSRAM**, Partition Scheme **"8M with spiffs (3MB APP/1.5MB SPIFFS)"**.
- [`partitions.csv`](./partitions.csv) (incluido en el sketch) fija la tabla de 8 MB con dos apps de 3.2 MB. La NVS se mantiene en `0x9000`, así que se conservan la báscula vinculada, la dosis y el historial. **Al cambiar de tabla, la primera carga debe hacerse por USB.**

---

## 2. Cómo consigue la baja latencia

1. **Intervalo BLE de 15 ms.** La Dot no pide cambiar el intervalo, pero el enlace quedaba en 60 ms. Si el intervalo supera 20 ms, el display vuelve a pedir 10-15 ms, y la Dot lo acepta a la primera (medido: **60 ms → 15 ms**).
2. **Tara por software** en el ESP32. No se envía la tara a la Dot, para evitar su *zero-lock*, que retrasa las lecturas tras tarar. Si se levanta la jarra para remover o servir, la tara se conserva. Si se pulsa el botón de tara de la propia Dot, se detecta (el peso sube poco a poco desde 0) y la tara del display se anula.
3. **MTU grande: la cola de la Dot no crece.** La Dot envía como máximo una notificación cada 100 ms. Con el MTU por defecto (23 bytes) solo cabe una trama de peso por notificación, y cada respuesta a un comando o aviso de batería retrasaba el peso 100 ms para siempre (hasta ~2,4 s tras unos minutos). Ahora se negocia MTU 247 un segundo después de suscribirse: la Dot junta varias tramas por notificación y vacía su cola. De cada notificación se usa la trama de peso (`A5 5A 01 01`) más reciente. Detalles en [`COLA_DOT_MTU.md`](./COLA_DOT_MTU.md).
4. **Motor de caudal** ([`src/flow_engine.*`](./src/flow_engine.h)). Combina una ventana de unos 350 ms, una mediana de 3, una EMA adaptativa (α 0.75/0.55/0.35/0.20), histéresis visual de 0.18 g/s y corte por "marca de agua": sin ganancia neta en 300 ms, el caudal pasa a 0.
5. **Sin carreras entre núcleos.** La tarea BLE (núcleo 0) solo decodifica y encola cada muestra con su hora; `loop()` (núcleo 1) es el único que toca el peso, la tara y el caudal.
6. **Conexión no bloqueante.** El `connect()` es asíncrono y el descubrimiento se completa en `loop()`. El encoder y la pantalla no se congelan mientras busca la báscula.
7. **OLED por páginas.** Solo se envían las franjas de 8 filas que cambian. Un volcado completo tarda unos **14-16 ms** (medido); con la pantalla quieta no se transmite nada.
8. **Encoder por hardware (PCNT)** con filtro de glitches y acumulador con histéresis: una muesca cuenta a los 3 de sus 4 pulsos, sin rebotes en la posición de reposo.
9. **`Serial` no bloqueante.** Sin monitor abierto, las trazas no frenan la CPU.
10. **PSRAM y NVS.** Cada extracción se graba en PSRAM a 2 Hz; al terminar se guarda un resumen con la curva en la NVS, en un anillo de 10 registros donde solo se escribe el nuevo.

---

## 3. Uso

### 3.1 Controles (encoder; el botón BOOT admite los mismos clics)

| Acción | Pantalla de pesaje | Menú | Visor de pour-overs |
| :--- | :--- | :--- | :--- |
| **1 clic** | Cronómetro inicio / pausa (al pausar se guarda el pour-over) | Entrar / confirmar | Alternar gráfica / ficha |
| **2 clics** | **Tara** y cronómetro a 0 (arma el Auto-Brew) | Salir | Salir |
| **3 clics** o **mantener 1.5 s** | Abrir el menú | Cerrar el menú | Salir |
| **Girar** | Cambiar de vista (4 vistas) | Mover el cursor / ajustar la dosis | Elegir pour-over |
| **BOOT mantenido 4 s** | Olvidar la báscula vinculada (re-vincular) | | |

Los clics consecutivos deben llegar con menos de 280 ms entre uno y otro.

### 3.2 Vistas (todas muestran el peso en grande)

- **Vista 0 (principal):** caudal `X.XG/S` con un medidor de 0-10 g/s, con marcas en la zona ideal de 3.5-7.5 g/s y barra rayada por encima de 7.5 g/s. Debajo, la dosis `DOSE:XX.XG`, el ratio `1:X.X` y el cronómetro (`TIMER(A)` cuando el Auto-Brew está armado).
- **Vista 1:** peso gigante, cronómetro y recordatorio de los clics.
- **Vista 2:** peso, barra de caudal (0-5 g/s), caudal en g/s y cronómetro.
- **Vista 3:** peso, gráfica en vivo del caudal (últimos 16 s) y cronómetro.

La barra superior muestra el estado de la Dot (`[FLOW]`, `[STBL]` o `[MOVE]`) y su batería. Si hay divisor, el **icono de batería del display** aparece arriba a la derecha: se anima al cargar y parpadea por debajo del 10 %.

### 3.3 Preparación con Auto-Brew

1. Coloca el dripper y la jarra y haz **2 clics**: tara a 0.0 g y cronómetro a 0, con el Auto-Brew armado (`TIMER(A)`).
2. Ajusta la dosis en *Menú → 3. Dosis* (5-60 g, en pasos de 0.5 g). Se guarda en la NVS.
3. Vierte. Con el Auto-Brew activo, el cronómetro **arranca solo** cuando el peso supera 0.5 g y el caudal 0.4 g/s. Sin Auto-Brew, un clic lo arranca.
4. Para terminar:
   - **1 clic:** para el cronómetro y guarda el pour-over. Si reanudas y vuelves a parar, se actualiza el mismo registro.
   - **Auto-stop** (con Auto-Brew): si retiras el dripper o la jarra y el peso se queda **5 s** por debajo del 70 % del máximo, el cronómetro se para en el instante de la caída y se guarda. El bloom y las pausas entre vertidos no lo disparan.
5. Aparece `POUR #N GUARDADO!` con el peso, el ratio y el tiempo.

### 3.4 Menú (3 clics)

1. **POUR-OVERS (n/10):** visor del historial. La gráfica muestra el peso acumulado y las barras de caudal; la ficha, el agua, la dosis, el ratio, el caudal medio y máximo y el número de vertidos.
2. **AUTO-TMR: ON/OFF:** arranque y parada automáticos.
3. **DOSIS:** clic para editar, girar para ajustar, clic para guardar.
4. **BRILLO:** bajo / medio / alto.
5. **RE-VINCULAR DOT:** olvida la báscula y busca otra. En este modo solo se acepta un dispositivo con servicio Timemore (0xFFF0/0xFFE0) o nombre Timemore, y su MAC se guarda al recibir la primera trama válida. Una placa recién flasheada, sin báscula vinculada, hace lo mismo al arrancar.
6. **ACTUALIZAR OTA:** ver 3.5.
7. **APAGAR (SLEEP):** reposo inmediato.
8. **SALIR.**

### 3.5 Actualización por Wi-Fi (OTA)

1. *Menú → 6. Actualizar OTA*. El LED se pone en magenta y la OLED muestra la red y su **clave**.
2. Conéctate a la red **`TIMEMORE-OTA`** con la clave de la pantalla (`TM` + 6 caracteres; única por placa y derivada de la MAC del chip).
3. Abre **`http://192.168.4.1`** y sube el `.bin` (*Sketch → Export Compiled Binary*). También puedes subirlo con ArduinoOTA a `timemore-dot.local`, con la misma clave.
4. La OLED muestra el progreso real y, al terminar, la placa se reinicia. **1 clic** cancela; sin actividad, sale a los 5 minutos.

### 3.6 LED RGB

| Color | Significado |
| :--- | :--- |
| Amarillo intermitente | Buscando o conectando con la Dot |
| Rojo intermitente | Sin báscula |
| Verde | Conectado, sin vertido |
| Cian | Vertido en curso (caudal ≥ 0.2 g/s) |
| Destello blanco | Acción confirmada (clic, tara...) |
| Magenta | Modo OTA |

### 3.7 Energía

- **Atenuado** de la OLED a los 2 min sin actividad.
- **Reposo** a los 10 min, o desde *Menú → 7*: *light sleep* real con la radio BLE apagada y RAM/PSRAM conservadas. Despierta al pulsar o girar el encoder, o al pulsar BOOT, y vuelve a conectar con la Dot.
- **Corte por batería baja** (solo con divisor): por debajo de 3.25 V sin cargar, muestra `BATERIA BAJA / CARGA POR USB-C` y entra en *deep sleep*. Solo despierta con el botón del encoder y, si sigue baja, se vuelve a apagar. En reposo se comprueba cada 10 min.
- Nota de hardware: el LED WS2812B de la placa consume del orden de 0.5-1 mA incluso apagado.

### 3.8 Monitor serie (115200)

| Tecla | Acción |
| :--- | :--- |
| `t` | Tara |
| `c` | Cronómetro a 0 |
| `r` | Olvidar la báscula vinculada |
| `x` | **Autotest** de los módulos (protocolo, motor de caudal y puente BooKoo), con resultado OK/FALLO por caso |
| `q` | **Sonda de la cola de la Dot**: envía `02 08` y mide cuánto tarda la respuesta. Con la cola vacía, unos 100 ms |

Cada 5 s se imprime `[STATUS]` con la conexión, el peso, el caudal, las baterías, el intervalo BLE, los paquetes recibidos, el tiempo de volcado de la OLED y el diagnóstico del divisor. Con la Dot conectada se añade `[LINK]`: el MTU negociado, las notificaciones con varias tramas (`tramas juntadas N/M`) y el resultado de la última sonda.

---

## 4. Estructura del código

| Archivo | Contenido |
| :--- | :--- |
| `basicoledtimemore_dot.ino` | Configuración, pines, tipos y estado global compartido (`UiState`, `PourOverRecord`, `WeightSample`, motor de caudal...) |
| `10_encoder.ino` | Encoder por PCNT |
| `20_oled.ino` | Driver de la OLED: dibujo, fuente 5×7 y volcado por páginas |
| `25_led.ino` | LED RGB de estado |
| `30_storage.ino` | NVS (báscula vinculada, dosis, anillo de pour-overs) y PSRAM |
| `40_brew.ino` | Auto-timer, auto-stop y muestreo de la extracción |
| `50_ble_scale.ino` | Bluetooth: tramas, tara, cola de muestras, vinculación, escaneo y conexión no bloqueante |
| `60_power.ino` | Batería, reposo (light sleep) y corte (deep sleep) |
| `70_ota.ino` | Actualización por Wi-Fi |
| `80_pantalla.ino` | Vistas, menú, visor e icono de batería |
| `85_controles.ino` | Encoder, multiclic, BOOT y órdenes por serie |
| `90_app.ino` | `setup()` y `loop()` |
| `src/timemore_protocol.*` | **Módulo puro**: decodificación de notificaciones y construcción de tramas Open Scale (CRC16) |
| `src/flow_engine.*` | **Módulo puro**: motor de caudal |
| `src/selftest.*` | Autotest de los dos módulos (orden `x`) |
| `partitions.csv` | Tabla de particiones de 8 MB |

El IDE de Arduino concatena las pestañas `.ino` detrás de la principal en orden alfabético, de ahí los prefijos numéricos. Los módulos de `src/` no dependen de Arduino ni de variables globales, así que se pueden probar con tramas o vertidos simulados (el autotest los ejecuta en la propia placa).
