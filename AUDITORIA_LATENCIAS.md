# Auditoría de latencias: Dot → panel → OLED / máquina (14-11-2026)

Firmware con puente BooKoo (`55_bookoo_bridge.ino`). Fuentes: grabaciones con la instrumentación `[LAT]`
(ya retirada), los `[STATUS]` del firmware actual y la lectura del código. **M** = medido en la placa,
**E** = estimado a partir del código o de la especificación BLE (no hay Lita conectada para medirlo).

## 1. Camino del peso

| Tramo | Típico | Peor caso | Fuente |
|---|---|---|---|
| Sensor y filtro internos de la Dot (PHY6222) | ? | ? | Fuera de nuestro control; suele ser el mayor (cientos de ms en básculas de este tipo) |
| Enlace Dot → panel (intervalo 15 ms) | 7.5 ms | 15 ms | **M** (15.0 ms estable) |
| …durante los primeros ~8 s tras conectar (60 ms) | 30 ms | 60 ms | **M** (`BLE=60.0ms` hasta el primer reintento) |
| Callback → cola → `loop()` | 0.03-0.07 ms | 0.5 ms (4.7 ms al conectar) | **M** |
| `loop()` → OLED actualizada (render + I2C) | 10-14 ms | 15.6 ms (20 ms al conectar) | **M** |
| Callback → tarea `bookooTx` → `notify()` | < 0.1 ms | ~1 ms | **E** (notificación de tarea en el mismo núcleo, prioridad 5) |
| Enlace panel → máquina (pedido 15-30 ms) | 7.5-15 ms | 30 ms | **E** (la Lita puede no aceptar lo pedido) |

**Total que añade el panel:**
- Peso → OLED: ~20 ms típico, ≤ 31 ms en el peor caso (≤ 76 ms en los primeros 8 s tras conectar).
- Peso → máquina: ~15-25 ms típico, ≤ 46 ms en el peor caso. No pasa por `loop()` ni espera a la OLED.

Las muestras llegan a 10 Hz y no se pierde ninguna (`Rx` sube 50 cada 5 s; `Muestras BLE perdidas=0`).

## 2. Camino del caudal (g/s)

El motor de caudal (`src/flow_engine.cpp`) prioriza la estabilidad sobre la rapidez:

| Etapa | Retardo de grupo aprox. |
|---|---|
| Pendiente sobre una ventana de 320-400 ms | ~175 ms |
| Mediana de 3 estimaciones (10 Hz) | ~100 ms |
| EMA adaptativa (alfa 0.75 / 0.55 / 0.35 / 0.20) | 35 ms (cambio grande) … 400 ms (régimen estable) |
| **Total en régimen** | **~300-650 ms** detrás del peso |

Notas:
- **El arranque es casi inmediato:** al empezar a verter, la mediana se siembra y alfa vale 0.75. El retardo grande aparece en los cambios suaves a mitad del vertido.
- **La pendiente se recorta a 11.5 g/s.** Por eso al poner una taza el panel marcaba 11.1-11.3 g/s mientras la Dot decía 47-99. No afecta a espresso (1-4 g/s) ni a pour-over normal.
- **Puente:** la trama a la máquina sale antes de que `loop()` procese esa muestra, así que el caudal enviado lleva **una muestra de retraso (100 ms)** respecto al peso de la misma trama.
- **El caudal nativo de la Dot (`Dot:`) no es fiable:** marcó 47.7 y 99.9 con el peso quieto, así que no sirve como alternativa.

## 3. Hallazgos y optimizaciones propuestas

| # | Cambio | Ganancia | Riesgo/esfuerzo |
|---|---|---|---|
| 1 | Primer reintento del intervalo BLE a los ~1.5 s de conectar (hoy espera 8 s) y siguientes cada 3 s | Los primeros ~8 s pasan de 60 ms a 15 ms (−45 ms en el peor caso justo tras encender) | Bajo, ~5 líneas |
| 2 | Caudal hacia la máquina con la muestra actual: calcular el caudal en la tarea BLE o pasar el envío al final de `processWeightSample` | −100 ms en el caudal que ve la Lita | Bajo-medio |
| 3 | Perfil de caudal "rápido" para el puente: ventana ~250 ms, sin mediana en régimen y alfa mínimo 0.35 | −150-300 ms en el caudal de la máquina; algo más de ruido (la Lita suaviza por su cuenta) | Medio; ajustar con vertidos reales |
| 4 | I2C de la OLED a 1 MHz (hoy 800 kHz nominal, ~600 kHz efectivos: 2 ms por página) | Volcado completo de 16 → ~11 ms | Bajo; probar estabilidad del SSD1309 |
| 5 | Volcado por columnas: enviar solo el rango de columnas que cambió dentro de cada página | Cambios de peso de 10-14 → ~4-7 ms | Medio, ~30 líneas |
| 6 | Pedir a la máquina 7.5-15 ms (hoy 15-30 ms) | −7 ms de media | Medio: comparte radio con el enlace de 15 ms de la Dot; posibles choques de agenda |
| 7 | Sin cambios: la tara es solo por software (la tara física de la Dot activa su zero-lock y retrasa el peso) | — | Ya hecho |

**Recomendación:** aplicar la 1 y la 2 (seguras y baratas) y la 4. La 3 conviene ajustarla probando con la Lita. La 5 y la 6 solo valen la pena si después se sigue notando lag. El tramo que más pesa (el filtro interno de la Dot) no se puede tocar desde el panel.

## 4. Aplicado (14-11-2026)
- **1. Reintento del intervalo:** primera comprobación a los 1.5 s y después cada 3 s. Medido al reconectar: la suscripción terminó en t=2.7 s y el enlace ya iba a 15 ms en t=4.1 s. Antes los 60 ms duraban unos 8 s.
- **2. Caudal del puente:** una segunda instancia de `FlowEngine` en la tarea `bookooTx` procesa cada muestra en cuanto llega y se reinicia cuando cambia la tara. Ya no se usa el caudal de `loop()`, que iba una muestra por detrás. Sin la Lita no se ha podido comprobar en uso real.
- **4. I2C a 1 MHz:** el volcado parcial típico pasa de 4.2 a 3.7 ms (−12 %), menos de lo calculado, probablemente porque las resistencias pull-up limitan la velocidad real del bus. El volcado completo no se ha medido otra vez; por proporción, ~14 ms. La pantalla funciona sin errores.

## 5. Pendiente de medir con la Lita
- Intervalo real que acepta la máquina (`[BOOKOO] Enlace con la maquina: X ms`).
- Comprobar que la Lita usa el peso y el caudal (registros Modbus 1415/1423) y cuánto se pasa del peso objetivo al parar.
