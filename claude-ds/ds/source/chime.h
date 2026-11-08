// Little square-wave jingles played on the DS speakers (PSG channels).

#ifndef CLAUDE_DS_CHIME_H
#define CLAUDE_DS_CHIME_H

typedef enum {
    CHIME_DONE,   // a session finished
    CHIME_WAIT,   // a session needs you
    CHIME_ASK,    // permission request on the bottom screen
    CHIME_CLICK,  // button feedback
    CHIME_HELLO,  // connected to the PC
    CHIME_COUNT
} ChimeId;

void chime_init(void);
void chime_play(ChimeId id);
void chime_step(void); // call once per frame
void chime_set_muted(int muted);
int chime_muted(void);

#endif // CLAUDE_DS_CHIME_H
