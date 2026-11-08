// Claude DS - Claude Code status on a Nintendo DS
//
// Joins the "CLAUDE-DS" Wi-Fi network made by the ESP32 dongle (or any
// open/WEP network set in claude-ds.ini), connects to the PC bridge over TCP
// and shows what Claude Code is doing. Permission prompts can be answered
// from the bottom screen when remote approval is on.
//
// Buttons: A/B/Y/X answer a permission request (allow/deny/stop/ask on PC),
// SELECT toggles remote approval, START turns the screens off/on, R mutes.
// Closing the lid keeps it running with the screens dark.

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <nds.h>
#include <dswifi9.h>
#include <fat.h>

#include "chime.h"
#include "config.h"
#include "glyphs.h"
#include "proto.h"
#include "ui.h"

#define APP_VERSION "1.0.0"

#define FPS 60
#define PING_INTERVAL (5 * FPS)
#define RX_TIMEOUT (15 * FPS)
#define ASSOC_TIMEOUT (20 * FPS)
#define CONNECT_TIMEOUT (6 * FPS)
#define RETRY_DELAY (2 * FPS)
#define SCAN_INTERVAL (FPS / 2)
#define IDLE_SCREEN_OFF (10 * 60 * FPS)
#define FLASH_FRAMES 12

static PrintConsole topConsole;
static PrintConsole bottomConsole;
static Grid topGrid;
static Grid bottomGrid;
static Model model;
static NetInfo net;
static Config cfg;

static uint32_t frame;
static uint32_t stateSince;
static uint32_t retryDelay;
static uint32_t connectStarted;
static uint32_t lastRx;
static uint32_t lastPing;
static uint32_t lastActivity;

static Wifi_AccessPoint targetAp;
static struct sockaddr_in serverAddr;
static int sock = -1;
static int tcpInProgress;
static char rxBuf[1024];
static int rxLen;

static int screensOn = 1;   // START toggles this
static int lidClosed;
static int backlightOn = 1;

static UiAction flashAction = ACT_NONE;
static uint32_t flashUntil;
static char answeredId[12];
static uint32_t answeredUntil;

// ---------------------------------------------------------------------------
// Video
// ---------------------------------------------------------------------------

// Uploads the extra glyphs right after the 96 ASCII tiles of the console font.
// Same format as the font: 4bpp, pixel value 15 = text color of the palette.
static void load_glyphs(PrintConsole *con)
{
    u32 *dst = (u32 *)con->fontBgGfx + (con->fontCharOffset + 96) * 8;
    for (int i = 0; i < GLYPH_COUNT; i++)
    {
        for (int r = 0; r < 8; r++)
        {
            u8 bits = glyph_bits[i][r];
            u32 v = 0;
            for (int b = 0; b < 8; b++)
            {
                if (bits & (0x80 >> b))
                    v |= 0xFu << (b * 4);
            }
            dst[i * 8 + r] = v;
        }
    }
}

static void tune_palette(u16 *pal)
{
    pal[0] = RGB15(1, 2, 5);                     // background: deep navy
    pal[C_FRAME * 16 + 15] = RGB15(7, 10, 20);   // frames: muted blue
    pal[C_ORANGE * 16 + 15] = RGB15(31, 16, 6);  // warm orange
    pal[C_GRAY * 16 + 15] = RGB15(14, 15, 18);   // soft gray
    pal[C_LBLUE * 16 + 15] = RGB15(12, 18, 31);  // idle buddy
}

static void setup_video(void)
{
    videoSetMode(MODE_0_2D);
    videoSetModeSub(MODE_0_2D);
    vramSetBankA(VRAM_A_MAIN_BG);
    vramSetBankC(VRAM_C_SUB_BG);

    consoleInit(&topConsole, 0, BgType_Text4bpp, BgSize_T_256x256, 31, 0, true, true);
    consoleInit(&bottomConsole, 0, BgType_Text4bpp, BgSize_T_256x256, 31, 0, false, true);

    load_glyphs(&topConsole);
    load_glyphs(&bottomConsole);
    tune_palette(BG_PALETTE);
    tune_palette(BG_PALETTE_SUB);
}

// Copies a character grid straight into the console's tile map.
static void blit(PrintConsole *con, const Grid *g)
{
    u16 *map = con->fontBgMap;
    u16 base = con->fontCharOffset;
    for (int y = 0; y < GRID_H; y++)
    {
        u16 *row = map + y * con->consoleWidth;
        for (int x = 0; x < GRID_W; x++)
        {
            u16 cell = g->cell[y][x];
            int ch = cell & 0xFF;
            int color = (cell >> 8) & 0xF;
            u16 tile = base;
            if (ch >= GLYPH_FIRST && ch < G_END)
                tile = base + 96 + (ch - GLYPH_FIRST);
            else if (ch >= 32 && ch < 128)
                tile = base + (ch - 32);
            row[x] = TILE_PALETTE(color) | tile;
        }
    }
}

static void render(void)
{
    net.muted = chime_muted();
    UiAction flash = frame < flashUntil ? flashAction : ACT_NONE;
    ui_render_top(&topGrid, &model, &net, frame);
    ui_render_bottom(&bottomGrid, &model, &net, frame, flash);
    blit(&topConsole, &topGrid);
    blit(&bottomConsole, &bottomGrid);
}

static void apply_backlight(void)
{
    int idle = (frame - lastActivity) > IDLE_SCREEN_OFF;
    int want = screensOn && !idle && !lidClosed;
    if (want == backlightOn)
        return;
    backlightOn = want;
    if (want)
        powerOn(PM_BACKLIGHT_TOP | PM_BACKLIGHT_BOTTOM);
    else
        powerOff(PM_BACKLIGHT_TOP | PM_BACKLIGHT_BOTTOM);
}

static void wake(void)
{
    lastActivity = frame;
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

static void load_config(void)
{
    config_defaults(&cfg);

    if (!fatInitDefault())
    {
        snprintf(cfg.source, sizeof(cfg.source), "settings: defaults (no SD)");
        return;
    }

    char paths[3][128];
    int count = 0;
    char *cwd = fatGetDefaultCwd();
    if (cwd)
    {
        snprintf(paths[count++], sizeof(paths[0]), "%sclaude-ds.ini", cwd);
        free(cwd);
    }
    const char *drive = fatGetDefaultDrive();
    snprintf(paths[count++], sizeof(paths[0]), "%sclaude-ds.ini", drive);
    snprintf(paths[count++], sizeof(paths[0]), "%s_nds/claude-ds.ini", drive);

    for (int i = 0; i < count; i++)
    {
        FILE *f = fopen(paths[i], "rb");
        if (!f)
            continue;
        static char text[1024];
        size_t len = fread(text, 1, sizeof(text) - 1, f);
        fclose(f);
        text[len] = '\0';
        int applied = config_parse(&cfg, text);
        snprintf(cfg.source, sizeof(cfg.source), "settings: claude-ds.ini (%d)", applied);
        return;
    }
    snprintf(cfg.source, sizeof(cfg.source), "settings: defaults");
}

// ---------------------------------------------------------------------------
// Network
// ---------------------------------------------------------------------------

static void set_status(const char *s)
{
    snprintf(net.status, sizeof(net.status), "%s", s);
}

static void net_enter(NetState st)
{
    net.state = st;
    stateSince = frame;
}

static void tcp_close(const char *why)
{
    if (sock >= 0)
    {
        close(sock);
        sock = -1;
    }
    tcpInProgress = 0;
    rxLen = 0;
    model_disconnected(&model);
    if (why)
        set_status(why);
}

// Go back to TCP connecting after a short pause (Wi-Fi stays up).
static void tcp_retry(const char *why)
{
    tcp_close(why);
    retryDelay = RETRY_DELAY;
    net_enter(NET_CONNECTING);
}

// Leave the network and search again after a short pause.
static void wifi_restart(const char *why)
{
    tcp_close(why);
    Wifi_DisconnectAP();
    net.ip[0] = '\0';
    net_enter(NET_INIT);
}

static void send_line(const char *line)
{
    if (sock < 0 || tcpInProgress)
        return;
    char buf[160];
    int n = snprintf(buf, sizeof(buf), "%s\n", line);
    if (n <= 0)
        return;
    if (n >= (int)sizeof(buf))
    {
        n = sizeof(buf) - 1;
        buf[n - 1] = '\n';
    }
    int off = 0;
    int tries = 0;
    while (off < n)
    {
        int w = send(sock, buf + off, n - off, 0);
        if (w > 0)
        {
            off += w;
            continue;
        }
        if (w < 0 && (errno == EWOULDBLOCK || errno == EAGAIN) && tries++ < 30)
        {
            cothread_yield();
            continue;
        }
        tcp_retry("LINK LOST");
        return;
    }
}

static void send_hello(void)
{
    char line[96];
    snprintf(line, sizeof(line), "HELLO claude-ds %d %s", PROTO_VERSION, cfg.token);
    send_line(line);
}

static void start_scan(void)
{
    Wifi_ScanMode();
    net.target_seen = 0;
    net.target_compatible = 0;
    set_status("LOOKING FOR WI-FI");
    net_enter(NET_SCANNING);
}

static const char *security_label(Wifi_ApSecurityType type)
{
    switch (type)
    {
        case AP_SECURITY_OPEN: return "open";
        case AP_SECURITY_WEP: return "WEP";
        default: return "WPA";
    }
}

static void scan_step(void)
{
    if ((frame - stateSince) % SCAN_INTERVAL != 0)
        return;

    int count = Wifi_GetNumAP();
    int seen = 0;
    int found = 0;

    for (int i = 0; i < count; i++)
    {
        Wifi_AccessPoint ap;
        if (Wifi_GetAPData(i, &ap) != WIFI_RETURN_OK || ap.ssid_len == 0)
            continue;

        int is_target = strcmp(ap.ssid, cfg.ssid) == 0;
        if (is_target && !found)
        {
            found = 1;
            targetAp = ap;
            // Keep our network at the top of the list.
            if (seen == SEEN_MAX)
                seen--;
            memmove(net.seen_ssid[1], net.seen_ssid[0], sizeof(net.seen_ssid[0]) * seen);
            memmove(net.seen_sec[1], net.seen_sec[0], sizeof(net.seen_sec[0]) * seen);
            snprintf(net.seen_ssid[0], sizeof(net.seen_ssid[0]), "%.20s", ap.ssid);
            snprintf(net.seen_sec[0], sizeof(net.seen_sec[0]), "%s",
                     security_label(ap.security_type));
            seen++;
        }
        else if (!is_target && seen < SEEN_MAX)
        {
            snprintf(net.seen_ssid[seen], sizeof(net.seen_ssid[0]), "%.20s", ap.ssid);
            snprintf(net.seen_sec[seen], sizeof(net.seen_sec[0]), "%s",
                     security_label(ap.security_type));
            seen++;
        }
    }
    net.seen_count = seen;

    if (!found)
        return;

    net.target_seen = 1;
    net.target_compatible = (targetAp.flags & WFLAG_APDATA_COMPATIBLE) != 0;

    Wifi_SetIP(0, 0, 0, 0, 0); // DHCP
    int rc;
    if (cfg.key[0])
        rc = Wifi_ConnectSecureAP(&targetAp, cfg.key, strlen(cfg.key));
    else
        rc = Wifi_ConnectSecureAP(&targetAp, NULL, 0);

    if (rc == 0)
    {
        set_status("JOINING WI-FI");
        net_enter(NET_ASSOCIATING);
    }
    else
    {
        set_status("BAD WI-FI SETTINGS");
    }
}

static void assoc_step(void)
{
    int st = Wifi_AssocStatus();
    if (st == ASSOCSTATUS_ASSOCIATED)
    {
        struct in_addr gw, mask, dns1, dns2;
        struct in_addr ip = Wifi_GetIPInfo(&gw, &mask, &dns1, &dns2);
        snprintf(net.ip, sizeof(net.ip), "%s", inet_ntoa(ip));
        set_status("WI-FI OK");
        retryDelay = 0;
        net_enter(NET_CONNECTING);
        return;
    }
    if (st == ASSOCSTATUS_CANNOTCONNECT)
    {
        wifi_restart("CAN'T JOIN, RETRYING");
        return;
    }
    if (st == ASSOCSTATUS_ACQUIRINGDHCP)
        set_status("GETTING IP ADDRESS");
    if (frame - stateSince > ASSOC_TIMEOUT)
        wifi_restart("JOIN TIMED OUT");
}

static void on_tcp_connected(void)
{
    tcpInProgress = 0;
    rxLen = 0;
    lastRx = frame;
    lastPing = frame;
    answeredId[0] = '\0';
    set_status("WAITING FOR PC");
    net_enter(NET_ONLINE);
    send_hello();
}

static void connect_step(void)
{
    if (sock < 0)
    {
        if (frame - stateSince < retryDelay)
            return;

        memset(&serverAddr, 0, sizeof(serverAddr));
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(cfg.port);
        if (inet_aton(cfg.server, &serverAddr.sin_addr) == 0)
        {
            set_status("BAD SERVER ADDRESS");
            retryDelay = 10 * FPS;
            stateSince = frame;
            return;
        }

        sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0)
        {
            set_status("NO SOCKET");
            retryDelay = RETRY_DELAY;
            stateSince = frame;
            return;
        }
        int one = 1;
        ioctl(sock, FIONBIO, &one);

        set_status("CONNECTING TO PC");
        tcpInProgress = 1;
        connectStarted = frame;
    }

    // Non-blocking connect: call connect() until it reports success/failure.
    int rc = connect(sock, (struct sockaddr *)&serverAddr, sizeof(serverAddr));
    if (rc == 0 || errno == EISCONN)
    {
        on_tcp_connected();
        return;
    }
    if (errno == EINPROGRESS || errno == EALREADY || errno == EWOULDBLOCK || errno == EAGAIN)
    {
        if (frame - connectStarted > CONNECT_TIMEOUT)
            tcp_retry("NO ANSWER FROM PC");
        return;
    }
    tcp_retry("PC NOT REACHABLE");
}

static void handle_effect(ProtoEffect fx)
{
    switch (fx)
    {
        case FX_DONE:
            chime_play(CHIME_DONE);
            wake();
            break;
        case FX_WAIT:
            chime_play(CHIME_WAIT);
            wake();
            break;
        case FX_ASK:
            chime_play(CHIME_ASK);
            wake();
            break;
        case FX_HELLO:
            chime_play(CHIME_HELLO);
            set_status("LINK OK");
            answeredId[0] = '\0';
            wake();
            break;
        default:
            break;
    }
}

static void process_lines(void)
{
    int start = 0;
    for (int i = 0; i < rxLen; i++)
    {
        if (rxBuf[i] != '\n')
            continue;
        rxBuf[i] = '\0';
        handle_effect(proto_handle_line(&model, &rxBuf[start], frame));
        start = i + 1;
    }
    if (start > 0)
    {
        memmove(rxBuf, rxBuf + start, rxLen - start);
        rxLen -= start;
    }
}

static void online_step(void)
{
    for (int guard = 0; guard < 8 && sock >= 0; guard++)
    {
        int space = (int)sizeof(rxBuf) - rxLen;
        if (space <= 0)
        {
            rxLen = 0; // a line that long is garbage: drop it
            space = sizeof(rxBuf);
        }
        int n = recv(sock, rxBuf + rxLen, space, 0);
        if (n > 0)
        {
            rxLen += n;
            lastRx = frame;
            process_lines();
            continue;
        }
        if (n == 0)
        {
            tcp_retry("PC CLOSED THE LINK");
            return;
        }
        if (errno == EWOULDBLOCK || errno == EAGAIN)
            break;
        tcp_retry("LINK ERROR");
        return;
    }
    if (sock < 0)
        return;

    if (model.who_requested)
    {
        model.who_requested = 0;
        send_hello();
    }
    if (frame - lastPing >= PING_INTERVAL)
    {
        lastPing = frame;
        send_line("PING");
    }
    if (sock >= 0 && frame - lastRx > RX_TIMEOUT)
    {
        tcp_retry("NO DATA FROM PC");
        return;
    }
    if (frame % FPS == 0 && Wifi_AssocStatus() != ASSOCSTATUS_ASSOCIATED)
        wifi_restart("WI-FI LOST");
}

static void net_step(void)
{
    switch (net.state)
    {
        case NET_INIT:
            if (frame - stateSince >= RETRY_DELAY)
                start_scan();
            break;
        case NET_SCANNING:
            scan_step();
            break;
        case NET_ASSOCIATING:
            assoc_step();
            break;
        case NET_CONNECTING:
            connect_step();
            break;
        case NET_ONLINE:
            online_step();
            break;
        case NET_FAILED:
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

static void flash(UiAction act)
{
    flashAction = act;
    flashUntil = frame + FLASH_FRAMES;
}

static void do_action(UiAction act)
{
    if (act == ACT_NONE)
        return;

    if (act == ACT_SCREEN)
    {
        screensOn = !screensOn;
        wake();
        return;
    }

    // With the screens dark, the first press only wakes them up.
    if (!backlightOn)
    {
        wake();
        return;
    }
    wake();

    int online = net.state == NET_ONLINE && model.server_hello;
    if (!online)
        return;

    switch (act)
    {
        case ACT_ALLOW:
        case ACT_DENY:
        case ACT_STOP:
        case ACT_PC:
        {
            if (!model.pr_active || strcmp(answeredId, model.pr_id) == 0)
                return;
            const char *word = act == ACT_ALLOW ? "allow"
                             : act == ACT_DENY  ? "deny"
                             : act == ACT_STOP  ? "stop"
                                                : "pc";
            char line[48];
            snprintf(line, sizeof(line), "ANS %s %s", model.pr_id, word);
            send_line(line);
            snprintf(answeredId, sizeof(answeredId), "%s", model.pr_id);
            answeredUntil = frame + FLASH_FRAMES;
            flash(act);
            chime_play(CHIME_CLICK);
            break;
        }
        case ACT_REMOTE:
        {
            int on = model.rem != 1;
            send_line(on ? "REM 1" : "REM 0");
            model.rem = on;
            flash(act);
            chime_play(CHIME_CLICK);
            break;
        }
        default:
            break;
    }
}

static void handle_input(void)
{
    scanKeys();
    u32 down = keysDown();
    u32 held = keysHeld();

    int lid = (held & KEY_LID) != 0;
    if (lid != lidClosed)
    {
        lidClosed = lid;
        wake();
    }

    if (down & KEY_R)
    {
        chime_set_muted(!chime_muted());
        if (!chime_muted())
            chime_play(CHIME_CLICK);
        wake();
    }

    UiAction act = ACT_NONE;
    if (down & KEY_START)
        act = ACT_SCREEN;
    else if (down & KEY_SELECT)
        act = ACT_REMOTE;
    else if (ui_bottom_mode(&model, &net) == BOTTOM_PERMISSION)
    {
        if (down & KEY_A)
            act = ACT_ALLOW;
        else if (down & KEY_B)
            act = ACT_DENY;
        else if (down & KEY_Y)
            act = ACT_STOP;
        else if (down & KEY_X)
            act = ACT_PC;
    }

    if (act == ACT_NONE && (down & KEY_TOUCH))
    {
        touchPosition tp;
        touchRead(&tp);
        act = ui_hit_test(&model, &net, tp.px, tp.py);
    }

    if (down)
        wake();
    do_action(act);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    setup_video();
    disableSleep(); // keep running when the lid is closed (screens go dark)
    chime_init();
    model_init(&model);

    memset(&net, 0, sizeof(net));
    net.version = APP_VERSION;
    set_status("READING SETTINGS");
    render();

    load_config();
    snprintf(net.ssid, sizeof(net.ssid), "%s", cfg.ssid);
    snprintf(net.server, sizeof(net.server), "%s", cfg.server);
    net.port = cfg.port;
    snprintf(net.config_source, sizeof(net.config_source), "%s", cfg.source);

    set_status("STARTING WI-FI");
    render();

    if (Wifi_InitDefault(INIT_ONLY | WIFI_DS_MODE_ONLY))
    {
        start_scan();
    }
    else
    {
        set_status("WI-FI INIT FAILED");
        net_enter(NET_FAILED);
    }

    while (1)
    {
        cothread_yield_irq(IRQ_VBLANK);
        frame++;

        handle_input();
        net_step();
        chime_step();

        // An answered request disappears once its button has flashed.
        if (answeredUntil && frame >= answeredUntil)
        {
            if (model.pr_active && strcmp(model.pr_id, answeredId) == 0)
                model.pr_active = 0;
            answeredUntil = 0;
        }

        if (model.pr_active)
            lastActivity = frame; // keep the screens on while Claude waits

        apply_backlight();
        if (backlightOn && (frame % 4) == 0)
            render();
    }

    return 0;
}
