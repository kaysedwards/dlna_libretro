/*
 * ui.c - the media browser.  See ui.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui.h"
#include "font.h"
#include "image.h"
#include "input.h"
#include "options.h"
#include "overlay.h"
#include "skin.h"
#include "thumbs.h"
#include "util.h"

#define MAX_LEVELS     48
#define MIN_THUMB      256   /* the smallest a thumbnail may be drawn */
#define GAP_RATIO_NUM  1     /* padding = thumbnail * 1/10 at least  */
#define GAP_RATIO_DEN  10

/* ------------------------------------------------------------------ */
/* Icons */

enum { ICON_FOLDER = 0, ICON_IMAGE, ICON_AUDIO, ICON_VIDEO, ICON_SERVER,
   ICON_FOLDER_PICTURES, ICON_FOLDER_AUDIO, ICON_FOLDER_VIDEO, ICON_COUNT };

/* The folders/... icons are used for folders inside the Picture, Audio
 * and Video divisions; folder.png (or its glyph) stands in when missing. */
static const char *const icon_files[ICON_COUNT] = {
   "folder.png", "image.png", "audio.png", "video.png", "server.png",
   "folders/pictures.png", "folders/audio.png", "folders/video.png"
};

/* Unicode fallbacks, best first; the first one some font has is used.
 * The final ASCII letter always works (built-in font). */
static const long icon_glyphs[ICON_COUNT][5] = {
   { 0x1F4C1L, 0x1F5C0L, 0x1F5BFL, 0x25A4L, 'F' }, /* folder   */
   { 0x1F5BCL, 0x1F4F7L, 0x25A3L,  0x25A3L, 'I' }, /* picture  */
   { 0x1F3B5L, 0x266BL,  0x266AL,  0x266AL, 'A' }, /* audio    */
   { 0x1F3ACL, 0x1F39EL, 0x25B6L,  0x25B6L, 'V' }, /* video    */
   { 0x1F5A5L, 0x1F5A7L, 0x2302L,  0x2302L, 'S' }, /* server   */
   { 0x1F4C1L, 0x1F5C0L, 0x1F5BFL, 0x25A4L, 'F' }, /* folders: as folder */
   { 0x1F4C1L, 0x1F5C0L, 0x1F5BFL, 0x25A4L, 'F' },
   { 0x1F4C1L, 0x1F5C0L, 0x1F5BFL, 0x25A4L, 'F' }
};

static gfx_image *g_icon_src[ICON_COUNT];   /* as loaded from disk  */
static gfx_image *g_icon[ICON_COUNT];       /* scaled for the grid  */
static char       g_icon_glyph[ICON_COUNT][8];

/* ------------------------------------------------------------------ */
/* Layout */

typedef struct layout
{
   int width, height;
   int font_px;
   int cols, rows;
   int thumb;          /* thumbnail square size */
   int gap_x, gap_y;   /* padding between thumbnails */
   int grid_x, grid_y; /* top left of the first thumbnail */
   int border;         /* selection frame thickness */
   gfx_rect title;     /* the title panel */
   gfx_rect arrow_up;  /* "more above" arrow strip, above the grid */
   gfx_rect arrow_down;/* "more below" arrow strip, above the title */
} layout;

static layout g_lay;

/* ------------------------------------------------------------------ */
/* Levels */

typedef enum level_kind
{
   LV_SERVERS = 0,
   LV_TYPES,
   LV_FOLDER,
   LV_RESOLVING
} level_kind;

typedef struct level
{
   level_kind   kind;
   int          server;
   char        *container_id;
   media_type   tree;
   media_type   division;   /* video/audio/pictures: picks folder icons */
   char        *title;
   dlna_entry **entries;
   size_t       count;
   size_t       selected;
   long         top_row;
   int          loading;
   int          token;
   size_t       progress, total;
   char        *error;
   char        *select_id;  /* select this entry once loaded */
   char        *uri;        /* dlna:// path of this level (NULL for the
                               server list); used by save states */
} level;

static level      g_levels[MAX_LEVELS];
static int        g_depth;
static unsigned   g_servers_version;
static int        g_dirty = 1;
static double     g_time;
static char       g_message[512];
static double     g_message_until;
static dlna_entry *g_play_next;   /* from a resolved dlna:// URI */
static ui_resolve_outcome g_resolve_outcome;
static char       g_system_dir[1024];

static void level_clear(level *lv)
{
   if (lv->token)
      dlna_cancel(lv->token);
   dlna_entries_free(lv->entries, lv->count);
   free(lv->container_id);
   free(lv->title);
   free(lv->error);
   free(lv->select_id);
   free(lv->uri);
   memset(lv, 0, sizeof(*lv));
}

static level *top_level(void)
{
   return g_depth > 0 ? &g_levels[g_depth - 1] : NULL;
}

/* ------------------------------------------------------------------ */
/* Sorting */

static int compare_entries(const void *a, const void *b)
{
   const dlna_entry *x = *(const dlna_entry * const *)a;
   const dlna_entry *y = *(const dlna_entry * const *)b;
   int               r;

   if (g_cfg.folders_first)
   {
      int fx = x->kind != ENT_MEDIA;
      int fy = y->kind != ENT_MEDIA;
      if (fx != fy)
         return fx ? -1 : 1;
   }
   r = util_natural_compare(x->title ? x->title : "", y->title ? y->title : "");
   if (g_cfg.sort_descending)
      r = -r;
   if (r == 0)
      r = x->order < y->order ? -1 : (x->order > y->order ? 1 : 0);
   return r;
}

static void sort_level(level *lv)
{
   dlna_entry *sel = NULL;
   size_t      i;

   if (lv->kind == LV_TYPES || !lv->count)
      return;
   if (lv->selected < lv->count)
      sel = lv->entries[lv->selected];
   qsort(lv->entries, lv->count, sizeof(dlna_entry*), compare_entries);
   for (i = 0; i < lv->count; i++)
      if (lv->entries[i] == sel)
         lv->selected = i;
}

void ui_resort(void)
{
   int i;
   for (i = 0; i < g_depth; i++)
      sort_level(&g_levels[i]);
   g_dirty = 1;
}

/* ------------------------------------------------------------------ */
/* Selection helpers */

static void ensure_visible(level *lv)
{
   long row, max_top, total_rows;

   if (!g_lay.cols || !g_lay.rows)
      return;
   if (lv->count == 0)
   {
      lv->selected = 0;
      lv->top_row  = 0;
      return;
   }
   if (lv->selected >= lv->count)
      lv->selected = lv->count - 1;
   row        = (long)(lv->selected / (size_t)g_lay.cols);
   total_rows = (long)((lv->count + g_lay.cols - 1) / g_lay.cols);
   max_top    = total_rows - g_lay.rows;
   if (max_top < 0)
      max_top = 0;
   if (row < lv->top_row)
      lv->top_row = row;
   if (row >= lv->top_row + g_lay.rows)
      lv->top_row = row - g_lay.rows + 1;
   if (lv->top_row > max_top)
      lv->top_row = max_top;
   if (lv->top_row < 0)
      lv->top_row = 0;
}

static void select_by_id(level *lv, const char *id)
{
   size_t i;
   if (!id)
      return;
   for (i = 0; i < lv->count; i++)
      if (lv->entries[i]->id && strcmp(lv->entries[i]->id, id) == 0)
      {
         lv->selected = i;
         break;
      }
   ensure_visible(lv);
}

/* ------------------------------------------------------------------ */
/* Levels: creation */

static level *push_level(level_kind kind)
{
   level *lv;
   if (g_depth >= MAX_LEVELS)
      return NULL;
   lv = &g_levels[g_depth++];
   memset(lv, 0, sizeof(*lv));
   lv->kind = kind;
   g_dirty  = 1;
   return lv;
}

static void refresh_servers(level *lv)
{
   char *sel_id = NULL;
   if (lv->selected < lv->count)
      sel_id = util_strdup(lv->entries[lv->selected]->id);
   dlna_entries_free(lv->entries, lv->count);
   lv->entries = dlna_servers_list(&lv->count);
   sort_level(lv);
   lv->selected = 0;
   select_by_id(lv, sel_id);
   free(sel_id);
   g_dirty = 1;
}

static void fill_types(level *lv, int server)
{
   static const struct { const char *title; media_type type; } kinds[3] = {
      { "Audio",   MT_AUDIO },
      { "Picture", MT_IMAGE },
      { "Video",   MT_VIDEO }
   };
   int i;

   lv->entries = (dlna_entry**)util_calloc(3, sizeof(dlna_entry*));
   if (!lv->entries)
      return;
   for (i = 0; i < 3; i++)
   {
      dlna_entry *e = dlna_entry_new(ENT_TYPEROOT);
      if (!e)
         break;
      e->title  = util_strdup(kinds[i].title);
      e->media  = kinds[i].type;
      e->server = server;
      e->order  = i;
      lv->entries[lv->count++] = e;
   }
}

static void start_browse(level *lv)
{
   lv->loading  = 1;
   lv->progress = lv->total = 0;
   lv->token    = dlna_browse(lv->server, lv->container_id, lv->tree);
   if (!lv->token)
   {
      lv->loading = 0;
      util_replace(&lv->error, "The network is not available");
   }
}

/* "parent/escaped-name" as a new string (NULL if parent is NULL). */
static char *uri_join(const char *parent, const char *name)
{
   char  esc[1024];
   char *out;

   if (!parent)
      return NULL;
   util_uri_escape(name ? name : "", esc, sizeof(esc));
   out = (char*)malloc(strlen(parent) + strlen(esc) + 2);
   if (out)
      sprintf(out, "%s/%s", parent, esc);
   return out;
}

static char *server_uri(int server)
{
   char name[256], esc[800];
   char *out;
   dlna_server_uri_name(server, name, sizeof(name));
   util_uri_escape(name, esc, sizeof(esc));
   out = (char*)malloc(strlen(esc) + 8);
   if (out)
      sprintf(out, "dlna://%s", esc);
   return out;
}

/* Both take ownership of 'uri'. */
static void push_types(int server, const char *title, char *uri)
{
   level *lv = push_level(LV_TYPES);
   if (!lv)
   {
      free(uri);
      return;
   }
   lv->server = server;
   lv->title  = util_strdup(title);
   lv->uri    = uri;
   fill_types(lv, server);
}

static void push_folder(int server, const char *id, media_type tree,
      media_type division, const char *title, char *uri)
{
   level *lv = push_level(LV_FOLDER);
   if (!lv)
   {
      free(uri);
      return;
   }
   lv->server       = server;
   lv->container_id = util_strdup(id);
   lv->tree         = tree;
   lv->division     = division;
   lv->title        = util_strdup(title);
   lv->uri          = uri;
   start_browse(lv);
}

void ui_start_servers(void)
{
   level *lv;
   while (g_depth > 0)
      level_clear(&g_levels[--g_depth]);
   lv = push_level(LV_SERVERS);
   if (lv)
   {
      lv->title = util_strdup("Media Servers");
      refresh_servers(lv);
      g_servers_version = dlna_servers_version();
   }
}

void ui_start_uri(const char *uri)
{
   level *lv;
   while (g_depth > 0)
      level_clear(&g_levels[--g_depth]);
   lv = push_level(LV_RESOLVING);
   if (lv)
   {
      lv->title   = util_strdup(uri);
      lv->loading = 1;
      lv->token   = dlna_resolve(uri);
      if (!lv->token)
      {
         lv->loading = 0;
         util_replace(&lv->error, "The network is not available");
      }
   }
}

/* ------------------------------------------------------------------ */
/* Results */

static level *level_for_token(int token)
{
   int i;
   for (i = 0; i < g_depth; i++)
      if (g_levels[i].token == token)
         return &g_levels[i];
   return NULL;
}

static void handle_result(dlna_result *r)
{
   level *lv = level_for_token(r->token);
   if (!lv)
      return; /* the user navigated away meanwhile */
   g_dirty = 1;

   switch (r->kind)
   {
      case RES_PROGRESS:
         lv->progress = r->count;
         lv->total    = r->total;
         break;

      case RES_BROWSE:
         dlna_entries_free(lv->entries, lv->count);
         lv->entries  = r->entries;
         lv->count    = r->count;
         r->entries   = NULL;
         r->count     = 0;
         lv->loading  = 0;
         lv->token    = 0;
         lv->top_row  = 0;
         util_replace(&lv->container_id, r->container_id);
         /* Sort first: sort_level() keeps the selected entry selected,
          * which is right for an options change but not for a fresh list. */
         sort_level(lv);
         lv->selected = 0;
         select_by_id(lv, lv->select_id);
         ensure_visible(lv);
         break;

      case RES_RESOLVE:
         /* Replace the "resolving" level with the real top folder. */
         lv->token   = 0;
         lv->loading = 0;
         if (lv != &g_levels[0])
            break;
      {
         /* The top level's path is the one we were given (minus the file
          * name when it named a file). */
         char  *uri = util_strdup(lv->title);
         size_t len = uri ? strlen(uri) : 0;
         while (len > 7 && uri[len - 1] == '/')
            uri[--len] = '\0';
         if (uri && r->media)
         {
            char *slash = strrchr(uri, '/');
            if (slash && slash > uri + 7)
               *slash = '\0';
         }
         g_resolve_outcome = r->media ? UI_RESOLVE_MEDIA : UI_RESOLVE_FOLDER;
         level_clear(lv);
         g_depth = 0;
         if (!r->container_id && r->tree == MT_NONE)
            push_types(r->server, r->title, uri);
         else
         {
            push_folder(r->server, r->container_id, r->tree, r->division,
                  r->title, uri);
            if (r->media && g_depth > 0)
            {
               util_replace(&g_levels[0].select_id, r->media->id);
               dlna_entry_free(g_play_next);
               g_play_next = r->media;
               r->media    = NULL;
            }
         }
      }
         break;

      case RES_ERROR:
         if (lv->kind == LV_RESOLVING)
            g_resolve_outcome = UI_RESOLVE_FAILED;
         lv->loading = 0;
         lv->token   = 0;
         util_replace(&lv->error, r->error ? r->error : "Unknown error");
         break;
   }
}

/* ------------------------------------------------------------------ */
/* Init, layout and icons */

static void encode_utf8(long cp, char *out)
{
   if (cp < 0x80)
   {
      out[0] = (char)cp;
      out[1] = '\0';
   }
   else if (cp < 0x800)
   {
      out[0] = (char)(0xC0 | (cp >> 6));
      out[1] = (char)(0x80 | (cp & 0x3F));
      out[2] = '\0';
   }
   else if (cp < 0x10000L)
   {
      out[0] = (char)(0xE0 | (cp >> 12));
      out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
      out[2] = (char)(0x80 | (cp & 0x3F));
      out[3] = '\0';
   }
   else
   {
      out[0] = (char)(0xF0 | (cp >> 18));
      out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
      out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
      out[3] = (char)(0x80 | (cp & 0x3F));
      out[4] = '\0';
   }
}

int ui_init(const char *system_dir)
{
   char dir[1024];
   char path[1024];
   int  i, j;

   strncpy(g_system_dir, system_dir ? system_dir : "", sizeof(g_system_dir) - 1);
   util_path_join(dir, sizeof(dir), g_system_dir, "dlna");
   for (i = 0; i < ICON_COUNT; i++)
   {
      util_path_join(path, sizeof(path), dir, icon_files[i]);
      g_icon_src[i] = util_file_exists(path)
         ? image_load(path, 1024, 1024, 0, NULL) : NULL;
      if (g_icon_src[i])
         dlna_log(LOG_INFO, "Using icon %s\n", path);

      /* Choose the glyph fallback now that fonts are loaded. */
      for (j = 0; j < 5; j++)
         if (j == 4 || font_has_glyph(icon_glyphs[i][j]))
         {
            encode_utf8(icon_glyphs[i][j], g_icon_glyph[i]);
            break;
         }
   }
   /* No server icon: use the folder image for servers. */
   return 1;
}

void ui_deinit(void)
{
   int i;
   while (g_depth > 0)
      level_clear(&g_levels[--g_depth]);
   for (i = 0; i < ICON_COUNT; i++)
   {
      gfx_image_free(g_icon_src[i]);
      gfx_image_free(g_icon[i]);
      g_icon_src[i] = g_icon[i] = NULL;
   }
   dlna_entry_free(g_play_next);
   g_play_next = NULL;
}

void ui_layout(int width, int height, int font_px)
{
   layout L;
   int    title_pad, bottom_margin, avail_h;
   int    n, t_w, t_h, r, i;

   memset(&L, 0, sizeof(L));
   L.width   = width;
   L.height  = height;
   L.font_px = font_px;

   /* Title panel: one line of text plus padding, near the bottom. */
   title_pad     = font_px / 3 + 4;
   bottom_margin = height / 40 + 4;
   L.title.h     = font_line_height(font_px) + 2 * title_pad;
   L.title.y     = height - bottom_margin - L.title.h;

   /* Scroll arrow strips: wide and flat, above the grid and between the
    * grid and the title panel.  They get the room left over once the most
    * rows that fit have been placed (at most 2/5 of the title panel's
    * height, at least 10 pixels), so they never cost a row of thumbnails. */
   for (r = 1; ; r++)
      if ((long)L.title.y * GAP_RATIO_DEN
            < (long)MIN_THUMB * ((r + 1) * GAP_RATIO_DEN
               + (r + 2) * GAP_RATIO_NUM))
         break;
   {
      long need  = (long)MIN_THUMB * (r * GAP_RATIO_DEN
            + (r + 1) * GAP_RATIO_NUM) / GAP_RATIO_DEN + 1;
      long slack = (L.title.y - need - 4) / 2;
      L.arrow_up.h = L.title.h * 2 / 5;
      if (L.arrow_up.h > slack)
         L.arrow_up.h = (int)slack;
      if (L.arrow_up.h < 10)
         L.arrow_up.h = 10;
   }
   L.arrow_down.h = L.arrow_up.h;
   L.arrow_up.y   = 2;
   L.arrow_down.y = L.title.y - L.arrow_down.h - 2;
   avail_h        = L.title.y - 2 * L.arrow_up.h - 4;

   /* Columns: the most (1-4) that fit at MIN_THUMB with padding of a
    * tenth of a thumbnail on every side:  W >= n*t + (n+1)*t/10. */
   for (n = 4; n > 1; n--)
      if ((long)width * GAP_RATIO_DEN
            >= (long)MIN_THUMB * (n * GAP_RATIO_DEN + (n + 1) * GAP_RATIO_NUM))
         break;
   L.cols = n;
   t_w    = (int)((long)width * GAP_RATIO_DEN
         / (n * GAP_RATIO_DEN + (n + 1) * GAP_RATIO_NUM));

   /* Rows: as many as fit at MIN_THUMB (at least one), then the biggest
    * thumbnail that still lets that many rows fit. */
   for (r = 1; ; r++)
      if ((long)avail_h * GAP_RATIO_DEN
            < (long)MIN_THUMB * ((r + 1) * GAP_RATIO_DEN
               + (r + 2) * GAP_RATIO_NUM))
         break;
   L.rows = r;
   t_h    = (int)((long)avail_h * GAP_RATIO_DEN
         / (r * GAP_RATIO_DEN + (r + 1) * GAP_RATIO_NUM));

   L.thumb = t_w < t_h ? t_w : t_h;
   if (L.thumb < 16)
      L.thumb = 16;
   L.gap_x  = (width - L.cols * L.thumb) / (L.cols + 1);
   L.gap_y  = (avail_h - L.rows * L.thumb) / (L.rows + 1);
   if (L.gap_y < 2)
      L.gap_y = 2;
   L.grid_x = L.gap_x + (width - L.cols * L.thumb - (L.cols + 1) * L.gap_x) / 2;
   L.grid_y = L.arrow_up.y + L.arrow_up.h + L.gap_y;
   L.border = L.thumb / 64 + 2;

   L.title.x = L.gap_x;
   L.title.w = width - 2 * L.gap_x;
   if (L.title.w < width / 2)
   {
      L.title.x = width / 20;
      L.title.w = width - width / 10;
   }
   L.arrow_up.x   = L.arrow_down.x = L.title.x;
   L.arrow_up.w   = L.arrow_down.w = L.title.w;

   g_lay = L;
   dlna_log(LOG_INFO, "Layout %dx%d: %d x %d thumbnails of %d px\n",
         width, height, L.cols, L.rows, L.thumb);

   thumbs_set_size(L.thumb);
   dlna_set_thumb_hint(L.thumb);

   for (i = 0; i < ICON_COUNT; i++)
   {
      gfx_image_free(g_icon[i]);
      g_icon[i] = g_icon_src[i]
         ? gfx_image_fit(g_icon_src[i], L.thumb * 8 / 10, L.thumb * 8 / 10)
         : NULL;
   }
   for (i = 0; i < g_depth; i++)
      ensure_visible(&g_levels[i]);
   g_dirty = 1;
}

/* ------------------------------------------------------------------ */
/* Update */

static dlna_entry *open_selected(level *lv)
{
   dlna_entry *e;

   if (lv->selected >= lv->count)
      return NULL;
   e = lv->entries[lv->selected];
   switch (e->kind)
   {
      case ENT_SERVER:
         push_types(e->server, e->title, server_uri(e->server));
         break;
      case ENT_TYPEROOT:
         push_folder(e->server, NULL, e->media, e->media, e->title,
               uri_join(lv->uri, e->title));
         break;
      case ENT_FOLDER:
         push_folder(e->server, e->id, lv->tree, lv->division, e->title,
               uri_join(lv->uri, e->title));
         break;
      case ENT_MEDIA:
         return dlna_entry_copy(e);
   }
   return NULL;
}

static void go_back(void)
{
   /* The bottom level is the top folder: never leave it. */
   if (g_depth <= 1)
      return;
   level_clear(&g_levels[--g_depth]);
   g_dirty = 1;
}

static void refresh(level *lv)
{
   if (lv->kind == LV_SERVERS)
   {
      dlna_search();
      ui_set_message("Searching for media servers...", 2.0);
   }
   else if (lv->kind == LV_FOLDER && !lv->loading)
   {
      if (lv->selected < lv->count)
         util_replace(&lv->select_id, lv->entries[lv->selected]->id);
      util_replace(&lv->error, NULL);
      start_browse(lv);
   }
   g_dirty = 1;
}

dlna_entry *ui_update(double dt)
{
   dlna_result *r;
   level       *lv;
   dlna_entry  *play = NULL;
   size_t       n, page, before;

   g_time += dt;
   while ((r = dlna_poll()) != NULL)
   {
      handle_result(r);
      dlna_result_free(r);
   }
   if (g_play_next)
   {
      play        = g_play_next;
      g_play_next = NULL;
      return play;
   }

   lv = top_level();
   if (!lv)
      return NULL;
   if (lv->kind == LV_SERVERS && dlna_servers_version() != g_servers_version)
   {
      g_servers_version = dlna_servers_version();
      refresh_servers(lv);
   }

   n      = lv->count;
   page   = (size_t)g_lay.cols * (size_t)g_lay.rows;
   before = lv->selected;
   if (n > 0 && g_lay.cols > 0)
   {
      size_t cols     = (size_t)g_lay.cols;
      size_t last_row = (n - 1) / cols;
      size_t col      = lv->selected % cols;

      /* Everything wraps around: past either end of the list you come
       * out at the other end (handy in folders with thousands of items). */
      if (input_repeat(ACT_LEFT, 0.35, 0.08))
         lv->selected = lv->selected > 0 ? lv->selected - 1 : n - 1;
      if (input_repeat(ACT_RIGHT, 0.35, 0.08))
         lv->selected = lv->selected + 1 < n ? lv->selected + 1 : 0;
      col = lv->selected % cols;
      if (input_repeat(ACT_UP, 0.35, 0.08))
      {
         if (lv->selected >= cols)
            lv->selected -= cols;
         else
         {
            /* Top row: same column in the last row (or the last item if
             * the last row is shorter). */
            lv->selected = last_row * cols + col;
            if (lv->selected >= n)
               lv->selected = n - 1;
         }
      }
      if (input_repeat(ACT_DOWN, 0.35, 0.08))
      {
         if (lv->selected + cols < n)
            lv->selected += cols;
         else if (lv->selected / cols < last_row)
            lv->selected = n - 1; /* partial last row */
         else
            lv->selected = col;   /* bottom row: back to the top */
      }
      if (input_repeat(ACT_SKIP_FWD, 0.4, 0.2))
      {
         /* One screenful down; from the last item, back to the start. */
         if (lv->selected == n - 1)
         {
            lv->selected = 0;
            lv->top_row  = 0;
         }
         else
         {
            lv->top_row  += g_lay.rows;
            lv->selected  = lv->selected + page < n ? lv->selected + page : n - 1;
         }
      }
      if (input_repeat(ACT_SKIP_BACK, 0.4, 0.2))
      {
         /* One screenful up; from the first item, to the end. */
         if (lv->selected == 0)
            lv->selected = n - 1;
         else
         {
            lv->top_row  -= g_lay.rows;
            lv->selected  = lv->selected >= page ? lv->selected - page : 0;
         }
      }
      ensure_visible(lv);
      if (lv->selected != before)
         g_dirty = 1;
   }

   if (input_pressed(ACT_OK))
      play = open_selected(lv);
   else if (input_pressed(ACT_BACK))
      go_back();
   else if (input_pressed(ACT_PAUSE))
      refresh(lv);
   return play;
}

ui_resolve_outcome ui_take_resolve_outcome(void)
{
   ui_resolve_outcome o = g_resolve_outcome;
   g_resolve_outcome = UI_RESOLVE_PENDING;
   return o;
}

char *ui_folder_uri(void)
{
   level *lv = top_level();
   return lv && lv->kind != LV_RESOLVING ? util_strdup(lv->uri) : NULL;
}

char *ui_media_uri(const dlna_entry *e)
{
   level *lv = top_level();
   char   ext[16], name[1100], tail[20];
   size_t tlen, elen;

   if (!lv || !lv->uri || !e || !e->title)
      return NULL;
   /* Add the file's extension so the path says what kind of media it is
    * (unless the title already ends with it). */
   strncpy(name, e->title, 1000);
   name[1000] = '\0';
   util_extension(e->url, ext, sizeof(ext));
   tlen = strlen(name);
   elen = strlen(ext);
   if (elen)
   {
      sprintf(tail, ".%s", ext);
      if (tlen <= elen || util_strcasecmp(name + tlen - elen - 1, tail) != 0)
         strcat(name, tail);
   }
   return uri_join(lv->uri, name);
}

int ui_needs_redraw(void)
{
   level *lv = top_level();
   int    d  = g_dirty;
   if (thumbs_take_dirty())
      d = 1;
   if (lv && lv->loading)
      d = 1; /* animated "Loading" */
   if (g_message[0])
      d = 1;
   return d;
}

void ui_set_message(const char *text, double seconds)
{
   strncpy(g_message, text ? text : "", sizeof(g_message) - 1);
   g_message[sizeof(g_message) - 1] = '\0';
   g_message_until = g_time + seconds;
   g_dirty         = 1;
}

/* ------------------------------------------------------------------ */
/* Drawing */

static int icon_for_entry(const level *lv, const dlna_entry *e)
{
   switch (e->kind)
   {
      case ENT_SERVER:
         return ICON_SERVER;
      case ENT_FOLDER:
         switch (lv->division)
         {
            case MT_IMAGE: return ICON_FOLDER_PICTURES;
            case MT_AUDIO: return ICON_FOLDER_AUDIO;
            case MT_VIDEO: return ICON_FOLDER_VIDEO;
            default:       break;
         }
         return ICON_FOLDER;
      case ENT_TYPEROOT:
      case ENT_MEDIA:
         if (e->media == MT_IMAGE)
            return ICON_IMAGE;
         if (e->media == MT_AUDIO)
            return ICON_AUDIO;
         return ICON_VIDEO;
   }
   return ICON_FOLDER;
}

/* Draw icon 'which' (or its glyph) centred in r.  Returns the rectangle
 * the icon picture actually covers (r itself for a glyph). */
static gfx_rect draw_icon(gfx_surface *fb, int which, gfx_rect r, int prescaled)
{
   const gfx_image *img;
   /* Division folder icons fall back to the plain folder icon. */
   if (which >= ICON_FOLDER_PICTURES && !g_icon_src[which])
      which = ICON_FOLDER;
   img = prescaled ? g_icon[which] : NULL;
   /* Servers without server.png use the folder image. */
   if (which == ICON_SERVER && !g_icon_src[ICON_SERVER])
   {
      if (g_icon_src[ICON_FOLDER])
         which = ICON_FOLDER;
      img = prescaled ? g_icon[which] : NULL;
   }
   if (img)
   {
      gfx_rect out;
      out.x = r.x + (r.w - img->width) / 2;
      out.y = r.y + (r.h - img->height) / 2;
      out.w = img->width;
      out.h = img->height;
      gfx_blit(fb, img, out.x, out.y, 255);
      return out;
   }
   if (g_icon_src[which])
   {
      gfx_rect fit = gfx_fit_aspect(r,
            (double)g_icon_src[which]->width / g_icon_src[which]->height);
      gfx_blit_scaled(fb, g_icon_src[which], fit.x, fit.y, fit.w, fit.h, 255);
      return fit;
   }
   {
      const char *glyph = g_icon_glyph[which];
      int         px    = r.h * 55 / 100;
      int         w     = font_text_width(glyph, px);
      int         base  = r.y + (r.h + font_ascent(px) - font_descent(px)) / 2;
      font_draw(fb, glyph, r.x + (r.w - w) / 2, base, px,
            g_cfg.title_text, 200);
   }
   return r;
}

void ui_draw_icon(gfx_surface *fb, media_type type, int folder, gfx_rect r)
{
   int which = folder ? ICON_FOLDER
      : type == MT_IMAGE ? ICON_IMAGE
      : type == MT_AUDIO ? ICON_AUDIO : ICON_VIDEO;
   draw_icon(fb, which, r, 0);
}

static void draw_centered_text(gfx_surface *fb, const char *text, int y,
      unsigned alpha)
{
   char buf[512];
   int  px = g_lay.font_px;
   int  w  = font_ellipsize(text, px, fb->width * 9 / 10, buf, sizeof(buf));
   font_draw(fb, buf, (fb->width - w) / 2, y, px, g_cfg.title_text, alpha);
}

static void draw_cell(gfx_surface *fb, const level *lv, const dlna_entry *e,
      int x, int y,
      int selected, int prefetch)
{
   const gfx_image *img = NULL;
   int              failed = 0;
   gfx_rect         r, content;

   r.x = x;
   r.y = y;
   r.w = g_lay.thumb;
   r.h = g_lay.thumb;

   if (prefetch)
   {
      if (e->thumb_url)
         thumbs_get(e->thumb_url, 1, &failed);
      return;
   }

   /* The box behind the thumbnail ("image" without selection.png falls
    * back to a colour that contrasts with the selection border). */
   skin_fill(fb, r, SKIN_SELECTION, g_cfg.sel_bg,
         skin_contrast(g_cfg.sel_border), 255, 0);
   if (e->thumb_url)
      img = thumbs_get(e->thumb_url, 0, &failed);
   if (img)
   {
      content.x = x + (r.w - img->width) / 2;
      content.y = y + (r.h - img->height) / 2;
      content.w = img->width;
      content.h = img->height;
      gfx_blit(fb, img, content.x, content.y, 255);
   }
   else
      content = draw_icon(fb, icon_for_entry(lv, e), r, 1);

   if (selected)
   {
      /* Without a box, frame the picture itself (e.g. a 328x246 frame for
       * a landscape picture) rather than the empty square around it. */
      gfx_rect f = COLOR_IS_TRANSPARENT(g_cfg.sel_bg) ? content : r;
      int      t = skin_frame_thickness(fb, g_cfg.sel_border, g_lay.border);
      int      o = t + g_lay.border / 2 + 1;
      f.x -= o;
      f.y -= o;
      f.w += 2 * o;
      f.h += 2 * o;
      /* With "image" but no borders.png: a colour that stands out against
       * the box (or the browser background without a box). */
      skin_frame(fb, f, t, g_cfg.sel_border,
            COLOR_IS_SOLID(g_cfg.sel_bg) ? skin_contrast(g_cfg.sel_bg)
            : COLOR_IS_SOLID(g_cfg.nav_bg) ? skin_contrast(g_cfg.nav_bg)
            : 0xFFFFFF, 255);
   }
}

static void draw_title_panel(gfx_surface *fb, level *lv)
{
   gfx_rect    r   = g_lay.title;
   int         px  = g_lay.font_px;
   int         pad = px / 2 + 4;
   char        counter[64];
   char        buf[1024];
   const char *title;
   int         cw = 0, tw, max_w;

   overlay_panel(fb, r, 0);

   counter[0] = '\0';
   if (lv->count > 0)
   {
      sprintf(counter, "%lu/%lu", (unsigned long)(lv->selected + 1),
            (unsigned long)lv->count);
      cw = font_text_width(counter, px);
   }
   if (lv->count > 0 && lv->selected < lv->count)
      title = lv->entries[lv->selected]->title;
   else
      title = lv->title ? lv->title : "";

   /* Keep the title centred: reserve the counter's width on both sides. */
   max_w = r.w - 2 * pad - 2 * (cw ? cw + pad : 0);
   if (max_w < r.w / 3)
   {
      max_w      = r.w - 2 * pad;
      counter[0] = '\0';
   }
   tw = font_ellipsize(title, px, max_w, buf, sizeof(buf));
   font_draw(fb, buf, r.x + (r.w - tw) / 2,
         r.y + (r.h - font_line_height(px)) / 2 + font_ascent(px), px,
         g_cfg.title_text, 255);
   if (counter[0])
      font_draw(fb, counter, r.x + r.w - pad - cw,
            r.y + (r.h - font_line_height(px)) / 2 + font_ascent(px), px,
            g_cfg.title_text, 150);
}

void ui_render(gfx_surface *fb)
{
   level *lv = top_level();
   int    mid = g_lay.title.y / 2;

   g_dirty = 0;
   thumbs_begin_frame();
   {
      gfx_rect all;
      all.x = 0;
      all.y = 0;
      all.w = fb->width;
      all.h = fb->height;
      skin_fill(fb, all, SKIN_NAVIGATION, g_cfg.nav_bg, 0x101318, 255, 1);
   }
   if (!lv)
      return;

   if (lv->count > 0)
   {
      size_t first = (size_t)lv->top_row * (size_t)g_lay.cols;
      size_t page  = (size_t)g_lay.cols * (size_t)g_lay.rows;
      size_t i;

      for (i = 0; i < page && first + i < lv->count; i++)
      {
         int col = (int)(i % (size_t)g_lay.cols);
         int row = (int)(i / (size_t)g_lay.cols);
         int x   = g_lay.grid_x + col * (g_lay.thumb + g_lay.gap_x);
         int y   = g_lay.grid_y + row * (g_lay.thumb + g_lay.gap_y);
         draw_cell(fb, lv, lv->entries[first + i], x, y,
               first + i == lv->selected, 0);
      }
      /* Warm the cache with the next screenful. */
      for (i = 0; i < page && first + page + i < lv->count; i++)
         draw_cell(fb, lv, lv->entries[first + page + i], 0, 0, 0, 1);
   }
   else if (lv->error)
   {
      draw_centered_text(fb, lv->error, mid, 255);
      draw_centered_text(fb, g_depth > 1 ? "Press Back to return"
            : "Press Start to try again", mid + font_line_height(g_lay.font_px)
            * 3 / 2, 160);
   }
   else if (lv->loading || lv->kind == LV_SERVERS)
   {
      static const char *const dots[4] = { "", ".", "..", "..." };
      char buf[256];
      int  phase = (int)(g_time * 3.0) % 4;
      if (lv->kind == LV_SERVERS)
         sprintf(buf, "Searching for media servers%s", dots[phase]);
      else if (lv->kind == LV_RESOLVING)
         sprintf(buf, "Finding %.180s%s", lv->title ? lv->title : "", dots[phase]);
      else if (lv->total > 0)
         sprintf(buf, "Loading %lu / %lu%s", (unsigned long)lv->progress,
               (unsigned long)lv->total, dots[phase]);
      else
         sprintf(buf, "Loading%s", dots[phase]);
      draw_centered_text(fb, buf, mid, 255);
      if (lv->kind == LV_SERVERS)
         draw_centered_text(fb, "Press Start to search again",
               mid + font_line_height(g_lay.font_px) * 3 / 2, 140);
   }
   else
      draw_centered_text(fb, "This folder is empty", mid, 200);

   /* Arrows showing that the list continues above / below the screen. */
   if (lv->count > 0 && g_lay.cols > 0)
   {
      long     total_rows = (long)((lv->count + g_lay.cols - 1) / g_lay.cols);
      uint32_t fallback   = COLOR_IS_SOLID(g_cfg.title_border)
         ? g_cfg.title_border : 0x7FB2FF;
      if (lv->top_row > 0)
         skin_arrow(fb, g_lay.arrow_up, 0, g_cfg.arrow_color, fallback,
               g_cfg.arrow_alpha);
      if (lv->top_row + g_lay.rows < total_rows)
         skin_arrow(fb, g_lay.arrow_down, 1, g_cfg.arrow_color, fallback,
               g_cfg.arrow_alpha);
   }

   draw_title_panel(fb, lv);

   if (g_message[0])
   {
      if (g_time < g_message_until)
         overlay_toast(fb, g_message);
      else
      {
         g_message[0] = '\0';
         g_dirty      = 1;
      }
   }
}
