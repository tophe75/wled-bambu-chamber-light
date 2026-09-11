#include "wled.h"
#include <PubSubClient.h>
#include "bambu_tls_client.h"   // minimal esp_tls-backed Arduino Client (no WiFiClientSecure in WLED's framework)

/*
 * Usermod: Bambu Chamber Light Sync
 * -----------------------------------------------------------------------
 * Mirrors WLED's on/off state directly onto a Bambu Lab 3D printer's
 * chamber light and/or toolhead ("work") light, over its own MQTT
 * connection straight to the printer's built-in LAN broker. No Home
 * Assistant, no bridge script, no cloud account — this usermod is
 * itself the MQTT client that talks to the printer.
 *
 * Each light is toggled independently in Config > Usermods (chamber on
 * by default, toolhead off, since most models don't expose a toolhead
 * light — enable it only if yours does).
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
 *               intentionally skipped, same as every other LAN Bambu
 *               integration does; the link is still encrypted, just not
 *               certificate-pinned)
 *   username  = "bblp"
 *   password  = LAN Access Code
 *   pub topic = device/<SERIAL>/request
 *
 * Command used to drive the light ("ledctrl"):
 *   {"system":{"sequence_id":"0","command":"ledctrl","led_node":"chamber_light",
 *              "led_mode":"on"|"off","led_on_time":500,"led_off_time":500,
 *              "loop_times":0,"interval_time":0}}
 *
 * TLS transport: WLED's ESP32 Arduino framework ships without the core
 * WiFiClientSecure library, so this usermod carries a tiny Arduino Client
 * built on ESP-IDF's esp_tls (see bambu_tls_client.h) — no certificate
 * verification, matching WiFiClientSecure::setInsecure().
 *
 * All connection details are exposed as usermod settings (Config > Usermods
 * in the WLED UI) — nothing is hardcoded, nothing needs recompiling to change.
 */

class BambuChamberLightUsermod : public Usermod {
  private:
    // One printer LED, tracked independently so each can resync/report on
    // its own (e.g. only the chamber light enabled, or both at once).
    struct LightNode {
      const char* apiName;             // "chamber_light" / "work_light"
      bool        lastSentOn   = false;
      bool        haveSentOnce = false;
      explicit LightNode(const char* n) : apiName(n) {}
    };

    // ---- persisted config (editable from Config > Usermods in the WLED UI) ----
    bool     enabled         = false;
    String   printerIP       = "";
    String   serial          = "";
    String   accessCode      = "";
    bool     chamberEnabled  = true;  // most printers: on by default
    bool     toolheadEnabled = false; // only some models expose this as "work_light"
    bool     invert          = false; // send "off" when WLED turns on, and vice versa

    // ---- runtime state ----
    BambuTlsClient   secureClient;
    PubSubClient     mqtt;
    unsigned long    lastReconnectAttempt = 0;
    const unsigned long reconnectIntervalMs = 30000; // don't hammer the printer if it's offline
    LightNode        chamber{"chamber_light"};
    LightNode        toolhead{"work_light"};
    char             reqTopic[48] = "";
    char             clientId[32] = "";

    static const char _name[];
    static const char _enabled[];
    static const char _printerIP[];
    static const char _serial[];
    static const char _accessCode[];
    static const char _chamberEnabled[];
    static const char _toolheadEnabled[];
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
      secureClient.setInsecure();          // printer uses a self-signed cert on LAN
      secureClient.setConnectTimeout(4000); // ms; bound the TLS handshake so an
                                            // unreachable printer can't stall WLED
      mqtt.setServer(printerIP.c_str(), 8883);
      mqtt.setSocketTimeout(2);   // keep a failed attempt from stalling the WLED loop
      mqtt.setBufferSize(512);

      bool ok = mqtt.connect(clientId, "bblp", accessCode.c_str());
      if (ok) {
        DEBUG_PRINTLN(F("[BambuLight] connected"));
        // push current state to every enabled node right after (re)connecting
        chamber.haveSentOnce  = false;
        toolhead.haveSentOnce = false;
      } else {
        DEBUG_PRINT(F("[BambuLight] connect failed, rc="));
        DEBUG_PRINTLN(mqtt.state());
      }
    }

    void sendLedState(LightNode& node, bool wledOn) {
      if (!mqtt.connected()) return;
      bool wantOn = invert ? !wledOn : wledOn;

      char payload[256];
      snprintf(payload, sizeof(payload),
        "{\"system\":{\"sequence_id\":\"0\",\"command\":\"ledctrl\","
        "\"led_node\":\"%s\",\"led_mode\":\"%s\","
        "\"led_on_time\":500,\"led_off_time\":500,"
        "\"loop_times\":0,\"interval_time\":0}}",
        node.apiName, wantOn ? "on" : "off");

      bool ok = mqtt.publish(reqTopic, payload);
      DEBUG_PRINT(F("[BambuLight] publish "));
      DEBUG_PRINT(node.apiName);
      DEBUG_PRINT(wantOn ? F(" on") : F(" off"));
      DEBUG_PRINTLN(ok ? F(" OK") : F(" FAILED"));

      node.lastSentOn   = wledOn;
      node.haveSentOnce = true;
    }

    void syncIfNeeded() {
      bool curOn = (bri > 0); // WLED's own convention for "is the strip on"
      if (chamberEnabled  && (!chamber.haveSentOnce  || curOn != chamber.lastSentOn))  sendLedState(chamber,  curOn);
      if (toolheadEnabled && (!toolhead.haveSentOnce || curOn != toolhead.lastSentOn)) sendLedState(toolhead, curOn);
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
      top[FPSTR(_enabled)]         = enabled;
      top[FPSTR(_printerIP)]       = printerIP;
      top[FPSTR(_serial)]          = serial;
      top[FPSTR(_accessCode)]      = accessCode;
      top[FPSTR(_chamberEnabled)]  = chamberEnabled;
      top[FPSTR(_toolheadEnabled)] = toolheadEnabled;
      top[FPSTR(_invert)]          = invert;
    }

    bool readFromConfig(JsonObject& root) {
      JsonObject top = root[FPSTR(_name)];
      bool complete = !top.isNull();
      complete &= getJsonValue(top[FPSTR(_enabled)],         enabled,         false);
      complete &= getJsonValue(top[FPSTR(_printerIP)],       printerIP,       "");
      complete &= getJsonValue(top[FPSTR(_serial)],          serial,          "");
      complete &= getJsonValue(top[FPSTR(_accessCode)],      accessCode,      "");
      complete &= getJsonValue(top[FPSTR(_chamberEnabled)],  chamberEnabled,  true);
      complete &= getJsonValue(top[FPSTR(_toolheadEnabled)], toolheadEnabled, false);
      complete &= getJsonValue(top[FPSTR(_invert)],          invert,          false);
      buildRuntimeStrings();
      // force a fresh push next loop, e.g. after credentials were edited in the UI
      chamber.haveSentOnce  = false;
      toolhead.haveSentOnce = false;
      return complete;
    }

    // Config > Usermods renders every String field as plain text; patch the
    // access code input to a password field so it isn't shown on screen.
    void appendConfigData() override {
      oappend(F("document.getElementsByName('BambuChamberLight:access-code')[0].type='password';"));
    }

    void addToJsonInfo(JsonObject& root) {
      JsonObject user = root["u"];
      if (user.isNull()) user = root.createNestedObject("u");
      JsonArray infoArr = user.createNestedArray(FPSTR(_name));
      if (!enabled) {
        infoArr.add(F("disabled"));
      } else if (!configComplete()) {
        infoArr.add(F("not configured"));
      } else if (!chamberEnabled && !toolheadEnabled) {
        infoArr.add(F("no lights enabled"));
      } else if (!mqtt.connected()) {
        infoArr.add(F("printer MQTT: disconnected"));
      } else {
        String status;
        if (chamberEnabled)  { status += F("chamber ");   status += chamber.lastSentOn  ? F("on") : F("off"); }
        if (toolheadEnabled) { if (status.length()) status += F(", ");
                                status += F("tool head "); status += toolhead.lastSentOn ? F("on") : F("off"); }
        infoArr.add(status);
      }
    }

    uint16_t getId() {
      return USERMOD_ID_UNSPECIFIED; // custom/local usermod, not registered upstream
    }
};

const char BambuChamberLightUsermod::_name[]            PROGMEM = "BambuChamberLight";
const char BambuChamberLightUsermod::_enabled[]         PROGMEM = "enabled";
const char BambuChamberLightUsermod::_printerIP[]       PROGMEM = "printer-ip";
const char BambuChamberLightUsermod::_serial[]          PROGMEM = "printer-serial";
const char BambuChamberLightUsermod::_accessCode[]      PROGMEM = "access-code";
const char BambuChamberLightUsermod::_chamberEnabled[]  PROGMEM = "chamber-enabled";
const char BambuChamberLightUsermod::_toolheadEnabled[] PROGMEM = "toolhead-enabled";
const char BambuChamberLightUsermod::_invert[]          PROGMEM = "invert";

static BambuChamberLightUsermod bambu_chamber_light;
REGISTER_USERMOD(bambu_chamber_light);
