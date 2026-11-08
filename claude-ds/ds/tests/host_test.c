// Host-side tests for the DS app's portable parts (protocol, config, layout).
//
//   make -C tests          # builds and runs on the PC (no DS toolchain needed)
//   ./build/host_test out.json   # also dumps the screens for render_preview.py

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "glyphs.h"
#include "proto.h"
#include "ui.h"

static int failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                  \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static const char *glyph_utf8(int ch)
{
    static const char *map[] = {
        "─", "│", "┌", "┐", "└", "┘", "├", "┤", "┬", "┴", "┼",
        "█", "▀", "▄", "▒", "•", "▶", "♪", "♥", "≋",
    };
    if (ch >= GLYPH_FIRST && ch < GLYPH_FIRST + (int)(sizeof(map) / sizeof(map[0])))
        return map[ch - GLYPH_FIRST];
    if (ch >= GLYPH_FIRST && ch < G_END)
    {
        // Picture tiles (the buddy): show how much of the tile is filled.
        int bits = 0;
        for (int r = 0; r < 8; r++)
            for (int b = 0; b < 8; b++)
                bits += (glyph_bits[ch - GLYPH_FIRST][r] >> b) & 1;
        return bits == 0 ? " " : bits <= 20 ? "░" : bits <= 44 ? "▒" : bits <= 60 ? "▓" : "█";
    }
    return "?";
}

// Row as plain text (glyphs become '#', handy for strstr checks).
static void row_ascii(const Grid *g, int y, char *out)
{
    for (int x = 0; x < GRID_W; x++)
    {
        int ch = g->cell[y][x] & 0xFF;
        out[x] = (ch >= 32 && ch < 127) ? (char)ch : '#';
    }
    out[GRID_W] = '\0';
}

static int row_has(const Grid *g, int y, const char *needle)
{
    char row[GRID_W + 1];
    row_ascii(g, y, row);
    return strstr(row, needle) != NULL;
}

static void print_grid(const char *title, const Grid *g)
{
    printf("+-- %s ", title);
    for (int i = (int)strlen(title); i < 28; i++)
        putchar('-');
    printf("+\n");
    for (int y = 0; y < GRID_H; y++)
    {
        putchar('|');
        for (int x = 0; x < GRID_W; x++)
        {
            int ch = g->cell[y][x] & 0xFF;
            if (ch >= GLYPH_FIRST)
                fputs(glyph_utf8(ch), stdout);
            else
                putchar(ch);
        }
        printf("|\n");
    }
    printf("+--------------------------------+\n");
}

static FILE *dump;
static int dump_count;

static void dump_grid(const Grid *g)
{
    fputc('[', dump);
    for (int y = 0; y < GRID_H; y++)
    {
        fputs(y ? ",[" : "[", dump);
        for (int x = 0; x < GRID_W; x++)
            fprintf(dump, "%s%d", x ? "," : "", g->cell[y][x]);
        fputc(']', dump);
    }
    fputc(']', dump);
}

static void show(const char *name, const Model *m, const NetInfo *n, uint32_t frame,
                 UiAction flash, Grid *top, Grid *bot)
{
    ui_render_top(top, m, n, frame);
    ui_render_bottom(bot, m, n, frame, flash);
    char title[64];
    snprintf(title, sizeof(title), "%s (top)", name);
    print_grid(title, top);
    snprintf(title, sizeof(title), "%s (bottom)", name);
    print_grid(title, bot);
    if (dump)
    {
        fprintf(dump, "%s{\"name\":\"%s\",\"top\":", dump_count++ ? "," : "", name);
        dump_grid(top);
        fputs(",\"bottom\":", dump);
        dump_grid(bot);
        fputc('}', dump);
    }
}

static void feed(Model *m, const char *lines, uint32_t frame)
{
    char buf[512];
    const char *p = lines;
    while (*p)
    {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        if (len >= sizeof(buf))
            len = sizeof(buf) - 1;
        memcpy(buf, p, len);
        buf[len] = '\0';
        proto_handle_line(m, buf, frame);
        p = eol ? eol + 1 : p + len;
    }
}

static void test_config(void)
{
    Config c;
    config_defaults(&c);
    CHECK(strcmp(c.ssid, "CLAUDE-DS") == 0);
    CHECK(strcmp(c.server, "192.168.4.1") == 0);
    CHECK(c.port == 7777);

    int n = config_parse(&c,
                         "# router mode\r\n"
                         "  SSID = my-old-router \r\n"
                         "key=abcde\n"
                         "server=192.168.0.10\n"
                         "port=9000\n"
                         "port=99999\n"
                         "token=s3cret\n"
                         "unknown=1\n"
                         "[section]\n");
    CHECK(n == 5);
    CHECK(strcmp(c.ssid, "my-old-router") == 0);
    CHECK(strcmp(c.key, "abcde") == 0);
    CHECK(strcmp(c.server, "192.168.0.10") == 0);
    CHECK(c.port == 9000);
    CHECK(strcmp(c.token, "s3cret") == 0);
}

static void test_proto(void)
{
    Model m;
    model_init(&m);
    CHECK(m.have_status == 0);
    CHECK(m.ctx == -1);

    CHECK(proto_handle_line(&m, "HI 1 1.0.0", 0) == FX_HELLO);
    CHECK(proto_handle_line(&m, "HI 1 1.0.0", 0) == FX_NONE);
    CHECK(m.server_hello == 1 && m.bridge_proto == 1);

    proto_handle_line(&m, "ST ses=3 wrk=1 wai=2 cpu=30 mem=36 dsk=653 unp=4 ctx=42 h5=13 "
                          "wk=52 rem=1 pend=0 clk=12:41\r", 0);
    CHECK(m.have_status == 1);
    CHECK(m.ses == 3 && m.wrk == 1 && m.wai == 2 && m.cpu == 30 && m.mem == 36);
    CHECK(m.dsk == 653 && m.unp == 4 && m.ctx == 42 && m.h5 == 13 && m.wk == 52);
    CHECK(m.rem == 1);
    CHECK(strcmp(m.clk, "12:41") == 0);

    proto_handle_line(&m, "SC 2 7", 0);
    proto_handle_line(&m, "SI 0 A 12 -1 blog", 0);
    proto_handle_line(&m, "SI 1 W 125 42 setouchi", 0);
    proto_handle_line(&m, "SI 9 W 1 1 ignored", 0);
    CHECK(m.rows_shown == 2 && m.rows_total == 7);
    CHECK(m.rows[0].state == 'A' && strcmp(m.rows[0].name, "blog") == 0);
    CHECK(m.rows[1].age == 125 && m.rows[1].ctx == 42);

    CHECK(proto_handle_line(&m, "EV DONE setouchi", 100) == FX_DONE);
    CHECK(strcmp(m.toast, "DONE: setouchi") == 0);
    CHECK(m.toast_until > 100);
    CHECK(m.log_count == 1 && m.log[0].kind == 'D');
    CHECK(proto_handle_line(&m, "EV WAIT blog permission", 120) == FX_WAIT);
    CHECK(m.log[0].kind == 'W' && m.log[1].kind == 'D');

    CHECK(proto_handle_line(&m, "PR ab12cd 1 Bash blog rm -rf dist && npm run build", 0) == FX_ASK);
    CHECK(m.pr_active && strcmp(m.pr_id, "ab12cd") == 0 && m.pr_more == 1);
    CHECK(strcmp(m.pr_tool, "Bash") == 0 && strcmp(m.pr_session, "blog") == 0);
    CHECK(strcmp(m.pr_detail, "rm -rf dist && npm run build") == 0);
    // Same request again (only "more" changed): no new sound.
    CHECK(proto_handle_line(&m, "PR ab12cd 0 Bash blog rm -rf dist && npm run build", 0) == FX_NONE);
    CHECK(m.pr_more == 0);
    proto_handle_line(&m, "PX zzzzzz", 0);
    CHECK(m.pr_active == 1);
    proto_handle_line(&m, "PX ab12cd", 0);
    CHECK(m.pr_active == 0);

    proto_handle_line(&m, "WHO", 0);
    CHECK(m.who_requested == 1);
    proto_handle_line(&m, "ERR auth", 0);
    CHECK(m.auth_error == 1);

    // Junk must not crash or change state.
    proto_handle_line(&m, "", 0);
    proto_handle_line(&m, "SI", 0);
    proto_handle_line(&m, "PR", 0);
    proto_handle_line(&m, "ST =1 x= ==", 0);
    proto_handle_line(&m, "\x01\x02\xff", 0);

    model_disconnected(&m);
    CHECK(m.server_hello == 0 && m.pr_active == 0);
}

static void base_net(NetInfo *n)
{
    memset(n, 0, sizeof(*n));
    strcpy(n->ssid, "CLAUDE-DS");
    strcpy(n->server, "192.168.4.1");
    n->port = 7777;
    strcpy(n->config_source, "built-in defaults");
    n->version = "1.0.0";
}

static void test_screens(void)
{
    static Grid top, bot;
    Model m;
    NetInfo n;

    // 1. Searching for the ESP32 network.
    model_init(&m);
    base_net(&n);
    n.state = NET_SCANNING;
    strcpy(n.status, "LOOKING FOR WI-FI");
    n.seen_count = 2;
    strcpy(n.seen_ssid[0], "iptime-5G");
    strcpy(n.seen_sec[0], "WPA");
    strcpy(n.seen_ssid[1], "KT_GiGA_2G_1234");
    strcpy(n.seen_sec[1], "WPA");
    show("searching", &m, &n, 30, ACT_NONE, &top, &bot);
    CHECK(row_has(&top, 0, "CLAUDE x DS"));
    CHECK(row_has(&top, 14, "LOOKING FOR WI-FI"));
    CHECK(row_has(&bot, 0, "CONNECTING"));
    CHECK(row_has(&bot, 16, "Plug the ESP32 into the PC."));
    CHECK(ui_bottom_mode(&m, &n) == BOTTOM_CONNECTING);
    CHECK(ui_hit_test(&m, &n, 128, 40) == ACT_NONE);

    // 2. Online, two sessions working / done.
    n.state = NET_ONLINE;
    strcpy(n.ip, "192.168.4.2");
    strcpy(n.status, "LINK OK");
    feed(&m, "HI 1 1.0.0\n"
             "ST ses=3 wrk=1 wai=0 cpu=30 mem=36 dsk=653 unp=4 ctx=42 h5=13 wk=52 rem=0 pend=0 "
             "clk=19:41\n"
             "SC 3 3\n"
             "SI 0 W 125 42 setouchi\n"
             "SI 1 D 40 61 blog\n"
             "SI 2 I 3600 -1 api-server\n"
             "EV DONE blog\n", 10);
    show("working", &m, &n, 200, ACT_NONE, &top, &bot);
    CHECK(row_has(&top, 0, "19:41"));
    CHECK(row_has(&top, 3, "SESSIONS"));
    CHECK(row_has(&top, 4, " 42%"));
    CHECK(row_has(&top, 7, " 30%"));
    CHECK(row_has(&top, 7, "653G"));
    CHECK(row_has(&top, 10, " 52%"));
    CHECK(row_has(&top, 13, "setouchi"));
    CHECK(row_has(&top, 13, "WORK"));
    CHECK(row_has(&top, 13, "2m"));
    CHECK(row_has(&top, 14, "DONE"));
    CHECK(row_has(&top, 15, "1h"));
    CHECK(row_has(&top, 20, "WORKING"));
    CHECK(row_has(&top, 21, "setouchi"));
    CHECK(row_has(&bot, 0, "CONTROL"));
    CHECK(row_has(&bot, 5, "REMOTE APPROVAL: OFF"));
    CHECK(row_has(&bot, 11, "DONE"));
    CHECK(row_has(&bot, 11, "blog"));
    CHECK(ui_bottom_mode(&m, &n) == BOTTOM_CONTROL);
    CHECK(ui_hit_test(&m, &n, 128, 40) == ACT_REMOTE);
    CHECK(ui_hit_test(&m, &n, 128, 150) == ACT_NONE);

    // 3. Remote approval on, a permission request arrives.
    feed(&m, "ST ses=3 wrk=0 wai=1 cpu=12 mem=40 dsk=653 unp=5 ctx=88 h5=71 wk=93 rem=1 pend=1 "
             "clk=19:42\n"
             "SC 3 3\n"
             "SI 0 A 3 61 blog\n"
             "SI 1 D 60 42 setouchi\n"
             "SI 2 I 3660 -1 api-server\n"
             "PR 4f9a1c 1 Bash blog rm -rf dist && npm run build && npx wrangler deploy "
             "--env production --minify\n", 300);
    show("permission", &m, &n, 320, ACT_NONE, &top, &bot);
    CHECK(row_has(&top, 0, "REMOTE"));
    CHECK(row_has(&top, 20, "NEEDS YOU!") || row_has(&top, 20, "NEEDS YOU"));
    CHECK(row_has(&bot, 0, "PERMISSION REQUEST"));
    CHECK(row_has(&bot, 0, "+1"));
    CHECK(row_has(&bot, 2, "blog > Bash"));
    CHECK(row_has(&bot, 4, "rm -rf dist && npm run build"));
    CHECK(row_has(&bot, 14, "ALLOW"));
    CHECK(row_has(&bot, 14, "DENY"));
    CHECK(row_has(&bot, 19, "STOP"));
    CHECK(row_has(&bot, 19, "ASK ON PC"));
    CHECK(ui_bottom_mode(&m, &n) == BOTTOM_PERMISSION);
    CHECK(ui_hit_test(&m, &n, 60, 112) == ACT_ALLOW);
    CHECK(ui_hit_test(&m, &n, 200, 112) == ACT_DENY);
    CHECK(ui_hit_test(&m, &n, 60, 160) == ACT_STOP);
    CHECK(ui_hit_test(&m, &n, 200, 160) == ACT_PC);
    CHECK(ui_hit_test(&m, &n, 128, 112) == ACT_NONE); // gap between buttons
    CHECK(ui_hit_test(&m, &n, 128, 20) == ACT_NONE);

    // 4. Pressed feedback.
    show("allow-pressed", &m, &n, 330, ACT_ALLOW, &top, &bot);
    CHECK((bot.cell[13][1] & 0xFF) == G_SHADE);

    // 5. All done, nothing pending.
    feed(&m, "PX 4f9a1c\n"
             "ST ses=2 wrk=0 wai=0 cpu=4 mem=35 dsk=653 unp=0 ctx=12 h5=20 wk=55 rem=1 pend=0 "
             "clk=19:50\n"
             "SC 2 2\n"
             "SI 0 D 5 12 blog\n"
             "SI 1 I 900 -1 setouchi\n"
             "EV DONE blog\n", 400);
    show("done", &m, &n, 420, ACT_NONE, &top, &bot);
    CHECK(row_has(&top, 20, "ALL DONE!"));
    CHECK(row_has(&top, 12, "DONE: blog"));
    CHECK(row_has(&bot, 5, "REMOTE APPROVAL: ON"));
}

int main(int argc, char *argv[])
{
    if (argc > 1)
    {
        dump = fopen(argv[1], "w");
        if (!dump)
        {
            perror(argv[1]);
            return 1;
        }
        fputs("{\"glyphs\":{", dump);
        for (int i = 0; i < GLYPH_COUNT; i++)
        {
            fprintf(dump, "%s\"%d\":[", i ? "," : "", GLYPH_FIRST + i);
            for (int r = 0; r < 8; r++)
                fprintf(dump, "%s%d", r ? "," : "", glyph_bits[i][r]);
            fputc(']', dump);
        }
        fputs("},\"screens\":[", dump);
    }

    test_config();
    test_proto();
    test_screens();

    if (dump)
    {
        fputs("]}\n", dump);
        fclose(dump);
    }

    if (failures)
    {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    fprintf(stderr, "all host tests passed\n");
    return 0;
}
