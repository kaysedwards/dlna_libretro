/*
 * viz.c - see viz.h.
 */
#include <string.h>
#include <math.h>

#include "viz.h"

#define FFT_BITS    11
#define FFT_SIZE    (1 << FFT_BITS)   /* 2048 samples = 43 ms at 48 kHz */
#define NUM_BARS    48
#define SAMPLE_RATE 48000.0
#define MIN_FREQ    40.0
#define MAX_FREQ    16000.0
#define FLOOR_DB    (-75.0)           /* quieter than this is no bar   */
#define RANGE_DB    65.0              /* FLOOR_DB .. FLOOR_DB+RANGE_DB  */
#define FALL_RATE   1.6               /* bar heights per second         */
#define PEAK_HOLD   0.4               /* seconds a peak marker waits    */
#define PEAK_FALL   0.6               /* peak marker fall speed         */

/* C90's <math.h> has no M_PI. */
#define VIZ_PI 3.14159265358979323846

static float  g_ring[FFT_SIZE];       /* last FFT_SIZE mono samples   */
static size_t g_ring_pos;
static float  g_window[FFT_SIZE];
static int    g_ready;                /* window and bins prepared      */
static int    g_bin_lo[NUM_BARS], g_bin_hi[NUM_BARS];
static double g_target[NUM_BARS];     /* latest analysis, 0..1         */
static double g_height[NUM_BARS];     /* what is drawn, 0..1           */
static double g_peak[NUM_BARS];
static double g_peak_age[NUM_BARS];

static void prepare(void)
{
   int i;

   /* Hann window: reduces the smearing between neighbouring bins. */
   for (i = 0; i < FFT_SIZE; i++)
      g_window[i] = (float)(0.5 - 0.5 * cos(2.0 * VIZ_PI * i / (FFT_SIZE - 1)));

   /* Bar edges spaced evenly on a log scale, as hearing works.  Each bar
    * covers at least one FFT bin. */
   for (i = 0; i < NUM_BARS; i++)
   {
      double f0 = MIN_FREQ * pow(MAX_FREQ / MIN_FREQ, (double)i / NUM_BARS);
      double f1 = MIN_FREQ * pow(MAX_FREQ / MIN_FREQ, (double)(i + 1) / NUM_BARS);
      int    b0 = (int)(f0 * FFT_SIZE / SAMPLE_RATE);
      int    b1 = (int)(f1 * FFT_SIZE / SAMPLE_RATE);
      if (b0 < 1)
         b0 = 1;
      if (b1 <= b0)
         b1 = b0 + 1;
      g_bin_lo[i] = b0;
      g_bin_hi[i] = b1;
   }
   g_ready = 1;
}

void viz_reset(void)
{
   memset(g_ring, 0, sizeof(g_ring));
   memset(g_target, 0, sizeof(g_target));
   memset(g_height, 0, sizeof(g_height));
   memset(g_peak, 0, sizeof(g_peak));
   memset(g_peak_age, 0, sizeof(g_peak_age));
   g_ring_pos = 0;
}

/* In-place iterative radix-2 FFT. */
static void fft(float *re, float *im)
{
   int i, j, k, len;

   /* Bit-reversal permutation. */
   for (i = 1, j = 0; i < FFT_SIZE; i++)
   {
      int bit = FFT_SIZE >> 1;
      for (; j & bit; bit >>= 1)
         j ^= bit;
      j ^= bit;
      if (i < j)
      {
         float t = re[i];
         re[i]   = re[j];
         re[j]   = t;
         t       = im[i];
         im[i]   = im[j];
         im[j]   = t;
      }
   }
   for (len = 2; len <= FFT_SIZE; len <<= 1)
   {
      double ang = -2.0 * VIZ_PI / len;
      float  wr  = (float)cos(ang), wi = (float)sin(ang);
      for (i = 0; i < FFT_SIZE; i += len)
      {
         float cr = 1.0f, ci = 0.0f;
         for (k = 0; k < len / 2; k++)
         {
            int   a  = i + k, b = i + k + len / 2;
            float tr = re[b] * cr - im[b] * ci;
            float ti = re[b] * ci + im[b] * cr;
            float nr;
            re[b] = re[a] - tr;
            im[b] = im[a] - ti;
            re[a] += tr;
            im[a] += ti;
            nr = cr * wr - ci * wi;
            ci = cr * wi + ci * wr;
            cr = nr;
         }
      }
   }
}

void viz_feed(const int16_t *samples, size_t frames)
{
   static float re[FFT_SIZE], im[FFT_SIZE];
   size_t       i;
   int          b;

   if (!g_ready)
      prepare();
   for (i = 0; i < frames; i++)
   {
      g_ring[g_ring_pos] = ((float)samples[i * 2] + (float)samples[i * 2 + 1])
         / 65536.0f;
      g_ring_pos = (g_ring_pos + 1) % FFT_SIZE;
   }
   for (i = 0; i < FFT_SIZE; i++)
   {
      re[i] = g_ring[(g_ring_pos + i) % FFT_SIZE] * g_window[i];
      im[i] = 0.0f;
   }
   fft(re, im);

   for (b = 0; b < NUM_BARS; b++)
   {
      double sum = 0.0, db, level;
      int    k;
      for (k = g_bin_lo[b]; k < g_bin_hi[b]; k++)
         sum += sqrt((double)re[k] * re[k] + (double)im[k] * im[k]);
      sum /= (g_bin_hi[b] - g_bin_lo[b]);
      /* A full scale sine gives about FFT_SIZE / 4 with a Hann window. */
      db    = 20.0 * log10(sum / (FFT_SIZE / 4.0) + 1e-9);
      level = (db - FLOOR_DB) / RANGE_DB;
      if (level < 0.0)
         level = 0.0;
      if (level > 1.0)
         level = 1.0;
      g_target[b] = level;
   }
}

void viz_draw(gfx_surface *fb, gfx_rect area, uint32_t rgb, double dt)
{
   int b;
   int slot, bar_w, gap, x0;

   if (area.w < NUM_BARS || area.h < 8)
      return;
   slot  = area.w / NUM_BARS;
   gap   = slot / 4;
   bar_w = slot - gap;
   if (bar_w < 1)
      bar_w = 1;
   x0    = area.x + (area.w - slot * NUM_BARS + gap) / 2;

   for (b = 0; b < NUM_BARS; b++)
   {
      int h, x = x0 + b * slot;

      /* Up at once, down smoothly. */
      if (g_target[b] >= g_height[b])
         g_height[b] = g_target[b];
      else
      {
         g_height[b] -= FALL_RATE * dt;
         if (g_height[b] < g_target[b])
            g_height[b] = g_target[b];
      }
      if (g_height[b] >= g_peak[b])
      {
         g_peak[b]     = g_height[b];
         g_peak_age[b] = 0.0;
      }
      else
      {
         g_peak_age[b] += dt;
         if (g_peak_age[b] > PEAK_HOLD)
            g_peak[b] -= PEAK_FALL * dt;
      }

      h = (int)(g_height[b] * area.h * 0.9);
      if (h > 0)
         gfx_fill(fb, x, area.y + area.h - h, bar_w, h, rgb, 200);
      if (g_peak[b] > 0.02)
      {
         int py = area.y + area.h - (int)(g_peak[b] * area.h * 0.9) - 3;
         gfx_fill(fb, x, py, bar_w, 3, rgb, 255);
      }
   }
}
