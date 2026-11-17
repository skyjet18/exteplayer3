#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* RTLD_DEFAULT */
#endif
#include "ca_bundle.h"

#include <dirent.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* A readable, non-empty regular file (links followed). */
static int readable_file(const char *path, size_t length)
{
    char buffer[4096];
    struct stat st;
    if (!length || length >= sizeof(buffer))
        return 0;
    memcpy(buffer, path, length);
    buffer[length] = '\0';
    return !stat(buffer, &st) && S_ISREG(st.st_mode) && st.st_size > 0 &&
           !access(buffer, R_OK);
}

/* OpenSSL looks certificates up in a directory as <8 hex digits>.<n>. */
static int hashed_name(const char *name)
{
    int i;
    for (i = 0; i < 8; i++)
        if (!name[i] || !strchr("0123456789abcdef", name[i]))
            return 0;
    if (name[8] != '.' || !name[9])
        return 0;
    for (i = 9; name[i]; i++)
        if (name[i] < '0' || name[i] > '9')
            return 0;
    return 1;
}

static int hashed_directory(const char *path, size_t length)
{
    char buffer[4096];
    struct dirent *entry;
    int found = 0;
    DIR *dir;
    if (!length || length >= sizeof(buffer))
        return 0;
    memcpy(buffer, path, length);
    buffer[length] = '\0';
    dir = opendir(buffer);
    if (!dir)
        return 0;
    while (!found && (entry = readdir(dir)))
        found = hashed_name(entry->d_name);
    closedir(dir);
    return found;
}

static int any_hashed_directory(const char *list)
{
    while (list && *list)
    {
        size_t length = strcspn(list, ":");
        if (hashed_directory(list, length))
            return 1;
        list += length;
        if (*list == ':')
            list++;
    }
    return 0;
}

const char *ca_bundle_choose(const char *cert_file_env, const char *cert_dir_env,
                             const char *ca_file_option, const char *default_file,
                             const char *default_dir, const char *candidates,
                             size_t *length)
{
    if (cert_file_env && *cert_file_env)
        return NULL;
    if (ca_file_option && readable_file(ca_file_option, strlen(ca_file_option)))
    {
        *length = strlen(ca_file_option);
        return ca_file_option;
    }
    if (default_file && readable_file(default_file, strlen(default_file)))
        return NULL;
    if (any_hashed_directory(cert_dir_env && *cert_dir_env ? cert_dir_env : default_dir))
        return NULL;
    while (candidates && *candidates)
    {
        size_t size = strcspn(candidates, ":");
        if (readable_file(candidates, size))
        {
            *length = size;
            return candidates;
        }
        candidates += size;
        if (*candidates == ':')
            candidates++;
    }
    return NULL;
}

int ca_bundle_setup(const char *ca_file_option)
{
    /* libcrypto comes with libavformat when FFmpeg uses OpenSSL; its
     * defaults follow OPENSSLDIR of that build. */
    const char *(*get_file)(void) = (const char *(*)(void))dlsym(RTLD_DEFAULT, "X509_get_default_cert_file");
    const char *(*get_dir)(void) = (const char *(*)(void))dlsym(RTLD_DEFAULT, "X509_get_default_cert_dir");
    const char *default_file = get_file ? get_file() : CA_BUNDLE_DEFAULT_FILE;
    const char *default_dir = get_dir ? get_dir() : CA_BUNDLE_DEFAULT_DIR;
    const char *candidates = getenv("EP3_CA_FILES");
    const char *test_default = getenv("EP3_CA_DEFAULT_FILE");
    const char *chosen;
    char path[4096];
    size_t length = 0;
    if (!candidates)
        candidates = CA_BUNDLE_CANDIDATES;
    if (test_default)
        default_file = test_default;
    chosen = ca_bundle_choose(getenv("SSL_CERT_FILE"), getenv("SSL_CERT_DIR"), ca_file_option,
                              default_file, default_dir, candidates, &length);
    if (!chosen || length >= sizeof(path))
        return 0;
    memcpy(path, chosen, length);
    path[length] = '\0';
    if (setenv("SSL_CERT_FILE", path, 1))
        return 0;
    printf("CA certificates for https: %s (SSL_CERT_FILE)\n", path);
    return 1;
}
