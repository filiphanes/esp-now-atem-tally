#include "configWebserver.h"
#include <WebServer_WT32_ETH01.h>
#include "atem.h"
#include "espNow.h"
#include "main.h"

WebServer web(80);

String getATEMInputs() {
  String s = "<div class=inputs>";
  for (int i=1; i<=40; i++) {
    if (AtemSwitcher.getProgramTally(i)) {
      s += "<i class=pgm>" + String(i) + "</i>";
    } else
    if (AtemSwitcher.getPreviewTally(i)) {
      s += "<i class=pvw>" + String(i) + "</i>";
    } else {
      s += "<i>" + String(i) + "</i>";
    }
    if (i%10 == 0) s += "<br/>";
  }
  return s + "</div>";
}

String getTalliesHTML() {
  String s = "";
  esp_now_tally_info_t *tallies = get_tallies();
  unsigned long now = millis();
  for (int i=1; i<64; i++) {
    if (tallies[i].id == 0) break;
    s += "<p>" + String(tallies[i].id) + ", " + String((now - tallies[i].last_seen) / 1000) + "s ago</p>";
  }
  return s;
}

void handleRoot() {
  web.send(200, "text/html", "<html>"
    "<head>"
      "<title>Tally bridge config</title>"
      "<style>"
        "body { background-color: black; color: #e6e5df; margin: 1rem; }"
        "form { display: flex; flex-direction: column; margin: 2rem 0; }"
        "input { max-width: 10rem; }"
        "button { margin: 0 0 .5rem 0; }"
        "i { padding: .3rem 0 0; width: 1.5rem; height: 1.5rem; text-align:center; display: inline-block; font-style: normal; border: 1px solid black; cursor: pointer; }"
        "i.pgm { background: red; color: black; }"
        "i.pvw { background: green; color: black; }"
        "i:hover { border-color: gray; }"
        "i.sel { border-color: yellow; }"
      "</style>"
    "</head>"
    "<body>"
    + getATEMInputs() + getTalliesHTML() +
    "<button onclick=\"selNone()\">None</button>"
    "<button onclick=\"selAll()\">All</button>"
    "<button onclick=\"color(0xFF0000)\">Red</button>"
    "<button onclick=\"color(0x00FF00)\">Green</button>"
    "<button onclick=\"color(0x0000FF)\">Blue</button><br/>"
    "<button onclick=\"color(0xFFFFFF)\">White</button><br/>"
    "<button onclick=\"signal(12)\">Change</button>"
    "<button onclick=\"signal(13)\">Left</button>"
    "<button onclick=\"signal(14)\">Down</button>"
    "<button onclick=\"signal(15)\">Up</button>"
    "<button onclick=\"signal(16)\">Right</button>"
    "<button onclick=\"signal(17)\">Focus</button>"
    "<button onclick=\"signal(18)\">DeFocus</button>"
    "<button onclick=\"signal(19)\">ZoomIn</button>"
    "<button onclick=\"signal(20)\">ZoomOut</button>"
    "<button onclick=\"signal(21)\">ISO+</button>"
    "<button onclick=\"signal(22)\">ISO-</button><br/>"
    "<input type=\"number\" name=\"camId\" placeholder=\"camId\"/>"
    "<button onclick=\"camId()\">Set camId</button><br/>"
    "<input type=\"number\" name=\"brightness\" placeholder=\"brightness\"/>"
    "<button onclick=\"brightness()\">Set brightness</button><br>"
    "<form method=\"post\" enctype=\"application/x-www-form-urlencoded\">"
      "<input type=\"text\" name=\"atemip\" value=\"" + config.atemIP.toString() + "\"><br>"
      "<input type=\"submit\" value=\"Set ATEM IP\">"
    "</form>"
    "<script>"
      "const all_i = document.querySelectorAll('i');"
      "all_i.forEach((e) => { e.addEventListener('click', () => { e.classList.toggle('sel')}) });"
      "function selNone() { all_i.forEach((e) => e.classList.remove('sel')) };"
      "function selAll() { all_i.forEach((e) => e.classList.add('sel')) };"
      "function inputs() {const ii=[]; document.querySelectorAll('i.sel').forEach((e) => ii.push(e.innerText)); return ii.join(); };"
      "function post(u){var x=new XMLHttpRequest();x.open('post',u);x.send()};"
      "function brightness() { const b = document.querySelector(\"input[name='brightness']\").value; post(`/set?brightness=${b}&i=${inputs()}`) };"
      "function color(c) { post(`/set?color=${c.toString(16)}&i=${inputs()}`) };"
      "function signal(n) { post(`/set?signal=${n}&i=${inputs()}`) };"
      "function camId() { const camId = document.querySelector(\"input[name='camId']\").value; post(`/set?camid=${camId}&i=${inputs()}`) };"
    "</script>"
    "</body>"
    "</html>");
}

uint64_t bitsFromCSV(String s) {
  uint64_t bits = 0;
  int number = 0;
  for (size_t i = 0; i < s.length(); ++i) {
    if (s[i] >= '0' && s[i] <= '9') {
      number = 10*number + s[i] - '0';
    } else if (number > 0) {
      bits |= 1 << (number-1);
      number = 0;
    }
  }
  if (number > 0) bits |= 1 << (number-1);
  return bits;
}

uint32_t parseHexColor(String s) {
  uint32_t color = 0;
  unsigned char c;
  for (int i=0; i < s.length(); i++) {
    c = s[i];
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
    } else if (name == "atemip") {
      config.atemIP.fromString(web.arg(i));
      configUpdated = true;
    } else if (name == "atemenable") {
      config.atemEnabled = (bool) web.arg(i).toInt();
      configUpdated = true;
    } else if (name == "obsip") {
      config.obsIP.fromString(web.arg(i));
      configUpdated = true;
    } else if (name == "obsport") {
      config.obsPort = web.arg(i).toInt();
      configUpdated = true;
    } else if (name == "obsenable") {
      config.obsEnabled = (bool) web.arg(i).toInt();
      configUpdated = true;
    }
  }
  web.send(200, "text/plain", "OK");
  delay(10);
  if (configUpdated) {
    writeConfig();
    ESP.restart();
  }
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
  web.on("/", handleRoot);
  web.on("/set", handleSet);
  web.begin();
}

void webserverLoop() {
  web.handleClient();
}