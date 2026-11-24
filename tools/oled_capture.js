#!/usr/bin/env node
// Convierte en PNG los volcados de pantalla de la OLED (orden serie 's' del firmware).
//
// Uso:  node tools/oled_capture.js <log del monitor serie> <carpeta de salida> [nombre1 nombre2 ...]
//
// Cada bloque "[FB] BEGIN 128x64" ... "[FB] END" del log se convierte en un PNG. Los nombres son
// opcionales (por orden); sin ellos se llaman pantalla_1.png, pantalla_2.png...
// Formato: 8 líneas "[FB] <página> <256 hex>", una por página de 8 filas; bit 0 = fila superior.
// Sin dependencias: solo módulos de Node (zlib, fs). También es un módulo: ver oled_gif.js.
"use strict";
const fs = require("fs");
const path = require("path");
const zlib = require("zlib");

// Colores: marco, píxel apagado y píxel encendido
const ON = [235, 244, 255], OFF = [12, 12, 14], BEZEL = [0, 0, 0];

// Devuelve los framebuffers completos (Uint8Array de 1024 bytes). Una línea "[GIF] DELAY <ms>" antes de
// un bloque fija cuánto se muestra ese fotograma en un GIF (propiedad delayMs).
function parseBlocks(text) {
  const blocks = [];
  let cur = null;
  let pendingDelay = null;
  for (const raw of text.split(/\r?\n/)) {
    const line = raw.trim();
    const d = /^\[GIF\] DELAY (\d+)$/.exec(line);
    if (d) { pendingDelay = Number(d[1]); continue; }
    if (line.startsWith("[FB] BEGIN")) {
      cur = new Uint8Array(1024); cur.pages = 0; cur.delayMs = pendingDelay; pendingDelay = null;
      continue;
    }
    if (!cur) continue;
    if (line === "[FB] END") { if (cur.pages === 8) blocks.push(cur); cur = null; continue; }
    const m = /^\[FB\] (\d) ([0-9A-Fa-f]{256})$/.exec(line);
    if (m) {
      const page = Number(m[1]);
      for (let x = 0; x < 128; x++) cur[page * 128 + x] = parseInt(m[2].substr(x * 2, 2), 16);
      cur.pages++;
    }
  }
  return blocks;
}

// Imagen indexada: 0 = marco, 1 = píxel apagado, 2 = encendido. Cada píxel de la OLED es una celda de
// cell x cell con dot x dot encendidos (se intuye la rejilla del panel).
function toIndexed(fb, { cell = 5, dot = 4, margin = 24 } = {}) {
  const W = 128 * cell + 2 * margin, H = 64 * cell + 2 * margin;
  const img = new Uint8Array(W * H);
  for (let y = 0; y < H; y++) {
    for (let x = 0; x < W; x++) {
      const px = x - margin, py = y - margin;
      let v = 0;
      if (px >= 0 && py >= 0 && px < 128 * cell && py < 64 * cell) {
        const ox = Math.floor(px / cell), oy = Math.floor(py / cell);
        const lit = (fb[(oy >> 3) * 128 + ox] >> (oy & 7)) & 1;
        v = lit && (px % cell) < dot && (py % cell) < dot ? 2 : 1;
      }
      img[y * W + x] = v;
    }
  }
  return { W, H, img };
}

const PALETTE = [BEZEL, OFF, ON];

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();
function crc32(buf) {
  let c = 0xFFFFFFFF;
  for (const b of buf) c = CRC_TABLE[(c ^ b) & 0xFF] ^ (c >>> 8);
  return (c ^ 0xFFFFFFFF) >>> 0;
}
function chunk(type, data) {
  const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
  const td = Buffer.concat([Buffer.from(type, "ascii"), data]);
  const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(td));
  return Buffer.concat([len, td, crc]);
}

function toPng(fb) {
  const { W, H, img } = toIndexed(fb);
  const raw = Buffer.alloc(H * (1 + W * 3));
  for (let y = 0; y < H; y++) {
    const row = y * (1 + W * 3);
    raw[row] = 0; // filtro "none"
    for (let x = 0; x < W; x++) raw.set(PALETTE[img[y * W + x]], row + 1 + x * 3);
  }
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(W, 0); ihdr.writeUInt32BE(H, 4);
  ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0; // RGB 8 bits
  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]),
    chunk("IHDR", ihdr),
    chunk("IDAT", zlib.deflateSync(raw, { level: 9 })),
    chunk("IEND", Buffer.alloc(0)),
  ]);
}

module.exports = { parseBlocks, toIndexed, PALETTE };

if (require.main === module) {
  const [, , logFile, outDir, ...names] = process.argv;
  if (!logFile || !outDir) {
    console.error("Uso: node tools/oled_capture.js <log> <carpeta> [nombres...]");
    process.exit(1);
  }
  const blocks = parseBlocks(fs.readFileSync(logFile, "utf8"));
  if (blocks.length === 0) { console.error("No hay volcados [FB] completos en el log."); process.exit(2); }
  fs.mkdirSync(outDir, { recursive: true });
  blocks.forEach((fb, i) => {
    const name = (names[i] || `pantalla_${i + 1}`).replace(/\.png$/i, "") + ".png";
    fs.writeFileSync(path.join(outDir, name), toPng(fb));
    console.log(name);
  });
}
