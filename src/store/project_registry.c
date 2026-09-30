/*
 * project_registry.c — see project_registry.h.
 *
 * The cache is a process-global table keyed by DB path, protected by one mutex.
 * Reads of an unchanged file (warm) hold the lock only for a table lookup; a
 * miss reads the DB under the lock too, so concurrent callers serialize on the
 * cold path. That is deliberate: the alternative — refcounted entries filled
 * outside the lock — buys parallel cold listings at the price of a second
 * lifetime protocol for entries that are re-read at most once per generation.
 */

#include "store/project_registry.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "foundation/compat.h"
#include "foundation/compat_fs.h"
#include "foundation/compat_thread.h"
#include "foundation/platform.h"
#include "store/store.h"

/* ── Identity ───────────────────────────────────────────────────── */

#if defined(CBM_CLI_ENABLE_TEST_API)
/* Counted with default (seq_cst) ordering on purpose: the handler that ticks
 * runs on the server thread while the test reads from its own, and a test seam
 * that could report a stale zero would fail as flaky instead of red. */
static atomic_size_t g_disk_reads = 0;

size_t cbm_project_registry_disk_reads_for_test(void) {
    return atomic_load(&g_disk_reads);
}

void cbm_project_registry_disk_reads_reset_for_test(void) {
    atomic_store(&g_disk_reads, 0);
}

static void registry_disk_read_tick(void) {
    atomic_fetch_add(&g_disk_reads, 1);
}
#else
static void registry_disk_read_tick(void) {}
#endif

static char *identity_strdup(const char *s) {
    if (!s) {
        return NULL;
    }
    size_t len = strlen(s) + 1;
    char *d = malloc(len);
    if (d) {
        memcpy(d, s, len);
    }
    return d;
}

void cbm_project_identity_clear(cbm_project_identity_t *identity) {
    if (!identity) {
        return;
    }
    free(identity->name);
    free(identity->root_path);
    free(identity->indexed_at);
    memset(identity, 0, sizeof(*identity));
}

static bool identity_clone(const cbm_project_identity_t *src, cbm_project_identity_t *dst) {
    memset(dst, 0, sizeof(*dst));
    dst->name = identity_strdup(src->name);
    dst->root_path = identity_strdup(src->root_path);
    dst->indexed_at = src->indexed_at ? identity_strdup(src->indexed_at) : NULL;
    if (!dst->name || !dst->root_path || (src->indexed_at && !dst->indexed_at)) {
        cbm_project_identity_clear(dst);
        return false;
    }
    dst->size_bytes = src->size_bytes;
    dst->mtime_ns = src->mtime_ns;
    dst->has_count_columns = src->has_count_columns;
    dst->counts_stored = src->counts_stored;
    dst->node_count = src->node_count;
    dst->edge_count = src->edge_count;
    return true;
}

/* Read what one .db file says about its primary project. One open, one schema
 * probe, one row read: the registry makes sure this runs once per published
 * generation rather than once per listing. */
static cbm_project_registry_status_t project_identity_read(const char *db_path, int64_t size_bytes,
                                                           int64_t mtime_ns,
                                                           cbm_project_identity_t *out) {
    registry_disk_read_tick();
    cbm_store_t *store = cbm_store_open_path_query(db_path);
    if (!store) {
        return CBM_PROJECT_REGISTRY_ABSENT;
    }
    cbm_project_t *projects = NULL;
    int count = 0;
    cbm_project_registry_status_t status = CBM_PROJECT_REGISTRY_ABSENT;
    if (cbm_store_list_projects(store, &projects, &count) == CBM_STORE_OK) {
        int primary = cbm_store_primary_project_index(projects, count);
        if (primary >= 0) {
            bool has_count_columns = cbm_store_projects_has_count_columns(store);
            cbm_project_t row = {0};
            cbm_project_stats_t stats = {0};
            if (cbm_store_get_project_stats(store, projects[primary].name, has_count_columns, &row,
                                            &stats) == CBM_STORE_OK) {
                out->name = (char *)row.name;
                out->root_path = (char *)row.root_path;
                out->indexed_at = (char *)row.indexed_at;
                row.name = NULL;
                row.root_path = NULL;
                row.indexed_at = NULL;
                out->size_bytes = size_bytes;
                out->mtime_ns = mtime_ns;
                out->has_count_columns = stats.has_count_columns;
                out->counts_stored = stats.counts_stored;
                out->node_count = stats.node_count;
                out->edge_count = stats.edge_count;
                /* root_path is NOT NULL in the store schema, so a missing copy
                 * here is usually allocation failure. But a foreign or
                 * hand-edited database can hold a NULL column, and the store
                 * reader reports both as the same NULL: this identity contract
                 * is "never NULL, empty when the row has none", so restore the
                 * empty spelling. If the allocator really is exhausted that
                 * one-byte copy fails too and the row stays OOM. */
                if (!out->root_path) {
                    out->root_path = identity_strdup("");
                }
                if (!out->name || !out->root_path) {
                    cbm_project_identity_clear(out);
                    status = CBM_PROJECT_REGISTRY_OOM;
                } else {
                    if (!out->counts_stored) {
                        /* No totals on the row: derive them now and cache them
                         * with this generation, so a reader that must report
                         * totals (/api/indexed-roots) costs no second open.
                         * COUNT(*) reads the same tables the row describes, so
                         * the value is as fresh as the generation itself. */
                        int nodes = cbm_store_count_nodes(store, out->name);
                        int edges = cbm_store_count_edges(store, out->name);
                        out->node_count = nodes < 0 ? 0 : nodes;
                        out->edge_count = edges < 0 ? 0 : edges;
                    }
                    status = CBM_PROJECT_REGISTRY_OK;
                }
            }
            cbm_project_free_fields(&row);
        }
    }
    cbm_store_free_projects(projects, count);
    cbm_store_close(store);
    return status;
}

/* ── Registry table ─────────────────────────────────────────────── */

typedef struct {
    char *path;
    int64_t size_bytes;
    int64_t mtime_ns;
    bool resolvable;
    cbm_project_identity_t identity;
} registry_entry_t;

static registry_entry_t *g_entries = NULL;
static size_t g_entry_count = 0;
static size_t g_entry_cap = 0;
static cbm_mutex_t g_mutex;

enum { REGISTRY_MUTEX_UNINIT = 0, REGISTRY_MUTEX_INITING = 1, REGISTRY_MUTEX_INITED = 2 };
static atomic_int g_mutex_state = REGISTRY_MUTEX_UNINIT;

/* Same once-init as the UI log ring (http_server.c): only publish INITED after
 * cbm_mutex_init() has completed, and let losers of the CAS wait for it. */
static void registry_lock(void) {
    int state = atomic_load(&g_mutex_state);
    if (state == REGISTRY_MUTEX_INITED) {
        cbm_mutex_lock(&g_mutex);
        return;
    }
    state = REGISTRY_MUTEX_UNINIT;
    if (atomic_compare_exchange_strong(&g_mutex_state, &state, REGISTRY_MUTEX_INITING)) {
        cbm_mutex_init(&g_mutex);
        atomic_store(&g_mutex_state, REGISTRY_MUTEX_INITED);
        cbm_mutex_lock(&g_mutex);
        return;
    }
    while (atomic_load(&g_mutex_state) != REGISTRY_MUTEX_INITED) {
        cbm_usleep(1000);
    }
    cbm_mutex_lock(&g_mutex);
}

static void registry_unlock(void) {
    cbm_mutex_unlock(&g_mutex);
}

static registry_entry_t *registry_find(const char *path) {
    for (size_t i = 0; i < g_entry_count; i++) {
        if (strcmp(g_entries[i].path, path) == 0) {
            return &g_entries[i];
        }
    }
    return NULL;
}

/* Unordered removal: the table is keyed by path, so row order carries nothing. */
static void registry_drop(registry_entry_t *entry) {
    free(entry->path);
    cbm_project_identity_clear(&entry->identity);
    *entry = g_entries[g_entry_count - 1];
    g_entry_count--;
}

static void registry_store(const char *path, int64_t size_bytes, int64_t mtime_ns, bool resolvable,
                           const cbm_project_identity_t *identity) {
    registry_entry_t *entry = registry_find(path);
    if (!entry) {
        if (g_entry_count == g_entry_cap) {
            size_t cap = g_entry_cap ? g_entry_cap * 2 : 16;
            void *grown = realloc(g_entries, cap * sizeof(*g_entries));
            if (!grown) {
                return; /* cache is best-effort: the caller keeps the identity */
            }
            g_entries = grown;
            g_entry_cap = cap;
        }
        entry = &g_entries[g_entry_count];
        memset(entry, 0, sizeof(*entry));
        entry->path = identity_strdup(path);
        if (!entry->path) {
            return;
        }
        g_entry_count++;
    } else {
        cbm_project_identity_clear(&entry->identity);
        memset(&entry->identity, 0, sizeof(entry->identity));
    }
    entry->size_bytes = size_bytes;
    entry->mtime_ns = mtime_ns;
    entry->resolvable = resolvable;
    if (resolvable && !identity_clone(identity, &entry->identity)) {
        registry_drop(entry);
    }
}

cbm_project_registry_status_t cbm_project_registry_lookup(const char *db_path,
                                                          cbm_project_identity_t *out) {
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    if (!db_path || !out) {
        return CBM_PROJECT_REGISTRY_ABSENT;
    }

    cbm_path_info_t info;
    if (cbm_path_info_utf8(db_path, &info) != CBM_PATH_INFO_OK ||
        (!info.is_regular && !info.is_symlink)) {
        return CBM_PROJECT_REGISTRY_ABSENT;
    }

    /* A symlink's own mtime does not track the target's, so a cached entry
     * could outlive a replacement behind the link. Read those through. */
    if (!info.is_regular) {
        cbm_project_identity_t fresh = {0};
        cbm_project_registry_status_t status =
            project_identity_read(db_path, cbm_file_size(db_path), info.mtime_ns, &fresh);
        if (status == CBM_PROJECT_REGISTRY_OK) {
            *out = fresh;
        }
        return status;
    }

    registry_lock();
    registry_entry_t *entry = registry_find(db_path);
    if (entry && entry->size_bytes == info.size && entry->mtime_ns == info.mtime_ns) {
        cbm_project_registry_status_t status = CBM_PROJECT_REGISTRY_ABSENT;
        if (entry->resolvable) {
            status = identity_clone(&entry->identity, out) ? CBM_PROJECT_REGISTRY_OK
                                                          : CBM_PROJECT_REGISTRY_OOM;
        }
        registry_unlock();
        return status;
    }

    cbm_project_identity_t fresh = {0};
    cbm_project_registry_status_t status =
        project_identity_read(db_path, info.size, info.mtime_ns, &fresh);
    if (status != CBM_PROJECT_REGISTRY_OOM) {
        /* Unresolvable files are cached too: a ghost .db would otherwise be
         * reopened on every call. */
        registry_store(db_path, info.size, info.mtime_ns, status == CBM_PROJECT_REGISTRY_OK, &fresh);
    }
    if (status == CBM_PROJECT_REGISTRY_OK) {
        *out = fresh;
    } else {
        cbm_project_identity_clear(&fresh);
    }
    registry_unlock();
    return status;
}

void cbm_project_registry_invalidate(const char *db_path) {
    if (!db_path) {
        return;
    }
    registry_lock();
    registry_entry_t *entry = registry_find(db_path);
    if (entry) {
        registry_drop(entry);
    }
    registry_unlock();
}

void cbm_project_registry_reset(void) {
    registry_lock();
    for (size_t i = 0; i < g_entry_count; i++) {
        free(g_entries[i].path);
        cbm_project_identity_clear(&g_entries[i].identity);
    }
    free(g_entries);
    g_entries = NULL;
    g_entry_count = 0;
    g_entry_cap = 0;
    registry_unlock();
}
