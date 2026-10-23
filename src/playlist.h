/*
 * playlist.h - reading .m3u, .dlna and .json playlist files.
 *
 * Every format is turned into a plain list of URIs the player knows how
 * to open:
 *
 *   dlna://server[:port]/folder/file.ext   (resolved through the browser)
 *   /local/path/file.ext                    (the "file" scheme)
 *   http://, https://, sftp://              (opened by FFmpeg)
 *   smb://, smbs://                         (opened through libsmb2)
 *
 * Formats (see README.md for examples):
 *
 *   .m3u / .m3u8  One URI or path per line.  Lines starting with '#' are
 *                 M3U directives/comments and are skipped.  Relative paths
 *                 are relative to the playlist file.
 *   .dlna         Line 1: scheme.  Line 2: host or host:port (blank,
 *                 "localhost" or "127.0.0.1" for "file").  Every further
 *                 line: a media path on that host.  Files written in the
 *                 older style (a full dlna:// URI per line) still work.
 *   .json         {"playlists": [{"scheme": "dlna", "host": "subaru",
 *                 "port": "5080", "username": "...", "password": "...",
 *                 "media": ["Folder/File.mp4", ...]}, ...]}
 *                 "scheme" defaults to "dlna"; port and credentials are
 *                 optional.
 */
#ifndef DLNA_PLAYLIST_H
#define DLNA_PLAYLIST_H

#include <stddef.h>

typedef struct playlist
{
   char  **items;   /* URIs, malloc'ed */
   size_t  count;
} playlist;

/* Is this a playlist file the core understands (by extension)? */
int playlist_is_file(const char *path);

/* Read a playlist file.  Returns 1 on success.  On failure, 'error'
 * describes the problem. */
int  playlist_load(const char *path, playlist *pl, char *error, size_t size);
void playlist_free(playlist *pl);

/* Build one URI from parts (used by the .dlna and .json readers, and
 * exposed for testing).  'base_dir' resolves relative "file" paths.
 * Returns NULL and sets 'error' if the scheme is not supported. */
char *playlist_make_uri(const char *scheme, const char *host, const char *port,
      const char *user, const char *password, const char *path,
      const char *base_dir, char *error, size_t size);

#endif
