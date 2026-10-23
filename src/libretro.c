/*
 * libretro.c - the libretro API: glue between the frontend and the
 * browser (ui.c) and the player (player.c).
 *
 * The core runs at a fixed 60 frames per second with 48 kHz audio.  Every
 * retro_run() call reads the pad, updates either the browser or the
 * player, pushes exactly 800 stereo audio frames and one video frame.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <libavformat/avformat.h>

#include <libretro.h>

#include "util.h"
#include "options.h"
#include "input.h"
#include "font.h"
#include "gfx.h"
#include "dlna.h"
#include "thumbs.h"
#include "ui.h"
#include "player.h"
#include "overlay.h"
#include "skin.h"
#include "playlist.h"
#include "sshio.h"
#include "viz.h"

#define CORE_FPS          60
#define AUDIO_FRAMES      (PLAYER_SAMPLE_RATE / CORE_FPS)  /* 800 */
#define TITLE_SECONDS     5.0
#define TOAST_SECONDS     2.5
#define SEEK_CHAIN_SECONDS 1.5

#ifndef GIT_VERSION
#define GIT_VERSION ""
#endif

static retro_environment_t        environ_cb;
static retro_video_refresh_t      video_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t         input_poll_cb;
static retro_input_state_t        input_state_cb;
static retro_log_printf_t         log_cb;

static char       g_system_dir[1024];
static uint32_t  *g_fb;
static int        g_fb_w, g_fb_h;
static int16_t    g_audio[AUDIO_FRAMES * 2];
static int        g_force_redraw;
static int        g_loaded;
static double     g_time;

/* Playback state. */
static player     *g_player;
static dlna_entry *g_playing;
static int         g_paused;
static aspect_mode g_aspect = ASPECT_MEDIA;
static int         g_info_visible;
static double      g_title_until;
static char        g_toast[256];
static double      g_toast_until;
static int         g_has_a, g_has_b, g_looping;
static double      g_mark_a, g_mark_b;
static int         g_seek_step;
static int         g_seek_dir;
static double      g_seek_time = -100.0;

static const double seek_steps[4] = { 10.0, 30.0, 60.0, 300.0 };

/* Playlist playback (.m3u / .dlna / .json content). */
static playlist    g_pl;
static size_t     *g_order;         /* play order: indices into g_pl  */
static size_t      g_order_pos;
static int         g_pl_active;     /* a playlist is being played     */
static int         g_pl_waiting;    /* a dlna:// entry is resolving   */
static size_t      g_pl_failures;   /* entries that failed in a row   */
static double      g_picture_since; /* when the current picture appeared */

/* The playback background (colour or playback.png), rendered once at
 * canvas size and rebuilt when the setting or the canvas changes. */
static gfx_image  *g_play_bg;
static unsigned    g_play_bg_serial = 1;
static int         g_play_bg_dirty  = 1;

/* The dlna:// path (or plain URL) of what is playing, for save states. */
static char       *g_playing_uri;

/* A save state that has been loaded but not applied yet: the media first
 * has to be found and opened before we can seek, pick tracks, etc. */
typedef struct restore_state
{
   int    active;
   int    player;          /* 1: media was playing, 0: browsing      */
   char   uri[4096];
   double pos;
   int    audio, sub;
   int    has_a, has_b, looping;
   double a, b;
   int    paused;
   int    aspect;
} restore_state;

static restore_state g_restore;

static const char *const aspect_names[ASPECT_COUNT] = {
   "Media Selected", "Forced Full Screen", "16:9", "4:3", "5:4"
};

/* ------------------------------------------------------------------ */
/* Logging */

static void log_wrapper(int level, const char *message)
{
   if (log_cb)
      log_cb((enum retro_log_level)level, "%s", message);
   else
      fputs(message, stderr);
}

/* ------------------------------------------------------------------ */
/* Framebuffer */

static int alloc_framebuffer(void)
{
   uint32_t *fb;
   if (g_fb && g_fb_w == g_cfg.width && g_fb_h == g_cfg.height)
      return 1;
   fb = (uint32_t*)calloc((size_t)g_cfg.width * g_cfg.height, sizeof(uint32_t));
   if (!fb)
      return 0;
   free(g_fb);
   g_fb   = fb;
   g_fb_w = g_cfg.width;
   g_fb_h = g_cfg.height;
   return 1;
}

static gfx_surface surface(void)
{
   gfx_surface s;
   s.pixels = g_fb;
   s.width  = g_fb_w;
   s.height = g_fb_h;
   s.pitch  = g_fb_w;
   return s;
}

static void apply_options(int first)
{
   int changed = options_update(environ_cb);

   if (first || (changed & OPT_CHANGED_INPUT))
      input_configure();
   if (!first && (changed & OPT_CHANGED_GEOMETRY))
   {
      struct retro_game_geometry geom;
      alloc_framebuffer();
      geom.base_width   = (unsigned)g_fb_w;
      geom.base_height  = (unsigned)g_fb_h;
      geom.max_width    = CANVAS_MAX_WIDTH;
      geom.max_height   = CANVAS_MAX_HEIGHT;
      geom.aspect_ratio = (float)g_fb_w / (float)g_fb_h;
      environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &geom);
   }
   if (!first && (changed & OPT_CHANGED_LAYOUT))
   {
      font_flush_cache();
      ui_layout(g_fb_w, g_fb_h, g_cfg.font_size);
   }
   if (!first && (changed & OPT_CHANGED_SORT))
      ui_resort();
   if (changed & (OPT_CHANGED_COLORS | OPT_CHANGED_GEOMETRY))
      g_play_bg_dirty = 1;
   g_force_redraw = 1;
}

/* ------------------------------------------------------------------ */
/* Playback control */

static void toast(const char *text)
{
   strncpy(g_toast, text, sizeof(g_toast) - 1);
   g_toast[sizeof(g_toast) - 1] = '\0';
   g_toast_until = g_time + TOAST_SECONDS;
}

static void start_playback(dlna_entry *e)
{
   if (!e || !e->url)
   {
      dlna_entry_free(e);
      return;
   }
   {
      /* Never put passwords in the log. */
      char *safe = util_uri_without_password(e->url);
      dlna_log(LOG_INFO, "Playing \"%s\" (%s)\n", e->title ? e->title : "",
            safe ? safe : "");
      free(safe);
   }
   g_player = player_open(e->url, e->media, e->thumb_url, g_cfg.subtitles,
         e->duration);
   if (!g_player)
   {
      ui_set_message("Could not start playback", 4.0);
      dlna_entry_free(e);
      return;
   }
   g_playing      = e;
   g_pl_waiting   = 0;
   viz_reset();
   free(g_playing_uri);
   g_playing_uri  = ui_media_uri(e);
   if (!g_playing_uri)
      g_playing_uri = util_strdup(e->url); /* played directly, not browsed */
   g_paused       = 0;
   g_info_visible = 0;
   g_title_until  = 0.0;
   /* Keep a message that is still showing (e.g. "Next 3 / 10"). */
   if (g_time >= g_toast_until)
      g_toast[0] = '\0';
   g_has_a = g_has_b = g_looping = 0;
   g_seek_time    = -100.0;
   input_reset();
}

static void start_playback(dlna_entry *e);

/* ------------------------------------------------------------------ */
/* Playlists */

static void playlist_shuffle(void)
{
   size_t i;
   for (i = 0; i < g_pl.count; i++)
      g_order[i] = i;
   if (!g_cfg.play_random)
      return;
   /* Fisher-Yates.  rand() is plenty for shuffling media. */
   for (i = g_pl.count; i > 1; i--)
   {
      size_t j = (size_t)rand() % i;
      size_t t = g_order[i - 1];
      g_order[i - 1] = g_order[j];
      g_order[j]     = t;
   }
}

static void playlist_reset(void)
{
   playlist_free(&g_pl);
   free(g_order);
   g_order      = NULL;
   g_pl_active  = 0;
   g_pl_waiting = 0;
}

/* The playlist is over: hand control back to RetroArch's menu. */
static void playlist_finish(void)
{
   dlna_log(LOG_INFO, "Playlist finished; returning to the frontend\n");
   g_pl_active  = 0;
   g_pl_waiting = 0;
   environ_cb(RETRO_ENVIRONMENT_SHUTDOWN, NULL);
}

/* A media entry for a URI that does not go through the browser. */
static dlna_entry *entry_from_uri(const char *uri)
{
   dlna_entry *e = dlna_entry_new(ENT_MEDIA);
   const char *name;
   char        ext[16];

   if (!e)
      return NULL;
   name = strrchr(uri, '/');
   if (!name)
      name = strrchr(uri, '\\');
   e->url   = util_strdup(uri);
   e->title = util_strdup(name && name[1] ? name + 1 : uri);
   if (e->title)
   {
      /* Strip a query string and make %20 etc. readable. */
      e->title[strcspn(e->title, "?#")] = '\0';
      util_percent_decode(e->title);
   }
   e->media = util_media_type_from_extension(
         util_extension(uri, ext, sizeof(ext)));
   if (e->media == MT_NONE)
      e->media = MT_VIDEO;
   return e;
}

static void playlist_play_current(void)
{
   const char *uri = g_pl.items[g_order[g_order_pos]];

   g_picture_since = -1.0;
   g_pl_waiting    = 0;
   if (util_starts_with_nocase(uri, "dlna://"))
   {
      /* Found through the browser; ui_update() hands it to us. */
      ui_take_resolve_outcome();
      ui_start_uri(uri);
      g_pl_waiting = 1;
   }
   else
      start_playback(entry_from_uri(uri));
}

/* The current entry finished (or failed): move on according to the
 * Looping option. */
static void playlist_advance(int failed)
{
   if (failed)
   {
      if (++g_pl_failures >= g_pl.count || g_cfg.play_loop == LOOP_SINGLE
            || g_cfg.play_loop == LOOP_REPEAT_ONE)
      {
         /* Nothing in the list can be played: stop rather than spin. */
         playlist_finish();
         return;
      }
   }
   switch (g_cfg.play_loop)
   {
      case LOOP_SINGLE:
         playlist_finish();
         return;
      case LOOP_REPEAT_ONE:
         break;
      case LOOP_ONCE:
         if (++g_order_pos >= g_pl.count)
         {
            playlist_finish();
            return;
         }
         break;
      case LOOP_REPEAT_ALL:
         if (++g_order_pos >= g_pl.count)
         {
            size_t last = g_order[g_pl.count - 1];
            g_order_pos = 0;
            playlist_shuffle(); /* a new order each time round */
            /* ...but never the entry that just played twice in a row. */
            if (g_pl.count > 1 && g_order[0] == last)
            {
               g_order[0] = g_order[1];
               g_order[1] = last;
            }
         }
         break;
   }
   playlist_play_current();
}

static int playlist_start(const char *path)
{
   char error[512];

   playlist_reset();
   if (!playlist_load(path, &g_pl, error, sizeof(error)))
   {
      dlna_log(LOG_ERROR, "Playlist %s: %s\n", path, error);
      ui_set_message(error, 8.0);
      return 0;
   }
   if (g_pl.count == 1)
   {
      /* One entry is not a list: play it exactly as if it had been opened
       * on its own (A-B looping on the shoulders, no Looping options; a
       * dlna:// entry may also just be a folder to browse). */
      char *uri = util_strdup(g_pl.items[0]);
      playlist_reset();
      if (!uri)
         return 0;
      if (util_starts_with_nocase(uri, "dlna://"))
         ui_start_uri(uri);
      else
         start_playback(entry_from_uri(uri));
      free(uri);
      return 1;
   }
   g_order = (size_t*)malloc(g_pl.count * sizeof(size_t));
   if (!g_order)
   {
      playlist_free(&g_pl);
      return 0;
   }
   dlna_log(LOG_INFO, "Playlist with %lu entries\n", (unsigned long)g_pl.count);
   srand((unsigned)time(NULL));
   playlist_shuffle();
   g_order_pos   = 0;
   g_pl_failures = 0;
   g_pl_active   = 1;
   playlist_play_current();
   return 1;
}

static void stop_playback(void);
static void toast(const char *text);

/* Previous (dir < 0) or next (dir > 0) entry, from the skip buttons.  At
 * the ends: Repeat All and Repeat Entry wrap around; otherwise "next" on
 * the last entry ends the playlist and "previous" on the first restarts
 * it. */
static void playlist_skip(int dir)
{
   char msg[64];

   if (g_player)
      stop_playback();
   if (dir > 0)
   {
      if (g_order_pos + 1 < g_pl.count)
         g_order_pos++;
      else if (g_cfg.play_loop == LOOP_REPEAT_ALL
            || g_cfg.play_loop == LOOP_REPEAT_ONE)
         g_order_pos = 0;
      else
      {
         playlist_finish();
         return;
      }
   }
   else
   {
      if (g_order_pos > 0)
         g_order_pos--;
      else if (g_cfg.play_loop == LOOP_REPEAT_ALL
            || g_cfg.play_loop == LOOP_REPEAT_ONE)
         g_order_pos = g_pl.count - 1;
   }
   g_pl_failures = 0;
   playlist_play_current();
   sprintf(msg, "%s  %lu / %lu", dir > 0 ? "Next" : "Previous",
         (unsigned long)(g_order_pos + 1), (unsigned long)g_pl.count);
   toast(msg);
}

static void stop_playback(void)
{
   player_close(g_player);
   g_player = NULL;
   dlna_entry_free(g_playing);
   g_playing      = NULL;
   free(g_playing_uri);
   g_playing_uri  = NULL;
   g_restore.active = 0;
   g_force_redraw = 1;
   input_reset();
}

static void format_track(int audio, char *out, size_t size)
{
   int  n   = player_track_count(g_player, audio);
   int  cur = player_track_current(g_player, audio);
   char label[128];

   if (n == 0)
      strcpy(out, audio ? "None" : "None available");
   else if (cur < 0)
      strcpy(out, "Off");
   else
   {
      player_track_label(g_player, audio, cur, label, sizeof(label));
      sprintf(out, "#%d of %d  %.*s", cur + 1, n,
            (int)(size > 32 ? size - 32 : 1), label);
   }
}

static void cycle_track(int audio, int dir)
{
   int  n   = player_track_count(g_player, audio);
   int  cur = player_track_current(g_player, audio);
   char buf[200], desc[180];

   if (audio)
   {
      if (n <= 1)
      {
         toast(n ? "Only one audio track" : "No audio tracks");
         return;
      }
      cur = (cur + dir + n) % n;
   }
   else
   {
      if (n == 0)
      {
         toast("No subtitle tracks");
         return;
      }
      /* Subtitles cycle through Off (-1) and every track. */
      cur += dir;
      if (cur >= n)
         cur = -1;
      if (cur < -1)
         cur = n - 1;
   }
   player_select_track(g_player, audio, cur);
   format_track(audio, desc, sizeof(desc));
   sprintf(buf, "%s: %s", audio ? "Audio" : "Subtitles", desc);
   toast(buf);
}

static void do_seek(int dir)
{
   char   buf[128], from[32], to[32];
   double target;

   if (!player_can_seek(g_player))
   {
      toast("Seeking is not available for this media");
      return;
   }
   /* Presses in quick succession in the same direction step through
    * 10 s, 30 s, 60 s and then 5 min (which repeats). */
   if (g_time - g_seek_time < SEEK_CHAIN_SECONDS && dir == g_seek_dir)
   {
      if (g_seek_step < 3)
         g_seek_step++;
   }
   else
      g_seek_step = 0;
   g_seek_time = g_time;
   g_seek_dir  = dir;

   target = player_seek_base(g_player) + dir * seek_steps[g_seek_step];
   if (target < 0.0)
      target = 0.0;
   if (player_duration(g_player) > 0.0 && target > player_duration(g_player))
      target = player_duration(g_player);
   player_seek(g_player, target);

   util_format_time(target, from, sizeof(from));
   util_format_time(player_duration(g_player), to, sizeof(to));
   if (seek_steps[g_seek_step] >= 60.0)
      sprintf(buf, "%s%d min   %s / %s", dir > 0 ? "+" : "-",
            (int)(seek_steps[g_seek_step] / 60.0), from, to);
   else
      sprintf(buf, "%s%d s   %s / %s", dir > 0 ? "+" : "-",
            (int)seek_steps[g_seek_step], from, to);
   toast(buf);
}

static void mark_loop(int is_b)
{
   char   buf[128], ta[32], tb[32];
   double pos;

   if (!player_can_seek(g_player))
   {
      toast("A-B looping is not available for this media");
      return;
   }
   if (g_looping)
   {
      /* Either shoulder ends the loop. */
      player_clear_loop(g_player);
      g_looping = g_has_a = g_has_b = 0;
      toast("A-B loop off");
      return;
   }
   pos = player_position(g_player);
   if (is_b)
   {
      g_mark_b = pos;
      g_has_b  = 1;
   }
   else
   {
      g_mark_a = pos;
      g_has_a  = 1;
   }
   util_format_time(g_mark_a, ta, sizeof(ta));
   util_format_time(g_mark_b, tb, sizeof(tb));

   if (g_has_a && g_has_b)
   {
      if (g_mark_a + 0.1 < g_mark_b)
      {
         player_set_loop(g_player, g_mark_a, g_mark_b, 1);
         g_looping = 1;
         sprintf(buf, "Looping A %s - B %s", ta, tb);
      }
      else
         sprintf(buf, "Point B (%s) must come after point A (%s)", tb, ta);
   }
   else if (is_b)
      sprintf(buf, "Loop point B: %s", tb);
   else
      sprintf(buf, "Loop point A: %s", ta);
   toast(buf);
}

static void player_input(void)
{
   int subs_vertical = g_cfg.dpad_subs_vertical;
   int picture       = player_is_picture(g_player);

   if (input_pressed(ACT_BACK))
   {
      stop_playback();
      if (g_pl_active)
         playlist_finish(); /* a playlist has no folder to go back to */
      return;
   }
   if (input_pressed(ACT_TITLE))
      g_title_until = g_time < g_title_until ? 0.0 : g_time + TITLE_SECONDS;
   if (input_pressed(ACT_INFO))
      g_info_visible = !g_info_visible;
   if (input_pressed(ACT_ASPECT))
   {
      char buf[64];
      g_aspect = (aspect_mode)((g_aspect + 1) % ASPECT_COUNT);
      sprintf(buf, "Aspect: %s", aspect_names[g_aspect]);
      toast(buf);
   }
   if (picture)
      return;

   if (input_pressed(ACT_PAUSE))
   {
      g_paused = !g_paused;
      toast(g_paused ? "Paused" : "Playing");
   }
   if (input_pressed(subs_vertical ? ACT_DOWN : ACT_RIGHT))
      cycle_track(0, 1);
   if (input_pressed(subs_vertical ? ACT_UP : ACT_LEFT))
      cycle_track(0, -1);
   if (input_pressed(subs_vertical ? ACT_RIGHT : ACT_DOWN))
      cycle_track(1, 1);
   if (input_pressed(subs_vertical ? ACT_LEFT : ACT_UP))
      cycle_track(1, -1);
   if (input_repeat(ACT_SKIP_FWD, 0.5, 0.35))
      do_seek(1);
   if (input_repeat(ACT_SKIP_BACK, 0.5, 0.35))
      do_seek(-1);
   /* In a playlist the A-B buttons skip between entries instead. */
   if (input_pressed(ACT_MARK_A))
   {
      if (g_pl_active)
      {
         playlist_skip(-1);
         return;
      }
      mark_loop(0);
   }
   if (input_pressed(ACT_MARK_B))
   {
      if (g_pl_active)
      {
         playlist_skip(1);
         return;
      }
      mark_loop(1);
   }
}

/* ------------------------------------------------------------------ */
/* Save states
 *
 * A state is a short block of text ("key=value" lines) padded with NULs
 * to a fixed size.  Text keeps it independent of endianness and struct
 * layout, and easy to inspect.  Times are stored in whole milliseconds so
 * the C library's locale can never change the decimal separator.
 *
 * What is saved: the dlna:// path of the media being played (or of the
 * folder being browsed), the position, the audio and subtitle track, the
 * A and B loop points, whether the loop is on, pause and aspect mode.
 * Loading a state re-opens the media through the normal dlna:// path
 * resolution, then applies the rest once it is playing. */

#define STATE_SIZE  8192
#define STATE_MAGIC "DLNA-STATE 1\n"

/* Same media?  Exact path match, or the same final name (the resolver may
 * reach the file through a differently named but equivalent path). */
static int same_media(const char *a, const char *b)
{
   const char *sa, *sb, *da, *db;
   size_t      la, lb;

   if (!a || !b)
      return 0;
   if (strcmp(a, b) == 0)
      return 1;
   sa = strrchr(a, '/');
   sb = strrchr(b, '/');
   sa = sa ? sa + 1 : a;
   sb = sb ? sb + 1 : b;
   da = strrchr(sa, '.');
   db = strrchr(sb, '.');
   la = da ? (size_t)(da - sa) : strlen(sa);
   lb = db ? (size_t)(db - sb) : strlen(sb);
   return la == lb && util_strncasecmp(sa, sb, la) == 0;
}

static void apply_restore(void)
{
   restore_state *r = &g_restore;
   char           msg[64];

   r->active = 0;
   if (!same_media(g_playing_uri, r->uri))
      return;
   if (r->audio >= 0 && r->audio < player_track_count(g_player, 1)
         && r->audio != player_track_current(g_player, 1))
      player_select_track(g_player, 1, r->audio);
   if (r->sub >= -1 && r->sub < player_track_count(g_player, 0)
         && r->sub != player_track_current(g_player, 0))
      player_select_track(g_player, 0, r->sub);
   if (g_looping && !r->looping)
      player_clear_loop(g_player);
   /* Skip tiny seeks (e.g. reloading a state saved a moment ago). */
   if (player_can_seek(g_player)
         && (player_position(g_player) - r->pos > 1.0
            || r->pos - player_position(g_player) > 1.0))
      player_seek(g_player, r->pos);
   g_has_a   = r->has_a;
   g_has_b   = r->has_b;
   g_mark_a  = r->a;
   g_mark_b  = r->b;
   g_looping = 0;
   if (r->looping && r->has_a && r->has_b && r->a < r->b)
   {
      player_set_loop(g_player, r->a, r->b, 0);
      g_looping = 1;
   }
   g_paused = r->paused;
   if (r->aspect >= 0 && r->aspect < ASPECT_COUNT)
      g_aspect = (aspect_mode)r->aspect;
   util_format_time(r->pos, msg + 13, sizeof(msg) - 13);
   memcpy(msg, "State loaded ", 13);
   toast(msg);
}

static long to_ms(double s)
{
   return s < 0.0 ? -1L : (long)(s * 1000.0 + 0.5);
}

size_t retro_serialize_size(void)
{
   return STATE_SIZE;
}

bool retro_serialize(void *data, size_t size)
{
   char          *buf = (char*)data;
   char          *folder = NULL;
   const char    *uri;
   restore_state  s;
   size_t         len;

   if (!data || size < STATE_SIZE)
      return false;
   memset(&s, 0, sizeof(s));
   if (g_restore.active)
      s = g_restore; /* loaded but not applied yet: keep it as it was */
   else if (g_player)
   {
      s.player  = 1;
      s.pos     = player_position(g_player);
      s.audio   = player_track_current(g_player, 1);
      s.sub     = player_track_current(g_player, 0);
      s.has_a   = g_has_a;
      s.has_b   = g_has_b;
      s.a       = g_mark_a;
      s.b       = g_mark_b;
      s.looping = g_looping;
      s.paused  = g_paused;
      s.aspect  = (int)g_aspect;
      if (g_playing_uri)
      {
         /* Passwords are not written into state files; on loading, the
          * playlist entry with the same URI supplies them again. */
         char *safe = util_uri_without_password(g_playing_uri);
         if (safe)
            strncpy(s.uri, safe, sizeof(s.uri) - 1);
         free(safe);
      }
   }
   else
   {
      folder = ui_folder_uri();
      if (folder)
         strncpy(s.uri, folder, sizeof(s.uri) - 1);
      free(folder);
   }
   uri = s.uri;
   if (strlen(uri) > 4000)
      return false;

   memset(buf, 0, STATE_SIZE);
   /* Worst case is about 4000 + 300 bytes, well inside STATE_SIZE. */
   len = (size_t)sprintf(buf, "%smode=%s\nuri=%s\n", STATE_MAGIC,
         s.player ? "player" : "browser", uri);
   if (s.player)
      sprintf(buf + len, "pos_ms=%ld\naudio=%d\nsub=%d\na_ms=%ld\n"
            "b_ms=%ld\nloop=%d\npaused=%d\naspect=%d\n",
            to_ms(s.pos), s.audio, s.sub, s.has_a ? to_ms(s.a) : -1L,
            s.has_b ? to_ms(s.b) : -1L, s.looping, s.paused, s.aspect);
   /* Where we are in a playlist (the playlist itself is the content, so
    * it is not saved; only the position in the play order). */
   if (g_pl_active)
      sprintf(buf + strlen(buf), "pl_pos=%lu\n", (unsigned long)g_order_pos);
   return true;
}

bool retro_unserialize(const void *data, size_t size)
{
   char          *text, *line, *next;
   restore_state  r;
   size_t         n = size < STATE_SIZE ? size : STATE_SIZE;
   long           a_ms = -1, b_ms = -1;

   if (!data || n < strlen(STATE_MAGIC)
         || memcmp(data, STATE_MAGIC, strlen(STATE_MAGIC)) != 0)
      return false;
   text = (char*)malloc(n + 1);
   if (!text)
      return false;
   memcpy(text, data, n);
   text[n] = '\0';

   memset(&r, 0, sizeof(r));
   r.audio  = -1;
   r.sub    = -2; /* "leave as it is" */
   r.aspect = -1;
   for (line = text + strlen(STATE_MAGIC); line && *line; line = next)
   {
      char *eq;
      next = strchr(line, '\n');
      if (next)
         *next++ = '\0';
      eq = strchr(line, '=');
      if (!eq)
         continue;
      *eq++ = '\0';
      if (strcmp(line, "mode") == 0)
         r.player = strcmp(eq, "player") == 0;
      else if (strcmp(line, "uri") == 0)
         strncpy(r.uri, eq, sizeof(r.uri) - 1);
      else if (strcmp(line, "pos_ms") == 0)
         r.pos = atol(eq) / 1000.0;
      else if (strcmp(line, "audio") == 0)
         r.audio = atoi(eq);
      else if (strcmp(line, "sub") == 0)
         r.sub = atoi(eq);
      else if (strcmp(line, "a_ms") == 0)
         a_ms = atol(eq);
      else if (strcmp(line, "b_ms") == 0)
         b_ms = atol(eq);
      else if (strcmp(line, "loop") == 0)
         r.looping = atoi(eq) != 0;
      else if (strcmp(line, "paused") == 0)
         r.paused = atoi(eq) != 0;
      else if (strcmp(line, "aspect") == 0)
         r.aspect = atoi(eq);
      else if (strcmp(line, "pl_pos") == 0 && g_pl_active
            && (size_t)atol(eq) < g_pl.count)
         g_order_pos = (size_t)atol(eq);
      /* Unknown keys are ignored, so newer states load in older cores. */
   }
   free(text);
   r.has_a = a_ms >= 0;
   r.has_b = b_ms >= 0;
   r.a     = a_ms / 1000.0;
   r.b     = b_ms / 1000.0;

   if (!r.player)
   {
      /* Browsing: just go to that folder (or the server list). */
      if (g_player)
         stop_playback();
      if (r.uri[0])
         ui_start_uri(r.uri);
      else
         ui_start_servers();
      g_force_redraw = 1;
      return true;
   }

   if (!r.uri[0])
      return false;
   /* Put back credentials the state file leaves out. */
   if (g_pl_active)
   {
      size_t k;
      for (k = 0; k < g_pl.count; k++)
      {
         char *safe = util_uri_without_password(g_pl.items[k]);
         int   same = safe && strcmp(safe, r.uri) == 0;
         free(safe);
         if (same)
         {
            strncpy(r.uri, g_pl.items[k], sizeof(r.uri) - 1);
            break;
         }
      }
   }
   if (g_player && same_media(g_playing_uri, r.uri))
   {
      /* Same media: apply right away (or once it has finished opening). */
      g_restore        = r;
      g_restore.active = 1;
      return true;
   }

   if (g_player)
      stop_playback();
   g_restore        = r;
   g_restore.active = 1;
   if (util_starts_with_nocase(r.uri, "dlna://"))
      ui_start_uri(r.uri); /* plays the file once it has been found */
   else
   {
      dlna_entry *e = dlna_entry_new(ENT_MEDIA);
      char        ext[16];
      if (e)
      {
         e->url   = util_strdup(r.uri);
         e->title = util_strdup(r.uri);
         e->media = util_media_type_from_extension(
               util_extension(r.uri, ext, sizeof(ext)));
         if (e->media == MT_NONE)
            e->media = MT_VIDEO;
         start_playback(e);
         g_restore = r; /* start_playback() does not clear it, but be sure */
         g_restore.active = 1;
      }
   }
   g_force_redraw = 1;
   return true;
}

static void draw_player_overlays(gfx_surface *fb)
{
   if (g_info_visible)
   {
      overlay_info_data d;
      memset(&d, 0, sizeof(d));
      format_track(0, d.subtitle, sizeof(d.subtitle));
      format_track(1, d.audio, sizeof(d.audio));
      d.subtitle_count = player_track_count(g_player, 0);
      d.audio_count    = player_track_count(g_player, 1);
      d.position       = player_is_picture(g_player) ? -1.0
         : player_position(g_player);
      d.duration       = player_is_picture(g_player) ? -1.0
         : player_duration(g_player);
      if (g_paused)
         strcat(d.status, "Paused   ");
      if (g_looping || g_has_a || g_has_b)
      {
         char ta[32], tb[32], tmp[96];
         util_format_time(g_has_a ? g_mark_a : -1.0, ta, sizeof(ta));
         util_format_time(g_has_b ? g_mark_b : -1.0, tb, sizeof(tb));
         sprintf(tmp, "Loop%s A %s - B %s   ", g_looping ? "ing" : "", ta, tb);
         strcat(d.status, tmp);
      }
      if (g_pl_active)
      {
         char tmp[48];
         sprintf(tmp, "Playlist %lu / %lu   ", (unsigned long)(g_order_pos + 1),
               (unsigned long)g_pl.count);
         strcat(d.status, tmp);
      }
      strcat(d.status, "Aspect: ");
      strcat(d.status, aspect_names[g_aspect]);
      overlay_info(fb, &d);
   }
   if (g_time < g_title_until && g_playing && g_playing->title)
      overlay_title(fb, g_playing->title);
   if (g_paused && g_time >= g_toast_until)
      overlay_toast(fb, "Paused");
   else if (g_time < g_toast_until)
      overlay_toast(fb, g_toast);
}

static const gfx_image *playback_background(void)
{
   if (g_play_bg_dirty || !g_play_bg || g_play_bg->width != g_fb_w
         || g_play_bg->height != g_fb_h)
   {
      gfx_image_free(g_play_bg);
      g_play_bg = gfx_image_new(g_fb_w, g_fb_h);
      if (g_play_bg)
      {
         gfx_surface s;
         gfx_rect    all;
         s.pixels = g_play_bg->pixels;
         s.width  = s.pitch = g_fb_w;
         s.height = g_fb_h;
         all.x = all.y = 0;
         all.w = g_fb_w;
         all.h = g_fb_h;
         skin_fill(&s, all, SKIN_PLAYBACK, g_cfg.play_bg, 0x000000, 255, 1);
      }
      g_play_bg_serial++;
      g_play_bg_dirty = 0;
   }
   return g_play_bg;
}

static void run_player(void)
{
   gfx_surface  fb = surface();
   char         err[256];
   player_state st = player_get_state(g_player, err, sizeof(err));

   if (st == PLAYER_ERROR)
   {
      stop_playback();
      ui_set_message(err[0] ? err : "Playback failed", 5.0);
      memset(g_audio, 0, sizeof(g_audio));
      if (g_pl_active)
         playlist_advance(1);
      return;
   }
   if (st == PLAYER_PLAYING && g_pl_active)
   {
      g_pl_failures = 0;
      /* Pictures never end by themselves: time them out in playlists. */
      if (player_is_picture(g_player))
      {
         if (g_picture_since < 0.0)
            g_picture_since = g_time;
         else if (g_time - g_picture_since >= g_cfg.picture_seconds && !g_paused)
            st = PLAYER_ENDED;
      }
   }
   if (st == PLAYER_ENDED)
   {
      stop_playback();
      memset(g_audio, 0, sizeof(g_audio));
      if (g_pl_active)
         playlist_advance(0);
      return;
   }

   if (st == PLAYER_PLAYING && g_restore.active)
      apply_restore();

   player_input();
   if (!g_player)
   {
      memset(g_audio, 0, sizeof(g_audio));
      return;
   }

   player_audio(g_player, g_audio, AUDIO_FRAMES, g_paused);

   {
      gfx_rect art;
      int      have_art = 0;

      if (!player_render(g_player, &fb, g_aspect, playback_background(),
               g_play_bg_serial) && st == PLAYER_PLAYING)
      {
         /* Audio without cover art: show the audio icon. */
         art.w = fb.height * 2 / 5;
         art.h = art.w;
         art.x = (fb.width - art.w) / 2;
         art.y = (fb.height - art.h) / 2;
         ui_draw_icon(&fb, g_playing ? g_playing->media : MT_AUDIO, 0, art);
         have_art = 1;
      }
      else
         have_art = player_art_rect(g_player, &art);

      /* Spectrum bars over the art (or icon) while music plays. */
      if (st == PLAYER_PLAYING && have_art && !player_has_video(g_player)
            && !player_is_picture(g_player)
            && COLOR_IS_SOLID(g_cfg.vis_color))
      {
         viz_feed(g_audio, AUDIO_FRAMES);
         viz_draw(&fb, art, g_cfg.vis_color, 1.0 / CORE_FPS);
      }
   }
   if (st == PLAYER_OPENING
         || (player_is_buffering(g_player) && !g_paused))
   {
      static const char *const dots[4] = { "", ".", "..", "..." };
      char buf[32];
      int  px = g_cfg.font_size;
      sprintf(buf, "Loading%s", dots[(int)(g_time * 3.0) % 4]);
      font_draw_outlined(&fb, buf, fb.width / 2 - font_text_width("Loading", px) / 2,
            fb.height / 2, px, 0xFFFFFF, 0x000000, 2);
   }
   draw_player_overlays(&fb);
}

/* ------------------------------------------------------------------ */
/* libretro API */

void retro_set_environment(retro_environment_t cb)
{
   struct retro_log_callback logging;
   bool                      no_game = true;

   environ_cb = cb;
   if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging))
      log_cb = logging.log;
   util_set_logger(log_wrapper);
   cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);
   options_init(cb);
}

void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

void retro_init(void)
{
   const char *dir = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) && dir)
   {
      strncpy(g_system_dir, dir, sizeof(g_system_dir) - 1);
      g_system_dir[sizeof(g_system_dir) - 1] = '\0';
   }
   avformat_network_init();
   sshio_global_init(g_system_dir);
   util_find_ca_file(g_system_dir);
   /* FFmpeg is chatty; only show its warnings and errors. */
   av_log_set_level(AV_LOG_ERROR);
}

void retro_deinit(void)
{
   sshio_global_deinit();
   avformat_network_deinit();
   options_deinit();
}

unsigned retro_api_version(void)
{
   return RETRO_API_VERSION;
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
   (void)port;
   (void)device;
}

void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
   info->library_name     = "DLNA";
   info->library_version  = "1.0.0" GIT_VERSION;
   info->valid_extensions = "dlna|m3u|m3u8|json";
   info->need_fullpath    = true;
   info->block_extract    = true;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   memset(info, 0, sizeof(*info));
   info->geometry.base_width   = (unsigned)g_cfg.width;
   info->geometry.base_height  = (unsigned)g_cfg.height;
   info->geometry.max_width    = CANVAS_MAX_WIDTH;
   info->geometry.max_height   = CANVAS_MAX_HEIGHT;
   info->geometry.aspect_ratio = (float)g_cfg.width / (float)g_cfg.height;
   info->timing.fps            = CORE_FPS;
   info->timing.sample_rate    = PLAYER_SAMPLE_RATE;
}

/* Work out what the content path asks for.  Returns a malloc'ed dlna://
 * URI, or NULL with *direct set to a media path/URL to play directly. */
static char *content_uri(const char *path, char **direct)
{
   char ext[16];

   *direct = NULL;
   if (!path || !*path)
      return NULL;
   if (util_starts_with_nocase(path, "dlna://"))
      return util_strdup(path);
   if (strcmp(util_extension(path, ext, sizeof(ext)), "dlna") == 0)
   {
      /* A .dlna file holds the URI on its first non-comment line. */
      char *text = util_read_file(path, NULL);
      char *line, *next, *uri = NULL;
      for (line = text; line && *line && !uri; line = next)
      {
         next = strchr(line, '\n');
         if (next)
            *next++ = '\0';
         util_trim(line);
         if (*line && *line != '#')
         {
            if (util_starts_with_nocase(line, "dlna://"))
               uri = util_strdup(line);
            else
               *direct = util_strdup(line);
            break;
         }
      }
      free(text);
      return uri;
   }
   /* Anything else (a local file, http:// URL) is played directly.  This
    * is mostly a testing aid. */
   *direct = util_strdup(path);
   return NULL;
}

bool retro_load_game(const struct retro_game_info *info)
{
   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
   char                   *uri, *direct = NULL;

   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
   {
      dlna_log(LOG_ERROR, "XRGB8888 is not supported by the frontend\n");
      return false;
   }
   {
      /* States restore a position in a stream, not an exact frame: tell
       * the frontend not to use them for run-ahead or netplay. */
      uint64_t quirks = RETRO_SERIALIZATION_QUIRK_INCOMPLETE;
      environ_cb(RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS, &quirks);
   }

   apply_options(1);
   input_describe(environ_cb);
   if (!alloc_framebuffer())
      return false;

   font_init(g_system_dir);
   dlna_log(LOG_INFO, "Primary font: %s\n", font_primary_name());
   thumbs_init();
   if (!dlna_init(g_system_dir))
      dlna_log(LOG_ERROR, "UPnP could not be started; no servers will be found\n");
   skin_init(g_system_dir);
   ui_init(g_system_dir);
   ui_layout(g_fb_w, g_fb_h, g_cfg.font_size);

   if (info && info->path && playlist_is_file(info->path))
   {
      /* .m3u / .dlna / .json: the browser sits behind the playlist. */
      ui_start_servers();
      playlist_start(info->path);
      uri = NULL;
   }
   else
   {
      uri = content_uri(info ? info->path : NULL, &direct);
      if (uri)
         ui_start_uri(uri);
      else
         ui_start_servers();
   }
   if (direct)
   {
      dlna_entry *e = dlna_entry_new(ENT_MEDIA);
      char        ext[16];
      if (e)
      {
         e->url   = direct;
         e->title = util_strdup(direct);
         e->media = util_media_type_from_extension(
               util_extension(direct, ext, sizeof(ext)));
         if (e->media == MT_NONE)
            e->media = MT_VIDEO;
         start_playback(e);
      }
      else
         free(direct);
   }
   free(uri);

   g_loaded       = 1;
   g_force_redraw = 1;
   return true;
}

bool retro_load_game_special(unsigned type, const struct retro_game_info *info,
      size_t num)
{
   (void)type;
   (void)info;
   (void)num;
   return false;
}

void retro_unload_game(void)
{
   if (!g_loaded)
      return;
   if (g_player)
      stop_playback();
   playlist_reset();
   ui_deinit();
   skin_deinit();
   gfx_image_free(g_play_bg);
   g_play_bg = NULL;
   dlna_deinit();
   thumbs_deinit();
   font_deinit();
   free(g_fb);
   g_fb     = NULL;
   g_fb_w   = g_fb_h = 0;
   g_loaded = 0;
}

void retro_reset(void)
{
   /* Reset = stop playback and go back to the top folder. */
   if (g_player)
      stop_playback();
}

void retro_run(void)
{
   const double dt      = 1.0 / CORE_FPS;
   bool         updated = false;

   input_poll_cb();
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
      apply_options(0);
   input_update(input_state_cb, dt);
   g_time += dt;

   if (!g_fb)
      return;

   if (g_player)
      run_player();
   else
   {
      dlna_entry *e = ui_update(dt);
      memset(g_audio, 0, sizeof(g_audio));
      if (g_pl_active && g_pl_waiting && !e)
      {
         ui_resolve_outcome o = ui_take_resolve_outcome();
         if (o == UI_RESOLVE_FOLDER)
         {
            if (g_pl.count == 1)
               playlist_reset(); /* a single folder: just browse it */
            else
            {
               ui_set_message("Playlist entry is a folder, skipping", 3.0);
               playlist_advance(1);
            }
         }
         else if (o == UI_RESOLVE_FAILED)
            playlist_advance(1);
      }
      else if (g_pl_active && input_pressed(ACT_BACK))
         playlist_finish();
      if (e)
         start_playback(e);
      if (g_player)
         run_player();
      else if (ui_needs_redraw() || g_force_redraw)
      {
         gfx_surface fb = surface();
         ui_render(&fb);
         g_force_redraw = 0;
      }
   }

   audio_batch_cb(g_audio, AUDIO_FRAMES);
   /* Always hand over the finished picture, even when nothing changed.
    * Sending NULL ("duplicate the last frame") saves a copy, but a
    * frontend may lose that last frame, e.g. RetroArch's Vulkan driver
    * when it rebuilds its swapchain as the window first appears, and the
    * static browser would then stay black until something changed.  The
    * expensive part, redrawing, still only happens when needed. */
   video_cb(g_fb, (unsigned)g_fb_w, (unsigned)g_fb_h,
         (size_t)g_fb_w * sizeof(uint32_t));
}

unsigned retro_get_region(void)
{
   return RETRO_REGION_NTSC;
}

void *retro_get_memory_data(unsigned id)
{
   (void)id;
   return NULL;
}

size_t retro_get_memory_size(unsigned id)
{
   (void)id;
   return 0;
}

void retro_cheat_reset(void)
{
}

void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
   (void)index;
   (void)enabled;
   (void)code;
}
