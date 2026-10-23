/*
 * playlist.c - see playlist.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "playlist.h"
#include "json.h"
#include "util.h"

static int push(playlist *pl, char *uri)
{
   char **items;
   if (!uri)
      return 0;
   items = (char**)realloc(pl->items, (pl->count + 1) * sizeof(char*));
   if (!items)
   {
      free(uri);
      return 0;
   }
   pl->items = items;
   pl->items[pl->count++] = uri;
   return 1;
}

void playlist_free(playlist *pl)
{
   size_t i;
   for (i = 0; i < pl->count; i++)
      free(pl->items[i]);
   free(pl->items);
   pl->items = NULL;
   pl->count = 0;
}

int playlist_is_file(const char *path)
{
   char ext[16];
   util_extension(path, ext, sizeof(ext));
   return strcmp(ext, "dlna") == 0 || strcmp(ext, "m3u") == 0
      || strcmp(ext, "m3u8") == 0 || strcmp(ext, "json") == 0;
}

static void set_error(char *error, size_t size, const char *msg,
      const char *arg)
{
   if (!error || size < 2)
      return;
   /* All messages are short literals; clamp the argument. */
   if (size > 300)
      sprintf(error, msg, arg ? arg : "");
   else
   {
      strncpy(error, msg, size - 1);
      error[size - 1] = '\0';
   }
}

/* Is it an absolute path on this system? */
static int is_absolute(const char *p)
{
   if (p[0] == '/' || p[0] == '\\')
      return 1;
   /* Windows drive letter: C:\ or C:/ */
   return isalpha((unsigned char)p[0]) && p[1] == ':'
      && (p[2] == '\\' || p[2] == '/');
}

/* Does the string start with "scheme://"? */
static int has_scheme(const char *s)
{
   const char *colon = strstr(s, "://");
   const char *c;
   if (!colon || colon == s)
      return 0;
   for (c = s; c < colon; c++)
      if (!isalnum((unsigned char)*c) && *c != '+' && *c != '-' && *c != '.')
         return 0;
   return 1;
}

/* Percent-encode a path for an http(s) URL, keeping '/' and characters
 * that need no encoding.  A path that already contains %XX escapes is
 * assumed to be encoded and is kept as it is. */
static char *http_path(const char *path)
{
   static const char hex[] = "0123456789ABCDEF";
   const char       *p;
   char             *out, *o;

   for (p = path; *p; p++)
      if (p[0] == '%' && isxdigit((unsigned char)p[1])
            && isxdigit((unsigned char)p[2]))
         return util_strdup(path);
   out = (char*)malloc(strlen(path) * 3 + 1);
   if (!out)
      return NULL;
   o = out;
   for (p = path; *p; p++)
   {
      unsigned char c = (unsigned char)*p;
      if (isalnum(c) || strchr("-._~/!$&'()*+,;=:@", c))
         *o++ = (char)c;
      else
      {
         *o++ = '%';
         *o++ = hex[c >> 4];
         *o++ = hex[c & 15];
      }
   }
   *o = '\0';
   return out;
}

/* Escape each '/'-separated component of a path for a dlna:// URI. */
static char *dlna_path(const char *path)
{
   char  *out = (char*)malloc(strlen(path) * 3 + 2);
   char   comp[1024], esc[3100];
   size_t n = 0;

   if (!out)
      return NULL;
   out[0] = '\0';
   while (*path)
   {
      size_t len = strcspn(path, "/");
      if (len && len < sizeof(comp))
      {
         memcpy(comp, path, len);
         comp[len] = '\0';
         util_uri_escape(comp, esc, sizeof(esc));
         if (n)
            out[n++] = '/';
         strcpy(out + n, esc);
         n += strlen(esc);
      }
      path += len;
      if (*path == '/')
         path++;
   }
   out[n] = '\0';
   return out;
}

char *playlist_make_uri(const char *scheme, const char *host, const char *port,
      const char *user, const char *password, const char *path,
      const char *base_dir, char *error, size_t size)
{
   char   sch[16];
   char  *encoded = NULL;
   char  *out;
   size_t len, i;

   if (!scheme || !*scheme)
      scheme = "dlna";
   if (!host)
      host = "";
   if (!path)
      path = "";
   for (i = 0; scheme[i] && i < sizeof(sch) - 1; i++)
      sch[i] = (char)tolower((unsigned char)scheme[i]);
   sch[i] = '\0';
   while (*path == ' ')
      path++;

   if (strcmp(sch, "file") == 0)
   {
      char joined[2048];
      if (*host && util_strcasecmp(host, "localhost") != 0
            && strcmp(host, "127.0.0.1") != 0)
      {
         set_error(error, size, "A file: playlist must use host localhost, "
               "127.0.0.1 or nothing, not \"%.100s\"", host);
         return NULL;
      }
      if (is_absolute(path) || !base_dir || !*base_dir)
         return util_strdup(path);
      util_path_join(joined, sizeof(joined), base_dir, path);
      return util_strdup(joined);
   }

   if (strcmp(sch, "dlna") != 0 && strcmp(sch, "http") != 0
         && strcmp(sch, "https") != 0 && strcmp(sch, "sftp") != 0
         && strcmp(sch, "smb") != 0 && strcmp(sch, "smbs") != 0)
   {
      set_error(error, size, "Unsupported scheme \"%.40s\" (use dlna, file, "
            "http, https, sftp, smb or smbs)", sch);
      return NULL;
   }
   if (!*host)
   {
      set_error(error, size, "No host given for %.40s", sch);
      return NULL;
   }

   while (*path == '/')
      path++;
   if (strcmp(sch, "dlna") == 0)
      encoded = dlna_path(path);
   else if (strcmp(sch, "http") == 0 || strcmp(sch, "https") == 0)
      encoded = http_path(path);
   else
      encoded = util_strdup(path); /* sftp, smb: used as given */
   if (!encoded)
      return NULL;

   len = strlen(sch) + strlen(host) + strlen(encoded) + 16
      + (port ? strlen(port) : 0) + (user ? strlen(user) : 0)
      + (password ? strlen(password) : 0);
   out = (char*)malloc(len);
   if (!out)
   {
      free(encoded);
      return NULL;
   }
   strcpy(out, sch);
   strcat(out, "://");
   /* Credentials mean nothing to a DLNA server, so they are left out. */
   if (user && *user && strcmp(sch, "dlna") != 0)
   {
      strcat(out, user);
      if (password && *password)
      {
         strcat(out, ":");
         strcat(out, password);
      }
      strcat(out, "@");
   }
   strcat(out, host);
   if (port && *port)
   {
      strcat(out, ":");
      strcat(out, port);
   }
   strcat(out, "/");
   strcat(out, encoded);
   free(encoded);
   return out;
}

/* Split a text file into trimmed lines (modifies 'text'). */
static size_t split_lines(char *text, char **lines, size_t max)
{
   size_t n = 0;
   char  *line = text, *next;

   /* Skip a UTF-8 byte order mark. */
   if ((unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB
         && (unsigned char)line[2] == 0xBF)
      line += 3;
   while (line && n < max)
   {
      next = strchr(line, '\n');
      if (next)
         *next++ = '\0';
      util_trim(line);
      lines[n++] = line;
      line = next;
   }
   return n;
}

static void dir_of(const char *path, char *dir, size_t size)
{
   const char *slash = strrchr(path, '/');
   const char *bslash = strrchr(path, '\\');
   size_t      len;

   if (bslash && (!slash || bslash > slash))
      slash = bslash;
   dir[0] = '\0';
   if (!slash)
   {
      strcpy(dir, ".");
      return;
   }
   len = (size_t)(slash - path);
   if (len >= size)
      len = size - 1;
   memcpy(dir, path, len);
   dir[len] = '\0';
}

/* A line of an .m3u (or an old style .dlna): a URI, or a path. */
static char *line_to_uri(const char *line, const char *base_dir)
{
   if (has_scheme(line))
   {
      if (util_starts_with_nocase(line, "file://"))
         return util_strdup(line + 7);
      return util_strdup(line);
   }
   return playlist_make_uri("file", "", NULL, NULL, NULL, line, base_dir,
         NULL, 0);
}

#define MAX_LINES 100000

static int load_lines(char *text, const char *base_dir, int dlna_format,
      playlist *pl, char *error, size_t size)
{
   char  **lines = (char**)malloc(MAX_LINES * sizeof(char*));
   size_t  n, i, first = 0;
   int     ok = 1;

   if (!lines)
      return 0;
   n = split_lines(text, lines, MAX_LINES);

   /* Skip leading blank lines. */
   while (first < n && !lines[first][0])
      first++;

   if (!dlna_format || (first < n && has_scheme(lines[first])))
   {
      /* .m3u, or a .dlna in the older one-URI-per-line style.  M3U
       * directives (#EXTM3U, #EXTINF, ...) and comments are skipped. */
      for (i = first; i < n; i++)
         if (lines[i][0] && lines[i][0] != '#')
            push(pl, line_to_uri(lines[i], base_dir));
   }
   else if (first < n)
   {
      /* .dlna: scheme, host[:port], then media paths.  The host line may
       * be blank (for "file"), so it is taken by position. */
      const char *scheme = lines[first];
      const char *hostport = first + 1 < n ? lines[first + 1] : "";
      char        host[512], *colon;
      const char *port = NULL;

      strncpy(host, hostport, sizeof(host) - 1);
      host[sizeof(host) - 1] = '\0';
      /* host:port, but leave IPv6 addresses ([...]) and user@host alone */
      colon = strrchr(host, ':');
      if (colon && host[0] != '[' && strspn(colon + 1, "0123456789")
            == strlen(colon + 1) && colon[1])
      {
         *colon = '\0';
         port   = colon + 1;
      }
      for (i = first + 2; i < n && ok; i++)
      {
         char *uri;
         if (!lines[i][0])
            continue;
         uri = playlist_make_uri(scheme, host, port, NULL, NULL, lines[i],
               base_dir, error, size);
         if (!uri)
            ok = 0;
         else
            push(pl, uri);
      }
   }
   free(lines);
   return ok;
}

static int load_json(const char *text, const char *base_dir, playlist *pl,
      char *error, size_t size)
{
   json_value       *root = json_parse(text, error, size);
   const json_value *lists;
   size_t            i, j;
   int               ok = 1;

   if (!root)
      return 0;
   lists = json_get(root, "playlists");
   if (!lists || lists->type != JSON_ARRAY)
   {
      set_error(error, size, "No \"playlists\" array in the file%s", "");
      json_free(root);
      return 0;
   }
   for (i = 0; i < lists->count && ok; i++)
   {
      const json_value *pl_obj = lists->items[i];
      const json_value *media  = json_get(pl_obj, "media");
      if (!media || media->type != JSON_ARRAY)
      {
         set_error(error, size, "Playlist entry without a \"media\" array%s", "");
         ok = 0;
         break;
      }
      for (j = 0; j < media->count && ok; j++)
      {
         char *uri;
         if (media->items[j]->type != JSON_STRING)
            continue;
         uri = playlist_make_uri(json_get_text(pl_obj, "scheme"),
               json_get_text(pl_obj, "host"), json_get_text(pl_obj, "port"),
               json_get_text(pl_obj, "username"),
               json_get_text(pl_obj, "password"), media->items[j]->text,
               base_dir, error, size);
         if (!uri)
            ok = 0;
         else
            push(pl, uri);
      }
   }
   json_free(root);
   return ok;
}

int playlist_load(const char *path, playlist *pl, char *error, size_t size)
{
   char  ext[16], base_dir[1024];
   char *text;
   int   ok;

   memset(pl, 0, sizeof(*pl));
   if (error && size)
      error[0] = '\0';
   text = util_read_file(path, NULL);
   if (!text)
   {
      set_error(error, size, "Cannot read %.200s", path);
      return 0;
   }
   dir_of(path, base_dir, sizeof(base_dir));
   util_extension(path, ext, sizeof(ext));
   if (strcmp(ext, "json") == 0)
      ok = load_json(text, base_dir, pl, error, size);
   else
      ok = load_lines(text, base_dir, strcmp(ext, "dlna") == 0, pl, error,
            size);
   free(text);
   if (ok && pl->count == 0)
   {
      set_error(error, size, "The playlist is empty%s", "");
      ok = 0;
   }
   if (!ok)
      playlist_free(pl);
   return ok;
}
