#include "configWebserver.h"
#include <WebServer_WT32_ETH01.h>
#include "atem.h"
#include "espNow.h"
#include "main.h"

#define MULTILINE(...) #__VA_ARGS__

WebServer web(80);

IPAddress asIp(uint32_t i) {
  return IPAddress(i);
}

void handleRoot() {
  String s = MULTILINE(<!DOCTYPE html>
<html>
<head>
  <title>Tally bridge</title>
  <meta name="viewport" content="width=device-width, initial-scale=1.0, user-scalable=yes" />
  <style>
    *{box-sizing:border-box;}
    body{background-color: #101010; color: #e6e5df; margin: 1rem;}
    form{display: flex; flex-direction: column; margin: 2rem 0;}
    input{max-width:8rem; background:#000; border:0; padding:.5rem; margin:0 .5rem 0 0; color:#d0d0d0;}
    button{line-height: 2.5rem; text-align: center; margin: 0 .3rem .3rem 0; width: 2.5rem; background: #333; color: #FFF; border: 0; border-radius: .5rem;}
    button:hover{background:#777;}
    button.set{width:auto; padding: 0 .7rem;line-height:2rem;}
    i{line-height: 2.5rem; width: 2.5rem; text-align: center; display: inline-block; font-style: normal; cursor: pointer; background: #333; border-radius: .5rem; margin: 0 .3rem .3rem 0; vertical-align: middle;}
    i.pvw{background: green;}
    i.pgm{background: red;}
    i:hover{background: #777;}
    i.sel{border: 1px solid orange;}
  </style>
</head>
<body>
);
  s += 
"<button onclick='color(0xFF0000)' style='color:red;'>&#9632;</button>"
"<button onclick='color(0x00FF00)' style='color:green;'>&#9632;</button>"
"<button onclick='color(0x0000FF)' style='color:blue;'>&#9632;</button>"
"<button onclick='color(0xFFFF00)' style='color:yellow;'>&#9632;</button>"
"<button onclick='color(0xFFFFFF)' style='color:white;'>&#9632;</button>"
"<br/>"
"<button onclick='signal(13)'>&leftarrow;</button>"
"<button onclick='signal(14)'>&downarrow;</button>"
"<button onclick='signal(15)'>&uparrow;</button>"
"<button onclick='signal(16)'>&rightarrow;</button>"
"<button onclick='signal(12)'>&times;</button>"
"<button onclick='signal(17)'>F</button>"
"<button onclick='signal(18)'>B</button>"
"<button onclick='signal(19)'>Z</button>"
"<button onclick='signal(20)'>&#9761;</button>"
"<button onclick='signal(21)'>I+</button>"
"<button onclick='signal(22)'>I-</button>"
"<button onclick='signal(23)'>&check;</button>"
"<div class='inputs'>";
  for (int i=1; i<=20; i++) {
    s += "<i>";
    s += i;
    s += "</i>";
    // if (i%10 == 0) s += "<br/>";
  }
  s += R"(
</div>
<button onclick='selNone()'>&empty;</button>
<button onclick='selAll()'>&forall;</button>
<label><input type='checkbox' id='multiple'/>Multiple selection</label>
<br/>
<input type='number' name='camId' placeholder='camId'/>
<button onclick='camId()' class='set'>Set camId</button><br/>
<input type='number' name='brightness' placeholder='brightness'/>
<button onclick='brightness()' class='set'>Set brightness</button>
<br>
<label><input type='radio' name='protocol' value='1' )";
  if (config.protocol == 1) s += "checked='checked'";
  s += "/> ATEM</label>"
"<input type='text' name='atemip' placeholder='ATEM IP' value='";
  s += asIp(config.atemIP).toString();
  s += "'/>"
"<br/>"
"<label><input type='radio' name='protocol' value='2' ";
  if (config.protocol == 2) s += "checked='checked'";
 s += "/> OBS</label>"
"<input type='text' name='obsip' placeholder='OBS IP' value='";
  s += asIp(config.obsIP).toString();
  s += "'/>"
"<br/>"
"port: <input type='text' name='obsport' placeholder='port' value='";
  s += config.obsPort;
  s += "'/>"
"<br/>"
"Group: <input type='text' name='group' size='1' maxlength='1' value='";
  s += config.group;
  s += R"('/>
<br/>
<button onclick='save()' class='set'>Save</button>
<script>
  const all_i = document.querySelectorAll('i');
  function iClick(e) {if(!document.getElementById('multiple').checked) selNone(); e.currentTarget.classList.toggle('sel')};
  all_i.forEach((e) => { e.addEventListener('click', iClick) });
  function selNone(){all_i.forEach((e) => e.classList.remove('sel'))};
  function selAll(){all_i.forEach((e) => e.classList.add('sel'))};
  function inputs(){const ii=[]; document.querySelectorAll('i.sel').forEach((e) => ii.push(e.innerText)); return ii.join();};
  function post(u){var x=new XMLHttpRequest();x.open('post',u);x.send();return x;};
  function inputVal(n){return document.getElementsByName(n)[0].value};
  function protocolVal(){return document.querySelector('input[name=protocol]:checked').value};
  function camId(){post(`/set?camid=${inputVal('camId')}&i=${inputs()}`)};
  function brightness(){post(`/set?brightness=${inputVal('brightness')}&i=${inputs()}`)};
  function color(c){post(`/set?color=${c.toString(16)}&i=${inputs()}`)};
  function signal(n){post(`/set?signal=${n}&i=${inputs()}`)};
  function save(){post(`/set?protocol=${protocolVal()}&atemip=${inputVal('atemip')}&obsip=${inputVal('obsip')}&obsport=${inputVal('obsport')}&group=${inputVal('group')}}`)};
  function tally(){var x=post('/tally');x.onreadystatechange=function(){ if(x.readyState===4){fillTallies(JSON.parse(x.responseText));setTimeout(tally,1000)}} };
  function fillTallies(d){all_i.forEach(function(e,i) {
   if(d.program & (1<<i)){e.classList.add('pgm')}else{e.classList.remove('pgm')};
   if(d.preview & (1<<i)){e.classList.add('pvw')}else{e.classList.remove('pvw')};
  })};
  tally();
</script>
</body>
</html>
)";
  web.send(200, "text/html", s);
}

inline uint64_t bitn(uint8_t n) {
  return (uint64_t)1 << (n-1);
}

uint64_t bitsFromCSV(String s) {
  uint64_t bits = 0;
  int number = 0;
  for (size_t i = 0; i < s.length(); ++i) {
    if (isdigit(s[i])) {
      number = 10*number + s[i] - '0';
    } else if (number > 0) {
      bits |= bitn(number);
      number = 0;
    }
  }
  if (number > 0) bits |= bitn(number);
  return bits;
}

uint32_t parseHexColor(String s) {
  uint32_t color = 0;
  for (int i=0; i < s.length(); i++) {
    unsigned char c = s[i];
    color = color << 4;
    if      (c >= '0' && c <= '9') color += c - '0';
    else if (c >= 'A' && c <= 'F') color += c - 'A' + 10;
    else if (c >= 'a' && c <= 'f') color += c - 'a' + 10;
    // else undefined
  }
  return color;
}

void handleSet() {
  String name;
  IPAddress ip;
  bool configUpdated = false;
  for (int i=0; i<web.args(); i++) {
    name = web.argName(i);
    if (name == "color") {
      uint32_t color = parseHexColor(web.arg(i));
      uint64_t bits = bitsFromCSV(web.arg("i"));
      broadcastColor(color, &bits);
      break;
    } else if (name == "program") {
      uint64_t programBits = bitsFromCSV(web.arg(i));
      uint64_t previewBits = bitsFromCSV(web.arg("preview"));
      broadcastTally(&programBits, &previewBits);
      break;
    } else if (name == "brightness") {
      uint64_t bits = bitsFromCSV(web.arg("i"));
      broadcastBrightness(web.arg(i).toInt(), &bits);
      break;
    } else if (name == "camid") {
      uint64_t bits = bitsFromCSV(web.arg("i"));
      broadcastCamId(web.arg(i).toInt(), &bits);
      break;
    } else if (name == "signal") {
      uint64_t bits = bitsFromCSV(web.arg("i"));
      broadcastSignal(web.arg(i).toInt(), &bits);
      break;
    } else if (name == "protocol") {
      config.protocol = (uint8_t) web.arg(i).toInt();
      if (config.protocol != 1 && config.protocol != 2) return;
      configUpdated = true;
    } else if (name == "atemip") {
      ip.fromString(web.arg(i));
      config.atemIP = (uint32_t) ip;
      if (config.atemIP == 0) return;
      configUpdated = true;
    } else if (name == "obsip") {
      ip.fromString(web.arg(i));
      config.obsIP = (uint32_t) ip;
      if (config.obsIP == 0) return;
      configUpdated = true;
    } else if (name == "obsport") {
      config.obsPort = web.arg(i).toInt();
      if (config.obsPort == 0) return;
      configUpdated = true;
    } else if (name == "group") {
      config.group = web.arg(i)[0];
    }
  }
  web.send(200, "text/plain", "OK");
  delay(10);
  if (configUpdated) {
    writeConfig();
    ESP.restart();
  }
}

void handleTally() {
  web.send(200, "application/json", "{\"program\":"+String(lastProgram)+",\"preview\":"+String(lastPreview)+"}");
}

void handleSeen() {
  String s = "{\"tallies\":[";
  esp_now_tally_info_t *tallies = get_tallies();
  unsigned long now = millis();
  for (int i=1; i<64; i++) {
    if (tallies[i].id == 0) break;
    s += "{\"id\":";
    s += tallies[i].id;
    s += ", \"seen\":";
    s += ((now - tallies[i].last_seen) / 1000);
    s += "},";
  }
  if (s[s.length()-1] == ',') s.remove(s.length()-1, 1); // remove last ,
  s += "]}";
  web.send(200, "application/json", s);
}

void setupWebserver() {
  Serial.print("Starting HTTP server on ");
  Serial.print(ARDUINO_BOARD);
  Serial.print(" with ");
  Serial.println(SHIELD_TYPE);
  Serial.println(WEBSERVER_WT32_ETH01_VERSION);
  // To be called before ETH.begin()
  WT32_ETH01_onEvent();
  // Initialize the Ethernet connection
  ETH.begin(ETH_PHY_ADDR, ETH_PHY_POWER);
  WT32_ETH01_waitForConnect();
  web.on("/tally", handleTally);
  web.on("/set", handleSet);
  web.on("/seen", handleSeen);
  web.on("/", handleRoot);
  web.begin();
}

void webserverLoop() {
  web.handleClient();
}