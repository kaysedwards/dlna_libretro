/*
 * image.h - decode still images (JPEG, PNG, ...) with FFmpeg.
 *
 * Used for thumbnails, the fallback icons in <system>/dlna and audio cover
 * art.  Works with local paths and http:// URLs alike.  EXIF orientation
 * (exported by FFmpeg as a display matrix) is honoured.
 */
#ifndef DLNA_IMAGE_H
#define DLNA_IMAGE_H

#include "gfx.h"

/* Decode the first frame of 'url' and scale it to fit inside
 * max_w x max_h, keeping the aspect ratio.  When allow_upscale is 0 small
 * images keep their size.  'abort_flag' (optional) is polled during network
 * I/O; set it non-zero from another thread to give up early.
 * Returns NULL on failure. */
gfx_image *image_load(const char *url, int max_w, int max_h,
      int allow_upscale, volatile int *abort_flag);

/* FFmpeg forward declarations so callers need not include FFmpeg. */
struct AVFrame;
struct AVCodecParameters;
struct AVPacket;

/* Convert a decoded frame to an RGB32 gfx_image, scaled to fit inside
 * max_w x max_h (0 = native size) and rotated according to its display
 * matrix. */
gfx_image *image_from_frame(const struct AVFrame *frame, int max_w, int max_h,
      int allow_upscale);

/* Decode a single packet (e.g. an MP3's embedded cover art) into an image. */
gfx_image *image_from_packet(const struct AVCodecParameters *par,
      const struct AVPacket *pkt, int max_w, int max_h);

/* Rotation in degrees (0, 90, 180, 270) of a frame's display matrix. */
int image_frame_rotation(const struct AVFrame *frame);

#endif
