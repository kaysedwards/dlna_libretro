/*
 * input.h - turns RetroPad state into high level actions.
 *
 * The button mapping is built from the core options (which face button
 * does what, Start/Select swap, shoulder/trigger swap).  Actions report
 * fresh presses, and optionally auto-repeat while held.
 */
#ifndef DLNA_INPUT_H
#define DLNA_INPUT_H

#include <libretro.h>

typedef enum action
{
   ACT_UP = 0,
   ACT_DOWN,
   ACT_LEFT,
   ACT_RIGHT,
   ACT_OK,          /* primary face button   */
   ACT_BACK,        /* secondary face button */
   ACT_TITLE,       /* tertiary face button  */
   ACT_INFO,        /* quaternary face button */
   ACT_ASPECT,      /* Select (or Start when swapped) */
   ACT_PAUSE,       /* Start (or Select when swapped) */
   ACT_SKIP_FWD,    /* R2 (or R1 when swapped): seek forward / page down */
   ACT_SKIP_BACK,   /* L2 (or L1 when swapped): seek back / page up      */
   ACT_MARK_B,      /* R1 (or R2 when swapped): A-B loop end   */
   ACT_MARK_A,      /* L1 (or L2 when swapped): A-B loop start */
   ACT_COUNT
} action;

/* Rebuild the mapping from g_cfg. */
void input_configure(void);

/* Read the pad.  'dt' is the time since the previous call in seconds. */
void input_update(retro_input_state_t state_cb, double dt);

/* Newly pressed this frame. */
int input_pressed(action a);

/* Newly pressed, or held long enough to auto-repeat.  'delay' is the time
 * before repeating starts and 'rate' the time between repeats (seconds). */
int input_repeat(action a, double delay, double rate);

/* Forget everything that is currently held (e.g. after switching
 * between browser and player so a held button does not leak across). */
void input_reset(void);

/* Input descriptors for the frontend's controls screen. */
void input_describe(retro_environment_t environ_cb);

#endif
