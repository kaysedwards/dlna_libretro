/*
 * player.c - FFmpeg based playback.  See player.h for the overview.
 *
 * Locking: one mutex (p->lock) guards every field shared between threads:
 * the packet queues, the frame queue, the audio chunk queue, the subtitle
 * list, requests from the main thread and the serial numbers.  Each worker
 * thread has a condition variable it sleeps on.  FFmpeg calls that can take
 * a while (reading, decoding, scaling) always happen with the lock released.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>

#include <rthreads/rthreads.h>

#include "player.h"
#include "image.h"
#include "font.h"
#include "smbio.h"
#include "sshio.h"
#include "util.h"

#define MAX_TRACKS        32
#define VQ_MAX_FRAMES     8          /* decoded video frames queued     */
#define AQ_MAX_SAMPLES    (PLAYER_SAMPLE_RATE / 2) /* 0.5 s of audio   */
#define PQ_MAX_BYTES      (48L * 1024L * 1024L)
#define PQ_ENOUGH_PACKETS 30
#define PQ_ENOUGH_SECONDS 4.0

/* C90 has no NAN; timestamps we do not know are marked with this. */
#define NO_PTS            (-1.0e30)
#define HAS_PTS(t)        ((t) > -1.0e29)

enum { PK_DATA = 0, PK_FLUSH, PK_EOF };

typedef struct pkt_node
{
   struct pkt_node *next;
   int              kind;
   AVPacket        *pkt;
   int              serial;
   double           target;   /* PK_FLUSH: decode-and-discard until here */
   AVCodecContext  *new_ctx;  /* PK_FLUSH: switch to this decoder        */
   double           new_tb;   /* ...and this stream time base            */
} pkt_node;

typedef struct pkt_queue
{
   pkt_node *head, *tail;
   int       count;
   long      bytes;
   double    duration;
} pkt_queue;

typedef struct frame_node
{
   struct frame_node *next;
   AVFrame           *frame;
   double             pts;
   int                serial;
} frame_node;

typedef struct audio_chunk
{
   struct audio_chunk *next;
   int16_t            *samples;  /* interleaved stereo */
   size_t              frames;
   size_t              offset;   /* frames already played */
   double              pts;
   int                 serial;
} audio_chunk;

typedef struct sub_event
{
   struct sub_event *next;
   double            start, end;
   int               serial;
   gfx_image        *bitmap;     /* bitmap subtitles (PGS, DVD, DVB) */
   int               x, y;       /* position on the subtitle canvas  */
   int               canvas_w, canvas_h;
   char             *text;       /* text subtitles (SRT, ASS, ...)   */
   int               open_ended; /* no end time given: lasts until the
                                    next subtitle replaces it         */
} sub_event;

struct player
{
   /* --- fixed after open (published by setting state under the lock) --- */
   char            *url;
   char            *art_url;
   media_type       type;
   subs_default     subs_mode;
   AVFormatContext *fmt;
   AVIOContext     *custom_pb;     /* custom I/O for smb:// / sftp:// */
   int              custom_ssh;    /* custom_pb is SFTP (else SMB)    */
   double           start_offset;  /* container start time, seconds */
   double           duration;
   double           known_duration; /* from the server, <= 0 unknown */
   int              video_stream;  /* -1 if none                    */
   double           video_tb;
   int              video_w, video_h;
   int              audio_streams[MAX_TRACKS];
   int              num_audio;
   int              sub_streams[MAX_TRACKS];
   int              num_subs;
   char             audio_labels[MAX_TRACKS][96];
   char             sub_labels[MAX_TRACKS][96];
   int              seekable;
   gfx_image       *art;           /* cover art for audio files     */

   /* --- threads --- */
   slock_t         *lock;
   scond_t         *c_demux, *c_vdec, *c_adec;
   sthread_t       *t_demux, *t_vdec, *t_adec;
   volatile int     quit;

   /* --- shared state (lock) --- */
   player_state     state;
   char             error[256];
   int              serial;        /* current serial (demux)        */
   int              flush_serial;  /* data older than this is junk  */
   int              eof;           /* demuxer reached end of input  */
   int              v_eof_serial, a_eof_serial;
   int              v_reached_b, a_reached_b; /* serial that passed B */
   int              cur_audio;     /* index into audio_streams or -1 */
   int              cur_sub;       /* index into sub_streams or -1   */
   int              req_seek;
   double           req_seek_target;
   int              req_audio, req_audio_index;
   double           req_audio_pos;
   int              req_sub, req_sub_index;
   int              loop_on;
   double           loop_a, loop_b;

   pkt_queue        vq, aq;        /* packets for the decoders */
   frame_node      *fq_head, *fq_tail;
   int              fq_count;
   audio_chunk     *ac_head, *ac_tail;
   size_t           ac_frames;
   sub_event       *subs;

   /* --- demux thread only --- */
   AVCodecContext  *sub_ctx;
   double           sub_tb;

   /* --- decoder threads (initial contexts handed over at start) --- */
   AVCodecContext  *vctx, *actx;
   double           audio_tb;

   /* --- main thread only --- */
   double           clock;
   int              clock_serial;
   int              clock_valid;
   AVFrame         *shown;
   int              shown_changed;
   gfx_image       *layer;
   int              layer_mode;
   unsigned         layer_bg;      /* background serial of the layer */
   gfx_rect         video_rect;
   gfx_rect         art_rect;      /* where render_art put the art */
   struct SwsContext *sws;
   double           pending_target;
};

/* ------------------------------------------------------------------ */
/* Helpers */

static double ts_to_sec(player *p, int64_t ts, double tb)
{
   if (ts == AV_NOPTS_VALUE)
      return NO_PTS;
   return (double)ts * tb - p->start_offset;
}

static void set_error(player *p, const char *msg, int err)
{
   char ebuf[128];
   ebuf[0] = '\0';
   if (err)
      av_strerror(err, ebuf, sizeof(ebuf));
   slock_lock(p->lock);
   sprintf(p->error, "%.120s%s%.100s", msg, err ? ": " : "", ebuf);
   p->state = PLAYER_ERROR;
   slock_unlock(p->lock);
   dlna_log(LOG_ERROR, "%s\n", p->error);
}

static int interrupt_cb(void *opaque)
{
   return ((player*)opaque)->quit;
}

/* ------------------------------------------------------------------ */
/* Packet queues (lock held) */

static void pq_push(pkt_queue *q, pkt_node *n)
{
   n->next = NULL;
   if (q->tail)
      q->tail->next = n;
   else
      q->head = n;
   q->tail = n;
   q->count++;
   if (n->pkt)
      q->bytes += n->pkt->size + (long)sizeof(*n);
}

static pkt_node *pq_pop(pkt_queue *q, double tb)
{
   pkt_node *n = q->head;
   if (!n)
      return NULL;
   q->head = n->next;
   if (!q->head)
      q->tail = NULL;
   q->count--;
   if (n->pkt)
   {
      q->bytes -= n->pkt->size + (long)sizeof(*n);
      if (n->pkt->duration > 0)
         q->duration -= n->pkt->duration * tb;
   }
   return n;
}

static void pq_node_free(pkt_node *n)
{
   if (!n)
      return;
   av_packet_free(&n->pkt);
   avcodec_free_context(&n->new_ctx);
   free(n);
}

static void pq_clear(pkt_queue *q)
{
   pkt_node *n = q->head;
   while (n)
   {
      pkt_node *next = n->next;
      pq_node_free(n);
      n = next;
   }
   memset(q, 0, sizeof(*q));
}

static void pq_put_marker(pkt_queue *q, int kind, int serial, double target,
      AVCodecContext *new_ctx, double new_tb)
{
   pkt_node *n = (pkt_node*)calloc(1, sizeof(*n));
   if (!n)
   {
      avcodec_free_context(&new_ctx);
      return;
   }
   n->kind    = kind;
   n->serial  = serial;
   n->target  = target;
   n->new_ctx = new_ctx;
   n->new_tb  = new_tb;
   pq_push(q, n);
}

/* ------------------------------------------------------------------ */
/* Output queues (lock held) */

static void fq_clear(player *p)
{
   frame_node *n = p->fq_head;
   while (n)
   {
      frame_node *next = n->next;
      av_frame_free(&n->frame);
      free(n);
      n = next;
   }
   p->fq_head  = p->fq_tail = NULL;
   p->fq_count = 0;
}

static void ac_clear(player *p)
{
   audio_chunk *c = p->ac_head;
   while (c)
   {
      audio_chunk *next = c->next;
      free(c->samples);
      free(c);
      c = next;
   }
   p->ac_head   = p->ac_tail = NULL;
   p->ac_frames = 0;
}

static void sub_free(sub_event *e)
{
   gfx_image_free(e->bitmap);
   free(e->text);
   free(e);
}

static void subs_clear(player *p)
{
   sub_event *e = p->subs;
   while (e)
   {
      sub_event *next = e->next;
      sub_free(e);
      e = next;
   }
   p->subs = NULL;
}

/* ------------------------------------------------------------------ */
/* Opening */

static void describe_stream(AVStream *st, int number, char *buf, size_t size)
{
   AVDictionaryEntry *lang  = av_dict_get(st->metadata, "language", NULL, 0);
   AVDictionaryEntry *title = av_dict_get(st->metadata, "title", NULL, 0);
   char               layout[32];
   char               tmp[160];

   layout[0] = '\0';
   if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO
         && st->codecpar->ch_layout.nb_channels > 0)
      av_channel_layout_describe(&st->codecpar->ch_layout, layout,
            sizeof(layout));

   (void)number;
   tmp[0] = '\0';
   if (title && title->value && *title->value)
   {
      strncat(tmp, title->value, 60);
      strcat(tmp, " ");
   }
   if (lang && lang->value && *lang->value && strcmp(lang->value, "und"))
   {
      strcat(tmp, "[");
      strncat(tmp, lang->value, 8);
      strcat(tmp, "] ");
   }
   strcat(tmp, "(");
   strncat(tmp, avcodec_get_name(st->codecpar->codec_id), 20);
   if (layout[0])
   {
      strcat(tmp, ", ");
      strcat(tmp, layout); /* at most 31 chars; tmp has room */
   }
   strcat(tmp, ")");
   strncpy(buf, tmp, size - 1);
   buf[size - 1] = '\0';
}

static AVCodecContext *open_decoder(AVStream *st, int threads)
{
   const AVCodec  *codec = avcodec_find_decoder(st->codecpar->codec_id);
   AVCodecContext *ctx;

   if (!codec)
      return NULL;
   ctx = avcodec_alloc_context3(codec);
   if (!ctx)
      return NULL;
   if (avcodec_parameters_to_context(ctx, st->codecpar) < 0)
   {
      avcodec_free_context(&ctx);
      return NULL;
   }
   ctx->pkt_timebase = st->time_base;
   ctx->thread_count = threads;
   if (avcodec_open2(ctx, codec, NULL) < 0)
   {
      avcodec_free_context(&ctx);
      return NULL;
   }
   return ctx;
}

static int open_input(player *p)
{
   AVDictionary *opts = NULL;
   unsigned      i;
   int           ret, best_audio;

   p->fmt = avformat_alloc_context();
   if (!p->fmt)
      return 0;
   p->fmt->interrupt_callback.callback = interrupt_cb;
   p->fmt->interrupt_callback.opaque   = p;

   /* rw_timeout: abandon a stalled connection after 15 s.
    * reconnect*: let the HTTP layer resume a dropped stream. */
   av_dict_set(&opts, "rw_timeout", "15000000", 0);
   av_dict_set(&opts, "reconnect", "1", 0);
   av_dict_set(&opts, "reconnect_streamed", "1", 0);
   av_dict_set(&opts, "pattern_type", "none", 0);
   /* Certificate authorities for https:// (see util_find_ca_file). */
   if (util_ca_file())
      av_dict_set(&opts, "ca_file", util_ca_file(), 0);
   if (smbio_is_smb(p->url) || sshio_is_sftp(p->url))
   {
      /* SMB and SFTP are read through libsmb2 / libssh rather than FFmpeg's
       * own protocols (see smbio.h and sshio.h for why). */
      char err[300];
      p->custom_ssh = sshio_is_sftp(p->url);
      p->custom_pb  = p->custom_ssh ? sshio_open(p->url, err, sizeof(err))
                                    : smbio_open(p->url, err, sizeof(err));
      if (!p->custom_pb)
      {
         av_dict_free(&opts);
         avformat_free_context(p->fmt);
         p->fmt = NULL;
         set_error(p, err, 0);
         return 0;
      }
      p->fmt->pb     = p->custom_pb;
      p->fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
   }
   ret = avformat_open_input(&p->fmt, p->url, NULL, &opts);
   av_dict_free(&opts);
   if (ret < 0)
   {
      p->fmt = NULL;
      set_error(p, "Could not open media", ret);
      return 0;
   }
   ret = avformat_find_stream_info(p->fmt, NULL);
   if (ret < 0)
      dlna_log(LOG_WARN, "avformat_find_stream_info: %d\n", ret);

   p->start_offset = p->fmt->start_time != AV_NOPTS_VALUE
      ? (double)p->fmt->start_time / AV_TIME_BASE : 0.0;
   p->duration = p->fmt->duration != AV_NOPTS_VALUE
      ? (double)p->fmt->duration / AV_TIME_BASE : -1.0;
   /* A length estimated from the bitrate (MP3s without a VBR header, some
    * MPEG streams) can be far off; the server scanned the whole file. */
   if (p->known_duration > 0.0 && (p->duration <= 0.0
            || p->fmt->duration_estimation_method == AVFMT_DURATION_FROM_BITRATE))
      p->duration = p->known_duration;
   p->seekable = p->type != MT_IMAGE && p->duration > 0.0
      && p->fmt->pb && (p->fmt->pb->seekable & AVIO_SEEKABLE_NORMAL);

   /* Pick streams.  Embedded cover art counts as a picture, not video. */
   p->video_stream = -1;
   ret = av_find_best_stream(p->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
   if (ret >= 0)
   {
      AVStream *st = p->fmt->streams[ret];
      if (st->disposition & AV_DISPOSITION_ATTACHED_PIC)
      {
         if (p->type != MT_IMAGE)
            p->art = image_from_packet(st->codecpar, &st->attached_pic,
                  0, 0);
         else
            p->video_stream = ret; /* a picture file is just this */
      }
      else
         p->video_stream = ret;
   }
   best_audio = p->type == MT_IMAGE ? -1
      : av_find_best_stream(p->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);

   p->cur_audio = -1;
   p->cur_sub   = -1;
   for (i = 0; i < p->fmt->nb_streams; i++)
   {
      AVStream *st = p->fmt->streams[i];
      enum AVMediaType t = st->codecpar->codec_type;

      st->discard = AVDISCARD_ALL;
      if (p->type == MT_IMAGE)
         continue;
      if (t == AVMEDIA_TYPE_AUDIO && p->num_audio < MAX_TRACKS
            && avcodec_find_decoder(st->codecpar->codec_id))
      {
         if ((int)i == best_audio)
            p->cur_audio = p->num_audio;
         p->audio_streams[p->num_audio] = (int)i;
         describe_stream(st, p->num_audio + 1,
               p->audio_labels[p->num_audio], sizeof(p->audio_labels[0]));
         p->num_audio++;
      }
      else if (t == AVMEDIA_TYPE_SUBTITLE && p->num_subs < MAX_TRACKS
            && avcodec_find_decoder(st->codecpar->codec_id))
      {
         p->sub_streams[p->num_subs] = (int)i;
         describe_stream(st, p->num_subs + 1,
               p->sub_labels[p->num_subs], sizeof(p->sub_labels[0]));
         p->num_subs++;
      }
   }
   if (p->cur_audio < 0 && p->num_audio > 0)
      p->cur_audio = 0;

   /* Initial subtitle track. */
   if (p->subs_mode == SUBS_FIRST && p->num_subs > 0)
      p->cur_sub = 0;
   else if (p->subs_mode == SUBS_AUTO)
   {
      int k;
      for (k = 0; k < p->num_subs && p->cur_sub < 0; k++)
         if (p->fmt->streams[p->sub_streams[k]]->disposition
               & AV_DISPOSITION_FORCED)
            p->cur_sub = k;
      for (k = 0; k < p->num_subs && p->cur_sub < 0; k++)
         if (p->fmt->streams[p->sub_streams[k]]->disposition
               & AV_DISPOSITION_DEFAULT)
            p->cur_sub = k;
   }

   /* Open decoders. */
   if (p->video_stream >= 0)
   {
      AVStream *st = p->fmt->streams[p->video_stream];
      p->vctx = open_decoder(st, 0);
      if (!p->vctx)
         p->video_stream = -1;
      else
      {
         st->discard = AVDISCARD_DEFAULT;
         p->video_tb = av_q2d(st->time_base);
         p->video_w  = st->codecpar->width;
         p->video_h  = st->codecpar->height;
      }
   }
   if (p->cur_audio >= 0)
   {
      AVStream *st = p->fmt->streams[p->audio_streams[p->cur_audio]];
      p->actx = open_decoder(st, 1);
      if (!p->actx)
         p->cur_audio = -1;
      else
      {
         st->discard = AVDISCARD_DEFAULT;
         p->audio_tb = av_q2d(st->time_base);
      }
   }
   if (p->cur_sub >= 0)
   {
      AVStream *st = p->fmt->streams[p->sub_streams[p->cur_sub]];
      p->sub_ctx = open_decoder(st, 1);
      if (!p->sub_ctx)
         p->cur_sub = -1;
      else
      {
         st->discard = AVDISCARD_DEFAULT;
         p->sub_tb = av_q2d(st->time_base);
      }
   }

   if (p->video_stream < 0 && p->cur_audio < 0)
   {
      set_error(p, "No playable audio or video in this file", 0);
      return 0;
   }

   /* Audio files without embedded art: try the server's album art. */
   if (p->type != MT_IMAGE && p->video_stream < 0 && !p->art && p->art_url)
      p->art = image_load(p->art_url, 0, 0, 0, &p->quit);
   return 1;
}

/* ------------------------------------------------------------------ */
/* Subtitles (demux thread) */

/* Pull the text out of an ASS "Dialogue" payload: skip the first eight
 * comma separated fields, drop {override} blocks, turn \N into newlines. */
static char *ass_to_text(const char *ass)
{
   const char *p = ass;
   char       *out, *o;
   int         commas = 0;

   while (*p && commas < 8)
      if (*p++ == ',')
         commas++;
   if (!*p && commas < 8)
      p = ass; /* not in the expected format; show it all */
   out = (char*)malloc(strlen(p) + 1);
   if (!out)
      return NULL;
   o = out;
   while (*p)
   {
      if (*p == '{')
      {
         const char *close = strchr(p, '}');
         if (close)
         {
            p = close + 1;
            continue;
         }
      }
      if (p[0] == '\\' && (p[1] == 'N' || p[1] == 'n'))
      {
         *o++ = '\n';
         p   += 2;
         continue;
      }
      if (p[0] == '\\' && p[1] == 'h')
      {
         *o++ = ' ';
         p   += 2;
         continue;
      }
      if (*p == '\r')
      {
         p++;
         continue;
      }
      *o++ = *p++;
   }
   *o = '\0';
   util_trim(out);
   return out;
}

static gfx_image *sub_bitmap(const AVSubtitleRect *r)
{
   gfx_image      *img;
   const uint32_t *pal;
   int             x, y;

   if (r->w <= 0 || r->h <= 0 || !r->data[0] || !r->data[1])
      return NULL;
   img = gfx_image_new(r->w, r->h);
   if (!img)
      return NULL;
   /* data[0]: palette indices, data[1]: palette of 0xAARRGGBB values. */
   pal = (const uint32_t*)r->data[1];
   for (y = 0; y < r->h; y++)
      for (x = 0; x < r->w; x++)
      {
         unsigned idx = r->data[0][y * r->linesize[0] + x];
         img->pixels[y * r->w + x] = idx < (unsigned)r->nb_colors ? pal[idx] : 0;
      }
   return img;
}

static void decode_subtitle(player *p, AVPacket *pkt)
{
   AVSubtitle sub;
   int        got = 0;
   double     base, start, end;
   unsigned   i;
   int        canvas_w, canvas_h;
   int        open_ended = 0;
   sub_event *list = NULL, **tail = &list;

   if (!p->sub_ctx)
      return;
   memset(&sub, 0, sizeof(sub));
   if (avcodec_decode_subtitle2(p->sub_ctx, &sub, &got, pkt) < 0 || !got)
      return;

   /* AVSubtitle.pts is in AV_TIME_BASE units. */
   if (sub.pts != AV_NOPTS_VALUE)
      base = (double)sub.pts / AV_TIME_BASE - p->start_offset;
   else
      base = ts_to_sec(p, pkt->pts, p->sub_tb);
   if (!HAS_PTS(base))
   {
      avsubtitle_free(&sub);
      return;
   }
   start = base + sub.start_display_time / 1000.0;
   if (sub.end_display_time > sub.start_display_time
         && sub.end_display_time != 0xFFFFFFFFu)
      end = base + sub.end_display_time / 1000.0;
   else if (pkt->duration > 0)
      end = start + pkt->duration * p->sub_tb;
   else
   {
      end        = start + 8.0; /* until replaced, but not forever */
      open_ended = 1;
   }

   canvas_w = p->sub_ctx->width;
   canvas_h = p->sub_ctx->height;
   if (canvas_w <= 0 || canvas_h <= 0)
   {
      /* Bitmap subtitles are positioned on the video frame. */
      canvas_w = p->video_w;
      canvas_h = p->video_h;
   }

   for (i = 0; i < sub.num_rects; i++)
   {
      AVSubtitleRect *r = sub.rects[i];
      sub_event      *e = (sub_event*)calloc(1, sizeof(*e));
      if (!e)
         break;
      e->start    = start;
      e->end      = end;
      e->serial   = p->serial;
      e->canvas_w = canvas_w;
      e->canvas_h = canvas_h;
      e->open_ended = open_ended;
      if (r->type == SUBTITLE_BITMAP)
      {
         e->bitmap = sub_bitmap(r);
         e->x      = r->x;
         e->y      = r->y;
      }
      else if (r->type == SUBTITLE_ASS && r->ass)
         e->text = ass_to_text(r->ass);
      else if (r->text)
         e->text = util_strdup(r->text);
      if (!e->bitmap && (!e->text || !*e->text))
      {
         sub_free(e);
         continue;
      }
      *tail = e;
      tail  = &e->next;
   }

   slock_lock(p->lock);
   {
      /* Bitmap subtitles (PGS, DVD, DVB) replace whatever is on screen,
       * and an empty event means "clear the screen".  Text subtitles
       * with real end times may overlap (common in ASS), so only those
       * without an end time are cut short by the next one. */
      sub_event *e;
      for (e = p->subs; e; e = e->next)
         if (e->serial == p->serial && e->start < start && e->end > start
               && (e->bitmap || e->open_ended || sub.num_rects == 0))
            e->end = start;
      /* Append. */
      if (list)
      {
         sub_event **pp = &p->subs;
         while (*pp)
            pp = &(*pp)->next;
         *pp = list;
      }
   }
   slock_unlock(p->lock);
   avsubtitle_free(&sub);
}

/* ------------------------------------------------------------------ */
/* Seeking (demux thread, lock held) */

static void do_seek(player *p, double target, int flush)
{
   int64_t ts;
   int     ret;

   if (target < 0.0)
      target = 0.0;
   ts = (int64_t)((target + p->start_offset) * AV_TIME_BASE);

   /* Seeking may wait on the network, so do it without the lock (only
    * this thread ever touches p->fmt once playback has started).  Anything
    * the decoders queue meanwhile carries the old serial and is dealt with
    * below. */
   slock_unlock(p->lock);
   ret = avformat_seek_file(p->fmt, -1, INT64_MIN, ts, ts, 0);
   if (ret < 0)
      ret = av_seek_frame(p->fmt, -1, ts, AVSEEK_FLAG_BACKWARD);
   if (ret < 0)
      dlna_log(LOG_WARN, "Seek to %.2f failed\n", target);
   slock_lock(p->lock);

   p->serial++;
   if (flush)
   {
      p->flush_serial = p->serial;
      fq_clear(p);
      ac_clear(p);
      subs_clear(p);
   }
   pq_clear(&p->vq);
   pq_clear(&p->aq);
   if (p->video_stream >= 0)
      pq_put_marker(&p->vq, PK_FLUSH, p->serial, target, NULL, 0.0);
   if (p->cur_audio >= 0)
      pq_put_marker(&p->aq, PK_FLUSH, p->serial, target, NULL, 0.0);
   if (p->sub_ctx)
      avcodec_flush_buffers(p->sub_ctx);
   p->eof = 0;
   scond_broadcast(p->c_vdec);
   scond_broadcast(p->c_adec);
}

/* Handle requests from the main thread and A-B loop wraps.  Lock held. */
static void handle_requests(player *p)
{
   if (p->req_sub)
   {
      int idx = p->req_sub_index;
      p->req_sub = 0;
      if (p->cur_sub >= 0)
         p->fmt->streams[p->sub_streams[p->cur_sub]]->discard = AVDISCARD_ALL;
      avcodec_free_context(&p->sub_ctx);
      subs_clear(p);
      p->cur_sub = -1;
      if (idx >= 0 && idx < p->num_subs)
      {
         AVStream *st = p->fmt->streams[p->sub_streams[idx]];
         p->sub_ctx = open_decoder(st, 1);
         if (p->sub_ctx)
         {
            st->discard = AVDISCARD_DEFAULT;
            p->sub_tb   = av_q2d(st->time_base);
            p->cur_sub  = idx;
         }
      }
   }

   if (p->req_audio)
   {
      int idx = p->req_audio_index;
      p->req_audio = 0;
      if (idx >= 0 && idx < p->num_audio && idx != p->cur_audio)
      {
         AVStream       *st  = p->fmt->streams[p->audio_streams[idx]];
         AVCodecContext *ctx = open_decoder(st, 1);
         if (ctx)
         {
            if (p->cur_audio >= 0)
               p->fmt->streams[p->audio_streams[p->cur_audio]]->discard =
                  AVDISCARD_ALL;
            st->discard  = AVDISCARD_DEFAULT;
            p->cur_audio = idx;
            /* Seek back to where we are so the new track starts in sync;
             * the decoder switches contexts at the flush marker. */
            do_seek(p, p->req_audio_pos, 1);
            pq_clear(&p->aq);
            pq_put_marker(&p->aq, PK_FLUSH, p->serial, p->req_audio_pos,
                  ctx, av_q2d(st->time_base));
            scond_broadcast(p->c_adec);
         }
      }
   }

   if (p->req_seek)
   {
      /* req_seek stays set while seeking so the main thread keeps the
       * clock still; if a newer target arrived meanwhile, go again. */
      double target = p->req_seek_target;
      do_seek(p, target, 1);
      if (p->req_seek_target == target)
         p->req_seek = 0;
   }

   /* A-B loop: once every active decoder has passed B, go back to A
    * without flushing, so what is queued before B still plays. */
   if (p->loop_on
         && (p->video_stream < 0 || p->v_reached_b == p->serial)
         && (p->cur_audio < 0 || p->a_reached_b == p->serial))
      do_seek(p, p->loop_a, 0);
}

static int queues_full(player *p)
{
   int video_enough, audio_enough;

   if (p->vq.bytes + p->aq.bytes > PQ_MAX_BYTES)
      return 1;
   video_enough = p->video_stream < 0 || (p->vq.count > PQ_ENOUGH_PACKETS
         && (p->vq.duration <= 0.0 || p->vq.duration > PQ_ENOUGH_SECONDS));
   audio_enough = p->cur_audio < 0 || (p->aq.count > PQ_ENOUGH_PACKETS
         && (p->aq.duration <= 0.0 || p->aq.duration > PQ_ENOUGH_SECONDS));
   return video_enough && audio_enough;
}

static int requests_pending(player *p)
{
   return p->req_seek || p->req_audio || p->req_sub
      || (p->loop_on
         && (p->video_stream < 0 || p->v_reached_b == p->serial)
         && (p->cur_audio < 0 || p->a_reached_b == p->serial));
}

/* ------------------------------------------------------------------ */
/* Video decoder thread */

static void push_frame(player *p, AVFrame *frame, double pts, int serial)
{
   frame_node *n = (frame_node*)calloc(1, sizeof(*n));
   if (!n)
   {
      av_frame_free(&frame);
      return;
   }
   n->frame  = frame;
   n->pts    = pts;
   n->serial = serial;
   slock_lock(p->lock);
   /* Wait for room, unless this frame went stale meanwhile. */
   while (!p->quit && p->fq_count >= VQ_MAX_FRAMES && serial >= p->flush_serial
         && p->type != MT_IMAGE)
      scond_wait(p->c_vdec, p->lock);
   if (p->quit || serial < p->flush_serial)
   {
      slock_unlock(p->lock);
      av_frame_free(&n->frame);
      free(n);
      return;
   }
   if (p->fq_tail)
      p->fq_tail->next = n;
   else
      p->fq_head = n;
   p->fq_tail = n;
   p->fq_count++;
   slock_unlock(p->lock);
}

static void video_thread(void *arg)
{
   player         *p      = (player*)arg;
   AVCodecContext *ctx    = p->vctx;
   double          tb     = p->video_tb;
   AVFrame        *frame  = av_frame_alloc();
   AVFrame        *held   = NULL;   /* last frame before a seek target */
   int             serial = 0;
   double          skip_until = -1.0;
   double          last_pts   = NO_PTS;
   double          frame_dur  = 1.0 / 25.0;
   AVRational      rate;

   p->vctx = NULL; /* this thread owns it now */
   rate    = av_guess_frame_rate(p->fmt, p->fmt->streams[p->video_stream], NULL);
   if (rate.num > 0 && rate.den > 0)
      frame_dur = (double)rate.den / rate.num;

   while (frame)
   {
      pkt_node *n;
      int       eof = 0;

      slock_lock(p->lock);
      while (!p->quit && !p->vq.head)
         scond_wait(p->c_vdec, p->lock);
      if (p->quit)
      {
         slock_unlock(p->lock);
         break;
      }
      n = pq_pop(&p->vq, tb);
      scond_signal(p->c_demux);
      slock_unlock(p->lock);

      if (n->kind == PK_FLUSH)
      {
         if (n->new_ctx)
         {
            avcodec_free_context(&ctx);
            ctx        = n->new_ctx;
            n->new_ctx = NULL;
            tb         = n->new_tb;
         }
         else if (ctx)
            avcodec_flush_buffers(ctx);
         serial     = n->serial;
         skip_until = n->target;
         last_pts   = NO_PTS;
         av_frame_free(&held);
         pq_node_free(n);
         continue;
      }
      if (!ctx)
      {
         pq_node_free(n);
         continue;
      }
      if (n->kind == PK_EOF)
      {
         avcodec_send_packet(ctx, NULL);
         eof = 1;
      }
      else if (avcodec_send_packet(ctx, n->pkt) < 0)
      {
         pq_node_free(n);
         continue;
      }
      pq_node_free(n);

      for (;;)
      {
         double pts;
         int    ret = avcodec_receive_frame(ctx, frame);
         if (ret < 0)
            break;

         pts = ts_to_sec(p, frame->best_effort_timestamp, tb);
         if (!HAS_PTS(pts))
            pts = HAS_PTS(last_pts) ? last_pts + frame_dur : 0.0;
         last_pts = pts;

         if (skip_until >= 0.0 && p->type != MT_IMAGE)
         {
            double dur = frame->duration > 0 ? frame->duration * tb : frame_dur;
            if (pts + dur <= skip_until)
            {
               /* Before the target: keep only the newest such frame. */
               av_frame_free(&held);
               held = av_frame_clone(frame);
               av_frame_unref(frame);
               continue;
            }
            if (held && pts > skip_until + 0.001)
               push_frame(p, held, skip_until, serial);
            else
               av_frame_free(&held);
            held       = NULL;
            skip_until = -1.0;
         }

         slock_lock(p->lock);
         if (p->loop_on && serial == p->serial && pts >= p->loop_b)
         {
            p->v_reached_b = serial;
            scond_signal(p->c_demux);
            slock_unlock(p->lock);
            av_frame_unref(frame);
            continue;
         }
         slock_unlock(p->lock);

         push_frame(p, av_frame_clone(frame), pts, serial);
         av_frame_unref(frame);
      }

      if (eof)
      {
         if (held)
         {
            push_frame(p, held, skip_until >= 0.0 ? skip_until : last_pts,
                  serial);
            held = NULL;
         }
         slock_lock(p->lock);
         p->v_eof_serial = serial;
         if (p->loop_on && serial == p->serial)
            p->v_reached_b = serial;
         scond_signal(p->c_demux);
         slock_unlock(p->lock);
         avcodec_flush_buffers(ctx);
      }
   }

   av_frame_free(&held);
   av_frame_free(&frame);
   avcodec_free_context(&ctx);
}

/* ------------------------------------------------------------------ */
/* Audio decoder thread */

typedef struct audio_dec
{
   SwrContext     *swr;
   int             in_rate;
   int             in_format;
   AVChannelLayout in_layout;
} audio_dec;

static int setup_swr(audio_dec *d, const AVFrame *frame)
{
   AVChannelLayout out = AV_CHANNEL_LAYOUT_STEREO;

   if (d->swr && d->in_rate == frame->sample_rate
         && d->in_format == frame->format
         && av_channel_layout_compare(&d->in_layout, &frame->ch_layout) == 0)
      return 1;

   swr_free(&d->swr);
   av_channel_layout_uninit(&d->in_layout);
   if (swr_alloc_set_opts2(&d->swr, &out, AV_SAMPLE_FMT_S16,
            PLAYER_SAMPLE_RATE, &frame->ch_layout,
            (enum AVSampleFormat)frame->format, frame->sample_rate,
            0, NULL) < 0 || swr_init(d->swr) < 0)
   {
      swr_free(&d->swr);
      return 0;
   }
   d->in_rate   = frame->sample_rate;
   d->in_format = frame->format;
   av_channel_layout_copy(&d->in_layout, &frame->ch_layout);
   return 1;
}

static void push_audio(player *p, int16_t *samples, size_t frames,
      double pts, int serial)
{
   audio_chunk *c = (audio_chunk*)calloc(1, sizeof(*c));
   if (!c)
   {
      free(samples);
      return;
   }
   c->samples = samples;
   c->frames  = frames;
   c->pts     = pts;
   c->serial  = serial;
   slock_lock(p->lock);
   while (!p->quit && p->ac_frames >= AQ_MAX_SAMPLES
         && serial >= p->flush_serial)
      scond_wait(p->c_adec, p->lock);
   if (p->quit || serial < p->flush_serial)
   {
      slock_unlock(p->lock);
      free(c->samples);
      free(c);
      return;
   }
   if (p->ac_tail)
      p->ac_tail->next = c;
   else
      p->ac_head = c;
   p->ac_tail    = c;
   p->ac_frames += frames;
   slock_unlock(p->lock);
}

static void process_audio_frame(player *p, audio_dec *d, AVFrame *frame,
      double tb, int serial, double *skip_until, double *next_pts)
{
   int      max_out, got;
   int16_t *buf;
   uint8_t *outp[1];
   double   pts, dur;
   size_t   first = 0, count;

   if (!setup_swr(d, frame))
      return;
   max_out = (int)av_rescale_rnd(swr_get_delay(d->swr, frame->sample_rate)
         + frame->nb_samples, PLAYER_SAMPLE_RATE, frame->sample_rate,
         AV_ROUND_UP) + 32;
   buf = (int16_t*)malloc((size_t)max_out * 4);
   if (!buf)
      return;
   outp[0] = (uint8_t*)buf;
   got = swr_convert(d->swr, outp, max_out,
         (const uint8_t**)frame->extended_data, frame->nb_samples);
   if (got <= 0)
   {
      free(buf);
      return;
   }

   pts = ts_to_sec(p, frame->best_effort_timestamp, tb);
   if (!HAS_PTS(pts))
      pts = HAS_PTS(*next_pts) ? *next_pts : 0.0;
   dur       = (double)got / PLAYER_SAMPLE_RATE;
   *next_pts = pts + dur;
   count     = (size_t)got;

   /* Accurate seeking: drop everything before the target. */
   if (*skip_until >= 0.0)
   {
      if (pts + dur <= *skip_until)
      {
         free(buf);
         return;
      }
      if (pts < *skip_until)
      {
         first = (size_t)((*skip_until - pts) * PLAYER_SAMPLE_RATE);
         if (first >= count)
            first = count - 1;
         pts = *skip_until;
      }
      *skip_until = -1.0;
   }

   /* A-B loop: cut exactly at B. */
   slock_lock(p->lock);
   if (p->loop_on && serial == p->serial)
   {
      if (pts >= p->loop_b)
         count = first;
      else if (pts + (double)(count - first) / PLAYER_SAMPLE_RATE > p->loop_b)
         count = first + (size_t)((p->loop_b - pts) * PLAYER_SAMPLE_RATE);
      if (count < (size_t)got)
      {
         p->a_reached_b = serial;
         scond_signal(p->c_demux);
      }
   }
   slock_unlock(p->lock);

   if (count <= first)
   {
      free(buf);
      return;
   }
   if (first)
      memmove(buf, buf + first * 2, (count - first) * 4);
   push_audio(p, buf, count - first, pts, serial);
}

static void audio_thread(void *arg)
{
   player         *p      = (player*)arg;
   AVCodecContext *ctx    = p->actx;
   double          tb     = p->audio_tb;
   AVFrame        *frame  = av_frame_alloc();
   audio_dec       dec;
   int             serial = 0;
   double          skip_until = -1.0;
   double          next_pts   = NO_PTS;

   p->actx = NULL; /* this thread owns it now */
   memset(&dec, 0, sizeof(dec));

   while (frame)
   {
      pkt_node *n;
      int       eof = 0;

      slock_lock(p->lock);
      while (!p->quit && !p->aq.head)
         scond_wait(p->c_adec, p->lock);
      if (p->quit)
      {
         slock_unlock(p->lock);
         break;
      }
      n = pq_pop(&p->aq, tb);
      scond_signal(p->c_demux);
      slock_unlock(p->lock);

      if (n->kind == PK_FLUSH)
      {
         if (n->new_ctx)
         {
            avcodec_free_context(&ctx);
            ctx        = n->new_ctx;
            n->new_ctx = NULL;
            tb         = n->new_tb;
         }
         else if (ctx)
            avcodec_flush_buffers(ctx);
         /* Drop whatever the resampler still holds. */
         swr_free(&dec.swr);
         serial     = n->serial;
         skip_until = n->target;
         next_pts   = NO_PTS;
         pq_node_free(n);
         continue;
      }
      if (!ctx)
      {
         pq_node_free(n);
         continue;
      }
      if (n->kind == PK_EOF)
      {
         avcodec_send_packet(ctx, NULL);
         eof = 1;
      }
      else if (avcodec_send_packet(ctx, n->pkt) < 0)
      {
         pq_node_free(n);
         continue;
      }
      pq_node_free(n);

      while (avcodec_receive_frame(ctx, frame) >= 0)
      {
         process_audio_frame(p, &dec, frame, tb, serial, &skip_until,
               &next_pts);
         av_frame_unref(frame);
      }

      if (eof)
      {
         slock_lock(p->lock);
         p->a_eof_serial = serial;
         if (p->loop_on && serial == p->serial)
            p->a_reached_b = serial;
         scond_signal(p->c_demux);
         slock_unlock(p->lock);
         avcodec_flush_buffers(ctx);
      }
   }

   swr_free(&dec.swr);
   av_channel_layout_uninit(&dec.in_layout);
   av_frame_free(&frame);
   avcodec_free_context(&ctx);
}

/* ------------------------------------------------------------------ */
/* Demux thread */

static void demux_thread(void *arg)
{
   player   *p   = (player*)arg;
   AVPacket *pkt = av_packet_alloc();

   if (!pkt || !open_input(p))
   {
      av_packet_free(&pkt);
      if (p->state != PLAYER_ERROR)
         set_error(p, "Could not open media", 0);
      return;
   }

   slock_lock(p->lock);
   p->serial       = 1;
   p->flush_serial = 1;
   if (p->video_stream >= 0)
      pq_put_marker(&p->vq, PK_FLUSH, 1, -1.0, NULL, 0.0);
   if (p->cur_audio >= 0)
      pq_put_marker(&p->aq, PK_FLUSH, 1, -1.0, NULL, 0.0);
   p->state = PLAYER_PLAYING;
   slock_unlock(p->lock);

   if (p->video_stream >= 0)
      p->t_vdec = sthread_create(video_thread, p);
   if (p->cur_audio >= 0)
      p->t_adec = sthread_create(audio_thread, p);

   for (;;)
   {
      int ret, si;

      slock_lock(p->lock);
      while (!p->quit)
      {
         if (requests_pending(p))
            handle_requests(p);
         if (p->eof || queues_full(p))
         {
            scond_wait_timeout(p->c_demux, p->lock, 100000);
            continue;
         }
         break;
      }
      if (p->quit)
      {
         slock_unlock(p->lock);
         break;
      }
      slock_unlock(p->lock);

      ret = av_read_frame(p->fmt, pkt);

      slock_lock(p->lock);
      if (ret < 0)
      {
         if (ret == AVERROR(EAGAIN))
         {
            slock_unlock(p->lock);
            av_usleep(10000);
            continue;
         }
         if (ret != AVERROR_EOF && !p->quit)
            dlna_log(LOG_WARN, "Read error %d; treating as end of file\n", ret);
         if (p->video_stream >= 0)
            pq_put_marker(&p->vq, PK_EOF, p->serial, 0.0, NULL, 0.0);
         if (p->cur_audio >= 0)
            pq_put_marker(&p->aq, PK_EOF, p->serial, 0.0, NULL, 0.0);
         p->eof = 1;
         scond_broadcast(p->c_vdec);
         scond_broadcast(p->c_adec);
         slock_unlock(p->lock);
         continue;
      }

      si = pkt->stream_index;
      if (si == p->video_stream || (p->cur_audio >= 0
               && si == p->audio_streams[p->cur_audio]))
      {
         pkt_node *n = (pkt_node*)calloc(1, sizeof(*n));
         if (n)
         {
            n->kind   = PK_DATA;
            n->serial = p->serial;
            n->pkt    = av_packet_alloc();
            if (n->pkt)
               av_packet_move_ref(n->pkt, pkt);
            if (!n->pkt)
               free(n);
            else if (si == p->video_stream)
            {
               pq_push(&p->vq, n);
               if (n->pkt->duration > 0)
                  p->vq.duration += n->pkt->duration * p->video_tb;
               scond_signal(p->c_vdec);
            }
            else
            {
               double tb = av_q2d(p->fmt->streams[si]->time_base);
               pq_push(&p->aq, n);
               if (n->pkt->duration > 0)
                  p->aq.duration += n->pkt->duration * tb;
               scond_signal(p->c_adec);
            }
         }
         slock_unlock(p->lock);
      }
      else if (p->cur_sub >= 0 && si == p->sub_streams[p->cur_sub])
      {
         slock_unlock(p->lock);
         decode_subtitle(p, pkt);
      }
      else
         slock_unlock(p->lock);
      av_packet_unref(pkt);
   }

   /* Shut down the decoders before tearing anything down. */
   slock_lock(p->lock);
   scond_broadcast(p->c_vdec);
   scond_broadcast(p->c_adec);
   slock_unlock(p->lock);
   if (p->t_vdec)
      sthread_join(p->t_vdec);
   if (p->t_adec)
      sthread_join(p->t_adec);
   p->t_vdec = p->t_adec = NULL;
   av_packet_free(&pkt);
}

/* ------------------------------------------------------------------ */
/* Public API */

player *player_open(const char *url, media_type type, const char *art_url,
      subs_default subs, double known_duration)
{
   player *p = (player*)calloc(1, sizeof(*p));
   if (!p)
      return NULL;
   p->url          = util_strdup(url);
   p->art_url      = util_strdup(art_url);
   p->type         = type;
   p->subs_mode    = subs;
   p->video_stream = -1;
   p->cur_audio    = -1;
   p->cur_sub      = -1;
   p->duration     = -1.0;
   p->known_duration = known_duration;
   p->layer_mode   = -1;
   p->pending_target = -1.0;
   p->v_eof_serial = p->a_eof_serial = -1;
   p->v_reached_b  = p->a_reached_b  = -1;
   p->lock    = slock_new();
   p->c_demux = scond_new();
   p->c_vdec  = scond_new();
   p->c_adec  = scond_new();
   if (!p->url || !p->lock || !p->c_demux || !p->c_vdec || !p->c_adec)
   {
      player_close(p);
      return NULL;
   }
   p->state   = PLAYER_OPENING;
   p->t_demux = sthread_create(demux_thread, p);
   if (!p->t_demux)
   {
      player_close(p);
      return NULL;
   }
   return p;
}

void player_close(player *p)
{
   if (!p)
      return;
   if (p->lock)
   {
      slock_lock(p->lock);
      p->quit = 1;
      scond_broadcast(p->c_demux);
      scond_broadcast(p->c_vdec);
      scond_broadcast(p->c_adec);
      slock_unlock(p->lock);
   }
   if (p->t_demux)
      sthread_join(p->t_demux);

   pq_clear(&p->vq);
   pq_clear(&p->aq);
   fq_clear(p);
   ac_clear(p);
   subs_clear(p);
   avcodec_free_context(&p->vctx);
   avcodec_free_context(&p->actx);
   avcodec_free_context(&p->sub_ctx);
   if (p->fmt)
      avformat_close_input(&p->fmt);
   /* Custom I/O is not freed by FFmpeg. */
   if (p->custom_ssh)
      sshio_close(&p->custom_pb);
   else
      smbio_close(&p->custom_pb);
   av_frame_free(&p->shown);
   gfx_image_free(p->layer);
   gfx_image_free(p->art);
   if (p->sws)
      sws_freeContext(p->sws);
   if (p->c_demux)
      scond_free(p->c_demux);
   if (p->c_vdec)
      scond_free(p->c_vdec);
   if (p->c_adec)
      scond_free(p->c_adec);
   if (p->lock)
      slock_free(p->lock);
   free(p->url);
   free(p->art_url);
   free(p);
}

player_state player_get_state(player *p, char *error, size_t size)
{
   player_state s;
   slock_lock(p->lock);
   s = p->state;
   if (s == PLAYER_PLAYING && p->type != MT_IMAGE && p->eof && !p->loop_on
         && !p->req_seek
         && (p->cur_audio < 0 || (p->a_eof_serial == p->serial && !p->ac_head))
         && (p->video_stream < 0
            || (p->v_eof_serial == p->serial && !p->fq_head)))
      s = PLAYER_ENDED;
   if (error && size)
   {
      strncpy(error, p->error, size - 1);
      error[size - 1] = '\0';
   }
   slock_unlock(p->lock);
   return s;
}

/* Is the audio track still delivering for the current serial?  Once it
 * ends (or if there is none) the clock runs freely.  Lock held. */
static int audio_drives_clock(player *p)
{
   if (p->cur_audio < 0)
      return 0;
   if (p->a_eof_serial == p->serial && !p->ac_head)
      return 0;
   return 1;
}

void player_audio(player *p, int16_t *out, size_t frames, int paused)
{
   size_t done = 0;

   memset(out, 0, frames * 4);
   slock_lock(p->lock);

   /* Throw away audio from before a user seek. */
   while (p->ac_head && p->ac_head->serial < p->flush_serial)
   {
      audio_chunk *c = p->ac_head;
      p->ac_head    = c->next;
      p->ac_frames -= c->frames - c->offset;
      free(c->samples);
      free(c);
   }
   if (!p->ac_head)
      p->ac_tail = NULL;

   if (p->state != PLAYER_PLAYING || paused || p->req_seek
         || p->type == MT_IMAGE)
   {
      slock_unlock(p->lock);
      return;
   }

   if (audio_drives_clock(p))
   {
      while (done < frames && p->ac_head)
      {
         audio_chunk *c     = p->ac_head;
         size_t       avail = c->frames - c->offset;
         size_t       take  = frames - done < avail ? frames - done : avail;

         memcpy(out + done * 2, c->samples + c->offset * 2, take * 4);
         c->offset    += take;
         done         += take;
         p->ac_frames -= take;
         p->clock        = c->pts + (double)c->offset / PLAYER_SAMPLE_RATE;
         p->clock_serial = c->serial;
         p->clock_valid  = 1;
         if (c->offset >= c->frames)
         {
            p->ac_head = c->next;
            if (!p->ac_head)
               p->ac_tail = NULL;
            free(c->samples);
            free(c);
         }
      }
      scond_signal(p->c_adec);
   }
   else if (p->clock_valid)
   {
      /* No audio to follow: advance by wall time, but wait for the
       * decoder if it has fallen behind. */
      int have_frame = 0;
      frame_node *n;
      for (n = p->fq_head; n; n = n->next)
         if (n->serial >= p->clock_serial)
         {
            have_frame = 1;
            break;
         }
      if (have_frame || p->v_eof_serial == p->serial || p->video_stream < 0)
         p->clock += (double)frames / PLAYER_SAMPLE_RATE;
   }
   slock_unlock(p->lock);
}

/* Advance the frame queue to the frame that should be on screen now.
 * Lock held. */
static void update_shown_frame(player *p)
{
   for (;;)
   {
      frame_node *n = p->fq_head;
      int         take = 0;

      if (!n)
         break;
      if (n->serial < p->flush_serial)
         take = -1;
      else if (p->type == MT_IMAGE)
         take = 1;
      else if (!p->clock_valid || n->serial > p->clock_serial)
      {
         /* With no audio driving the clock, adopt the new serial (after a
          * seek or loop wrap) as soon as its first frame shows up. */
         if (!audio_drives_clock(p) && !p->req_seek)
         {
            p->clock        = n->pts;
            p->clock_serial = n->serial;
            p->clock_valid  = 1;
            take            = 1;
         }
         else
            break;
      }
      else if (n->serial < p->clock_serial)
         take = -1;
      else if (n->pts <= p->clock + 0.008)
         take = 1;
      else
         break;

      p->fq_head = n->next;
      if (!p->fq_head)
         p->fq_tail = NULL;
      p->fq_count--;
      if (take > 0)
      {
         av_frame_free(&p->shown);
         p->shown         = n->frame;
         p->shown_changed = 1;
      }
      else
         av_frame_free(&n->frame);
      free(n);
      scond_signal(p->c_vdec);
   }
}

static double mode_aspect(aspect_mode mode, double media_aspect)
{
   switch (mode)
   {
      case ASPECT_16_9: return 16.0 / 9.0;
      case ASPECT_4_3:  return 4.0 / 3.0;
      case ASPECT_5_4:  return 5.0 / 4.0;
      case ASPECT_FULL: return 0.0;
      default:          break;
   }
   return media_aspect;
}

static int sws_colorspace(const AVFrame *f)
{
   switch (f->colorspace)
   {
      case AVCOL_SPC_BT709:      return SWS_CS_ITU709;
      case AVCOL_SPC_FCC:        return SWS_CS_FCC;
      case AVCOL_SPC_SMPTE240M:  return SWS_CS_SMPTE240M;
      case AVCOL_SPC_BT2020_NCL:
      case AVCOL_SPC_BT2020_CL:  return SWS_CS_BT2020;
      case AVCOL_SPC_BT470BG:
      case AVCOL_SPC_SMPTE170M:  return SWS_CS_ITU601;
      default:                   break;
   }
   return f->height >= 720 ? SWS_CS_ITU709 : SWS_CS_ITU601;
}

/* Scale the shown frame into the layer.  Main thread. */
static void render_frame(player *p, gfx_rect full, aspect_mode mode)
{
   AVFrame  *f = p->shown;
   int       rot = image_frame_rotation(f);
   double    dar;
   gfx_rect  r;
   int       sw, sh;
   uint32_t *dst;
   uint32_t *tmp = NULL;
   uint8_t  *dst_data[4];
   int       dst_lines[4];
   AVRational sar = f->sample_aspect_ratio;

   if (sar.num <= 0 || sar.den <= 0)
   {
      sar.num = 1;
      sar.den = 1;
   }
   dar = (double)f->width * sar.num / ((double)f->height * sar.den);
   if (rot == 90 || rot == 270)
      dar = 1.0 / dar;
   r = gfx_fit_aspect(full, mode_aspect(mode, dar));
   p->video_rect = r;

   /* Scale to the pre-rotation size, then rotate into place. */
   if (rot == 90 || rot == 270)
   {
      sw = r.h;
      sh = r.w;
   }
   else
   {
      sw = r.w;
      sh = r.h;
   }
   if (rot)
   {
      tmp = (uint32_t*)malloc((size_t)sw * sh * 4);
      if (!tmp)
         return;
      dst          = tmp;
      dst_lines[0] = sw * 4;
   }
   else
   {
      dst          = p->layer->pixels + (size_t)r.y * p->layer->width + r.x;
      dst_lines[0] = p->layer->width * 4;
   }
   dst_data[0] = (uint8_t*)dst;
   dst_data[1] = dst_data[2] = dst_data[3] = NULL;
   dst_lines[1] = dst_lines[2] = dst_lines[3] = 0;

   p->sws = sws_getCachedContext(p->sws, f->width, f->height,
         (enum AVPixelFormat)f->format, sw, sh, AV_PIX_FMT_RGB32,
         SWS_BILINEAR, NULL, NULL, NULL);
   if (!p->sws)
   {
      free(tmp);
      return;
   }
   {
      /* Apply the right YUV matrix and range; harmless for RGB input. */
      int cs = sws_colorspace(f);
      sws_setColorspaceDetails(p->sws, sws_getCoefficients(cs),
            f->color_range == AVCOL_RANGE_JPEG,
            sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16);
   }
   sws_scale(p->sws, (const uint8_t * const*)f->data, f->linesize, 0,
         f->height, dst_data, dst_lines);

   if (tmp)
   {
      int x, y;
      for (y = 0; y < sh; y++)
         for (x = 0; x < sw; x++)
         {
            uint32_t px = tmp[(size_t)y * sw + x];
            int      dx, dy;
            if (rot == 90)
            {
               dx = sh - 1 - y;
               dy = x;
            }
            else if (rot == 270)
            {
               dx = y;
               dy = sw - 1 - x;
            }
            else
            {
               dx = sw - 1 - x;
               dy = sh - 1 - y;
            }
            p->layer->pixels[(size_t)(r.y + dy) * p->layer->width + r.x + dx] = px;
         }
      free(tmp);
   }
}

static void render_art(player *p, gfx_rect full, aspect_mode mode)
{
   gfx_surface s;
   gfx_rect    box, r;
   double      aspect = (double)p->art->width / p->art->height;

   box.w = full.w * 7 / 10;
   box.h = full.h * 7 / 10;
   box.x = full.x + (full.w - box.w) / 2;
   box.y = full.y + (full.h - box.h) / 2;
   r = gfx_fit_aspect(box, mode_aspect(mode, aspect));
   p->video_rect = r;
   p->art_rect   = r;
   s.pixels = p->layer->pixels;
   s.width  = p->layer->width;
   s.height = p->layer->height;
   s.pitch  = p->layer->width;
   gfx_blit_scaled(&s, p->art, r.x, r.y, r.w, r.h, 255);
}

/* Word-wrap and draw one text subtitle, bottom line at 'bottom'.
 * Returns the y coordinate of the top of the block. */
static int draw_text_sub(gfx_surface *fb, const char *text, gfx_rect vr,
      int bottom, int px)
{
   char  lines[16][512];
   int   nlines = 0;
   int   max_w  = vr.w * 9 / 10;
   int   lh     = font_line_height(px);
   int   outline = px / 14 + 1;
   const char *p = text;
   int   i, y;

   /* Split on newlines, then wrap each paragraph at word boundaries. */
   while (*p && nlines < 16)
   {
      char para[512];
      size_t len = strcspn(p, "\n");
      const char *q;
      if (len >= sizeof(para))
         len = sizeof(para) - 1;
      memcpy(para, p, len);
      para[len] = '\0';
      p += strcspn(p, "\n");
      if (*p == '\n')
         p++;

      q = para;
      while (*q && nlines < 16)
      {
         char        line[512];
         size_t      best = 0, n = 0;
         const char *r = q;
         /* Grow the line word by word while it fits. */
         for (;;)
         {
            size_t wl = strcspn(r, " ");
            size_t cand = (size_t)(r - q) + wl;
            if (cand >= sizeof(line))
               break;
            memcpy(line, q, cand);
            line[cand] = '\0';
            if (font_text_width(line, px) > max_w && best > 0)
               break;
            best = cand;
            r   += wl;
            if (!*r)
               break;
            r++; /* the space */
         }
         if (best == 0) /* a single word longer than the line */
            best = strlen(q) < sizeof(line) - 1 ? strlen(q) : sizeof(line) - 1;
         n = best;
         memcpy(lines[nlines], q, n);
         lines[nlines][n] = '\0';
         nlines++;
         q += n;
         while (*q == ' ')
            q++;
      }
   }

   y = bottom - (nlines - 1) * lh;
   for (i = 0; i < nlines; i++)
   {
      int w = font_text_width(lines[i], px);
      font_draw_outlined(fb, lines[i], vr.x + (vr.w - w) / 2, y + i * lh,
            px, 0xFFFFFF, 0x000000, outline);
   }
   return y - font_ascent(px);
}

/* Draw subtitles active at the current clock.  Lock held. */
static void render_subs(player *p, gfx_surface *fb)
{
   sub_event **pp = &p->subs;
   gfx_rect    vr = p->video_rect;
   int         px = vr.h * 55 / 1000 * g_cfg.subtitle_scale / 100;
   int         bottom;

   if (px < 12)
      px = 12;
   bottom = vr.y + vr.h - vr.h / 20 - font_descent(px);
   while (*pp)
   {
      sub_event *e = *pp;
      /* Forget events that are over (only for the serial being shown, so
       * events decoded after a loop wrap survive until their turn). */
      if (e->serial <= p->clock_serial && e->end < p->clock)
      {
         *pp = e->next;
         sub_free(e);
         continue;
      }
      if (e->serial == p->clock_serial && e->start <= p->clock
            && p->clock < e->end)
      {
         if (e->bitmap && e->canvas_w > 0 && e->canvas_h > 0)
         {
            double sx = (double)vr.w / e->canvas_w;
            double sy = (double)vr.h / e->canvas_h;
            gfx_blit_scaled(fb, e->bitmap, vr.x + (int)(e->x * sx),
                  vr.y + (int)(e->y * sy), (int)(e->bitmap->width * sx + 0.5),
                  (int)(e->bitmap->height * sy + 0.5), 255);
         }
         else if (e->text)
            bottom = draw_text_sub(fb, e->text, vr, bottom, px)
               - font_descent(px) - px / 4;
      }
      pp = &e->next;
   }
}

int player_art_rect(player *p, gfx_rect *r)
{
   if (!p->art || p->shown)
      return 0;
   *r = p->art_rect;
   return 1;
}

int player_render(player *p, gfx_surface *fb, aspect_mode mode,
      const gfx_image *background, unsigned bg_serial)
{
   gfx_rect full;
   int      have_visual;
   int      y;

   full.x = 0;
   full.y = 0;
   full.w = fb->width;
   full.h = fb->height;

   slock_lock(p->lock);
   if (p->state == PLAYER_PLAYING)
      update_shown_frame(p);
   slock_unlock(p->lock);

   have_visual = p->shown != NULL || p->art != NULL;

   if (!p->layer || p->layer->width != fb->width
         || p->layer->height != fb->height)
   {
      gfx_image_free(p->layer);
      p->layer      = gfx_image_new(fb->width, fb->height);
      p->layer_mode = -1;
      if (!p->layer)
         return 0;
   }
   if (p->shown_changed || p->layer_mode != (int)mode
         || p->layer_bg != bg_serial)
   {
      if (background && background->width == p->layer->width
            && background->height == p->layer->height)
         memcpy(p->layer->pixels, background->pixels,
               (size_t)p->layer->width * p->layer->height * 4);
      else
         memset(p->layer->pixels, 0, (size_t)p->layer->width
               * p->layer->height * 4);
      p->layer_bg = bg_serial;
      p->video_rect = full;
      if (p->shown)
         render_frame(p, full, mode);
      else if (p->art)
         render_art(p, full, mode);
      p->shown_changed = 0;
      p->layer_mode    = (int)mode;
   }
   for (y = 0; y < fb->height; y++)
      memcpy(fb->pixels + (size_t)y * fb->pitch,
            p->layer->pixels + (size_t)y * p->layer->width,
            (size_t)fb->width * 4);

   /* Subtitles go over the video, or the whole screen for audio. */
   if (!p->shown)
      p->video_rect = full;
   slock_lock(p->lock);
   render_subs(p, fb);
   slock_unlock(p->lock);
   return have_visual;
}

int player_is_picture(player *p)
{
   return p->type == MT_IMAGE;
}

int player_has_video(player *p)
{
   int v;
   slock_lock(p->lock);
   v = p->state == PLAYER_PLAYING && p->video_stream >= 0;
   slock_unlock(p->lock);
   return v;
}

int player_can_seek(player *p)
{
   int s;
   slock_lock(p->lock);
   s = p->state == PLAYER_PLAYING && p->seekable;
   slock_unlock(p->lock);
   return s;
}

int player_is_buffering(player *p)
{
   int b;
   slock_lock(p->lock);
   b = p->state == PLAYER_OPENING || p->req_seek
      || (p->state == PLAYER_PLAYING && p->type != MT_IMAGE && !p->eof
         && ((audio_drives_clock(p) && !p->ac_head)
            || (p->video_stream >= 0 && !p->shown && !p->fq_head)));
   slock_unlock(p->lock);
   return b;
}

double player_position(player *p)
{
   double t;
   slock_lock(p->lock);
   t = p->clock_valid ? p->clock : 0.0;
   if (p->req_seek)
      t = p->req_seek_target;
   slock_unlock(p->lock);
   return t < 0.0 ? 0.0 : t;
}

double player_duration(player *p)
{
   double d;
   slock_lock(p->lock);
   d = p->state == PLAYER_PLAYING ? p->duration : -1.0;
   slock_unlock(p->lock);
   return d;
}

double player_seek_base(player *p)
{
   return player_position(p);
}

void player_seek(player *p, double target)
{
   slock_lock(p->lock);
   if (p->state == PLAYER_PLAYING && p->seekable)
   {
      if (target < 0.0)
         target = 0.0;
      if (p->duration > 0.0 && target > p->duration - 0.5)
         target = p->duration - 0.5;
      p->req_seek        = 1;
      p->req_seek_target = target;
      scond_signal(p->c_demux);
   }
   slock_unlock(p->lock);
}

void player_set_loop(player *p, double a, double b, int jump)
{
   slock_lock(p->lock);
   if (p->state == PLAYER_PLAYING && p->seekable && b > a)
   {
      p->loop_on = 1;
      p->loop_a  = a;
      p->loop_b  = b;
      if (jump)
      {
         /* Data past B is already queued; jump back to A right away. */
         p->req_seek        = 1;
         p->req_seek_target = a;
         scond_signal(p->c_demux);
      }
   }
   slock_unlock(p->lock);
}

void player_clear_loop(player *p)
{
   slock_lock(p->lock);
   if (p->loop_on)
   {
      double here = p->clock_valid ? p->clock : p->loop_a;
      int    trimmed = p->v_reached_b == p->serial
         || p->a_reached_b == p->serial || p->serial != p->clock_serial;
      p->loop_on = 0;
      /* If the decoders already cut the stream at B, or have wrapped
       * back to A ahead of the clock, re-seek to where we are so playback
       * simply continues past B. */
      if (trimmed)
      {
         p->req_seek        = 1;
         p->req_seek_target = here;
         scond_signal(p->c_demux);
      }
   }
   slock_unlock(p->lock);
}

int player_track_count(player *p, int audio)
{
   int n;
   slock_lock(p->lock);
   n = p->state == PLAYER_PLAYING ? (audio ? p->num_audio : p->num_subs) : 0;
   slock_unlock(p->lock);
   return n;
}

int player_track_current(player *p, int audio)
{
   int c;
   slock_lock(p->lock);
   if (audio)
      c = p->req_audio ? p->req_audio_index : p->cur_audio;
   else
      c = p->req_sub ? p->req_sub_index : p->cur_sub;
   slock_unlock(p->lock);
   return c;
}

void player_track_label(player *p, int audio, int index, char *buf, size_t size)
{
   buf[0] = '\0';
   slock_lock(p->lock);
   if (p->state == PLAYER_PLAYING)
   {
      if (audio && index >= 0 && index < p->num_audio)
         strncpy(buf, p->audio_labels[index], size - 1);
      else if (!audio && index >= 0 && index < p->num_subs)
         strncpy(buf, p->sub_labels[index], size - 1);
      buf[size - 1] = '\0';
   }
   slock_unlock(p->lock);
}

void player_select_track(player *p, int audio, int index)
{
   slock_lock(p->lock);
   if (p->state == PLAYER_PLAYING)
   {
      if (audio && index >= 0 && index < p->num_audio)
      {
         p->req_audio       = 1;
         p->req_audio_index = index;
         p->req_audio_pos   = p->clock_valid ? p->clock : 0.0;
      }
      else if (!audio && index >= -1 && index < p->num_subs)
      {
         p->req_sub       = 1;
         p->req_sub_index = index;
      }
      scond_signal(p->c_demux);
   }
   slock_unlock(p->lock);
}
