/*
 * skin.c - see skin.h.
 */
#include <stdlib.h>
#include <string.h>

#include "skin.h"
#include "image.h"
#include "options.h"
#include "util.h"

static const char *const skin_files[SKIN_COUNT] = {
   "information.png", "title.png", "selection.png", "navigation.png",
   "playback.png", "borders.png", "arrows/backward.png", "arrows/forward.png"
};

/* The eight pieces of borders.png, in this order. */
enum { NS_TL = 0, NS_T, NS_TR, NS_L, NS_R, NS_BL, NS_B, NS_BR, NS_COUNT };

static gfx_image *g_ns_piece[NS_COUNT];   /* cut from borders.png        */
static gfx_image *g_ns_scaled[NS_COUNT];  /* scaled to the frame size    */
static int        g_ns_corner;            /* corner size they were cut at */
static int        g_ns_side;              /* edge length they were cut at */
static int        g_ns_size;              /* thickness they were scaled to */

/* With Fit, each edge is drawn from one image the exact length of the
 * edge, made by laying whole edge pieces end to end and resizing the
 * row.  Panels, boxes and pop-ups have different sizes, so a few lengths
 * are kept for each direction (replaced in turn). */
#define FIT_SLOTS 8
typedef struct fit_strip
{
   int        len;     /* edge length; 0 = unused slot              */
   gfx_image *img[2];  /* top and bottom, or left and right         */
} fit_strip;

static fit_strip g_fit[2][FIT_SLOTS];     /* [0] horizontal, [1] vertical */
static int       g_fit_next[2];           /* slot to replace next        */

typedef struct skin_image
{
   gfx_image *source;  /* as loaded from disk                */
   gfx_image *scaled;  /* cached copy for the last size asked */
   int        w, h, cover;
} skin_image;

static skin_image g_skin[SKIN_COUNT];

void skin_init(const char *system_dir)
{
   char dir[1024], path[1024];
   int  i;

   skin_deinit();
   util_path_join(dir, sizeof(dir), system_dir, "dlna");
   for (i = 0; i < SKIN_COUNT; i++)
   {
      util_path_join(path, sizeof(path), dir, skin_files[i]);
      if (!util_file_exists(path))
         continue;
      /* Large limit: navigation/playback images may be full screen. */
      g_skin[i].source = image_load(path, 4096, 4096, 0, NULL);
      if (g_skin[i].source)
         dlna_log(LOG_INFO, "Using background image %s\n", path);
   }
}

static void free_fit_strips(void)
{
   int d, i;
   for (d = 0; d < 2; d++)
   {
      for (i = 0; i < FIT_SLOTS; i++)
      {
         gfx_image_free(g_fit[d][i].img[0]);
         gfx_image_free(g_fit[d][i].img[1]);
         g_fit[d][i].img[0] = g_fit[d][i].img[1] = NULL;
         g_fit[d][i].len = 0;
      }
      g_fit_next[d] = 0;
   }
}

static void free_pieces(void)
{
   int i;
   free_fit_strips();
   for (i = 0; i < NS_COUNT; i++)
   {
      gfx_image_free(g_ns_piece[i]);
      gfx_image_free(g_ns_scaled[i]);
      g_ns_piece[i] = g_ns_scaled[i] = NULL;
   }
   g_ns_corner = g_ns_side = g_ns_size = 0;
}

void skin_deinit(void)
{
   int i;
   free_pieces();
   for (i = 0; i < SKIN_COUNT; i++)
   {
      gfx_image_free(g_skin[i].source);
      gfx_image_free(g_skin[i].scaled);
      g_skin[i].source = g_skin[i].scaled = NULL;
   }
}

int skin_has_image(skin_part part)
{
   return g_skin[part].source != NULL;
}

uint32_t skin_contrast(uint32_t rgb)
{
   /* Perceived brightness (ITU-R BT.601 weights, scaled by 1000). */
   unsigned long luma = ((rgb >> 16) & 0xFF) * 299UL
      + ((rgb >> 8) & 0xFF) * 587UL + (rgb & 0xFF) * 114UL;
   return luma > 128000UL ? 0x000000 : 0xFFFFFF;
}

/* The part's image at the requested size, cached because the same size is
 * asked for over and over (every thumbnail box, every frame). */
static const gfx_image *scaled_image(skin_part part, int w, int h, int cover,
      int *ox, int *oy)
{
   skin_image *s = &g_skin[part];
   int         sw = w, sh = h;

   *ox = 0;
   *oy = 0;
   if (cover)
   {
      /* Scale so the image covers w x h, keeping its shape. */
      if ((long)s->source->width * h > (long)s->source->height * w)
         sw = (int)((long)s->source->width * h / s->source->height);
      else
         sh = (int)((long)s->source->height * w / s->source->width);
      *ox = (w - sw) / 2;
      *oy = (h - sh) / 2;
   }
   if (!s->scaled || s->w != w || s->h != h || s->cover != cover)
   {
      gfx_image_free(s->scaled);
      s->scaled = gfx_image_resize(s->source, sw, sh);
      s->w      = w;
      s->h      = h;
      s->cover  = cover;
   }
   return s->scaled;
}

void skin_fill(gfx_surface *fb, gfx_rect r, skin_part part, uint32_t setting,
      uint32_t fallback, unsigned alpha, int cover)
{
   if (r.w <= 0 || r.h <= 0 || COLOR_IS_TRANSPARENT(setting))
      return;
   if (COLOR_IS_IMAGE(setting))
   {
      if (g_skin[part].source)
      {
         int              ox, oy;
         const gfx_image *img = scaled_image(part, r.w, r.h, cover, &ox, &oy);
         if (img)
         {
            /* Clip the (possibly larger, for 'cover') image to r. */
            gfx_surface sub;
            gfx_rect    clip = r;
            if (!gfx_clip(fb, &clip))
               return;
            sub.pixels = fb->pixels + (size_t)clip.y * fb->pitch + clip.x;
            sub.width  = clip.w;
            sub.height = clip.h;
            sub.pitch  = fb->pitch;
            gfx_blit(&sub, img, r.x - clip.x + ox, r.y - clip.y + oy, alpha);
            return;
         }
      }
      setting = fallback;
   }
   gfx_fill(fb, r.x, r.y, r.w, r.h, setting & 0x00FFFFFFu, alpha);
}

/* Cut (for new corner/side sizes) and scale (for a new frame thickness)
 * the eight nine-slice pieces.  Returns 0 if borders.png is missing or too
 * small. */
static int prepare_pieces(int size)
{
   const gfx_image *src  = g_skin[SKIN_BORDERS].source;
   int              c    = g_cfg.border_corner;
   int              side = g_cfg.border_side;
   int              len, i;

   if (!src || src->width < 2 * c + side || src->height < 2 * c + side)
      return 0;
   if (g_ns_corner != c || g_ns_side != side)
   {
      /* Edge pieces come from the middle of each edge: for an image of
       * exactly corner + side + corner pixels that is everything between
       * the corners; a bigger image has its extra middle part left out. */
      int mx = (src->width - side) / 2, my = (src->height - side) / 2;
      int rx = src->width - c, by = src->height - c;
      free_pieces();
      g_ns_piece[NS_TL] = gfx_image_crop(src, 0,  0,  c,    c);
      g_ns_piece[NS_T]  = gfx_image_crop(src, mx, 0,  side, c);
      g_ns_piece[NS_TR] = gfx_image_crop(src, rx, 0,  c,    c);
      g_ns_piece[NS_L]  = gfx_image_crop(src, 0,  my, c,    side);
      g_ns_piece[NS_R]  = gfx_image_crop(src, rx, my, c,    side);
      g_ns_piece[NS_BL] = gfx_image_crop(src, 0,  by, c,    c);
      g_ns_piece[NS_B]  = gfx_image_crop(src, mx, by, side, c);
      g_ns_piece[NS_BR] = gfx_image_crop(src, rx, by, c,    c);
      g_ns_corner = c;
      g_ns_side   = side;
   }
   if (g_ns_size != size)
   {
      /* Corners become size x size; edge pieces keep their proportion to
       * the corners along the edge and are size thick across it. */
      len = (int)((long)side * size / c);
      if (len < 1)
         len = 1;
      free_fit_strips();
      for (i = 0; i < NS_COUNT; i++)
      {
         int w = size, h = size;
         if (i == NS_T || i == NS_B)
            w = len;
         else if (i == NS_L || i == NS_R)
            h = len;
         gfx_image_free(g_ns_scaled[i]);
         g_ns_scaled[i] = g_ns_piece[i]
            ? gfx_image_resize(g_ns_piece[i], w, h) : NULL;
      }
      g_ns_size = size;
   }
   for (i = 0; i < NS_COUNT; i++)
      if (!g_ns_scaled[i])
         return 0;
   return 1;
}

int skin_frame_thickness(const gfx_surface *fb, uint32_t setting,
      int thickness)
{
   if (COLOR_IS_IMAGE(setting) && g_skin[SKIN_BORDERS].source)
   {
      /* One image pixel per screen pixel at 720 lines, scaled from there. */
      int t = g_cfg.border_corner * fb->height / 720;
      return t < 2 ? 2 : t;
   }
   return thickness;
}

/* Repeat a piece along a strip, clipping the last copy to the strip. */
static void tile(gfx_surface *fb, const gfx_image *piece, gfx_rect strip,
      int horizontal, unsigned alpha)
{
   gfx_surface sub;
   gfx_rect    clip = strip;
   int         pos;

   if (strip.w <= 0 || strip.h <= 0 || !gfx_clip(fb, &clip))
      return;
   sub.pixels = fb->pixels + (size_t)clip.y * fb->pitch + clip.x;
   sub.width  = clip.w;
   sub.height = clip.h;
   sub.pitch  = fb->pitch;
   if (horizontal)
      for (pos = 0; pos < strip.w; pos += piece->width)
         gfx_blit(&sub, piece, strip.x - clip.x + pos, strip.y - clip.y, alpha);
   else
      for (pos = 0; pos < strip.h; pos += piece->height)
         gfx_blit(&sub, piece, strip.x - clip.x, strip.y - clip.y + pos, alpha);
}

/* Whole copies of an unscaled edge piece laid end to end, resized to
 * 'len' along the edge and 't' across it. */
static gfx_image *make_fit_strip(const gfx_image *piece, int horizontal,
      int copies, int len, int t)
{
   gfx_image *row, *out;
   int        i, y;

   if (horizontal)
      row = gfx_image_new(piece->width * copies, piece->height);
   else
      row = gfx_image_new(piece->width, piece->height * copies);
   if (!row)
      return NULL;
   /* Straight copies (no blending), so the row keeps the piece's alpha. */
   for (i = 0; i < copies; i++)
      for (y = 0; y < piece->height; y++)
      {
         uint32_t *to = horizontal
            ? row->pixels + (size_t)y * row->width + (size_t)i * piece->width
            : row->pixels + ((size_t)i * piece->height + y) * row->width;
         memcpy(to, piece->pixels + (size_t)y * piece->width,
               (size_t)piece->width * sizeof(uint32_t));
      }
   out = horizontal ? gfx_image_resize(row, len, t)
                    : gfx_image_resize(row, t, len);
   gfx_image_free(row);
   return out;
}

/* The two fitted edges (top/bottom or left/right) for an edge 'len'
 * pixels long, from the cache or made now.  NULL if out of memory. */
static fit_strip *get_fit_strip(int horizontal, int len, int t)
{
   int        d = horizontal ? 0 : 1;
   int        a = horizontal ? NS_T : NS_L;
   int        b = horizontal ? NS_B : NS_R;
   int        scaled, copies, i;
   fit_strip *f;

   for (i = 0; i < FIT_SLOTS; i++)
      if (g_fit[d][i].len == len)
         return &g_fit[d][i];
   /* As many whole pieces (at their scaled size) as come closest. */
   scaled = horizontal ? g_ns_scaled[a]->width : g_ns_scaled[a]->height;
   copies = (len + scaled / 2) / scaled;
   if (copies < 1)
      copies = 1;
   f = &g_fit[d][g_fit_next[d]];
   g_fit_next[d] = (g_fit_next[d] + 1) % FIT_SLOTS;
   gfx_image_free(f->img[0]);
   gfx_image_free(f->img[1]);
   f->img[0] = make_fit_strip(g_ns_piece[a], horizontal, copies, len, t);
   f->img[1] = make_fit_strip(g_ns_piece[b], horizontal, copies, len, t);
   f->len    = len;
   if (!f->img[0] || !f->img[1])
   {
      gfx_image_free(f->img[0]);
      gfx_image_free(f->img[1]);
      f->img[0] = f->img[1] = NULL;
      f->len    = 0;
      return NULL;
   }
   return f;
}

static void nine_slice(gfx_surface *fb, gfx_rect r, int t, unsigned alpha)
{
   gfx_rect   strip;
   fit_strip *h = NULL, *v = NULL;

   /* Fit: one image per edge, exactly as long as the edge.  Clip (or if
    * memory runs out): pieces repeated and the last one cut off. */
   if (g_cfg.border_fit && r.w > 2 * t && r.h > 2 * t)
   {
      h = get_fit_strip(1, r.w - 2 * t, t);
      v = get_fit_strip(0, r.h - 2 * t, t);
   }

   /* Edges first, then the corners on top. */
   strip.x = r.x + t;
   strip.w = r.w - 2 * t;
   strip.h = t;
   strip.y = r.y;
   tile(fb, h ? h->img[0] : g_ns_scaled[NS_T], strip, 1, alpha);
   strip.y = r.y + r.h - t;
   tile(fb, h ? h->img[1] : g_ns_scaled[NS_B], strip, 1, alpha);
   strip.x = r.x;
   strip.y = r.y + t;
   strip.w = t;
   strip.h = r.h - 2 * t;
   tile(fb, v ? v->img[0] : g_ns_scaled[NS_L], strip, 0, alpha);
   strip.x = r.x + r.w - t;
   tile(fb, v ? v->img[1] : g_ns_scaled[NS_R], strip, 0, alpha);
   gfx_blit(fb, g_ns_scaled[NS_TL], r.x, r.y, alpha);
   gfx_blit(fb, g_ns_scaled[NS_TR], r.x + r.w - t, r.y, alpha);
   gfx_blit(fb, g_ns_scaled[NS_BL], r.x, r.y + r.h - t, alpha);
   gfx_blit(fb, g_ns_scaled[NS_BR], r.x + r.w - t, r.y + r.h - t, alpha);
}

void skin_frame(gfx_surface *fb, gfx_rect r, int thickness, uint32_t setting,
      uint32_t fallback, unsigned alpha)
{
   if (COLOR_IS_TRANSPARENT(setting))
      return;
   if (COLOR_IS_IMAGE(setting))
   {
      int t = skin_frame_thickness(fb, setting, thickness);
      if (r.w >= 2 * t && r.h >= 2 * t && prepare_pieces(t))
      {
         nine_slice(fb, r, t, alpha);
         return;
      }
      setting = fallback;
   }
   gfx_frame(fb, r.x, r.y, r.w, r.h, thickness, setting & 0x00FFFFFFu, alpha);
}

void skin_arrow(gfx_surface *fb, gfx_rect r, int down, uint32_t setting,
      uint32_t fallback, unsigned alpha)
{
   skin_part part = down ? SKIN_ARROW_FWD : SKIN_ARROW_BACK;

   if (COLOR_IS_TRANSPARENT(setting) || r.w <= 0 || r.h <= 0)
      return;
   if (COLOR_IS_IMAGE(setting))
   {
      const gfx_image *src = g_skin[part].source;
      if (src)
      {
         /* Keep the image's shape, fit it in r, centre it. */
         gfx_rect fit = gfx_fit_aspect(r, (double)src->width / src->height);
         int      ox, oy;
         const gfx_image *img = scaled_image(part, fit.w, fit.h, 0, &ox, &oy);
         if (img)
            gfx_blit(fb, img, fit.x, fit.y, alpha);
         return;
      }
      setting = fallback;
   }
   {
      /* A wide, flat arrowhead: half the strip wide, most of it tall. */
      double cx = r.x + r.w / 2.0;
      double hb = r.w / 4.0;
      double top = r.y + r.h * 0.1, bottom = r.y + r.h * 0.9;
      if (down)
         gfx_fill_triangle(fb, cx, bottom, top, hb, setting & 0x00FFFFFFu, alpha);
      else
         gfx_fill_triangle(fb, cx, top, bottom, hb, setting & 0x00FFFFFFu, alpha);
   }
}
