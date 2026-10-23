/*
 * options.h - core options ("Quick Menu > Core Options" in RetroArch).
 *
 * Every user-tweakable setting lives in the global 'g_cfg'.  options_init()
 * registers the option definitions with the frontend (core options v2 with
 * categories when available, older formats otherwise) and options_update()
 * re-reads them, reporting which groups of settings changed.
 */
#ifndef DLNA_OPTIONS_H
#define DLNA_OPTIONS_H

#include <libretro.h>

/* The largest canvas we ever use; sent to the frontend as max geometry. */
#define CANVAS_MAX_WIDTH  3840
#define CANVAS_MAX_HEIGHT 2160

/* Aspect ratio modes cycled with the Select button during playback. */
typedef enum aspect_mode
{
   ASPECT_MEDIA = 0,   /* whatever the media says              */
   ASPECT_FULL,        /* stretch to fill the whole screen     */
   ASPECT_16_9,
   ASPECT_4_3,
   ASPECT_5_4,
   ASPECT_COUNT
} aspect_mode;

typedef enum subs_default
{
   SUBS_AUTO = 0,      /* tracks flagged "default" or "forced" */
   SUBS_OFF,
   SUBS_FIRST
} subs_default;

typedef enum play_loop
{
   LOOP_SINGLE = 0,   /* "Individual Selection": play one entry, exit  */
   LOOP_ONCE,         /* "Playlist Once": every entry once, exit       */
   LOOP_REPEAT_ONE,   /* "Repeat Entry": one entry forever             */
   LOOP_REPEAT_ALL    /* "Repeat All": the whole list forever          */
} play_loop;

typedef struct dlna_config
{
   int          width, height;     /* canvas size in pixels            */
   int          font_size;         /* UI font size in pixels           */
   int          subtitle_scale;    /* percent                          */
   subs_default subtitles;

   /* Face buttons, as RETRO_DEVICE_ID_JOYPAD_* ids. */
   unsigned     btn_primary;       /* open folder / play                */
   unsigned     btn_back;          /* parent folder / stop              */
   unsigned     btn_title;         /* show title during playback        */
   unsigned     btn_info;          /* playback information panel        */

   int          dpad_subs_vertical; /* 1: Up/Down subtitles, L/R audio  */
   int          swap_start_select;
   int          swap_shoulders;    /* swap L1<->L2 and R1<->R2          */

   int          play_random;       /* playlist order: shuffled          */
   play_loop    play_loop;
   int          picture_seconds;   /* how long a playlist picture shows */

   int          sort_descending;   /* Z-A instead of A-Z                */
   int          folders_first;

   uint32_t     title_border, title_text, title_bg;
   uint32_t     info_border, info_text, info_bg;
   uint32_t     sel_border, sel_bg;  /* thumbnail boxes                  */
   uint32_t     nav_bg;            /* browser background                */
   uint32_t     play_bg;           /* behind media during playback      */
   uint32_t     vis_color;         /* audio visualization bars          */
   uint32_t     arrow_color;       /* scroll arrows                     */
   unsigned     arrow_alpha;       /* 0-255 scroll arrow opacity        */
   int          border_corner;     /* borders.png corner square size    */
   int          border_side;       /* borders.png edge piece length     */
   int          border_fit;        /* stretch edge pieces to fit edges  */
   unsigned     panel_alpha;       /* 0-255 background opacity          */
   /* Colour settings may also hold COLOR_TRANSPARENT or COLOR_IMAGE
    * (see skin.h) where the option offers them. */
} dlna_config;

extern dlna_config g_cfg;

/* Bits returned by options_update(). */
#define OPT_CHANGED_GEOMETRY 0x01
#define OPT_CHANGED_LAYOUT   0x02
#define OPT_CHANGED_SORT     0x04
#define OPT_CHANGED_INPUT    0x08
#define OPT_CHANGED_COLORS   0x10

void options_init(retro_environment_t environ_cb);
int  options_update(retro_environment_t environ_cb);
void options_deinit(void);

#endif
