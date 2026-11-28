/* The CA bundle of https inputs: when SSL_CERT_FILE is set and to what. */
#define _GNU_SOURCE
#include "ca_bundle.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char root[] = "/tmp/exteplayer3-ca-test-XXXXXX";

static const char *at(const char *name)
{
    static char paths[8][512];
    static int next;
    char *path = paths[next++ % 8];
    snprintf(path, sizeof(paths[0]), "%s/%s", root, name);
    return path;
}

static void file(const char *name, const char *content)
{
    FILE *f = fopen(at(name), "w");
    assert(f);
    fputs(content, f);
    fclose(f);
}

/* ca_bundle_choose() as a NUL terminated string, "" for NULL. */
static const char *choose(const char *file_env, const char *dir_env, const char *option,
                          const char *default_file, const char *default_dir,
                          const char *candidates)
{
    static char result[1024];
    size_t length = 0;
    const char *chosen = ca_bundle_choose(file_env, dir_env, option, default_file,
                                          default_dir, candidates, &length);
    if (!chosen)
        return "";
    assert(length && length < sizeof(result));
    memcpy(result, chosen, length);
    result[length] = '\0';
    return result;
}

int main(void)
{
    char candidates[2048];
    assert(mkdtemp(root));
    assert(!mkdir(at("plain"), 0700));   /* VTi: the bundle only */
    assert(!mkdir(at("hashed"), 0700));  /* openATV: hashed links */
    assert(!mkdir(at("empty"), 0700));
    file("plain/ca-certificates.crt", "PEM\n");
    file("hashed/ca-certificates.crt", "PEM\n");
    assert(!symlink("ca-certificates.crt", at("hashed/5ad8a5d6.0")));
    file("plain/1234.0", "not a hash\n");
    file("plain/abcdef01.r0", "a CRL, not a certificate\n");
    file("bundle.crt", "PEM\n");
    file("other.pem", "PEM\n");
    file("zero.pem", "");
    snprintf(candidates, sizeof(candidates), "%s:%s:%s:%s", at("missing.crt"), at("zero.pem"),
             at("plain"), at("bundle.crt"));

    /* VTi: no default file, no hashed link -> the first readable,
     * non-empty regular file of the list. */
    assert(!strcmp(choose(NULL, NULL, NULL, at("cert.pem"), at("plain"), candidates),
                   at("bundle.crt")));
    assert(!strcmp(choose("", "", "", at("cert.pem"), at("plain"), candidates),
                   at("bundle.crt")));
    /* An explicit SSL_CERT_FILE wins, also over -f ca_file. */
    assert(!*choose(at("other.pem"), NULL, NULL, at("cert.pem"), at("plain"), candidates));
    assert(!*choose("/missing", NULL, at("other.pem"), at("cert.pem"), at("plain"), candidates));
    /* -f ca_file: passed on, also where the defaults are usable. */
    assert(!strcmp(choose(NULL, NULL, at("other.pem"), at("cert.pem"), at("plain"), candidates),
                   at("other.pem")));
    assert(!strcmp(choose(NULL, NULL, at("other.pem"), at("bundle.crt"), at("hashed"), candidates),
                   at("other.pem")));
    /* An unreadable -f ca_file is not exported; FFmpeg reports it. */
    assert(!strcmp(choose(NULL, NULL, at("missing.crt"), at("cert.pem"), at("plain"), candidates),
                   at("bundle.crt")));
    /* Usable defaults: a default file, or a hashed link in the directory. */
    assert(!*choose(NULL, NULL, NULL, at("other.pem"), at("plain"), candidates));
    assert(!*choose(NULL, NULL, NULL, at("zero.pem"), at("hashed"), candidates));
    assert(!*choose(NULL, NULL, NULL, at("cert.pem"), at("hashed"), candidates));
    assert(!strcmp(choose(NULL, NULL, NULL, at("zero.pem"), at("plain"), candidates),
                   at("bundle.crt")));
    /* SSL_CERT_DIR replaces the default directory; ':' separated. */
    assert(!strcmp(choose(NULL, at("empty"), NULL, at("cert.pem"), at("hashed"), candidates),
                   at("bundle.crt")));
    {
        char dirs[1100];
        snprintf(dirs, sizeof(dirs), "%s:%s", at("empty"), at("hashed"));
        assert(!*choose(NULL, dirs, NULL, at("cert.pem"), at("plain"), candidates));
        snprintf(dirs, sizeof(dirs), "%s::%s:", at("missing"), at("plain"));
        assert(!strcmp(choose(NULL, dirs, NULL, at("cert.pem"), at("hashed"), candidates),
                       at("bundle.crt")));
    }
    /* Nothing usable: the environment stays as it is. */
    assert(!*choose(NULL, NULL, NULL, at("cert.pem"), at("plain"), at("missing.crt")));
    assert(!*choose(NULL, NULL, NULL, at("cert.pem"), at("plain"), ""));
    assert(!*choose(NULL, NULL, NULL, NULL, NULL, NULL));

    /* ca_bundle_setup(): the environment of the player, with the default
     * file and directory of the test (EP3_CA_DEFAULT_FILE, SSL_CERT_DIR). */
    {
        assert(!setenv("SSL_CERT_DIR", at("empty"), 1));
        assert(!setenv("EP3_CA_FILES", candidates, 1));
        assert(!setenv("EP3_CA_DEFAULT_FILE", at("cert.pem"), 1));
        unsetenv("SSL_CERT_FILE");
        assert(ca_bundle_setup(at("other.pem")) == 1);
        assert(getenv("SSL_CERT_FILE") && !strcmp(getenv("SSL_CERT_FILE"), at("other.pem")));
        assert(ca_bundle_setup(at("bundle.crt")) == 0);  /* now set: it wins */
        assert(getenv("SSL_CERT_FILE") && !strcmp(getenv("SSL_CERT_FILE"), at("other.pem")));
        unsetenv("SSL_CERT_FILE");
        assert(ca_bundle_setup(NULL) == 1);
        assert(getenv("SSL_CERT_FILE") && !strcmp(getenv("SSL_CERT_FILE"), at("bundle.crt")));
        unsetenv("SSL_CERT_FILE");
        assert(!setenv("EP3_CA_DEFAULT_FILE", at("other.pem"), 1));
        assert(ca_bundle_setup(NULL) == 0);
        assert(!getenv("SSL_CERT_FILE"));
        assert(!setenv("EP3_CA_DEFAULT_FILE", at("cert.pem"), 1));
        assert(!setenv("SSL_CERT_DIR", at("hashed"), 1));
        assert(ca_bundle_setup(NULL) == 0);
        assert(!getenv("SSL_CERT_FILE"));
    }

    unlink(at("hashed/5ad8a5d6.0"));
    unlink(at("hashed/ca-certificates.crt"));
    unlink(at("plain/ca-certificates.crt"));
    unlink(at("plain/1234.0"));
    unlink(at("plain/abcdef01.r0"));
    unlink(at("bundle.crt"));
    unlink(at("other.pem"));
    unlink(at("zero.pem"));
    rmdir(at("plain"));
    rmdir(at("hashed"));
    rmdir(at("empty"));
    rmdir(root);
    printf("ca_bundle: OK\n");
    return 0;
}
