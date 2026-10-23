/*
 * skin.h - configurable backgrounds.
 *
 * Several parts of the screen (the panels, the thumbnail boxes, the
 * browser and playback backgrounds) take a "background" setting that is
 * either a colour, "transparent" (draw nothing) or "image" (draw a PNG
 * from <system>/dlna).  This module loads those images and draws any such
 * setting into a rectangle.
 */
#ifndef DLNA_SKIN_H
#define DLNA_SKIN_H

#include "gfx.h"

/* Special values a colour setting can hold besides 0x00RRGGBB.  They use
 * the top byte, which real colours never set. */
#define COLOR_TRANSPARENT 0xFF000000u
#define COLOR_IMAGE       0xFE000000u
#define COLOR_IS_TRANSPARENT(c) ((c) == COLOR_TRANSPARENT)
#define COLOR_IS_IMAGE(c)       ((c) == COLOR_IMAGE)
#define COLOR_IS_SOLID(c)       (((c) & 0xFF000000u) == 0)

typedef enum skin_part
{
   SKIN_INFO = 0,    /* information.png: playback information panel  */
   SKIN_TITLE,       /* title.png:       title panel                  */
   SKIN_SELECTION,   /* selection.png:   thumbnail boxes              */
   SKIN_NAVIGATION,  /* navigation.png:  browser background           */
   SKIN_PLAYBACK,    /* playback.png:    behind video, pictures, art  */
   SKIN_BORDERS,     /* borders.png:     nine-slice border image      */
   SKIN_ARROW_BACK,  /* arrows/backward.png: "more above" arrow       */
   SKIN_ARROW_FWD,   /* arrows/forward.png:  "more below" arrow       */
   SKIN_COUNT
} skin_part;

void skin_init(const char *system_dir);
void skin_deinit(void);

/* Is the image for this part available? */
int skin_has_image(skin_part part);

/* Black or white, whichever stands out more against 'rgb'. */
uint32_t skin_contrast(uint32_t rgb);

/* Fill r according to 'setting':
 *   a colour     - filled with it at 'alpha' (0-255)
 *   transparent  - nothing is drawn
 *   image        - the part's image, stretched to r (or, with 'cover',
 *                  scaled keeping its shape until it covers r and cropped);
 *                  if there is no image, 'fallback' is used as a colour. */
void skin_fill(gfx_surface *fb, gfx_rect r, skin_part part, uint32_t setting,
      uint32_t fallback, unsigned alpha, int cover);

/* Draw a frame inside r.  A colour setting draws a solid frame of the
 * given thickness, transparent draws nothing, and "image" draws a
 * nine-slice frame from borders.png (see skin_frame_thickness) or, if the
 * file is missing, a solid frame in 'fallback'. */
void skin_frame(gfx_surface *fb, gfx_rect r, int thickness, uint32_t setting,
      uint32_t fallback, unsigned alpha);

/* How thick a frame drawn with this setting will be: 'thickness' for
 * colours, the scaled corner size for an image border. */
int skin_frame_thickness(const gfx_surface *fb, uint32_t setting,
      int thickness);

/* A wide, flat arrowhead centred in r pointing up (down = 0) or down: a
 * shape in the setting's colour, or arrows/backward.png (up) /
 * arrows/forward.png (down) scaled to fit r and centred; 'fallback' is the
 * colour used when the image is missing. */
void skin_arrow(gfx_surface *fb, gfx_rect r, int down, uint32_t setting,
      uint32_t fallback, unsigned alpha);

#endif
