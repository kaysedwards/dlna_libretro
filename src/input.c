/*
 * input.c - see input.h.
 */
#include <string.h>

#include "input.h"
#include "options.h"
#include "util.h"

#define NO_BUTTON 0xFFFFu

static unsigned g_map[ACT_COUNT];     /* RETRO_DEVICE_ID_JOYPAD_* per action */
static int      g_down[ACT_COUNT];
static int      g_pressed[ACT_COUNT];
static int      g_blocked[ACT_COUNT]; /* held since a reset: ignore */
static double   g_held[ACT_COUNT];    /* seconds held */
static double   g_last_repeat[ACT_COUNT];
static double   g_dt;

void input_configure(void)
{
   unsigned faces[4];
   int      i, j;

   g_map[ACT_UP]    = RETRO_DEVICE_ID_JOYPAD_UP;
   g_map[ACT_DOWN]  = RETRO_DEVICE_ID_JOYPAD_DOWN;
   g_map[ACT_LEFT]  = RETRO_DEVICE_ID_JOYPAD_LEFT;
   g_map[ACT_RIGHT] = RETRO_DEVICE_ID_JOYPAD_RIGHT;

   /* Face buttons.  If two actions were given the same button, the more
    * important one (open > back > title > info) keeps it and the other is
    * disabled, so one press never does two things. */
   faces[0] = g_cfg.btn_primary;
   faces[1] = g_cfg.btn_back;
   faces[2] = g_cfg.btn_title;
   faces[3] = g_cfg.btn_info;
   for (i = 1; i < 4; i++)
      for (j = 0; j < i; j++)
         if (faces[i] == faces[j] && faces[i] != NO_BUTTON)
         {
            dlna_log(LOG_WARN, "Two actions share one face button; "
                  "the less important one is disabled\n");
            faces[i] = NO_BUTTON;
         }
   g_map[ACT_OK]    = faces[0];
   g_map[ACT_BACK]  = faces[1];
   g_map[ACT_TITLE] = faces[2];
   g_map[ACT_INFO]  = faces[3];

   if (g_cfg.swap_start_select)
   {
      g_map[ACT_ASPECT] = RETRO_DEVICE_ID_JOYPAD_START;
      g_map[ACT_PAUSE]  = RETRO_DEVICE_ID_JOYPAD_SELECT;
   }
   else
   {
      g_map[ACT_ASPECT] = RETRO_DEVICE_ID_JOYPAD_SELECT;
      g_map[ACT_PAUSE]  = RETRO_DEVICE_ID_JOYPAD_START;
   }

   if (g_cfg.swap_shoulders)
   {
      g_map[ACT_SKIP_FWD]  = RETRO_DEVICE_ID_JOYPAD_R;
      g_map[ACT_SKIP_BACK] = RETRO_DEVICE_ID_JOYPAD_L;
      g_map[ACT_MARK_B]    = RETRO_DEVICE_ID_JOYPAD_R2;
      g_map[ACT_MARK_A]    = RETRO_DEVICE_ID_JOYPAD_L2;
   }
   else
   {
      g_map[ACT_SKIP_FWD]  = RETRO_DEVICE_ID_JOYPAD_R2;
      g_map[ACT_SKIP_BACK] = RETRO_DEVICE_ID_JOYPAD_L2;
      g_map[ACT_MARK_B]    = RETRO_DEVICE_ID_JOYPAD_R;
      g_map[ACT_MARK_A]    = RETRO_DEVICE_ID_JOYPAD_L;
   }
}

void input_update(retro_input_state_t state_cb, double dt)
{
   int i;

   g_dt = dt;
   for (i = 0; i < ACT_COUNT; i++)
   {
      int down = 0;
      if (g_map[i] != NO_BUTTON && state_cb)
         down = state_cb(0, RETRO_DEVICE_JOYPAD, 0, g_map[i]) != 0;

      g_pressed[i] = down && !g_down[i] && !g_blocked[i];
      if (!down)
         g_blocked[i] = 0;
      if (down && !g_blocked[i])
         g_held[i] = g_down[i] ? g_held[i] + dt : 0.0;
      else
         g_held[i] = 0.0;
      if (g_pressed[i])
         g_last_repeat[i] = 0.0;
      g_down[i] = down;
   }
}

int input_pressed(action a)
{
   return g_pressed[a];
}

int input_repeat(action a, double delay, double rate)
{
   if (g_pressed[a])
      return 1;
   if (!g_down[a] || g_blocked[a] || g_held[a] < delay)
      return 0;
   /* g_last_repeat tracks the hold time of the previous repeat. */
   if (g_held[a] - g_last_repeat[a] >= rate || g_last_repeat[a] < delay)
   {
      g_last_repeat[a] = g_held[a];
      return 1;
   }
   return 0;
}

void input_reset(void)
{
   int i;
   for (i = 0; i < ACT_COUNT; i++)
   {
      g_blocked[i] = g_down[i];
      g_pressed[i] = 0;
      g_held[i]    = 0.0;
   }
}

void input_describe(retro_environment_t cb)
{
   /* Descriptions follow the default mapping; RetroArch shows them in
    * Quick Menu > Controls. */
   static struct retro_input_descriptor desc[] = {
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,
        "Up (move / subtitle track)" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,
        "Down (move / subtitle track)" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,
        "Left (move / audio track)" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,
        "Right (move / audio track)" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,
        "South face button (default: open / play)" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,
        "East face button (default: back / stop)" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,
        "West face button (default: show title)" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,
        "North face button (default: playback info)" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,
        "Select (default: aspect ratio)" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,
        "Start (default: pause)" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,
        "L1 (default: loop point A)" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,
        "R1 (default: loop point B)" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,
        "L2 (default: seek back / page up)" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,
        "R2 (default: seek forward / page down)" },
      { 0, 0, 0, 0, NULL }
   };
   cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);
}
