#!/usr/bin/env node
// Convierte en un GIF animado una serie de volcados de pantalla de la OLED (orden serie 's').
//
// Uso:  node tools/oled_gif.js <log del monitor serie> <salida.gif> [ms por fotograma, 100 por defecto]
//
// Cada bloque [FB] es un fotograma. Una línea "[GIF] DELAY <ms>" antes de un bloque cambia lo que dura
// ese fotograma (por ejemplo, para dejar quieta la pantalla de resumen). Los fotogramas idénticos al
// anterior se funden (suman su duración) y cada fotograma solo guarda el rectángulo que cambia, así el
// GIF pesa poco. Sin dependencias: codificador GIF89a con LZW propio.
"use strict";
const fs = require("fs");
const { parseBlocks, toIndexed, PALETTE } = require("./oled_capture.js");

// Celdas de 4 px con punto de 3: más ligero que los PNG y se sigue intuyendo la rejilla
const LOOK = { cell: 4, dot: 3, margin: 16 };

// LZW de GIF con códigos de longitud variable (hasta 12 bits)
function lzwEncode(pixels, minCodeSize) {
  const clear = 1 << minCodeSize, eoi = clear + 1;
  const out = [];
  let cur = 0, curBits = 0;
  let codeSize = minCodeSize + 1;
  const emit = (code) => {
    cur |= code << curBits; curBits += codeSize;
    while (curBits >= 8) { out.push(cur & 0xFF); cur >>>= 8; curBits -= 8; }
  };
  let dict = new Map();
  let next = eoi + 1;
  emit(clear);
  let prefix = pixels[0];
  for (let i = 1; i < pixels.length; i++) {
    const k = pixels[i];
    const key = prefix * 4096 + k;
    const hit = dict.get(key);
    if (hit !== undefined) { prefix = hit; continue; }
    emit(prefix);
    if (next < 4096) {
      dict.set(key, next++);
      if (next > (1 << codeSize) && codeSize < 12) codeSize++;   // el decodificador va una entrada por detrás
    } else {
      emit(clear);
      dict = new Map(); next = eoi + 1; codeSize = minCodeSize + 1;
    }
    prefix = k;
  }
  emit(prefix);
  emit(eoi);
  if (curBits > 0) out.push(cur & 0xFF);
  return Buffer.from(out);
}

function subBlocks(data) {
  const parts = [];
  for (let i = 0; i < data.length; i += 255) {
    const n = Math.min(255, data.length - i);
    parts.push(Buffer.from([n]), data.subarray(i, i + n));
  }
  parts.push(Buffer.from([0]));
  return Buffer.concat(parts);
}

function encodeGif(frames, W, H) {
  const parts = [];
  const hdr = Buffer.alloc(13);
  hdr.write("GIF89a", 0, "ascii");
  hdr.writeUInt16LE(W, 6); hdr.writeUInt16LE(H, 8);
  hdr[10] = 0xF1;   // tabla global de 4 colores
  parts.push(hdr);
  const pal = Buffer.alloc(12);
  PALETTE.forEach((c, i) => pal.set(c, i * 3));
  parts.push(pal);
  parts.push(Buffer.from([0x21, 0xFF, 0x0B, ...Buffer.from("NETSCAPE2.0"), 0x03, 0x01, 0x00, 0x00, 0x00])); // bucle infinito

  for (const f of frames) {
    const gce = Buffer.from([0x21, 0xF9, 0x04, 0x04, 0, 0, 0, 0x00]);  // disposición 1: no borrar
    gce.writeUInt16LE(Math.max(2, Math.round(f.delayMs / 10)), 4);
    parts.push(gce);
    const desc = Buffer.alloc(10);
    desc[0] = 0x2C;
    desc.writeUInt16LE(f.x, 1); desc.writeUInt16LE(f.y, 3);
    desc.writeUInt16LE(f.w, 5); desc.writeUInt16LE(f.h, 7);
    desc[9] = 0;
    parts.push(desc);
    parts.push(Buffer.from([2]));   // tamaño mínimo de código LZW (4 colores)
    parts.push(subBlocks(lzwEncode(f.pixels, 2)));
  }
  parts.push(Buffer.from([0x3B]));
  return Buffer.concat(parts);
}

if (require.main === module) {
  const [, , logFile, outFile, defDelayArg] = process.argv;
  if (!logFile || !outFile) {
    console.error("Uso: node tools/oled_gif.js <log> <salida.gif> [ms por fotograma]");
    process.exit(1);
  }
  const defDelay = Number(defDelayArg) || 100;
  const blocks = parseBlocks(fs.readFileSync(logFile, "utf8"));
  if (blocks.length === 0) { console.error("No hay volcados [FB] completos en el log."); process.exit(2); }

  const frames = [];
  let prev = null, W = 0, H = 0;
  for (const fb of blocks) {
    const delay = fb.delayMs != null ? fb.delayMs : defDelay;
    const { W: w, H: h, img } = toIndexed(fb, LOOK);
    W = w; H = h;
    if (!prev) {
      frames.push({ x: 0, y: 0, w: W, h: H, pixels: img, delayMs: delay });
      prev = img;
      continue;
    }
    // Rectángulo que cambia respecto al fotograma anterior
    let x0 = W, y0 = H, x1 = -1, y1 = -1;
    for (let y = 0; y < H; y++) {
      for (let x = 0; x < W; x++) {
        if (img[y * W + x] !== prev[y * W + x]) {
          if (x < x0) x0 = x; if (x > x1) x1 = x;
          if (y < y0) y0 = y; if (y > y1) y1 = y;
        }
      }
    }
    if (x1 < 0) { frames[frames.length - 1].delayMs += delay; continue; }  // igual: alargar el anterior
    const fw = x1 - x0 + 1, fh = y1 - y0 + 1;
    const pixels = new Uint8Array(fw * fh);
    for (let y = 0; y < fh; y++) pixels.set(img.subarray((y0 + y) * W + x0, (y0 + y) * W + x0 + fw), y * fw);
    frames.push({ x: x0, y: y0, w: fw, h: fh, pixels, delayMs: delay });
    prev = img;
  }
  const gif = encodeGif(frames, W, H);
  fs.writeFileSync(outFile, gif);
  const total = frames.reduce((s, f) => s + f.delayMs, 0) / 1000;
  console.log(`${outFile}: ${frames.length} fotogramas, ${total.toFixed(1)} s, ${(gif.length / 1024).toFixed(0)} KB`);
}
