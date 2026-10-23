/*
 * util.c - see util.h.
 */

/* vsnprintf() is C99 / POSIX.1-2001, not C90.  This asks the C library to
 * declare it even when compiling in strict C89 mode.  This file is the only
 * place in the core that needs it; see docs/C90_NOTES.md. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200112L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>

#if defined(_MSC_VER)
/* Old Visual C++ versions call it _vsnprintf; libretro-common knows. */
#include <compat/msvc.h>
#endif

#include "util.h"

static util_log_fn g_logger = NULL;

void util_set_logger(util_log_fn fn)
{
   g_logger = fn;
}

void dlna_log(int level, const char *fmt, ...)
{
   char    buf[1024];
   va_list ap;

   strcpy(buf, "[DLNA] ");
   va_start(ap, fmt);
   /* vsnprintf (C99) rather than vsprintf (C90) so a long message can
    * never overflow the buffer; see the note at the top of this file. */
   vsnprintf(buf + 7, sizeof(buf) - 7, fmt, ap);
   va_end(ap);

   if (g_logger)
      g_logger(level, buf);
   else
      fputs(buf, stderr);
}

char *util_strdup(const char *s)
{
   size_t len;
   char  *out;

   if (!s)
      return NULL;
   len = strlen(s);
   out = (char*)malloc(len + 1);
   if (out)
      memcpy(out, s, len + 1);
   return out;
}

char *util_strndup(const char *s, size_t n)
{
   size_t len = 0;
   char  *out;

   if (!s)
      return NULL;
   while (len < n && s[len])
      len++;
   out = (char*)malloc(len + 1);
   if (!out)
      return NULL;
   memcpy(out, s, len);
   out[len] = '\0';
   return out;
}

void *util_calloc(size_t count, size_t size)
{
   return calloc(count ? count : 1, size ? size : 1);
}

void util_replace(char **dst, const char *src)
{
   char *copy = util_strdup(src);
   free(*dst);
   *dst = copy;
}

int util_strcasecmp(const char *a, const char *b)
{
   for (;;)
   {
      int ca = tolower((unsigned char)*a);
      int cb = tolower((unsigned char)*b);
      if (ca != cb)
         return ca - cb;
      if (!ca)
         return 0;
      a++;
      b++;
   }
}

int util_strncasecmp(const char *a, const char *b, size_t n)
{
   while (n--)
   {
      int ca = tolower((unsigned char)*a);
      int cb = tolower((unsigned char)*b);
      if (ca != cb)
         return ca - cb;
      if (!ca)
         return 0;
      a++;
      b++;
   }
   return 0;
}

const char *util_strcasestr(const char *haystack, const char *needle)
{
   size_t n = strlen(needle);

   if (!n)
      return haystack;
   for (; *haystack; haystack++)
      if (util_strncasecmp(haystack, needle, n) == 0)
         return haystack;
   return NULL;
}

int util_starts_with_nocase(const char *s, const char *prefix)
{
   return util_strncasecmp(s, prefix, strlen(prefix)) == 0;
}

int util_natural_compare(const char *a, const char *b)
{
   while (*a && *b)
   {
      if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b))
      {
         const char *sa, *sb;
         size_t      la, lb;

         /* Skip leading zeros, then the longer run of digits is bigger;
          * equal lengths compare digit by digit. */
         while (*a == '0')
            a++;
         while (*b == '0')
            b++;
         sa = a;
         sb = b;
         while (isdigit((unsigned char)*a))
            a++;
         while (isdigit((unsigned char)*b))
            b++;
         la = (size_t)(a - sa);
         lb = (size_t)(b - sb);
         if (la != lb)
            return la < lb ? -1 : 1;
         while (sa < a)
         {
            if (*sa != *sb)
               return *sa < *sb ? -1 : 1;
            sa++;
            sb++;
         }
      }
      else
      {
         int ca = tolower((unsigned char)*a);
         int cb = tolower((unsigned char)*b);
         if (ca != cb)
            return ca < cb ? -1 : 1;
         a++;
         b++;
      }
   }
   if (*a)
      return 1;
   if (*b)
      return -1;
   return 0;
}

void util_trim(char *s)
{
   size_t len;
   size_t start = 0;

   if (!s)
      return;
   len = strlen(s);
   while (len && isspace((unsigned char)s[len - 1]))
      s[--len] = '\0';
   while (s[start] && isspace((unsigned char)s[start]))
      start++;
   if (start)
      memmove(s, s + start, len - start + 1);
}

static int hex_value(int c)
{
   if (c >= '0' && c <= '9')
      return c - '0';
   if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
   if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
   return -1;
}

void util_percent_decode(char *s)
{
   char *out = s;

   while (*s)
   {
      if (s[0] == '%' && hex_value(s[1]) >= 0 && hex_value(s[2]) >= 0)
      {
         *out++ = (char)(hex_value(s[1]) * 16 + hex_value(s[2]));
         s     += 3;
      }
      else
         *out++ = *s++;
   }
   *out = '\0';
}

char *util_uri_without_password(const char *uri)
{
   const char *auth, *end, *at = NULL, *colon = NULL, *p;
   char       *out;

   if (!uri)
      return NULL;
   auth = strstr(uri, "://");
   if (!auth)
      return util_strdup(uri);
   auth += 3;
   end   = auth + strcspn(auth, "/");
   for (p = auth; p < end; p++)
      if (*p == '@')
         at = p;  /* the last '@' ends the credentials */
   if (!at)
      return util_strdup(uri);
   for (p = auth; p < at; p++)
      if (*p == ':')
      {
         colon = p;
         break;
      }
   if (!colon)
      return util_strdup(uri);
   out = (char*)malloc(strlen(uri) + 1);
   if (!out)
      return NULL;
   memcpy(out, uri, (size_t)(colon - uri));
   strcpy(out + (colon - uri), at);
   return out;
}

static char g_ca_file[1024];

void util_find_ca_file(const char *system_dir)
{
   static const char *const bundles[] = {
      "/etc/ssl/certs/ca-certificates.crt",   /* Debian, Ubuntu, Arch */
      "/etc/pki/tls/certs/ca-bundle.crt",     /* Fedora, RHEL         */
      "/etc/ssl/ca-bundle.pem",               /* openSUSE             */
      "/etc/ssl/cert.pem",                    /* macOS, Alpine, BSD   */
      "/system/etc/security/cacerts.pem",
      NULL
   };
   char dir[1024];
   int  i;

   util_path_join(dir, sizeof(dir), system_dir, "dlna");
   util_path_join(g_ca_file, sizeof(g_ca_file), dir, "ca-certificates.crt");
   if (util_file_exists(g_ca_file))
      return;
   for (i = 0; bundles[i]; i++)
      if (util_file_exists(bundles[i]))
      {
         strncpy(g_ca_file, bundles[i], sizeof(g_ca_file) - 1);
         g_ca_file[sizeof(g_ca_file) - 1] = '\0';
         return;
      }
   g_ca_file[0] = '\0';
}

const char *util_ca_file(void)
{
   return g_ca_file[0] ? g_ca_file : NULL;
}

void util_uri_escape(const char *in, char *out, size_t size)
{
   static const char hex[] = "0123456789ABCDEF";
   size_t            n     = 0;

   if (!size)
      return;
   for (; in && *in && n + 4 < size; in++)
   {
      unsigned char c = (unsigned char)*in;
      if (c == '%' || c == '/' || c == '?' || c == '#')
      {
         out[n++] = '%';
         out[n++] = hex[c >> 4];
         out[n++] = hex[c & 15];
      }
      else
         out[n++] = (char)c;
   }
   out[n] = '\0';
}

char *util_extension(const char *path, char *buf, size_t size)
{
   const char *end;
   const char *p;
   const char *dot = NULL;
   size_t      len;

   buf[0] = '\0';
   if (!path || size < 2)
      return buf;

   /* Stop at a query string or fragment. */
   end = path + strcspn(path, "?#");
   for (p = path; p < end; p++)
   {
      if (*p == '.')
         dot = p;
      else if (*p == '/' || *p == '\\')
         dot = NULL;
   }
   if (!dot)
      return buf;
   dot++;
   len = (size_t)(end - dot);
   if (len == 0 || len >= size || len > 8)
      return buf;
   for (p = dot; p < end; p++)
      buf[p - dot] = (char)tolower((unsigned char)*p);
   buf[len] = '\0';
   return buf;
}

/* Common extension lists.  Extend these freely; they are only used for
 * guessing when a server does not tell us what a file is, and for picking
 * which of the video/audio/picture trees to search for a dlna:// path. */
static const char *const video_exts[] = {
   "mp4", "m4v", "mkv", "webm", "avi", "mov", "qt", "wmv", "asf", "flv",
   "f4v", "mpg", "mpeg", "mpe", "m1v", "m2v", "ts", "m2ts", "mts", "tp",
   "trp", "vob", "ogv", "3gp", "3g2", "divx", "xvid", "rm", "rmvb", "dv",
   "mxf", "tod", "vro", "evo", "nut", NULL
};
static const char *const audio_exts[] = {
   "mp3", "m4a", "m4b", "aac", "flac", "ogg", "oga", "opus", "wav", "wma",
   "ac3", "eac3", "dts", "aif", "aiff", "aifc", "ape", "mka", "mpc", "wv",
   "alac", "amr", "au", "snd", "pcm", "dsf", "dff", "tta", "mp2", "mpa",
   "spx", "caf", "weba", NULL
};
static const char *const image_exts[] = {
   "jpg", "jpeg", "jpe", "jfif", "png", "gif", "bmp", "dib", "tif", "tiff",
   "webp", "heic", "heif", "avif", "jxl", "tga", "ppm", "pgm", "pbm", "pnm",
   "pcx", "ico", "jp2", "j2k", "sgi", "exr", "qoi", NULL
};

static int in_list(const char *const *list, const char *ext)
{
   for (; *list; list++)
      if (strcmp(*list, ext) == 0)
         return 1;
   return 0;
}

media_type util_media_type_from_extension(const char *ext)
{
   if (!ext || !*ext)
      return MT_NONE;
   if (in_list(video_exts, ext))
      return MT_VIDEO;
   if (in_list(audio_exts, ext))
      return MT_AUDIO;
   if (in_list(image_exts, ext))
      return MT_IMAGE;
   return MT_NONE;
}

media_type util_media_type_from_mime(const char *mime)
{
   if (!mime)
      return MT_NONE;
   if (util_starts_with_nocase(mime, "video/"))
      return MT_VIDEO;
   if (util_starts_with_nocase(mime, "audio/"))
      return MT_AUDIO;
   if (util_starts_with_nocase(mime, "image/"))
      return MT_IMAGE;
   /* A few servers use application/ types for containers. */
   if (util_strcasestr(mime, "x-mpegurl") || util_strcasestr(mime, "mp2t")
         || util_strcasestr(mime, "x-matroska"))
      return MT_VIDEO;
   if (util_strcasestr(mime, "ogg"))
      return MT_AUDIO;
   return MT_NONE;
}

int util_utf8_len(unsigned char c)
{
   if (c < 0x80)
      return 1;
   if ((c & 0xE0) == 0xC0)
      return 2;
   if ((c & 0xF0) == 0xE0)
      return 3;
   if ((c & 0xF8) == 0xF0)
      return 4;
   return 1;
}

long util_utf8_next(const char **s)
{
   const unsigned char *p = (const unsigned char*)*s;
   long                 cp;
   int                  len;
   int                  i;

   if (!*p)
      return -1;
   len = util_utf8_len(*p);
   if (len == 1)
   {
      *s += 1;
      return (*p < 0x80) ? (long)*p : 0xFFFDL;
   }
   if (len == 2)
      cp = *p & 0x1F;
   else if (len == 3)
      cp = *p & 0x0F;
   else
      cp = *p & 0x07;
   for (i = 1; i < len; i++)
   {
      if ((p[i] & 0xC0) != 0x80)
      {
         *s += 1;
         return 0xFFFDL;
      }
      cp = (cp << 6) | (p[i] & 0x3F);
   }
   *s += len;
   return cp;
}

void util_format_time(double seconds, char *buf, size_t size)
{
   long total, h, m, s;

   /* (seconds != seconds) is the C90 way of testing for NaN. */
   if (seconds != seconds || seconds < 0.0 || seconds > 360000000.0)
   {
      sprintf(buf, "%.*s", (int)(size - 1), "--:--");
      return;
   }
   total = (long)seconds;
   h     = total / 3600;
   m     = (total / 60) % 60;
   s     = total % 60;
   /* The longest output is "100000:59:59" which fits in 16 bytes. */
   if (size < 16)
   {
      buf[0] = '\0';
      return;
   }
   if (h > 0)
      sprintf(buf, "%ld:%02ld:%02ld", h, m, s);
   else
      sprintf(buf, "%ld:%02ld", m, s);
}

double util_parse_duration(const char *s)
{
   double parts[3];
   int    n = 0;
   double frac = 0.0;

   if (!s || !*s)
      return -1.0;
   parts[0] = parts[1] = parts[2] = 0.0;
   while (*s && n < 3)
   {
      double v = 0.0;
      if (!isdigit((unsigned char)*s))
         return -1.0;
      while (isdigit((unsigned char)*s))
         v = v * 10.0 + (*s++ - '0');
      parts[n++] = v;
      if (*s == ':')
         s++;
      else
         break;
   }
   if (*s == '.')
   {
      double scale = 0.1;
      s++;
      while (isdigit((unsigned char)*s))
      {
         frac  += (*s++ - '0') * scale;
         scale *= 0.1;
      }
   }
   if (n == 3)
      return parts[0] * 3600.0 + parts[1] * 60.0 + parts[2] + frac;
   if (n == 2)
      return parts[0] * 60.0 + parts[1] + frac;
   if (n == 1)
      return parts[0] + frac;
   return -1.0;
}

int util_parse_resolution(const char *s, int *w, int *h)
{
   char *end;
   long  a, b;

   if (!s)
      return 0;
   a = strtol(s, &end, 10);
   if (end == s || (*end != 'x' && *end != 'X'))
      return 0;
   s = end + 1;
   b = strtol(s, &end, 10);
   if (end == s || a <= 0 || b <= 0)
      return 0;
   *w = (int)a;
   *h = (int)b;
   return 1;
}

int util_file_exists(const char *path)
{
   FILE *f;

   if (!path || !*path)
      return 0;
   f = fopen(path, "rb");
   if (!f)
      return 0;
   fclose(f);
   return 1;
}

void util_path_join(char *out, size_t size, const char *dir, const char *name)
{
   size_t len;
#ifdef _WIN32
   const char sep = '\\';
#else
   const char sep = '/';
#endif

   if (!size)
      return;
   out[0] = '\0';
   if (dir && *dir)
   {
      len = strlen(dir);
      if (len >= size)
         return;
      memcpy(out, dir, len + 1);
      if (len && dir[len - 1] != '/' && dir[len - 1] != '\\' && len + 1 < size)
      {
         out[len++] = sep;
         out[len]   = '\0';
      }
   }
   len = strlen(out);
   if (name)
   {
      size_t nlen = strlen(name);
      if (len + nlen >= size)
         nlen = size - len - 1;
      memcpy(out + len, name, nlen);
      out[len + nlen] = '\0';
   }
}

char *util_read_file(const char *path, size_t *len)
{
   FILE  *f;
   char  *buf;
   long   size;
   size_t got;

   f = fopen(path, "rb");
   if (!f)
      return NULL;
   if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0
         || fseek(f, 0, SEEK_SET) != 0)
   {
      fclose(f);
      return NULL;
   }
   buf = (char*)malloc((size_t)size + 1);
   if (!buf)
   {
      fclose(f);
      return NULL;
   }
   got = fread(buf, 1, (size_t)size, f);
   fclose(f);
   buf[got] = '\0';
   if (len)
      *len = got;
   return buf;
}

int ptr_array_push(ptr_array *a, void *item)
{
   if (a->count == a->capacity)
   {
      size_t cap   = a->capacity ? a->capacity * 2 : 16;
      void **items = (void**)realloc(a->items, cap * sizeof(void*));
      if (!items)
         return 0;
      a->items    = items;
      a->capacity = cap;
   }
   a->items[a->count++] = item;
   return 1;
}

void ptr_array_clear(ptr_array *a)
{
   free(a->items);
   a->items    = NULL;
   a->count    = 0;
   a->capacity = 0;
}
