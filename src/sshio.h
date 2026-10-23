/*
 * sshio.h - reading files over SFTP with libssh, presented to FFmpeg as a
 * custom AVIOContext (like smbio.h does for SMB).
 *
 * Why not FFmpeg's own sftp:// support?  FFmpeg releases up to and
 * including 9.0 do not check the server's host key at all, so anyone on
 * the network could pose as the server and collect the password.  Here
 * the host key is always checked against ~/.ssh/known_hosts and unknown
 * or changed keys are refused.  (Connect to the server once with ssh or
 * sftp to record its key.)  If <system>/dlna/known_hosts exists it is used
 * instead of ~/.ssh/known_hosts, which keeps the core's trusted servers
 * separate and works on systems without a usual home directory.
 *
 * URL:  sftp://[user[:password]@]host[:port]/absolute/path
 * Authentication tries the SSH agent and the default keys first, then the
 * password.  User and password may contain %XX escapes.
 *
 * libssh is optional at build time (HAVE_LIBSSH).
 */
#ifndef DLNA_SSHIO_H
#define DLNA_SSHIO_H

#include <stddef.h>

struct AVIOContext;

void sshio_global_init(const char *system_dir);
void sshio_global_deinit(void);

/* Is this an sftp:// URL? */
int sshio_is_sftp(const char *url);

/* Connect, verify, authenticate and open.  NULL on failure, with a reason
 * in 'error'. */
struct AVIOContext *sshio_open(const char *url, char *error, size_t size);
void                sshio_close(struct AVIOContext **ctx);

#endif
