/*
 * image.c - see image.h.
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/display.h>
#include <libavutil/pixdesc.h>
#include <libavutil/time.h>

#include "image.h"
#include "util.h"

/* How long a single image download may take before we give up. */
#define IMAGE_TIMEOUT_US 15000000

typedef struct interrupt_ctx
{
   volatile int *abort_flag;
   int64_t       deadline;
} interrupt_ctx;

static int interrupt_cb(void *opaque)
{
   interrupt_ctx *ic = (interrupt_ctx*)opaque;
   if (ic->abort_flag && *ic->abort_flag)
      return 1;
   return av_gettime_relative() > ic->deadline;
}

int image_frame_rotation(const AVFrame *frame)
{
   AVFrameSideData *sd;
   double           theta;
   int              deg;

   /* The cast keeps us compatible with FFmpeg versions whose
    * av_frame_get_side_data() takes a non-const frame. */
   sd = av_frame_get_side_data((AVFrame*)frame, AV_FRAME_DATA_DISPLAYMATRIX);
   if (!sd || sd->size < 9 * 4)
      return 0;
   /* Same convention as the ffmpeg command line tool: the matrix stores a
    * counter-clockwise rotation, we want the clockwise correction. */
   theta = -av_display_rotation_get((const int32_t*)sd->data);
   if (theta != theta) /* NaN */
      return 0;
   theta -= 360.0 * floor(theta / 360.0 + 0.9 / 360.0);
   deg    = ((int)(theta + 45.0) / 90) * 90;
   return deg % 360;
}

/* Rotate an image clockwise by 90, 180 or 270 degrees. */
static gfx_image *rotate_image(gfx_image *src, int deg)
{
   gfx_image *dst;
   int        x, y;
   int        w = src->width, h = src->height;

   if (deg == 180)
   {
      size_t n = (size_t)w * h, i;
      for (i = 0; i < n / 2; i++)
      {
         uint32_t t              = src->pixels[i];
         src->pixels[i]          = src->pixels[n - 1 - i];
         src->pixels[n - 1 - i]  = t;
      }
      return src;
   }
   if (deg != 90 && deg != 270)
      return src;
   dst = gfx_image_new(h, w);
   if (!dst)
      return src;
   for (y = 0; y < h; y++)
      for (x = 0; x < w; x++)
      {
         uint32_t p = src->pixels[(size_t)y * w + x];
         if (deg == 90)
            dst->pixels[(size_t)x * h + (h - 1 - y)] = p;
         else
            dst->pixels[(size_t)(w - 1 - x) * h + y] = p;
      }
   gfx_image_free(src);
   return dst;
}

gfx_image *image_from_frame(const AVFrame *frame, int max_w, int max_h,
      int allow_upscale)
{
   struct SwsContext        *sws;
   const AVPixFmtDescriptor *desc;
   gfx_image                *img;
   int                       rot;
   double                    disp_w, disp_h, scale;
   int                       tw, th, sw, sh;
   uint8_t                  *dst_data[4];
   int                       dst_linesize[4];

   if (!frame || frame->width <= 0 || frame->height <= 0)
      return NULL;
   rot = image_frame_rotation(frame);

   /* Displayed size, honouring non-square pixels. */
   disp_w = frame->width;
   disp_h = frame->height;
   if (frame->sample_aspect_ratio.num > 0 && frame->sample_aspect_ratio.den > 0)
      disp_w = disp_w * frame->sample_aspect_ratio.num
         / frame->sample_aspect_ratio.den;
   if (rot == 90 || rot == 270)
   {
      double t = disp_w;
      disp_w   = disp_h;
      disp_h   = t;
   }

   scale = 1.0;
   if (max_w > 0 && max_h > 0)
   {
      double sx = max_w / disp_w;
      double sy = max_h / disp_h;
      scale = sx < sy ? sx : sy;
      if (!allow_upscale && scale > 1.0)
         scale = 1.0;
   }
   tw = (int)(disp_w * scale + 0.5);
   th = (int)(disp_h * scale + 0.5);
   if (tw < 1)
      tw = 1;
   if (th < 1)
      th = 1;

   /* Size to scale to before rotating. */
   if (rot == 90 || rot == 270)
   {
      sw = th;
      sh = tw;
   }
   else
   {
      sw = tw;
      sh = th;
   }

   img = gfx_image_new(sw, sh);
   if (!img)
      return NULL;

   sws = sws_getContext(frame->width, frame->height,
         (enum AVPixelFormat)frame->format, sw, sh, AV_PIX_FMT_RGB32,
         (sw < frame->width ? SWS_AREA : SWS_BICUBIC) | SWS_ACCURATE_RND,
         NULL, NULL, NULL);
   if (!sws)
   {
      gfx_image_free(img);
      return NULL;
   }
   dst_data[0]     = (uint8_t*)img->pixels;
   dst_data[1]     = dst_data[2] = dst_data[3] = NULL;
   dst_linesize[0] = sw * 4;
   dst_linesize[1] = dst_linesize[2] = dst_linesize[3] = 0;
   sws_scale(sws, (const uint8_t * const*)frame->data, frame->linesize,
         0, frame->height, dst_data, dst_linesize);
   sws_freeContext(sws);

   /* Formats without alpha must come out fully opaque. */
   desc = av_pix_fmt_desc_get((enum AVPixelFormat)frame->format);
   if (desc && !(desc->flags & AV_PIX_FMT_FLAG_ALPHA)
         && !(desc->flags & AV_PIX_FMT_FLAG_PAL))
   {
      size_t i, n = (size_t)sw * sh;
      for (i = 0; i < n; i++)
         img->pixels[i] |= 0xFF000000u;
   }

   if (rot)
      img = rotate_image(img, rot);
   return img;
}

/* Feed packets until the decoder returns a frame. */
static AVFrame *decode_first_frame(AVFormatContext *fmt, AVCodecContext *ctx,
      int stream)
{
   AVPacket *pkt   = av_packet_alloc();
   AVFrame  *frame = av_frame_alloc();
   int       sent_eof = 0;

   if (!pkt || !frame)
      goto fail;

   for (;;)
   {
      int ret = avcodec_receive_frame(ctx, frame);
      if (ret == 0)
      {
         av_packet_free(&pkt);
         return frame;
      }
      if (ret != AVERROR(EAGAIN) || sent_eof)
         goto fail;

      ret = av_read_frame(fmt, pkt);
      if (ret < 0)
      {
         /* End of input: flush the decoder to get buffered frames. */
         avcodec_send_packet(ctx, NULL);
         sent_eof = 1;
         continue;
      }
      if (pkt->stream_index == stream)
         avcodec_send_packet(ctx, pkt);
      av_packet_unref(pkt);
   }

fail:
   av_packet_free(&pkt);
   av_frame_free(&frame);
   return NULL;
}

gfx_image *image_load(const char *url, int max_w, int max_h,
      int allow_upscale, volatile int *abort_flag)
{
   AVFormatContext *fmt  = NULL;
   AVCodecContext  *ctx  = NULL;
   AVDictionary    *opts = NULL;
   const AVCodec   *codec;
   AVFrame         *frame = NULL;
   gfx_image       *img   = NULL;
   interrupt_ctx    ic;
   int              stream;

   if (!url || !*url)
      return NULL;

   fmt = avformat_alloc_context();
   if (!fmt)
      return NULL;
   ic.abort_flag                   = abort_flag;
   ic.deadline                     = av_gettime_relative() + IMAGE_TIMEOUT_US;
   fmt->interrupt_callback.callback = interrupt_cb;
   fmt->interrupt_callback.opaque   = &ic;

   /* rw_timeout: give up on a stalled connection (microseconds).
    * seekable=0: do not send "Range: bytes=0-".  MiniDLNA (and probably
    * other servers) answer "406 Not Acceptable" to range requests for the
    * thumbnails and resized pictures they generate on the fly, and we read
    * images front to back anyway.
    * pattern_type=none: stop the image2 demuxer from treating '%' in local
    * file names as a printf style sequence pattern. */
   av_dict_set(&opts, "rw_timeout", "10000000", 0);
   av_dict_set(&opts, "seekable", "0", 0);
   if (util_ca_file())
      av_dict_set(&opts, "ca_file", util_ca_file(), 0);
   av_dict_set(&opts, "pattern_type", "none", 0);
   if (avformat_open_input(&fmt, url, NULL, &opts) < 0)
   {
      /* avformat_open_input frees the context on failure. */
      av_dict_free(&opts);
      return NULL;
   }
   av_dict_free(&opts);

   /* Always run the stream-info pass even though it costs a decode: the
    * JPEG pipe demuxer depends on it to set up its parser, and without it
    * av_read_frame() fails with AVERROR_INVALIDDATA on some files (seen
    * with MiniDLNA's resized pictures). */
   avformat_find_stream_info(fmt, NULL);
   stream = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
   if (stream < 0)
      goto done;

   codec = avcodec_find_decoder(fmt->streams[stream]->codecpar->codec_id);
   if (!codec)
      goto done;
   ctx = avcodec_alloc_context3(codec);
   if (!ctx || avcodec_parameters_to_context(ctx,
            fmt->streams[stream]->codecpar) < 0)
      goto done;
   if (avcodec_open2(ctx, codec, NULL) < 0)
      goto done;

   frame = decode_first_frame(fmt, ctx, stream);
   if (frame)
      img = image_from_frame(frame, max_w, max_h, allow_upscale);

done:
   av_frame_free(&frame);
   avcodec_free_context(&ctx);
   avformat_close_input(&fmt);
   return img;
}

gfx_image *image_from_packet(const AVCodecParameters *par,
      const AVPacket *pkt, int max_w, int max_h)
{
   const AVCodec  *codec;
   AVCodecContext *ctx   = NULL;
   AVFrame        *frame = NULL;
   gfx_image      *img   = NULL;

   if (!par || !pkt)
      return NULL;
   codec = avcodec_find_decoder(par->codec_id);
   if (!codec)
      return NULL;
   ctx = avcodec_alloc_context3(codec);
   if (!ctx || avcodec_parameters_to_context(ctx, par) < 0
         || avcodec_open2(ctx, codec, NULL) < 0)
      goto done;
   frame = av_frame_alloc();
   if (!frame)
      goto done;
   if (avcodec_send_packet(ctx, pkt) < 0)
      goto done;
   avcodec_send_packet(ctx, NULL);
   if (avcodec_receive_frame(ctx, frame) == 0)
      img = image_from_frame(frame, max_w, max_h, 1);

done:
   av_frame_free(&frame);
   avcodec_free_context(&ctx);
   return img;
}
