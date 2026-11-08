// Claude DS - ESP32 Wi-Fi dongle for the Nintendo DS
//
// Plug this ESP32 into the PC with a USB cable. It creates a tiny Wi-Fi
// network ("CLAUDE-DS") that only the DS uses, and relays lines of text
// between the DS (TCP port 7777) and the PC bridge (USB serial).
//
// Why an ESP32: the original Nintendo DS can only join open/WEP networks at
// 802.11b rates, which modern home routers no longer offer. This network has
// no route to your home network or the internet - the ESP32 only forwards
// short text lines - and it pairs with the first DS that joins (the bridge
// command `--unpair` resets that).
//
// Build: Arduino IDE / arduino-cli with the "esp32" core 3.x.
// Board: "ESP32 Dev Module" (classic ESP32) or "ESP32C3 Dev Module" with
//        "USB CDC On Boot: Enabled" (ESP32-C3 SuperMini and similar).

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_wifi.h>

#define CLAUDE_DS_FW_VERSION "1.0.0"

// ---- settings ---------------------------------------------------------------

static const char *AP_SSID = "CLAUDE-DS";  // must match ssid= on the DS
static const int AP_CHANNEL = 6;
static const int AP_MAX_STATIONS = 2;       // the DS (+1 slot for a stale entry)
static const uint16_t TCP_PORT = 7777;
static const uint32_t SERIAL_BAUD = 115200;
static const size_t RELAY_LINE_MAX = 300;

// ---- state ------------------------------------------------------------------

static WiFiServer server(TCP_PORT);
static WiFiClient client;
static bool clientUp = false;
static Preferences prefs;

static uint8_t pairedMac[6];
static bool hasPaired = false;

// Station join/leave events arrive on the Wi-Fi event task; loop() handles them.
struct StaEvent {
    uint8_t mac[6];
    uint16_t aid;
    bool joined;
};
static portMUX_TYPE evMux = portMUX_INITIALIZER_UNLOCKED;
static StaEvent evQueue[8];
static volatile int evHead = 0;
static volatile int evTail = 0;

static char serialLine[RELAY_LINE_MAX + 2];
static size_t serialLen = 0;
static char clientLine[RELAY_LINE_MAX + 2];
static size_t clientLen = 0;

// ---- helpers ----------------------------------------------------------------

static void macToStr(const uint8_t *mac, char *out) {
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void printReady() {
    uint8_t proto = 0;
    esp_wifi_get_protocol(WIFI_IF_AP, &proto);
    char mac[18] = "none";
    if (hasPaired) {
        macToStr(pairedMac, mac);
    }
    Serial.printf("#ESP READY %s %s ch=%d proto=0x%x paired=%s ip=%s\n",
                  AP_SSID, CLAUDE_DS_FW_VERSION, AP_CHANNEL, proto, mac,
                  WiFi.softAPIP().toString().c_str());
}

static void printLink() {
    if (clientUp) {
        Serial.printf("#DS UP %s\n", client.remoteIP().toString().c_str());
    } else {
        Serial.println("#DS DOWN");
    }
}

static void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
    StaEvent ev;
    if (event == ARDUINO_EVENT_WIFI_AP_STACONNECTED) {
        memcpy(ev.mac, info.wifi_ap_staconnected.mac, 6);
        ev.aid = info.wifi_ap_staconnected.aid;
        ev.joined = true;
    } else if (event == ARDUINO_EVENT_WIFI_AP_STADISCONNECTED) {
        memcpy(ev.mac, info.wifi_ap_stadisconnected.mac, 6);
        ev.aid = info.wifi_ap_stadisconnected.aid;
        ev.joined = false;
    } else {
        return;
    }
    portENTER_CRITICAL(&evMux);
    int next = (evHead + 1) % 8;
    if (next != evTail) {
        evQueue[evHead] = ev;
        evHead = next;
    }
    portEXIT_CRITICAL(&evMux);
}

static void handleStaEvents() {
    for (;;) {
        StaEvent ev;
        bool have = false;
        portENTER_CRITICAL(&evMux);
        if (evTail != evHead) {
            ev = evQueue[evTail];
            evTail = (evTail + 1) % 8;
            have = true;
        }
        portEXIT_CRITICAL(&evMux);
        if (!have) {
            return;
        }

        char mac[18];
        macToStr(ev.mac, mac);
        if (!ev.joined) {
            Serial.printf("#STA LEAVE %s\n", mac);
            continue;
        }
        if (!hasPaired) {
            memcpy(pairedMac, ev.mac, 6);
            hasPaired = true;
            prefs.putBytes("mac", pairedMac, 6);
            Serial.printf("#PAIRED %s\n", mac);
        } else if (memcmp(pairedMac, ev.mac, 6) != 0) {
            esp_wifi_deauth_sta(ev.aid);
            Serial.printf("#STA BLOCKED %s\n", mac);
            continue;
        }
        Serial.printf("#STA JOIN %s\n", mac);
    }
}

static void handleCommand(const char *cmd) {
    if (strcmp(cmd, "#INFO") == 0) {
        printReady();
        printLink();
    } else if (strcmp(cmd, "#UNPAIR") == 0) {
        hasPaired = false;
        prefs.remove("mac");
        Serial.println("#UNPAIRED");
    } else if (strcmp(cmd, "#PING") == 0) {
        Serial.println("#PONG");
    }
}

// PC -> DS. Lines starting with '#' are commands for the ESP32 itself.
static void handleSerial() {
    while (Serial.available() > 0) {
        int c = Serial.read();
        if (c < 0) {
            break;
        }
        if (c == '\r') {
            continue;
        }
        if (c != '\n') {
            if (serialLen < RELAY_LINE_MAX) {
                serialLine[serialLen++] = (char)c;
            }
            continue;
        }
        serialLine[serialLen] = '\0';
        if (serialLen > 0) {
            if (serialLine[0] == '#') {
                handleCommand(serialLine);
            } else if (clientUp) {
                serialLine[serialLen] = '\n';
                client.write((const uint8_t *)serialLine, serialLen + 1);
            }
        }
        serialLen = 0;
    }
}

// DS -> PC. The DS may not send '#' lines (those are reserved for the ESP32).
static void handleClient() {
    if (server.hasClient()) {
        WiFiClient incoming = server.accept();
        if (incoming) {
            if (clientUp) {
                client.stop();  // the newest connection wins (DS reconnected)
            }
            client = incoming;
            client.setNoDelay(true);
            clientUp = true;
            clientLen = 0;
            printLink();
        }
    }
    if (!clientUp) {
        return;
    }
    if (!client.connected()) {
        client.stop();
        clientUp = false;
        printLink();
        return;
    }
    int avail = client.available();
    while (avail-- > 0) {
        int c = client.read();
        if (c < 0) {
            break;
        }
        if (c == '\r') {
            continue;
        }
        if (c != '\n') {
            if (clientLen < RELAY_LINE_MAX) {
                clientLine[clientLen++] = (char)c;
            }
            continue;
        }
        if (clientLen > 0 && clientLine[0] != '#') {
            clientLine[clientLen] = '\n';
            Serial.write((const uint8_t *)clientLine, clientLen + 1);
        }
        clientLen = 0;
    }
}

// ---- Arduino entry points -----------------------------------------------------

void setup() {
    Serial.begin(SERIAL_BAUD);
#if ARDUINO_USB_CDC_ON_BOOT && defined(CONFIG_IDF_TARGET_ESP32C3)
    // Native USB: never stall the relay when no PC program is reading.
    Serial.setTxTimeoutMs(0);
#endif

    prefs.begin("claude-ds", false);
    hasPaired = prefs.getBytes("mac", pairedMac, sizeof(pairedMac)) == sizeof(pairedMac);

    WiFi.onEvent(onWiFiEvent);
    WiFi.persistent(false);
    WiFi.mode(WIFI_AP);
    // The DS only speaks 802.11b (1-2 Mbit/s): advertise 802.11b rates only,
    // otherwise the DS treats the network as incompatible.
    esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_11B);
    WiFi.softAP(AP_SSID, nullptr, AP_CHANNEL, 0, AP_MAX_STATIONS);
    // Notice a powered-off DS within a minute (the DS pings every 5 s).
    esp_wifi_set_inactive_time(WIFI_IF_AP, 60);

    server.begin();
    server.setNoDelay(true);
    printReady();
}

void loop() {
    handleStaEvents();
    handleClient();
    handleSerial();
    delay(1);
}
