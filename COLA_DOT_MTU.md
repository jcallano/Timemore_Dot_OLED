# Retraso creciente del peso: la cola de salida de la Dot (y cómo arreglarlo en este firmware)

Fecha: 2026-11-23. Se encontró y resolvió en el proyecto hermano **Timemore Dot Panel** (panel de 7").
La explicación completa, con todas las mediciones, está en su archivo `docs/DOT_COLA_BLE.md`.
Este documento resume el problema y dice **qué se cambió en este firmware OLED** (aplicado el 2026-11-24).

## Síntoma

Recién conectada, la Dot responde rápido. Con los minutos, el peso en la OLED (y en la máquina, con el
puente BooKoo) llega cada vez más tarde, hasta unos **2–2,5 s** después de poner o quitar el objeto.
La app oficial de Timemore no tiene este problema: su retraso es pequeño y no crece.

## Causa

1. **La Dot envía como máximo una notificación BLE cada 100 ms** (su ritmo de muestreo), sacándola de una
   cola de salida.
2. Además de las tramas de peso, en esa cola entran:
   - las **respuestas a nuestros comandos** (sondeo de batería, tara, timer…);
   - un **aviso de batería que la Dot envía sola cada ~30 s** (`A5 5A 01 05 …`).
3. Con el **MTU por defecto (23 bytes, 20 útiles)** solo cabe **una** trama de peso (17 bytes) por notificación.
   Cada trama extra ocupa el hueco de una de peso, y esa trama de peso ya no sale a tiempo: **el retraso
   crece 100 ms por cada trama extra y nunca se recupera**, hasta llenar la cola (~2,4 s).
4. **La Dot sí junta varias tramas en una notificación cuando caben.** La app de Android negocia un MTU grande
   y paquetes largos; así la Dot vacía la cola en cuanto se forma.

**Este firmware conectaba con `exchangeMTU = false`** (`50_ble_scale.ino`:
`pScaleClient->connect(addr, true, true, false)`) y nunca negociaba el MTU después. Por eso le pasaba lo mismo.

## Lo que NO lo arregla (probado en el panel)

- Cambiar el intervalo de conexión (15 ms o 52,5 ms como la app oficial): la cola crece igual.
- Desconectar y reconectar limpiamente, aunque sea con 8 s de pausa o con otro motivo de desconexión: la cola
  se conserva. Solo la vacía un corte brusco (reinicio del ESP32) o apagar la Dot.
- Desactivar y reactivar las notificaciones: empeora (+0,27 s).
- Mandar menos comandos: solo hace que crezca más despacio. Sin ningún comando, además, **la Dot se apaga
  sola** (~70 s medidos), y su aviso de batería cada 30 s sigue llenando la cola.

## Cambios aplicados en este firmware (2026-11-24)

1. **MTU preferido 247** con `NimBLEDevice::setMTU(DOT_PREFERRED_MTU)` justo después de `NimBLEDevice::init()`
   (`initBleStack()`, `50_ble_scale.ino`). También afecta a la conexión del puente BooKoo con la máquina, sin
   efectos negativos.
2. **Intercambio de MTU 1 s después de suscribirse.** `connect()` sigue con `exchangeMTU = false` para evitar el
   error 0x3E/574 en los primeros eventos de conexión. `finishScaleConnection()` programa el intercambio
   (`pendingMtuExchangeAtMs`) y `loop()` lo hace en el paso 6a (`90_app.ino`): llama a `exchangeMTU()`, comprueba
   el MTU a los 2 s y reintenta hasta 3 veces. No se pide en `onConnect`, porque ahí todavía no hay suscripción y
   es justo el momento que provocaba el error. En NimBLE 2.5.2, `exchangeMTU()` no bloquea.
3. **Lectura de varias tramas por notificación.** Ya existía en `tmproto::parsePayload()`; ahora además:
   - solo `A5 5A 01 01` cuenta como peso (antes valía cualquier opcode con `cmd 0x01`, salvo el ACK `03`);
   - cuenta las tramas del paquete (`frameCount`) y detecta la respuesta a la sonda `02 08` (`sawProbeReply`);
   - el autotest (`x`) incluye un paquete juntado (sonda + batería + peso) y una trama `02 01` que no es peso.
4. **Se mantiene lo que ya funcionaba bien:** tara solo por software, ninguna secuencia de inicio y un único sondeo
   ligero (`02 05`) cada 25 s.
5. **Sondeo forzado a los 50 s.** El sondeo se salta durante un vertido (cronómetro en marcha o peso cambiando).
   Ahora, si pasan 50 s sin enviar ningún comando, se envía igualmente: sin comandos, la Dot se apaga sola a los
   ~70 s (por ejemplo, con el cronómetro olvidado en marcha y la báscula quieta). Con MTU grande esa trama extra
   ya no retrasa el peso.

## Cómo comprobar que quedó bien (monitor serie a 115200)

> Los comandos de medición de `DOT_COLA_BLE.md` (`w`, `l`, `x`, `R`) son del panel de 7". **En esta placa `x`
> lanza el autotest y `r`/`R` olvida la Dot vinculada.** Aquí se usan `q` y la línea `[LINK]`.

- Al conectar debe aparecer `[BLE] MTU con la Dot = 247 …` (o el valor que acepte la Dot, mayor que 23). Si sale
  `AVISO: la Dot sigue con MTU 23`, la corrección no se aplicó y el retraso volverá a crecer.
- Cada 5 s, la línea `[LINK] MTU=247 | tramas juntadas N/M | ultima sonda X ms`. Que N crezca de vez en cuando
  es buena señal: es la Dot vaciando la cola.
- **Sonda de cola, sin tocar la báscula:** escribir `q`. Envía `A5 5A 02 08 00 00 00 00` e imprime
  `[COLA] Sonda 02 08 -> respuesta en X ms`. **Con la corrección: ~100 ms siempre**, aunque se envíen varias `q`
  seguidas o se repita a los 15 minutos. Sin ella crece 100 ms por cada trama extra y no baja.
- Prueba final a mano: poner y quitar un objeto recién conectado y otra vez a los 15 minutos; el retraso
  debe ser igual de pequeño.
