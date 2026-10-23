/*
 * font.h - text rendering.
 *
 * Fonts are rendered with FreeType.  The primary font is chosen in this
 * order (first one that loads wins):
 *
 *   1. <system>/dlna/font.ttf
 *   2. <system>/dlna/font.otf
 *   3. The fonts RetroArch itself ships in its assets directory
 *      (assets/pkg/osd-font.ttf, assets/xmb/monochrome/font.ttf, ...)
 *   4. Common operating system fonts
 *
 * Further fonts are loaded as *fallbacks*: any character missing from the
 * primary font (CJK titles, the Unicode icon glyphs, ...) is looked up in
 * them in order.  If FreeType cannot load anything at all, a tiny built-in
 * 5x7 bitmap font covering printable ASCII is used so the UI never ends up
 * blank.
 *
 * All functions must be called from the main (retro_run) thread.
 */
#ifndef DLNA_FONT_H
#define DLNA_FONT_H

#include "gfx.h"

int  font_init(const char *system_dir);
void font_deinit(void);

/* Name of the primary font file (for the log / info screen). */
const char *font_primary_name(void);

/* Is there a real glyph for this code point in any loaded font?  The
 * built-in bitmap font does not count. */
int font_has_glyph(long codepoint);

/* Vertical metrics for a pixel size.  line height = ascent + descent +
 * line gap; ascent/descent are positive numbers. */
int font_ascent(int px);
int font_descent(int px);
int font_line_height(int px);

/* Width in pixels of a UTF-8 string. */
int font_text_width(const char *utf8, int px);

/* Draw a UTF-8 string with its baseline at y. */
void font_draw(gfx_surface *s, const char *utf8, int x, int baseline, int px,
      uint32_t rgb, unsigned alpha);

/* Same, with a solid outline 'outline' pixels thick drawn first. */
void font_draw_outlined(gfx_surface *s, const char *utf8, int x, int baseline,
      int px, uint32_t rgb, uint32_t outline_rgb, int outline);

/* Copy 'utf8' into 'out', shortening it with a trailing ellipsis so that it
 * fits in max_w pixels.  Returns the final width. */
int font_ellipsize(const char *utf8, int px, int max_w, char *out, size_t size);

/* Drop cached glyph bitmaps (call when sizes change a lot). */
void font_flush_cache(void);

#endif
