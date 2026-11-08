#include <nds.h>

#include "chime.h"

typedef struct {
    u16 freq;  // Hz, 0 = rest
    u8 frames; // 0 ends the tune
} Note;

static const Note TUNE_DONE[] = {
    { 1047, 5 }, { 1319, 5 }, { 1568, 5 }, { 2093, 14 }, { 0, 0 },
};
static const Note TUNE_WAIT[] = {
    { 1760, 6 }, { 0, 4 }, { 1760, 6 }, { 0, 4 }, { 2349, 12 }, { 0, 0 },
};
static const Note TUNE_ASK[] = {
    { 880, 5 }, { 1175, 5 }, { 1760, 12 }, { 0, 0 },
};
static const Note TUNE_CLICK[] = {
    { 2637, 2 }, { 0, 0 },
};
static const Note TUNE_HELLO[] = {
    { 784, 4 }, { 1047, 4 }, { 1319, 4 }, { 1568, 10 }, { 0, 0 },
};

static const Note *const TUNES[CHIME_COUNT] = {
    [CHIME_DONE] = TUNE_DONE,
    [CHIME_WAIT] = TUNE_WAIT,
    [CHIME_ASK] = TUNE_ASK,
    [CHIME_CLICK] = TUNE_CLICK,
    [CHIME_HELLO] = TUNE_HELLO,
};

static const Note *cur = NULL;
static int idx = 0;
static int left = 0;
static int channel = -1;
static int muted = 0;

#define VOLUME 72
#define PAN_CENTER 64

void chime_init(void)
{
    soundEnable();
}

static void stop_note(void)
{
    if (channel >= 0)
    {
        soundKill(channel);
        channel = -1;
    }
}

void chime_play(ChimeId id)
{
    if (muted || id >= CHIME_COUNT)
        return;
    stop_note();
    cur = TUNES[id];
    idx = 0;
    left = 0;
}

void chime_step(void)
{
    if (cur == NULL)
        return;
    if (left > 0)
    {
        left--;
        return;
    }
    stop_note();
    const Note *n = &cur[idx];
    if (n->frames == 0)
    {
        cur = NULL;
        return;
    }
    if (n->freq != 0)
        channel = soundPlayPSG(DutyCycle_25, n->freq, VOLUME, PAN_CENTER);
    left = n->frames - 1;
    idx++;
}

void chime_set_muted(int m)
{
    muted = m;
    if (muted)
    {
        stop_note();
        cur = NULL;
    }
}

int chime_muted(void)
{
    return muted;
}
