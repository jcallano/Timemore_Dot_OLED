# Accesorios BLE: formato del anuncio

Los accesorios propios (por ahora, la sonda de pH y temperatura) **no se conectan** al display. Cada uno emite sus lecturas en su anuncio BLE y el display solo escucha. Así:

- un accesorio puede ir a pilas y dormir entre medidas;
- varios displays, o Home Assistant con un decodificador, pueden leer la misma sonda;
- el display no abre conexiones nuevas que compitan con la Dot y el puente BooKoo.

El código de referencia, que comparten el display y los accesorios, está en [`firmware/src/accessory_protocol.h`](../firmware/src/accessory_protocol.h) y [`firmware/src/accessory_protocol.cpp`](../firmware/src/accessory_protocol.cpp). El autotest (`x` por el monitor serie) lo comprueba.

## Anuncio

Va en el campo **Manufacturer Specific Data** (AD type `0xFF`), con 16 bytes de datos (2 del Company ID y 14 de lectura). Todos los números son little-endian.

| Bytes | Campo | Formato |
| :--- | :--- | :--- |
| 0–1 | Company ID | `0xFFFF` (reservado por el Bluetooth SIG para pruebas y uso interno) |
| 2–3 | Firma | `'T' 'D'` |
| 4 | Versión | `1` |
| 5 | Tipo de accesorio | `1` = sonda de pH + PT100 |
| 6 | Secuencia | sube con cada medida nueva |
| 7 | Flags | ver abajo |
| 8–9 | Temperatura | int16, centésimas de °C |
| 10–11 | pH | uint16, milésimas |
| 12–13 | Tensión de la sonda de pH | int16, décimas de mV (diagnóstico y calibración) |
| 14 | Batería del accesorio | % (`0xFF` = desconocida) |
| 15 | Días desde la calibración del pH | `0xFF` = nunca o desconocido |

Flags:

| Bit | Significado |
| :--- | :--- |
| 0 | temperatura válida |
| 1 | pH válido |
| 2 | el pH sale de una calibración |
| 3 | lectura estable (se puede guardar) |
| 4 | toca calibrar |
| 5 | batería baja |

Con los 3 bytes de flags del anuncio y los 18 de este campo (16 de datos, longitud y tipo) quedan 10 bytes para un nombre corto, por ejemplo `TMD-Sonda`.

## Cómo debe emitir el accesorio

- Mientras mide (al pulsar su botón, durante 1–2 minutos), debe emitir **cada 100–200 ms**. El display, con la Dot conectada, escucha 32 ms de cada 160 ms para no restarle radio a la Dot. A ese ritmo recoge varias lecturas por segundo.
- Debe subir la **secuencia** solo cuando la lectura cambia, y marcar **estable** cuando el pH varíe menos de 0,01 en unos 10 s.
- La **calibración** se guarda en el propio accesorio, no en el display.
- Cuando no mide, el accesorio puede dejar de emitir y dormir.

## Qué hace el display con las lecturas

- **Modo Fermento:**
  - La línea superior muestra `PH 3.82 21.4C`. Una `~` al final indica que la lectura aún no es estable.
  - Una lectura de más de 15 s ya no se muestra.
- **Menú → Lotes:** 1 clic añade la lectura actual al lote seleccionado, con fecha si el reloj está en hora.
- **Pruebas sin sonda:** la orden serie `f` simula una lectura.
