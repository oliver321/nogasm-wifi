#include "../include/WebSocketHelper.h"
#include "../VERSION.h"

#include <FS.h>
#include <SD.h>
#include <SPI.h>

#include "../include/Console.h"
#include "../include/OrgasmControl.h"
#include "../include/Hardware.h"
#include "../include/Page.h"
#include "../include/SDHelper.h"

#include "../config.h"

namespace WebSocketHelper {
  void begin() {
    // Start WebSocket server and assign callback
    webSocket = new RedirectingWebSocketsServer(Config.websocket_port);
    webSocket->begin();
    webSocket->onEvent(onWebSocketEvent);
    Serial.println("Websocket server running.");
  }

  void tick() {
    if (webSocket != nullptr)
      webSocket->loop();
  }

  void send(const char *cmd, JsonDocument &doc, int num) {
    if (webSocket == nullptr) return;

    DynamicJsonDocument envelope(1024);
    envelope[cmd] = doc;

    String payload;
    serializeJson(envelope, payload);

    if (num > 0) {
      webSocket->sendTXT(num, payload);
    } else {
      for (auto const &p : connections) {
        int num = p.first;
        webSocket->sendTXT(num, payload);
      }
    }
  }

  void send(const char *cmd, String text, int num) {
    DynamicJsonDocument doc(1024);
    doc["text"] = text;
    send(cmd, doc, num);
  }

  /*
   * Helpers here which handle sending all server responses.
   * The first parameter should be int num, followed by any additional
   * parameters needed for this request (int nonce, ...)
   */

  void sendSystemInfo(int num) {
    DynamicJsonDocument doc(200);
    doc["device"] = "Edge-o-Matic 3000";
    doc["serial"] = String(Hardware::getDeviceSerial());
    doc["hwVersion"] = "";
    doc["fwVersion"] = VERSION;

    send("info", doc, num);
  }

  void sendSettings(int num) {
    DynamicJsonDocument doc(4096);
    dumpConfigToJsonObject(doc);

    send("configList", doc, num);
  }

  void sendWxStatus(int num) {
    DynamicJsonDocument doc(200);
    doc["ssid"] = Config.wifi_ssid;
    doc["ip"] = WiFi.localIP().toString();
    doc["rssi"] = WiFi.RSSI();

    send("wifiStatus", doc, num);
  }

  void sendSdStatus(int num) {
    uint8_t cardType = SD.cardType();
    uint64_t cardSize = SD.cardSize() / (1024 * 1024);

    DynamicJsonDocument doc(200);
    doc["size"] = cardSize;

    switch(cardType) {
      case CARD_MMC:
        doc["type"] = "MMC";
        break;
      case CARD_SD:
        doc["type"] = "SD";
        break;
      case CARD_SDHC:
        doc["type"] = "SDHC";
        break;
      default:
        doc["type"] = "UNKNOWN";
        break;
    }

    send("sdStatus", doc, num);
  }

  void sendReadings(int num) {
//    String screenshot;
//    UI.screenshot(screenshot);

    // Serialize Data
    DynamicJsonDocument doc(3072);
    doc["pressure"] = OrgasmControl::getLastPressure();
    doc["pavg"] = OrgasmControl::getAveragePressure();
    doc["motor"] = Hardware::getMotorSpeed();
    doc["arousal"] = OrgasmControl::getArousal();
    doc["millis"] = millis();
//    doc["screenshot"] = screenshot;

    send("readings", doc, num);
  }

  /*
   * Helpers here for parsing and responding to commands sent
   * by the client. First parameter should also be int num.
   */

  void cbSerialCmd(int num, JsonVariant args) {
    int nonce = args["nonce"];
    String text;

    char line[SERIAL_BUFFER_LEN];
    strlcpy(line, args["cmd"], SERIAL_BUFFER_LEN - 1);
    Console::handleMessage(line, text);

    DynamicJsonDocument resp(1024);
    resp["nonce"] = nonce;
    resp["text"] = text;

    send("serialCmd", resp, num);
  }

  void cbDir(int num, JsonVariant args) {
    int nonce = args["nonce"];
    String path = args["path"];

    if (path[0] != '/') {
      path = String("/") + path;
    }

    DynamicJsonDocument resp(1024);
    resp["nonce"] = nonce;
    JsonArray files = resp.createNestedArray("files");

    File f = SD.open(path);
    if (!f) {
      resp["error"] = "Invalid directory.";
    } else {
      while (true) {
        File entry = f.openNextFile();
        if (! entry) {
          break;
        }

        JsonObject file = files.createNestedObject();
        file["name"] = String(entry.name());
        file["size"] = entry.size();
        file["dir"]  = entry.isDirectory();

        entry.close();
      }
      f.close();
    }

    send("dir", resp, num);
  }

  void cbMkdir(int num, JsonVariant args) {
    int nonce = args["nonce"];
    String path = args["path"];

    // Clean up Path:
    if (path[0] != '/') {
      path = String("/") + path;
    }

    DynamicJsonDocument resp(1024);
    resp["nonce"] = nonce;
    resp["path"] = path;

    bool success = SD.mkdir(path);
    if (!success) {
      resp["error"] = "Failed to create directory.";
    }

    send("mkdir", resp, num);
  }

  void cbConfigSet(int num, JsonVariant args) {
    auto config = args.as<JsonObject>();
    bool restart_required = false;

    for (auto kvp : config) {
      setConfigValue(kvp.key().c_str(), kvp.value().as<String>().c_str(), restart_required);
    }

    // Send new settings to client:
    saveConfigToSd(millis() + 300);
  }

  void cbSetMode(int num, JsonVariant mode) {
    RunGraphPage.setMode(mode);
  }

  void cbSetMotor(int num, JsonVariant speed) {
    Hardware::setMotorSpeed(speed);
  }

  namespace {
    void onMessage(int num, uint8_t * payload) {
      Serial.printf("[%u] %s", num, payload);
      Serial.println();

      DynamicJsonDocument doc(1024);
      DeserializationError err = deserializeJson(doc, payload);

      if (err) {
        Serial.println("Deserialization Error!");
      } else {
        for (auto kvp : doc.as<JsonObject>()) {
          auto cmd = kvp.key().c_str();

          if (! strcmp(cmd, "configSet")) {
            cbConfigSet(num, kvp.value());
          } else if (! strcmp(cmd, "info")) {
            sendSystemInfo(num);
          } else if (! strcmp(cmd, "configList")) {
            sendSettings(num);
          } else if (! strcmp(cmd, "serialCmd")) {
            cbSerialCmd(num, kvp.value());
          } else if (! strcmp(cmd, "getWiFiStatus")) {
            sendWxStatus(num);
          } else if (! strcmp(cmd, "getSDStatus")) {
            sendSdStatus(num);
          } else if (! strcmp(cmd, "setMode")) {
             cbSetMode(num, kvp.value());
          } else if (! strcmp(cmd, "setMotor")) {
             cbSetMotor(num, kvp.value());
          } else if (! strcmp(cmd, "streamReadings")) {
            WebSocketConnection *client = connections[num];
            client->stream_readings = kvp.value();
          } else if (! strcmp(cmd, "dir")) {
            cbDir(num, kvp.value());
          } else if (! strcmp(cmd, "mkdir")) {
            cbMkdir(num, kvp.value());
          } else {
            send("error", String("Unknown command: ") + String(cmd), num);
          }
        }
      }
    }

    // Called when receiving any WebSocket message
    void onWebSocketEvent(int num,
                          WStype_t type,
                          uint8_t * payload,
                          size_t length) {

      // Figure out the type of WebSocket event
      switch(type) {

        // Client has disconnected
        case WStype_DISCONNECTED:
          Serial.printf("[%u] Disconnected!\n", num);
          connections.erase(num);
          break;

          // New client has connected
        case WStype_CONNECTED:
        {
          WebSocketConnection *client = new WebSocketConnection;
          IPAddress ip = webSocket->remoteIP(num);
          client->ip = ip;
          client->num = num;
          connections[num] = client;

          last_connection = num;
          sendSystemInfo(num);
          Serial.printf("[%u] Connection from ", num);
          Serial.println(ip.toString());
        }
          break;

          // Echo text message back to client
        case WStype_TEXT:
          onMessage(num, payload);
          break;

          // For everything else: do nothing
        case WStype_BIN:
        case WStype_ERROR:
        case WStype_FRAGMENT_TEXT_START:
        case WStype_FRAGMENT_BIN_START:
        case WStype_FRAGMENT:
        case WStype_FRAGMENT_FIN:
        default:
          break;
      }
    }
  }
}