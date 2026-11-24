#!/usr/bin/env node
// Monta los renders de generar_renders.py con ffmpeg (debe estar en el PATH):
//   - recorta las 4 vistas fijas a su contenido, con margen, y las deja a 1200 px de ancho;
//   - recorta todos los fotogramas con el MISMO rectángulo (la unión de todos, para que no "bailen")
//     y crea animacion.gif (560 px, 12 fps, 64 colores: ~2,4 MB) y animacion.mp4 (H.264, 30 fps).
// Uso (desde cad/):  node montar_renders.js
"use strict";
const { execFileSync, spawnSync } = require("child_process");
const fs = require("fs");
const path = require("path");

const DIR = path.join(__dirname, "renders");
const ff = (args) => execFileSync("ffmpeg", ["-hide_banner", "-y", ...args], { stdio: ["ignore", "pipe", "pipe"] });
// ffmpeg escribe su información en stderr
const ffLog = (args) => spawnSync("ffmpeg", ["-hide_banner", ...args], { encoding: "utf8", maxBuffer: 64 * 1024 * 1024 }).stderr || "";

// Rectángulos de contenido (fondo blanco: se invierte para que cropdetect lo vea como negro)
function detect(input, extra = []) {
  const out = ffLog([...extra, "-i", input, "-vf", "negate,cropdetect=limit=0.02:round=2:reset=1:skip=0", "-f", "null", "-"]);
  const boxes = [...out.matchAll(/crop=(\d+):(\d+):(\d+):(\d+)/g)].map((m) => m.slice(1).map(Number));
  if (boxes.length === 0) throw new Error("sin contenido en " + input);
  let x0 = 1e9, y0 = 1e9, x1 = 0, y1 = 0;
  for (const [w, h, x, y] of boxes) { x0 = Math.min(x0, x); y0 = Math.min(y0, y); x1 = Math.max(x1, x + w); y1 = Math.max(y1, y + h); }
  return { x0, y0, x1, y1 };
}

// Recorte con margen proporcional, dentro de la imagen y con medidas pares (H.264)
function cropFilter(b, imgW, imgH, marginFrac = 0.06) {
  const m = Math.round(Math.max(b.x1 - b.x0, b.y1 - b.y0) * marginFrac);
  let x = Math.max(0, b.x0 - m), y = Math.max(0, b.y0 - m);
  let w = Math.min(imgW, b.x1 + m) - x, h = Math.min(imgH, b.y1 + m) - y;
  w -= w % 2; h -= h % 2;
  return `crop=${w}:${h}:${x}:${y}`;
}

// 1. Vistas fijas (de renders/raw/ a renders/)
for (const name of ["montado", "explosionado", "interior", "trasera"]) {
  const src = path.join(DIR, "raw", name + ".png");
  if (!fs.existsSync(src)) continue;
  const b = detect(src);
  ff(["-i", src, "-vf", `${cropFilter(b, 1600, 1200)},scale=1200:-2:flags=lanczos`, path.join(DIR, name + ".png")]);
  console.log(name + ".png recortado");
}

// 2. Animación
const frames = path.join(DIR, "frames", "f_%03d.png");
const b = detect(frames, ["-framerate", "24"]);
const crop = cropFilter(b, 1600, 1200, 0.04);
console.log("recorte común de la animación:", crop);
ff(["-framerate", "20", "-i", frames, "-vf",
    `${crop},fps=12,scale=560:-2:flags=lanczos,split[a][b];[a]palettegen=max_colors=64:stats_mode=diff[p];[b][p]paletteuse=dither=bayer:bayer_scale=4:diff_mode=rectangle`,
    "-loop", "0", path.join(DIR, "animacion.gif")]);
ff(["-framerate", "30", "-i", frames, "-vf", `${crop},scale=1200:-2:flags=lanczos,format=yuv420p`,
    "-c:v", "libx264", "-crf", "20", "-preset", "slow", "-movflags", "+faststart", path.join(DIR, "animacion.mp4")]);
for (const f of ["animacion.gif", "animacion.mp4"]) {
  console.log(f, (fs.statSync(path.join(DIR, f)).size / 1024).toFixed(0), "KB");
}
