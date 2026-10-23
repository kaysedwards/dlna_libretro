/*
 * thumbs.h - asynchronous thumbnail cache.
 *
 * The browser asks for the thumbnail of every visible entry each frame.
 * Images that are not loaded yet are queued and fetched by a few background
 * threads, most recently requested first, so scrolling quickly through a
 * big folder only loads what is actually on screen.  Loaded images are
 * pre-scaled to the current thumbnail size and evicted (least recently
 * shown first) once the cache grows beyond a memory budget.
 */
#ifndef DLNA_THUMBS_H
#define DLNA_THUMBS_H

#include "gfx.h"

int  thumbs_init(void);
void thumbs_deinit(void);

/* Set the size (in pixels) thumbnails are drawn at.  Changing it throws
 * away every loaded image. */
void thumbs_set_size(int px);

/* Call once per rendered browser frame before any thumbs_get(). */
void thumbs_begin_frame(void);

/* Returns the image if it is ready, otherwise NULL (and queues it).
 * 'prefetch' requests (entries just off screen) load after visible ones.
 * *failed is set when the image could not be loaded at all. */
const gfx_image *thumbs_get(const char *url, int prefetch, int *failed);

/* Returns 1 (once) if any thumbnail finished loading since the last call,
 * meaning the screen should be redrawn. */
int thumbs_take_dirty(void);

#endif
