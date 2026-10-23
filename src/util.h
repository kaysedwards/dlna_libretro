/*
 * util.h - small string, time and logging helpers shared by every module.
 *
 * Everything in this project is written in ISO C90.  See docs/C90_NOTES.md
 * for the handful of places where the libraries we depend on force us to
 * step outside of strict C90 (fixed width integer types, snprintf, ...).
 */
#ifndef DLNA_UTIL_H
#define DLNA_UTIL_H

#include <stddef.h>

/* Log levels mirror enum retro_log_level so the values can be passed
 * straight through to the frontend's logger. */
#define LOG_DEBUG 0
#define LOG_INFO  1
#define LOG_WARN  2
#define LOG_ERROR 3

/* Media categories used throughout the core.  The numeric values are used
 * as array indices in a few places, so keep MT_COUNT last. */
typedef enum media_type
{
   MT_NONE = 0,
   MT_VIDEO,
   MT_AUDIO,
   MT_IMAGE,
   MT_COUNT
} media_type;

/* Logging.  util_set_logger() is called once by libretro.c with a function
 * that receives an already formatted message (NULL means stderr).
 * dlna_log() is safe to call from any thread. */
typedef void (*util_log_fn)(int level, const char *message);
void util_set_logger(util_log_fn fn);
void dlna_log(int level, const char *fmt, ...);

/* Memory helpers.  util_strdup(NULL) returns NULL. */
char *util_strdup(const char *s);
char *util_strndup(const char *s, size_t n);
void *util_calloc(size_t count, size_t size);

/* Replace *dst with a copy of src (freeing the previous value). */
void util_replace(char **dst, const char *src);

/* ASCII-only case insensitive comparisons (UTF-8 bytes >= 0x80 compare
 * as-is, which is good enough for matching server and folder names). */
int util_strcasecmp(const char *a, const char *b);
int util_strncasecmp(const char *a, const char *b, size_t n);
const char *util_strcasestr(const char *haystack, const char *needle);
int util_starts_with_nocase(const char *s, const char *prefix);

/* "Natural" comparison: case insensitive, and runs of digits compare by
 * numeric value so "Episode 2" sorts before "Episode 10". */
int util_natural_compare(const char *a, const char *b);

/* Trim leading/trailing ASCII white space in place. */
void util_trim(char *s);

/* Decode %XX escapes in place ('+' is left alone since dlna:// URIs are not
 * form encoded). */
void util_percent_decode(char *s);

/* Trusted certificate authorities for https:// (FFmpeg's "ca_file").
 * util_find_ca_file() looks for <system>/dlna/ca-certificates.crt, then
 * the operating system's usual bundle; util_ca_file() returns what was
 * found, or NULL (HTTPS then fails rather than skipping verification). */
void        util_find_ca_file(const char *system_dir);
const char *util_ca_file(void);

/* Escape one dlna:// path component: '%', '/', '?' and '#' become %XX so
 * the component survives being split on '/' and percent-decoded again.
 * Everything else (spaces, UTF-8) is kept readable. */
void util_uri_escape(const char *in, char *out, size_t size);

/* Copy a URL with any password removed ("sftp://me:secret@host/x" becomes
 * "sftp://me@host/x"), for logs and save states.  Returns a new string. */
char *util_uri_without_password(const char *uri);

/* Returns a pointer to the extension of a file name or URL (without the
 * dot), ignoring any "?query" or "#fragment".  The result is copied into
 * buf (lower-cased) and buf is returned; an empty string means "none". */
char *util_extension(const char *path, char *buf, size_t size);

/* Guess the media type from a lower-case file extension (no dot). */
media_type util_media_type_from_extension(const char *ext);

/* Guess the media type from a MIME type such as "video/mp4". */
media_type util_media_type_from_mime(const char *mime);

/* Decode one UTF-8 code point from *s and advance *s.  Returns -1 at the
 * terminating NUL.  Malformed sequences yield U+FFFD and consume one byte. */
long util_utf8_next(const char **s);

/* Number of bytes of the UTF-8 sequence starting with lead byte c. */
int util_utf8_len(unsigned char c);

/* Format seconds as "H:MM:SS" (or "M:SS" when under an hour).  Negative or
 * non-finite values print "--:--". */
void util_format_time(double seconds, char *buf, size_t size);

/* Parse a DIDL-Lite duration such as "1:02:03.456" into seconds.
 * Returns a negative number when the string is not a duration. */
double util_parse_duration(const char *s);

/* Parse a "WIDTHxHEIGHT" resolution string.  Returns 1 on success. */
int util_parse_resolution(const char *s, int *w, int *h);

/* Does the file exist and is it readable?  (Plain stdio, works with any
 * path the frontend gives us.) */
int util_file_exists(const char *path);

/* Join a directory and a file name with the platform separator. */
void util_path_join(char *out, size_t size, const char *dir, const char *name);

/* Read an entire file into a malloc'ed, NUL terminated buffer.  Returns
 * NULL on failure.  *len (optional) receives the length without the NUL. */
char *util_read_file(const char *path, size_t *len);

/* A tiny growable array of pointers. */
typedef struct ptr_array
{
   void  **items;
   size_t  count;
   size_t  capacity;
} ptr_array;

int  ptr_array_push(ptr_array *a, void *item);
void ptr_array_clear(ptr_array *a); /* frees the array, not the items */

#endif
