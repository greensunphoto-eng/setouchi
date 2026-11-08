// Settings, with defaults for the ESP32 dongle. An optional claude-ds.ini on
// the SD card can override them (router mode, other SSID, token...).

#ifndef CLAUDE_DS_CONFIG_H
#define CLAUDE_DS_CONFIG_H

typedef struct {
    char ssid[33];    // Wi-Fi network name
    char key[33];     // WEP key (5 or 13 characters); empty for open networks
    char server[40];  // IPv4 address of the ESP32 or the PC bridge
    int port;
    char token[40];   // shared secret for router mode (empty with the ESP32)
    char source[48];  // where the settings came from (shown on screen)
} Config;

void config_defaults(Config *c);

// Applies "key=value" lines; '#' and ';' start comments. Returns the number
// of settings that were applied.
int config_parse(Config *c, const char *text);

#endif // CLAUDE_DS_CONFIG_H
