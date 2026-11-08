#include <stdlib.h>
#include <string.h>

#include "config.h"

void config_defaults(Config *c)
{
    memset(c, 0, sizeof(*c));
    strcpy(c->ssid, "CLAUDE-DS");
    strcpy(c->server, "192.168.4.1"); // the ESP32's own address
    c->port = 7777;
    strcpy(c->source, "built-in defaults");
}

static void set_str(char *dst, size_t size, const char *src, size_t len)
{
    if (len >= size)
        len = size - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static int is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

int config_parse(Config *c, const char *text)
{
    int applied = 0;
    const char *p = text;

    while (p && *p)
    {
        const char *line = p;
        const char *eol = strchr(p, '\n');
        size_t line_len = eol ? (size_t)(eol - p) : strlen(p);
        p = eol ? eol + 1 : NULL;

        // Trim
        while (line_len > 0 && is_space(*line))
        {
            line++;
            line_len--;
        }
        while (line_len > 0 && is_space(line[line_len - 1]))
            line_len--;
        if (line_len == 0 || line[0] == '#' || line[0] == ';' || line[0] == '[')
            continue;

        const char *eq = memchr(line, '=', line_len);
        if (!eq)
            continue;

        size_t key_len = (size_t)(eq - line);
        while (key_len > 0 && is_space(line[key_len - 1]))
            key_len--;
        const char *val = eq + 1;
        size_t val_len = line_len - (size_t)(val - line);
        while (val_len > 0 && is_space(*val))
        {
            val++;
            val_len--;
        }

        char key[16];
        set_str(key, sizeof(key), line, key_len);
        for (char *k = key; *k; k++)
        {
            if (*k >= 'A' && *k <= 'Z')
                *k = (char)(*k - 'A' + 'a');
        }

        if (strcmp(key, "ssid") == 0)
            set_str(c->ssid, sizeof(c->ssid), val, val_len);
        else if (strcmp(key, "key") == 0 || strcmp(key, "wep") == 0)
            set_str(c->key, sizeof(c->key), val, val_len);
        else if (strcmp(key, "server") == 0)
            set_str(c->server, sizeof(c->server), val, val_len);
        else if (strcmp(key, "token") == 0)
            set_str(c->token, sizeof(c->token), val, val_len);
        else if (strcmp(key, "port") == 0)
        {
            char num[8];
            set_str(num, sizeof(num), val, val_len);
            int port = atoi(num);
            if (port <= 0 || port > 65535)
                continue;
            c->port = port;
        }
        else
            continue;
        applied++;
    }
    return applied;
}
