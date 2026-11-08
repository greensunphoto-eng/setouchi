#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "proto.h"

#define TOAST_FRAMES (60 * 6)

static void copy_str(char *dst, size_t size, const char *src)
{
    if (size == 0)
        return;
    size_t i = 0;
    for (; src && src[i] && i + 1 < size; i++)
    {
        char c = src[i];
        dst[i] = (c >= 32 && c < 127) ? c : '?';
    }
    dst[i] = '\0';
}

// Splits `line` in place into at most `max` space-separated words. The last
// word takes the rest of the line when `rest_last` is set.
static int split_words(char *line, char **words, int max, int rest_last)
{
    int n = 0;
    char *p = line;
    while (*p && n < max)
    {
        while (*p == ' ')
            p++;
        if (!*p)
            break;
        words[n++] = p;
        if (rest_last && n == max)
            break;
        while (*p && *p != ' ')
            p++;
        if (*p)
            *p++ = '\0';
    }
    return n;
}

static int to_int(const char *s, int fallback)
{
    if (!s || !*s)
        return fallback;
    char *end;
    long v = strtol(s, &end, 10);
    if (end == s)
        return fallback;
    if (v > 1000000)
        v = 1000000;
    if (v < -1000000)
        v = -1000000;
    return (int)v;
}

void model_init(Model *m)
{
    memset(m, 0, sizeof(*m));
    m->ses = m->wrk = m->wai = m->cpu = m->mem = m->dsk = -1;
    m->unp = m->ctx = m->h5 = m->wk = -1;
    m->rem = 0;
    m->pend = 0;
    strcpy(m->clk, "--:--");
}

void model_disconnected(Model *m)
{
    m->server_hello = 0;
    m->who_requested = 0;
    m->pr_active = 0;
}

static void push_log(Model *m, char kind, const char *name)
{
    if (m->log_count < LOG_LEN)
        m->log_count++;
    memmove(&m->log[1], &m->log[0], sizeof(LogEntry) * (LOG_LEN - 1));
    m->log[0].kind = kind;
    copy_str(m->log[0].clk, sizeof(m->log[0].clk), m->clk);
    copy_str(m->log[0].name, sizeof(m->log[0].name), name);
}

static void set_toast(Model *m, char kind, const char *text, uint32_t frame)
{
    copy_str(m->toast, sizeof(m->toast), text);
    m->toast_kind = kind;
    m->toast_until = frame + TOAST_FRAMES;
}

static void parse_status(Model *m, char **w, int n)
{
    static const struct { const char *key; size_t offset; } keys[] = {
        { "ses", offsetof(Model, ses) }, { "wrk", offsetof(Model, wrk) },
        { "wai", offsetof(Model, wai) }, { "cpu", offsetof(Model, cpu) },
        { "mem", offsetof(Model, mem) }, { "dsk", offsetof(Model, dsk) },
        { "unp", offsetof(Model, unp) }, { "ctx", offsetof(Model, ctx) },
        { "h5", offsetof(Model, h5) },   { "wk", offsetof(Model, wk) },
        { "rem", offsetof(Model, rem) }, { "pend", offsetof(Model, pend) },
    };

    for (int i = 1; i < n; i++)
    {
        char *eq = strchr(w[i], '=');
        if (!eq)
            continue;
        *eq = '\0';
        const char *key = w[i];
        const char *val = eq + 1;
        if (strcmp(key, "clk") == 0)
        {
            copy_str(m->clk, sizeof(m->clk), val);
            continue;
        }
        for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++)
        {
            if (strcmp(key, keys[k].key) == 0)
            {
                int *field = (int *)((char *)m + keys[k].offset);
                *field = to_int(val, -1);
                break;
            }
        }
    }
    m->have_status = 1;
}

ProtoEffect proto_handle_line(Model *m, const char *line, uint32_t frame)
{
    // Copy without trailing CR/LF/spaces; anything unprintable becomes '?'.
    char buf[320];
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n' || line[len - 1] == ' '))
        len--;
    if (len >= sizeof(buf))
        len = sizeof(buf) - 1;
    for (size_t i = 0; i < len; i++)
    {
        char c = line[i];
        buf[i] = (c >= 32 && c < 127) ? c : '?';
    }
    buf[len] = '\0';

    char *w[16];

    if (strncmp(buf, "PR ", 3) == 0)
    {
        // PR <id> <more> <tool> <session> <detail...>
        int n = split_words(buf, w, 6, 1);
        if (n < 5)
            return FX_NONE;
        int is_new = !m->pr_active || strcmp(m->pr_id, w[1]) != 0;
        copy_str(m->pr_id, sizeof(m->pr_id), w[1]);
        m->pr_more = to_int(w[2], 0);
        copy_str(m->pr_tool, sizeof(m->pr_tool), w[3]);
        copy_str(m->pr_session, sizeof(m->pr_session), w[4]);
        copy_str(m->pr_detail, sizeof(m->pr_detail), n > 5 ? w[5] : "");
        m->pr_active = 1;
        if (is_new)
        {
            push_log(m, 'A', m->pr_session);
            return FX_ASK;
        }
        return FX_NONE;
    }

    int n = split_words(buf, w, 16, 0);
    if (n == 0)
        return FX_NONE;

    if (strcmp(w[0], "ST") == 0)
    {
        parse_status(m, w, n);
        return FX_NONE;
    }

    if (strcmp(w[0], "SC") == 0 && n >= 3)
    {
        int shown = to_int(w[1], 0);
        if (shown < 0)
            shown = 0;
        if (shown > MAX_ROWS)
            shown = MAX_ROWS;
        m->rows_shown = shown;
        m->rows_total = to_int(w[2], shown);
        return FX_NONE;
    }

    if (strcmp(w[0], "SI") == 0 && n >= 6)
    {
        int idx = to_int(w[1], -1);
        if (idx < 0 || idx >= MAX_ROWS)
            return FX_NONE;
        SessionRow *row = &m->rows[idx];
        row->state = w[2][0];
        row->age = to_int(w[3], 0);
        row->ctx = to_int(w[4], -1);
        copy_str(row->name, sizeof(row->name), w[5]);
        return FX_NONE;
    }

    if (strcmp(w[0], "EV") == 0 && n >= 3)
    {
        char text[TOAST_LEN];
        if (strcmp(w[1], "DONE") == 0)
        {
            push_log(m, 'D', w[2]);
            snprintf(text, sizeof(text), "DONE: %s", w[2]);
            set_toast(m, 'D', text, frame);
            copy_str(m->last_done, sizeof(m->last_done), w[2]);
            m->last_done_frame = frame;
            return FX_DONE;
        }
        if (strcmp(w[1], "WAIT") == 0)
        {
            push_log(m, 'W', w[2]);
            snprintf(text, sizeof(text), "%s NEEDS YOU", w[2]);
            set_toast(m, 'W', text, frame);
            return FX_WAIT;
        }
        push_log(m, 'I', w[2]);
        return FX_NONE;
    }

    if (strcmp(w[0], "PX") == 0 && n >= 2)
    {
        if (m->pr_active && strcmp(m->pr_id, w[1]) == 0)
            m->pr_active = 0;
        return FX_NONE;
    }

    if (strcmp(w[0], "HI") == 0)
    {
        int first = !m->server_hello;
        m->server_hello = 1;
        m->auth_error = 0;
        m->bridge_proto = n >= 2 ? to_int(w[1], 0) : 0;
        return first ? FX_HELLO : FX_NONE;
    }

    if (strcmp(w[0], "WHO") == 0)
    {
        m->who_requested = 1;
        return FX_NONE;
    }

    if (strcmp(w[0], "ERR") == 0)
    {
        if (n >= 2 && strcmp(w[1], "auth") == 0)
            m->auth_error = 1;
        return FX_NONE;
    }

    return FX_NONE; // PONG and unknown lines
}
