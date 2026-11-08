#include <stdio.h>
#include <string.h>

#include "glyphs.h"
#include "ui.h"

// ---------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------

static void put(Grid *g, int x, int y, int ch, int color)
{
    if (x < 0 || y < 0 || x >= GRID_W || y >= GRID_H)
        return;
    g->cell[y][x] = (uint16_t)(((color & 0xF) << 8) | (ch & 0xFF));
}

static void clear(Grid *g)
{
    for (int y = 0; y < GRID_H; y++)
        for (int x = 0; x < GRID_W; x++)
            g->cell[y][x] = ' ';
}

static int text(Grid *g, int x, int y, const char *s, int color)
{
    while (s && *s)
        put(g, x++, y, (unsigned char)*s++, color);
    return x;
}

static void textn(Grid *g, int x, int y, const char *s, int maxlen, int color)
{
    for (int i = 0; s && s[i] && i < maxlen; i++)
        put(g, x + i, y, (unsigned char)s[i], color);
}

static void center(Grid *g, int x0, int w, int y, const char *s, int color)
{
    int len = (int)strlen(s);
    if (len > w)
        len = w;
    textn(g, x0 + (w - len) / 2, y, s, len, color);
}

// Text that ends just before column x_end.
static void right(Grid *g, int x_end, int y, const char *s, int color)
{
    text(g, x_end - (int)strlen(s), y, s, color);
}

static void hline(Grid *g, int x0, int x1, int y, int color)
{
    for (int x = x0; x <= x1; x++)
        put(g, x, y, G_H, color);
}

static void fill(Grid *g, int x0, int y0, int x1, int y1, int ch, int color)
{
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            put(g, x, y, ch, color);
}

static void box(Grid *g, int x0, int y0, int x1, int y1, int color)
{
    put(g, x0, y0, G_TL, color);
    put(g, x1, y0, G_TR, color);
    put(g, x0, y1, G_BL, color);
    put(g, x1, y1, G_BR, color);
    for (int x = x0 + 1; x < x1; x++)
    {
        put(g, x, y0, G_H, color);
        put(g, x, y1, G_H, color);
    }
    for (int y = y0 + 1; y < y1; y++)
    {
        put(g, x0, y, G_V, color);
        put(g, x1, y, G_V, color);
    }
}

// Word-wraps `s` into a w x max_lines area. Returns the number of lines used.
static int wrap(Grid *g, int x0, int y0, int w, int max_lines, const char *s, int color)
{
    int line = 0;
    const char *p = s;
    while (*p && line < max_lines)
    {
        int len = (int)strlen(p);
        int take = len < w ? len : w;
        if (len > w)
        {
            for (int i = w; i > w / 2; i--)
            {
                if (p[i] == ' ')
                {
                    take = i;
                    break;
                }
            }
        }
        if (line == max_lines - 1 && len > take)
        {
            textn(g, x0, y0 + line, p, w - 3, color);
            text(g, x0 + w - 3, y0 + line, "...", C_GRAY);
            return max_lines;
        }
        textn(g, x0, y0 + line, p, take, color);
        p += take;
        while (*p == ' ')
            p++;
        line++;
    }
    return line;
}

static void fmt_age(char *out, size_t size, int s)
{
    if (s < 0)
        s = 0;
    if (s < 60)
        snprintf(out, size, "%ds", s);
    else if (s < 3600)
        snprintf(out, size, "%dm", s / 60);
    else if (s < 86400)
        snprintf(out, size, "%dh", s / 3600);
    else
        snprintf(out, size, "%dd", s / 86400);
}

static int pct_color(int v)
{
    if (v >= 80)
        return C_LRED;
    if (v >= 60)
        return C_YELLOW;
    return C_LGREEN;
}

static int link_up(const Model *m, const NetInfo *n)
{
    return n->state == NET_ONLINE && m->server_hello;
}

static int blink(uint32_t frame, int period)
{
    return (frame / (uint32_t)period) % 2;
}

static const char *state_word(char s)
{
    switch (s)
    {
        case 'W': return "WORK";
        case 'A': return "WAIT";
        case 'D': return "DONE";
        default: return "IDLE";
    }
}

static int state_color(char s)
{
    switch (s)
    {
        case 'W': return C_CYAN;
        case 'A': return C_YELLOW;
        case 'D': return C_LGREEN;
        default: return C_GRAY;
    }
}

// ---------------------------------------------------------------------------
// Top screen: dashboard
// ---------------------------------------------------------------------------

#define STAT_X(col) (1 + (col) * 10)
#define STAT_LABEL_Y(row) (3 + (row) * 3)
#define STAT_VALUE_Y(row) (4 + (row) * 3)

static void draw_stat_frame(Grid *g)
{
    for (int r = 0; r <= 3; r++)
    {
        int y = 2 + r * 3;
        for (int x = 0; x <= 30; x++)
        {
            int ch = G_H;
            if (x % 10 == 0)
            {
                if (r == 0)
                    ch = x == 0 ? G_TL : (x == 30 ? G_TR : G_TT);
                else if (r == 3)
                    ch = x == 0 ? G_BL : (x == 30 ? G_BR : G_BT);
                else
                    ch = x == 0 ? G_LT : (x == 30 ? G_RT : G_X);
            }
            put(g, x, y, ch, C_FRAME);
        }
        if (r < 3)
        {
            for (int x = 0; x <= 30; x += 10)
            {
                put(g, x, y + 1, G_V, C_FRAME);
                put(g, x, y + 2, G_V, C_FRAME);
            }
        }
    }
}

static void stat_label(Grid *g, int col, int row, const char *label)
{
    center(g, STAT_X(col), 9, STAT_LABEL_Y(row), label, C_GRAY);
}

static void stat_value(Grid *g, int col, int row, const char *s, int color)
{
    center(g, STAT_X(col), 9, STAT_VALUE_Y(row), s, color);
}

static void stat_int(Grid *g, int col, int row, int v, int color, int zero_color)
{
    char buf[16];
    if (v < 0)
    {
        stat_value(g, col, row, "--", C_GRAY);
        return;
    }
    snprintf(buf, sizeof(buf), "%d", v);
    stat_value(g, col, row, buf, v == 0 ? zero_color : color);
}

// "  42% ##.." : number plus a 4-step bar.
static void stat_pct(Grid *g, int col, int row, int v)
{
    int x0 = STAT_X(col);
    int y = STAT_VALUE_Y(row);
    if (v < 0)
    {
        center(g, x0, 9, y, "--", C_GRAY);
        return;
    }
    if (v > 999)
        v = 999;
    char buf[8];
    snprintf(buf, sizeof(buf), "%3d%%", v);
    int color = pct_color(v);
    text(g, x0, y, buf, color);
    int filled = (v + 12) / 25;
    if (filled > 4)
        filled = 4;
    for (int i = 0; i < 4; i++)
    {
        if (i < filled)
            put(g, x0 + 5 + i, y, G_FULL, color);
        else
            put(g, x0 + 5 + i, y, G_SHADE, C_FRAME);
    }
}

static void draw_stats(Grid *g, const Model *m, uint32_t frame)
{
    draw_stat_frame(g);

    stat_label(g, 0, 0, "SESSIONS");
    stat_label(g, 1, 0, "WAITING");
    stat_label(g, 2, 0, "CONTEXT");
    stat_label(g, 0, 1, "CPU");
    stat_label(g, 1, 1, "MEMORY");
    stat_label(g, 2, 1, "DISK FREE");
    stat_label(g, 0, 2, "5H LIMIT");
    stat_label(g, 1, 2, "WEEK");
    stat_label(g, 2, 2, "UNPUSHED");

    if (!m->have_status)
    {
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++)
                stat_value(g, c, r, "--", C_GRAY);
        return;
    }

    stat_int(g, 0, 0, m->ses, m->wrk > 0 ? C_CYAN : C_WHITE, C_GRAY);
    stat_int(g, 1, 0, m->wai, blink(frame, 20) ? C_YELLOW : C_LRED, C_GRAY);
    stat_pct(g, 2, 0, m->ctx);
    stat_pct(g, 0, 1, m->cpu);
    stat_pct(g, 1, 1, m->mem);

    if (m->dsk < 0)
    {
        stat_value(g, 2, 1, "--", C_GRAY);
    }
    else
    {
        char buf[16];
        if (m->dsk >= 1000)
            snprintf(buf, sizeof(buf), "%d.%dT", m->dsk / 1000, (m->dsk % 1000) / 100);
        else
            snprintf(buf, sizeof(buf), "%dG", m->dsk);
        stat_value(g, 2, 1, buf, m->dsk < 10 ? C_LRED : (m->dsk < 50 ? C_YELLOW : C_WHITE));
    }

    stat_pct(g, 0, 2, m->h5);
    stat_pct(g, 1, 2, m->wk);
    stat_int(g, 2, 2, m->unp, C_YELLOW, C_GRAY);
}

static void draw_sessions(Grid *g, const Model *m, const NetInfo *n, uint32_t frame)
{
    if (!link_up(m, n))
    {
        center(g, 0, 32, 14, n->status, C_YELLOW);
        center(g, 0, 32, 15, "(details on the bottom screen)", C_GRAY);
        return;
    }
    if (m->rows_shown == 0)
    {
        center(g, 0, 32, 15, "no Claude Code sessions yet", C_GRAY);
        return;
    }

    for (int i = 0; i < m->rows_shown && i < MAX_ROWS; i++)
    {
        const SessionRow *r = &m->rows[i];
        int y = 13 + i;
        int color = state_color(r->state);

        int icon = '.';
        if (r->state == 'W')
            icon = "|/-\\"[(frame / 8) % 4];
        else if (r->state == 'A')
            icon = blink(frame, 20) ? '!' : ' ';
        else if (r->state == 'D')
            icon = G_DOT;
        put(g, 1, y, icon, color);

        textn(g, 3, y, r->name, 14, r->state == 'I' ? C_LGRAY : C_WHITE);
        if (r->state != 'A' || blink(frame, 20))
            text(g, 18, y, state_word(r->state), color);

        char buf[16];
        fmt_age(buf, sizeof(buf), r->age);
        right(g, 27, y, buf, C_GRAY);
        if (r->ctx >= 0)
        {
            snprintf(buf, sizeof(buf), "%d%%", r->ctx);
            right(g, 32, y, buf, pct_color(r->ctx));
        }
    }
    if (m->rows_total > m->rows_shown)
    {
        char buf[24];
        snprintf(buf, sizeof(buf), "+%d more", m->rows_total - m->rows_shown);
        right(g, 32, 18, buf, C_GRAY);
    }
}

typedef enum { MOOD_OFF, MOOD_WAIT, MOOD_DONE, MOOD_WORK, MOOD_IDLE } Mood;

static const char *first_row_named(const Model *m, char state)
{
    for (int i = 0; i < m->rows_shown && i < MAX_ROWS; i++)
        if (m->rows[i].state == state)
            return m->rows[i].name;
    return "";
}

static Mood mood_of(const Model *m, const NetInfo *n, uint32_t frame, const char **who)
{
    *who = "";
    if (!link_up(m, n))
        return MOOD_OFF;
    if (m->pr_active)
    {
        *who = m->pr_session;
        return MOOD_WAIT;
    }
    if (m->wai > 0)
    {
        *who = first_row_named(m, 'A');
        return MOOD_WAIT;
    }
    if (m->wrk > 0)
    {
        *who = first_row_named(m, 'W');
        return MOOD_WORK;
    }
    if (m->last_done[0] && frame - m->last_done_frame < 60 * 60)
    {
        *who = m->last_done;
        return MOOD_DONE;
    }
    return MOOD_IDLE;
}

// The buddy is 5x5 tiles of real pixel art (see tools/gen_buddy.py); only the
// two eye tiles change with the mood. Everything is drawn in one color.
static void draw_buddy(Grid *g, int x0, int y0, int color, int eyes)
{
    for (int ty = 0; ty < BUDDY_TILES; ty++)
    {
        for (int tx = 0; tx < BUDDY_TILES; tx++)
        {
            int ch = G_BUDDY0 + ty * BUDDY_TILES + tx;
            if (ty == 2 && tx == 1)
                ch = G_EYES0 + eyes * 2;
            else if (ty == 2 && tx == 3)
                ch = G_EYES0 + eyes * 2 + 1;
            put(g, x0 + tx, y0 + ty, ch, color);
        }
    }
}

static void draw_mascot(Grid *g, const Model *m, const NetInfo *n, uint32_t frame)
{
    const char *who;
    Mood mood = mood_of(m, n, frame, &who);

    int body = C_LBLUE, eyes = EYES_CLOSED, bounce = 0;
    const char *line1 = "zzz...";
    int line1_color = C_GRAY;
    const char *line2 = "all quiet";
    char dots[20];

    switch (mood)
    {
        case MOOD_OFF:
            body = C_GRAY;
            eyes = EYES_DEAD;
            line1 = "NO LINK";
            line1_color = C_LRED;
            line2 = n->status;
            break;
        case MOOD_WAIT:
            body = C_YELLOW;
            eyes = EYES_SURPRISED;
            line1 = "NEEDS YOU!";
            line1_color = blink(frame, 20) ? C_YELLOW : C_WHITE;
            line2 = who;
            break;
        case MOOD_WORK:
            body = C_ORANGE;
            eyes = EYES_OPEN;
            bounce = blink(frame, 30);
            snprintf(dots, sizeof(dots), "WORKING%.*s", (int)((frame / 20) % 4), "...");
            line1 = dots;
            line1_color = C_CYAN;
            line2 = who;
            break;
        case MOOD_DONE:
            body = C_LGREEN;
            eyes = EYES_HAPPY;
            line1 = "ALL DONE!";
            line1_color = C_LGREEN;
            line2 = who;
            break;
        case MOOD_IDLE:
        default:
            break;
    }

    // Blink every few seconds.
    if ((mood == MOOD_WORK || mood == MOOD_WAIT) && (frame % 240) < 8)
        eyes = EYES_CLOSED;

    draw_buddy(g, 1, 19 - bounce, body, eyes);

    box(g, 8, 19, 31, 22, C_FRAME);
    put(g, 7, 20, '<', C_FRAME);
    textn(g, 10, 20, line1, 21, line1_color);
    textn(g, 10, 21, line2 ? line2 : "", 21, C_WHITE);

    if (mood == MOOD_IDLE)
        put(g, 6 + (int)((frame / 30) % 2), 18 - (int)((frame / 30) % 2), 'z', C_GRAY);
}

void ui_render_top(Grid *g, const Model *m, const NetInfo *n, uint32_t frame)
{
    clear(g);

    text(g, 1, 0, "CLAUDE", C_ORANGE);
    text(g, 8, 0, "x", C_GRAY);
    text(g, 10, 0, "DS", C_WHITE);
    if (n->muted)
        text(g, 13, 0, "MUTE", C_GRAY);
    if (link_up(m, n) && m->rem == 1)
        text(g, 19, 0, "REMOTE", C_LGREEN);
    text(g, 27, 0, m->have_status ? m->clk : "--:--", C_WHITE);
    hline(g, 0, 31, 1, C_FRAME);

    draw_stats(g, m, frame);

    // Toast between the stats and the session list.
    if (m->toast[0] && frame < m->toast_until)
    {
        int color = m->toast_kind == 'D' ? C_LGREEN : C_YELLOW;
        center(g, 0, 32, 12, m->toast, blink(frame, 15) ? color : C_WHITE);
    }

    draw_sessions(g, m, n, frame);
    draw_mascot(g, m, n, frame);
}

// ---------------------------------------------------------------------------
// Bottom screen: controls
// ---------------------------------------------------------------------------

typedef struct {
    UiAction act;
    int x0, y0, x1, y1;
    const char *label;
    const char *key;
    int color;
} Button;

static const Button PR_BUTTONS[] = {
    { ACT_ALLOW, 0, 12, 14, 16, "ALLOW", "(A)", C_LGREEN },
    { ACT_DENY, 17, 12, 31, 16, "DENY", "(B)", C_LRED },
    { ACT_STOP, 0, 18, 14, 21, "STOP", "(Y)", C_ORANGE },
    { ACT_PC, 17, 18, 31, 21, "ASK ON PC", "(X)", C_CYAN },
};

#define REMOTE_X0 1
#define REMOTE_Y0 3
#define REMOTE_X1 30
#define REMOTE_Y1 7

static void draw_button(Grid *g, const Button *b, int flash)
{
    box(g, b->x0, b->y0, b->x1, b->y1, b->color);
    int w = b->x1 - b->x0 - 1;
    int h = b->y1 - b->y0 - 1;
    if (flash)
        fill(g, b->x0 + 1, b->y0 + 1, b->x1 - 1, b->y1 - 1, G_SHADE, b->color);
    int lines = b->key ? 2 : 1;
    int y = b->y0 + 1 + (h - lines + 1) / 2;
    center(g, b->x0 + 1, w, y, b->label, flash ? C_WHITE : b->color);
    if (b->key)
        center(g, b->x0 + 1, w, y + 1, b->key, flash ? C_WHITE : C_GRAY);
}

static int inside(const Button *b, int cx, int cy)
{
    return cx >= b->x0 && cx <= b->x1 && cy >= b->y0 && cy <= b->y1;
}

BottomMode ui_bottom_mode(const Model *m, const NetInfo *n)
{
    if (!link_up(m, n))
        return BOTTOM_CONNECTING;
    if (m->pr_active)
        return BOTTOM_PERMISSION;
    return BOTTOM_CONTROL;
}

static void render_permission(Grid *g, const Model *m, uint32_t frame, UiAction flash)
{
    text(g, 1, 0, "! PERMISSION REQUEST", blink(frame, 20) ? C_YELLOW : C_WHITE);
    if (m->pr_more > 0)
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "+%d", m->pr_more);
        right(g, 31, 0, buf, C_GRAY);
    }
    hline(g, 0, 31, 1, C_FRAME);

    int x = text(g, 1, 2, m->pr_session, C_ORANGE);
    x = text(g, x, 2, " > ", C_GRAY);
    textn(g, x, 2, m->pr_tool, GRID_W - 1 - x, C_WHITE);

    wrap(g, 1, 4, 30, 7, m->pr_detail, C_LGRAY);

    for (size_t i = 0; i < sizeof(PR_BUTTONS) / sizeof(PR_BUTTONS[0]); i++)
        draw_button(g, &PR_BUTTONS[i], flash == PR_BUTTONS[i].act);

    center(g, 0, 32, 23, "tap a button or press A/B/Y/X", C_GRAY);
}

static void render_control(Grid *g, const Model *m, const NetInfo *n, UiAction flash)
{
    text(g, 1, 0, "CONTROL", C_ORANGE);
    right(g, 31, 0, "LINK OK", C_LGREEN);
    hline(g, 0, 31, 1, C_FRAME);

    int on = m->rem == 1;
    Button rb = { ACT_REMOTE, REMOTE_X0, REMOTE_Y0, REMOTE_X1, REMOTE_Y1,
                  on ? "REMOTE APPROVAL: ON" : "REMOTE APPROVAL: OFF",
                  "tap here or press SELECT", on ? C_LGREEN : C_GRAY };
    draw_button(g, &rb, flash == ACT_REMOTE);
    center(g, 0, 32, 8,
           on ? "permission prompts come here" : "permission prompts stay on PC",
           on ? C_LGREEN : C_GRAY);

    text(g, 1, 10, "RECENT", C_GRAY);
    hline(g, 8, 31, 10, C_FRAME);
    if (m->log_count == 0)
        text(g, 1, 11, "nothing yet", C_GRAY);
    for (int i = 0; i < m->log_count && i < LOG_LEN; i++)
    {
        const LogEntry *e = &m->log[i];
        int y = 11 + i;
        const char *word = "INFO";
        int color = C_GRAY;
        switch (e->kind)
        {
            case 'D': word = "DONE"; color = C_LGREEN; break;
            case 'W': word = "WAIT"; color = C_YELLOW; break;
            case 'A': word = "ASK"; color = C_ORANGE; break;
            default: break;
        }
        text(g, 1, y, e->clk, C_GRAY);
        text(g, 7, y, word, color);
        textn(g, 12, y, e->name, 19, i == 0 ? C_WHITE : C_LGRAY);
    }

    hline(g, 0, 31, 20, C_FRAME);
    put(g, 1, 21, G_WIFI, C_LGREEN);
    textn(g, 3, 21, n->ssid, 14, C_WHITE);
    right(g, 32, 21, n->ip, C_GRAY);
    text(g, 1, 22, "SELECT remote  START screen", C_GRAY);
    char ver[24];
    snprintf(ver, sizeof(ver), "claude-ds %s", n->version ? n->version : "");
    right(g, 32, 23, ver, C_FRAME);
}

static void render_connecting(Grid *g, const Model *m, const NetInfo *n, uint32_t frame)
{
    text(g, 1, 0, "CONNECTING", C_ORANGE);
    textn(g, 11, 0, "...", (int)((frame / 20) % 4), C_ORANGE);
    hline(g, 0, 31, 1, C_FRAME);

    text(g, 1, 3, "WI-FI ", C_GRAY);
    textn(g, 7, 3, n->ssid, 24, C_WHITE);
    text(g, 1, 4, "STATE ", C_GRAY);
    textn(g, 7, 4, n->status, 24, C_YELLOW);
    text(g, 1, 5, "IP    ", C_GRAY);
    textn(g, 7, 5, n->ip[0] ? n->ip : "-", 24, C_WHITE);

    text(g, 1, 7, "NETWORKS IN RANGE", C_GRAY);
    if (n->seen_count == 0)
        text(g, 2, 8, "(none yet)", C_GRAY);
    for (int i = 0; i < n->seen_count && i < SEEN_MAX; i++)
    {
        int is_target = strcmp(n->seen_ssid[i], n->ssid) == 0;
        int y = 8 + i;
        put(g, 1, y, is_target ? G_ARROW : ' ', C_LGREEN);
        textn(g, 3, y, n->seen_ssid[i], 20, is_target ? C_LGREEN : C_LGRAY);
        right(g, 31, y, n->seen_sec[i], is_target ? C_LGREEN : C_GRAY);
    }

    const char *tips[4] = { NULL, NULL, NULL, NULL };
    if (n->state == NET_FAILED)
    {
        tips[0] = "Wi-Fi could not start.";
        tips[1] = "Turn the DS off and on.";
    }
    else if (n->state == NET_ONLINE && m->auth_error)
    {
        tips[0] = "The PC refused our token.";
        tips[1] = "Check token= in claude-ds.ini";
        tips[2] = "and --token on the PC.";
    }
    else if (n->state == NET_ONLINE)
    {
        tips[0] = "Wi-Fi OK! Waiting for the PC";
        tips[1] = "program. On the PC run:";
        tips[2] = "python claude_ds_bridge.py";
    }
    else if (n->state == NET_CONNECTING)
    {
        tips[0] = "Wi-Fi OK. Connecting to";
        tips[1] = n->server;
    }
    else if (n->target_seen && !n->target_compatible)
    {
        tips[0] = "Network found, but the DS";
        tips[1] = "can't use it (needs open/WEP";
        tips[2] = "and 802.11b).";
    }
    else if (!n->target_seen)
    {
        tips[0] = "Plug the ESP32 into the PC.";
        tips[1] = "Its Wi-Fi network is called";
        tips[2] = n->ssid;
    }
    for (int i = 0; i < 4; i++)
        if (tips[i])
            textn(g, 1, 16 + i, tips[i], 30, i == 2 && !n->target_seen ? C_WHITE : C_LGRAY);

    textn(g, 1, 22, n->config_source, 30, C_FRAME);
    char ver[24];
    snprintf(ver, sizeof(ver), "claude-ds %s", n->version ? n->version : "");
    right(g, 32, 23, ver, C_FRAME);
}

void ui_render_bottom(Grid *g, const Model *m, const NetInfo *n, uint32_t frame, UiAction flash)
{
    clear(g);
    switch (ui_bottom_mode(m, n))
    {
        case BOTTOM_PERMISSION:
            render_permission(g, m, frame, flash);
            break;
        case BOTTOM_CONTROL:
            render_control(g, m, n, flash);
            break;
        case BOTTOM_CONNECTING:
        default:
            render_connecting(g, m, n, frame);
            break;
    }
}

UiAction ui_hit_test(const Model *m, const NetInfo *n, int px, int py)
{
    int cx = px / 8;
    int cy = py / 8;
    BottomMode mode = ui_bottom_mode(m, n);

    if (mode == BOTTOM_PERMISSION)
    {
        for (size_t i = 0; i < sizeof(PR_BUTTONS) / sizeof(PR_BUTTONS[0]); i++)
            if (inside(&PR_BUTTONS[i], cx, cy))
                return PR_BUTTONS[i].act;
    }
    else if (mode == BOTTOM_CONTROL)
    {
        Button rb = { ACT_REMOTE, REMOTE_X0, REMOTE_Y0, REMOTE_X1, REMOTE_Y1, "", NULL, 0 };
        if (inside(&rb, cx, cy))
            return ACT_REMOTE;
    }
    return ACT_NONE;
}
