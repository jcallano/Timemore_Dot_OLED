// Página que el display sirve por WiFi para enviar cafés a Beanconqueror (77_compartir.ino).
// El display da los datos ya en el formato de importación de Beanconqueror (sobre "handoff" v1,
// docs/brew-import-api.md de Beanconqueror); la página los completa con los nombres que usa el usuario en
// la app (método, molino, café: se recuerdan en el navegador), los comprime con gzip (CompressionStream),
// arma el enlace beanconqueror://ADD_BREW o ADD_BREWS en trozos de 400 caracteres y lo abre.
#pragma once

static const char SHARE_PAGE_HTML[] = R"HTML(<!DOCTYPE html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Timemore Dot OLED</title>
<style>
body{font-family:system-ui,sans-serif;margin:0;background:#f3efe9;color:#2b2118}
header{background:#2b2118;color:#fff;padding:14px 16px;font-size:18px;font-weight:600}
main{padding:12px 14px 90px}
.card{background:#fff;border-radius:10px;padding:10px 12px;margin:8px 0;display:flex;gap:10px;align-items:flex-start}
.card input{width:22px;height:22px;margin-top:3px}
.card .t{font-weight:600}.card .s{font-size:13px;color:#6b5a4a;margin-top:2px}
.sent{font-size:12px;color:#3a7d44;font-weight:600}.hl{outline:2px solid #b07a4a}
details{background:#fff;border-radius:10px;padding:10px 12px;margin:8px 0}
label{display:block;font-size:13px;margin-top:8px;color:#6b5a4a}
label input,label select{display:block;width:100%;box-sizing:border-box;font-size:16px;padding:6px;margin-top:3px}
#go{position:fixed;left:12px;right:12px;bottom:14px;font-size:17px;padding:14px;border:0;border-radius:10px;
background:#b07a4a;color:#fff;font-weight:600}#go:disabled{background:#c9b8a8}
#msg{font-size:14px;margin:10px 2px}#msg a{color:#b07a4a;font-weight:600}
.row{display:flex;gap:8px;margin:6px 0}.row button{flex:1;padding:8px;border:1px solid #b07a4a;background:#fff;
border-radius:8px;color:#b07a4a;font-size:14px}
</style></head><body>
<header id="h">Timemore Dot OLED &rarr; Beanconqueror</header>
<main>
<details id="cfg"><summary id="cfgT"></summary>
<label><span id="lM"></span><input id="method" placeholder="V60"></label>
<label><span id="lT"></span><select id="ptype"></select></label>
<label><span id="lG"></span><input id="grinder"></label>
<label><span id="lB"></span><input id="bean"></label>
</details>
<div class="row"><button id="selP"></button><button id="selN"></button></div>
<div id="list"></div><div id="msg"></div>
</main>
<button id="go" disabled></button>
<script>
const TYPES=["V60","ORIGAMI","KALITA_WAVE","CHEMEX","APRIL_BREWER","BLUE_DRIPPER","CAFEC_FLOWER","DECEMBER_DRIPPER",
"DRIPPER","GINA","HARIO_SWITCH","KONO","OREA","TORNADO_DUO","AEROPRESS","FRENCH_PRESS","CUSTOM_PREPARATION"];
const ES=(navigator.language||"").toLowerCase().startsWith("es");
const L=ES?{cfg:"Nombres en tu Beanconqueror",m:"Método de preparación",t:"Tipo de método",g:"Molino (opcional)",
b:"Café (opcional; si no, el primero de tu lista)",p:"Pendientes",n:"Ninguno",go:"Enviar a Beanconqueror",
none:"No hay cafés guardados.",sent:"ENVIADO",load:"Cargando…",open:"Abriendo Beanconqueror…",
fall:"Si no se abre, toca aquí: ",link:"Abrir en Beanconqueror",err:"Error: ",old:"Este navegador no puede comprimir (gzip). Actualízalo.",
tds:"TDS",bev:"bebida",est:"estimada"}
:{cfg:"Names in your Beanconqueror",m:"Preparation method",t:"Method type",g:"Grinder (optional)",
b:"Coffee (optional; otherwise the first in your list)",p:"Pending",n:"None",go:"Send to Beanconqueror",
none:"No saved brews.",sent:"SENT",load:"Loading…",open:"Opening Beanconqueror…",
fall:"If it does not open, tap here: ",link:"Open in Beanconqueror",err:"Error: ",old:"This browser cannot gzip. Please update it.",
tds:"TDS",bev:"beverage",est:"estimated"};
const $=id=>document.getElementById(id);
$("cfgT").textContent=L.cfg;$("lM").textContent=L.m;$("lT").textContent=L.t;$("lG").textContent=L.g;$("lB").textContent=L.b;
$("selP").textContent=L.p;$("selN").textContent=L.n;$("go").textContent=L.go;$("msg").textContent=L.load;
for(const t of TYPES){const o=document.createElement("option");o.value=t;o.textContent=t.replace(/_/g," ");$("ptype").appendChild(o);}
const S={method:"V60",ptype:"V60",grinder:"",bean:""};
for(const k in S){const v=localStorage.getItem("tmdot_"+k);if(v!==null)S[k]=v;$(k).value=S[k];
 $(k).addEventListener("change",()=>localStorage.setItem("tmdot_"+k,$(k).value));}
const pre=parseInt((location.pathname.match(/\/P\/(\d+)/i)||[])[1]||"0");
let brews=[];
function fmtDate(e){if(!e)return"";const d=new Date(e*1000);return d.toLocaleDateString()+" "+d.toLocaleTimeString([], {hour:"2-digit",minute:"2-digit"});}
function mmss(s){s=Math.round(s);return Math.floor(s/60)+":"+String(s%60).padStart(2,"0");}
function upd(){const n=document.querySelectorAll(".card input:checked").length;$("go").disabled=n==0;$("go").textContent=L.go+(n?" ("+n+")":"");}
function render(){const el=$("list");el.innerHTML="";
 if(!brews.length){$("msg").textContent=L.none;return;} $("msg").textContent="";
 for(const b of brews.slice().reverse()){const c=document.createElement("label");c.className="card"+(b.id==pre?" hl":"");
  const i=document.createElement("input");i.type="checkbox";i.value=b.id;i.checked=pre?b.id==pre:!b.sent;i.onchange=upd;
  const d=document.createElement("div");
  d.innerHTML='<div class="t">#'+b.id+" &middot; "+b.dose.toFixed(1)+" g &rarr; "+b.water.toFixed(0)+" g &middot; "+mmss(b.time)+"</div>"+
   '<div class="s">'+(b.date?fmtDate(b.date)+" &middot; ":"")+L.bev+" "+b.bev.toFixed(1)+" g"+(b.bevMeasured?"":" ("+L.est+")")+
   (b.tds>0?" &middot; "+L.tds+" "+b.tds.toFixed(2)+" %":"")+"</div>"+(b.sent?'<div class="sent">&#10003; '+L.sent+"</div>":"");
  c.appendChild(i);c.appendChild(d);el.appendChild(c);}
 upd();}
$("selP").onclick=()=>{document.querySelectorAll(".card input").forEach(i=>{const b=brews.find(x=>x.id==i.value);i.checked=!b.sent;});upd();};
$("selN").onclick=()=>{document.querySelectorAll(".card input").forEach(i=>i.checked=false);upd();};
async function gzip64(txt){
 const s=new Blob([new TextEncoder().encode(txt)]).stream().pipeThrough(new CompressionStream("gzip"));
 const u=new Uint8Array(await new Response(s).arrayBuffer());let bin="";for(let i=0;i<u.length;i++)bin+=String.fromCharCode(u[i]);
 return btoa(bin).replace(/\+/g,"-").replace(/\//g,"_").replace(/=+$/,"");}
async function send(){
 if(typeof CompressionStream==="undefined"){$("msg").textContent=L.old;return;}
 const ids=[...document.querySelectorAll(".card input:checked")].map(i=>i.value);if(!ids.length)return;
 $("go").disabled=true;
 try{
  const r=await fetch("/api/handoff?ids="+ids.join(","));const j=await r.json();
  const m=$("method").value.trim()||"V60",t=$("ptype").value,g=$("grinder").value.trim(),bn=$("bean").value.trim();
  for(const e of j.brews){e.brew.preparationMethod=m;e.brew.preparationType=t;if(g)e.brew.grinderName=g;
   if(bn)e.bean={name:bn};if(!e.brew.date)e.brew.date=new Date().toISOString();}
  const one=j.brews.length==1;const p=await gzip64(JSON.stringify(one?j.brews[0]:{v:1,brews:j.brews}));
  let q="";for(let i=0,k=0;i<p.length;i+=400,k++)q+="&shareBrew"+k+"="+p.slice(i,i+400);
  const link="beanconqueror://"+(one?"ADD_BREW":"ADD_BREWS")+"?len="+p.length+q;
  await fetch("/api/sent?ids="+ids.join(","),{method:"POST"});
  for(const b of brews)if(ids.includes(String(b.id)))b.sent=true;render();
  $("msg").innerHTML=L.open+"<br>"+L.fall+'<a id="bc" href="'+link+'">'+L.link+"</a>";
  location.href=link;
 }catch(x){$("msg").textContent=L.err+x;}
 upd();}
$("go").onclick=send;
fetch("/api/brews").then(r=>r.json()).then(j=>{brews=j.brews;render();}).catch(x=>$("msg").textContent=L.err+x);
</script></body></html>)HTML";
