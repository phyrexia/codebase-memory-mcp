#ifndef CBM_GIT_CONTEXT_H
#define CBM_GIT_CONTEXT_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    bool is_git;
    bool is_worktree;
    bool is_detached;
    bool root_exists;
    char *input_path;
    char *worktree_root;
    char *git_dir;
    char *git_common_dir;
    char *canonical_root;
    char *branch;
    char *branch_slug;
    char *head_sha;
    char *base_sha;
} cbm_git_context_t;

/* True when path is the root of a LINKED git worktree (`git worktree add`).
 *
 * Plumbing-only, no subprocess: <path>/.git must be a regular file holding a
 * "gitdir: <path>" pointer AND that gitdir must contain a `commondir` file.
 * The commondir check is what separates a linked worktree from a submodule —
 * a submodule's .git is also a gitlink file, but its gitdir
 * (<super>/.git/modules/<name>) has no commondir entry.
 *
 * Callers run this on every session start, so it stays fork-free; the richer
 * cbm_git_context_resolve() shells out to git and is not usable on that path. */
bool cbm_git_is_linked_worktree(const char *path);

/* Branch of the git work tree containing `path`: the value
 * cbm_git_context_resolve() reports as `branch`, read from the repository's
 * files instead of from seven git processes. list_projects detail=stats asks
 * this once per listed project.
 *
 * Discovery follows git's: from the canonical `path` upward, the first `.git`
 * wins — a directory is the git directory, a "gitdir: <path>" file (linked
 * worktree, submodule) is followed exactly one level. A directory that is
 * itself a git directory (bare repository, path inside .git) has no work tree
 * and reports no branch. HEAD and the gitlink are untrusted input: read with a
 * 4 KiB bound, never followed through a symlink, and accepted only as
 *   "ref: refs/heads/<name>", with <name> a valid ref name -> <name>
 *   a 40- or 64-hex object id                              -> "DETACHED"
 * Any other on-disk form — the reftable stub "ref: refs/heads/.invalid", a
 * symlinked HEAD, a symref outside refs/heads, a malformed or oversized file —
 * costs ONE `git symbolic-ref --quiet --short HEAD`, read as resolve() reads
 * it: a printed name is the branch, "not a symbolic ref" is "DETACHED", any
 * other failure is no branch.
 *
 * Returns true with the NUL-terminated name in `out`, never truncated; false
 * with `out` empty when there is no branch to report or it does not fit. Not
 * mirrored from git, on purpose: GIT_DIR/GIT_CEILING_DIRECTORIES, the
 * filesystem-boundary stop, the safe.directory ownership refusal, and the
 * `--short` "heads/<name>" spelling git uses when a tag shares the name. */
bool cbm_git_head_branch(const char *path, char *out, size_t out_size);

int cbm_git_context_resolve(const char *path, cbm_git_context_t *out);
void cbm_git_context_free(cbm_git_context_t *ctx);
char *cbm_git_context_branch_qn(const char *project_name, const cbm_git_context_t *ctx);
int cbm_git_context_props_json(const cbm_git_context_t *ctx, char *buf, int buf_size);

/* Bounded snapshot of the git worktree status for one validated repo root.
 *
 * available is true ONLY when `git status --porcelain=v1 -z --untracked-files=all`
 * exited 0 AND every NUL-delimited record parsed cleanly. Any other outcome —
 * shell-unsafe path, spawn failure, nonzero exit, malformed/oversized record,
 * trailing partial record — leaves available=false with zero counts and no
 * samples. Callers MUST treat !available as "status unavailable", never as a
 * clean worktree.
 *
 * The entire stream is counted, but only max_samples paths per class (tracked
 * / untracked) are ever retained, in stream order; exceeding the cap sets the
 * matching *_truncated flag. Rename/copy records are counted once (their two
 * NUL-separated paths are consumed without counting the second), and the
 * sampled path is the record's NEW path. Ignored files never appear because
 * --ignored is not requested.
 *
 * cbm_git_worktree_status returns 0 when *out has been filled (available tells
 * whether the snapshot succeeded) and CBM_NOT_FOUND for invalid arguments
 * (NULL root/out or max_samples < 0). The caller owns the returned samples via
 * cbm_git_worktree_status_free. */
typedef struct {
    bool available;
    int tracked_count;
    int untracked_count;
    char **tracked_paths; /* heap-owned samples, tracked_sample_count entries */
    int tracked_sample_count;
    bool tracked_truncated;
    char **untracked_paths;
    int untracked_sample_count;
    bool untracked_truncated;
} cbm_worktree_status_t;

int cbm_git_worktree_status(const char *validated_root, int max_samples,
                            cbm_worktree_status_t *out);
void cbm_git_worktree_status_free(cbm_worktree_status_t *st);

#endif
