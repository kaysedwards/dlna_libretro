/*
 * overlay.c - see overlay.h.
 */
#include <stdio.h>
#include <string.h>

#include "overlay.h"
#include "font.h"
#include "options.h"
#include "skin.h"
#include "util.h"

int overlay_border_px(const gfx_surface *fb)
{
   int b = fb->height / 240;
   return b < 2 ? 2 : b;
}

void overlay_panel(gfx_surface *fb, gfx_rect r, int info)
{
   uint32_t border = info ? g_cfg.info_border : g_cfg.title_border;
   uint32_t bg     = info ? g_cfg.info_bg : g_cfg.title_bg;
   uint32_t text   = info ? g_cfg.info_text : g_cfg.title_text;

   /* With "image" but no image file, use a colour the text stands out
    * against (a background in the text colour itself would hide it). */
   skin_fill(fb, r, info ? SKIN_INFO : SKIN_TITLE, bg, skin_contrast(text),
         g_cfg.panel_alpha, 0);
   skin_frame(fb, r, overlay_border_px(fb), border, text, 255);
}

void overlay_title(gfx_surface *fb, const char *text)
{
   int      px     = g_cfg.font_size;
   int      pad    = px / 2 + 4;
   int      max_w  = fb->width * 9 / 10 - 2 * pad;
   int      margin = fb->height / 20;
   char     buf[1024];
   int      tw;
   gfx_rect r;

   tw  = font_ellipsize(text, px, max_w, buf, sizeof(buf));
   r.w = tw + 2 * pad;
   r.h = font_line_height(px) + 2 * pad;
   r.x = (fb->width - r.w) / 2;
   r.y = fb->height - margin - r.h;
   overlay_panel(fb, r, 0);
   font_draw(fb, buf, r.x + pad, r.y + pad + font_ascent(px), px,
         g_cfg.title_text, 255);
}

void overlay_toast(gfx_surface *fb, const char *text)
{
   int      px    = g_cfg.font_size * 4 / 5;
   int      pad   = px / 2 + 4;
   int      max_w = fb->width * 8 / 10 - 2 * pad;
   char     buf[512];
   int      tw;
   gfx_rect r;

   if (px < 12)
      px = 12;
   tw  = font_ellipsize(text, px, max_w, buf, sizeof(buf));
   r.w = tw + 2 * pad;
   r.h = font_line_height(px) + 2 * pad;
   r.x = (fb->width - r.w) / 2;
   r.y = fb->height / 20;
   overlay_panel(fb, r, 1);
   font_draw(fb, buf, r.x + pad, r.y + pad + font_ascent(px), px,
         g_cfg.info_text, 255);
}

void overlay_info(gfx_surface *fb, const overlay_info_data *d)
{
   int      px    = g_cfg.font_size;
   int      lh    = font_line_height(px);
   int      pad   = px;
   int      w     = fb->width * 7 / 10;
   int      lines = d->status[0] ? 5 : 4;
   int      x, y, inner, label_w;
   char     buf[512], fit[512];
   char     pos[32], dur[32], pct[16];
   double   frac = 0.0;
   gfx_rect r, bar;

   if (w < 300)
      w = fb->width - 20;
   r.w = w;
   r.h = lines * lh + (lines - 1) * (lh / 3) + 2 * pad;
   r.x = (fb->width - r.w) / 2;
   r.y = (fb->height - r.h) / 2;
   overlay_panel(fb, r, 1);

   x       = r.x + pad;
   y       = r.y + pad + font_ascent(px);
   inner   = r.w - 2 * pad;
   label_w = font_text_width("Subtitles: ", px);
   if (font_text_width("Audio: ", px) > label_w)
      label_w = font_text_width("Audio: ", px);

   /* Line 1: subtitles. */
   font_draw(fb, "Subtitles:", x, y, px, g_cfg.info_text, 255);
   font_ellipsize(d->subtitle, px, inner - label_w, fit, sizeof(fit));
   font_draw(fb, fit, x + label_w, y, px, g_cfg.info_text, 255);
   y += lh + lh / 3;

   /* Line 2: audio. */
   font_draw(fb, "Audio:", x, y, px, g_cfg.info_text, 255);
   font_ellipsize(d->audio, px, inner - label_w, fit, sizeof(fit));
   font_draw(fb, fit, x + label_w, y, px, g_cfg.info_text, 255);
   y += lh + lh / 3;

   /* Line 3: how many tracks there are. */
   sprintf(buf, "%d subtitle track%s, %d audio track%s available",
         d->subtitle_count, d->subtitle_count == 1 ? "" : "s",
         d->audio_count, d->audio_count == 1 ? "" : "s");
   font_ellipsize(buf, px, inner, fit, sizeof(fit));
   font_draw(fb, fit, x, y, px, g_cfg.info_text, 200);
   y += lh + lh / 3;

   /* Line 4: progress bar, percent on the left, time on the right. */
   if (d->duration > 0.0)
   {
      frac = d->position / d->duration;
      if (frac < 0.0)
         frac = 0.0;
      if (frac > 1.0)
         frac = 1.0;
      sprintf(pct, "%d%%", (int)(frac * 100.0));
   }
   else
      strcpy(pct, "--%");
   util_format_time(d->position, pos, sizeof(pos));
   util_format_time(d->duration > 0.0 ? d->duration : -1.0, dur, sizeof(dur));
   sprintf(buf, "%s / %s", pos, dur);
   {
      int pct_w  = font_text_width("100%", px) + px / 2;
      int time_w = font_text_width(buf, px);
      int bar_h  = lh / 2;
      font_draw(fb, pct, x, y, px, g_cfg.info_text, 255);
      font_draw(fb, buf, x + inner - time_w, y, px, g_cfg.info_text, 255);
      bar.x = x + pct_w;
      bar.w = inner - pct_w - time_w - px / 2;
      bar.h = bar_h;
      bar.y = y - font_ascent(px) / 2 - bar_h / 2;
      if (bar.w > 10)
      {
         gfx_fill(fb, bar.x, bar.y, bar.w, bar.h, g_cfg.info_text, 60);
         gfx_fill(fb, bar.x, bar.y, (int)(bar.w * frac), bar.h,
               COLOR_IS_SOLID(g_cfg.info_border) ? g_cfg.info_border
               : g_cfg.info_text, 255);
         gfx_frame(fb, bar.x, bar.y, bar.w, bar.h, 1, g_cfg.info_text, 160);
      }
   }
   y += lh + lh / 3;

   /* Line 5: state (paused, A-B loop, aspect). */
   if (d->status[0])
   {
      font_ellipsize(d->status, px, inner, fit, sizeof(fit));
      font_draw(fb, fit, x, y, px, g_cfg.info_text, 200);
   }
}
