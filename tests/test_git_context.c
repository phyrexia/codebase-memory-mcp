/*
 * test_git_context.c — Tests for cbm_git_context_resolve(), focusing on
 * the canonical_root derivation for git worktrees and subdirectory projects.
 *
 * Issue #659: canonical_root was computed incorrectly for linked worktrees
 * and projects indexed from a subdirectory of the repository root.
 * git rev-parse --git-common-dir outputs a path relative to the -C directory
 * (input_path), not to worktree_root. Joining it with worktree_root and then
 * string-stripping "/.git" left unresolved ".." components in the result.
 *
 * These tests shell out to `git`, so the canonical_root tests SKIP_PLATFORM on
 * Windows (the CI shell there cannot init a repo via system()). The bounded
 * worktree-status tests below shell out through cbm_popen instead, so they run
 * on every platform that has git on PATH.
 *
 * Reproduce-first guard: canonical_root_subdir is the genuine RED-without-the-fix
 * guard — a repo indexed from a subdirectory yields a relative --git-common-dir
 * ("../.git"), so the unfixed code returns an un-normalized "<root>/subdir/.."
 * (verified FAIL on the unfixed derive_canonical_root; GREEN with the realpath
 * normalization). canonical_root_linked_worktree is a SUPPORTING INVARIANT, not
 * the #659 reproducer: on git that emits an *absolute* --git-common-dir for a
 * linked worktree (e.g. 2.48.x) the bug does not manifest there, so that test
 * passes with or without the fix. It still enforces the worktree->main-root
 * invariant and would catch the bug on git builds that emit a relative
 * worktree common-dir. canonical_root_repo_root is a baseline (no `..` to
 * normalize), not a guard.
 */
#include "test_framework.h"
#include "test_helpers.h"
#include "git/git_context.h"

#include <stdio.h>
#include <string.h>

#ifndef _WIN32
#include <limits.h>
#include <unistd.h> /* symlink */
#endif

/* These helpers shell out to git and are only used by the non-Windows test
 * bodies below; on Windows every test SKIP_PLATFORMs, so guard them here too or
 * they'd be unused-static functions and fail the -Werror build. */
#ifndef _WIN32
/* Run a git command inside dir, return 0 on success. */
static int git_run(const char *dir, const char *args) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "git -C \"%s\" %s >/dev/null 2>&1", dir, args);
    return system(cmd);
}

/* Create a minimal git repo at dir (init + empty commit so HEAD exists). */
static int make_git_repo(const char *dir) {
    if (th_mkdir_p(dir) != 0) return -1;
    if (git_run(dir, "init -q") != 0) return -1;
    if (git_run(dir, "config user.email test@example.com") != 0) return -1;
    if (git_run(dir, "config user.name Test") != 0) return -1;
    /* Create a file so HEAD points to a real commit. */
    char path[1024];
    snprintf(path, sizeof(path), "%s/.keep", dir);
    th_write_file(path, "");
    if (git_run(dir, "add .keep") != 0) return -1;
    if (git_run(dir, "commit -q -m init") != 0) return -1;
    return 0;
}
#endif /* _WIN32 */

/* ── canonical_root: normal repo indexed from its root ──────────── */

TEST(canonical_root_repo_root) {
#ifdef _WIN32
    SKIP_PLATFORM("git-based canonical_root test not supported on Windows CI");
#else
    char *tmp = th_mktempdir("cbm_gitctx");
    if (!tmp) FAIL("th_mktempdir returned NULL");

    if (make_git_repo(tmp) != 0) {
        th_rmtree(tmp);
        SKIP_PLATFORM("git not available to init a repo");
    }

    cbm_git_context_t ctx = {0};
    int rc = cbm_git_context_resolve(tmp, &ctx);
    if (rc != 0 || !ctx.is_git) {
        cbm_git_context_free(&ctx);
        th_rmtree(tmp);
        FAIL("cbm_git_context_resolve failed or not a git repo");
    }

    char expected[4096];
    if (realpath(tmp, expected) == NULL) {
        cbm_git_context_free(&ctx);
        th_rmtree(tmp);
        FAIL("realpath(tmp) failed");
    }

    ASSERT_STR_EQ(ctx.canonical_root, expected);

    cbm_git_context_free(&ctx);
    th_rmtree(tmp);
    PASS();
#endif /* _WIN32 */
}

/* ── canonical_root: indexed from a subdirectory (issue #659) ─────
 * THE reproduce-first guard: from a subdir, --git-common-dir is relative, so the
 * unfixed derive_canonical_root joins it against worktree_root and strips "/.git"
 * textually, leaving canonical_root = "<root>/subdir/.." (or "<root>/..") instead
 * of "<root>". Verified RED on the unfixed code, GREEN with the realpath fix. */

TEST(canonical_root_subdir) {
#ifdef _WIN32
    SKIP_PLATFORM("git-based canonical_root test not supported on Windows CI");
#else
    char *tmp = th_mktempdir("cbm_gitctx");
    if (!tmp) FAIL("th_mktempdir returned NULL");

    if (make_git_repo(tmp) != 0) {
        th_rmtree(tmp);
        SKIP_PLATFORM("git not available to init a repo");
    }

    /* Create a subdirectory inside the repo. */
    char subdir[1024];
    snprintf(subdir, sizeof(subdir), "%s/scripts", tmp);
    if (th_mkdir_p(subdir) != 0) {
        th_rmtree(tmp);
        FAIL("failed to create subdir");
    }

    cbm_git_context_t ctx = {0};
    int rc = cbm_git_context_resolve(subdir, &ctx);
    if (rc != 0 || !ctx.is_git) {
        cbm_git_context_free(&ctx);
        th_rmtree(tmp);
        FAIL("cbm_git_context_resolve on subdir failed or not a git repo");
    }

    /* canonical_root must equal the repo root, NOT "<repo>/.." or "<subdir>/..". */
    char expected[4096];
    if (realpath(tmp, expected) == NULL) {
        cbm_git_context_free(&ctx);
        th_rmtree(tmp);
        FAIL("realpath(tmp) failed");
    }

    ASSERT_STR_EQ(ctx.canonical_root, expected);

    /* Sanity: canonical_root must not contain ".." or end with a slash. */
    ASSERT(strstr(ctx.canonical_root, "..") == NULL);
    ASSERT(ctx.canonical_root[strlen(ctx.canonical_root) - 1] != '/');

    cbm_git_context_free(&ctx);
    th_rmtree(tmp);
    PASS();
#endif /* _WIN32 */
}

/* ── canonical_root: linked git worktree (supporting invariant) ────
 * NOT the #659 reproducer on modern git: git that emits an *absolute*
 * --git-common-dir for a linked worktree (e.g. 2.48.x) takes the path_is_absolute
 * branch, so the bug does not manifest and this passes with or without the fix.
 * It is kept as an invariant — canonical_root of a linked worktree must equal the
 * MAIN repo root (never the worktree root or its parent) — and would fail on a git
 * build that emits a *relative* worktree common-dir. The genuine RED-without-fix
 * guard for #659 is canonical_root_subdir above. */

TEST(canonical_root_linked_worktree) {
#ifdef _WIN32
    SKIP_PLATFORM("git worktree test not implemented for Windows");
#else
    /* th_mktempdir() returns a static buffer — copy before the second call. */
    char main_tmp[256];
    char *raw = th_mktempdir("cbm_main");
    if (!raw) FAIL("th_mktempdir returned NULL");
    strncpy(main_tmp, raw, sizeof(main_tmp) - 1);
    main_tmp[sizeof(main_tmp) - 1] = '\0';

    char wt_tmp[256];
    raw = th_mktempdir("cbm_worktree");
    if (!raw) FAIL("th_mktempdir returned NULL");
    strncpy(wt_tmp, raw, sizeof(wt_tmp) - 1);
    wt_tmp[sizeof(wt_tmp) - 1] = '\0';

    /* Remove the worktree dir first — git worktree add creates it. */
    th_rmtree(wt_tmp);

    if (make_git_repo(main_tmp) != 0) {
        th_rmtree(main_tmp);
        SKIP_PLATFORM("git not available to init a repo");
    }

    /* Create a branch for the worktree. */
    if (git_run(main_tmp, "branch wt-branch") != 0) {
        th_rmtree(main_tmp);
        FAIL("failed to create branch for worktree");
    }

    /* Add a linked worktree. */
    char wt_cmd[1024];
    snprintf(wt_cmd, sizeof(wt_cmd), "worktree add \"%s\" wt-branch", wt_tmp);
    if (git_run(main_tmp, wt_cmd) != 0) {
        th_rmtree(wt_tmp);
        th_rmtree(main_tmp);
        SKIP_PLATFORM("git worktree add unavailable (git 2.5+ required)");
    }

    cbm_git_context_t ctx = {0};
    int rc = cbm_git_context_resolve(wt_tmp, &ctx);
    if (rc != 0 || !ctx.is_git) {
        cbm_git_context_free(&ctx);
        git_run(main_tmp, "worktree prune");
        th_rmtree(main_tmp);
        th_rmtree(wt_tmp);
        FAIL("cbm_git_context_resolve on linked worktree failed");
    }

    /* canonical_root must be the MAIN repo root, not the worktree root or its parent. */
    char expected[4096];
    if (realpath(main_tmp, expected) == NULL) {
        cbm_git_context_free(&ctx);
        git_run(main_tmp, "worktree prune");
        th_rmtree(main_tmp);
        th_rmtree(wt_tmp);
        FAIL("realpath(main_tmp) failed");
    }

    ASSERT_STR_EQ(ctx.canonical_root, expected);
    ASSERT(strstr(ctx.canonical_root, "..") == NULL);

    cbm_git_context_free(&ctx);
    git_run(main_tmp, "worktree prune");
    th_rmtree(main_tmp);
    th_rmtree(wt_tmp);
    PASS();
#endif /* _WIN32 */
}

/* ── Bounded worktree status (BT-240 increment 3) ──────────────────────
 * These run on EVERY platform, not just POSIX: they shell out through
 * cbm_popen (the same isolated spawn production git_context uses) instead of
 * system(), so they work whenever git is on PATH — Windows CI included. The
 * core status parser is platform-neutral and must be exercised everywhere git
 * is available; only a genuinely missing git skips. */

static int status_git_run(const char *dir, const char *args) {
    char cmd[1024];
#ifdef _WIN32
    const char *null_dev = "NUL";
#else
    const char *null_dev = "/dev/null";
#endif
    snprintf(cmd, sizeof(cmd), "git -C \"%s\" %s 2>%s", dir, args, null_dev);
    FILE *fp = cbm_popen(cmd, "r");
    if (!fp) {
        return -1;
    }
    char drain[256];
    while (fgets(drain, sizeof(drain), fp)) {
    }
    return cbm_pclose(fp);
}

static int status_make_repo(const char *dir) {
    if (th_mkdir_p(dir) != 0) return -1;
    if (status_git_run(dir, "init -q") != 0) return -1;
    if (status_git_run(dir, "config user.email test@example.com") != 0) return -1;
    if (status_git_run(dir, "config user.name Test") != 0) return -1;
    if (th_write_file(TH_PATH(dir, ".keep"), "") != 0) return -1;
    if (status_git_run(dir, "add -A") != 0) return -1;
    if (status_git_run(dir, "commit -qm init") != 0) return -1;
    return 0;
}

TEST(worktree_status_clean) {
    char *tmp = th_mktempdir("cbm_wtstatus");
    if (!tmp) FAIL("th_mktempdir returned NULL");

    if (status_make_repo(tmp) != 0) {
        th_rmtree(tmp);
        SKIP_PLATFORM("git not available to init a repo");
    }

    cbm_worktree_status_t st = {0};
    int rc = cbm_git_worktree_status(tmp, 8, &st);
    if (rc != 0 || !st.available) {
        cbm_git_worktree_status_free(&st);
        th_rmtree(tmp);
        FAIL("clean repo status not available");
    }
    ASSERT_EQ(st.tracked_count, 0);
    ASSERT_EQ(st.untracked_count, 0);
    ASSERT_EQ(st.tracked_sample_count, 0);
    ASSERT_EQ(st.untracked_sample_count, 0);
    ASSERT_FALSE(st.tracked_truncated);
    ASSERT_FALSE(st.untracked_truncated);

    cbm_git_worktree_status_free(&st);
    th_rmtree(tmp);
    PASS();
}

TEST(worktree_status_tracked_modified) {
    char *tmp = th_mktempdir("cbm_wtstatus");
    if (!tmp) FAIL("th_mktempdir returned NULL");

    if (status_make_repo(tmp) != 0) {
        th_rmtree(tmp);
        SKIP_PLATFORM("git not available to init a repo");
    }
    if (th_write_file(TH_PATH(tmp, "tracked.txt"), "v1") != 0 ||
        status_git_run(tmp, "add -f -- tracked.txt") != 0 ||
        status_git_run(tmp, "commit -q -m add-tracked") != 0) {
        th_rmtree(tmp);
        FAIL("failed to commit tracked file");
    }
    if (th_write_file(TH_PATH(tmp, "tracked.txt"), "v2") != 0) {
        th_rmtree(tmp);
        FAIL("failed to modify tracked file");
    }

    cbm_worktree_status_t st = {0};
    int rc = cbm_git_worktree_status(tmp, 8, &st);
    if (rc != 0 || !st.available) {
        cbm_git_worktree_status_free(&st);
        th_rmtree(tmp);
        FAIL("status not available");
    }
    ASSERT_EQ(st.tracked_count, 1);
    ASSERT_EQ(st.untracked_count, 0);
    ASSERT_EQ(st.tracked_sample_count, 1);
    ASSERT_STR_EQ(st.tracked_paths[0], "tracked.txt");
    ASSERT_FALSE(st.tracked_truncated);

    cbm_git_worktree_status_free(&st);
    th_rmtree(tmp);
    PASS();
}

TEST(worktree_status_untracked) {
    char *tmp = th_mktempdir("cbm_wtstatus");
    if (!tmp) FAIL("th_mktempdir returned NULL");

    if (status_make_repo(tmp) != 0) {
        th_rmtree(tmp);
        SKIP_PLATFORM("git not available to init a repo");
    }
    if (th_write_file(TH_PATH(tmp, "newfile.txt"), "n") != 0) {
        th_rmtree(tmp);
        FAIL("failed to create untracked file");
    }

    cbm_worktree_status_t st = {0};
    int rc = cbm_git_worktree_status(tmp, 8, &st);
    if (rc != 0 || !st.available) {
        cbm_git_worktree_status_free(&st);
        th_rmtree(tmp);
        FAIL("status not available");
    }
    ASSERT_EQ(st.untracked_count, 1);
    ASSERT_EQ(st.tracked_count, 0);
    ASSERT_EQ(st.untracked_sample_count, 1);
    ASSERT_STR_EQ(st.untracked_paths[0], "newfile.txt");
    ASSERT_FALSE(st.untracked_truncated);

    cbm_git_worktree_status_free(&st);
    th_rmtree(tmp);
    PASS();
}

TEST(worktree_status_rename_counts_once) {
    char *tmp = th_mktempdir("cbm_wtstatus");
    if (!tmp) FAIL("th_mktempdir returned NULL");

    if (status_make_repo(tmp) != 0) {
        th_rmtree(tmp);
        SKIP_PLATFORM("git not available to init a repo");
    }
    if (th_write_file(TH_PATH(tmp, "old.txt"), "o") != 0 ||
        th_write_file(TH_PATH(tmp, "other.txt"), "x") != 0 ||
        status_git_run(tmp, "add -A") != 0 ||
        status_git_run(tmp, "commit -qm base") != 0 ||
        status_git_run(tmp, "mv old.txt new.txt") != 0) {
        th_rmtree(tmp);
        FAIL("failed to stage a rename");
    }

    cbm_worktree_status_t st = {0};
    int rc = cbm_git_worktree_status(tmp, 8, &st);
    if (rc != 0 || !st.available) {
        cbm_git_worktree_status_free(&st);
        th_rmtree(tmp);
        FAIL("status not available");
    }
    ASSERT_EQ(st.tracked_count, 1); /* rename/copy counts ONCE, not twice */
    ASSERT_EQ(st.tracked_sample_count, 1);
    ASSERT_STR_EQ(st.tracked_paths[0], "new.txt"); /* sampled NEW path */
    ASSERT_EQ(st.untracked_count, 0);

    cbm_git_worktree_status_free(&st);
    th_rmtree(tmp);
    PASS();
}

TEST(worktree_status_sample_cap_truncates) {
    char *tmp = th_mktempdir("cbm_wtstatus");
    if (!tmp) FAIL("th_mktempdir returned NULL");

    if (status_make_repo(tmp) != 0) {
        th_rmtree(tmp);
        SKIP_PLATFORM("git not available to init a repo");
    }
    enum { N_FILES = 5, SAMPLE_MAX = 2 };
    for (int i = 0; i < N_FILES; i++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/u%d.txt", tmp, i);
        if (th_write_file(path, "u") != 0) {
            th_rmtree(tmp);
            FAIL("failed to create untracked file");
        }
    }

    cbm_worktree_status_t st = {0};
    int rc = cbm_git_worktree_status(tmp, SAMPLE_MAX, &st);
    if (rc != 0 || !st.available) {
        cbm_git_worktree_status_free(&st);
        th_rmtree(tmp);
        FAIL("status not available");
    }
    ASSERT_EQ(st.untracked_count, N_FILES); /* full stream counted... */
    ASSERT_EQ(st.untracked_sample_count, SAMPLE_MAX); /* ...samples bounded */
    ASSERT_TRUE(st.untracked_truncated);
    ASSERT_EQ(st.tracked_count, 0);
    ASSERT_FALSE(st.tracked_truncated);

    cbm_git_worktree_status_free(&st);
    th_rmtree(tmp);
    PASS();
}

TEST(worktree_status_mixed_tracked_and_untracked) {
    char *tmp = th_mktempdir("cbm_wtstatus");
    if (!tmp) FAIL("th_mktempdir returned NULL");

    if (status_make_repo(tmp) != 0) {
        th_rmtree(tmp);
        SKIP_PLATFORM("git not available to init a repo");
    }
    if (th_write_file(TH_PATH(tmp, "tracked.txt"), "v1") != 0 ||
        status_git_run(tmp, "add -f -- tracked.txt") != 0 ||
        status_git_run(tmp, "commit -q -m add-tracked") != 0) {
        th_rmtree(tmp);
        FAIL("failed to commit tracked file");
    }
    if (th_write_file(TH_PATH(tmp, "tracked.txt"), "v2") != 0 ||
        th_write_file(TH_PATH(tmp, "untracked.txt"), "n") != 0) {
        th_rmtree(tmp);
        FAIL("failed to prepare mixed worktree");
    }

    cbm_worktree_status_t st = {0};
    int rc = cbm_git_worktree_status(tmp, 8, &st);
    if (rc != 0 || !st.available) {
        cbm_git_worktree_status_free(&st);
        th_rmtree(tmp);
        FAIL("status not available");
    }
    ASSERT_EQ(st.tracked_count, 1);
    ASSERT_EQ(st.untracked_count, 1);
    ASSERT_STR_EQ(st.tracked_paths[0], "tracked.txt");
    ASSERT_STR_EQ(st.untracked_paths[0], "untracked.txt");

    cbm_git_worktree_status_free(&st);
    th_rmtree(tmp);
    PASS();
}

/* Fail-closed contract: a non-repo directory must report unavailable with
 * zero counts — never a silent "clean" tree. Does not need git on PATH. */
TEST(worktree_status_unavailable_non_repo) {
    char *tmp = th_mktempdir("cbm_wtstatus");
    if (!tmp) FAIL("th_mktempdir returned NULL");

    cbm_worktree_status_t st = {0};
    int rc = cbm_git_worktree_status(tmp, 8, &st);
    if (rc != 0) {
        th_rmtree(tmp);
        FAIL("status call returned an error");
    }
    ASSERT_FALSE(st.available);
    ASSERT_EQ(st.tracked_count, 0);
    ASSERT_EQ(st.untracked_count, 0);
    ASSERT_EQ(st.tracked_sample_count, 0);
    ASSERT_EQ(st.untracked_sample_count, 0);

    cbm_git_worktree_status_free(&st);
    th_rmtree(tmp);
    PASS();
}

/* ── cbm_git_is_linked_worktree ─────────────────────────────────── */
/* Detection backing the `ignore_worktrees` config gate. The predicate must be
 * TRUE only for a linked worktree (`git worktree add`) — never for the main
 * checkout, a plain directory, or a submodule. The submodule case is the one a
 * naive "is .git a regular file?" check gets wrong: a submodule's .git is also a
 * gitlink file, and it is separated here by the absence of a `commondir` entry
 * in the pointed-at gitdir. */

TEST(is_linked_worktree_true_for_linked_worktree) {
#ifdef _WIN32
    SKIP_PLATFORM("git worktree test not implemented for Windows");
#else
    /* th_mktempdir() returns a static buffer — copy before the second call. */
    char main_tmp[256];
    char *raw = th_mktempdir("cbm_wt_main");
    if (!raw) FAIL("th_mktempdir returned NULL");
    strncpy(main_tmp, raw, sizeof(main_tmp) - 1);
    main_tmp[sizeof(main_tmp) - 1] = '\0';

    char wt_tmp[256];
    raw = th_mktempdir("cbm_wt_linked");
    if (!raw) FAIL("th_mktempdir returned NULL");
    strncpy(wt_tmp, raw, sizeof(wt_tmp) - 1);
    wt_tmp[sizeof(wt_tmp) - 1] = '\0';
    th_rmtree(wt_tmp); /* git worktree add creates it */

    if (make_git_repo(main_tmp) != 0) {
        th_rmtree(main_tmp);
        SKIP_PLATFORM("git not available to init a repo");
    }
    if (git_run(main_tmp, "branch wt-branch") != 0) {
        th_rmtree(main_tmp);
        FAIL("failed to create branch for worktree");
    }
    char wt_cmd[1024];
    snprintf(wt_cmd, sizeof(wt_cmd), "worktree add \"%s\" wt-branch", wt_tmp);
    if (git_run(main_tmp, wt_cmd) != 0) {
        th_rmtree(wt_tmp);
        th_rmtree(main_tmp);
        SKIP_PLATFORM("git worktree add unavailable (git 2.5+ required)");
    }

    bool worktree_detected = cbm_git_is_linked_worktree(wt_tmp);
    /* The MAIN checkout of the very same repo must NOT be flagged — otherwise
     * enabling ignore_worktrees would stop indexing ordinary repositories. */
    bool main_detected = cbm_git_is_linked_worktree(main_tmp);

    git_run(main_tmp, "worktree prune");
    th_rmtree(main_tmp);
    th_rmtree(wt_tmp);

    ASSERT(worktree_detected);
    ASSERT(!main_detected);
    PASS();
#endif /* _WIN32 */
}

TEST(is_linked_worktree_false_for_submodule_and_nongit) {
#ifdef _WIN32
    SKIP_PLATFORM("git worktree test not implemented for Windows");
#else
    char super_tmp[256];
    char *raw = th_mktempdir("cbm_wt_super");
    if (!raw) FAIL("th_mktempdir returned NULL");
    strncpy(super_tmp, raw, sizeof(super_tmp) - 1);
    super_tmp[sizeof(super_tmp) - 1] = '\0';

    char child_tmp[256];
    raw = th_mktempdir("cbm_wt_child");
    if (!raw) FAIL("th_mktempdir returned NULL");
    strncpy(child_tmp, raw, sizeof(child_tmp) - 1);
    child_tmp[sizeof(child_tmp) - 1] = '\0';

    if (make_git_repo(super_tmp) != 0 || make_git_repo(child_tmp) != 0) {
        th_rmtree(super_tmp);
        th_rmtree(child_tmp);
        SKIP_PLATFORM("git not available to init a repo");
    }

    /* A plain directory that is not a git repo at all. */
    char plain[1024];
    snprintf(plain, sizeof(plain), "%s/plain", super_tmp);
    if (th_mkdir_p(plain) != 0) {
        th_rmtree(super_tmp);
        th_rmtree(child_tmp);
        FAIL("failed to create plain dir");
    }
    bool plain_detected = cbm_git_is_linked_worktree(plain);

    /* file:// submodules are refused by default since the CVE-2022-39253 fix. */
    char sub_cmd[1024];
    snprintf(sub_cmd, sizeof(sub_cmd),
             "-c protocol.file.allow=always submodule add -q \"%s\" subm", child_tmp);
    int sub_rc = git_run(super_tmp, sub_cmd);

    char subm[1024];
    snprintf(subm, sizeof(subm), "%s/subm", super_tmp);
    bool submodule_detected = sub_rc == 0 && cbm_git_is_linked_worktree(subm);

    th_rmtree(super_tmp);
    th_rmtree(child_tmp);

    ASSERT(!plain_detected);
    if (sub_rc != 0) {
        SKIP_PLATFORM("git submodule add unavailable in this environment");
    }
    /* A submodule gitlink has no commondir → must not be treated as a worktree. */
    ASSERT(!submodule_detected);
    PASS();
#endif /* _WIN32 */
}

/* ── cbm_git_head_branch ────────────────────────────────────────── */
/* The branch list_projects detail=stats reports, read from HEAD instead of
 * seven git processes. Fixtures are hand-made git directories — HEAD plus the
 * empty objects/ and refs/heads/ of a real one, no commits — so a branch is
 * knowable only by reading HEAD, and a fake git first on PATH counts (and, for
 * the fallback, answers) every process the helper spawns. The last test checks
 * the helper against cbm_git_context_resolve() on real repositories. */

#ifndef _WIN32
static bool hb_git_dir(const char *base, const char *rel, const char *head) {
    char dir[1024];
    char path[1200];
    snprintf(dir, sizeof(dir), "%s/%s", base, rel);
    snprintf(path, sizeof(path), "%s/objects", dir);
    if (th_mkdir_p(path) != 0) {
        return false;
    }
    snprintf(path, sizeof(path), "%s/refs/heads", dir);
    if (th_mkdir_p(path) != 0) {
        return false;
    }
    snprintf(path, sizeof(path), "%s/HEAD", dir);
    return th_write_file(path, head) == 0;
}

static bool hb_file(const char *base, const char *rel, const char *content) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", base, rel);
    return th_write_file(path, content) == 0;
}

/* Raw bytes, for content th_write_file() cannot carry (an embedded NUL). */
static bool hb_bytes(const char *base, const char *rel, const char *bytes, size_t len) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", base, rel);
    FILE *f = fopen(path, "wb");
    bool ok = f && fwrite(bytes, 1, len, f) == len;
    if (f) {
        ok = fclose(f) == 0 && ok;
    }
    return ok;
}

static bool hb_dir(const char *base, const char *rel) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", base, rel);
    return th_mkdir_p(path) == 0;
}

/* What cbm_git_head_branch() reports for <base>/<rel>, as text: the branch,
 * "(none)", or a marker when a false return left `out` non-empty. */
static void hb_query(const char *base, const char *rel, char *got, size_t got_size) {
    char root[1024];
    snprintf(root, sizeof(root), "%s/%s", base, rel);
    char out[256];
    memset(out, 'Z', sizeof(out));
    bool found = cbm_git_head_branch(root, out, sizeof(out));
    snprintf(got, got_size, "%s", found ? out : out[0] ? "(none, out not emptied)" : "(none)");
}

static bool hb_make_tempdir(char *out, size_t out_size, const char *prefix) {
    char *raw = th_mktempdir(prefix);
    if (!raw) {
        return false;
    }
    snprintf(out, out_size, "%s", raw);
    return true;
}
#endif /* _WIN32 */

TEST(head_branch_reads_loose_head_without_git) {
#ifdef _WIN32
    SKIP_PLATFORM("the fake git on PATH is a /bin/sh script");
#else
    char base[256];
    if (!hb_make_tempdir(base, sizeof(base), "cbm_headbr")) {
        FAIL("th_mktempdir returned NULL");
    }

    char text[1024];
    char link_path[1024];
    char link_target[1024];
    snprintf(text, sizeof(text), "gitdir: %s/main/.git/modules/sub\n", base);
    snprintf(link_path, sizeof(link_path), "%s/link", base);
    snprintf(link_target, sizeof(link_target), "%s/mono/packages", base);
    bool fixtures =
        hb_git_dir(base, "branch/.git", "ref: refs/heads/feature/x\n") &&
        /* git accepts "ref:" without the space, and a CRLF-terminated file. */
        hb_git_dir(base, "tight/.git", "ref:refs/heads/tight\r\n") &&
        hb_git_dir(base, "sha1/.git", "0123456789abcdef0123456789abcdef01234567\n") &&
        hb_git_dir(base, "sha256/.git",
                   "0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef\n") &&
        hb_git_dir(base, "utf8/.git", "ref: refs/heads/funci\xc3\xb3n\n") &&
        hb_git_dir(base, "main/.git", "ref: refs/heads/main\n") &&
        /* Linked worktree through a relative gitdir: file. */
        hb_git_dir(base, "main/.git/worktrees/wt", "ref: refs/heads/wt-branch\n") &&
        hb_file(base, "main/.git/worktrees/wt/commondir", "../..\n") &&
        hb_file(base, "wt/.git", "gitdir: ../main/.git/worktrees/wt\n") &&
        /* Submodule through an absolute one. */
        hb_git_dir(base, "main/.git/modules/sub", "ref: refs/heads/sub-branch\n") &&
        hb_file(base, "main/sub/.git", text) &&
        /* Root in a subdirectory, and a symlink into it whose lexical parent
         * holds no .git: discovery walks the canonical path, as git does. */
        hb_git_dir(base, "mono/.git", "ref: refs/heads/mono-main\n") &&
        hb_dir(base, "mono/packages/app") && symlink(link_target, link_path) == 0;

    th_fake_git_t fake = {0};
    bool fake_ready = fixtures && th_fake_git_install(&fake, base, NULL, 128);
    static const struct {
        const char *root;
        const char *expected;
    } cases[] = {
        {"branch", "feature/x"}, {"tight", "tight"},         {"sha1", "DETACHED"},
        {"sha256", "DETACHED"},  {"utf8", "funci\xc3\xb3n"}, {"main", "main"},
        {"wt", "wt-branch"},     {"main/sub", "sub-branch"}, {"mono/packages/app", "mono-main"},
        {"link", "mono-main"},
    };
    enum { HB_CASES = sizeof(cases) / sizeof(cases[0]) };
    char got[HB_CASES][256];
    for (int i = 0; i < HB_CASES; i++) {
        hb_query(base, cases[i].root, got[i], sizeof(got[i]));
    }
    int calls = fake_ready ? th_fake_git_calls(&fake) : -1;
    th_fake_git_remove(&fake);
    th_rmtree(base);

    ASSERT_TRUE(fixtures);
    ASSERT_TRUE(fake_ready);
    for (int i = 0; i < HB_CASES; i++) {
        ASSERT_STR_EQ(got[i], cases[i].expected);
    }
    ASSERT_EQ(calls, 0);
    PASS();
#endif /* _WIN32 */
}

/* No work tree, no branch — and no git process to find that out. Includes the
 * two places git itself refuses a work tree: a bare repository and a path
 * inside a .git directory. */
TEST(head_branch_reports_none_outside_a_work_tree) {
#ifdef _WIN32
    SKIP_PLATFORM("the fake git on PATH is a /bin/sh script");
#else
    char base[256];
    if (!hb_make_tempdir(base, sizeof(base), "cbm_headbr_none")) {
        FAIL("th_mktempdir returned NULL");
    }
    bool fixtures =
        hb_dir(base, "plain/deeper") && hb_git_dir(base, "bare.git", "ref: refs/heads/main\n") &&
        hb_dir(base, "bare.git/hooks") && hb_git_dir(base, "repo/.git", "ref: refs/heads/main\n") &&
        hb_dir(base, "repo/.git/info") && hb_file(base, "file.txt", "not a dir\n");
    th_fake_git_t fake = {0};
    bool fake_ready = fixtures && th_fake_git_install(&fake, base, NULL, 128);

    static const char *const roots[] = {"plain",          "plain/deeper",   "missing", "bare.git",
                                        "bare.git/hooks", "repo/.git/info", "file.txt"};
    enum { HB_ROOTS = sizeof(roots) / sizeof(roots[0]) };
    char got[HB_ROOTS][256];
    for (int i = 0; i < HB_ROOTS; i++) {
        hb_query(base, roots[i], got[i], sizeof(got[i]));
    }
    char out[16] = "untouched";
    bool null_path = cbm_git_head_branch(NULL, out, sizeof(out));
    bool null_path_emptied = out[0] == '\0';
    bool empty_path = cbm_git_head_branch("", out, sizeof(out));
    bool zero_size = cbm_git_head_branch(base, out, 0);
    bool null_out = cbm_git_head_branch(base, NULL, sizeof(out));
    int calls = fake_ready ? th_fake_git_calls(&fake) : -1;
    th_fake_git_remove(&fake);
    th_rmtree(base);

    ASSERT_TRUE(fixtures);
    ASSERT_TRUE(fake_ready);
    for (int i = 0; i < HB_ROOTS; i++) {
        ASSERT_STR_EQ(got[i], "(none)");
    }
    ASSERT_FALSE(null_path);
    ASSERT_TRUE(null_path_emptied);
    ASSERT_FALSE(empty_path);
    ASSERT_FALSE(zero_size);
    ASSERT_FALSE(null_out);
    ASSERT_EQ(calls, 0);
    PASS();
#endif /* _WIN32 */
}

/* Forms the helper does not parse go to ONE `git symbolic-ref`, read the way
 * cbm_git_context_resolve() reads it: printed name -> branch, exit 1 ("not a
 * symbolic ref") -> DETACHED, any other failure -> none. The fake git plays
 * each answer; a symlinked HEAD must be left to git, not read through. */
TEST(head_branch_asks_git_once_for_unparsed_forms) {
#ifdef _WIN32
    SKIP_PLATFORM("the fake git on PATH is a /bin/sh script");
#else
    char base[256];
    if (!hb_make_tempdir(base, sizeof(base), "cbm_headbr_ask")) {
        FAIL("th_mktempdir returned NULL");
    }
    char head_link[1024];
    snprintf(head_link, sizeof(head_link), "%s/symhead/.git/HEAD", base);
    bool fixtures = hb_git_dir(base, "reftable/.git", "ref: refs/heads/.invalid\n") &&
                    hb_git_dir(base, "nonheads/.git", "ref: refs/remotes/origin/main\n") &&
                    hb_git_dir(base, "symhead/.git", "placeholder\n") &&
                    hb_file(base, "symhead/.git/refs/heads/linked",
                            "0123456789abcdef0123456789abcdef01234567\n") &&
                    unlink(head_link) == 0 && symlink("refs/heads/linked", head_link) == 0;
    th_fake_git_t fake = {0};
    bool fake_ready = fixtures && th_fake_git_install(&fake, base, "main", 0);

    char got_named[256];
    hb_query(base, "reftable", got_named, sizeof(got_named));
    int named_calls = th_fake_git_calls(&fake);
    char *named_log = th_fake_git_log(&fake);
    char expected_line[1200];
    snprintf(expected_line, sizeof(expected_line),
             "-C %s/reftable symbolic-ref --quiet --short HEAD\n", base);
    bool named_line = named_log && strcmp(named_log, expected_line) == 0;
    free(named_log);

    th_fake_git_reset_calls(&fake);
    bool reconfigured = th_fake_git_configure(&fake, NULL, 1);
    char got_detached[256];
    hb_query(base, "reftable", got_detached, sizeof(got_detached));
    int detached_calls = th_fake_git_calls(&fake);

    th_fake_git_reset_calls(&fake);
    reconfigured = th_fake_git_configure(&fake, NULL, 128) && reconfigured;
    char got_failed[256];
    hb_query(base, "reftable", got_failed, sizeof(got_failed));
    int failed_calls = th_fake_git_calls(&fake);

    th_fake_git_reset_calls(&fake);
    reconfigured = th_fake_git_configure(&fake, "evil\x1b[0m", 0) && reconfigured;
    char got_control[256];
    hb_query(base, "reftable", got_control, sizeof(got_control));
    int control_calls = th_fake_git_calls(&fake);

    th_fake_git_reset_calls(&fake);
    reconfigured = th_fake_git_configure(&fake, "origin/main", 0) && reconfigured;
    char got_nonheads[256];
    hb_query(base, "nonheads", got_nonheads, sizeof(got_nonheads));
    int nonheads_calls = th_fake_git_calls(&fake);

    th_fake_git_reset_calls(&fake);
    reconfigured = th_fake_git_configure(&fake, "linked", 0) && reconfigured;
    char got_symhead[256];
    hb_query(base, "symhead", got_symhead, sizeof(got_symhead));
    int symhead_calls = th_fake_git_calls(&fake);

    th_fake_git_remove(&fake);
    th_rmtree(base);

    ASSERT_TRUE(fixtures);
    ASSERT_TRUE(fake_ready);
    ASSERT_TRUE(reconfigured);
    ASSERT_STR_EQ(got_named, "main");
    ASSERT_EQ(named_calls, 1);
    ASSERT_TRUE(named_line);
    ASSERT_STR_EQ(got_detached, "DETACHED");
    ASSERT_EQ(detached_calls, 1);
    ASSERT_STR_EQ(got_failed, "(none)");
    ASSERT_EQ(failed_calls, 1);
    ASSERT_STR_EQ(got_control, "(none)"); /* git output is filtered too */
    ASSERT_EQ(control_calls, 1);
    ASSERT_STR_EQ(got_nonheads, "origin/main");
    ASSERT_EQ(nonheads_calls, 1);
    ASSERT_STR_EQ(got_symhead, "linked");
    ASSERT_EQ(symhead_calls, 1);
    PASS();
#endif /* _WIN32 */
}

/* Untrusted HEAD / gitdir: content: never an overflow, never a truncated or
 * unvalidated name, never more than one git process, and a gitdir: target is
 * not followed as a second gitlink. The fake git answers "not a repository",
 * so every case must report no branch. */
TEST(head_branch_bounds_hostile_content) {
#ifdef _WIN32
    SKIP_PLATFORM("the fake git on PATH is a /bin/sh script");
#else
    char base[256];
    if (!hb_make_tempdir(base, sizeof(base), "cbm_headbr_hostile")) {
        FAIL("th_mktempdir returned NULL");
    }
    enum { HB_HUGE = 64 * 1024, HB_HUGE_GITDIR = 8 * 1024 };
    char *huge = malloc(HB_HUGE + 64);
    char *huge_gitlink = malloc(HB_HUGE_GITDIR + 64);
    bool fixtures = huge && huge_gitlink;
    if (fixtures) {
        int n = snprintf(huge, 64, "ref: refs/heads/");
        memset(huge + n, 'h', HB_HUGE);
        memcpy(huge + n + HB_HUGE, "\n", 2);
        n = snprintf(huge_gitlink, 64, "gitdir: ");
        memset(huge_gitlink + n, 'g', HB_HUGE_GITDIR);
        memcpy(huge_gitlink + n + HB_HUGE_GITDIR, "\n", 2);
    }
    static const char nul_head[] = "ref: refs/heads/ma\0in\n";
    char text[1024];
    snprintf(text, sizeof(text), "gitdir: %s/target/.git\n", base);
    fixtures = fixtures && hb_git_dir(base, "huge/.git", huge) &&
               hb_git_dir(base, "nul/.git", "placeholder\n") &&
               hb_bytes(base, "nul/.git/HEAD", nul_head, sizeof(nul_head) - 1) &&
               hb_file(base, "gitlink-huge/.git", huge_gitlink) &&
               hb_file(base, "gitlink-nospace/.git", "gitdir:../target/.git\n") &&
               hb_file(base, "gitlink-file/.git", "gitdir: ../target/.git/HEAD\n") &&
               hb_git_dir(base, "target/.git", "ref: refs/heads/target-branch\n") &&
               /* chain-mid has no HEAD, only a gitlink onward to target. */
               hb_file(base, "chain-mid/.git", text) &&
               hb_file(base, "gitlink-chain/.git", "gitdir: ../chain-mid\n");

    static const char *const bad_names[] = {
        "..",      "a/../b",    "a//b",        "trail/",   "/lead",
        ".hidden", "a/.dot",    "x.lock",      "a.lock/b", "a@{1}",
        "@",       "end.",      "has space",   "tab\tin",  "q?",
        "star*",   "brack[",    "back\\slash", "colon:x",  "tilde~1",
        "caret^",  "ctrl\x01x", "del\x7fx",    "nl\nin",   "",
    };
    enum { HB_BAD = sizeof(bad_names) / sizeof(bad_names[0]) };
    char rel[64];
    char head[256];
    for (int i = 0; i < HB_BAD && fixtures; i++) {
        snprintf(rel, sizeof(rel), "bad%02d/.git", i);
        snprintf(head, sizeof(head), "ref: refs/heads/%s\n", bad_names[i]);
        fixtures = hb_git_dir(base, rel, head);
    }
    th_fake_git_t fake = {0};
    bool fake_ready = fixtures && th_fake_git_install(&fake, base, NULL, 128);

    char got_bad[HB_BAD][256];
    int calls_bad[HB_BAD];
    for (int i = 0; i < HB_BAD; i++) {
        snprintf(rel, sizeof(rel), "bad%02d", i);
        th_fake_git_reset_calls(&fake);
        hb_query(base, rel, got_bad[i], sizeof(got_bad[i]));
        calls_bad[i] = th_fake_git_calls(&fake);
    }
    static const char *const hostile[] = {
        "huge", "nul", "gitlink-huge", "gitlink-nospace", "gitlink-file", "gitlink-chain"};
    enum { HB_HOSTILE = sizeof(hostile) / sizeof(hostile[0]) };
    char got_hostile[HB_HOSTILE][256];
    int calls_hostile[HB_HOSTILE];
    for (int i = 0; i < HB_HOSTILE; i++) {
        th_fake_git_reset_calls(&fake);
        hb_query(base, hostile[i], got_hostile[i], sizeof(got_hostile[i]));
        calls_hostile[i] = th_fake_git_calls(&fake);
    }

    /* A valid name that does not fit: false, out emptied, the bytes past the
     * buffer untouched, and no git call (git would print the same name). */
    char long_head[256];
    snprintf(long_head, sizeof(long_head), "ref: refs/heads/%s\n",
             "a-branch-name-longer-than-the-caller-buffer");
    bool long_fixture = hb_git_dir(base, "long/.git", long_head);
    struct {
        char out[16];
        char canary[16];
    } bounded;
    memset(&bounded, 'C', sizeof(bounded));
    char long_root[1024];
    snprintf(long_root, sizeof(long_root), "%s/long", base);
    th_fake_git_reset_calls(&fake);
    bool long_found = cbm_git_head_branch(long_root, bounded.out, sizeof(bounded.out));
    int long_calls = th_fake_git_calls(&fake);
    bool canary_intact = true;
    for (size_t i = 0; i < sizeof(bounded.canary); i++) {
        canary_intact = canary_intact && bounded.canary[i] == 'C';
    }

    th_fake_git_remove(&fake);
    th_rmtree(base);
    free(huge);
    free(huge_gitlink);

    ASSERT_TRUE(fixtures);
    ASSERT_TRUE(fake_ready);
    for (int i = 0; i < HB_BAD; i++) {
        ASSERT_STR_EQ(got_bad[i], "(none)");
        ASSERT_LTE(calls_bad[i], 1);
    }
    for (int i = 0; i < HB_HOSTILE; i++) {
        ASSERT_STR_EQ(got_hostile[i], "(none)");
        ASSERT_LTE(calls_hostile[i], 1);
    }
    ASSERT_TRUE(long_fixture);
    ASSERT_FALSE(long_found);
    ASSERT_EQ(bounded.out[0], '\0');
    ASSERT_TRUE(canary_intact);
    ASSERT_EQ(long_calls, 0);
    PASS();
#endif /* _WIN32 */
}

/* The helper reports what cbm_git_context_resolve() reports, on repositories
 * real git made: a branch with a slash, a subdirectory root, a linked worktree,
 * a detached HEAD and — where this git supports it — a reftable repository,
 * whose HEAD file is only a stub, on a branch and detached: both answered by
 * the one-process fallback. */
#ifndef _WIN32
typedef struct {
    char via_head[256];
    char via_git[256];
} hb_row_t;

static void hb_compare(const char *root, hb_row_t *row) {
    cbm_git_context_t ctx = {0};
    (void)cbm_git_context_resolve(root, &ctx);
    snprintf(row->via_git, sizeof(row->via_git), "%s",
             ctx.is_git && ctx.branch ? ctx.branch : "(none)");
    cbm_git_context_free(&ctx);
    if (!cbm_git_head_branch(root, row->via_head, sizeof(row->via_head))) {
        snprintf(row->via_head, sizeof(row->via_head), "(none)");
    }
}
#endif /* _WIN32 */

TEST(head_branch_matches_git_context_branch) {
#ifdef _WIN32
    SKIP_PLATFORM("git-based head-branch test not supported on Windows CI");
#else
    char repo[256];
    char wt[256];
    char reftable[256];
    if (!hb_make_tempdir(repo, sizeof(repo), "cbm_headbr_repo") ||
        !hb_make_tempdir(wt, sizeof(wt), "cbm_headbr_wt") ||
        !hb_make_tempdir(reftable, sizeof(reftable), "cbm_headbr_reftable")) {
        FAIL("th_mktempdir returned NULL");
    }
    th_rmtree(wt); /* git worktree add creates it */
    if (make_git_repo(repo) != 0) {
        th_rmtree(repo);
        th_rmtree(reftable);
        SKIP_PLATFORM("git not available to init a repo");
    }

    enum { HB_ROWS = 6 };
    hb_row_t rows[HB_ROWS];
    memset(rows, 0, sizeof(rows));
    int count = 0;
    char path[1024];
    char cmd[1024];
    snprintf(path, sizeof(path), "%s/sub/dir", repo);
    snprintf(cmd, sizeof(cmd), "worktree add -b wt-real \"%s\"", wt);
    bool steps = git_run(repo, "checkout -q -b feature/real") == 0 && hb_dir(repo, "sub/dir") &&
                 git_run(repo, cmd) == 0;
    if (steps) {
        hb_compare(repo, &rows[count++]);
        hb_compare(path, &rows[count++]);
        hb_compare(wt, &rows[count++]);
    }
    /* After the comparisons above: detaching moves the main checkout's HEAD. */
    steps = steps && git_run(repo, "checkout -q --detach") == 0;
    if (steps) {
        hb_compare(repo, &rows[count++]);
    }
    /* reftable needs git 2.45+; older git refuses the init and the two rows
     * are simply not compared. */
    bool reftable_ok = git_run(reftable, "init -q --ref-format=reftable") == 0 &&
                       git_run(reftable, "config user.email test@example.com") == 0 &&
                       git_run(reftable, "config user.name Test") == 0 &&
                       git_run(reftable, "commit -q --allow-empty -m init") == 0;
    if (reftable_ok) {
        hb_compare(reftable, &rows[count++]);
        if (git_run(reftable, "checkout -q --detach") == 0) {
            hb_compare(reftable, &rows[count++]);
        }
    }

    git_run(repo, "worktree prune");
    th_rmtree(wt);
    th_rmtree(repo);
    th_rmtree(reftable);

    ASSERT_TRUE(steps);
    for (int i = 0; i < count; i++) {
        ASSERT_STR_EQ(rows[i].via_head, rows[i].via_git);
    }
    ASSERT_STR_EQ(rows[0].via_head, "feature/real");
    ASSERT_STR_EQ(rows[1].via_head, "feature/real");
    ASSERT_STR_EQ(rows[2].via_head, "wt-real");
    ASSERT_STR_EQ(rows[3].via_head, "DETACHED");
    if (reftable_ok) {
        ASSERT_EQ(count, HB_ROWS);
        ASSERT_STR_EQ(rows[5].via_head, "DETACHED");
    }
    PASS();
#endif /* _WIN32 */
}

/* ── Suite ──────────────────────────────────────────────────────── */

SUITE(git_context) {
    RUN_TEST(canonical_root_repo_root);
    RUN_TEST(canonical_root_subdir);
    RUN_TEST(canonical_root_linked_worktree);
    RUN_TEST(worktree_status_clean);
    RUN_TEST(worktree_status_tracked_modified);
    RUN_TEST(worktree_status_untracked);
    RUN_TEST(worktree_status_rename_counts_once);
    RUN_TEST(worktree_status_sample_cap_truncates);
    RUN_TEST(worktree_status_mixed_tracked_and_untracked);
    RUN_TEST(worktree_status_unavailable_non_repo);
    RUN_TEST(is_linked_worktree_true_for_linked_worktree);
    RUN_TEST(is_linked_worktree_false_for_submodule_and_nongit);
    RUN_TEST(head_branch_reads_loose_head_without_git);
    RUN_TEST(head_branch_reports_none_outside_a_work_tree);
    RUN_TEST(head_branch_asks_git_once_for_unparsed_forms);
    RUN_TEST(head_branch_bounds_hostile_content);
    RUN_TEST(head_branch_matches_git_context_branch);
}
