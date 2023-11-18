#include "configWebserver.h"
#include <WebServer_WT32_ETH01.h>
#include "main.h"
#include "constants.h"
#include "espNow.h"
#include "memory.h"
#include "main.h"

WebServer web(80);

String getATEMInformationHTML() {
  String s = "";
  for (int i=1; i<64; i++) {
    if (AtemSwitcher.getProgramTally(i)) {
      s += "<i class=pgm>" + String(i) + "</i>";
    } else
    if (AtemSwitcher.getPreviewTally(i)) {
      s += "<i class=pvw>" + String(i) + "</i>";
    } else {
      s += "<i>" + String(i) + "</i>";
    }
  }
  return s;
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
  String message = "";
  if (web.method() == HTTP_POST) {
    for (uint8_t i = 0; i < web.args(); i++) {
      if (web.argName(i) == "atemip") {
        writeAtemIP(web.arg(i));
        message += "ATEM IP set to: " + web.arg(i) + "\n";
      }
    }
  }
  web.send(200, "text/html", "<html>"
    "<head>"
      "<title>Tally bridge config</title>"
      "<style>"
      "body {background-color: black; color: #e6e5df; margin-top: 2rem;}"
      "form {display: flex; flex-direction: column; margin: 2rem 0;}"
      "input {max-width: 10rem;}"
      "i {padding: .3rem .5rem; display: inline-block;}"
      "i.pgm {background: red; color: black;}"
      "i.pvw {background: green; color: black;}"
      "</style>"
    "</head>"
    "<body>"
    "<pre>" + message + "</pre>"
    "<form method=\"post\" enctype=\"application/x-www-form-urlencoded\">"
      "<label>ATEM IP address:</label><br>"
      "<input type=\"text\" name=\"atemip\" value=\"" + atemIP.toString() + "\"><br>"
      "<input type=\"submit\" value=\"Save\">"
    "</form>"
    "<form method=\"post\" action=\"test\" enctype=\"application/x-www-form-urlencoded\">"
      "<label for=\"preview\">Preview:</label><input type=\"number\" name=\"preview\" value=\"2\">"
      "<label for=\"program\">Program:</label><input type=\"number\" name=\"program\" value=\"1\">"
      "<input type=\"submit\" value=\"Test\">"
    "</form>"
    + getATEMInformationHTML() + getTalliesHTML() +
    "<script>"
      "function post(u){var x=new XMLHttpRequest();x.open('post',u);x.send()}"
      "document.querySelectorAll('div').forEach((e) => {e.addEventListener('click', ()=>{post(`/tally?program=${e.innerText}&preview=1`)})})"
    "</script>"
    "</body>"
    "</html>");
}

void handleTest() {
  if (web.method() != HTTP_POST) {
    web.send(405, F("text/plain"), F("Method Not Allowed"));
    return;
  }
  broadcastTest(web.arg("program").toInt(), web.arg("preview").toInt());
  web.sendHeader("Location", "/");
  web.send(301, F("text/plain"), F("OK"));
}

uint64_t bitsFromCSV(String s) {
  uint64_t bits = 0;
  int number;
  size_t startPos = 0;
  for (size_t i = 0; i <= s.length(); ++i) {
    if (i == s.length() || s[i] == ',') {
      number = s.substring(startPos, i - startPos).toInt();
      bits += 1 << (number-1);
      startPos = i + 1;
    }
  }
  return bits;
}

void handleTally() {
  uint64_t programBits = bitsFromCSV(web.arg("program"));
  uint64_t previewBits = bitsFromCSV(web.arg("preview"));
  broadcastTally(&programBits, &previewBits);
  web.sendHeader("Location", "/");
  web.send(301, F("text/plain"), F("OK"));
}

void handleColor() {
  uint64_t bits = bitsFromCSV(web.arg("i"));
  uint8_t r = web.arg("r").toInt();
  uint8_t g = web.arg("g").toInt();
  uint8_t b = web.arg("b").toInt();
  broadcastColor(r, g, b, &bits);
  web.sendHeader("Location", "/");
  web.send(301, F("text/plain"), F("OK"));
}

void handleBrightness() {
  broadcastBrightness(web.arg("id").toInt(), web.arg("brightness").toInt());
  web.sendHeader("Location", "/");
  web.send(301, F("text/plain"), F("OK"));
}

void handleNotFound() {
  web.send(404, F("text/plain"), F("File Not Found"));
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
  web.on(F("/"), handleRoot);
  web.on(F("/test"), handleTest);
  web.on(F("/tally"), handleTally);
  web.on(F("/color"), handleColor);
  web.on(F("/brightness"), handleBrightness);
  web.onNotFound(handleNotFound);
  web.begin();
}

void webserverLoop()
{
  web.handleClient();
}