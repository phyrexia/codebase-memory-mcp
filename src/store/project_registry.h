/*
 * project_registry.h — in-process cache of project DB identity (C5).
 *
 * list_projects reports name, root_path, indexed_at and node/edge totals for
 * every project DB in the cache directory. Answering that from disk means
 * opening every SQLite file: at ~120 projects a listing cost seconds. This
 * registry keeps the last read identity per .db file and reuses it while the
 * file is unchanged (same size and mtime), so a warm listing opens no DB at
 * all and the schema probe runs once per published generation instead of once
 * per call.
 *
 * Validation is by (size, mtime_ns), which every publish changes (a published
 * generation is a freshly written file renamed into place); publishing from
 * this process also calls cbm_project_registry_invalidate() so the very next
 * lookup re-reads regardless of timestamps.
 */
#ifndef CBM_STORE_PROJECT_REGISTRY_H
#define CBM_STORE_PROJECT_REGISTRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Identity of one project DB file: what its single primary `projects` row
 * says, plus the persisted counts when the file carries them. */
typedef struct {
    char *name; /* internal project name (may differ from the file name) */
    /* Never NULL: empty when the row has none, or holds a NULL column (a
     * foreign or hand-edited database can, the store schema notwithstanding).
     * Consumers may dereference it without a guard. */
    char *root_path;
    char *indexed_at; /* ISO 8601 UTC, NULL when unavailable */
    int64_t size_bytes;
    int64_t mtime_ns;
    bool has_count_columns; /* schema probe result for node_count/edge_count */
    bool counts_stored;     /* both count columns are NOT NULL for the row */
    /* Totals of the primary project, always filled: the persisted values when
     * the row carries them, otherwise the live COUNT(*) computed while reading
     * this generation. Cached with the generation, so a reader that must report
     * totals — /api/indexed-roots has no "skip stats" projection — never has to
     * open the file just to count. */
    int64_t node_count;
    int64_t edge_count;
} cbm_project_identity_t;

typedef enum {
    /* Not a resolvable project: missing, ghost, corrupt, or no single primary
     * project row. Same judgement the listing makes, cached. */
    CBM_PROJECT_REGISTRY_ABSENT = 0,
    CBM_PROJECT_REGISTRY_OK = 1,
    CBM_PROJECT_REGISTRY_OOM = -1,
} cbm_project_registry_status_t;

/* Fill `out` with the identity of `db_path`, reading the file only when it
 * changed since the cached entry. `out` is zeroed first and owns its strings;
 * release them with cbm_project_identity_clear(). */
cbm_project_registry_status_t cbm_project_registry_lookup(const char *db_path,
                                                          cbm_project_identity_t *out);

void cbm_project_identity_clear(cbm_project_identity_t *identity);

/* Drop the cached entry for `db_path` (publish/rename, deletion). */
void cbm_project_registry_invalidate(const char *db_path);

/* Drop every entry (tests). */
void cbm_project_registry_reset(void);

#if defined(CBM_CLI_ENABLE_TEST_API)
/* Test seam: how many times the registry has gone to disk to fill an identity —
 * one per .db file generation actually opened (an unreadable file counts once,
 * and is then cached as unresolvable). A warm lookup performs none. */
size_t cbm_project_registry_disk_reads_for_test(void);
void cbm_project_registry_disk_reads_reset_for_test(void);
#endif

#endif /* CBM_STORE_PROJECT_REGISTRY_H */
