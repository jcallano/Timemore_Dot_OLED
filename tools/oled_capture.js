#!/usr/bin/env node
// Convierte en PNG los volcados de pantalla de la OLED (orden serie 's' del firmware).
//
// Uso:  node tools/oled_capture.js <log del monitor serie> <carpeta de salida> [nombre1 nombre2 ...]
//
// Cada bloque "[FB] BEGIN 128x64" ... "[FB] END" del log se convierte en un PNG. Los nombres son
// opcionales (por orden); sin ellos se llaman pantalla_1.png, pantalla_2.png...
// Formato: 8 líneas "[FB] <página> <256 hex>", una por página de 8 filas; bit 0 = fila superior.
// Sin dependencias: solo módulos de Node (zlib, fs).
"use strict";
const fs = require("fs");
const path = require("path");
const zlib = require("zlib");

const [, , logFile, outDir, ...names] = process.argv;
if (!logFile || !outDir) {
  console.error("Uso: node tools/oled_capture.js <log> <carpeta> [nombres...]");
  process.exit(1);
}

// Aspecto: cada píxel de la OLED es una celda de 5x5 con 4x4 encendidos (se intuye la rejilla)
const CELL = 5, DOT = 4, MARGIN = 24;
const ON = [235, 244, 255], OFF = [12, 12, 14], BEZEL = [0, 0, 0];

function parseBlocks(text) {
  const blocks = [];
  let cur = null;
  for (const raw of text.split(/\r?\n/)) {
    const line = raw.trim();
    if (line.startsWith("[FB] BEGIN")) { cur = new Uint8Array(1024); cur.pages = 0; continue; }
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
  const W = 128 * CELL + 2 * MARGIN, H = 64 * CELL + 2 * MARGIN;
  const raw = Buffer.alloc(H * (1 + W * 3));
  for (let y = 0; y < H; y++) {
    const row = y * (1 + W * 3);
    raw[row] = 0; // filtro "none"
    for (let x = 0; x < W; x++) {
      let col = BEZEL;
      const px = x - MARGIN, py = y - MARGIN;
      if (px >= 0 && py >= 0 && px < 128 * CELL && py < 64 * CELL) {
        const ox = Math.floor(px / CELL), oy = Math.floor(py / CELL);
        const lit = (fb[(oy >> 3) * 128 + ox] >> (oy & 7)) & 1;
        const inDot = (px % CELL) < DOT && (py % CELL) < DOT;
        col = lit && inDot ? ON : OFF;
      }
      raw.set(col, row + 1 + x * 3);
    }
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

const blocks = parseBlocks(fs.readFileSync(logFile, "utf8"));
if (blocks.length === 0) { console.error("No hay volcados [FB] completos en el log."); process.exit(2); }
fs.mkdirSync(outDir, { recursive: true });
blocks.forEach((fb, i) => {
  const name = (names[i] || `pantalla_${i + 1}`).replace(/\.png$/i, "") + ".png";
  fs.writeFileSync(path.join(outDir, name), toPng(fb));
  console.log(`${name}`);
});
