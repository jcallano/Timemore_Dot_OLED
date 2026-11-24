# Auditoría del firmware `basicoledtimemore_dot.ino`

Fecha: 14-11-2026 · Archivo revisado: `basicoledtimemore_dot.ino` (3111 líneas), además de `README.md`, la `sdkconfig` y la tabla de particiones de `build/`.
Método: lectura completa del código y comprobación de cada hallazgo contra el código y la configuración de compilación. **No he compilado ni probado en el hardware**; los consumos y tiempos son estimaciones razonadas, y así se indica en cada caso.

> **Estado (14-11-2026): fase 1 aplicada.** Copia del original en `respaldo/basicoledtimemore_dot_antes_fase1.ino.txt`.
> Compilado con arduino-cli (esp32 3.3.12, `PartitionScheme=default_8MB`): sin errores ni avisos nuevos, 1 253 031 bytes, **37 % de 3.3 MB**. **No probado en el hardware.**
>
> | Hallazgo | Cambio |
> |---|---|
> | A3 | Nuevo `partitions.csv` (2 apps de 3.2 MB; la NVS sigue en 0x9000, así que se conservan la MAC, la dosis y los pour-overs). Además, en el IDE: *Partition Scheme → "8M with spiffs (3MB APP/1.5MB SPIFFS)"*, para que la comprobación de tamaño use el límite nuevo. **La primera carga, por USB.** |
> | A1 | Con báscula vinculada solo se acepta su MAC; en modo re-vincular se exige 0xFFF0/0xFFE0 o un nombre TIMEMORE/BLACK MIRROR/MACARON. El descubrimiento GATT genérico solo se usa con la MAC ya vinculada. La MAC se guarda al llegar el **primer peso o la primera trama Timemore válida** (por ejemplo, la respuesta de batería que se pide al conectar), no al conectar. Si en 10 s no llega nada así: desconexión y lista de descartados (solo en modo re-vincular; la báscula ya vinculada nunca se descarta). |
> | A4 | Cuando el peso bruto cae a 0 ya no se anula la tara: si vuelve de golpe por encima del 50 % del offset, se repuso la jarra y la tara se conserva; si sube poco a poco desde 0, fue la tara física y se anula. |
> | M3 | Pausar y reanudar actualiza el mismo pour-over (`activeRecordingPourId`) en lugar de crear otro. |
> | M7 | Si el paquete trae tramas `A5 5A` válidas sin peso, se devuelve sin pasar por los parsers heurísticos. (El CRC **no** se verifica todavía: falta confirmar con la báscula real que el CRC de las notificaciones es el mismo que el de los comandos.) |
> | M6 | AP WPA2 y ArduinoOTA con clave única por placa, `TM` + 6 hexadecimales de la MAC, que se muestra en la OLED; se comprueban `Update.begin/write`, `Update.abort()` en caso de error, y el progreso usa el tamaño real. |
> | B1 / B6 / B7 / B8 | Dosis 5-60 g (`DOSE_MIN_G`/`DOSE_MAX_G`); TX a +3 dBm; eliminados `BTN_TARE/TIMER_MAX_MS`, `OLED_REFRESH_MS`, `pendingTimerCmd` y `executeTimemoreTimerAction()`; cabecera actualizada; `millis()` actual en el re-escaneo y el keep-alive. |
>
> **Fase 2 aplicada (energía)**, con copia previa en `respaldo/basicoledtimemore_dot_antes_fase2.ino.txt`. Compila sin avisos nuevos (1 267 559 bytes, 37 %), subida a COM14, y arranca y conecta con la Dot. **No he probado el reposo ni el corte en el hardware.**
>
> | Hallazgo | Cambio |
> |---|---|
> | A2 reposo | `enterDeepSleepMode()` usa **light sleep** real: pila NimBLE desinicializada (`shutdownBleStack()`), pines con `gpio_sleep_sel_dis()` y despertar por GPIO al pulsar el encoder o BOOT, o al girar (nivel contrario al actual de cada fase). Se conservan RAM, PSRAM y estado; al despertar, `initBleStack()` y búsqueda. Despierta además cada 10 min para medir la batería. |
> | A2 corte | `BATT_CUTOFF_VOLTS` (3.25 V) ya se usa: 2 lecturas seguidas por debajo y sin cargar hacen que se muestre "BATERIA BAJA / CARGA POR USB-C" y entre en **deep sleep**, del que solo despierta el botón del encoder (EXT1 en el GPIO 12, con pull-up RTC); al arrancar se vuelve a medir. También se comprueba durante el reposo. La primera lectura del divisor se toma tal cual (antes tardaba ~30 s en converger desde 4.0 V). |
> | Fase 2 · 9 | Icono de batería del display (13×7 px, arriba a la derecha): relleno proporcional, animado al cargar y parpadeando por debajo del 10 %. En la vista 0 el medidor de caudal se acorta a 76 px para dejarle sitio. |
>
> ⚠ El icono y el corte **solo funcionan con el divisor 100k/100k de B+ al GPIO 4**. En el arranque de prueba el firmware no lo detectó (la línea `[STATUS]` no muestra `BatHW107`), así que hoy la placa no lo tiene o no está conectado. Sin divisor, el reposo en light sleep funciona igual, pero no hay corte ni icono.
>
> **Fase 3 aplicada (robustez)**, con copia previa en `respaldo/basicoledtimemore_dot_antes_fase3.ino.txt`. Compila sin avisos nuevos (1 271 139 bytes, 38 %) y está subida a COM14. Verificado por monitor serie: conecta y recibe 10 paquetes/s sin pérdidas en la cola, y el intervalo BLE baja a 15 ms.
>
> | Hallazgo | Cambio |
> |---|---|
> | M1 | El callback BLE solo parsea y encola `{ms, peso, estable}` en una cola FreeRTOS de 32; `loop()` la vacía (`drainWeightQueue()`) y ejecuta `processWeightSample()` (tara, motor de caudal). `onDisconnect` solo levanta `bleDisconnectEvent`. El estado de peso y caudal tiene un único dueño. |
> | M5 | Conexión no bloqueante: `connect()` asíncrono + máquina `BLE_IDLE/CONNECTING/LINK_UP/LINK_FAILED`; `serviceBleConnection()` reintenta con el otro tipo de dirección y completa el descubrimiento en `finishScaleConnection()`. Fuera los `delay(60)`/`delay(120)`: la consulta de batería se programa (2 intentos). |
> | M4 | `oledFlush()` solo envía las páginas que cambian (copia `oledShadow`). **Medido:** un volcado completo tarda **14-16 ms** (peor que mis 12 ms estimados); con la pantalla quieta se envían 0 páginas. `[STATUS]` muestra último y máximo tiempo de volcado y páginas cada 5 s. |
> | M2 | Encoder con acumulador e histéresis (una muesca cuenta a los 3 de 4 pulsos) y *watch points* del PCNT en ±32000 para que `accum_count` funcione. |
> | B2 | Auto-stop con Auto-Brew: si el peso cae por debajo del 70 % del máximo de la extracción (≥ 20 g) durante 5 s, el cronómetro para en el instante de la caída, recorta las muestras de la caída y guarda. El bloom no lo dispara. |
> | B3 | NVS en anillo: un registro por clave `rec_<id % 10>`; solo se escribe el nuevo; al cargar se valida `magic`, el id y el hueco, y se ordena por id. Migración automática y única desde `pour_0..9`. |
> | Intervalo BLE | Diagnóstico: la Dot **no** envía peticiones que pasen por `onConnParamsUpdateRequest` (0 recibidas), pero el enlace quedaba en 60 ms. Ahora, si el intervalo supera 20 ms, el central vuelve a pedir 10-15 ms (hasta 5 veces). **Resultado medido: 60 ms → 15 ms** en el primer intento. |
> | Batería | Sonda del divisor robusta: compara pull-down y pull-up internos (antes, con pull-down solo y la celda a ~3.8 V, un divisor 100k/100k quedaba en el umbral). Diagnóstico en `[STATUS]`. **En la placa actual el GPIO 4 está al aire (pd = 80 mV, pu = 3082 mV): el divisor no está montado**, y por eso no aparece el icono. |
>
> **Fase 4 aplicada (mantenibilidad)**, con copias previas en `respaldo/basicoledtimemore_dot_antes_fase4.ino.txt` y `respaldo/README_antes_fase4.md`. Compila **sin ningún aviso**, incluso con `--warnings all` (1 275 275 bytes, 38 %), está subida a COM14 y el autotest en la placa da **29 OK, 0 fallos**. Conecta y baja a 15 ms.
>
> | Punto | Cambio |
> |---|---|
> | Módulos puros | `src/timemore_protocol.*` (`tmproto::parsePayload()` devuelve un `ParseResult` sin tocar globales; `buildFrame()` y `crc16()`) y `src/flow_engine.*` (clase `FlowEngine`: `addSample`, `decayIfStale`, `reset`). El algoritmo es idéntico; los nombres anteriores (`currentFlowRateGps`...) son referencias al motor. |
> | Pruebas | No hay compilador de C++ para el PC en este equipo, así que el autotest (`src/selftest.*`) se ejecuta **en la propia placa** con la orden `x` por el monitor serie. 17 casos de protocolo (CRC de referencia, varias tramas por paquete, bytes previos, truncadas, batería, **regresión M7** del ACK de 9 bytes, formatos 0x2A6D, ASCII y UART) y 12 de caudal (vertidos simulados a 10 Hz con la resolución de 0.1 g de la Dot y el ADC a 5 Hz: constante, lento, corte, oscilación ±0.1 g, colocar la jarra, pico y decaimiento sin paquetes). Es C++11 portable: compila en un PC si algún día hay compilador. |
> | Pestañas | El `.ino` de 3300 líneas pasa a ser una pestaña principal (configuración, tipos y estado global) más 11 pestañas por área (`10_encoder` … `90_app`). Se añadió `sketchPrototypeAnchor()` para que el IDE inserte los prototipos automáticos después de los tipos. |
> | `UiState` | `enum class UiState { Main, Menu, MenuEditDose, PourViewer }` sustituye a `inSettingsMenu`, `editingDoseInMenu` e `inPourOverViewer`. |
> | Avisos | Corregidos los 5 restantes: buffers de texto del visor y de la OTA, el texto de la ficha (`"VERTIDOS:%u 3CLK=SALIR"`, que antes se cortaba en pantalla), la comparación siempre verdadera del parser y el `#undef Serial` explícito. |
> | README | Reescrito: hardware actual (OLED 2.42", 18650 en portapilas, MTS-103), conexiones con el divisor opcional, FQBN con `PartitionScheme=default_8MB`, controles, vistas, menú de 8 opciones, Auto-Brew y auto-stop, OTA con clave, LED, energía, órdenes por serie y estructura del código. |
>
> **Las cuatro fases de la auditoría están aplicadas.** Lo que queda solo se puede comprobar en el uso real: la sensación del encoder, el auto-stop, el reposo y el despertar, la migración del historial y la OTA con clave. Aviso ya existente: en la ficha del visor, `"VERTIDOS:%u  [3xCLK=SALIR]"` no cabe en 21 caracteres y se corta.

---

## 1. Resumen

El firmware está **muy trabajado y bien pensado en lo que más importa al usuario: la latencia**. El rechazo de la petición de parámetros de conexión del PHY6222, la tara por software, el `Serial` no bloqueante, el encoder por PCNT y el motor de caudal en tres etapas son decisiones con criterio, y están explicadas en los comentarios. Para un único `.ino` de 3100 líneas, se lee bien.

Los problemas importantes **no están en la parte de tiempo real, sino alrededor**:

| # | Prioridad | Problema | Impacto |
|---|---|---|---|
| A1 | **Alta** | La búsqueda BLE acepta casi cualquier dispositivo (nombre con "DOT", servicio 0x181A…) aunque ya haya una MAC guardada, y la nueva **se guarda en NVS** | Puede quedarse vinculado a un termómetro Xiaomi o a un Echo Dot del vecino |
| A2 | **Alta** | El "modo reposo" es un bucle activo a 80 MHz, no un sleep, y **no hay corte por batería baja** | Unos 15-25 mA en reposo (estimado); la 18650 se descarga hasta los 2.4 V del DW01A |
| A3 | **Alta** | Partición de la app llena al **95 %** (1.25 / 1.31 MB) y la mitad de la flash de 8 MB sin usar | La próxima función nueva puede no caber, ni por OTA ni por USB |
| A4 | **Alta** | Levantar la jarra (o el V60 con la jarra) **anula la tara por software** | Al volver a apoyarla marca +350 g y el ratio se rompe en pleno vertido |
| M1 | Media | Carrera de datos entre la tarea BLE (núcleo 0) y `loop()` (núcleo 1), sin mutex ni cola | Fallos esporádicos de peso o caudal justo después de la tara |
| M2 | Media | Encoder: el umbral de muesca coincide con la posición de reposo | Pasos dobles o rebotes al cambiar de vista o de dosis |
| M3 | Media | Pausar y reanudar el cronómetro guarda **un pour-over duplicado** en cada pausa | El historial se llena de registros repetidos |
| M4 | Media | La OLED transfiere 1 KB completo en cada redibujado: unos **12 ms**, no los 1.4 ms que dice el README | Hasta un 30 % del bucle bloqueado en I2C |
| M5 | Media | `connectToTimemoreScale()` bloquea `loop()` hasta ~7 s | Encoder y pantalla congelados mientras intenta conectar |
| M6 | Media | OTA con Wi-Fi abierto, sin contraseña y sin comprobar errores de `Update` | Cualquiera cerca puede flashear durante los 5 minutos de la ventana OTA |
| M7 | Media | Tramas `A5 5A` válidas pero sin peso pasan a los parsers heurísticos | Latente: una trama de 9 bytes se leería como ~328 g |

Más abajo hay 9 hallazgos de prioridad baja y una sección sobre **documentación desfasada**: el README describe vistas, controles, rangos y colores de LED que el código ya no tiene.

---

## 2. Lo que está bien hecho

- **Latencia BLE**: rechazar en `onConnParamsUpdateRequest` las peticiones con intervalo mayor de 20 ms o latencia mayor de 0, y volver a imponer 10-15 ms (línea 1590), es exactamente la solución correcta para el PHY6222.
- **Tara por software** (línea 1344): evita el *zero-lock* de la báscula y elimina el retardo tras tarar. Es una idea muy buena.
- **`NonBlockingUsbSerial`** (línea 79): resuelve el bloqueo de 100 ms del HWCDC sin monitor serie abierto. Bien resuelto.
- **Encoder por PCNT** con filtro de glitches: cero carga de CPU.
- **Motor de caudal**: ventana deslizante, mediana de 3, EMA adaptativa e histéresis visual, con "marca de agua" para el corte. Está bien ajustado al caso de uso (impacto del chorro, oleaje).
- **Parser de tramas agrupadas** (`A5 5A`): recorre el paquete entero y se queda con la última lectura.
- **PSRAM** para la grabación en vivo y NVS solo al terminar: buen reparto entre memoria volátil y flash.
- **Compatibilidad NimBLE 1.4 y 2.x**, y robustez general: punteros nulos comprobados y `snprintf` con tamaño en todas partes.

---

## 3. Hallazgos detallados

### A1 · Emparejamiento demasiado permisivo, que además sobrescribe la MAC guardada — **Alta**
**Dónde:** `isTimemoreAdvertisedDevice()` (línea 1643), `handleScannedDevice()` (línea 1692) y `saveScaleToNVS()` dentro de `connectToTimemoreScale()` (línea 1955).

Aunque haya una MAC guardada (que siempre la hay, porque si la NVS está vacía se carga `DEFAULT_TIMEMORE_DOT_MAC`), el filtro acepta **el primer dispositivo** que cumpla cualquiera de estas condiciones:
- el nombre contiene `"DOT"`: Echo Dot de Amazon, "Polar Dot", etc.;
- anuncia el servicio o el *service data* **0x181A (Environmental Sensing)**: es exactamente lo que emiten los **termómetros Xiaomi LYWSD03MMC con firmware ATC/pvvx**, muy comunes en casa;
- 0x181D (cualquier báscula de baño BLE estándar).

Si ese dispositivo tiene alguna característica *notify*, el descubrimiento dinámico (paso 5, línea 1828) la acepta, la conexión "tiene éxito" y **se guarda su MAC en NVS**. Desde ese momento la coincidencia directa por MAC (paso 1) prioriza el dispositivo equivocado en cada búsqueda, hasta que hagas "Re-vincular".

**Escenario:** tienes un termómetro pvvx en la cocina. Apagas la Dot, el display busca, el termómetro anuncia 0x181A, se conecta y el display muestra "pesos" sin sentido o nada. Al día siguiente sigue vinculado al termómetro.

**Propuesta:**
- Con MAC guardada, aceptar **solo** esa MAC. El filtro amplio, solo en modo "Re-vincular".
- En modo vinculación, exigir el servicio **0xFFF0** (el protocolo oficial de la Dot) o un nombre que empiece por `TIMEMORE`, y quitar `"DOT"` suelto y 0x181A/0x181D como criterios suficientes.
- Guardar en NVS solo después de recibir **una trama de peso válida** (`A5 5A … cmd 0x01`), no al suscribirse.

### A2 · El "reposo" consume como si estuviera despierto, y no hay corte por batería — **Alta**
**Dónde:** `enterDeepSleepMode()` (líneas 1988-2098); `BATT_CUTOFF_VOLTS` definida en la línea 172 y **sin usar**.

- Las constantes y los comentarios hablan de "Deep Sleep (<50 µA)", pero la función hace un `while(true){ digitalRead…; delay(15); }` a 80 MHz, con el controlador BLE inicializado y sin `CONFIG_PM_ENABLE`. Estimo **15-25 mA**, es decir, una 18650 de 3000 mAh en "reposo" dura **unos 5-7 días**.
- No existe corte por software: la celda baja hasta que el DW01A desconecta a **2.4 V**, lo que degrada la 18650 y además deja el sistema con *brown-outs* intermitentes.

**Propuesta, de menor a mayor esfuerzo:**
1. **Light sleep** con despertar por GPIO, lo más sencillo y conservando PSRAM y estado: `gpio_wakeup_enable()` en el GPIO 12 (y en el 10/11 para despertar al girar), `esp_sleep_enable_gpio_wakeup()` y `esp_light_sleep_start()`. Sin problema de *strapping*, porque el light sleep no reinicia. Previamente: `NimBLEDevice::deinit(true)` o, como mínimo, parar el escaneo. Consumo estimado del chip: alrededor de 1 mA.
2. **Deep sleep real** por EXT1 solo con el GPIO 12, sin el GPIO 0: `rtc_gpio_pullup_en(GPIO_NUM_12)` y `esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON)` mantienen el pull-up. Esto resuelve los dos motivos que da el comentario para no usarlo. Los pour-overs ya están en NVS, así que perder la PSRAM no importa.
3. **Corte por batería:** si `localBattVolts` es menor de `BATT_CUTOFF_VOLTS` durante más de 10 s, mostrar "BATERIA BAJA" y entrar en deep sleep sin despertar por giro.
4. En hardware hay consumos que el firmware no elimina: el WS2812B del GPIO 48 gasta unos 0.5-1 mA aunque esté "apagado", el divisor 100k+100k unos 20 µA y la OLED en *display off* unos 10 µA. Para una autonomía de semanas, plantéate alimentar el LED desde un GPIO o cortar la OLED con un MOSFET.

### A3 · Partición de la app al 95 %, con la mitad de la flash sin usar — **Alta**
**Dónde:** `build/…partitions.bin`: `app0` y `app1` de 1 310 720 bytes, `spiffs` de 1.4 MB, y la tabla termina en 4 MB. El `.bin` ocupa **1 250 112 bytes (95.4 %)**.

El FQBN dice `FlashSize=8M`, pero la tabla es la de 4 MB por defecto: **4 MB de flash sin usar**. El margen que queda (unos 60 KB) se agota con cualquier mejora (más vistas, Wi-Fi, otra librería) y entonces fallan tanto la OTA como la carga por USB.

**Propuesta:** compilar con `PartitionScheme=default_8MB` (dos apps de 3.3 MB). En arduino-cli: `esp32:esp32:esp32s3:…,PartitionScheme=default_8MB`. Ten en cuenta que **la primera vez hay que cargar por USB**, porque una OTA no cambia la tabla de particiones.

### A4 · Levantar la jarra anula la tara — **Alta**
**Dónde:** `timemoreNotifyCallback()`, líneas 1419-1424.

```cpp
if (fabsf(parsedWeight) <= 0.10f && fabsf(softwareTareOffset) > 2.0f &&
    fabsf(parsedWeight - rawWeightGrams) > 2.0f) {
  softwareTareOffset = 0.0f;   // <- se pierde la tara
```

La intención es detectar que se pulsó el botón de tara físico de la Dot. Pero como la tara es por software, el cero interno de la báscula es **la plataforma vacía**. Al levantar todo lo que hay encima (algo muy habitual para remover el V60 o servir), el peso bruto vale 0.0, se cumple la condición y la tara desaparece.

**Escenario:** tara con la jarra y el V60 (offset de 350 g), viertes 120 g, levantas el conjunto para remover y lo vuelves a apoyar. La pantalla marca 470 g, el ratio 1:31 y el pour-over se guarda con datos erróneos.

**Propuesta:** distinguir los dos casos. Un *tare* físico deja el peso bruto en 0 **con la jarra encima**, y después el peso sigue subiendo desde 0. Una retirada vuelve, al reponerla, a un valor cercano a `softwareTareOffset`. Opciones:
- (a) no anular el offset; si el bruto vuelve a ±3 g del offset en menos de 10 s, mantener la tara;
- (b) eliminar la autodetección y documentar que la tara se hace siempre desde el display, que es lo coherente con el diseño.

### M1 · Carrera de datos entre núcleos — Media
**Dónde:** todo el motor de caudal vive en `timemoreNotifyCallback()` (líneas 1393-1575), que se ejecuta en la tarea host de NimBLE. La `sdkconfig` confirma `CONFIG_BT_NIMBLE_PINNED_TO_CORE=0`, mientras `loop()` corre en el núcleo 1 (`CONFIG_ARDUINO_RUNNING_CORE=1`). Desde `loop()` se modifican las mismas variables: `executeTimemoreTare()` llama a `resetFlowWindow()` y escribe `softwareTareOffset`, y `updateFlowAndAutoTimer()` escribe `currentFlowRateGps`, `displayedFlowRateGps` y `weightIsStable`. **No hay ni un mutex, una cola ni un `portMUX` en todo el archivo.**

`volatile` no protege eso: solo garantiza que cada lectura vaya a memoria, no que un conjunto de variables (los arrays de la ventana, el offset y el peso) sea coherente entre sí.

**Escenario:** haces doble clic de tara justo mientras llega un paquete. El callback ya había leído el offset antiguo y escribe `currentWeightGrams` después de la tara; la ventana de caudal queda medio reiniciada. Resultado: un pico de caudal o un peso no nulo durante 100 ms. Es poco frecuente, pero real, y difícil de depurar.

**Propuesta (es la mejora estructural más rentable):** el callback solo **parsea y encola** `{millis, peso bruto, estable}` en una `xQueueCreate(16, sizeof(Sample))`, y `loop()` vacía la cola y ejecuta allí el motor de caudal, la tara y el auto-timer. Todo el estado pasa a tener un único dueño. Además la tarea BLE queda libre de cálculo en coma flotante.

### M2 · Rebote del encoder en el umbral de muesca — Media
**Dónde:** `handleRotaryEncoder()`, línea 2818: `getEncoderPulseCount() / ENC_STEPS_PER_DETENT`.

En un EC11 de 4 pulsos por muesca, la posición de reposo cae justo en un múltiplo de 4, que es **exactamente el umbral** de la división. Un rebote de un solo pulso (3↔4) hace oscilar el detent 0↔1. Además, la división en C++ trunca hacia cero, así que con conteos negativos el umbral es asimétrico. El filtro de 10 µs del PCNT no cubre rebotes de milisegundos. El RC de tu PCB ayuda, pero no lo garantiza.

**Propuesta:** desplazar el umbral a media muesca y aplicar histéresis.
```cpp
int32_t c = getEncoderPulseCount();
static int32_t acc = 0;              // pulsos acumulados desde la última muesca
acc += c - lastCount; lastCount = c;
while (acc >= 3)  { acc -= 4; delta++; }   // umbral asimétrico = histéresis
while (acc <= -3) { acc += 4; delta--; }
```
Detalle adicional: `unitCfg.flags.accum_count = 1` no hace nada sin `pcnt_unit_add_watch_point()` en ±32000. Al desbordar, el contador vuelve a 0 y genera un salto de −8000 muescas, que en la edición de la dosis la lleva de golpe a 5 g. Es raro, pero sale gratis añadir los *watch points* o usar el acumulador incremental de arriba.

### M3 · Pausar el cronómetro guarda un pour-over cada vez — Media
**Dónde:** `startOrStopStopwatch()`, línea 2690. Cada pausa llama a `finalizeAndSaveCurrentPourOver()`. Al reanudar no se reinicia la grabación (porque `timerAccumulatedMs != 0`), y la siguiente pausa **guarda otro registro**, con un id nuevo y la curva acumulada.

**Escenario:** un pour-over con una pausa intermedia acaba ocupando 2 de los 10 huecos del historial.

**Propuesta:** guardar solo al **terminar**. Por ejemplo: 1 clic pausa sin guardar, y el guardado ocurre con el doble clic (tara y reset) si hay una grabación pausada, o con el auto-stop (ver B2). Alternativa: al guardar, si `rec.pourId` coincide con la grabación en curso, sobrescribir ese mismo hueco.

### M4 · Coste real del refresco de la OLED — Media
**Dónde:** `oledFlush()` (línea 762) y la lógica de refresco de `loop()` (línea 3065).

Cada refresco envía 8 páginas × (comando + 129 bytes), unos 1080 bytes. A 9 bits por byte y 800 kHz eso son **≈12 ms**, no los 1.4 ms que dice el README. Y eso suponiendo que el bus llegue de verdad a 800 kHz: los módulos OLED suelen llevar pull-ups de 4.7-10 kΩ, y con ellos los flancos no dan para Fast-mode Plus, así que el periférico puede quedarse por debajo. Se redibuja con cada paquete que cambia el peso (10 Hz) más el refresco del cronómetro (10 Hz).

**Propuesta, de mayor a menor beneficio:**
1. **Envío por páginas modificadas:** guardar una copia del último frame enviado (`oledShadow[1024]`) y transmitir solo las páginas que cambian (`memcmp` de 128 bytes por página). El peso solo ocupa 3 páginas, y con el cronómetro corriendo suelen cambiar 1 o 2. Ahorro típico: 60-85 % del tiempo de I2C.
2. No redibujar si el frame es idéntico al anterior (por ejemplo, el peso cambió 0.01 g pero el texto en pantalla es el mismo).
3. Medir la frecuencia real del bus con un `micros()` alrededor de `oledFlush()` e imprimirla en `[STATUS]`.
4. Dibujo: `oledFillRect` y `oledDrawChar` van píxel a píxel con comprobación de límites. Con *spans* de bytes y glifos escalados mediante tabla sería unas 5-10 veces más rápido, aunque esto importa menos que el I2C.

### M5 · Conexión BLE bloqueante — Media
**Dónde:** `connectToTimemoreScale()`, línea 1878, llamada desde `loop()`. Suma `delay(60)`, un `connect()` síncrono de hasta 4.5 s, un reintento de 2 s, el descubrimiento GATT, `delay(120)`… Mientras tanto no se leen ni el encoder ni el botón y la OLED no se actualiza.

**Propuesta:** con NimBLE 2.x, `connect(addr, true, /*asyncConnect*/ true)` y continuar en `onConnect()`; o bien mover toda la gestión BLE (escaneo, conexión, descubrimiento) a una tarea propia que se comunique con `loop()` mediante flags o una cola, lo que encaja con M1.

Efecto colateral: tras volver de la conexión, `now` (capturado al principio de `loop()`) es anterior a `lastScanAttemptMs` y `lastKeepAliveMs`. La resta sin signo da un desbordamiento enorme y se reescanea o se envía un keep-alive duplicado al instante (ver B4).

### M6 · Seguridad y robustez de la OTA — Media
**Dónde:** `enterWifiOtaMode()`, línea 2140.
- `WiFi.softAP("TIMEMORE-OTA")` es una red **abierta**, y `ArduinoOTA` va **sin contraseña**: durante los 5 minutos de la ventana, cualquiera en alcance puede cargar un firmware.
- No se comprueba el retorno de `Update.begin()` ni de `Update.write()`. Si la escritura falla a mitad, el usuario sigue viendo el progreso hasta el final.
- El progreso supone un binario de 660 000 bytes, pero el tuyo ocupa 1.25 MB, así que la barra se clava en 99 % a mitad de la subida.

**Propuesta:** `WiFi.softAP("TIMEMORE-OTA", "<clave de 8+ caracteres>")`, `ArduinoOTA.setPassword(…)`, comprobar los retornos y abortar con `Update.abort()`, y calcular el progreso con la cabecera `Content-Length` de la petición. Opcional: validar un MD5 con `Update.setMD5()`.

### M7 · Tramas `A5 5A` sin peso que acaban en los parsers heurísticos — Media (latente)
**Dónde:** `parseTimemorePayload()`, línea 1171. Si el paquete contiene tramas `A5 5A` válidas pero ninguna de peso (un ACK, una respuesta de batería…), la función **sigue** con los casos B, C1, C2 y D. Una trama de **9 bytes** (por ejemplo, un ACK con 1 byte de *payload*) cae en el caso C1 (`len == 9`), que lee los bytes 1-4 como miligramos: `5A 03 05 00` da 0x0005035A = 328 538 mg, es decir **328.5 g**, un valor que pasa el filtro de ±5000 g.

No sé si la Dot envía tramas de 9 bytes; si lo hiciera ya lo habrías notado como saltos cada 25 s, cuando sale el keep-alive. Es un fallo latente que se activaría con un firmware nuevo de la báscula.

**Propuesta:** si se encontró al menos una trama `A5 5A` válida, `return foundWeightInBatch;` sin pasar a las heurísticas. De paso, conviene **verificar el CRC16** de las tramas recibidas, porque la función ya existe.

---

### Prioridad baja

| # | Dónde | Hallazgo | Propuesta |
|---|---|---|---|
| B1 | Líneas 958 y 2836 | Dosis limitada a **5-40 g**, mientras el README dice 5-60 g. Al cargar desde NVS, un valor mayor de 40 se convierte en 15 g | Unificar en `DOSE_MIN`/`DOSE_MAX` |
| B2 | `lowFlowSinceMs` (líneas 321 y 1104) | El **auto-stop no está implementado**: la variable se pone a 0 y nunca se usa. El README describe que el cronómetro se para tras 4 s sin peso | Implementarlo en `updateFlowAndAutoTimer()`: si el caudal se mantiene por debajo de 0.2 g/s durante 4 s, parar, restar esos 4 s y guardar |
| B3 | `finalizeAndSaveCurrentPourOver()` (línea 1037) | Reescribe **los 10 registros** en NVS cada vez (unos 2.7 KB, bloqueante), y al cargar no valida `magic` ni la versión de la estructura | Guardar solo el hueco nuevo con un índice circular `head`; validar `magic` y añadir `uint8_t version` para que un cambio de estructura no muestre basura |
| B4 | Varias | Las comparaciones del tipo `(now >= x) ? now - x : 0` **rompen** el desbordamiento de `millis()` a los 49.7 días (la resta sin signo ya lo resuelve sola); `now` queda viejo tras las llamadas bloqueantes | Usar siempre `(uint32_t)(millis() - t) >= intervalo` y refrescar `now` después de cualquier llamada bloqueante |
| B5 | `isrEncoderAB()` (línea 460) | La ISR de respaldo llama a `digitalRead()`, que no está en IRAM | `gpio_get_level()`, o quitar el respaldo, porque el S3 siempre tiene PCNT |
| B6 | `setPower(ESP_PWR_LVL_P9)` (línea 2994) | Potencia de emisión máxima para una báscula a 30 cm | `ESP_PWR_LVL_N0` o `P3`: menos consumo e interferencia con el Wi-Fi |
| B7 | Varias | Código muerto: `BTN_TARE_MAX_MS`, `BTN_TIMER_MAX_MS`, `OLED_REFRESH_MS`, `BATT_CUTOFF_VOLTS`, `executeTimemoreTimerAction()` vacía, `pendingTimerCmd`, `scaleBatteryBars`; `localBattPct` nunca se muestra en la OLED | Eliminarlo o usarlo (por ejemplo, mostrar la batería del display en la vista 1) |
| B8 | Cabecera (líneas 1-48) | El comentario inicial describe otra versión: OLED de 1.54" M154, I2C a 400 kHz y controles del botón BOOT por duración | Actualizarlo (ahora es una OLED de 2.42" con encoder y multiclic) |
| B9 | Línea 843 | Tu MAC escrita en el código | Mejor en un `secrets.h` o solo en NVS, si vas a compartir el código |

---

## 4. Documentación desfasada (README frente a código)

| Tema | README dice | El código hace |
|---|---|---|
| OLED | 1.54" M154-4P | Modelo de 2.42" (TZT) en el hardware nuevo |
| Batería | LiPo | 18650 en portapilas (nueva caja) |
| Vista 0, girar el encoder | Ajusta la dosis en pasos de 0.5 g | **Cambia de vista**; la dosis solo se edita en el menú |
| Vista 2 | Telemetría *raw* BLE (HEX, PKTS, Hz, RSSI) | Peso + barra de caudal + cronómetro |
| Vista 3 | Monitor de sistema (PSRAM, heap, uptime) | Peso + gráfica de caudal en vivo |
| Pulsación larga | ≥ 1.8 s, cambia de vista o abre el menú | ≥ 1.5 s, abre o cierra el menú |
| Menú | 7 opciones (1. Ver pour-overs … 7. Salir) | 8 opciones en otro orden (2. Auto-timer, 4. Brillo, 5. Re-vincular, 7. Apagar…) |
| Dosis | 5-60 g | 5-40 g |
| Historial | Visor `#1..#5` | Hasta 10 |
| Intervalo BLE | 7.5-15 ms (6..12) | 10-15 ms (8..12) |
| Auto-timer | Arranca con > 0.5 g y > 0.8 g/s | > 0.5 g y **≥ 0.40 g/s** |
| Auto-stop | Se para solo tras 4 s sin peso | **No implementado** (B2) |
| Colores del LED | Ámbar, verde, **azul** (peso activo), cian, violeta | Amarillo parpadeante, verde, cian, **rojo parpadeante** (desconectado), magenta |
| Reposo | "Deep sleep < 50 µA" en las constantes | Bucle activo a 80 MHz (A2) |
| OLED | Volcado en ~1.4 ms | ~12 ms (M4) |
| Estructura | La sección "2. Estrategias…" aparece **dos veces** (líneas 75-111 y 113-149) | — |

---

## 5. Hoja de ruta propuesta

**Fase 1: arreglos rápidos (unas horas, sin cambiar la arquitectura)**
1. A3: cambiar a `PartitionScheme=default_8MB` (una carga por USB).
2. A1: con MAC guardada, aceptar solo esa MAC; endurecer el filtro de vinculación.
3. A4: quitar o corregir la autodetección de la tara física.
4. M3: no guardar al pausar.
5. M7: `return` tras encontrar tramas `A5 5A`, y verificar el CRC.
6. M6: contraseña en el AP y en ArduinoOTA, comprobar `Update`, progreso real.
7. B1, B6, B7: unificar el rango de dosis, bajar la potencia TX y limpiar el código muerto.

**Fase 2: energía (lo que más notarás con la 18650)**
8. A2: light sleep, o deep sleep EXT1 en el GPIO 12, más el corte por `BATT_CUTOFF_VOLTS`.
9. Mostrar la batería del propio display (`localBattPct`) y un icono de carga en la OLED.

**Fase 3: robustez estructural**
10. M1 + M5: una cola de muestras BLE hacia `loop()`, y la gestión de conexión en una tarea propia o asíncrona.
11. M4: envío de páginas modificadas a la OLED y medición real del bus.
12. M2: encoder con acumulador e histéresis.
13. B2: auto-stop.
14. B3: NVS con índice circular, `magic` y versión.

**Fase 4: mantenibilidad (opcional)**
- Dividir el `.ino` en módulos: `ble_scale.{h,cpp}`, `flow_engine.{h,cpp}`, `oled_gfx.{h,cpp}`, `ui.{h,cpp}`, `power.{h,cpp}` y `storage.{h,cpp}`. Hoy todo es estado global compartido, y separarlo hace visibles las dependencias de M1.
- Sustituir los `bool` del estado de la interfaz (`inSettingsMenu`, `inPourOverViewer`, `editingDoseInMenu`) por un `enum class UiState`.
- Extraer `parseTimemorePayload()` y el motor de caudal a funciones puras, sin globales, para **probarlos en el PC** con tramas grabadas. Es la mejor forma de ajustar los filtros sin estar vertiendo agua.
- Actualizar el README (sección 4).
