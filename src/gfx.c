/*
 * gfx.c - see gfx.h.
 */
#include <stdlib.h>
#include <string.h>

#include "gfx.h"

/* Blend one channel: (src * a + dst * (255 - a)) / 255, using the usual
 * "divide by 255" approximation (exact for all inputs in range). */
static unsigned blend_channel(unsigned src, unsigned dst, unsigned a)
{
   unsigned t = src * a + dst * (255 - a) + 128;
   return (t + (t >> 8)) >> 8;
}

static uint32_t blend_pixel(uint32_t dst, uint32_t rgb, unsigned a)
{
   unsigned r, g, b;

   if (a >= 255)
      return rgb & 0x00FFFFFFu;
   if (a == 0)
      return dst;
   r = blend_channel((rgb >> 16) & 0xFF, (dst >> 16) & 0xFF, a);
   g = blend_channel((rgb >>  8) & 0xFF, (dst >>  8) & 0xFF, a);
   b = blend_channel( rgb        & 0xFF,  dst        & 0xFF, a);
   return (uint32_t)((r << 16) | (g << 8) | b);
}

/* a * b / 255 for 8 bit values. */
static unsigned mul255(unsigned a, unsigned b)
{
   unsigned t = a * b + 128;
   return (t + (t >> 8)) >> 8;
}

gfx_image *gfx_image_new(int width, int height)
{
   gfx_image *img;

   if (width <= 0 || height <= 0)
      return NULL;
   img = (gfx_image*)calloc(1, sizeof(*img));
   if (!img)
      return NULL;
   img->pixels = (uint32_t*)calloc((size_t)width * (size_t)height, sizeof(uint32_t));
   if (!img->pixels)
   {
      free(img);
      return NULL;
   }
   img->width  = width;
   img->height = height;
   return img;
}

void gfx_image_free(gfx_image *img)
{
   if (!img)
      return;
   free(img->pixels);
   free(img);
}

int gfx_clip(const gfx_surface *s, gfx_rect *r)
{
   if (r->x < 0)
   {
      r->w += r->x;
      r->x  = 0;
   }
   if (r->y < 0)
   {
      r->h += r->y;
      r->y  = 0;
   }
   if (r->x + r->w > s->width)
      r->w = s->width - r->x;
   if (r->y + r->h > s->height)
      r->h = s->height - r->y;
   return r->w > 0 && r->h > 0;
}

void gfx_fill(gfx_surface *s, int x, int y, int w, int h,
      uint32_t rgb, unsigned alpha)
{
   gfx_rect r;
   int      i, j;

   r.x = x;
   r.y = y;
   r.w = w;
   r.h = h;
   if (!alpha || !gfx_clip(s, &r))
      return;
   rgb &= 0x00FFFFFFu;
   for (j = 0; j < r.h; j++)
   {
      uint32_t *row = s->pixels + (size_t)(r.y + j) * s->pitch + r.x;
      if (alpha >= 255)
         for (i = 0; i < r.w; i++)
            row[i] = rgb;
      else
         for (i = 0; i < r.w; i++)
            row[i] = blend_pixel(row[i], rgb, alpha);
   }
}

void gfx_frame(gfx_surface *s, int x, int y, int w, int h, int thickness,
      uint32_t rgb, unsigned alpha)
{
   if (thickness <= 0 || w <= 0 || h <= 0)
      return;
   if (thickness * 2 >= w || thickness * 2 >= h)
   {
      gfx_fill(s, x, y, w, h, rgb, alpha);
      return;
   }
   gfx_fill(s, x, y, w, thickness, rgb, alpha);
   gfx_fill(s, x, y + h - thickness, w, thickness, rgb, alpha);
   gfx_fill(s, x, y + thickness, thickness, h - 2 * thickness, rgb, alpha);
   gfx_fill(s, x + w - thickness, y + thickness, thickness,
         h - 2 * thickness, rgb, alpha);
}

void gfx_blit(gfx_surface *s, const gfx_image *img, int x, int y,
      unsigned opacity)
{
   gfx_rect r;
   int      i, j;

   if (!img)
      return;
   r.x = x;
   r.y = y;
   r.w = img->width;
   r.h = img->height;
   if (!opacity || !gfx_clip(s, &r))
      return;
   for (j = 0; j < r.h; j++)
   {
      uint32_t       *dst = s->pixels + (size_t)(r.y + j) * s->pitch + r.x;
      const uint32_t *src = img->pixels
         + (size_t)(r.y - y + j) * img->width + (r.x - x);
      for (i = 0; i < r.w; i++)
      {
         uint32_t p = src[i];
         unsigned a = p >> 24;
         if (opacity < 255)
            a = mul255(a, opacity);
         dst[i] = blend_pixel(dst[i], p, a);
      }
   }
}

void gfx_blit_scaled(gfx_surface *s, const gfx_image *img,
      int dx, int dy, int dw, int dh, unsigned opacity)
{
   gfx_rect r;
   int      i, j;
   long     step_x, step_y;

   if (!img || dw <= 0 || dh <= 0)
      return;
   if (dw == img->width && dh == img->height)
   {
      gfx_blit(s, img, dx, dy, opacity);
      return;
   }
   r.x = dx;
   r.y = dy;
   r.w = dw;
   r.h = dh;
   if (!opacity || !gfx_clip(s, &r))
      return;

   /* 16.16 fixed point source coordinates of each destination pixel
    * centre.  'long' is at least 32 bits in C90, enough for images up to
    * 32767 pixels wide. */
   step_x = ((long)img->width << 16) / dw;
   step_y = ((long)img->height << 16) / dh;

   for (j = 0; j < r.h; j++)
   {
      uint32_t *dst = s->pixels + (size_t)(r.y + j) * s->pitch + r.x;
      long      sy  = (r.y - dy + j) * step_y + step_y / 2 - 32768L;
      int       y0, y1;
      unsigned  fy;

      if (sy < 0)
         sy = 0;
      y0 = (int)(sy >> 16);
      fy = (unsigned)((sy >> 8) & 0xFF);
      y1 = y0 + 1 < img->height ? y0 + 1 : y0;

      for (i = 0; i < r.w; i++)
      {
         long            sx = (r.x - dx + i) * step_x + step_x / 2 - 32768L;
         int             x0, x1;
         unsigned        fx, c, a;
         unsigned        out[4];
         const uint32_t *row0, *row1;
         uint32_t        p00, p01, p10, p11;

         if (sx < 0)
            sx = 0;
         x0 = (int)(sx >> 16);
         fx = (unsigned)((sx >> 8) & 0xFF);
         x1 = x0 + 1 < img->width ? x0 + 1 : x0;

         row0 = img->pixels + (size_t)y0 * img->width;
         row1 = img->pixels + (size_t)y1 * img->width;
         p00  = row0[x0];
         p01  = row0[x1];
         p10  = row1[x0];
         p11  = row1[x1];

         /* Interpolate each of the four channels (A, R, G, B). */
         for (c = 0; c < 4; c++)
         {
            unsigned shift = 24 - c * 8;
            unsigned top   = ((p00 >> shift) & 0xFF) * (256 - fx)
                           + ((p01 >> shift) & 0xFF) * fx;
            unsigned bot   = ((p10 >> shift) & 0xFF) * (256 - fx)
                           + ((p11 >> shift) & 0xFF) * fx;
            out[c] = (top * (256 - fy) + bot * fy) >> 16;
         }
         a = out[0];
         if (opacity < 255)
            a = mul255(a, opacity);
         dst[i] = blend_pixel(dst[i],
               (uint32_t)((out[1] << 16) | (out[2] << 8) | out[3]), a);
      }
   }
}

void gfx_blend_mask(gfx_surface *s, const unsigned char *mask, int mw, int mh,
      int mpitch, int x, int y, uint32_t rgb, unsigned alpha)
{
   gfx_rect r;
   int      i, j;

   if (!mask)
      return;
   r.x = x;
   r.y = y;
   r.w = mw;
   r.h = mh;
   if (!alpha || !gfx_clip(s, &r))
      return;
   for (j = 0; j < r.h; j++)
   {
      uint32_t            *dst = s->pixels + (size_t)(r.y + j) * s->pitch + r.x;
      const unsigned char *src = mask + (size_t)(r.y - y + j) * mpitch + (r.x - x);
      for (i = 0; i < r.w; i++)
      {
         unsigned a = src[i];
         if (!a)
            continue;
         if (alpha < 255)
            a = mul255(a, alpha);
         dst[i] = blend_pixel(dst[i], rgb, a);
      }
   }
}

/* Bilinear resample of all four channels (used when enlarging). */
static void scale_bilinear(const gfx_image *src, gfx_image *out)
{
   int  i, j;
   long step_x = ((long)src->width << 16) / out->width;
   long step_y = ((long)src->height << 16) / out->height;

   for (j = 0; j < out->height; j++)
   {
      long     sy = j * step_y + step_y / 2 - 32768L;
      int      y0, y1;
      unsigned fy;

      if (sy < 0)
         sy = 0;
      y0 = (int)(sy >> 16);
      if (y0 >= src->height)
         y0 = src->height - 1;
      fy = (unsigned)((sy >> 8) & 0xFF);
      y1 = y0 + 1 < src->height ? y0 + 1 : y0;

      for (i = 0; i < out->width; i++)
      {
         long            sx = i * step_x + step_x / 2 - 32768L;
         int             x0, x1;
         unsigned        fx, c;
         uint32_t        p[4], result = 0;
         const uint32_t *row0, *row1;

         if (sx < 0)
            sx = 0;
         x0 = (int)(sx >> 16);
         if (x0 >= src->width)
            x0 = src->width - 1;
         fx = (unsigned)((sx >> 8) & 0xFF);
         x1 = x0 + 1 < src->width ? x0 + 1 : x0;

         row0 = src->pixels + (size_t)y0 * src->width;
         row1 = src->pixels + (size_t)y1 * src->width;
         p[0] = row0[x0];
         p[1] = row0[x1];
         p[2] = row1[x0];
         p[3] = row1[x1];
         for (c = 0; c < 4; c++)
         {
            unsigned shift = c * 8;
            unsigned top   = ((p[0] >> shift) & 0xFF) * (256 - fx)
                           + ((p[1] >> shift) & 0xFF) * fx;
            unsigned bot   = ((p[2] >> shift) & 0xFF) * (256 - fx)
                           + ((p[3] >> shift) & 0xFF) * fx;
            result |= (uint32_t)(((top * (256 - fy) + bot * fy) >> 16) & 0xFF)
               << shift;
         }
         out->pixels[(size_t)j * out->width + i] = result;
      }
   }
}

/* Box filter (used when shrinking).  Colour is weighted by alpha so that
 * transparent pixels do not bleed dark fringes into the result. */
static void scale_box(const gfx_image *src, gfx_image *out)
{
   int i, j;

   for (j = 0; j < out->height; j++)
   {
      int sy0 = (int)((long)j * src->height / out->height);
      int sy1 = (int)((long)(j + 1) * src->height / out->height);
      if (sy1 <= sy0)
         sy1 = sy0 + 1;
      for (i = 0; i < out->width; i++)
      {
         int           sx0 = (int)((long)i * src->width / out->width);
         int           sx1 = (int)((long)(i + 1) * src->width / out->width);
         unsigned long acc[4];
         unsigned long n = 0;
         int           x, y;

         if (sx1 <= sx0)
            sx1 = sx0 + 1;
         acc[0] = acc[1] = acc[2] = acc[3] = 0;
         for (y = sy0; y < sy1 && y < src->height; y++)
         {
            const uint32_t *row = src->pixels + (size_t)y * src->width;
            for (x = sx0; x < sx1 && x < src->width; x++)
            {
               uint32_t p = row[x];
               unsigned a = p >> 24;
               acc[0] += a;
               acc[1] += ((p >> 16) & 0xFF) * a;
               acc[2] += ((p >>  8) & 0xFF) * a;
               acc[3] += ( p        & 0xFF) * a;
               n++;
            }
         }
         if (!n || !acc[0])
            out->pixels[(size_t)j * out->width + i] = 0;
         else
            out->pixels[(size_t)j * out->width + i] = (uint32_t)
               (((acc[0] / n) << 24)
                | ((acc[1] / acc[0]) << 16)
                | ((acc[2] / acc[0]) << 8)
                |  (acc[3] / acc[0]));
      }
   }
}

gfx_image *gfx_image_fit(const gfx_image *src, int max_w, int max_h)
{
   gfx_image *out;
   int        w, h;

   if (!src || max_w <= 0 || max_h <= 0)
      return NULL;
   /* Pick the limiting dimension without floating point. */
   if ((long)src->width * max_h > (long)src->height * max_w)
   {
      w = max_w;
      h = (int)((long)src->height * max_w / src->width);
   }
   else
   {
      h = max_h;
      w = (int)((long)src->width * max_h / src->height);
   }
   if (w < 1)
      w = 1;
   if (h < 1)
      h = 1;
   out = gfx_image_new(w, h);
   if (!out)
      return NULL;
   if (w > src->width || h > src->height)
      scale_bilinear(src, out);
   else
      scale_box(src, out);
   return out;
}

void gfx_fill_triangle(gfx_surface *s, double cx, double apex_y,
      double base_y, double half_base, uint32_t rgb, unsigned alpha)
{
   double height = base_y - apex_y;
   int    y0, y1, y;

   if (height == 0.0 || half_base <= 0.0 || !alpha)
      return;
   y0 = (int)(apex_y < base_y ? apex_y : base_y);
   y1 = (int)(apex_y < base_y ? base_y : apex_y);
   if (y0 < 0)
      y0 = 0;
   if (y1 > s->height)
      y1 = s->height;
   for (y = y0; y < y1; y++)
   {
      /* Half width of the triangle at the middle of this pixel row. */
      double t  = ((double)y + 0.5 - apex_y) / height;
      double hw, left, right;
      int    xl, xr;

      if (t < 0.0 || t > 1.0)
         continue;
      hw    = t * half_base;
      left  = cx - hw;
      right = cx + hw;
      xl    = (int)left;
      xr    = (int)right;
      /* The two end pixels are partly covered (anti-aliasing); everything
       * between them is drawn as one solid span. */
      if (xl == xr)
         gfx_fill(s, xl, y, 1, 1, rgb, (unsigned)((right - left) * alpha + 0.5));
      else
      {
         gfx_fill(s, xl, y, 1, 1, rgb, (unsigned)((xl + 1 - left) * alpha + 0.5));
         if (xr - xl > 1)
            gfx_fill(s, xl + 1, y, xr - xl - 1, 1, rgb, alpha);
         gfx_fill(s, xr, y, 1, 1, rgb, (unsigned)((right - xr) * alpha + 0.5));
      }
   }
}

gfx_image *gfx_image_crop(const gfx_image *src, int x, int y, int w, int h)
{
   gfx_image *out;
   int        j;

   if (!src || x < 0 || y < 0 || w <= 0 || h <= 0
         || x + w > src->width || y + h > src->height)
      return NULL;
   out = gfx_image_new(w, h);
   if (!out)
      return NULL;
   for (j = 0; j < h; j++)
      memcpy(out->pixels + (size_t)j * w,
            src->pixels + (size_t)(y + j) * src->width + x,
            (size_t)w * sizeof(uint32_t));
   return out;
}

gfx_image *gfx_image_resize(const gfx_image *src, int w, int h)
{
   gfx_image *out;

   if (!src || w <= 0 || h <= 0)
      return NULL;
   out = gfx_image_new(w, h);
   if (!out)
      return NULL;
   if (w > src->width || h > src->height)
      scale_bilinear(src, out);
   else
      scale_box(src, out);
   return out;
}

gfx_rect gfx_fit_aspect(gfx_rect outer, double aspect)
{
   gfx_rect r;

   if (aspect <= 0.0 || outer.w <= 0 || outer.h <= 0)
      return outer;
   if ((double)outer.w / (double)outer.h > aspect)
   {
      r.h = outer.h;
      r.w = (int)(outer.h * aspect + 0.5);
   }
   else
   {
      r.w = outer.w;
      r.h = (int)(outer.w / aspect + 0.5);
   }
   if (r.w < 1)
      r.w = 1;
   if (r.h < 1)
      r.h = 1;
   r.x = outer.x + (outer.w - r.w) / 2;
   r.y = outer.y + (outer.h - r.h) / 2;
   return r;
}
