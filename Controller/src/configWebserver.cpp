#include "configWebserver.h"
#include <WebServer_WT32_ETH01.h>
#include "atemConnection.h"
#include "constants.h"
#include "espNow.h"
#include "memory.h"

#define HTTP_PORT 80

WebServer web(HTTP_PORT);

void handleRoot() {
  String message = "";
  if (web.method() == HTTP_POST) {
    for (uint8_t i = 0; i < web.args(); i++) {
      message = "ATEM IP set to: " + web.argName(i) + ": " + web.arg(i) + "\n";
      writeAtemIP(web.arg(i));
    }
  }
  web.send(200, "text/html", "<html>"
    "<head>"
      "<title>WebServer_WT32_ETH01 POST handling</title>"
      "<style>"
      "body { background-color: black; color: #e6e5df; margin-top: 2rem; }"
      "form { display: flex; flex-direction: column; margin: 2rem 0; }"
      "</style>"
      "<script>"
        "function post(u){var x=new XMLHttpRequest();x.open('post',u);x.send()}"
      "</script>"
    "</head>"
    "<body>"
    "<strong>" + message + "</strong>"
    "<form method=\"post\" enctype=\"application/x-www-form-urlencoded\">"
      "<h1>ATEM IP address:</h1><br>"
      "<input type=\"text\" name=\"ip\" value=\"" + atemIP.toString() + "\"><br>"
      "<input type=\"submit\" value=\"Save\">"
    "</form>"
    "<form method=\"post\" action=\"test\" enctype=\"application/x-www-form-urlencoded\">"
      "<label for=\"preview\">Preview:</label><input type=\"number\" name=\"preview\" value=\"2\">"
      "<label for=\"program\">Program:</label><input type=\"number\" name=\"program\" value=\"1\">"
      "<label for=\"transition\">Transition:</label><input type=\"checkbox\" name=\"transition\">"
      "<input type=\"submit\" value=\"Test\">"
    "</form>"
    "<pre>" + getATEMInformation() + "</pre>"
    "</body>"
    "</html>");
}

void handleTest()
{
  if (web.method() != HTTP_POST)
  {
    web.send(405, F("text/plain"), F("Method Not Allowed"));
    return;
  }
  broadcastTest(web.arg("program").toInt(), web.arg("preview").toInt());
  web.sendHeader("Location", "/");
  web.send(301, F("text/plain"), F("OK"));
}

void handleNotFound()
{
  web.send(404, F("text/plain"), F("File Not Found"));
}

void setupWebserver()
{
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
  web.on(F("/test/"), handleTest);
  web.onNotFound(handleNotFound);
  web.begin();
}

void webserverLoop()
{
  web.handleClient();
}