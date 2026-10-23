/*
 * ui.h - the media browser: a grid of square thumbnails with the title of
 * the selected entry in a panel near the bottom of the screen.
 *
 * Navigation is a stack of "levels" (server list, Audio/Picture/Video
 * chooser, folders).  The bottom level is the "top folder": Back never
 * goes above it.
 */
#ifndef DLNA_UI_H
#define DLNA_UI_H

#include "gfx.h"
#include "dlna.h"

int  ui_init(const char *system_dir);
void ui_deinit(void);

/* Recalculate the grid for a canvas size and font size, reload icons. */
void ui_layout(int width, int height, int font_px);

/* Start in the server list (no content given) or at a dlna:// URI. */
void ui_start_servers(void);
void ui_start_uri(const char *uri);

/* Process network results and input.  Returns an entry the user chose to
 * play (the caller owns it) or NULL. */
dlna_entry *ui_update(double dt);

/* Draw the browser. */
void ui_render(gfx_surface *fb);

/* Something changed that needs a redraw (results, thumbnails, ...). */
int ui_needs_redraw(void);

/* Sort options changed: re-sort every level. */
void ui_resort(void);

/* dlna:// path of the folder being shown (NULL in the server list), and
 * of a media entry in it.  Both return malloc'ed strings. */
char *ui_folder_uri(void);
char *ui_media_uri(const dlna_entry *e);

/* How the last ui_start_uri() turned out (reported once, then reset):
 * the URI named a file (which ui_update() then hands over to play), a
 * folder (now shown), or could not be resolved. */
typedef enum ui_resolve_outcome
{
   UI_RESOLVE_PENDING = 0,
   UI_RESOLVE_MEDIA,
   UI_RESOLVE_FOLDER,
   UI_RESOLVE_FAILED
} ui_resolve_outcome;
ui_resolve_outcome ui_take_resolve_outcome(void);

/* Show a message (e.g. why playback failed) for a few seconds. */
void ui_set_message(const char *text, double seconds);

/* The fallback icon for a media type (NULL if no image was found), and a
 * glyph-or-icon drawing helper used by the player for audio files. */
void ui_draw_icon(gfx_surface *fb, media_type type, int folder, gfx_rect r);

#endif
