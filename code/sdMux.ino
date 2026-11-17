/*
  SD_MUX demo con WiFi - ESP32 WROOM-32D (Arduino IDE, core ESP32 de Espressif)

  - La camara tiene la tarjeta por defecto.
  - Escaneo periodico opcional: toma la tarjeta, cuenta archivos nuevos y la devuelve.
  - Pagina web: boton "Tomar tarjeta", explorador de archivos (ver JPG / descargar),
    boton "Devolver tarjeta" y lista de archivos nuevos.
  - Si CAM_SENSE sube (la camara despierta) o la sesion queda inactiva, la tarjeta
    se devuelve sola.

  Acceso: la IP sale por el monitor serie; tambien http://sdmux.local si tu equipo
  soporta mDNS. Usuario/clave: WEB_USER / WEB_PASS.

  JP1 (mux, lado NO) -> SPI:  CLK=GPIO18  CMD=GPIO23(MOSI)  DAT0=GPIO19(MISO)  DAT3=GPIO5(CS)
  JP2 (control):  GPIO_A=GPIO25  GPIO_B=GPIO26  SEL=GPIO27  CAM_SENSE=GPIO34

  Probar primero con tarjeta FAT32 (<= 32 GB).
*/

#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <vector>
#include <algorithm>

// ---- WiFi y acceso web (NO compartir este archivo con las claves puestas) ----
const char* WIFI_SSID = "udd-recicla";
const char* WIFI_PASS = "R3c1claj3#udd-2019";
const char* WEB_USER  = "admin";
const char* WEB_PASS  = "sdmux2026";      // "" = sin autenticacion

// ---- Pines ----
const int PIN_GPIO_A    = 25;
const int PIN_GPIO_B    = 26;
const int PIN_SEL       = 27;
const int PIN_CAM_SENSE = 34;   // solo entrada

const int PIN_SCK  = 18;
const int PIN_MISO = 19;   // DAT0
const int PIN_MOSI = 23;   // CMD
const int PIN_CS   = 5;    // DAT3

// ---- Opciones ----
const bool  MOSTRAR_INDICE = false;  // imprime cada archivo del escaneo por serie
const char* RUTA_ESCANEO   = "/";    // "/" = toda la tarjeta, "/DCIM" = solo fotos
const bool  DEBUG_CAM      = true;   // imprime la tension de CAM_SENSE periodicamente
const bool  FORZAR_TOMA    = false;  // true = ignora CAM_SENSE (solo pruebas de banco)
const size_t MAX_NUEVOS    = 100;    // maximo de rutas nuevas que se recuerdan

// ---- Tiempos ----
const uint32_t DEBUG_INTERVAL_MS   = 1000;
const uint32_t AUTO_ESCANEO_MS     = 120000;  // 0 = sin escaneo periodico (produccion: 3600000)
const uint32_t RETENER_MS          = 0;       // retencion tras el escaneo periodico (0 = no)
const uint32_t SESION_TIMEOUT_MS   = 120000;  // devuelve la tarjeta tras este tiempo sin actividad web
const uint32_t CAM_OFF_CONFIRM_MS  = 3000;    // escaneo periodico: camara apagada este tiempo
const uint32_t WEB_CONFIRM_MS      = 1000;    // toma desde la web: camara apagada este tiempo
const uint32_t POWER_UP_MS         = 30;
const uint32_t DISCHARGE_MS        = 200;
const uint32_t SPI_HZ              = 4000000; // probar 10-20 MHz mas adelante
const int      CAM_UMBRAL_MV       = 1500;

// ---- Estado ----
enum Dueno { DUENO_CAMARA, DUENO_ESP };
Dueno dueno = DUENO_CAMARA;
uint32_t ultimaActividad = 0;
String ultimoMotivo = "";

SPIClass spiSD(VSPI);
WebServer server(80);

std::vector<uint32_t> vistos;        // hashes de rutas conocidas (ordenado)
std::vector<uint32_t> pendientes;    // hashes nuevos del escaneo en curso
std::vector<String>   ultimosNuevos; // rutas nuevas pendientes de revisar
bool     primeraPasada = true;
bool     hayEscaneo = false;
uint32_t ultimoEscaneo = 0;

// ---------- Utilidades ----------
uint32_t hashRuta(const char* s) {
  uint32_t h = 2166136261u;
  while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
  return h;
}

String jsonEsc(const String &s) {
  String o;
  o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((uint8_t)c < 0x20) { o += ' '; }
    else o += c;
  }
  return o;
}

bool rutaValida(const String &r) {
  return r.length() > 0 && r[0] == '/' && r.indexOf("..") < 0;
}

const char* mimeDe(String r) {
  r.toLowerCase();
  if (r.endsWith(".jpg") || r.endsWith(".jpeg")) return "image/jpeg";
  if (r.endsWith(".png"))  return "image/png";
  if (r.endsWith(".gif"))  return "image/gif";
  if (r.endsWith(".webp")) return "image/webp";
  if (r.endsWith(".mp4"))  return "video/mp4";
  if (r.endsWith(".txt") || r.endsWith(".csv")) return "text/plain";
  return "application/octet-stream";
}

inline bool camaraEncendida() {
  if (FORZAR_TOMA) return false;
  return analogReadMilliVolts(PIN_CAM_SENSE) > CAM_UMBRAL_MV;
}

bool camaraApagadaPor(uint32_t ms) {
  uint32_t t0 = millis();
  while (millis() - t0 < ms) {
    if (camaraEncendida()) return false;
    delay(20);
  }
  return true;
}

// ---------- Control del mux y la tarjeta ----------
void devolverACamara() {
  SD.end();
  spiSD.end();
  pinMode(PIN_SCK,  INPUT);
  pinMode(PIN_MISO, INPUT);
  pinMode(PIN_MOSI, INPUT);
  pinMode(PIN_CS,   INPUT);

  digitalWrite(PIN_GPIO_B, HIGH);   // apaga alimentacion del ESP
  delay(DISCHARGE_MS);              // deja descargar SD_VDD
  digitalWrite(PIN_SEL, LOW);       // mux hacia la camara
  pinMode(PIN_GPIO_A, INPUT);       // suelta: R2 lo baja y la camara recupera la tarjeta
  Serial.println("[MUX] Tarjeta devuelta a la camara");
}

bool tomarTarjeta() {
  pinMode(PIN_GPIO_A, OUTPUT);
  digitalWrite(PIN_GPIO_A, HIGH);   // corta camino camara -> tarjeta
  delay(10);
  if (camaraEncendida()) {
    pinMode(PIN_GPIO_A, INPUT);
    return false;
  }
  digitalWrite(PIN_SEL, HIGH);      // mux hacia el ESP
  delay(5);
  digitalWrite(PIN_GPIO_B, LOW);    // energiza la tarjeta desde 3V3
  delay(POWER_UP_MS);

  spiSD.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);
  for (int i = 0; i < 3; i++) {
    if (SD.begin(PIN_CS, spiSD, SPI_HZ)) {
      Serial.printf("[SD] tipo=%d tamano=%llu MB\n",
                    SD.cardType(), SD.cardSize() / (1024ULL * 1024ULL));
      return true;
    }
    delay(50);
  }
  Serial.println("[SD] No se pudo montar la tarjeta");
  return false;
}

// Devuelve la tarjeta a la camara si la sesion web la tenia
void liberar(const char* motivo) {
  if (dueno != DUENO_ESP) return;
  devolverACamara();
  dueno = DUENO_CAMARA;
  ultimoMotivo = motivo;
  Serial.printf("[WEB] Tarjeta liberada: %s\n", motivo);
}

bool tomarParaWeb(String &msg) {
  if (dueno == DUENO_ESP) { ultimaActividad = millis(); return true; }
  if (camaraEncendida()) { msg = "La camara esta encendida (CAM_SENSE alto)"; return false; }
  if (!camaraApagadaPor(WEB_CONFIRM_MS)) { msg = "La camara esta activa"; return false; }
  if (!tomarTarjeta()) {
    devolverACamara();
    msg = "No se pudo montar la tarjeta (o la camara se encendio)";
    return false;
  }
  dueno = DUENO_ESP;
  ultimaActividad = millis();
  ultimoMotivo = "";
  Serial.println("[WEB] Tarjeta tomada por solicitud web");
  return true;
}

// Verifica que la sesion siga valida; si la camara despierta, libera
bool tarjetaDisponible() {
  if (dueno != DUENO_ESP) return false;
  if (camaraEncendida()) { liberar("camara encendida"); return false; }
  return true;
}

// ---------- Escaneo periodico (solo conteo de nuevos) ----------
bool escanear(const String &ruta, int profundidad, int &nuevos) {
  File dir = SD.open(ruta);
  if (!dir || !dir.isDirectory()) return true;

  File f = dir.openNextFile();
  while (f) {
    if (camaraEncendida()) { f.close(); dir.close(); return false; }

    String nombre = String(f.path());
    if (f.isDirectory()) {
      if (MOSTRAR_INDICE) Serial.printf("[DIR ] %s\n", nombre.c_str());
      if (profundidad < 3) {
        if (!escanear(nombre, profundidad + 1, nuevos)) { f.close(); dir.close(); return false; }
      }
    } else {
      uint32_t h = hashRuta(nombre.c_str());
      bool esNuevo = !std::binary_search(vistos.begin(), vistos.end(), h);
      if (MOSTRAR_INDICE) Serial.printf("[ARCH] %s (%u bytes)\n", nombre.c_str(), (unsigned)f.size());
      if (esNuevo) {
        pendientes.push_back(h);
        if (!primeraPasada) {
          nuevos++;
          if (ultimosNuevos.size() < MAX_NUEVOS) ultimosNuevos.push_back(nombre);
          Serial.printf("[NUEVO] %s\n", nombre.c_str());
        }
      }
    }
    f.close();
    f = dir.openNextFile();
  }
  dir.close();
  return true;
}

void revisarTarjeta() {
  Serial.println("\n[CICLO] Inicio");

  if (camaraEncendida()) {
    Serial.println("[CICLO] Camara encendida, se omite");
    return;
  }
  if (!camaraApagadaPor(CAM_OFF_CONFIRM_MS)) {
    Serial.println("[CICLO] Camara activa, se omite");
    return;
  }

  if (tomarTarjeta()) {
    int nuevos = 0;
    pendientes.clear();
    bool completo = escanear(RUTA_ESCANEO, 0, nuevos);
    if (!completo) {
      pendientes.clear();
      Serial.println("[CICLO] Abortado: la camara se encendio");
    } else {
      vistos.insert(vistos.end(), pendientes.begin(), pendientes.end());
      std::sort(vistos.begin(), vistos.end());
      pendientes.clear();
      hayEscaneo = true;
      ultimoEscaneo = millis();
      if (primeraPasada) {
        Serial.printf("[CICLO] Base inicial: %u archivos\n", (unsigned)vistos.size());
        primeraPasada = false;
      } else {
        Serial.printf("[CICLO] Archivos nuevos: %d\n", nuevos);
      }
    }

    if (completo && RETENER_MS > 0) {
      Serial.printf("[CICLO] Reteniendo la tarjeta %u s\n", (unsigned)(RETENER_MS / 1000));
      uint32_t t0 = millis();
      while (millis() - t0 < RETENER_MS) {
        if (camaraEncendida()) { Serial.println("[CICLO] Camara activa, se libera"); break; }
        delay(50);
      }
    }
  }
  devolverACamara();
}

// ---------- Pagina web ----------
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

// ---------- Manejadores HTTP ----------
bool autorizado() {
  if (strlen(WEB_PASS) == 0) return true;
  if (server.authenticate(WEB_USER, WEB_PASS)) return true;
  server.requestAuthentication();
  return false;
}

void hRaiz() {
  if (!autorizado()) return;
  server.send_P(200, "text/html; charset=utf-8", PAGINA);
}

void hEstado() {
  if (!autorizado()) return;
  int mv = analogReadMilliVolts(PIN_CAM_SENSE);
  long hace = hayEscaneo ? (long)((millis() - ultimoEscaneo) / 1000) : -1L;
  long expira = 0;
  if (dueno == DUENO_ESP) {
    uint32_t t = millis() - ultimaActividad;
    expira = (t >= SESION_TIMEOUT_MS) ? 0 : (long)((SESION_TIMEOUT_MS - t) / 1000);
  }
  String j = "{";
  j += "\"dueno\":\"" + String(dueno == DUENO_ESP ? "esp" : "camara") + "\",";
  j += "\"mv\":" + String(mv) + ",";
  j += "\"camara\":" + String(mv > CAM_UMBRAL_MV ? "true" : "false") + ",";
  j += "\"forzado\":" + String(FORZAR_TOMA ? "true" : "false") + ",";
  j += "\"vistos\":" + String((unsigned)vistos.size()) + ",";
  j += "\"hace\":" + String(hace) + ",";
  j += "\"expira\":" + String(expira) + ",";
  j += "\"motivo\":\"" + jsonEsc(ultimoMotivo) + "\",";
  j += "\"nuevos\":[";
  for (size_t i = 0; i < ultimosNuevos.size(); i++) {
    if (i) j += ',';
    j += "\"" + jsonEsc(ultimosNuevos[i]) + "\"";
  }
  j += "]}";
  server.send(200, "application/json", j);
}

void hTomar() {
  if (!autorizado()) return;
  String msg;
  bool ok = tomarParaWeb(msg);
  server.send(200, "application/json",
              String("{\"ok\":") + (ok ? "true" : "false") + ",\"msg\":\"" + jsonEsc(msg) + "\"}");
}

void hDevolver() {
  if (!autorizado()) return;
  liberar("solicitud web");
  server.send(200, "application/json", "{\"ok\":true}");
}

void hLimpiar() {
  if (!autorizado()) return;
  ultimosNuevos.clear();
  server.send(200, "application/json", "{\"ok\":true}");
}

void hLs() {
  if (!autorizado()) return;
  if (!tarjetaDisponible()) {
    server.send(409, "application/json", "{\"error\":\"La tarjeta no esta con el ESP32\"}");
    return;
  }
  ultimaActividad = millis();

  String ruta = server.hasArg("path") ? server.arg("path") : "/";
  if (!rutaValida(ruta)) {
    server.send(400, "application/json", "{\"error\":\"Ruta invalida\"}");
    return;
  }
  int off = server.arg("off").toInt();
  const int LIM = 100;

  File dir = SD.open(ruta);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    server.send(404, "application/json", "{\"error\":\"No es una carpeta\"}");
    return;
  }

  String j = "{\"items\":[";
  int i = 0, enviados = 0;
  bool mas = false;
  File f = dir.openNextFile();
  while (f) {
    if (i >= off) {
      if (enviados >= LIM) { mas = true; f.close(); break; }
      if (enviados) j += ',';
      j += "{\"n\":\"" + jsonEsc(String(f.name())) + "\",\"d\":" + String(f.isDirectory() ? 1 : 0) +
           ",\"s\":" + String((unsigned long)f.size()) + "}";
      enviados++;
    }
    i++;
    f.close();
    f = dir.openNextFile();
  }
  dir.close();
  j += "],\"mas\":";
  j += mas ? "true" : "false";
  j += "}";
  server.send(200, "application/json", j);
}

void hFile() {
  if (!autorizado()) return;
  if (!tarjetaDisponible()) {
    server.send(409, "text/plain", "La tarjeta no esta con el ESP32");
    return;
  }
  ultimaActividad = millis();

  String ruta = server.arg("path");
  if (!rutaValida(ruta)) { server.send(400, "text/plain", "Ruta invalida"); return; }

  File f = SD.open(ruta, FILE_READ);
  if (!f || f.isDirectory()) {
    if (f) f.close();
    server.send(404, "text/plain", "No encontrado");
    return;
  }

  size_t total = f.size();
  server.setContentLength(total);
  server.sendHeader("Cache-Control", "private, max-age=3600");
  server.send(200, mimeDe(ruta), "");

  WiFiClient cli = server.client();
  static uint8_t buf[4096];
  size_t enviado = 0;
  while (enviado < total && cli.connected()) {
    if (camaraEncendida()) { liberar("camara encendida"); break; }
    size_t n = f.read(buf, sizeof(buf));
    if (n == 0) break;
    if (cli.write(buf, n) != n) break;
    enviado += n;
    ultimaActividad = millis();
  }
  f.close();
}

// ---------- WiFi ----------
void gestionarWifi() {
  static bool eraConectado = false;
  static uint32_t ultimoIntento = 0;
  bool conectado = (WiFi.status() == WL_CONNECTED);

  if (conectado && !eraConectado) {
    Serial.printf("[WIFI] Conectado. IP: %s  (http://sdmux.local)\n", WiFi.localIP().toString().c_str());
    MDNS.end();
    if (MDNS.begin("sdmux")) MDNS.addService("http", "tcp", 80);
  } else if (!conectado && eraConectado) {
    Serial.println("[WIFI] Conexion perdida");
  }
  eraConectado = conectado;

  if (!conectado && millis() - ultimoIntento > 15000) {
    ultimoIntento = millis();
    Serial.println("[WIFI] Conectando...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }
}

// ---------- Setup / loop ----------
void setup() {
  Serial.begin(115200);
  delay(300);

  // Estado seguro: camara con la tarjeta
  pinMode(PIN_GPIO_A, INPUT);        // flotante: R2 lo mantiene bajo (camino camara ON)
  pinMode(PIN_GPIO_B, OUTPUT);
  digitalWrite(PIN_GPIO_B, HIGH);    // Q3 apagado
  pinMode(PIN_SEL, OUTPUT);
  digitalWrite(PIN_SEL, LOW);        // mux hacia la camara

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("sdmux");
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  server.on("/", HTTP_GET, hRaiz);
  server.on("/api/estado", HTTP_GET, hEstado);
  server.on("/api/tomar", HTTP_POST, hTomar);
  server.on("/api/devolver", HTTP_POST, hDevolver);
  server.on("/api/limpiar", HTTP_POST, hLimpiar);
  server.on("/api/ls", HTTP_GET, hLs);
  server.on("/file", HTTP_GET, hFile);
  server.onNotFound([]() { server.send(404, "text/plain", "No encontrado"); });
  server.begin();

  Serial.println("SD_MUX demo listo");
}

void loop() {
  static uint32_t ultimoDbg = 0, ultimoCheck = 0, ultimoVigia = 0;
  uint32_t ahora = millis();

  server.handleClient();
  gestionarWifi();

  if (DEBUG_CAM && ahora - ultimoDbg >= DEBUG_INTERVAL_MS) {
    ultimoDbg = ahora;
    Serial.printf("[DBG] CAM_SENSE mV=%d  tarjeta=%s\n",
                  analogReadMilliVolts(PIN_CAM_SENSE), dueno == DUENO_ESP ? "ESP" : "camara");
  }

  if (dueno == DUENO_ESP) {
    if (ahora - ultimoVigia >= 100) {
      ultimoVigia = ahora;
      if (camaraEncendida()) liberar("camara encendida");
      else if (ahora - ultimaActividad > SESION_TIMEOUT_MS) liberar("inactividad");
    }
  } else if (AUTO_ESCANEO_MS > 0 && (ultimoCheck == 0 || ahora - ultimoCheck >= AUTO_ESCANEO_MS)) {
    ultimoCheck = ahora;
    revisarTarjeta();
  }

  delay(2);
}
