// Screen layouts. Renders the Model into two 32x24 character grids.
//
// Pure C, no libnds: the DS app copies the grids into the console tile maps,
// and the host tests print them / turn them into preview images.

#ifndef CLAUDE_DS_UI_H
#define CLAUDE_DS_UI_H

#include <stdint.h>

#include "proto.h"

#define GRID_W 32
#define GRID_H 24

// Palette numbers of the libnds console (main.c retunes a few of them).
enum {
    C_BLACK = 0,
    C_RED = 1,
    C_GREEN = 2,
    C_OLIVE = 3,
    C_FRAME = 4,   // retuned: muted blue for frames
    C_ORANGE = 5,  // retuned: warm orange
    C_TEAL = 6,
    C_LGRAY = 7,
    C_GRAY = 8,
    C_LRED = 9,
    C_LGREEN = 10,
    C_YELLOW = 11,
    C_LBLUE = 12,
    C_PINK = 13,
    C_CYAN = 14,
    C_WHITE = 15,
};

// A cell is (color << 8) | character. Characters 32-127 are ASCII, 128 and
// up are the extra glyphs from glyphs.h.
typedef struct {
    uint16_t cell[GRID_H][GRID_W];
} Grid;

typedef enum {
    NET_INIT,
    NET_SCANNING,
    NET_ASSOCIATING,
    NET_CONNECTING,  // Wi-Fi is up, opening the TCP connection
    NET_ONLINE,      // TCP connected
    NET_FAILED,      // Wi-Fi hardware problem
} NetState;

#define SEEN_MAX 6

typedef struct {
    NetState state;
    char ssid[33];        // network we are looking for
    char server[40];
    int port;
    char ip[16];          // our address once associated
    char status[30];      // short status text
    int seen_count;
    char seen_ssid[SEEN_MAX][21];
    char seen_sec[SEEN_MAX][6];
    int target_seen;
    int target_compatible;
    char config_source[48];
    const char *version;
    int muted;            // sound off (R button)
} NetInfo;

typedef enum {
    ACT_NONE = 0,
    ACT_ALLOW,
    ACT_DENY,
    ACT_STOP,
    ACT_PC,
    ACT_REMOTE,
    ACT_SCREEN,
} UiAction;

typedef enum {
    BOTTOM_CONNECTING,
    BOTTOM_CONTROL,
    BOTTOM_PERMISSION,
} BottomMode;

BottomMode ui_bottom_mode(const Model *m, const NetInfo *n);

void ui_render_top(Grid *g, const Model *m, const NetInfo *n, uint32_t frame);

// `flash` highlights a button for a moment after it was pressed.
void ui_render_bottom(Grid *g, const Model *m, const NetInfo *n, uint32_t frame,
                      UiAction flash);

// Touch screen coordinates (0-255, 0-191) -> action on the current layout.
UiAction ui_hit_test(const Model *m, const NetInfo *n, int px, int py);

#endif // CLAUDE_DS_UI_H
