/*
 * smbio.h - reading files from SMB2/SMB3 shares with libsmb2, presented to
 * FFmpeg as a custom AVIOContext so they stream and seek like any other
 * input.
 *
 * URLs:  smb://[user[:password]@]server[:port]/share/path/to/file
 *        smbs://...   the same, but requires SMB3 encryption
 *
 * libsmb2 is optional at build time (HAVE_LIBSMB2); without it smbio_open
 * simply reports that SMB is not supported.
 */
#ifndef DLNA_SMBIO_H
#define DLNA_SMBIO_H

#include <stddef.h>

struct AVIOContext;

/* Is this an smb:// or smbs:// URL? */
int smbio_is_smb(const char *url);

/* Connect and open the file.  Returns NULL on failure with a reason in
 * 'error'. */
struct AVIOContext *smbio_open(const char *url, char *error, size_t size);

/* Close the file, disconnect and free the context (sets *ctx to NULL). */
void smbio_close(struct AVIOContext **ctx);

#endif
