/*
 * sshio.c - see sshio.h.  Runs on the player's demux thread only.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

#include <libavformat/avio.h>
#include <libavutil/mem.h>
#include <libavutil/error.h>

#include "sshio.h"
#include "util.h"

#ifdef HAVE_LIBSSH
#include <libssh/libssh.h>
#include <libssh/sftp.h>
#endif

#define SSHIO_BUFFER  (128 * 1024)
#define SSHIO_TIMEOUT 15L   /* seconds */

int sshio_is_sftp(const char *url)
{
   return url && util_starts_with_nocase(url, "sftp://");
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

#ifndef HAVE_LIBSSH

void sshio_global_init(const char *system_dir)
{
   (void)system_dir;
}

void sshio_global_deinit(void)
{
}

struct AVIOContext *sshio_open(const char *url, char *error, size_t size)
{
   (void)url;
   set_error(error, size, "This build has no SFTP support (libssh)", NULL);
   return NULL;
}

void sshio_close(struct AVIOContext **ctx)
{
   (void)ctx;
}

#else

typedef struct sshio
{
   ssh_session  ssh;
   sftp_session sftp;
   sftp_file    file;
   int64_t      size;
} sshio;

static char g_known_hosts[1024];

void sshio_global_init(const char *system_dir)
{
   char dir[1024];
   ssh_init();
   g_known_hosts[0] = '\0';
   util_path_join(dir, sizeof(dir), system_dir, "dlna");
   util_path_join(g_known_hosts, sizeof(g_known_hosts), dir, "known_hosts");
   if (!util_file_exists(g_known_hosts))
      g_known_hosts[0] = '\0';
   else
      dlna_log(LOG_INFO, "SFTP host keys from %s\n", g_known_hosts);
}

void sshio_global_deinit(void)
{
   ssh_finalize();
}

static int sshio_read(void *opaque, uint8_t *buf, int len)
{
   sshio *s = (sshio*)opaque;
   /* ssize_t is POSIX; libssh uses it for read results. */
   long   n = (long)sftp_read(s->file, buf, (size_t)len);
   if (n < 0)
   {
      dlna_log(LOG_WARN, "SFTP read failed: %s\n", ssh_get_error(s->ssh));
      return AVERROR(EIO);
   }
   return n == 0 ? AVERROR_EOF : (int)n;
}

static int64_t sshio_seek(void *opaque, int64_t offset, int whence)
{
   sshio  *s = (sshio*)opaque;
   int64_t target;

   if (whence & AVSEEK_SIZE)
      return s->size >= 0 ? s->size : AVERROR(ENOSYS);
   whence &= ~AVSEEK_FORCE;
   if (whence == SEEK_SET)
      target = offset;
   else if (whence == SEEK_CUR)
      target = (int64_t)sftp_tell64(s->file) + offset;
   else if (whence == SEEK_END && s->size >= 0)
      target = s->size + offset;
   else
      return AVERROR(EINVAL);
   if (target < 0 || sftp_seek64(s->file, (uint64_t)target) < 0)
      return AVERROR(EIO);
   return target;
}

static void sshio_free(sshio *s)
{
   if (!s)
      return;
   if (s->file)
      sftp_close(s->file);
   if (s->sftp)
      sftp_free(s->sftp);
   if (s->ssh)
   {
      ssh_disconnect(s->ssh);
      ssh_free(s->ssh);
   }
   free(s);
}

/* Split sftp://[user[:pass]@]host[:port]/path into its parts (all newly
 * allocated; user, pass and port may be NULL). */
static int split_url(const char *url, char **user, char **pass, char **host,
      int *port, char **path)
{
   const char *auth  = url + 7; /* after "sftp://" */
   const char *slash = strchr(auth, '/');
   const char *at = NULL, *p, *hp, *colon;

   *user = *pass = *host = *path = NULL;
   *port = 0;
   if (!slash)
      return 0;
   for (p = auth; p < slash; p++)
      if (*p == '@')
         at = p;
   hp = auth;
   if (at)
   {
      const char *c = memchr(auth, ':', (size_t)(at - auth));
      if (c)
      {
         *user = util_strndup(auth, (size_t)(c - auth));
         *pass = util_strndup(c + 1, (size_t)(at - c - 1));
      }
      else
         *user = util_strndup(auth, (size_t)(at - auth));
      hp = at + 1;
   }
   colon = memchr(hp, ':', (size_t)(slash - hp));
   if (colon && hp[0] != '[')
   {
      *host = util_strndup(hp, (size_t)(colon - hp));
      *port = atoi(colon + 1);
   }
   else
      *host = util_strndup(hp, (size_t)(slash - hp));
   *path = util_strdup(slash);
   if (*user)
      util_percent_decode(*user);
   if (*pass)
      util_percent_decode(*pass);
   if (*path)
      util_percent_decode(*path);
   return *host && **host && *path;
}

/* Refuse servers whose key is not already in known_hosts. */
static int verify_host(sshio *s, char *error, size_t size)
{
   switch (ssh_session_is_known_server(s->ssh))
   {
      case SSH_KNOWN_HOSTS_OK:
         return 1;
      case SSH_KNOWN_HOSTS_CHANGED:
      case SSH_KNOWN_HOSTS_OTHER:
         set_error(error, size, "SFTP server's host key has CHANGED; refusing "
               "to connect (possible attack)", NULL);
         return 0;
      case SSH_KNOWN_HOSTS_UNKNOWN:
      case SSH_KNOWN_HOSTS_NOT_FOUND:
         set_error(error, size, "SFTP server is not in ~/.ssh/known_hosts; "
               "connect to it once with ssh or sftp first", NULL);
         return 0;
      default:
         break;
   }
   set_error(error, size, "Could not check the SFTP host key",
         ssh_get_error(s->ssh));
   return 0;
}

struct AVIOContext *sshio_open(const char *url, char *error, size_t size)
{
   sshio         *s;
   char          *user, *pass, *host, *path;
   int            port, auth;
   long           timeout = SSHIO_TIMEOUT;
   sftp_attributes attr;
   unsigned char *buffer;
   AVIOContext   *avio;

   s = (sshio*)calloc(1, sizeof(*s));
   if (!s)
      return NULL;
   s->size = -1;
   if (!split_url(url, &user, &pass, &host, &port, &path))
   {
      set_error(error, size, "Bad SFTP URL (sftp://host/path)", NULL);
      goto fail;
   }

   s->ssh = ssh_new();
   if (!s->ssh)
      goto fail;
   ssh_options_set(s->ssh, SSH_OPTIONS_HOST, host);
   if (port > 0)
      ssh_options_set(s->ssh, SSH_OPTIONS_PORT, &port);
   if (user && *user)
      ssh_options_set(s->ssh, SSH_OPTIONS_USER, user);
   ssh_options_set(s->ssh, SSH_OPTIONS_TIMEOUT, &timeout);
   /* Read ~/.ssh/config for host aliases, ports and keys like ssh does. */
   ssh_options_parse_config(s->ssh, NULL);
   if (g_known_hosts[0])
      ssh_options_set(s->ssh, SSH_OPTIONS_KNOWNHOSTS, g_known_hosts);

   if (ssh_connect(s->ssh) != SSH_OK)
   {
      set_error(error, size, "SFTP connection failed", ssh_get_error(s->ssh));
      goto fail;
   }
   if (!verify_host(s, error, size))
      goto fail;

   /* Keys (agent, ~/.ssh/id_*) first, then the password. */
   auth = ssh_userauth_publickey_auto(s->ssh, NULL, NULL);
   if (auth != SSH_AUTH_SUCCESS && pass)
      auth = ssh_userauth_password(s->ssh, NULL, pass);
   if (auth != SSH_AUTH_SUCCESS)
   {
      set_error(error, size, "SFTP login refused", ssh_get_error(s->ssh));
      goto fail;
   }

   s->sftp = sftp_new(s->ssh);
   if (!s->sftp || sftp_init(s->sftp) != SSH_OK)
   {
      set_error(error, size, "SFTP is not available on the server",
            ssh_get_error(s->ssh));
      goto fail;
   }
   s->file = sftp_open(s->sftp, path, O_RDONLY, 0);
   if (!s->file)
   {
      set_error(error, size, "Cannot open SFTP file", ssh_get_error(s->ssh));
      goto fail;
   }
   attr = sftp_fstat(s->file);
   if (attr)
   {
      s->size = (int64_t)attr->size;
      sftp_attributes_free(attr);
   }

   buffer = (unsigned char*)av_malloc(SSHIO_BUFFER);
   if (!buffer)
      goto fail;
   avio = avio_alloc_context(buffer, SSHIO_BUFFER, 0, s, sshio_read, NULL,
         sshio_seek);
   if (!avio)
   {
      av_free(buffer);
      goto fail;
   }
   avio->seekable = AVIO_SEEKABLE_NORMAL;
   free(user);
   free(pass);
   free(host);
   free(path);
   return avio;

fail:
   free(user);
   free(pass);
   free(host);
   free(path);
   sshio_free(s);
   return NULL;
}

void sshio_close(struct AVIOContext **ctx)
{
   if (!ctx || !*ctx)
      return;
   sshio_free((sshio*)(*ctx)->opaque);
   av_freep(&(*ctx)->buffer);
   avio_context_free(ctx);
}

#endif
