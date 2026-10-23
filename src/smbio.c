/*
 * smbio.c - see smbio.h.
 *
 * Everything here runs on the player's demux thread, which is the only
 * thread that touches the AVFormatContext (and therefore this I/O).  The
 * synchronous libsmb2 calls are used; smb2_set_timeout() keeps a dead
 * server from blocking forever.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

#include <libavformat/avio.h>
#include <libavutil/mem.h>
#include <libavutil/error.h>

#include "smbio.h"
#include "util.h"

#ifdef HAVE_LIBSMB2
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#endif

#define SMBIO_BUFFER   (256 * 1024)
#define SMBIO_TIMEOUT  15   /* seconds */

int smbio_is_smb(const char *url)
{
   return url && (util_starts_with_nocase(url, "smb://")
         || util_starts_with_nocase(url, "smbs://"));
}

static void set_error(char *error, size_t size, const char *msg,
      const char *detail)
{
   if (!error || size < 2)
      return;
   if (size > 300)
      sprintf(error, "%.100s%s%.180s", msg, detail ? ": " : "",
            detail ? detail : "");
   else
   {
      strncpy(error, msg, size - 1);
      error[size - 1] = '\0';
   }
}

#ifndef HAVE_LIBSMB2

struct AVIOContext *smbio_open(const char *url, char *error, size_t size)
{
   (void)url;
   set_error(error, size, "This build has no SMB support (libsmb2)", NULL);
   return NULL;
}

void smbio_close(struct AVIOContext **ctx)
{
   (void)ctx;
}

#else

typedef struct smbio
{
   struct smb2_context *smb2;
   struct smb2fh       *fh;
   int64_t              size;
   uint32_t             max_read;
} smbio;

static int smbio_read(void *opaque, uint8_t *buf, int len)
{
   smbio *s = (smbio*)opaque;
   int    n;

   if ((uint32_t)len > s->max_read)
      len = (int)s->max_read;
   n = smb2_read(s->smb2, s->fh, buf, (uint32_t)len);
   if (n < 0)
   {
      dlna_log(LOG_WARN, "SMB read failed: %s\n", smb2_get_error(s->smb2));
      return AVERROR(EIO);
   }
   return n == 0 ? AVERROR_EOF : n;
}

static int64_t smbio_seek(void *opaque, int64_t offset, int whence)
{
   smbio   *s = (smbio*)opaque;
   uint64_t current = 0;

   if (whence & AVSEEK_SIZE)
      return s->size >= 0 ? s->size : AVERROR(ENOSYS);
   whence &= ~AVSEEK_FORCE;
   if (smb2_lseek(s->smb2, s->fh, offset, whence, &current) < 0)
      return AVERROR(EIO);
   return (int64_t)current;
}

static void smbio_free(smbio *s)
{
   if (!s)
      return;
   if (s->smb2)
   {
      if (s->fh)
         smb2_close(s->smb2, s->fh);
      smb2_disconnect_share(s->smb2);
      smb2_destroy_context(s->smb2);
   }
   free(s);
}

/* Split "smb[s]://user:password@rest" into a URL without the password
 * (which is what smb2_parse_url understands) and the password itself,
 * percent-decoded so that special characters can be written as %XX. */
static char *strip_password(const char *url, char **password)
{
   const char *auth  = strstr(url, "://") + 3;
   const char *slash = strchr(auth, '/');
   const char *at    = NULL, *p, *colon = NULL;
   char       *out;

   *password = NULL;
   /* The last '@' before the path separates credentials from the host. */
   for (p = auth; *p && (!slash || p < slash); p++)
      if (*p == '@')
         at = p;
   if (at)
      for (p = auth; p < at; p++)
         if (*p == ':')
         {
            colon = p;
            break;
         }
   /* libsmb2 only knows "smb://"; smbs is our own spelling. */
   out = (char*)malloc(strlen(url) + 1);
   if (!out)
      return NULL;
   strcpy(out, "smb://");
   if (colon)
   {
      *password = util_strndup(colon + 1, (size_t)(at - colon - 1));
      if (*password)
         util_percent_decode(*password);
      strncat(out, auth, (size_t)(colon - auth));
      strcat(out, at);
   }
   else
      strcat(out, auth);
   return out;
}

struct AVIOContext *smbio_open(const char *url, char *error, size_t size)
{
   smbio              *s;
   struct smb2_url    *u = NULL;
   struct smb2_stat_64 st;
   char               *clean, *password = NULL;
   unsigned char      *buffer;
   AVIOContext        *avio;
   int                 rc;

   s = (smbio*)calloc(1, sizeof(*s));
   if (!s)
      return NULL;
   s->size = -1;
   s->smb2 = smb2_init_context();
   clean   = strip_password(url, &password);
   if (!s->smb2 || !clean)
   {
      set_error(error, size, "Out of memory", NULL);
      goto fail;
   }
   smb2_set_timeout(s->smb2, SMBIO_TIMEOUT);
   smb2_set_security_mode(s->smb2, SMB2_NEGOTIATE_SIGNING_ENABLED);
   if (util_starts_with_nocase(url, "smbs://"))
      smb2_set_seal(s->smb2, 1); /* require SMB3 encryption */
   if (password)
      smb2_set_password(s->smb2, password);

   u = smb2_parse_url(s->smb2, clean);
   if (!u || !u->server || !u->share || !u->path)
   {
      set_error(error, size, "Bad SMB URL (smb://server/share/path)",
            u ? NULL : smb2_get_error(s->smb2));
      goto fail;
   }
   if (u->domain)
      smb2_set_domain(s->smb2, u->domain);
   rc = smb2_connect_share(s->smb2, u->server, u->share, u->user);
   if (rc < 0)
   {
      set_error(error, size, "SMB connection failed", smb2_get_error(s->smb2));
      goto fail;
   }
   s->fh = smb2_open(s->smb2, u->path, O_RDONLY);
   if (!s->fh)
   {
      set_error(error, size, "Cannot open SMB file", smb2_get_error(s->smb2));
      goto fail;
   }
   if (smb2_fstat(s->smb2, s->fh, &st) == 0)
      s->size = (int64_t)st.smb2_size;
   s->max_read = smb2_get_max_read_size(s->smb2);
   if (s->max_read == 0 || s->max_read > SMBIO_BUFFER)
      s->max_read = SMBIO_BUFFER;

   buffer = (unsigned char*)av_malloc(SMBIO_BUFFER);
   if (!buffer)
      goto fail;
   avio = avio_alloc_context(buffer, SMBIO_BUFFER, 0, s, smbio_read, NULL,
         smbio_seek);
   if (!avio)
   {
      av_free(buffer);
      goto fail;
   }
   avio->seekable = AVIO_SEEKABLE_NORMAL;
   smb2_destroy_url(u);
   free(clean);
   free(password);
   return avio;

fail:
   if (u)
      smb2_destroy_url(u);
   free(clean);
   free(password);
   smbio_free(s);
   return NULL;
}

void smbio_close(struct AVIOContext **ctx)
{
   if (!ctx || !*ctx)
      return;
   smbio_free((smbio*)(*ctx)->opaque);
   av_freep(&(*ctx)->buffer);
   avio_context_free(ctx);
}

#endif
