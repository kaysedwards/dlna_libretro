/*
 * gfx.h - a very small software renderer.
 *
 * All drawing happens on the CPU into a 32 bit framebuffer that is handed to
 * the frontend as RETRO_PIXEL_FORMAT_XRGB8888.  Pixels are native-endian
 * uint32_t values laid out as 0xAARRGGBB, which is exactly what FFmpeg calls
 * AV_PIX_FMT_RGB32, so decoded images can be copied without swizzling.
 *
 * Images (gfx_image) carry straight (non-premultiplied) alpha in the top
 * byte.  The framebuffer ignores its top byte.
 */
#ifndef DLNA_GFX_H
#define DLNA_GFX_H

#include <stddef.h>
#include "util.h"

/* uint32_t comes from libretro.h's <stdint.h>.  C90 has no fixed width
 * types; see docs/C90_NOTES.md. */
#include <libretro.h>

typedef struct gfx_surface
{
   uint32_t *pixels;
   int       width;
   int       height;
   int       pitch;   /* in pixels, not bytes */
} gfx_surface;

typedef struct gfx_image
{
   uint32_t *pixels;  /* width * height, tightly packed */
   int       width;
   int       height;
} gfx_image;

typedef struct gfx_rect
{
   int x, y, w, h;
} gfx_rect;

#define GFX_RGB(r, g, b) ((uint32_t)(((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b)))

gfx_image *gfx_image_new(int width, int height);
void       gfx_image_free(gfx_image *img);

/* Clip a rectangle to the surface; returns 0 when nothing is left. */
int gfx_clip(const gfx_surface *s, gfx_rect *r);

/* Fill a rectangle with a colour at the given opacity (0-255). */
void gfx_fill(gfx_surface *s, int x, int y, int w, int h,
      uint32_t rgb, unsigned alpha);

/* Draw a rectangular frame of the given thickness inside (x,y,w,h). */
void gfx_frame(gfx_surface *s, int x, int y, int w, int h, int thickness,
      uint32_t rgb, unsigned alpha);

/* Blend an image at (x,y) honouring its alpha channel, multiplied by
 * the extra opacity (255 = as is). */
void gfx_blit(gfx_surface *s, const gfx_image *img, int x, int y,
      unsigned opacity);

/* Blend an image scaled (bilinear) into the destination rectangle. */
void gfx_blit_scaled(gfx_surface *s, const gfx_image *img,
      int dx, int dy, int dw, int dh, unsigned opacity);

/* Blend an 8 bit coverage mask (e.g. a font glyph) in a solid colour. */
void gfx_blend_mask(gfx_surface *s, const unsigned char *mask, int mw, int mh,
      int mpitch, int x, int y, uint32_t rgb, unsigned alpha);

/* Scale an image to fit inside max_w x max_h keeping its aspect ratio.
 * Returns a new image (the original is untouched). */
gfx_image *gfx_image_fit(const gfx_image *src, int max_w, int max_h);

/* Fill an isosceles triangle with a horizontal base, edges anti-aliased:
 * the apex is at (cx, apex_y), the base runs along base_y from
 * cx - half_base to cx + half_base.  base_y may be above or below the apex
 * (an arrow pointing down or up). */
void gfx_fill_triangle(gfx_surface *s, double cx, double apex_y,
      double base_y, double half_base, uint32_t rgb, unsigned alpha);

/* Copy a w x h piece of an image starting at (x, y). */
gfx_image *gfx_image_crop(const gfx_image *src, int x, int y, int w, int h);

/* Scale an image to exactly w x h (ignoring its aspect ratio). */
gfx_image *gfx_image_resize(const gfx_image *src, int w, int h);

/* Compute the largest rectangle with the given display aspect ratio that
 * fits centred inside 'outer'. */
gfx_rect gfx_fit_aspect(gfx_rect outer, double aspect);

#endif
