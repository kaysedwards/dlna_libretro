/*
 * player.h - media playback with FFmpeg.
 *
 * Architecture (see docs/ARCHITECTURE.md for the long version):
 *
 *   demux thread   reads packets from the server, decodes subtitles and
 *                  queues audio/video packets; performs seeks and A-B loops
 *   video thread   decodes video packets into a small queue of frames
 *   audio thread   decodes + resamples audio into 48 kHz stereo chunks
 *   main thread    (retro_run) pulls exactly one video frame's worth of
 *                  audio per frame; the audio timestamps drive the clock
 *                  and the clock decides which video frame is shown
 *
 * Every seek bumps a "serial" number that travels with packets, frames and
 * audio chunks, so data from before and after a seek can be told apart.
 * A user seek also bumps the "flush serial": anything older is discarded.
 * An A-B loop wrap does not, so the audio before B plays out seamlessly
 * while the decoders are already working on the data from A.
 */
#ifndef DLNA_PLAYER_H
#define DLNA_PLAYER_H

#include <stddef.h>
#include "gfx.h"
#include "options.h"
#include "util.h"

#define PLAYER_SAMPLE_RATE 48000

typedef struct player player;

typedef enum player_state
{
   PLAYER_OPENING = 0,
   PLAYER_PLAYING,
   PLAYER_ENDED,
   PLAYER_ERROR
} player_state;

/* Start playing 'url'.  'art_url' (optional) is shown for audio files
 * without embedded cover art.  'known_duration' is the length the server
 * reported (<= 0 if unknown); it is preferred over FFmpeg's guess when
 * FFmpeg can only estimate the length from the bitrate. */
player *player_open(const char *url, media_type type, const char *art_url,
      subs_default subs, double known_duration);
void    player_close(player *p);

player_state player_get_state(player *p, char *error, size_t size);

/* Fill 'out' with 'frames' stereo frames of audio and advance the clock.
 * Call once per retro_run.  When paused the output is silence and the
 * clock stands still. */
void player_audio(player *p, int16_t *out, size_t frames, int paused);

/* Draw the current video frame (or picture / cover art) and subtitles
 * into the framebuffer, over 'background' (a canvas-sized image, or NULL
 * for black).  Change 'bg_serial' whenever the background changes so the
 * cached picture is rebuilt.  Returns 0 when there is nothing to show (an
 * audio file without art), in which case the caller draws a placeholder. */
int player_render(player *p, gfx_surface *fb, aspect_mode mode,
      const gfx_image *background, unsigned bg_serial);

/* Where the cover art of an audio file is drawn (for the visualization).
 * Returns 0 if there is no art. */
int player_art_rect(player *p, gfx_rect *r);

int    player_is_picture(player *p);
int    player_has_video(player *p);
int    player_can_seek(player *p);
int    player_is_buffering(player *p);
double player_position(player *p);   /* seconds */
double player_duration(player *p);   /* seconds, <= 0 when unknown */

/* Seeking.  player_seek_base() is the position the next relative seek
 * should start from (the pending target if a seek is in flight). */
double player_seek_base(player *p);
void   player_seek(player *p, double target);

/* A-B looping.  With 'jump' set, playback jumps straight back to A (what
 * the user expects after pressing B); without, it carries on and wraps
 * when it reaches B (used when restoring a save state). */
void player_set_loop(player *p, double a, double b, int jump);
void player_clear_loop(player *p);

/* Tracks.  'audio' selects audio tracks (1) or subtitle tracks (0).
 * Subtitle index -1 means "off". */
int  player_track_count(player *p, int audio);
int  player_track_current(player *p, int audio);
void player_track_label(player *p, int audio, int index, char *buf, size_t size);
void player_select_track(player *p, int audio, int index);

#endif
