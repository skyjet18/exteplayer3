#ifndef CA_BUNDLE_H
#define CA_BUNDLE_H

#include <stddef.h>

/* CA bundles of the common images and distributions, in the order tried:
 * Debian/OE (openATV, OpenPLi, VTi, DreamOS), Fedora/RHEL 6, openSUSE,
 * OpenELEC, CentOS/RHEL 7, Alpine/LibreSSL. */
#define CA_BUNDLE_CANDIDATES \
    "/etc/ssl/certs/ca-certificates.crt:" \
    "/etc/pki/tls/certs/ca-bundle.crt:" \
    "/etc/ssl/ca-bundle.pem:" \
    "/etc/pki/tls/cacert.pem:" \
    "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem:" \
    "/etc/ssl/cert.pem"

/* Default locations of OpenSSL when the player cannot ask libcrypto
 * (OPENSSLDIR=/etc/ssl). */
#define CA_BUNDLE_DEFAULT_FILE "/etc/ssl/cert.pem"
#define CA_BUNDLE_DEFAULT_DIR  "/etc/ssl/certs"

/* The file to export as SSL_CERT_FILE, or NULL to leave the environment
 * as it is:
 * - cert_file_env (SSL_CERT_FILE) set: NULL, it wins.
 * - ca_file_option (-f ca_file / cafile) a readable file: that file, so
 *   that the connections FFmpeg opens without the option (HLS/DASH
 *   playlists, segments, keys) trust it too.
 * - default_file readable, or a directory of cert_dir_env (SSL_CERT_DIR,
 *   ':' separated) or else of default_dir with a hashed certificate
 *   link: NULL, OpenSSL finds the certificates itself.
 * - otherwise the first readable file of candidates (':' separated).
 * The result points into candidates or ca_file_option. */
const char *ca_bundle_choose(const char *cert_file_env, const char *cert_dir_env,
                             const char *ca_file_option, const char *default_file,
                             const char *default_dir, const char *candidates,
                             size_t *length);

/* At startup, before any thread: sets SSL_CERT_FILE as ca_bundle_choose()
 * decides, with the default locations of the libcrypto loaded with
 * FFmpeg and the candidates of EP3_CA_FILES (tests) or
 * CA_BUNDLE_CANDIDATES. EP3_CA_DEFAULT_FILE (tests) replaces the default
 * file it checks; OpenSSL itself keeps its own. Returns 1 when it set
 * the variable. */
int ca_bundle_setup(const char *ca_file_option);

#endif
