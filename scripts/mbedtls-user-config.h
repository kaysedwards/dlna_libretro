/*
 * mbedtls-user-config.h - extra settings for the mbedTLS built by
 * scripts/build-deps.sh (added on top of mbedTLS's default configuration
 * through MBEDTLS_USER_CONFIG_FILE, so the submodule itself is untouched).
 *
 * Threading support: the core uses TLS (FFmpeg's https://) and SSH
 * (libssh) from several threads, and libssh refuses to build against an
 * mbedTLS without it.
 */
#ifndef DLNA_MBEDTLS_USER_CONFIG_H
#define DLNA_MBEDTLS_USER_CONFIG_H

#if !defined(_WIN32)
#define MBEDTLS_THREADING_C
#define MBEDTLS_THREADING_PTHREAD
#endif

#endif
