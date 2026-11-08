// Bridge -> DS line protocol (see ../../PROTOCOL.md) parsed into a Model.
//
// Pure C, no libnds: compiled into the DS app and into the host tests.

#ifndef CLAUDE_DS_PROTO_H
#define CLAUDE_DS_PROTO_H

#include <stdint.h>

#define PROTO_VERSION 1

#define MAX_ROWS 5
#define NAME_LEN 16
#define TOOL_LEN 24
#define DETAIL_LEN 192
#define LOG_LEN 8
#define TOAST_LEN 40

typedef struct {
    char state;       // 'W' working, 'A' needs you, 'D' done, 'I' idle
    int age;          // seconds in this state (when received)
    int ctx;          // context window use in %, -1 = unknown
    char name[NAME_LEN];
} SessionRow;

typedef struct {
    char kind;        // 'D' done, 'W' needs you, 'A' asked (permission), 'I' info
    char clk[6];
    char name[NAME_LEN];
} LogEntry;

typedef struct {
    // "HI": the bridge answered our HELLO
    int server_hello;
    int bridge_proto;
    int auth_error;       // "ERR auth"
    int who_requested;    // "WHO": the bridge wants a new HELLO

    // "ST": status numbers (-1 = unknown)
    int have_status;
    int ses, wrk, wai, cpu, mem, dsk, unp, ctx, h5, wk, rem, pend;
    char clk[6];

    // "SC" / "SI": session rows
    int rows_shown;
    int rows_total;
    SessionRow rows[MAX_ROWS];

    // "PR" / "PX": the permission request shown on the bottom screen
    int pr_active;
    char pr_id[12];
    int pr_more;
    char pr_tool[TOOL_LEN];
    char pr_session[NAME_LEN];
    char pr_detail[DETAIL_LEN];

    // "EV": recent events (newest first) and a short toast
    LogEntry log[LOG_LEN];
    int log_count;
    char toast[TOAST_LEN];
    char toast_kind;
    uint32_t toast_until;     // frame number
    char last_done[NAME_LEN];
    uint32_t last_done_frame;
} Model;

// What the app should do after a line (play a sound, ...).
typedef enum {
    FX_NONE = 0,
    FX_DONE,   // a session finished
    FX_WAIT,   // a session needs you
    FX_ASK,    // a new permission request arrived
    FX_HELLO,  // the bridge answered our HELLO
} ProtoEffect;

void model_init(Model *m);

// Forget everything that only makes sense while connected.
void model_disconnected(Model *m);

// Parses one line (without the trailing newline). `frame` is the current
// frame counter, used for toasts.
ProtoEffect proto_handle_line(Model *m, const char *line, uint32_t frame);

#endif // CLAUDE_DS_PROTO_H
