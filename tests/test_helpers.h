/*
 * test_helpers.h — Cross-platform test fixture helpers.
 *
 * Replaces system("rm -rf"), system("mkdir -p && echo >"), chmod(),
 * and hardcoded /tmp/ paths with portable C implementations.
 * Uses compat.h/compat_fs.h underneath — same abstraction layer
 * as production code.
 *
 * All functions are static inline to avoid linker issues when
 * included from multiple test files.
 */
#ifndef TEST_HELPERS_H
#define TEST_HELPERS_H

#include "../src/foundation/compat.h"
#include "../src/foundation/compat_fs.h"
#include "../src/foundation/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include "../src/foundation/win_utf8.h"
#endif

/* ── Path building ────────────────────────────────────────────── */

/* Build a path from base + relative. Uses a static buffer — not reentrant.
 * Usage: th_write_file(TH_PATH(base, "src/main.go"), "content"); */
#define TH_PATH(base, rel) th_path_join(base, rel)

static inline const char *th_path_join(const char *base, const char *rel) {
    static char buf[4][1024];
    static int idx = 0;
    int i = idx;
    idx = (idx + 1) & 3;
    snprintf(buf[i], sizeof(buf[i]), "%s/%s", base, rel);
    return buf[i];
}

/* ── File writing ─────────────────────────────────────────────── */

/* Write content to a file, creating parent directories as needed. */
static inline int th_write_file(const char *path, const char *content) {
    /* Create parent directories */
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", path);
    char *last_slash = strrchr(dir, '/');
#ifdef _WIN32
    char *last_bslash = strrchr(dir, '\\');
    if (last_bslash && (!last_slash || last_bslash > last_slash)) {
        last_slash = last_bslash;
    }
#endif
    if (last_slash) {
        *last_slash = '\0';
        cbm_mkdir_p(dir, 0755);
    }

    FILE *f = cbm_fopen(path, "wb");
    if (!f) {
        return -1;
    }
    if (content && content[0]) {
        fputs(content, f);
    }
    fclose(f);
    return 0;
}

/* Append content to a file. */
static inline int th_append_file(const char *path, const char *content) {
    FILE *f = cbm_fopen(path, "ab");
    if (!f) {
        return -1;
    }
    if (content && content[0]) {
        fputs(content, f);
    }
    fclose(f);
    return 0;
}

/* ── Directory creation ───────────────────────────────────────── */

/* Create a directory and all parents. Returns 0 on success. */
static inline int th_mkdir_p(const char *path) {
    return cbm_mkdir_p(path, 0755) ? 0 : -1;
}

/* ── Recursive directory removal ──────────────────────────────── */

/* Git for Windows marks loose objects read-only. Match rm -rf semantics in
 * test cleanup without broadening production cbm_unlink behavior. */
static inline int th_unlink_force(const char *path) {
    int result = cbm_unlink(path);
#ifdef _WIN32
    if (result != 0) {
        wchar_t *wide = cbm_path_to_wide(path);
        DWORD attributes = wide ? GetFileAttributesW(wide) : INVALID_FILE_ATTRIBUTES;
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY) != 0U &&
            SetFileAttributesW(wide, attributes & ~((DWORD)FILE_ATTRIBUTE_READONLY))) {
            result = cbm_unlink(path);
        }
        free(wide);
    }
#endif
    return result;
}

/* Remove a file or directory tree recursively. Cross-platform rm -rf. */
static inline int th_rmtree(const char *path) {
    /* The platform wrappers preserve UTF-8 and add the extended-length prefix
     * on Windows; narrow CRT stat() silently treats paths beyond MAX_PATH as
     * absent and leaves their parents nonempty. */
    if (!cbm_file_exists(path)) {
        return 0; /* doesn't exist — success */
    }

    if (!cbm_is_dir(path)) {
        return th_unlink_force(path);
    }

    /* Directory — recurse into children */
    cbm_dir_t *d = cbm_opendir(path);
    if (!d) {
        return -1;
    }

    cbm_dirent_t *entry;
    int rc = 0;
    while ((entry = cbm_readdir(d)) != NULL) {
        if (strcmp(entry->name, ".") == 0 || strcmp(entry->name, "..") == 0) {
            continue;
        }
        char child[1024];
        snprintf(child, sizeof(child), "%s/%s", path, entry->name);
        if (entry->is_dir) {
            if (th_rmtree(child) != 0) {
                rc = -1;
            }
        } else {
            if (th_unlink_force(child) != 0) {
                rc = -1;
            }
        }
    }
    cbm_closedir(d);
    if (cbm_rmdir(path) != 0) {
        rc = -1;
    }
    return rc;
}

/* ── Temp directory creation ──────────────────────────────────── */

/* Create a temporary directory. Returns static buffer with path.
 * Pattern: prefix is used as part of the dirname. */
static inline char *th_mktempdir(const char *prefix) {
    static char buf[256];
#ifdef _WIN32
    const char *tmp = getenv("TEMP");
    if (!tmp)
        tmp = getenv("TMP");
    if (!tmp)
        tmp = "C:\\Temp";
    snprintf(buf, sizeof(buf), "%s\\%s_XXXXXX", tmp, prefix);
#else
    snprintf(buf, sizeof(buf), "/tmp/%s_XXXXXX", prefix);
#endif
    if (!cbm_mkdtemp(buf)) {
        return NULL;
    }
    return buf;
}

/* Runtime IPC validates the complete Windows directory ancestry, so an MSYS2
 * TEMP rooted under C:/msys64/tmp is intentionally unsuitable even when the
 * leaf made by cbm_mkdtemp() has a private DACL. Keep these security-sensitive
 * fixtures under LocalAppData on Windows; preserve the ordinary temporary-root
 * behavior on POSIX. */
static inline bool th_secure_runtime_parent_new(char *out, size_t out_cap, const char *tag) {
    if (!out || out_cap == 0 || !tag || !tag[0]) {
        return false;
    }
#ifdef _WIN32
    const char *base = cbm_app_local_dir();
#else
    const char *base = cbm_tmpdir();
#endif
    if (!base || !base[0]) {
        out[0] = '\0';
        return false;
    }
    int written = snprintf(out, out_cap, "%s/cbm-runtime-%s-XXXXXX", base, tag);
    if (written <= 0 || (size_t)written >= out_cap) {
        out[0] = '\0';
        return false;
    }
    return cbm_mkdtemp(out) != NULL;
}

/* ── File permissions (no-op on Windows) ──────────────────────── */

/* Make a file executable. On Windows this is a no-op (all files are
 * "executable" if they have the right extension). */
static inline void th_make_executable(const char *path) {
#ifndef _WIN32
    chmod(path, 0755);
#else
    (void)path;
#endif
}

/* ── Cleanup helper ───────────────────────────────────────────── */

/* Remove a temp directory tree. Safe to call with NULL. */
static inline void th_cleanup(const char *path) {
    if (path && path[0]) {
        th_rmtree(path);
    }
}

#ifndef _WIN32
/* ── Fake git on PATH (POSIX /bin/sh script) ──────────────────── */

/* A `git` first on PATH that appends its argv as one line to `calls`, prints
 * `stdout_line` (when non-NULL) and exits with `exit_code`. Code that shells
 * out through cbm_popen("git ...") finds it through the shell's PATH lookup,
 * so a test can count — and steer — every git process the code under test
 * spawns. th_fake_git_remove() restores PATH (a no-op when install never got
 * that far); the files live in the caller's temporary directory. */
typedef struct {
    char bin[1024];
    char script[1024];
    char calls[1024];
    char *saved_path;
    bool had_path;
    bool path_replaced;
} th_fake_git_t;

/* (Re)write the script: what the next git invocations print and return. The
 * printed line is single-quoted shell text, so it may not hold a quote. */
static inline bool th_fake_git_configure(const th_fake_git_t *fake, const char *stdout_line,
                                         int exit_code) {
    if (stdout_line && strchr(stdout_line, '\'')) {
        return false;
    }
    char body[4096];
    int n =
        stdout_line
            ? snprintf(body, sizeof(body),
                       "#!/bin/sh\nprintf '%%s\\n' \"$*\" >> '%s'\nprintf '%%s\\n' '%s'\n"
                       "exit %d\n",
                       fake->calls, stdout_line, exit_code)
            : snprintf(body, sizeof(body), "#!/bin/sh\nprintf '%%s\\n' \"$*\" >> '%s'\nexit %d\n",
                       fake->calls, exit_code);
    if (n <= 0 || (size_t)n >= sizeof(body) || th_write_file(fake->script, body) != 0) {
        return false;
    }
    return chmod(fake->script, 0700) == 0;
}

static inline bool th_fake_git_install(th_fake_git_t *fake, const char *dir,
                                       const char *stdout_line, int exit_code) {
    memset(fake, 0, sizeof(*fake));
    int bin_n = snprintf(fake->bin, sizeof(fake->bin), "%s/fake-git-bin", dir);
    int script_n = snprintf(fake->script, sizeof(fake->script), "%s/fake-git-bin/git", dir);
    int calls_n = snprintf(fake->calls, sizeof(fake->calls), "%s/fake-git.calls", dir);
    if (bin_n <= 0 || (size_t)bin_n >= sizeof(fake->bin) || script_n <= 0 ||
        (size_t)script_n >= sizeof(fake->script) || calls_n <= 0 ||
        (size_t)calls_n >= sizeof(fake->calls) || strchr(fake->calls, '\'') ||
        th_mkdir_p(fake->bin) != 0 || !th_fake_git_configure(fake, stdout_line, exit_code)) {
        return false;
    }
    const char *old_path = getenv("PATH");
    fake->had_path = old_path != NULL;
    fake->saved_path = old_path ? strdup(old_path) : NULL;
    if (old_path && !fake->saved_path) {
        return false;
    }
    size_t size = strlen(fake->bin) + 2U + (old_path ? strlen(old_path) : 0U);
    char *path = malloc(size);
    if (!path) {
        return false;
    }
    if (old_path) {
        (void)snprintf(path, size, "%s:%s", fake->bin, old_path);
    } else {
        (void)snprintf(path, size, "%s", fake->bin);
    }
    fake->path_replaced = cbm_setenv("PATH", path, 1) == 0;
    free(path);
    return fake->path_replaced;
}

/* Everything the fake recorded, one argv per line ("" when git never ran).
 * The caller frees it; NULL only on allocation failure. */
static inline char *th_fake_git_log(const th_fake_git_t *fake) {
    size_t cap = 1024;
    size_t len = 0;
    char *log = malloc(cap);
    if (!log) {
        return NULL;
    }
    FILE *f = cbm_fopen(fake->calls, "rb");
    int c;
    while (f && (c = fgetc(f)) != EOF) {
        if (len + 1 >= cap) {
            char *grown = realloc(log, cap * 2);
            if (!grown) {
                free(log);
                fclose(f);
                return NULL;
            }
            log = grown;
            cap *= 2;
        }
        log[len++] = (char)c;
    }
    log[len] = '\0';
    if (f) {
        fclose(f);
    }
    return log;
}

/* How many times git ran since install (or the last reset). -1 when the log
 * could not be read. */
static inline int th_fake_git_calls(const th_fake_git_t *fake) {
    char *log = th_fake_git_log(fake);
    if (!log) {
        return -1;
    }
    int calls = 0;
    for (const char *p = log; *p; p++) {
        calls += *p == '\n';
    }
    free(log);
    return calls;
}

static inline void th_fake_git_reset_calls(const th_fake_git_t *fake) {
    (void)cbm_unlink(fake->calls);
}

static inline void th_fake_git_remove(th_fake_git_t *fake) {
    if (fake->path_replaced) {
        if (fake->had_path) {
            (void)cbm_setenv("PATH", fake->saved_path, 1);
        } else {
            (void)cbm_unsetenv("PATH");
        }
    }
    free(fake->saved_path);
    fake->saved_path = NULL;
    fake->path_replaced = false;
}
#endif /* _WIN32 */

#endif /* TEST_HELPERS_H */
