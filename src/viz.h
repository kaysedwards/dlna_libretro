/*
 * viz.h - the audio visualization: spectrum bars drawn over the cover art
 * (or the audio icon) while music plays.
 *
 * Each frame the audio sent to the frontend is fed in; the last 2048
 * samples are analysed with an FFT and grouped into bars on a logarithmic
 * frequency scale.  Bars rise immediately and fall back smoothly, with a
 * small peak marker that lingers.  Main thread only.
 */
#ifndef DLNA_VIZ_H
#define DLNA_VIZ_H

#include <stddef.h>
#include "gfx.h"

/* Forget everything (new track). */
void viz_reset(void);

/* Feed the stereo audio just output (interleaved, 48 kHz). */
void viz_feed(const int16_t *samples, size_t frames);

/* Draw the bars inside 'area' in 'rgb'.  'dt' is the time since the last
 * call in seconds (for the falling animation). */
void viz_draw(gfx_surface *fb, gfx_rect area, uint32_t rgb, double dt);

#endif
