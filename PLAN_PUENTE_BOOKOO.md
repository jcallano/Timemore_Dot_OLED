# Plan: modo puente BooKoo en el panel (para la Wendougee Lita)

Estado: **pendiente** (análisis del 14-11-2026). Referencia: repo `jcallano/BLE-_Scale_Translator_Bridge__Wendougee_E-Bar` (`Bascula_Bridge_ESP32S3.ino`, `docs/PROTOCOLS.md`).

Objetivo: que el panel, además de mostrar la Timemore Dot, se anuncie como **BooKoo Themis Ultra** para que la placa BLE de la Lita lo use como báscula. El panel haría de central hacia la Dot y de periférico hacia la máquina a la vez.

## Viabilidad (medida/estimada)

| Recurso | Hoy | Con puente | Nota |
|---|---|---|---|
| Flash (app 3.3 MB) | 1.275 MB (38 %) | ~1.33-1.36 MB (~41 %) | El puente solo, compilado para la WeAct: 603 KB / 34 KB de RAM; NimBLE ya está dentro |
| RAM interna | 63 KB globales, ~260 KB libres | +5-10 KB | Sobra |
| Conexiones BLE | 1 | 2 (Dot + Lita) | `CONFIG_BT_NIMBLE_MAX_CONNECTIONS=3` ya |
| CPU por muestra | µs | +0.33 ms (medido en el puente) | Despreciable a 10 Hz |

Riesgos que hay que diseñar:
1. **Retardo extra si se envía desde `loop()`.** El volcado de la OLED tarda hasta 16 ms. La trama BooKoo debe enviarse desde la tarea BLE en cuanto se decodifica el peso (peso neto = bruto − `softwareTareOffset`; el caudal puede llevar ≤ 100 ms de antigüedad).
2. **Escaneo:** bajar a 60/15 ms (25 % de la radio) mientras se anuncia o la máquina está conectada (hoy: 85/55 ms, ~65 %).
3. **Energía:** no entrar en reposo con la máquina conectada; anuncio más lento (100-200 ms) cuando no hay nadie conectado; opción de menú para activar o desactivar el puente.
4. **Tara de la máquina:** solo por software (la OLED y la máquina ven lo mismo). No enviar la tara física a la Dot (*zero-lock*).

## Piezas y orden

1. `src/bookoo_protocol.*` (módulo puro + autotest): trama 0x0B de 20 bytes con XOR (peso ×100 con signo, caudal ×100, cronómetro en ms, batería) y lectura de los comandos `03 0A cmd d1 d2 xor`: tara `01`, start `04`, stop `05`, reset `06`, tara+start `07`, suavizado `08`. ~120 líneas.
2. Servidor GATT: servicio `0x0FFE` (notify `0xFF11`, write `0xFF12`) + Device Info `0x180A`; callbacks de conexión, suscripción y escritura. ~150 líneas.
3. Anuncio legacy de 27 bytes: flags `02 01 06` + UUID `03 03 FE 0F` + nombre **`BOOKOO_SC_U XXXXXX`** (18 caracteres; 6 hexadecimales de la MAC), con respuesta al escaneo nombre + UUID. ~70 líneas.
4. Envío de cada muestra desde el callback/tarea BLE, relleno si pasan 250 ms sin muestra y separación mínima de 15 ms. ~60 líneas.
5. Comandos de la máquina → tara por software y cronómetro del panel (que sigue al de la máquina). ~60 líneas.
6. Ajuste del enlace con la máquina (15-30 ms, hasta 3 reintentos; ya existe para la Dot). Política de escaneo y reposo, menú "Puente BooKoo ON/OFF", indicador en OLED y LED. ~100 líneas.
7. Pruebas con la Lita: E-Bar/GeeFlow → *Machine Settings → Connectivity → Smart Scale → ON → Scan* → elegir `BOOKOO_SC_U XXXXXX`. Comprobar peso, caudal (registros Modbus 1415/1423), tara al empezar el shot y parada por peso objetivo.

Total: ~500-600 líneas; complejidad media. El modo Acaia queda fuera (~300 líneas más; la Lita usa BooKoo).

## Diferencias a unificar antes de empezar
- **Lectura del peso de la Dot:** el puente usa int16 en los bytes 8-9 y el panel int32 en los bytes 6-9 (coinciden en el rango útil).
- **Batería:** el puente dice que la respuesta no trae porcentaje (una Dot llena reporta 3); el panel lee valores coherentes (71 % → 64 % en horas). Verificar qué byte es cuál antes de rellenar el byte 13 de la trama BooKoo.
- **Inicio y keep-alive:** el puente envía la secuencia de inicio de la app oficial (copia exacta de de1app) y sondea cada 10 s; el panel no envía la secuencia y sondea cada 25 s con tramas con CRC. Ambos funcionan; elegir uno.
