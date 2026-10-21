#include "git/git_context.h"

#include "foundation/compat_fs.h"
#include "foundation/constants.h"
#include "foundation/platform.h"
#include "foundation/str_util.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <sys/wait.h>
#endif

enum {
    GIT_CMD_MAX = 1024,
    GIT_OUTPUT_MAX = 4096,
    GIT_EXIT_UNKNOWN = -1, /* git could not be run, or did not exit normally */
};

/* The branch reported for a detached HEAD — by cbm_git_context_resolve() when
 * `symbolic-ref` fails, and by cbm_git_head_branch() for an object-id HEAD. */
#define GIT_DETACHED_BRANCH "DETACHED"

/* Bounded worktree-status stream constants.
 * STATUS_FIELD_MAX is the largest single NUL-delimited record field we
 * buffer; a field at/over the bound is treated as a malformed stream and the
 * whole snapshot fails closed. PATH components of real repos are far below
 * this even with long paths. */
enum {
    STATUS_HEADER_LEN = 3, /* porcelain-v1 record prefix "XY " */
    STATUS_CHUNK_MAX = CBM_SZ_4K,
    STATUS_FIELD_MAX = CBM_SZ_16K,
};

static char *git_strdup(const char *s) {
    if (!s) {
        s = "";
    }
    size_t n = strlen(s) + 1;
    char *out = (char *)malloc(n);
    if (!out) {
        return NULL;
    }
    memcpy(out, s, n);
    return out;
}

static void trim_newlines(char *s) {
    if (!s) {
        return;
    }
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[--n] = '\0';
    }
}

static bool git_validate_repo_path(const char *repo_path) {
    return cbm_validate_shell_path_arg(repo_path);
}

/* Exit status of a cbm_pclose() result: POSIX pclose() reports a wait status,
 * the Windows wrapper the child's exit code. -1 when the child did not exit
 * normally or its status is unknown. */
static int git_exit_status(int pclose_rc) {
#ifdef _WIN32
    return pclose_rc < 0 ? GIT_EXIT_UNKNOWN : pclose_rc;
#else
    if (pclose_rc < 0 || !WIFEXITED(pclose_rc)) {
        return GIT_EXIT_UNKNOWN;
    }
    return WEXITSTATUS(pclose_rc);
#endif
}

/* First output line of `git -C <repo_path> <git_args>`. Returns 0 with a heap
 * copy in *out only when git exited 0 and printed a non-empty line; *exit_status
 * receives git's exit status, GIT_EXIT_UNKNOWN when git could not be run or
 * did not exit normally. */
static int git_capture_status(const char *repo_path, const char *git_args, char **out,
                              int *exit_status) {
    if (!out || !exit_status) {
        return CBM_NOT_FOUND;
    }
    *out = NULL;
    *exit_status = GIT_EXIT_UNKNOWN;
    if (!repo_path || !git_args || !git_validate_repo_path(repo_path)) {
        return CBM_NOT_FOUND;
    }

    char cmd[GIT_CMD_MAX];
#ifdef _WIN32
    const char *null_dev = "NUL";
#else
    const char *null_dev = "/dev/null";
#endif
    /* Double quotes work for POSIX shells and cmd.exe. cbm_validate_shell_arg()
     * rejects quote/backslash/substitution metacharacters before interpolation. */
    int n = snprintf(cmd, sizeof(cmd), "git -C \"%s\" %s 2>%s", repo_path, git_args, null_dev);
    if (n < 0 || n >= (int)sizeof(cmd)) {
        return CBM_NOT_FOUND;
    }

    FILE *fp = cbm_popen(cmd, "r");
    if (!fp) {
        return CBM_NOT_FOUND;
    }

    char buf[GIT_OUTPUT_MAX];
    bool got_line = fgets(buf, sizeof(buf), fp) != NULL;
    *exit_status = git_exit_status(cbm_pclose(fp));
    if (!got_line) {
        return CBM_NOT_FOUND;
    }
    trim_newlines(buf);
    if (*exit_status != 0 || buf[0] == '\0') {
        return CBM_NOT_FOUND;
    }

    *out = git_strdup(buf);
    return *out ? 0 : CBM_NOT_FOUND;
}

static int git_capture(const char *repo_path, const char *git_args, char **out) {
    int exit_status = 0;
    return git_capture_status(repo_path, git_args, out, &exit_status);
}

static bool path_is_absolute(const char *path) {
    if (!path || !path[0]) {
        return false;
    }
    if (path[0] == '/') {
        return true;
    }
#ifdef _WIN32
    return isalpha((unsigned char)path[0]) && path[1] == ':';
#else
    return false;
#endif
}

/* Read the "gitdir: <path>" pointer out of a gitlink FILE at <path>/.git.
 * Returns false when .git is missing, a directory (ordinary repo), or holds no
 * pointer. A relative pointer is resolved against path. */
static bool read_gitlink_target(const char *path, char *out, size_t out_size) {
    char dot_git[GIT_OUTPUT_MAX];
    int n = snprintf(dot_git, sizeof(dot_git), "%s/.git", path);
    if (n < 0 || n >= (int)sizeof(dot_git)) {
        return false;
    }
    struct stat st;
    if (stat(dot_git, &st) != 0 || !S_ISREG(st.st_mode)) {
        return false;
    }

    FILE *f = cbm_fopen(dot_git, "r");
    if (!f) {
        return false;
    }
    char line[GIT_OUTPUT_MAX];
    bool got = false;
    while (fgets(line, sizeof(line), f)) {
        trim_newlines(line);
        if (strncmp(line, "gitdir:", 7) != 0) {
            continue;
        }
        const char *value = line + 7;
        while (*value == ' ' || *value == '\t') {
            value++;
        }
        if (!value[0]) {
            break;
        }
        int written = path_is_absolute(value) ? snprintf(out, out_size, "%s", value)
                                              : snprintf(out, out_size, "%s/%s", path, value);
        got = written > 0 && written < (int)out_size;
        break;
    }
    fclose(f);
    return got;
}

bool cbm_git_is_linked_worktree(const char *path) {
    if (!path || !path[0]) {
        return false;
    }
    char git_dir[GIT_OUTPUT_MAX];
    if (!read_gitlink_target(path, git_dir, sizeof(git_dir))) {
        return false;
    }
    /* Only linked worktrees carry <gitdir>/commondir; a submodule gitlink
     * points at <super>/.git/modules/<name>, which does not. */
    char commondir[GIT_OUTPUT_MAX];
    int n = snprintf(commondir, sizeof(commondir), "%s/commondir", git_dir);
    if (n < 0 || n >= (int)sizeof(commondir)) {
        return false;
    }
    struct stat st;
    return stat(commondir, &st) == 0 && S_ISREG(st.st_mode);
}

/* ── Branch from HEAD, without a subprocess ───────────────────────── */

enum {
    /* A loose HEAD ("ref: refs/heads/<name>" or an object id) and a gitlink
     * file ("gitdir: <path>") are one short line. Ref names and git-directory
     * paths live under PATH_MAX, so a file this large is neither and is never
     * parsed. */
    GIT_SMALL_FILE_MAX = CBM_SZ_4K,
    /* A canonical directory plus the entry probed inside it. */
    GIT_PROBE_PATH_MAX = CBM_SZ_4K + CBM_SZ_64,
    GIT_OID_HEX_SHA1 = 40,
    GIT_OID_HEX_SHA256 = 64,
    /* Bound on the upward search for .git; real trees are far shallower. */
    GIT_DISCOVERY_DEPTH_MAX = CBM_SZ_256,
    /* `git symbolic-ref --quiet` exits 1, silently, when HEAD exists but is
     * not a symbolic ref (detached). "Not a repository" and every other fatal
     * error exit 128. */
    GIT_SYMREF_NOT_SYMBOLIC = 1,
    ASCII_FIRST_PRINTABLE = 0x20,
    ASCII_DEL = 0x7f,
};

typedef enum {
    HEAD_NO_BRANCH = 0, /* nothing to report: no work tree, or no room in out */
    HEAD_FILE_FOUND,    /* head_path names the HEAD file to read */
    HEAD_BRANCH_READ,   /* out holds the branch, or the detached marker */
    HEAD_ASK_GIT,       /* an on-disk form not parsed here: ask git, once */
} head_lookup_t;

static const char GITLINK_PREFIX[] = "gitdir: ";
static const char HEAD_SYMREF_PREFIX[] = "ref:";
static const char HEADS_PREFIX[] = "refs/heads/";
static const char REF_LOCK_SUFFIX[] = ".lock";
/* Bytes git's check_refname_format() never accepts in a ref name. */
static const char REF_FORBIDDEN_BYTES[] = " ~^:?*[\\";

/* Copy text into out whole; false, with out emptied, when it does not fit. */
static bool copy_whole(char *out, size_t out_size, const char *text) {
    size_t len = strlen(text);
    if (len >= out_size) {
        out[0] = '\0';
        return false;
    }
    memcpy(out, text, len + 1);
    return true;
}

static bool path_is_separator(char c) {
#ifdef _WIN32
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

/* Append "/<name>" to the path in buf, in place (no doubled separator after a
 * root such as "/"). False, leaving buf unchanged, when it does not fit. */
static bool path_append(char *buf, size_t buf_size, const char *name) {
    size_t len = strlen(buf);
    bool add_separator = len > 0 && !path_is_separator(buf[len - 1]);
    size_t name_len = strlen(name);
    if (len + (add_separator ? 1U : 0U) + name_len >= buf_size) {
        return false;
    }
    if (add_separator) {
        buf[len++] = '/';
    }
    memcpy(buf + len, name, name_len + 1);
    return true;
}

/* path + "/<name>" into out; false when it does not fit. */
static bool path_join(char *out, size_t out_size, const char *path, const char *name) {
    int n = snprintf(out, out_size, "%s", path);
    return n >= 0 && (size_t)n < out_size && path_append(out, out_size, name);
}

/* Drop the last component of a canonical absolute path, in place. False at
 * the root ("/", or "C:\" on Windows), which has no parent. */
static bool path_parent_in_place(char *dir) {
    char *last = NULL;
    for (char *p = dir; *p; p++) {
        if (path_is_separator(*p) && p[1] != '\0') {
            last = p;
        }
    }
    if (!last) {
        return false;
    }
    size_t keep = (size_t)(last - dir);
    bool parent_is_root = keep == 0;
#ifdef _WIN32
    parent_is_root = parent_is_root || (keep == 2 && dir[1] == ':');
#endif
    /* A root keeps its separator: "/a" -> "/", "C:\a" -> "C:\". */
    dir[parent_is_root ? keep + 1 : keep] = '\0';
    return true;
}

static bool is_ascii_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

static void trim_trailing_space(char *s) {
    size_t n = strlen(s);
    while (n > 0 && is_ascii_space(s[n - 1])) {
        s[--n] = '\0';
    }
}

/* Read a small regular file whole, NUL-terminated. False when the path is not
 * a regular file (a symlink is not followed), cannot be read, holds a NUL
 * byte, or does not fit buf — never a truncated prefix. */
static bool read_small_regular_file(const char *path, char *buf, size_t buf_size) {
    cbm_path_info_t info;
    if (buf_size == 0 || cbm_path_info_utf8(path, &info) != CBM_PATH_INFO_OK || !info.is_regular ||
        info.size < 0 || (uint64_t)info.size >= buf_size) {
        return false;
    }
    FILE *f = cbm_fopen(path, "rb");
    if (!f) {
        return false;
    }
    /* Ask for a full buffer: filling it means the file grew past the bound. */
    size_t n = fread(buf, sizeof(char), buf_size, f);
    bool complete = !ferror(f) && n < buf_size;
    fclose(f);
    if (!complete || memchr(buf, '\0', n) != NULL) {
        return false;
    }
    buf[n] = '\0';
    return true;
}

/* Is dir itself a git directory — a bare repository, or the .git of the work
 * tree the walk started inside? git's is_git_directory() test, short of
 * validating HEAD's content: HEAD, objects/ and refs/ all present. */
static bool dir_is_git_directory(const char *dir) {
    char probe[GIT_PROBE_PATH_MAX];
    return path_join(probe, sizeof(probe), dir, "HEAD") && cbm_file_exists(probe) &&
           path_join(probe, sizeof(probe), dir, "objects") && cbm_is_dir(probe) &&
           path_join(probe, sizeof(probe), dir, "refs") && cbm_is_dir(probe);
}

/* The git directory a gitlink file (linked worktree, submodule) names, read
 * the way git's read_gitfile() reads it: "gitdir: <path>" with trailing
 * newlines dropped, a relative path taken from the directory holding the
 * file. The target must be a directory, and it is used as the git directory
 * itself — never read as a further gitlink. */
static bool gitlink_git_dir(const char *gitlink, const char *dir, char *out, size_t out_size) {
    char content[GIT_SMALL_FILE_MAX];
    if (!read_small_regular_file(gitlink, content, sizeof(content))) {
        return false;
    }
    trim_newlines(content);
    size_t prefix_len = strlen(GITLINK_PREFIX);
    if (strncmp(content, GITLINK_PREFIX, prefix_len) != 0 || !content[prefix_len]) {
        return false;
    }
    const char *target = content + prefix_len;
    bool joined = path_is_absolute(target) ? copy_whole(out, out_size, target)
                                           : path_join(out, out_size, dir, target);
    return joined && cbm_is_dir(out);
}

/* HEAD file of the git directory that a present `.git` entry stands for. */
static head_lookup_t dot_git_head_file(const char *dot_git, const char *dir,
                                       const cbm_path_info_t *info, char *head_path,
                                       size_t head_path_size) {
    bool git_dir_found;
    if (cbm_is_dir(dot_git)) { /* a directory, or a symlink to one */
        int n = snprintf(head_path, head_path_size, "%s", dot_git);
        git_dir_found = n > 0 && (size_t)n < head_path_size;
    } else {
        git_dir_found =
            info->is_regular && gitlink_git_dir(dot_git, dir, head_path, head_path_size);
    }
    return git_dir_found && path_append(head_path, head_path_size, "HEAD") ? HEAD_FILE_FOUND
                                                                           : HEAD_ASK_GIT;
}

/* The HEAD file of the work tree containing `path`, found the way git's
 * discovery finds the repository: from the canonical directory upward, the
 * first `.git` wins — a directory is the git directory, a regular file is a
 * gitlink followed exactly one level. A directory that is itself a git
 * directory (a bare repository, or a path inside .git) has no work tree, which
 * is where `git rev-parse --show-toplevel` fails as well. */
static head_lookup_t locate_head_file(const char *path, char *head_path, size_t head_path_size) {
    char dir[CBM_SZ_4K];
    if (!cbm_canonical_path(path, dir, sizeof(dir)) || !cbm_is_dir(dir)) {
        return HEAD_NO_BRANCH; /* a missing root reports no branch, as before */
    }
    for (int depth = 0; depth < GIT_DISCOVERY_DEPTH_MAX; depth++) {
        char dot_git[GIT_PROBE_PATH_MAX];
        if (!path_join(dot_git, sizeof(dot_git), dir, ".git")) {
            return HEAD_ASK_GIT;
        }
        cbm_path_info_t info;
        int probed = cbm_path_info_utf8(dot_git, &info);
        if (probed == CBM_PATH_INFO_OK) {
            return dot_git_head_file(dot_git, dir, &info, head_path, head_path_size);
        }
        if (probed != CBM_PATH_INFO_ABSENT) {
            return HEAD_ASK_GIT; /* .git is there but cannot be inspected */
        }
        if (dir_is_git_directory(dir) || !path_parent_in_place(dir)) {
            return HEAD_NO_BRANCH;
        }
    }
    return HEAD_ASK_GIT;
}

static bool text_is_printable(const char *s) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < ASCII_FIRST_PRINTABLE || c == ASCII_DEL) {
            return false;
        }
    }
    return true;
}

static bool ref_component_is_valid(const char *component, size_t len) {
    size_t lock_len = strlen(REF_LOCK_SUFFIX);
    bool locked =
        len >= lock_len && memcmp(component + len - lock_len, REF_LOCK_SUFFIX, lock_len) == 0;
    return len > 0 && component[0] != '.' && !locked;
}

/* A branch name git's check_refname_format() accepts under refs/heads/: no
 * control byte, DEL, space or ~^:?*[\, no ".." or "@{", no empty component,
 * none starting with '.' or ending in ".lock", no trailing '.', and not "@"
 * alone. The reftable stub "refs/heads/.invalid" fails on purpose: the real
 * HEAD of such a repository is not in this file. */
static bool branch_name_is_valid(const char *name) {
    size_t len = strlen(name);
    if (len == 0 || strcmp(name, "@") == 0 || name[len - 1] == '.') {
        return false;
    }
    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || name[i] == '/') {
            if (!ref_component_is_valid(name + start, i - start)) {
                return false;
            }
            start = i + 1;
            continue;
        }
        unsigned char c = (unsigned char)name[i];
        if (c < ASCII_FIRST_PRINTABLE || c == ASCII_DEL || strchr(REF_FORBIDDEN_BYTES, c) ||
            (c == '.' && name[i + 1] == '.') || (c == '@' && name[i + 1] == '{')) {
            return false;
        }
    }
    return true;
}

static bool is_object_id(const char *s) {
    size_t len = strlen(s);
    if (len != GIT_OID_HEX_SHA1 && len != GIT_OID_HEX_SHA256) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}

/* The branch a loose HEAD names. Only the two forms the files backend writes
 * are answered here — a symref into refs/heads/ and a detached object id;
 * everything else is git's to interpret. */
static head_lookup_t parse_head(char *content, char *out, size_t out_size) {
    trim_trailing_space(content);
    size_t symref_len = strlen(HEAD_SYMREF_PREFIX);
    if (strncmp(content, HEAD_SYMREF_PREFIX, symref_len) == 0) {
        const char *target = content + symref_len;
        while (is_ascii_space(*target)) {
            target++;
        }
        size_t heads_len = strlen(HEADS_PREFIX);
        if (strncmp(target, HEADS_PREFIX, heads_len) != 0 ||
            !branch_name_is_valid(target + heads_len)) {
            return HEAD_ASK_GIT;
        }
        return copy_whole(out, out_size, target + heads_len) ? HEAD_BRANCH_READ : HEAD_NO_BRANCH;
    }
    if (is_object_id(content)) {
        return copy_whole(out, out_size, GIT_DETACHED_BRANCH) ? HEAD_BRANCH_READ : HEAD_NO_BRANCH;
    }
    return HEAD_ASK_GIT;
}

/* One `git symbolic-ref` for the HEAD forms parse_head() leaves to git, read
 * with the meaning cbm_git_context_resolve() gives its outcome: a printed name
 * is the branch, "not a symbolic ref" is the detached marker, and any other
 * failure (no repository, no git) reports no branch. */
static bool head_branch_from_git(const char *path, char *out, size_t out_size) {
    char *branch = NULL;
    int exit_status = GIT_EXIT_UNKNOWN;
    if (git_capture_status(path, "symbolic-ref --quiet --short HEAD", &branch, &exit_status) == 0) {
        bool copied = text_is_printable(branch) && copy_whole(out, out_size, branch);
        free(branch);
        return copied;
    }
    return exit_status == GIT_SYMREF_NOT_SYMBOLIC && copy_whole(out, out_size, GIT_DETACHED_BRANCH);
}

bool cbm_git_head_branch(const char *path, char *out, size_t out_size) {
    if (!out || out_size == 0) {
        return false;
    }
    out[0] = '\0';
    if (!path || !path[0]) {
        return false;
    }
    char head_path[GIT_PROBE_PATH_MAX];
    head_lookup_t state = locate_head_file(path, head_path, sizeof(head_path));
    if (state == HEAD_FILE_FOUND) {
        char content[GIT_SMALL_FILE_MAX];
        state = read_small_regular_file(head_path, content, sizeof(content))
                    ? parse_head(content, out, out_size)
                    : HEAD_ASK_GIT;
    }
    if (state == HEAD_ASK_GIT) {
        return head_branch_from_git(path, out, out_size);
    }
    return state == HEAD_BRANCH_READ;
}

static char *join_root_relative(const char *root, const char *rel) {
    if (!root || !root[0]) {
        return git_strdup(rel);
    }
    int n = snprintf(NULL, 0, "%s/%s", root, rel);
    if (n < 0) {
        return NULL;
    }
    char *out = (char *)malloc((size_t)n + 1);
    if (!out) {
        return NULL;
    }
    snprintf(out, (size_t)n + 1, "%s/%s", root, rel);
    return out;
}

/* Derive the canonical repo root.
 *
 * Preferred (git 2.31+): abs_common_dir is `git rev-parse --path-format=absolute
 * --git-common-dir` — git's OWN absolute, canonical common-dir. Because a main
 * repo and its linked worktree both ask the same git binary, they resolve to the
 * IDENTICAL path, so canonical_root is consistent across worktrees AND platforms
 * with no manual join or realpath/_fullpath — which diverged under msys vs native
 * path representations (#659 root cause, and the worktree==main-root breakage in
 * test_pipeline.c git_context_linked_worktree). git also resolves the relative
 * ".." internally, so the subdirectory case (#659) is fixed here too.
 *
 * Fallback (git < 2.31, no --path-format → abs_common_dir empty): the relative
 * --git-common-dir is relative to input_path (the -C dir), so join against it and
 * realpath-normalize the "..". Unix only — on Windows git emits an absolute
 * common-dir so this branch isn't reached in practice, and _fullpath there
 * reintroduces the msys divergence. */
static char *derive_canonical_root(const char *input_path, const char *worktree_root,
                                   const char *git_common_dir, const char *abs_common_dir) {
    char *root = NULL;
    if (abs_common_dir && abs_common_dir[0] && path_is_absolute(abs_common_dir)) {
        root = git_strdup(abs_common_dir);
        if (!root) {
            return NULL;
        }
    } else {
        const char *src = git_common_dir && git_common_dir[0] ? git_common_dir : worktree_root;
        if (!src) {
            return git_strdup("");
        }
#ifndef _WIN32
        root = path_is_absolute(src) ? git_strdup(src) : join_root_relative(input_path, src);
        if (!root) {
            return NULL;
        }
        {
            char resolved[4096];
            if (realpath(root, resolved) != NULL) {
                free(root);
                root = git_strdup(resolved);
                if (!root) {
                    return NULL;
                }
            }
        }
#else
        (void)input_path;
        root = path_is_absolute(src) ? git_strdup(src) : join_root_relative(worktree_root, src);
        if (!root) {
            return NULL;
        }
#endif
    }

    size_t len = strlen(root);
    while (len > 1 && (root[len - 1] == '/' || root[len - 1] == '\\')) {
        root[--len] = '\0';
    }

    if (len >= 5 && strcmp(root + len - 5, "/.git") == 0) {
        root[len - 5] = '\0';
    }
#ifdef _WIN32
    else if (len >= 5 && strcmp(root + len - 5, "\\.git") == 0) {
        root[len - 5] = '\0';
    }
#endif

    return root;
}

static char *slug_from_branch(const char *branch, bool detached) {
    const char *fallback = detached ? "detached" : "working-tree";
    const char *src = detached ? fallback : (branch && branch[0] ? branch : fallback);
    size_t len = strlen(src);
    char *slug = (char *)malloc(len + 1);
    if (!slug) {
        return NULL;
    }

    size_t j = 0;
    bool in_dash = false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)src[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.') {
            if (j == 0 && c == '-') {
                in_dash = true;
                continue;
            }
            slug[j++] = (char)c;
            in_dash = false;
        } else if (j > 0 && !in_dash) {
            slug[j++] = '-';
            in_dash = true;
        }
    }
    while (j > 0 && slug[j - 1] == '-') {
        j--;
    }
    slug[j] = '\0';

    if (slug[0] == '\0') {
        free(slug);
        return git_strdup(fallback);
    }
    return slug;
}

void cbm_git_context_free(cbm_git_context_t *ctx) {
    if (!ctx) {
        return;
    }
    free(ctx->input_path);
    free(ctx->worktree_root);
    free(ctx->git_dir);
    free(ctx->git_common_dir);
    free(ctx->canonical_root);
    free(ctx->branch);
    free(ctx->branch_slug);
    free(ctx->head_sha);
    free(ctx->base_sha);
    memset(ctx, 0, sizeof(*ctx));
}

int cbm_git_context_resolve(const char *path, cbm_git_context_t *out) {
    if (!out) {
        return CBM_NOT_FOUND;
    }

    memset(out, 0, sizeof(*out));
    if (!path || !path[0]) {
        return CBM_NOT_FOUND;
    }

    out->input_path = git_strdup(path);
    if (!out->input_path) {
        return CBM_NOT_FOUND;
    }

    struct stat st;
    out->root_exists = (stat(path, &st) == 0);
    if (!out->root_exists) {
        return 0;
    }

    if (git_capture(path, "rev-parse --show-toplevel", &out->worktree_root) != 0) {
        out->is_git = false;
        return 0;
    }
    out->is_git = true;

    if (git_capture(path, "rev-parse --git-dir", &out->git_dir) != 0) {
        out->git_dir = git_strdup("");
    }
    if (git_capture(path, "rev-parse --git-common-dir", &out->git_common_dir) != 0) {
        out->git_common_dir = git_strdup("");
    }
    if (git_capture(path, "rev-parse --verify HEAD", &out->head_sha) != 0) {
        out->head_sha = git_strdup("");
    }

    if (git_capture(path, "symbolic-ref --quiet --short HEAD", &out->branch) != 0) {
        out->branch = git_strdup(GIT_DETACHED_BRANCH);
        out->is_detached = true;
    }

    out->is_worktree =
        out->git_dir && out->git_common_dir && strcmp(out->git_dir, out->git_common_dir) != 0;
    /* git 2.31+ canonical absolute common-dir (best-effort; NULL on older git,
     * where derive_canonical_root falls back to the relative common-dir). */
    char *abs_common_dir = NULL;
    (void)git_capture(path, "rev-parse --path-format=absolute --git-common-dir", &abs_common_dir);
    out->canonical_root =
        derive_canonical_root(path, out->worktree_root, out->git_common_dir, abs_common_dir);
    free(abs_common_dir);
    out->branch_slug = slug_from_branch(out->branch, out->is_detached);
    if (git_capture(path, "merge-base HEAD @{upstream}", &out->base_sha) != 0) {
        out->base_sha = git_strdup("");
    }

    if (!out->git_dir || !out->git_common_dir || !out->head_sha || !out->branch ||
        !out->canonical_root || !out->branch_slug || !out->base_sha) {
        cbm_git_context_free(out);
        return CBM_NOT_FOUND;
    }

    return 0;
}

char *cbm_git_context_branch_qn(const char *project_name, const cbm_git_context_t *ctx) {
    const char *project = project_name && project_name[0] ? project_name : "project";
    const char *slug = "working-tree";
    if (ctx) {
        if (ctx->is_detached) {
            slug = "detached";
        } else if (ctx->is_git && ctx->branch_slug && ctx->branch_slug[0]) {
            slug = ctx->branch_slug;
        }
    }

    int n = snprintf(NULL, 0, "%s.__branch__.%s", project, slug);
    if (n < 0) {
        return NULL;
    }
    char *out = (char *)malloc((size_t)n + 1);
    if (!out) {
        return NULL;
    }
    snprintf(out, (size_t)n + 1, "%s.__branch__.%s", project, slug);
    return out;
}

static bool append_fmt_checked(char *buf, int buf_size, int *off, const char *fmt, ...) {
    if (!buf || !off || buf_size <= 0 || *off < 0 || *off >= buf_size) {
        return false;
    }

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *off, (size_t)(buf_size - *off), fmt, ap);
    va_end(ap);
    if (n < 0 || n >= buf_size - *off) {
        buf[buf_size - 1] = '\0';
        return false;
    }
    *off += n;
    return true;
}

static int json_escaped_len(const char *src) {
    if (!src) {
        return 0;
    }
    int len = 0;
    for (int i = 0; src[i]; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\' || c == '\n' || c == '\r' || c == '\t') {
            len += 2;
        } else if (c < 0x20) {
            len += 6; /* \u00XX */
        } else {
            len++;
        }
    }
    return len;
}

static bool json_append_bool(char *buf, int buf_size, int *off, const char *name, bool value,
                             bool comma) {
    return append_fmt_checked(buf, buf_size, off, "\"%s\":%s%s", name, value ? "true" : "false",
                              comma ? "," : "");
}

static bool json_append_string(char *buf, int buf_size, int *off, const char *name,
                               const char *value, bool comma) {
    int needed = json_escaped_len(value ? value : "");
    char *escaped = malloc((size_t)needed + 1);
    if (!escaped) {
        return false;
    }
    int actual = cbm_json_escape(escaped, needed + 1, value ? value : "");
    bool ok = actual == needed && append_fmt_checked(buf, buf_size, off, "\"%s\":\"%s\"%s", name,
                                                     escaped, comma ? "," : "");
    free(escaped);
    return ok;
}

int cbm_git_context_props_json(const cbm_git_context_t *ctx, char *buf, int buf_size) {
    if (!ctx || !buf || buf_size <= 0) {
        return 0;
    }

    int off = 0;
    bool ok =
        append_fmt_checked(buf, buf_size, &off, "{") &&
        json_append_bool(buf, buf_size, &off, "is_git", ctx->is_git, true) &&
        json_append_bool(buf, buf_size, &off, "is_worktree", ctx->is_worktree, true) &&
        json_append_bool(buf, buf_size, &off, "is_detached", ctx->is_detached, true) &&
        json_append_bool(buf, buf_size, &off, "root_exists", ctx->root_exists, true) &&
        json_append_string(buf, buf_size, &off, "canonical_root", ctx->canonical_root, true) &&
        json_append_string(buf, buf_size, &off, "worktree_root", ctx->worktree_root, true) &&
        json_append_string(buf, buf_size, &off, "git_common_dir", ctx->git_common_dir, true) &&
        json_append_string(buf, buf_size, &off, "branch", ctx->branch, true) &&
        json_append_string(buf, buf_size, &off, "head_sha", ctx->head_sha, true) &&
        json_append_string(buf, buf_size, &off, "base_sha", ctx->base_sha, false) &&
        append_fmt_checked(buf, buf_size, &off, "}");
    if (!ok) {
        if (buf_size > 0) {
            buf[0] = '\0';
        }
        return 0;
    }
    return off;
}

/* ── Bounded worktree status ──────────────────────────────────────────────
 * git status --porcelain=v1 -z --untracked-files=all is parsed record by
 * record, NUL-delimited, over a bounded chunk buffer. The full stream is
 * counted but only max_samples paths per class (tracked / untracked) are
 * ever retained, so a huge dirty tree cannot grow memory. Rename/copy
 * records carry a second NUL-terminated path (the source); it is consumed
 * without being counted so one rename counts once. */

typedef struct {
    cbm_worktree_status_t *out;
    int max_samples;
    bool expecting_source; /* next field is the source path of an R/C record */
    bool parse_ok;
    char partial[STATUS_FIELD_MAX];
    size_t partial_len;
} status_parser_t;

/* porcelain-v1 status columns (X/Y): documented codes plus the lowercase
 * submodule variants and type-change 'T'. Anything else is not a record
 * header, so a malformed stream fails closed instead of being misread. */
static bool status_code_ok(char c) {
    switch (c) {
        case ' ':
        case 'M':
        case 'A':
        case 'D':
        case 'R':
        case 'C':
        case 'U':
        case '?':
        case '!':
        case 'T':
        case 'm':
        case 'a':
        case 'd':
        case 'r':
        case 'c':
        case 'u':
        case 't':
            return true;
        default:
            return false;
    }
}

/* Retain one bounded sample; the running total always advances so the caller
 * sees the true stream count even when samples are capped or OOM. */
static void status_add_sample(int *total, char ***samples, int *sample_count,
                              bool *truncated, int max_samples, const char *path,
                              size_t path_len) {
    (*total)++;
    if (*sample_count >= max_samples) {
        *truncated = true;
        return;
    }
    char *copy = (char *)malloc(path_len + 1);
    if (!copy) {
        *truncated = true;
        return;
    }
    memcpy(copy, path, path_len);
    copy[path_len] = '\0';
    char **grown = (char **)realloc(*samples, (size_t)(*sample_count + 1) * sizeof(char *));
    if (!grown) {
        free(copy);
        *truncated = true;
        return;
    }
    *samples = grown;
    (*samples)[*sample_count] = copy;
    (*sample_count)++;
}

/* Parse one complete NUL-delimited field. Returns false on malformed input,
 * which fails the whole snapshot closed. */
static bool status_parse_record(cbm_worktree_status_t *out, status_parser_t *parser,
                                const char *field, size_t field_len) {
    if (parser->expecting_source) {
        parser->expecting_source = false;
        return true; /* second path of a rename/copy record — not counted */
    }
    if (field_len < STATUS_HEADER_LEN || field[2] != ' ' ||
        !status_code_ok(field[0]) || !status_code_ok(field[1])) {
        return false;
    }
    const char *path = field + STATUS_HEADER_LEN;
    size_t path_len = field_len - STATUS_HEADER_LEN;
    if (field[0] == '?' && field[1] == '?') {
        status_add_sample(&out->untracked_count, &out->untracked_paths,
                          &out->untracked_sample_count, &out->untracked_truncated,
                          parser->max_samples, path, path_len);
        return true;
    }
    if (field[0] == 'R' || field[0] == 'C' || field[1] == 'R' || field[1] == 'C') {
        parser->expecting_source = true;
    }
    status_add_sample(&out->tracked_count, &out->tracked_paths,
                      &out->tracked_sample_count, &out->tracked_truncated,
                      parser->max_samples, path, path_len);
    return true;
}

/* Feed a chunk of the NUL-delimited stream through the bounded record parser. */
static void status_feed(status_parser_t *parser, const char *data, size_t data_len) {
    size_t i = 0;
    while (i < data_len) {
        if (parser->partial_len >= STATUS_FIELD_MAX) {
            parser->parse_ok = false; /* field exceeded the bound: malformed */
            return;
        }
        size_t j = i;
        while (j < data_len && data[j] != '\0' &&
               parser->partial_len + (j - i) < STATUS_FIELD_MAX) {
            j++;
        }
        size_t take = j - i;
        memcpy(parser->partial + parser->partial_len, data + i, take);
        parser->partial_len += take;
        i += take;
        if (i < data_len && data[i] == '\0') {
            if (!status_parse_record(parser->out, parser, parser->partial,
                                     parser->partial_len)) {
                parser->parse_ok = false;
                return;
            }
            parser->partial_len = 0;
            i++; /* consume the record terminator */
        } else if (i >= data_len) {
            return; /* chunk exhausted mid-record */
        }
    }
}

int cbm_git_worktree_status(const char *validated_root, int max_samples,
                            cbm_worktree_status_t *out) {
    if (!out) {
        return CBM_NOT_FOUND;
    }
    memset(out, 0, sizeof(*out));
    if (!validated_root || !validated_root[0] || max_samples < 0) {
        return CBM_NOT_FOUND;
    }
    if (!git_validate_repo_path(validated_root)) {
        return CBM_NOT_FOUND;
    }

    char cmd[GIT_CMD_MAX];
#ifdef _WIN32
    const char *null_dev = "NUL";
#else
    const char *null_dev = "/dev/null";
#endif
    int n = snprintf(cmd, sizeof(cmd),
                     "git -C \"%s\" status --porcelain=v1 -z --untracked-files=all 2>%s",
                     validated_root, null_dev);
    if (n < 0 || n >= (int)sizeof(cmd)) {
        return CBM_NOT_FOUND;
    }

    FILE *fp = cbm_popen(cmd, "r");
    if (!fp) {
        return 0; /* out is zeroed: available=false */
    }

    status_parser_t parser;
    memset(&parser, 0, sizeof(parser));
    parser.out = out;
    parser.max_samples = max_samples;
    parser.parse_ok = true;

    char chunk[STATUS_CHUNK_MAX];
    size_t got;
    bool io_error = false;
    while (parser.parse_ok && (got = fread(chunk, 1, sizeof(chunk), fp)) > 0) {
        status_feed(&parser, chunk, got);
    }
    if (ferror(fp)) {
        io_error = true;
    }
    int rc = cbm_pclose(fp);

    if (rc != 0 || !parser.parse_ok || io_error || parser.partial_len > 0) {
        /* Fail closed: any deviation from a clean, complete status stream is a
         * machine-readable "unavailable", never a clean worktree. */
        cbm_git_worktree_status_free(out);
        return 0;
    }

    out->available = true;
    return 0;
}

void cbm_git_worktree_status_free(cbm_worktree_status_t *st) {
    if (!st) {
        return;
    }
    for (int i = 0; i < st->tracked_sample_count; i++) {
        free(st->tracked_paths[i]);
    }
    free(st->tracked_paths);
    for (int i = 0; i < st->untracked_sample_count; i++) {
        free(st->untracked_paths[i]);
    }
    free(st->untracked_paths);
    memset(st, 0, sizeof(*st));
}
