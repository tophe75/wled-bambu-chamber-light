#include "wled.h"
#include <WiFiClientSecure.h>
#include <PubSubClient.h>

/*
 * Usermod: Bambu Chamber Light Sync
 * -----------------------------------------------------------------------
 * Mirrors WLED's on/off state directly onto a Bambu Lab 3D printer's
 * chamber (or work) light, over its own MQTT connection straight to the
 * printer's built-in LAN broker. No Home Assistant, no bridge script,
 * no cloud account — this usermod is itself the MQTT client that talks
 * to the printer.
 *
 * Requirements on the printer side:
 *   - "LAN Only Mode" / Developer Mode enabled (Settings > WLAN on the
 *     printer's touchscreen), which is what exposes the local MQTT
 *     broker and the LAN Access Code.
 *   - You know the printer's LAN IP, serial number, and Access Code.
 *
 * Printer's local MQTT broker:
 *   host      = printer LAN IP
 *   port      = 8883 (TLS, self-signed certificate — verification is
 *               intentionally skipped below via setInsecure(), same as
 *               every other LAN Bambu integration does; the link is
 *               still encrypted, just not certificate-pinned)
 *   username  = "bblp"
 *   password  = LAN Access Code
 *   pub topic = device/<SERIAL>/request
 *
 * Command used to drive the light ("ledctrl"):
 *   {"system":{"sequence_id":"0","command":"ledctrl","led_node":"chamber_light",
 *              "led_mode":"on"|"off","led_on_time":500,"led_off_time":500,
 *              "loop_times":0,"interval_time":0}}
 *
 * All connection details are exposed as usermod settings (Config > Usermods
 * in the WLED UI) — nothing is hardcoded, nothing needs recompiling to change.
 */

class BambuChamberLightUsermod : public Usermod {
  private:
    // ---- persisted config (editable from Config > Usermods in the WLED UI) ----
    bool     enabled     = false;
    String   printerIP   = "";
    String   serial      = "";
    String   accessCode  = "";
    String   ledNode     = "chamber_light"; // or "work_light" on some models
    bool     invert      = false;           // send "off" when WLED turns on, and vice versa

    // ---- runtime state ----
    WiFiClientSecure secureClient;
    PubSubClient     mqtt;
    unsigned long    lastReconnectAttempt = 0;
    const unsigned long reconnectIntervalMs = 30000; // don't hammer the printer if it's offline
    bool             lastSentOn   = false;
    bool             haveSentOnce = false;
    char             reqTopic[48] = "";
    char             clientId[32] = "";

    static const char _name[];
    static const char _enabled[];
    static const char _printerIP[];
    static const char _serial[];
    static const char _accessCode[];
    static const char _ledNode[];
    static const char _invert[];

    void buildRuntimeStrings() {
      snprintf(reqTopic, sizeof(reqTopic), "device/%s/request", serial.c_str());
#ifdef ESP32
      snprintf(clientId, sizeof(clientId), "wled-bambu-%04X", (unsigned int)(ESP.getEfuseMac() & 0xFFFF));
#else
      snprintf(clientId, sizeof(clientId), "wled-bambu-%04X", (unsigned int)(ESP.getChipId() & 0xFFFF));
#endif
    }

    bool configComplete() {
      return enabled && printerIP.length() > 0 && serial.length() > 0 && accessCode.length() > 0;
    }

    void connectMqtt() {
      unsigned long now = millis();
      if (lastReconnectAttempt != 0 && (now - lastReconnectAttempt < reconnectIntervalMs)) return;
      lastReconnectAttempt = now;

      DEBUG_PRINTLN(F("[BambuLight] connecting to printer MQTT..."));
      secureClient.setInsecure(); // printer uses a self-signed cert on LAN
      mqtt.setServer(printerIP.c_str(), 8883);
      mqtt.setSocketTimeout(2);   // keep a failed attempt from stalling the WLED loop
      mqtt.setBufferSize(512);

      bool ok = mqtt.connect(clientId, "bblp", accessCode.c_str());
      if (ok) {
        DEBUG_PRINTLN(F("[BambuLight] connected"));
        haveSentOnce = false; // push current state right after (re)connecting
      } else {
        DEBUG_PRINT(F("[BambuLight] connect failed, rc="));
        DEBUG_PRINTLN(mqtt.state());
      }
    }

    void sendLedState(bool wledOn) {
      if (!mqtt.connected()) return;
      bool wantOn = invert ? !wledOn : wledOn;

      char payload[256];
      snprintf(payload, sizeof(payload),
        "{\"system\":{\"sequence_id\":\"0\",\"command\":\"ledctrl\","
        "\"led_node\":\"%s\",\"led_mode\":\"%s\","
        "\"led_on_time\":500,\"led_off_time\":500,"
        "\"loop_times\":0,\"interval_time\":0}}",
        ledNode.c_str(), wantOn ? "on" : "off");

      bool ok = mqtt.publish(reqTopic, payload);
      DEBUG_PRINT(F("[BambuLight] publish "));
      DEBUG_PRINT(wantOn ? F("on") : F("off"));
      DEBUG_PRINTLN(ok ? F(" OK") : F(" FAILED"));

      lastSentOn   = wledOn;
      haveSentOnce = true;
    }

    void syncIfNeeded() {
      bool curOn = (bri > 0); // WLED's own convention for "is the strip on"
      if (!haveSentOnce || curOn != lastSentOn) sendLedState(curOn);
    }

  public:
    BambuChamberLightUsermod() : mqtt(secureClient) {}

    void setup() {
      buildRuntimeStrings();
    }

    void loop() {
      if (!configComplete() || !WLED_CONNECTED) return;

      if (!mqtt.connected()) {
        connectMqtt();
      } else {
        mqtt.loop();
        syncIfNeeded();
      }
    }

    void onStateChange(uint8_t mode) {
      if (!configComplete() || !mqtt.connected()) return;
      syncIfNeeded();
    }

    void addToConfig(JsonObject& root) {
      JsonObject top = root.createNestedObject(FPSTR(_name));
      top[FPSTR(_enabled)]    = enabled;
      top[FPSTR(_printerIP)]  = printerIP;
      top[FPSTR(_serial)]     = serial;
      top[FPSTR(_accessCode)] = accessCode;
      top[FPSTR(_ledNode)]    = ledNode;
      top[FPSTR(_invert)]     = invert;
    }

    bool readFromConfig(JsonObject& root) {
      JsonObject top = root[FPSTR(_name)];
      bool complete = !top.isNull();
      complete &= getJsonValue(top[FPSTR(_enabled)],    enabled,    false);
      complete &= getJsonValue(top[FPSTR(_printerIP)],  printerIP,  "");
      complete &= getJsonValue(top[FPSTR(_serial)],     serial,     "");
      complete &= getJsonValue(top[FPSTR(_accessCode)], accessCode, "");
      complete &= getJsonValue(top[FPSTR(_ledNode)],    ledNode,    "chamber_light");
      complete &= getJsonValue(top[FPSTR(_invert)],     invert,     false);
      buildRuntimeStrings();
      // force a fresh push next loop, e.g. after credentials were edited in the UI
      haveSentOnce = false;
      return complete;
    }

    void addToJsonInfo(JsonObject& root) {
      JsonObject user = root["u"];
      if (user.isNull()) user = root.createNestedObject("u");
      JsonArray infoArr = user.createNestedArray(FPSTR(_name));
      if (!enabled) {
        infoArr.add(F("disabled"));
      } else if (!configComplete()) {
        infoArr.add(F("not configured"));
      } else if (!mqtt.connected()) {
        infoArr.add(F("printer MQTT: disconnected"));
      } else {
        infoArr.add(lastSentOn ? F("chamber light: on") : F("chamber light: off"));
      }
    }

    uint16_t getId() {
      return USERMOD_ID_UNSPECIFIED; // custom/local usermod, not registered upstream
    }
};

const char BambuChamberLightUsermod::_name[]       PROGMEM = "BambuChamberLight";
const char BambuChamberLightUsermod::_enabled[]    PROGMEM = "enabled";
const char BambuChamberLightUsermod::_printerIP[]  PROGMEM = "printer-ip";
const char BambuChamberLightUsermod::_serial[]     PROGMEM = "printer-serial";
const char BambuChamberLightUsermod::_accessCode[] PROGMEM = "access-code";
const char BambuChamberLightUsermod::_ledNode[]    PROGMEM = "led-node";
const char BambuChamberLightUsermod::_invert[]     PROGMEM = "invert";

static BambuChamberLightUsermod bambu_chamber_light;
REGISTER_USERMOD(bambu_chamber_light);
