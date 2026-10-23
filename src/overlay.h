/*
 * overlay.h - pop-up panels drawn over the browser or the playing media:
 * the title panel, short notifications ("toasts") and the playback
 * information panel.  Colours come from the core options.
 */
#ifndef DLNA_OVERLAY_H
#define DLNA_OVERLAY_H

#include "gfx.h"

/* A bordered, semi-transparent box in the title panel's colours
 * (info = 0) or the information panel's (info = 1). */
void overlay_panel(gfx_surface *fb, gfx_rect r, int info);

/* Thickness of panel borders for the current canvas. */
int overlay_border_px(const gfx_surface *fb);

/* Title panel centred near the bottom of the screen. */
void overlay_title(gfx_surface *fb, const char *text);

/* A short message centred near the top of the screen. */
void overlay_toast(gfx_surface *fb, const char *text);

/* Everything the information panel shows. */
typedef struct overlay_info_data
{
   char   subtitle[160];   /* "2/3  English [eng] (subrip)" or "Off" */
   char   audio[160];
   int    subtitle_count;
   int    audio_count;
   double position;        /* seconds */
   double duration;        /* seconds, <= 0 when unknown */
   char   status[160];     /* paused / loop / aspect line, may be empty */
} overlay_info_data;

void overlay_info(gfx_surface *fb, const overlay_info_data *d);

#endif
