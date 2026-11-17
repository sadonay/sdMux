#pragma once

const char PAGINA[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="es"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>SD_MUX</title>
<style>
body{font-family:system-ui,sans-serif;margin:0;background:#15171a;color:#e8e8e8}
header{padding:12px 16px;background:#1f2328;position:sticky;top:0;z-index:2;border-bottom:1px solid #333}
main{padding:12px 16px;max-width:900px;margin:auto}
h3{margin:18px 0 6px;font-size:15px;color:#9ab}
button{padding:8px 14px;margin:6px 6px 0 0;border:0;border-radius:6px;background:#2f6fed;color:#fff;font-size:14px}
button:disabled{background:#444;color:#888}
.b{display:inline-block;padding:2px 10px;border-radius:12px;background:#444;margin-left:8px;font-size:13px}
.b.esp{background:#1f8f4e}.b.cam{background:#a86a12}
#info{font-size:12px;color:#aab;margin-top:6px}
ul{list-style:none;padding:0;margin:0}
li{padding:5px 0;border-bottom:1px solid #262a30;font-size:14px}
a{color:#7db4ff;text-decoration:none}
small{color:#889;margin-left:8px}
img{max-width:100%;border-radius:6px;margin-top:8px}
</style></head><body>
<header>
  <b>SD_MUX</b><span id="badge" class="b">...</span>
  <div id="info"></div>
  <button id="bTomar" onclick="tomar()">Tomar tarjeta</button>
  <button id="bDev" onclick="devolver()">Devolver tarjeta</button>
</header>
<main>
  <h3>Nuevos desde el último escaneo</h3>
  <div id="nuevos"></div>
  <button onclick="limpiar()">Marcar como vistos</button>
  <h3>Explorador</h3>
  <div id="ruta"></div>
  <ul id="lista"></ul>
  <button id="bMas" onclick="mas()" hidden>Más</button>
  <div id="visor" hidden><img id="img"></div>
</main>
<script>
let ruta='/',off=0,dueno='camara';
const $=id=>document.getElementById(id);
async function api(u,o){const r=await fetch(u,o);let j={};try{j=await r.json()}catch(e){}j._s=r.status;return j}
const esc=s=>s.replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
const url=p=>'/file?path='+encodeURIComponent(p);
const esImg=p=>/\.(jpe?g|png|gif|webp)$/i.test(p);
const padre=p=>{const i=p.lastIndexOf('/');return i<=0?'/':p.slice(0,i)};
const fmt=n=>n>1048576?(n/1048576).toFixed(1)+' MB':n>1024?(n/1024).toFixed(0)+' KB':n+' B';

function limpiarVista(){
  $('lista').innerHTML='';$('ruta').textContent='';$('bMas').hidden=true;
  $('visor').hidden=true;$('img').removeAttribute('src');
}
async function estado(){
  const e=await api('/api/estado'); if(e._s!==200) return;
  const antes=dueno; dueno=e.dueno;
  const b=$('badge');
  b.textContent=dueno==='esp'?'Tarjeta: ESP32':'Tarjeta: cámara';
  b.className='b '+(dueno==='esp'?'esp':'cam');
  $('info').textContent=`CAM_SENSE ${e.mv} mV (${e.camara?'cámara encendida':'cámara apagada'}) · ${e.vistos} archivos conocidos · último escaneo: ${e.hace<0?'nunca':'hace '+e.hace+' s'}`
    +(dueno==='esp'?` · la sesión expira en ${e.expira} s`:'')
    +(e.motivo&&dueno!=='esp'?` · última devolución: ${e.motivo}`:'');
  $('bTomar').disabled=dueno==='esp'||(e.camara&&!e.forzado);
  $('bDev').disabled=dueno!=='esp';
  $('nuevos').innerHTML=e.nuevos.length
    ?e.nuevos.map(p=>`<div><a href="#" data-p="${esc(p)}">${esc(p)}</a></div>`).join('')
    :'<small>Ninguno</small>';
  if(antes==='esp'&&dueno!=='esp') limpiarVista();
}
async function listar(){
  $('ruta').textContent='Ruta: '+ruta;
  const j=await api('/api/ls?path='+encodeURIComponent(ruta)+'&off='+off);
  const ul=$('lista');
  if(j._s!==200){ul.innerHTML='<li>'+esc(j.error||'No disponible')+'</li>';$('bMas').hidden=true;return}
  if(off===0) ul.innerHTML=ruta!=='/'?`<li><a href="#" data-d="${esc(padre(ruta))}">.. (subir)</a></li>`:'';
  ul.insertAdjacentHTML('beforeend',j.items.map(it=>{
    const p=(ruta==='/'?'':ruta)+'/'+it.n;
    return it.d
      ?`<li><a href="#" data-d="${esc(p)}">${esc(it.n)}/</a></li>`
      :`<li><a href="#" data-p="${esc(p)}">${esc(it.n)}</a><small>${fmt(it.s)}</small></li>`;
  }).join(''));
  $('bMas').hidden=!j.mas;
}
function abrir(p){
  if(dueno!=='esp'){alert('Primero toma la tarjeta');return}
  if(esImg(p)){$('visor').hidden=false;$('img').src=url(p);$('visor').scrollIntoView()}
  else window.open(url(p),'_blank');
}
document.addEventListener('click',ev=>{
  const a=ev.target.closest('a[data-d],a[data-p]'); if(!a) return;
  ev.preventDefault();
  if(a.dataset.d!==undefined){ruta=a.dataset.d;off=0;listar();return}
  abrir(a.dataset.p);
});
async function tomar(){
  $('bTomar').disabled=true;
  const j=await api('/api/tomar',{method:'POST'});
  if(!j.ok) alert(j.msg||'No se pudo tomar la tarjeta');
  await estado();
  if(dueno==='esp'){ruta='/';off=0;listar()}
}
async function devolver(){await api('/api/devolver',{method:'POST'});await estado();limpiarVista()}
function mas(){off+=100;listar()}
async function limpiar(){await api('/api/limpiar',{method:'POST'});estado()}
estado();setInterval(estado,2000);
</script></body></html>
)rawliteral";
