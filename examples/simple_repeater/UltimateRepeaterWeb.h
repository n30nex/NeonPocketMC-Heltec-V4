#pragma once

#ifdef NEONPOCKET_ULTIMATE_REPEATER_WEB

#include <Arduino.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_system.h>

#include "MyMesh.h"

#ifndef NEONPOCKET_REPEATER_WEB_DEVICE
#define NEONPOCKET_REPEATER_WEB_DEVICE "HELTEC"
#endif
class UltimateRepeaterWeb {
  static constexpr const char* kUser = "neonpocket";
  WebServer server{80};
  Preferences prefs;
  MyMesh* mesh = nullptr;
  String access_key;
  String ap_ssid;
  bool ap_mode = true;
  bool reboot_pending = false;
  uint32_t reboot_at = 0;

  static const char kIndex[] PROGMEM;

  static String jsonQuote(const String& value) {
    String out;
    out.reserve(value.length() + 8);
    out += '"';
    for (size_t i = 0; i < value.length(); ++i) {
      const char c = value[i];
      if (c == '"' || c == '\\') {
        out += '\\';
        out += c;
      } else if (c == '\n') {
        out += "\\n";
      } else if (c == '\r') {
        out += "\\r";
      } else if (static_cast<uint8_t>(c) >= 0x20) {
        out += c;
      }
    }
    out += '"';
    return out;
  }

  String runCommand(const String& input) {
    if (!mesh || input.length() == 0 || input.length() > 95) return "Invalid command";
    char command[96];
    char reply[512];
    input.toCharArray(command, sizeof(command));
    reply[0] = 0;
    mesh->handleCommand(0, command, reply);
    return String(reply);
  }

  bool authenticate() {
    if (server.authenticate(kUser, access_key.c_str())) return true;
    server.requestAuthentication(BASIC_AUTH, "NeonPocket Ultimate Repeater");
    return false;
  }

  static bool isAllowedCommand(String command) {
    command.trim();
    static const char* exact[] = {
      "advert", "advert.zerohop", "discover.neighbors", "neighbors",
      "stats-core", "stats-radio", "stats-packets", "clear stats",
      "gps advert", "gps advert none", "gps advert prefs",
      "set repeat on", "set repeat off"
    };
    for (const char* value : exact) if (command == value) return true;

    static const char* reads[] = {
      "get name", "get role", "get public.key", "get radio", "get freq",
      "get tx", "get lat", "get lon", "get repeat", "get path.hash.mode",
      "get advert.interval", "get flood.advert.interval", "get flood.max.advert",
      "get radio.rxgain", "get dutycycle"
    };
    for (const char* value : reads) if (command == value) return true;

    static const char* prefixes[] = {
      "set name ", "set freq ", "set bw ", "set sf ", "set cr ", "set tx ",
      "set lat ", "set lon ", "set path.hash.mode ", "set advert.interval ",
      "set flood.advert.interval ", "set flood.max.advert ",
      "set radio.rxgain ", "set dutycycle "
    };
    for (const char* value : prefixes) if (command.startsWith(value)) return true;
    return false;
  }

  void loadAccessKey() {
    access_key = prefs.getString("web-key", "");
    if (access_key.length() == 12) return;
    static const char alphabet[] = "23456789abcdefghjkmnpqrstuvwxyzABCDEFGHJKMNPQRSTUVWXYZ";
    access_key = "";
    for (uint8_t i = 0; i < 12; ++i) {
      access_key += alphabet[esp_random() % (sizeof(alphabet) - 1)];
    }
    prefs.putString("web-key", access_key);
  }

  void startAccessPoint() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP);
    char suffix[7];
    snprintf(suffix, sizeof(suffix), "%06lX", static_cast<unsigned long>(ESP.getEfuseMac() & 0xFFFFFFULL));
    ap_ssid = String("NeonPocket-") + suffix;
    WiFi.softAP(ap_ssid.c_str(), access_key.c_str());
    ap_mode = true;
  }

  void startNetwork() {
    const bool prefer_sta = prefs.getBool("sta-mode", false);
    const String ssid = prefs.getString("sta-ssid", "");
    const String password = prefs.getString("sta-pass", "");
    if (prefer_sta && ssid.length()) {
      WiFi.mode(WIFI_STA);
      WiFi.setSleep(false);
      WiFi.begin(ssid.c_str(), password.c_str());
      const uint32_t started = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - started < 10000U) {
        delay(50);
      }
      if (WiFi.status() == WL_CONNECTED) {
        ap_mode = false;
        return;
      }
    }
    startAccessPoint();
  }

  void sendStatus() {
    if (!authenticate()) return;
    String core = runCommand("stats-core");
    String radio = runCommand("stats-radio");
    String packets = runCommand("stats-packets");
    if (!core.startsWith("{")) core = "{}";
    if (!radio.startsWith("{")) radio = "{}";
    if (!packets.startsWith("{")) packets = "{}";
    const String neighbors = runCommand("neighbors");
    const String radio_config = runCommand("get radio");
    const NodePrefs* node = mesh->getNodePrefs();

    String body;
    body.reserve(1400);
    body += "{\"device\":";
    body += jsonQuote(NEONPOCKET_REPEATER_WEB_DEVICE);
    body += ",\"role\":\"repeater\",\"version\":";
    body += jsonQuote(NEONPOCKET_ULTIMATE_VERSION);
    body += ",\"mode\":\"";
    body += ap_mode ? "AP" : "LAN";
    body += "\",\"ssid\":";
    body += jsonQuote(ap_mode ? ap_ssid : WiFi.SSID());
    body += ",\"ip\":";
    body += jsonQuote(ipText());
    body += ",\"wifi_rssi\":";
    body += ap_mode ? "0" : String(WiFi.RSSI());
    body += ",\"free_heap\":";
    body += String(ESP.getFreeHeap());
    body += ",\"name\":";
    body += jsonQuote(node ? String(node->node_name) : String(""));
    body += ",\"lat\":";
    body += node ? String(node->node_lat, 6) : "0";
    body += ",\"lon\":";
    body += node ? String(node->node_lon, 6) : "0";
    body += ",\"path_hash_mode\":";
    body += node ? String(node->path_hash_mode) : "0";
    body += ",\"forwarding\":";
    body += (node && !node->disable_fwd) ? "true" : "false";
    body += ",\"core\":" + core;
    body += ",\"radio\":" + radio;
    body += ",\"packets\":" + packets;
    body += ",\"radio_config\":" + jsonQuote(radio_config);
    body += ",\"neighbors\":" + jsonQuote(neighbors);
    body += "}";
    server.send(200, "application/json", body);
  }

  void sendCommand() {
    if (!authenticate()) return;
    const String command = server.arg("plain");
    if (!isAllowedCommand(command)) {
      server.send(403, "application/json", "{\"ok\":false,\"reply\":\"Command not allowed\"}");
      return;
    }
    const String reply = runCommand(command);
    String body = String("{\"ok\":true,\"reply\":") + jsonQuote(reply) + "}";
    server.send(200, "application/json", body);
  }

  void saveNetwork() {
    if (!authenticate()) return;
    if (server.arg("mode") == "ap") {
      prefs.putBool("sta-mode", false);
      prefs.remove("sta-ssid");
      prefs.remove("sta-pass");
    } else {
      const String ssid = server.arg("ssid");
      const String password = server.arg("password");
      if (!ssid.length() || ssid.length() > 32 || password.length() > 63) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"Invalid Wi-Fi credentials\"}");
        return;
      }
      if (!prefs.putString("sta-ssid", ssid) || !prefs.putString("sta-pass", password) ||
          !prefs.putBool("sta-mode", true)) {
        server.send(500, "application/json", "{\"ok\":false,\"error\":\"Could not save settings\"}");
        return;
      }
    }
    server.send(202, "application/json", "{\"ok\":true,\"restarting\":true}");
    reboot_pending = true;
    reboot_at = millis() + 750U;
  }

  void registerRoutes() {
    server.on("/", HTTP_GET, [this]() {
      if (!authenticate()) return;
      server.send_P(200, "text/html", kIndex);
    });
    server.on("/api/status", HTTP_GET, [this]() { sendStatus(); });
    server.on("/api/command", HTTP_POST, [this]() { sendCommand(); });
    server.on("/api/network", HTTP_POST, [this]() { saveNetwork(); });
    server.onNotFound([this]() {
      if (ap_mode) {
        server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString(), true);
        server.send(302, "text/plain", "");
      } else {
        server.send(404, "text/plain", "Not found");
      }
    });
  }

public:
  bool begin(MyMesh& instance) {
    mesh = &instance;
    if (!prefs.begin("np-rpt-web", false)) return false;
    loadAccessKey();
    startNetwork();
    registerRoutes();
    server.begin();
    Serial.printf("Ultimate Repeater Web: %s http://%s user=%s key=%s\n",
                  ap_mode ? ap_ssid.c_str() : WiFi.SSID().c_str(),
                  ipText().c_str(), kUser, access_key.c_str());
    return true;
  }

  void loop() {
    server.handleClient();
    if (reboot_pending && static_cast<int32_t>(millis() - reboot_at) >= 0) ESP.restart();
  }

  bool isAccessPoint() const { return ap_mode; }
  const String& accessKey() const { return access_key; }
  const String& accessPointSsid() const { return ap_ssid; }
  String ipText() const { return (ap_mode ? WiFi.softAPIP() : WiFi.localIP()).toString(); }
  String networkName() const { return ap_mode ? ap_ssid : WiFi.SSID(); }
};

const char UltimateRepeaterWeb::kIndex[] PROGMEM = R"HTML(
<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>NeonPocket Ultimate Repeater</title><style>
:root{color-scheme:dark;--bg:#060816;--panel:#0d1328;--line:#1f72ff;--cyan:#25d9ff;--lime:#8cff58;--hot:#ff4d9d;--text:#eff6ff;--muted:#8fa8c8}*{box-sizing:border-box}body{margin:0;background:radial-gradient(circle at 12% 0,#15264a 0,transparent 36%),var(--bg);color:var(--text);font:15px system-ui,sans-serif}main{width:min(1080px,94vw);margin:auto;padding:24px 0 60px}header{display:flex;align-items:end;justify-content:space-between;gap:16px;margin-bottom:18px}h1{font-size:clamp(28px,6vw,58px);line-height:.9;margin:0;letter-spacing:-.05em}.neon{color:var(--cyan);text-shadow:0 0 22px #25d9ff88}.pill{padding:7px 11px;border:1px solid #2e4d80;border-radius:99px;color:var(--lime);background:#0b1730}.grid{display:grid;grid-template-columns:repeat(12,1fr);gap:12px}.card{grid-column:span 4;background:linear-gradient(145deg,#111a35dd,#090f22ee);border:1px solid #233d68;border-radius:16px;padding:16px;box-shadow:0 14px 50px #0008}.wide{grid-column:span 8}.full{grid-column:1/-1}.label{font-size:11px;color:var(--cyan);letter-spacing:.14em;text-transform:uppercase}.value{font-size:25px;font-weight:750;margin-top:4px}.muted{color:var(--muted)}pre{white-space:pre-wrap;margin:8px 0 0;color:#bfe7ff;font:13px ui-monospace,monospace}canvas{width:100%;height:160px;border-radius:12px;background:#080d1b;margin-top:12px}form{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:10px;margin-top:10px}input,select,button{width:100%;border:1px solid #29466f;border-radius:10px;background:#080f22;color:var(--text);padding:11px 12px;font:inherit}button{cursor:pointer;background:linear-gradient(120deg,#1251d5,#0b94b4);font-weight:750}button.alt{background:#111a31}button.hot{background:linear-gradient(120deg,#a51b66,#f24b75)}.span2{grid-column:span 2}.toast{position:fixed;right:18px;bottom:18px;max-width:390px;padding:13px 16px;background:#102344;border:1px solid #2f75be;border-radius:12px;opacity:0;transform:translateY(12px);transition:.2s;pointer-events:none}.toast.show{opacity:1;transform:none}@media(max-width:760px){.card,.wide{grid-column:1/-1}header{align-items:start;flex-direction:column}form{grid-template-columns:1fr}.span2{grid-column:auto}}
</style></head><body><main><header><div><div class="label">NeonPocketMC</div><h1>Ultimate<br><span class="neon">Repeater</span></h1></div><div id="link" class="pill">CONNECTING</div></header>
<section class="grid"><article class="card"><div class="label">Node</div><div id="node" class="value">--</div><div id="role" class="muted">repeater</div></article><article class="card"><div class="label">Radio</div><div id="signal" class="value">--</div><div id="radioCfg" class="muted">--</div></article><article class="card"><div class="label">Traffic</div><div id="traffic" class="value">--</div><div id="air" class="muted">--</div></article>
<article class="card wide"><div class="label">Seven-minute signal trace</div><canvas id="chart" width="700" height="170"></canvas></article><article class="card"><div class="label">System</div><pre id="system">--</pre></article>
<article class="card wide"><div class="label">Recently heard</div><pre id="neighbors">--</pre></article><article class="card"><div class="label">Actions</div><form><button type="button" onclick="cmd('advert')">Flood advert</button><button type="button" class="alt" onclick="cmd('discover.neighbors')">Discover</button><button type="button" class="alt span2" onclick="cmd('clear stats')">Clear counters</button></form></article>
<article class="card full"><div class="label">Repeater setup</div><form id="nodeForm"><input id="name" placeholder="Node name" maxlength="31"><input id="lat" placeholder="Latitude"><input id="lon" placeholder="Longitude"><select id="hash"><option value="0">1-byte path hash</option><option value="1">2-byte path hash</option><option value="2">3-byte path hash</option></select><select id="repeat"><option value="on">Forwarding on</option><option value="off">Forwarding off</option></select><button>Save identity + mesh</button></form>
<form id="radioForm"><input id="freq" placeholder="Frequency MHz"><input id="bw" placeholder="Bandwidth kHz"><input id="sf" placeholder="Spreading factor"><input id="cr" placeholder="Coding rate"><input id="tx" placeholder="TX power dBm"><button>Save radio preset</button></form></article>
<article class="card full"><div class="label">Wi-Fi deployment</div><p class="muted">Join a trusted 2.4 GHz network or return to the private setup AP. The device restarts after saving.</p><form id="wifiForm"><input id="ssid" placeholder="2.4 GHz SSID" maxlength="32"><input id="pass" type="password" placeholder="Wi-Fi password" maxlength="63"><button>Join local network</button><button type="button" class="hot" onclick="setupAp()">Use setup AP</button></form></article></section></main><div id="toast" class="toast"></div>
<script>
const $=id=>document.getElementById(id),samples=[];let latest={};function note(s){const t=$('toast');t.textContent=s;t.classList.add('show');setTimeout(()=>t.classList.remove('show'),2400)}
async function req(path,opt){const r=await fetch(path,opt);let d={};try{d=await r.json()}catch{}if(!r.ok)throw Error(d.error||d.reply||r.statusText);return d}
function graph(){const c=$('chart'),x=c.getContext('2d'),w=c.width,h=c.height;x.clearRect(0,0,w,h);x.strokeStyle='#17335c';for(let y=20;y<h;y+=30){x.beginPath();x.moveTo(0,y);x.lineTo(w,y);x.stroke()}if(samples.length<2)return;x.strokeStyle='#25d9ff';x.lineWidth=3;x.beginPath();samples.forEach((v,i)=>{const px=i*w/59,py=h-12-Math.max(0,Math.min(140,(v+140)*1.25));i?x.lineTo(px,py):x.moveTo(px,py)});x.stroke()}
async function refresh(){try{const d=latest=await req('/api/status');$('link').textContent=`${d.mode} · ${d.ip}`;$('node').textContent=d.name||'Unnamed';$('role').textContent=`${d.device} · ${d.version}`;$('signal').textContent=`${d.radio.last_rssi??'--'} dBm`;$('radioCfg').textContent=d.radio_config.replace(/^>\s*/, '');$('traffic').textContent=`${d.packets.recv??0} RX · ${d.packets.sent??0} TX`;$('air').textContent=`SNR ${d.radio.last_snr??'--'} · noise ${d.radio.noise_floor??'--'}`;$('system').textContent=`Battery ${d.core.battery_mv??0} mV\nUptime ${d.core.uptime_secs??0} s\nQueue ${d.core.queue_len??0}\nHeap ${d.free_heap}\nWi-Fi ${d.wifi_rssi} dBm`;$('neighbors').textContent=(d.neighbors||'-none-').split('\n').map(v=>{const p=v.split(':');return p.length===3?`${p[0]}   ${p[1]}s ago   SNR ${p[2]}`:v}).join('\n');samples.push(Number(d.radio.last_rssi||-140));if(samples.length>60)samples.shift();graph();$('name').value=d.name||'';$('lat').value=d.lat||0;$('lon').value=d.lon||0;$('hash').value=String(d.path_hash_mode||0);$('repeat').value=d.forwarding?'on':'off'}catch(e){$('link').textContent='OFFLINE';note(e.message)}}
async function cmd(c,quiet=false){const d=await req('/api/command',{method:'POST',headers:{'Content-Type':'text/plain'},body:c});if(!quiet)note(d.reply||'OK');return d}
$('nodeForm').onsubmit=async e=>{e.preventDefault();try{await cmd(`set name ${$('name').value}`,true);await cmd(`set lat ${$('lat').value}`,true);await cmd(`set lon ${$('lon').value}`,true);await cmd(`set path.hash.mode ${$('hash').value}`,true);await cmd(`set repeat ${$('repeat').value}`,true);note('Repeater settings saved');refresh()}catch(e){note(e.message)}};
$('radioForm').onsubmit=async e=>{e.preventDefault();try{for(const id of ['freq','bw','sf','cr','tx'])if($(id).value)await cmd(`set ${id} ${$(id).value}`,true);note('Radio settings saved');refresh()}catch(e){note(e.message)}};
$('wifiForm').onsubmit=async e=>{e.preventDefault();try{await req('/api/network',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams({ssid:$('ssid').value,password:$('pass').value})});note('Saved. Reconnecting…')}catch(e){note(e.message)}};
async function setupAp(){if(!confirm('Restart into setup AP mode?'))return;try{await req('/api/network',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams({mode:'ap'})});note('Restarting into setup AP…')}catch(e){note(e.message)}}
refresh();setInterval(refresh,7000);
</script></body></html>
)HTML";

#endif
