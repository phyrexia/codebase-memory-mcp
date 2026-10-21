/*
 * host.c — Lifecycle owner for the mandatory per-account CBM daemon.
 */
#include "daemon/host.h"
#include "daemon/host_internal.h"

#include "daemon/application.h"
#include "daemon/bootstrap.h"
#include "daemon/runtime.h"
#include "daemon/project_lock.h"
#include "daemon/version_cohort.h"
#include "cli/cli.h"
#include "foundation/compat.h"
#include "foundation/compat_fs.h"
#include "foundation/compat_thread.h"
#include "foundation/constants.h"
#include "foundation/diagnostics.h"
#include "foundation/limits.h"
#include "foundation/log.h"
#include "foundation/mem.h"
#include "foundation/platform.h"
#include "foundation/secure_random.h"
#include "foundation/sha256.h"
#include "mcp/index_supervisor.h"
#include "store/store.h"
#include "ui/config.h"
#include "ui/embedded_assets.h"
#include "ui/http_server.h"
#include "watcher/watcher.h"

#include <errno.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    /* Descriptor budget for the configured connection cap. Measured on a live
     * daemon: 86 open descriptors while serving ~64 connections, so roughly
     * 1.1 per connection over a fixed base (log, listener, index files). Ask
     * for two per connection plus a generous base, and never less than the
     * floor — launchd hands this process a soft cap of 256, under which a
     * daemon configured for a few hundred connections runs out of descriptors
     * for its own files long before it runs out of connection slots. */
    HOST_FD_PER_CLIENT = 2,
    HOST_FD_BASE = 256,
    HOST_FD_FLOOR = 1024,
    HOST_LEASE_TIMEOUT_MS = 30000,
    HOST_REQUEST_TIMEOUT_MS = 5000,
    HOST_RUNTIME_SHUTDOWN_MS = 10000,
    HOST_APPLICATION_SHUTDOWN_MS = 10000,
    HOST_COORDINATION_CLEANUP_MS = 500,
    HOST_INITIAL_CLIENT_TIMEOUT_MS = 10000,
    /* A just-superseded daemon generation can still hold the cohort claim for
     * the brief window between going client-terminal (which lets the next
     * generation start) and releasing its cohort locks. Wait out that handoff
     * rather than failing startup on a transient BUSY. Bounded well under the
     * client's 30 s daemon-start budget. */
    HOST_DAEMON_CLAIM_TIMEOUT_MS = 12000,
    HOST_WAIT_TICK_MS = 100,
    HOST_HTTP_CONFIG_POLL_MS = 1000,
    HOST_METRICS_FLUSH_MS = 60000,
    HOST_HTTP_RETRY_INITIAL_MS = 1000,
    HOST_HTTP_RETRY_MAX_MS = 30000,
    HOST_WATCH_INTERVAL_MS = 5000,
    /* Age-based cleaners reap runtime files after about 3 days (a common
     * systemd-tmpfiles /tmp policy; macOS tmp_cleaner's daily sweep). An
     * hourly refresh keeps every held name far inside either threshold. */
    HOST_KEEPALIVE_INTERVAL_S = 3600,
    /* How often the daemon checks that its runtime names still resolve to
     * what it holds, and how many IO observations in a row end the
     * generation: one transient failure must not, two on its own directory
     * do, because a generation it cannot verify must not keep serving. */
    HOST_NAMES_CHECK_MS = 1000,
    HOST_NAMES_IO_STREAK_LIMIT = 2,
    HOST_NAMES_CHECK_SEAM_CEILING_MS = 60000,
    HOST_CONFLICT_LOG_CAP = 1024 * 1024,
    HOST_OPERATION_LOG_CAP = 5 * 1024 * 1024,
    HOST_PATH_CAP = 4096,
#ifdef _WIN32
    /* LockFileEx has no non-owning F_GETLK equivalent. A lifetime probe must
     * momentarily take and release the private file range. The child can also
     * briefly lose the legacy-mutex handoff to another waiting starter, so it
     * retries both bounded observation/compatibility windows. */
    HOST_WINDOWS_LIFETIME_ACQUIRE_MS = 250,
#endif
};

typedef struct host_state host_state_t;

typedef struct {
    void *context;
    void (*config_load)(void *context, cbm_ui_config_t *config_out);
    cbm_http_server_t *(*server_new)(void *context, int port);
    void (*server_configure)(void *context, cbm_http_server_t *server, host_state_t *host);
    void (*server_stop)(void *context, cbm_http_server_t *server);
    bool (*server_free)(void *context, cbm_http_server_t *server);
    int (*thread_start)(void *context, cbm_thread_t *thread, void *(*entry)(void *),
                        void *entry_context);
    int (*thread_join)(void *context, cbm_thread_t *thread);
} host_http_ops_t;

struct host_state {
    /* Mirrors cbm_daemon_host_config_t.permanent for prepare-time consumers. */
    bool permanent;
    cbm_daemon_application_t *application;
    cbm_watcher_t *watcher;
    cbm_store_t *watch_store;
    cbm_config_t *runtime_config;
    cbm_project_lock_manager_t *project_locks;
    cbm_http_server_t *http;
    cbm_thread_t watcher_thread;
    cbm_thread_t http_thread;
    bool watcher_started;
    bool http_started;
    bool http_retiring;
    bool http_config_loaded;
    bool http_config_enabled;
    bool http_assets_available;
    int http_config_port;
    uint64_t http_next_config_load_ms;
    uint64_t metrics_next_flush_ms;
    uint64_t http_retry_at_ms;
    uint32_t http_retry_delay_ms;
    uint32_t http_largest_scheduled_retry_ms;
    const host_http_ops_t *http_ops;
    uint8_t ui_readiness_secret[CBM_SHA256_DIGEST_LEN];
};

static FILE *g_host_log_file = NULL;
static cbm_mutex_t g_host_log_mutex;
static bool g_host_log_mutex_initialized = false;

static _Noreturn void host_force_terminate(const char *component);

static void host_log_sink(const char *line) {
    cbm_ui_log_append(line);
    if (!g_host_log_file || !g_host_log_mutex_initialized) {
        return;
    }
    cbm_mutex_lock(&g_host_log_mutex);
    (void)fprintf(g_host_log_file, "%s\n", line ? line : "");
    (void)fflush(g_host_log_file);
    cbm_mutex_unlock(&g_host_log_mutex);
}

static bool host_log_open(char conflict_log_out[HOST_PATH_CAP]) {
    const char *cache = cbm_resolve_cache_dir();
    if (!cache || !cache[0] || !conflict_log_out) {
        return false;
    }
    char logs[HOST_PATH_CAP];
    int written = snprintf(logs, sizeof(logs), "%s/logs", cache);
    if (written <= 0 || written >= (int)sizeof(logs)) {
        return false;
    }
    written = snprintf(conflict_log_out, HOST_PATH_CAP, "%s/daemon-conflicts.ndjson", logs);
    if (written <= 0 || written >= HOST_PATH_CAP) {
        return false;
    }
    g_host_log_file =
        cbm_daemon_ipc_private_log_open(logs, "cbm-daemon.log", HOST_OPERATION_LOG_CAP);
    if (!g_host_log_file) {
        conflict_log_out[0] = '\0';
        return false;
    }
    cbm_mutex_init(&g_host_log_mutex);
    g_host_log_mutex_initialized = true;
    cbm_log_set_sink(host_log_sink);
    return true;
}

/* #1828: a detached daemon has no stderr; its start failure must reach the
 * client that is waiting for it, or that client burns its whole deadline and
 * reports the opposite of the truth. */
static void host_start_failure_record(const cbm_daemon_ipc_endpoint_t *endpoint,
                                      const char *component) {
    char logs[HOST_PATH_CAP];
    if (!cbm_daemon_bootstrap_log_directory(logs, sizeof(logs)) ||
        !cbm_daemon_bootstrap_start_failure_record(logs, endpoint, component)) {
        cbm_log_error("daemon.start_failure_record_failed", "component", component);
    }
}

static void host_log_close(void) {
    cbm_log_set_sink(NULL);
    if (g_host_log_mutex_initialized) {
        cbm_mutex_lock(&g_host_log_mutex);
    }
    FILE *file = g_host_log_file;
    g_host_log_file = NULL;
    if (file) {
        (void)fclose(file);
    }
    if (g_host_log_mutex_initialized) {
        cbm_mutex_unlock(&g_host_log_mutex);
        cbm_mutex_destroy(&g_host_log_mutex);
        g_host_log_mutex_initialized = false;
    }
}

static uint64_t host_deadline_after(uint32_t timeout_ms) {
    uint64_t now = cbm_now_ms();
    return now > UINT64_MAX - timeout_ms ? UINT64_MAX : now + timeout_ms;
}

static uint64_t host_current_process_id(void) {
#ifdef _WIN32
    return (uint64_t)GetCurrentProcessId();
#else
    return (uint64_t)getpid();
#endif
}

static _Noreturn void host_cleanup_force_terminate(const char *component) {
    /* Early startup failures can precede the ordinary operation-log setup.
     * Make one best-effort attempt to establish that same owner-only durable
     * sink before the process-level escape releases every native claim. */
    if (!g_host_log_file) {
        char conflict_log[HOST_PATH_CAP];
        cbm_ui_log_init();
        (void)host_log_open(conflict_log);
    }
    host_force_terminate(component);
}

static void host_cleanup_release_until_complete(cbm_daemon_host_cleanup_release_for_test_fn release,
                                                void *context, const char *component) {
    if (!release) {
        return;
    }
    uint64_t deadline = host_deadline_after(HOST_COORDINATION_CLEANUP_MS);
    while (!release(context)) {
        if (cbm_now_ms() >= deadline) {
            host_cleanup_force_terminate(component);
        }
        cbm_usleep(1000);
    }
}

void cbm_daemon_host_cleanup_release_until_complete_for_test(
    cbm_daemon_host_cleanup_release_for_test_fn release, void *context) {
    host_cleanup_release_until_complete(release, context, "coordination_cleanup");
}

static bool host_cohort_lease_release_once(void *context) {
    cbm_version_cohort_lease_t **lease = context;
    if (!lease || !*lease) {
        return true;
    }
    (void)cbm_version_cohort_lease_release(lease);
    return *lease == NULL;
}

static bool host_cohort_manager_free_once(void *context) {
    cbm_version_cohort_manager_t **manager = context;
    if (!manager || !*manager) {
        return true;
    }
    (void)cbm_version_cohort_manager_free(manager);
    return *manager == NULL;
}

static void host_cohort_close(cbm_version_cohort_lease_t **lease,
                              cbm_version_cohort_manager_t **manager) {
    host_cleanup_release_until_complete(host_cohort_lease_release_once, lease,
                                        "cohort_lease_cleanup");
    host_cleanup_release_until_complete(host_cohort_manager_free_once, manager,
                                        "cohort_manager_cleanup");
}

static bool host_daemon_claim_release_once(void *context) {
    cbm_version_cohort_daemon_claim_t **claim = context;
    if (!claim || !*claim) {
        return true;
    }
    (void)cbm_version_cohort_daemon_claim_release(claim);
    return *claim == NULL;
}

static void host_daemon_claim_close(cbm_version_cohort_daemon_claim_t **claim) {
    host_cleanup_release_until_complete(host_daemon_claim_release_once, claim,
                                        "daemon_claim_cleanup");
}

static bool host_participant_guard_release_once(void *context) {
    cbm_daemon_ipc_participant_guard_t **guard = context;
    if (!guard || !*guard) {
        return true;
    }
    (void)cbm_daemon_ipc_participant_guard_release(guard);
    return *guard == NULL;
}

static void host_participant_guard_close(cbm_daemon_ipc_participant_guard_t **guard) {
    host_cleanup_release_until_complete(host_participant_guard_release_once, guard,
                                        "participant_guard_cleanup");
}

static int host_lifetime_reservation_acquire(
    const cbm_daemon_ipc_endpoint_t *endpoint,
    cbm_daemon_ipc_lifetime_reservation_t **reservation_out) {
#ifdef _WIN32
    uint64_t now = cbm_now_ms();
    uint64_t deadline = now > UINT64_MAX - HOST_WINDOWS_LIFETIME_ACQUIRE_MS
                            ? UINT64_MAX
                            : now + HOST_WINDOWS_LIFETIME_ACQUIRE_MS;
    do {
        int status = cbm_daemon_ipc_lifetime_reservation_try_acquire(endpoint, reservation_out);
        if (status != 0) {
            return status;
        }
        cbm_usleep(1000);
    } while (cbm_now_ms() < deadline);
    return 0;
#else
    return cbm_daemon_ipc_lifetime_reservation_try_acquire(endpoint, reservation_out);
#endif
}

static void *host_watcher_thread(void *opaque) {
    cbm_watcher_run(opaque, HOST_WATCH_INTERVAL_MS);
    return NULL;
}

static void *host_http_thread(void *opaque) {
    cbm_http_server_run(opaque);
    return NULL;
}

static int host_watcher_index(const char *project_name, const char *root_path, void *opaque) {
    host_state_t *host = opaque;
    return host && host->application
               ? cbm_daemon_application_watcher_index(project_name, root_path, host->application)
               : -1;
}

static int host_ui_index(void *opaque, const char *root_path, const char *project_name) {
    host_state_t *host = opaque;
    return host && host->application
               ? cbm_daemon_application_index(host->application, project_name ? project_name : "",
                                              root_path)
               : -1;
}

static bool host_ui_mutation_begin(void *opaque, const char *project) {
    host_state_t *host = opaque;
    return host && host->application &&
           cbm_daemon_application_project_mutation_try_begin(host->application, project);
}

static void host_ui_mutation_end(void *opaque, const char *project) {
    host_state_t *host = opaque;
    if (host && host->application) {
        cbm_daemon_application_project_mutation_end(host->application, project);
    }
}

static void host_http_config_load_default(void *context, cbm_ui_config_t *config_out) {
    (void)context;
    cbm_ui_config_load(config_out);
}

static void host_http_metrics_sink(void *context, const char *path, uint64_t dur_us, bool is_error);

static cbm_http_server_t *host_http_server_new_default(void *context, int port) {
    (void)context;
    return cbm_http_server_new(port);
}

static void host_http_server_configure_default(void *context, cbm_http_server_t *server,
                                               host_state_t *host) {
    (void)context;
    cbm_http_server_set_watcher(server, host->watcher);
    cbm_http_server_set_index_executor(server, host_ui_index, host);
    cbm_http_server_set_metrics_sink(server, host_http_metrics_sink, host);
    cbm_http_server_set_project_mutation_guard(server, host_ui_mutation_begin, host_ui_mutation_end,
                                               host);
    cbm_http_server_set_readiness_secret(server, host->ui_readiness_secret);
}

static void host_http_server_stop_default(void *context, cbm_http_server_t *server) {
    (void)context;
    cbm_http_server_stop(server);
}

static bool host_http_server_free_default(void *context, cbm_http_server_t *server) {
    (void)context;
    return cbm_http_server_free(server);
}

typedef int (*host_http_thread_create_fn)(void *context, cbm_thread_t *thread,
                                          void *(*entry)(void *), void *entry_context);

static int host_http_thread_create_default(void *context, cbm_thread_t *thread,
                                           void *(*entry)(void *), void *entry_context) {
    (void)context;
    return cbm_thread_create(thread, 0, entry, entry_context);
}

static int host_http_thread_schedule_create(cbm_thread_t *thread, void *(*entry)(void *),
                                            cbm_http_server_t *server,
                                            host_http_thread_create_fn create,
                                            void *create_context) {
    if (!server || !create || !cbm_http_server_schedule_run(server))
        return -1;
    int rc = create(create_context, thread, entry, server);
    if (rc != 0 && !cbm_http_server_cancel_scheduled_run(server))
        return -1;
    return rc;
}

static int host_http_thread_start_default(void *context, cbm_thread_t *thread,
                                          void *(*entry)(void *), void *entry_context) {
    (void)context;
    return host_http_thread_schedule_create(thread, entry, entry_context,
                                            host_http_thread_create_default, NULL);
}

static int host_http_thread_join_default(void *context, cbm_thread_t *thread) {
    (void)context;
    return cbm_thread_join(thread);
}

static const host_http_ops_t g_host_http_default_ops = {
    .context = NULL,
    .config_load = host_http_config_load_default,
    .server_new = host_http_server_new_default,
    .server_configure = host_http_server_configure_default,
    .server_stop = host_http_server_stop_default,
    .server_free = host_http_server_free_default,
    .thread_start = host_http_thread_start_default,
    .thread_join = host_http_thread_join_default,
};

static bool host_http_stop_join_free(host_state_t *host) {
    const host_http_ops_t *ops = host->http_ops;
    if (host->http && host->http_started) {
        ops->server_stop(ops->context, host->http);
    }
    if (host->http_started) {
        if (ops->thread_join(ops->context, &host->http_thread) != 0) {
            return false;
        }
        host->http_started = false;
    }
    if (!ops->server_free(ops->context, host->http)) {
        return false;
    }
    host->http = NULL;
    return true;
}

static uint64_t host_deadline_from(uint64_t now_ms, uint32_t delay_ms) {
    return now_ms > UINT64_MAX - delay_ms ? UINT64_MAX : now_ms + delay_ms;
}

static void host_http_schedule_retry(host_state_t *host, uint64_t now_ms, const char *reason) {
    uint32_t delay =
        host->http_retry_delay_ms ? host->http_retry_delay_ms : HOST_HTTP_RETRY_INITIAL_MS;
    host->http_retry_at_ms = host_deadline_from(now_ms, delay);
    if (delay > host->http_largest_scheduled_retry_ms) {
        host->http_largest_scheduled_retry_ms = delay;
    }
    host->http_retry_delay_ms =
        delay >= HOST_HTTP_RETRY_MAX_MS / 2U ? HOST_HTTP_RETRY_MAX_MS : delay * 2U;
    char delay_text[32];
    (void)snprintf(delay_text, sizeof(delay_text), "%u", delay);
    cbm_log_warn("ui.retry_scheduled", "reason", reason, "delay_ms", delay_text);
}

/* UI HTTP requests feed the same accumulator as tool calls ("http" source). */
static void host_http_metrics_sink(void *context, const char *path, uint64_t dur_us,
                                   bool is_error) {
    host_state_t *host = context;
    if (host && host->application) {
        cbm_daemon_application_metrics_record(host->application, "http", path ? path : "", "",
                                              dur_us, is_error);
    }
}

/* Minute cadence for the request-metrics drain, same deadline pattern as the
 * HTTP config poll below. Failed drains keep their data (see metrics_flush). */
static void host_metrics_flush_at(host_state_t *host, uint64_t now_ms) {
    if (!host || !host->application || now_ms < host->metrics_next_flush_ms) {
        return;
    }
    host->metrics_next_flush_ms = host_deadline_from(now_ms, HOST_METRICS_FLUSH_MS);
    cbm_daemon_application_metrics_flush(host->application);
}

static void host_http_reconcile_at(host_state_t *host, uint64_t now_ms, bool force_config_load) {
    if (!host || !host->http_ops) {
        return;
    }
    if (!force_config_load && host->http_config_loaded && now_ms < host->http_next_config_load_ms) {
        return;
    }
    host->http_next_config_load_ms = host_deadline_from(now_ms, HOST_HTTP_CONFIG_POLL_MS);

    cbm_ui_config_t desired;
    host->http_ops->config_load(host->http_ops->context, &desired);
    bool config_changed = !host->http_config_loaded ||
                          desired.ui_enabled != host->http_config_enabled ||
                          desired.ui_port != host->http_config_port;
    if (config_changed && !host->http_retiring) {
        host->http_retiring = true;
        host->http_retry_at_ms = now_ms;
        host->http_retry_delay_ms = HOST_HTTP_RETRY_INITIAL_MS;
    }
    if (host->http_retiring) {
        if (now_ms < host->http_retry_at_ms) {
            return;
        }
        if (!host_http_stop_join_free(host)) {
            host_http_schedule_retry(host, now_ms, "server_retire");
            return;
        }
        host->http_retiring = false;
    }
    if (config_changed) {
        host->http_config_loaded = true;
        host->http_config_enabled = desired.ui_enabled;
        host->http_config_port = desired.ui_port;
        host->http_retry_at_ms = now_ms;
        host->http_retry_delay_ms = HOST_HTTP_RETRY_INITIAL_MS;
    }
    if (!desired.ui_enabled) {
        return;
    }
    if (host->http_started || host->http) {
        return;
    }
    if (!host->http_assets_available) {
        if (config_changed) {
            cbm_log_warn("ui.no_assets", "hint", "rebuild with: make -f Makefile.cbm cbm-with-ui");
        }
        host->http_retry_at_ms = UINT64_MAX;
        return;
    }
    if (now_ms < host->http_retry_at_ms) {
        return;
    }

    const host_http_ops_t *ops = host->http_ops;
    host->http = ops->server_new(ops->context, desired.ui_port);
    if (!host->http) {
        host_http_schedule_retry(host, now_ms, "server_create");
        return;
    }
    ops->server_configure(ops->context, host->http, host);
    if (ops->thread_start(ops->context, &host->http_thread, host_http_thread, host->http) == 0) {
        host->http_started = true;
        host->http_retry_at_ms = 0;
        host->http_retry_delay_ms = HOST_HTTP_RETRY_INITIAL_MS;
    } else {
        host->http_retiring = true;
        if (host_http_stop_join_free(host)) {
            host->http_retiring = false;
        }
        host_http_schedule_retry(host, now_ms, "thread_start");
    }
}

static void host_background_stop(host_state_t *host) {
    if (host->http && host->http_started) {
        host->http_ops->server_stop(host->http_ops->context, host->http);
    }
    if (host->watcher) {
        cbm_watcher_stop(host->watcher);
    }
}

static bool host_background_join(host_state_t *host) {
    if (host->http_started) {
        if (host->http_ops->thread_join(host->http_ops->context, &host->http_thread) != 0) {
            return false;
        }
        host->http_started = false;
    }
    if (host->watcher_started) {
        if (cbm_thread_join(&host->watcher_thread) != 0) {
            return false;
        }
        host->watcher_started = false;
    }
    return true;
}

static bool host_project_lock_manager_free_once(void *context) {
    cbm_project_lock_manager_t **manager = context;
    if (!manager || !*manager) {
        return true;
    }
    (void)cbm_project_lock_manager_free(manager);
    return *manager == NULL;
}

static void host_state_free(host_state_t *host) {
    if (!host_http_stop_join_free(host)) {
        /* The retained server or one of its callbacks can still borrow every
         * dependency below. Process containment is the only safe teardown. */
        host_force_terminate("http_cleanup");
    }
    if (host->application) {
        if (!cbm_daemon_application_free(host->application)) {
            /* The application still owns work or a borrowed callback context.
             * Freeing any dependency below would turn the retained application
             * into UAF. The daemon is the ultimate containment boundary. */
            host_cleanup_force_terminate("application_cleanup");
        }
        host->application = NULL;
    }
    cbm_watcher_free(host->watcher);
    host->watcher = NULL;
    cbm_store_close(host->watch_store);
    host->watch_store = NULL;
    cbm_config_close(host->runtime_config);
    host->runtime_config = NULL;
    if (host->project_locks) {
        host_cleanup_release_until_complete(host_project_lock_manager_free_once,
                                            &host->project_locks, "project_lock_manager_cleanup");
    }
    cbm_secure_zero(host->ui_readiness_secret, sizeof(host->ui_readiness_secret));
}

/* ── B1: durable runtime knobs, read before the memory ceiling is fixed ──
 *
 * cbm_mem_init_with_cap resolves the memory ceiling exactly once, so the
 * config layer (index_memory_budget_mb) has to be staged BEFORE it runs. The
 * order in cbm_daemon_host_run is therefore: cbm_alloc_init (first statement
 * of main, which binds SQLite to mimalloc) → cohort/IPC/lifetime → this
 * read → cbm_mem_init_with_cap → host_state_prepare (which reuses the handle
 * opened here). Opening _config.db on this side of the ceiling is safe and
 * accounted: every SQLite allocation goes through cbm_sqlite_malloc and is
 * charged to the budget by cbm_mem_charged() regardless of when it happened. */

static cbm_daemon_host_config_open_probe_fn g_host_config_open_probe;
static void *g_host_config_open_probe_context;

void cbm_daemon_host_set_config_open_probe_for_test(cbm_daemon_host_config_open_probe_fn probe,
                                                    void *context) {
    g_host_config_open_probe = probe;
    g_host_config_open_probe_context = context;
}

/* C8: the connection cap, resolved once at startup — an explicit CBM_MAX_CLIENTS
 * outranks max_clients in _config.db, which outranks the compiled default.
 *
 * The env layer is deliberately interpreted by the cap's own rule
 * (cbm_daemon_max_clients) rather than re-parsed here: a value that rule would
 * clamp or WARN-drop must keep exactly that behaviour, not gain a second and
 * subtly different reading of the same variable. Presence is probed the way the
 * cap itself reads it (raw getenv, non-empty), so "set" means precisely what it
 * means to cbm_daemon_max_clients() — an empty CBM_MAX_CLIENTS is not set and
 * falls through to the config layer. */
static uint32_t host_resolve_max_clients(cbm_config_t *config,
                                         cbm_daemon_max_clients_source_t *source_out) {
    const char *env_clients = getenv("CBM_MAX_CLIENTS");
    if (env_clients && env_clients[0]) {
        if (source_out) {
            *source_out = CBM_DAEMON_MAX_CLIENTS_SOURCE_ENV;
        }
        return (uint32_t)cbm_daemon_max_clients();
    }

    size_t configured = 0;
    cbm_config_max_clients_status_t status = cbm_config_max_clients(config, &configured);
    if (status == CBM_CONFIG_MAX_CLIENTS_SET) {
        if (source_out) {
            *source_out = CBM_DAEMON_MAX_CLIENTS_SOURCE_CONFIG;
        }
        return (uint32_t)configured;
    }
    if (status == CBM_CONFIG_MAX_CLIENTS_INVALID) {
        /* A hand edit `config set` would have refused. Say which text was
         * rejected and what took its place, like the env layer does. */
        char shown[CBM_SZ_32];
        char fallback[CBM_SZ_32];
        const char *stored = cbm_config_get(config, CBM_CONFIG_MAX_CLIENTS, "");
        (void)snprintf(shown, sizeof(shown), "%s", stored ? stored : "");
        (void)snprintf(fallback, sizeof(fallback), "%d", CBM_DAEMON_MAX_CLIENTS_DEFAULT);
        cbm_log_warn("daemon.max_clients.config_invalid", "value", shown, "using", fallback);
    }
    if (source_out) {
        *source_out = CBM_DAEMON_MAX_CLIENTS_SOURCE_DEFAULT;
    }
    return (uint32_t)CBM_DAEMON_MAX_CLIENTS_DEFAULT;
}

static bool host_runtime_knobs_load(cbm_daemon_host_runtime_knobs_t *knobs,
                                    cbm_config_t **config_out) {
    if (!knobs || !config_out) {
        return false;
    }
    *config_out = NULL;
    memset(knobs, 0, sizeof(*knobs));
    (void)snprintf(knobs->index_workers_text, sizeof(knobs->index_workers_text), "auto");
    knobs->max_clients = (uint32_t)CBM_DAEMON_MAX_CLIENTS_DEFAULT;
    knobs->max_clients_source = CBM_DAEMON_MAX_CLIENTS_SOURCE_DEFAULT;

    const char *cache = cbm_resolve_cache_dir();
    if (!cache || !cache[0]) {
        cbm_log_error("daemon.runtime_config_open_failed", "reason", "cache_dir_unavailable");
        return false;
    }
    if (g_host_config_open_probe != NULL) {
        g_host_config_open_probe(g_host_config_open_probe_context);
    }
    cbm_config_t *config = cbm_config_open(cache);
    if (!config) {
        cbm_log_error("daemon.runtime_config_open_failed", "reason", "config_db_unavailable");
        return false;
    }

    /* Memory: staged for the resolver (explicit CBM_MEM_BUDGET_MB still wins;
     * see cbm_mem_resolve_budget_layers). A stored value the strict parse
     * refuses is not the same as an unset key: the very budget B1 exists to
     * pin would quietly become a RAM fraction, so name the text that was
     * refused and the layer that takes over. `using` is cbm_mem_budget_source's
     * vocabulary — the source `daemon.start` will report. */
    size_t configured_budget_mb = 0;
    cbm_config_index_knob_status_t budget_status =
        cbm_config_index_memory_budget_mb(config, &configured_budget_mb);
    knobs->configured_budget_mb = configured_budget_mb;
    cbm_mem_set_configured_budget_mb(knobs->configured_budget_mb);
    if (budget_status == CBM_CONFIG_INDEX_KNOB_INVALID) {
        char shown[CBM_SZ_32];
        char env_budget[CBM_SZ_32];
        const char *stored = cbm_config_get(config, CBM_CONFIG_INDEX_MEMORY_BUDGET_MB, "");
        (void)snprintf(shown, sizeof(shown), "%s", stored ? stored : "");
        const char *env_layer =
            cbm_safe_getenv("CBM_MEM_BUDGET_MB", env_budget, sizeof(env_budget), NULL);
        cbm_log_warn("daemon.mem.config_invalid", "value", shown, "using",
                     env_layer != NULL ? "CBM_MEM_BUDGET_MB" : "ram_fraction");
    }

    /* Workers: every index worker reads CBM_WORKERS from its own environment
     * and inherits the daemon's, so the daemon pins it here. An existing
     * CBM_WORKERS is the user's explicit choice and is never overwritten
     * (cbm_setenv with overwrite=0), exactly as an explicit CBM_MEM_BUDGET_MB
     * outranks index_memory_budget_mb. */
    char env_buf[CBM_SZ_32];
    size_t workers = 0;
    cbm_config_index_knob_status_t workers_status = cbm_config_index_workers(config, &workers);
    const char *env_workers = cbm_safe_getenv("CBM_WORKERS", env_buf, sizeof(env_buf), NULL);
    if (workers > 0 && env_workers == NULL) {
        char text[CBM_SZ_32];
        (void)snprintf(text, sizeof(text), "%zu", workers);
        knobs->workers_exported = cbm_setenv("CBM_WORKERS", text, 0) == 0;
        env_workers = cbm_safe_getenv("CBM_WORKERS", env_buf, sizeof(env_buf), NULL);
    }

    /* What is reported is what the workers will read back: an inherited
     * CBM_WORKERS in cbm_default_worker_count's accepted range pins them, and
     * everything else (unset, malformed, out of range) is auto-detection —
     * the same fallback the workers themselves take. The parse deliberately
     * mirrors cbm_default_worker_count (strtol, 1..CBM_WORKERS_MAX, lenient
     * about trailing garbage) so this never reports a value the workers would
     * not use. */
    if (env_workers != NULL) {
        long parsed = strtol(env_workers, NULL, 10);
        if (parsed >= 1 && parsed <= CBM_WORKERS_MAX) {
            knobs->index_workers = (size_t)parsed;
            (void)snprintf(knobs->index_workers_text, sizeof(knobs->index_workers_text), "%ld",
                           parsed);
        }
    }
    if (workers_status == CBM_CONFIG_INDEX_KNOB_INVALID) {
        /* Same duty as the budget warning above: say which text was refused
         * and what the workers will run with instead — the number just
         * resolved, or "auto" when nothing pins them. */
        char shown[CBM_SZ_32];
        const char *stored = cbm_config_get(config, CBM_CONFIG_INDEX_WORKERS, "");
        (void)snprintf(shown, sizeof(shown), "%s", stored ? stored : "");
        cbm_log_warn("daemon.workers.config_invalid", "value", shown, "using",
                     knobs->index_workers_text);
    }
    /* C8: the connection cap, with the same one-shot precedence as the two
     * knobs above. */
    knobs->max_clients = host_resolve_max_clients(config, &knobs->max_clients_source);

    *config_out = config;
    return true;
}

bool cbm_daemon_host_runtime_knobs_load_for_test(cbm_daemon_host_runtime_knobs_t *knobs) {
    cbm_config_t *config = NULL;
    cbm_daemon_host_runtime_knobs_t loaded = {0};
    bool loaded_ok = host_runtime_knobs_load(&loaded, &config);
    /* host_runtime_knobs_load initializes the struct to the sentinel defaults
     * before any of its failure returns, so the out-param is always well
     * defined: only the return value says whether startup may proceed. */
    if (knobs != NULL) {
        *knobs = loaded;
    }
    cbm_config_close(config);
    return loaded_ok;
}

static bool host_state_prepare(host_state_t *host, const cbm_daemon_ipc_endpoint_t *endpoint,
                               cbm_config_t *runtime_config) {
    /* Takes ownership of `runtime_config` from the first statement, so every
     * failure path frees it through host_state_free. NULL means "open it here"
     * (the focused test seam); the production path hands over the handle that
     * the B1 startup read opened before the memory ceiling was fixed. */
    host->runtime_config = runtime_config;
    host->http_ops = &g_host_http_default_ops;
    host->http_assets_available = CBM_EMBEDDED_FILE_COUNT > 0;
    if (!cbm_secure_random(host->ui_readiness_secret, sizeof(host->ui_readiness_secret))) {
        cbm_log_error("daemon.readiness_secret_failed", "reason", "system_rng_unavailable");
        return false;
    }
    size_t aggregate_memory_budget_bytes = cbm_mem_budget();
    if (!host->runtime_config) {
        const char *cache = cbm_resolve_cache_dir();
        if (!cache || !cache[0]) {
            cbm_log_error("daemon.runtime_config_open_failed", "reason", "cache_dir_unavailable");
            return false;
        }
        host->runtime_config = cbm_config_open(cache);
        if (!host->runtime_config) {
            cbm_log_error("daemon.runtime_config_open_failed", "reason", "config_db_unavailable");
            return false;
        }
    }
    size_t max_index_jobs = cbm_config_max_index_jobs(host->runtime_config);
    host->project_locks = cbm_project_lock_manager_new(endpoint);
    /* #335: watcher_enabled (default true) is the master switch for the
     * background watcher. When false the daemon never builds the watcher or the
     * in-memory store backing it, so the poll thread never starts
     * (host_background_start has nothing to run) and no project ever registers
     * (register_watcher_if_enabled early-returns on a NULL watcher). The daemon
     * still starts and still owns everything else: IPC, HTTP/UI, project locks,
     * and manual index_repository. Read once here at daemon startup, so a change
     * takes effect when the daemon next starts — see docs/CONFIGURATION.md. */
    bool watcher_enabled = cbm_config_watcher_enabled(host->runtime_config);
    if (watcher_enabled) {
        host->watch_store = cbm_store_open_memory();
        host->watcher = cbm_watcher_new(host->watch_store, host_watcher_index, host);
    } else {
        cbm_log_info("watcher.disabled", "reason", "config");
    }
    cbm_daemon_application_config_t application_config = {
        .watcher = host->watcher,
        .config = host->runtime_config,
        .physical_job_limit = max_index_jobs,
        .aggregate_memory_budget_bytes = aggregate_memory_budget_bytes,
        .project_locks = host->project_locks,
        .ui_readiness_secret = host->ui_readiness_secret,
        .ui_readiness_secret_length = sizeof(host->ui_readiness_secret),
    };
    host->application = cbm_daemon_application_new(&application_config);
    if (host->application && host->permanent) {
        cbm_daemon_application_set_permanent(host->application, true);
    }
    if (!host->project_locks || !host->application) {
        return false;
    }
    /* A watcher deliberately left unbuilt by watcher_enabled=false is not a
     * startup failure; an allocation failure while it is enabled still is. */
    if (watcher_enabled && (!host->watch_store || !host->watcher)) {
        return false;
    }
    return true;
}

bool cbm_daemon_host_state_prepare_for_test(const cbm_daemon_ipc_endpoint_t *endpoint) {
    if (!endpoint) {
        return false;
    }
    host_state_t host = {0};
    bool prepared = host_state_prepare(&host, endpoint, NULL);
    host_state_free(&host);
    return prepared;
}

typedef struct {
    size_t create_failures;
    size_t thread_start_failures;
    size_t server_free_failures;
    size_t config_change_after_load;
    size_t config_loads;
    size_t server_create_attempts;
    size_t thread_start_attempts;
    size_t server_stops;
    size_t server_frees;
    size_t thread_joins;
} host_http_reconcile_test_context_t;

static void host_http_test_config_load(void *opaque, cbm_ui_config_t *config_out) {
    host_http_reconcile_test_context_t *context = opaque;
    context->config_loads++;
    config_out->ui_enabled = true;
    bool change_port = context->config_change_after_load > 0 &&
                       context->config_loads == context->config_change_after_load;
    config_out->ui_port = change_port ? CBM_UI_DEFAULT_PORT + 1 : CBM_UI_DEFAULT_PORT;
}

static cbm_http_server_t *host_http_test_server_new(void *opaque, int port) {
    (void)port;
    host_http_reconcile_test_context_t *context = opaque;
    context->server_create_attempts++;
    return context->server_create_attempts <= context->create_failures
               ? NULL
               : (cbm_http_server_t *)context;
}

static void host_http_test_server_configure(void *opaque, cbm_http_server_t *server,
                                            host_state_t *host) {
    (void)opaque;
    (void)server;
    (void)host;
}

/* WHY: host_http_ops_t is also implemented by production callbacks that mutate
 * the server; this test double must retain that callback ABI even though it
 * only observes whether the opaque fake server is present. */
// cppcheck-suppress constParameterCallback
static void host_http_test_server_stop(void *opaque, cbm_http_server_t *server) {
    if (server) {
        ((host_http_reconcile_test_context_t *)opaque)->server_stops++;
    }
}

/* WHY: See host_http_test_server_stop; changing only this implementation to a
 * pointer-to-const parameter would make it incompatible with host_http_ops_t. */
// cppcheck-suppress constParameterCallback
static bool host_http_test_server_free(void *opaque, cbm_http_server_t *server) {
    host_http_reconcile_test_context_t *context = opaque;
    if (server) {
        context->server_frees++;
    }
    return !server || context->server_frees > context->server_free_failures;
}

static int host_http_test_thread_start(void *opaque, cbm_thread_t *thread, void *(*entry)(void *),
                                       void *entry_context) {
    (void)thread;
    (void)entry;
    (void)entry_context;
    host_http_reconcile_test_context_t *context = opaque;
    context->thread_start_attempts++;
    return context->thread_start_attempts <= context->thread_start_failures ? -1 : 0;
}

static int host_http_test_thread_join(void *opaque, cbm_thread_t *thread) {
    (void)thread;
    host_http_reconcile_test_context_t *context = opaque;
    context->thread_joins++;
    return 0;
}

bool cbm_daemon_host_http_reconcile_sequence_for_test(
    const uint64_t *timestamps_ms, size_t timestamp_count, size_t create_failures,
    size_t thread_start_failures, cbm_daemon_host_http_reconcile_test_result_t *result_out) {
    if (!timestamps_ms || timestamp_count == 0 || !result_out) {
        return false;
    }
    for (size_t i = 1; i < timestamp_count; i++) {
        if (timestamps_ms[i] < timestamps_ms[i - 1]) {
            return false;
        }
    }

    host_http_reconcile_test_context_t context = {
        .create_failures = create_failures,
        .thread_start_failures = thread_start_failures,
    };
    const host_http_ops_t ops = {
        .context = &context,
        .config_load = host_http_test_config_load,
        .server_new = host_http_test_server_new,
        .server_configure = host_http_test_server_configure,
        .server_stop = host_http_test_server_stop,
        .server_free = host_http_test_server_free,
        .thread_start = host_http_test_thread_start,
        .thread_join = host_http_test_thread_join,
    };
    host_state_t host = {
        .http_assets_available = true,
        .http_ops = &ops,
    };
    for (size_t i = 0; i < timestamp_count; i++) {
        host_http_reconcile_at(&host, timestamps_ms[i], i == 0);
    }
    bool active = host.http_started;
    uint32_t largest_retry = host.http_largest_scheduled_retry_ms;
    uint64_t next_retry = host.http_retry_at_ms;
    if (!host_http_stop_join_free(&host)) {
        return false;
    }

    *result_out = (cbm_daemon_host_http_reconcile_test_result_t){
        .config_loads = context.config_loads,
        .server_create_attempts = context.server_create_attempts,
        .thread_start_attempts = context.thread_start_attempts,
        .server_stops = context.server_stops,
        .server_frees = context.server_frees,
        .thread_joins = context.thread_joins,
        .largest_scheduled_retry_ms = largest_retry,
        .next_retry_ms = next_retry,
        .active_after_sequence = active,
    };
    return true;
}

bool cbm_daemon_host_http_reconcile_free_refusal_for_test(
    cbm_daemon_host_http_free_refusal_test_result_t *result_out) {
    if (!result_out) {
        return false;
    }
    host_http_reconcile_test_context_t context = {
        .server_free_failures = 1,
        .config_change_after_load = 2,
    };
    const host_http_ops_t ops = {
        .context = &context,
        .config_load = host_http_test_config_load,
        .server_new = host_http_test_server_new,
        .server_configure = host_http_test_server_configure,
        .server_stop = host_http_test_server_stop,
        .server_free = host_http_test_server_free,
        .thread_start = host_http_test_thread_start,
        .thread_join = host_http_test_thread_join,
    };
    host_state_t host = {
        .http_assets_available = true,
        .http_ops = &ops,
    };

    host_http_reconcile_at(&host, 0, true);
    host_http_reconcile_at(&host, HOST_HTTP_CONFIG_POLL_MS, false);
    bool retained_after_refusal =
        host.http != NULL && !host.http_started && context.server_create_attempts == 1;
    size_t creates_after_refusal = context.server_create_attempts;
    host_http_reconcile_at(&host, HOST_HTTP_CONFIG_POLL_MS * 2U, false);
    bool replacement_active = host.http != NULL && host.http_started;

    *result_out = (cbm_daemon_host_http_free_refusal_test_result_t){
        .server_create_attempts_after_refusal = creates_after_refusal,
        .server_create_attempts = context.server_create_attempts,
        .thread_start_attempts = context.thread_start_attempts,
        .server_stops = context.server_stops,
        .server_free_attempts = context.server_frees,
        .thread_joins = context.thread_joins,
        .retained_after_refusal = retained_after_refusal,
        .replacement_active_after_retry = replacement_active,
    };
    return host_http_stop_join_free(&host);
}

typedef struct {
    cbm_http_server_t *server;
    bool free_refused_while_scheduled;
} host_http_thread_create_failure_test_t;

/* WHY: the signature is fixed by the host_http_ops_t thread_start hook type;
 * a const pointee would make the function-pointer types incompatible. */
static int host_http_thread_create_failure_for_test(
    void *opaque, cbm_thread_t *thread, void *(*entry)(void *),
    void *entry_context) { // cppcheck-suppress constParameterCallback
    (void)thread;
    (void)entry;
    host_http_thread_create_failure_test_t *test = opaque;
    if (!test || entry_context != test->server)
        return -1;
    test->free_refused_while_scheduled = !cbm_http_server_free(test->server);
    return -1;
}

bool cbm_daemon_host_http_thread_create_failure_lifecycle_for_test(void) {
    cbm_http_server_t *server = cbm_http_server_new(0);
    if (!server)
        return false;
    host_http_thread_create_failure_test_t test = {
        .server = server,
    };
    cbm_thread_t unused_thread = {0};
    int rc = host_http_thread_schedule_create(&unused_thread, host_http_thread, server,
                                              host_http_thread_create_failure_for_test, &test);
    bool free_refused = test.free_refused_while_scheduled;
    bool final_free_succeeded = free_refused && cbm_http_server_free(server);
    return rc != 0 && free_refused && final_free_succeeded;
}

static bool host_background_start(host_state_t *host) {
    /* No watcher object when watcher_enabled=false (#335) — nothing to run, and
     * the daemon must still come up with its remaining subsystems. */
    if (host->watcher) {
        if (cbm_thread_create(&host->watcher_thread, 0, host_watcher_thread, host->watcher) != 0) {
            return false;
        }
        host->watcher_started = true;
    }

    host_http_reconcile_at(host, cbm_now_ms(), true);
    return true;
}

/* ── Runtime-directory upkeep ──
 *
 * The names this generation holds in the runtime directory, each kept by its
 * owner: the listener (inside the runtime service) holds the lifetime
 * reservation and the identity record; the host itself holds the participant
 * guard, the cohort lease and the daemon claim. See ipc.h for why age-based
 * cleaners would otherwise reap them under a daemon that runs for days. */
typedef struct {
    const cbm_daemon_ipc_endpoint_t *endpoint;
    const cbm_daemon_ipc_participant_guard_t *participant_guard;
    const cbm_version_cohort_daemon_claim_t *daemon_claim;
    const cbm_version_cohort_lease_t *cohort_lease;
    uint64_t keepalive_interval_ms;
    uint64_t keepalive_last_wall_ms;
    bool keepalive_done;
    bool keepalive_failure_logged;
    uint64_t names_check_ms;
    uint64_t names_next_check_ms;
    unsigned names_io_streak;
} host_runtime_dir_t;

/* Wall-clock milliseconds since the epoch, 0 if unavailable. The keepalive is
 * paced by the wall clock rather than cbm_now_ms(): cleaners age files by wall
 * time, and the monotonic clock stops while the machine sleeps, so the first
 * tick after a long suspend has to see the whole gap. */
static uint64_t host_wall_clock_ms(void) {
#ifdef _WIN32
    time_t now = time(NULL);
    return now > 0 ? (uint64_t)now * CBM_MSEC_PER_SEC : 0;
#else
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) != 0 || now.tv_sec < 0) {
        return 0;
    }
    return (uint64_t)now.tv_sec * CBM_MSEC_PER_SEC + (uint64_t)now.tv_nsec / CBM_NSEC_PER_MSEC;
#endif
}

#ifdef CBM_ENABLE_TEST_SEAMS
/* Test builds may shorten a host cadence. Only a whole number of milliseconds
 * in [HOST_WAIT_TICK_MS, ceiling_ms] is honoured; anything else keeps the
 * production value. */
static uint64_t host_cadence_seam_ms(const char *name, uint64_t production_ms,
                                     uint64_t ceiling_ms) {
    char text[32];
    const char *configured = cbm_safe_getenv(name, text, sizeof(text), NULL);
    if (configured) {
        char *end = NULL;
        errno = 0;
        unsigned long long parsed = strtoull(configured, &end, 10);
        if (errno == 0 && end && end != configured && *end == '\0' &&
            parsed >= HOST_WAIT_TICK_MS && parsed <= ceiling_ms) {
            return (uint64_t)parsed;
        }
    }
    return production_ms;
}
#endif

static uint64_t host_keepalive_interval_ms(void) {
    uint64_t interval = (uint64_t)HOST_KEEPALIVE_INTERVAL_S * CBM_MSEC_PER_SEC;
#ifdef CBM_ENABLE_TEST_SEAMS
    interval = host_cadence_seam_ms("CBM_TEST_KEEPALIVE_MS", interval, interval);
#endif
    return interval;
}

/* Evaluated every tick: refresh on the first one, once the interval has passed
 * on the wall clock, and whenever that clock went backwards. A failure is
 * logged once and never stops the daemon; the next complete refresh re-arms
 * the log, so a new episode shows up without a persistent one repeating
 * hourly. */
static void host_runtime_dir_keepalive_at(host_runtime_dir_t *dir,
                                          cbm_daemon_runtime_service_t *service,
                                          uint64_t now_wall_ms) {
    bool due = !dir->keepalive_done || now_wall_ms < dir->keepalive_last_wall_ms ||
               now_wall_ms - dir->keepalive_last_wall_ms >= dir->keepalive_interval_ms;
    if (!due) {
        return;
    }
    dir->keepalive_done = true;
    dir->keepalive_last_wall_ms = now_wall_ms;

    cbm_daemon_ipc_keepalive_failure_t first;
    cbm_daemon_ipc_keepalive_failure_t failure;
    bool refreshed =
        cbm_daemon_ipc_listener_keepalive(cbm_daemon_runtime_service_listener(service), &first);
    if (!cbm_daemon_ipc_participant_guard_keepalive(dir->participant_guard, &failure)) {
        if (refreshed) {
            first = failure;
        }
        refreshed = false;
    }
    if (!cbm_version_cohort_daemon_keepalive(dir->daemon_claim, dir->cohort_lease, &failure)) {
        if (refreshed) {
            first = failure;
        }
        refreshed = false;
    }
    if (refreshed) {
        dir->keepalive_failure_logged = false;
        return;
    }
    if (dir->keepalive_failure_logged) {
        return;
    }
    dir->keepalive_failure_logged = true;
    const char *errno_name = first.errno_value != 0 ? cbm_errno_name(first.errno_value) : NULL;
    cbm_log_warn("daemon.runtime_dir.keepalive_failed", "name", first.name, "errno",
                 errno_name ? errno_name : "none");
}

static uint64_t host_names_check_ms(void) {
    uint64_t period = HOST_NAMES_CHECK_MS;
#ifdef CBM_ENABLE_TEST_SEAMS
    period = host_cadence_seam_ms("CBM_TEST_NAMES_INTACT_MS", period,
                                  HOST_NAMES_CHECK_SEAM_CEILING_MS);
#endif
    return period;
}

/* One observation over every owner. A replaced or missing name outranks IO
 * from another owner: it is terminal, and IO only counts when it repeats. */
static cbm_daemon_ipc_names_state_t host_runtime_dir_names_state(
    const host_runtime_dir_t *dir, cbm_daemon_runtime_service_t *service) {
    cbm_daemon_ipc_names_state_t observed[] = {
        cbm_daemon_ipc_listener_names_intact(cbm_daemon_runtime_service_listener(service)),
        cbm_daemon_ipc_participant_guard_names_intact(dir->participant_guard, dir->endpoint),
        cbm_version_cohort_daemon_names_intact(dir->daemon_claim, dir->cohort_lease),
    };
    cbm_daemon_ipc_names_state_t combined = CBM_DAEMON_IPC_NAMES_INTACT;
    for (size_t index = 0; index < sizeof(observed) / sizeof(observed[0]); index++) {
        if (observed[index] == CBM_DAEMON_IPC_NAMES_IO) {
            combined = CBM_DAEMON_IPC_NAMES_IO;
        } else if (observed[index] != CBM_DAEMON_IPC_NAMES_INTACT) {
            return observed[index];
        }
    }
    return combined;
}

/* Whether this generation must stop serving. Checked once per period, only
 * while the runtime is RUNNING: every name was created by this process before
 * the loop began, so any discrepancy is external, and a new generation can
 * only publish under absent names, so none can be racing this one yet. A
 * replaced or missing name acts on its first observation; IO only after
 * HOST_NAMES_IO_STREAK_LIMIT in a row. Leaving, never relocking or
 * republishing in place, is the one move that cannot race a legitimate
 * starter: if one exists it wins, if not the next client launches one. */
static bool host_runtime_dir_orphaned_at(host_runtime_dir_t *dir,
                                         cbm_daemon_runtime_service_t *service, uint64_t now_ms) {
    if (now_ms < dir->names_next_check_ms) {
        return false;
    }
    dir->names_next_check_ms = host_deadline_from(now_ms, (uint32_t)dir->names_check_ms);
    cbm_daemon_ipc_names_state_t state = host_runtime_dir_names_state(dir, service);
    if (state == CBM_DAEMON_IPC_NAMES_INTACT) {
        dir->names_io_streak = 0;
        return false;
    }
    if (state == CBM_DAEMON_IPC_NAMES_IO && ++dir->names_io_streak < HOST_NAMES_IO_STREAK_LIMIT) {
        return false;
    }
    cbm_log_warn("daemon.runtime_dir.orphaned", "detail", cbm_daemon_ipc_names_state_name(state));
    return true;
}

/* How a generation's lifetime ended; the first four are the `reason`s of
 * daemon.lifetime_end. */
typedef enum {
    HOST_END_RUNTIME_EXITED,
    HOST_END_STOP_REQUESTED,
    HOST_END_INITIAL_WINDOW,
    HOST_END_ORPHANED,
    HOST_END_SHUTDOWN_TIMEOUT,
} host_lifetime_end_t;

static host_lifetime_end_t host_wait_for_lifetime(cbm_daemon_runtime_service_t *service,
                                                  atomic_int *stop_requested, host_state_t *host,
                                                  bool permanent,
                                                  host_runtime_dir_t *runtime_dir) {
    uint64_t initial_deadline = cbm_now_ms() + HOST_INITIAL_CLIENT_TIMEOUT_MS;
    uint64_t stopping_deadline = 0;
    bool orphaned = false;
    for (;;) {
        cbm_daemon_runtime_service_state_t state = cbm_daemon_runtime_service_state(service);
        if (state == CBM_DAEMON_RUNTIME_SERVICE_EXITED) {
            /* An orphaned generation ends through the ordinary stop below;
             * the reason is the host's, recorded when it decided to leave. */
            cbm_log_info("daemon.lifetime_end", "reason",
                         orphaned ? "runtime_dir_orphaned" : "runtime_exited");
            return orphaned ? HOST_END_ORPHANED : HOST_END_RUNTIME_EXITED;
        }
        if (state == CBM_DAEMON_RUNTIME_SERVICE_STOPPING) {
            if (stopping_deadline == 0) {
                stopping_deadline = cbm_now_ms() + HOST_RUNTIME_SHUTDOWN_MS;
            }
            if (cbm_now_ms() >= stopping_deadline) {
                return HOST_END_SHUTDOWN_TIMEOUT;
            }
            (void)cbm_daemon_runtime_service_wait_exited(service, HOST_WAIT_TICK_MS);
            continue;
        }
        /* The nobody-ever-connected window latches on the monotonic admission
         * total, not the live client count: a short first session can begin
         * and end entirely between two polls of this loop, and a sampled
         * count then reads zero forever — stopping a daemon whose client is
         * mid-conversation between per-request connections. */
        bool admitted = cbm_daemon_runtime_service_clients_admitted_total(service) > 0;
        bool stop = stop_requested && atomic_load(stop_requested);
        /* A permanent generation (`daemon start`) skips the nobody-ever-
         * connected window — it exists precisely to idle ahead of clients.
         * Explicit stop requests (signals) are still honored. */
        if (stop || (!permanent && !admitted && cbm_now_ms() >= initial_deadline)) {
            cbm_log_info("daemon.lifetime_end", "reason",
                         stop ? "stop_requested" : "initial_window_expired");
            if (!cbm_daemon_runtime_service_stop(service, HOST_RUNTIME_SHUTDOWN_MS)) {
                return HOST_END_SHUTDOWN_TIMEOUT;
            }
            return stop ? HOST_END_STOP_REQUESTED : HOST_END_INITIAL_WINDOW;
        }
        host_runtime_dir_keepalive_at(runtime_dir, service, host_wall_clock_ms());
        if (host_runtime_dir_orphaned_at(runtime_dir, service, cbm_now_ms())) {
            /* Not an emergency stop: admission ends, sessions are cancelled
             * and given the usual bound to unwind, and the listener removes
             * only the names that are still its own. */
            orphaned = true;
            cbm_daemon_runtime_service_begin_stop(service, HOST_RUNTIME_SHUTDOWN_MS,
                                                  "runtime_dir_orphaned");
            continue;
        }
        host_http_reconcile_at(host, cbm_now_ms(), false);
        host_metrics_flush_at(host, cbm_now_ms());
        /* Retire an ephemeral generation that lingered for cold-storm cohort
         * participants once they drain, or once its bounded linger elapses.
         * A no-op unless a cohort-participant hook armed a linger. */
        cbm_daemon_runtime_service_reconcile_lifetime(service);
        (void)cbm_daemon_runtime_service_wait_exited(service, HOST_WAIT_TICK_MS);
    }
}

static bool host_application_shutdown(host_state_t *host) {
    if (cbm_daemon_application_shutdown(host->application, HOST_APPLICATION_SHUTDOWN_MS)) {
        return true;
    }
    cbm_log_error("daemon.shutdown_timeout", "component", "operations");
    return false;
}

static bool host_runtime_stop_free(cbm_daemon_runtime_service_t *service) {
    if (cbm_daemon_runtime_service_state(service) != CBM_DAEMON_RUNTIME_SERVICE_EXITED) {
        if (!cbm_daemon_runtime_service_stop(service, HOST_RUNTIME_SHUTDOWN_MS)) {
            cbm_log_error("daemon.shutdown_timeout", "component", "runtime");
            return false;
        }
    }
    if (!cbm_daemon_runtime_service_free(service)) {
        cbm_log_error("daemon.cleanup_timeout", "component", "runtime");
        return false;
    }
    return true;
}

static _Noreturn void host_force_terminate(const char *component) {
    /* The operation-log sink flushes every record before returning. Do not run
     * ordinary teardown after the cooperative deadline: a callback thread may
     * still own application storage. Process termination releases native
     * locks, and supervised worker groups observe their daemon parent's death. */
    cbm_log_error("daemon.forced_shutdown", "component", component);
#ifdef _WIN32
    (void)TerminateProcess(GetCurrentProcess(), EXIT_FAILURE);
    abort();
#else
    _exit(EXIT_FAILURE);
#endif
}

_Noreturn void cbm_daemon_host_force_terminate_for_test(const char *component) {
    char conflict_log[HOST_PATH_CAP];
    cbm_ui_log_init();
    if (host_log_open(conflict_log)) {
        host_force_terminate(component ? component : "test");
    }
#ifdef _WIN32
    (void)TerminateProcess(GetCurrentProcess(), EXIT_FAILURE);
    abort();
#else
    _exit(EXIT_FAILURE);
#endif
}

unsigned long cbm_daemon_host_descriptor_budget(uint32_t max_clients) {
    unsigned long desired = (unsigned long)max_clients * HOST_FD_PER_CLIENT + HOST_FD_BASE;
    return desired < HOST_FD_FLOOR ? (unsigned long)HOST_FD_FLOOR : desired;
}

cbm_daemon_host_fd_verdict_t cbm_daemon_host_classify_descriptor_limit(uint32_t max_clients,
                                                                       unsigned long desired,
                                                                       unsigned long effective) {
    if (effective == 0) {
        return CBM_DAEMON_HOST_FD_UNKNOWN;
    }
    if (effective >= desired) {
        return CBM_DAEMON_HOST_FD_SUFFICIENT;
    }
    /* The floor the configured cap can actually reach: one descriptor per
     * connection plus the daemon's own files. Below this the cap is a promise
     * the process cannot keep — it starts failing its own open() and accept()
     * calls with EMFILE before the connection cap is ever reached, and from
     * outside that reads as file corruption, not as "too many clients". */
    unsigned long minimum = (unsigned long)max_clients + HOST_FD_BASE;
    return effective < minimum ? CBM_DAEMON_HOST_FD_INSUFFICIENT : CBM_DAEMON_HOST_FD_PARTIAL;
}

/* Raise the descriptor limit to match the connection cap, and say in the log
 * what it settled on.
 *
 * This runs in the daemon role itself rather than in the launchd plist because
 * the daemon is also started by hand (`codebase-memory-mcp daemon start`) and
 * by the client bootstrap, neither of which goes through launchd. A plist edit
 * would cover one of the three.
 *
 * Never fails startup: a daemon that cannot raise its limit should still serve
 * whatever the limit it has allows, and refusing to start would take down the
 * whole fleet's index over a tunable. */
static void host_prepare_descriptor_limit(uint32_t max_clients) {
    unsigned long desired = cbm_daemon_host_descriptor_budget(max_clients);
    unsigned long effective = cbm_raise_open_file_limit(desired);
    cbm_daemon_host_fd_verdict_t verdict =
        cbm_daemon_host_classify_descriptor_limit(max_clients, desired, effective);
    if (verdict == CBM_DAEMON_HOST_FD_UNKNOWN) {
        /* Nothing to promise and nothing to warn about. */
        return;
    }

    char clients_text[32];
    char desired_text[32];
    char effective_text[32];
    (void)snprintf(clients_text, sizeof(clients_text), "%u", max_clients);
    (void)snprintf(desired_text, sizeof(desired_text), "%lu", desired);
    (void)snprintf(effective_text, sizeof(effective_text), "%lu", effective);

    if (verdict == CBM_DAEMON_HOST_FD_INSUFFICIENT) {
        char minimum_text[32];
        (void)snprintf(minimum_text, sizeof(minimum_text), "%lu",
                       (unsigned long)max_clients + HOST_FD_BASE);
        cbm_log_warn("daemon.descriptor_limit.insufficient", "max_clients", clients_text,
                     "soft_limit", effective_text, "minimum_needed", minimum_text, "requested",
                     desired_text);
        return;
    }
    cbm_log_info(verdict == CBM_DAEMON_HOST_FD_SUFFICIENT ? "daemon.descriptor_limit"
                                                          : "daemon.descriptor_limit.partial",
                 "max_clients", clients_text, "requested", desired_text, "soft_limit",
                 effective_text);
}

int cbm_daemon_host_run(const cbm_daemon_host_config_t *config) {
    if (!config || !config->endpoint || !config->executable_path ||
        !config->identity.semantic_version || !config->identity.build_fingerprint) {
        return -1;
    }
    cbm_ui_log_init();
    char conflict_log[HOST_PATH_CAP];
    if (!host_log_open(conflict_log)) {
        (void)fprintf(stderr, "codebase-memory: daemon log path is not private or safe\n");
        return -1;
    }
    cbm_version_cohort_manager_t *cohort_manager = cbm_version_cohort_manager_new(config->endpoint);
    cbm_version_cohort_lease_t *cohort_lease = NULL;
    cbm_daemon_conflict_t cohort_conflict;
    uint64_t now_ms = cbm_now_ms();
    uint64_t cohort_deadline = now_ms > UINT64_MAX - HOST_INITIAL_CLIENT_TIMEOUT_MS
                                   ? UINT64_MAX
                                   : now_ms + HOST_INITIAL_CLIENT_TIMEOUT_MS;
    cbm_version_cohort_status_t cohort_status =
        cohort_manager
            ? cbm_version_cohort_acquire(cohort_manager, &config->identity, cohort_deadline,
                                         &cohort_lease, &cohort_conflict)
            : CBM_VERSION_COHORT_IO;
    if (cohort_status != CBM_VERSION_COHORT_OK) {
        cbm_log_error("daemon.start_failed", "component", "cohort");
        char message[CBM_DAEMON_CONFLICT_MESSAGE_SIZE];
        bool formatted = cohort_status == CBM_VERSION_COHORT_CONFLICT &&
                         cbm_daemon_conflict_format(&cohort_conflict, message, sizeof(message));
        if (cohort_status == CBM_VERSION_COHORT_CONFLICT) {
            (void)cbm_version_cohort_log_conflict(&cohort_conflict);
        }
        (void)fprintf(stderr, "codebase-memory: %s\n",
                      formatted ? message : "daemon exact-build admission failed");
        host_cohort_close(&cohort_lease, &cohort_manager);
        host_log_close();
        return -1;
    }
    cbm_daemon_ipc_participant_guard_t *participant_guard = NULL;
    if (cbm_daemon_ipc_participant_guard_try_join(config->endpoint, &participant_guard) != 1) {
        cbm_log_error("daemon.start_failed", "component", "participant");
        (void)fprintf(stderr, "codebase-memory: daemon participant admission failed\n");
        host_participant_guard_close(&participant_guard);
        host_cohort_close(&cohort_lease, &cohort_manager);
        host_log_close();
        return -1;
    }
    cbm_version_cohort_daemon_claim_t *daemon_claim = NULL;
    uint64_t claim_deadline = host_deadline_after(HOST_DAEMON_CLAIM_TIMEOUT_MS);
    cbm_version_cohort_status_t claim_status =
        cbm_version_cohort_daemon_claim_acquire(cohort_manager, &daemon_claim);
    while (claim_status == CBM_VERSION_COHORT_BUSY && cbm_now_ms() < claim_deadline) {
        /* The previous generation is releasing; retry until the claim is free
         * or the handoff window elapses (a persistent BUSY = a real conflict). */
        cbm_usleep(HOST_WAIT_TICK_MS * 1000);
        claim_status = cbm_version_cohort_daemon_claim_acquire(cohort_manager, &daemon_claim);
    }
    if (claim_status != CBM_VERSION_COHORT_OK) {
        cbm_log_error("daemon.start_failed", "component", "claim");
        host_participant_guard_close(&participant_guard);
        host_daemon_claim_close(&daemon_claim);
        host_cohort_close(&cohort_lease, &cohort_manager);
        host_log_close();
        return -1;
    }
    cbm_daemon_ipc_lifetime_reservation_t *lifetime_reservation = NULL;
    if (host_lifetime_reservation_acquire(config->endpoint, &lifetime_reservation) != 1) {
        cbm_log_error("daemon.start_failed", "component", "lifetime");
        cbm_daemon_ipc_lifetime_reservation_release(lifetime_reservation);
        host_participant_guard_close(&participant_guard);
        host_daemon_claim_close(&daemon_claim);
        host_cohort_close(&cohort_lease, &cohort_manager);
        host_log_close();
        return -1;
    }
    /* B1: read the durable runtime knobs BEFORE the ceiling is fixed —
     * cbm_mem_init resolves the budget once, with the config layer staged by
     * this read (explicit CBM_MEM_BUDGET_MB still wins over it). The handle it
     * opens is reused below for watcher_enabled/max_index_jobs. */
    cbm_daemon_host_runtime_knobs_t knobs = {0};
    cbm_config_t *knob_config = NULL;
    if (!host_runtime_knobs_load(&knobs, &knob_config)) {
        cbm_log_error("daemon.start_failed", "component", "application");
        host_log_close();
        cbm_daemon_ipc_lifetime_reservation_release(lifetime_reservation);
        host_participant_guard_close(&participant_guard);
        host_cohort_close(&cohort_lease, &cohort_manager);
        return -1;
    }
    cbm_mem_init(cbm_mem_ram_fraction_for_total(cbm_system_info().total_ram));
    cbm_http_server_set_binary_path(config->executable_path);
    cbm_index_supervisor_mark_host();

    host_state_t host = {0};
    host.permanent = config->permanent;
    if (!host_state_prepare(&host, config->endpoint, knob_config)) {
        cbm_log_error("daemon.start_failed", "component", "application");
        host_state_free(&host);
        host_log_close();
        cbm_daemon_ipc_lifetime_reservation_release(lifetime_reservation);
        host_participant_guard_close(&participant_guard);
        host_daemon_claim_close(&daemon_claim);
        host_cohort_close(&cohort_lease, &cohort_manager);
        return -1;
    }
    cbm_daemon_runtime_application_callbacks_t callbacks =
        cbm_daemon_application_runtime_callbacks(host.application);
    /* Resolved by the startup read above, so the descriptor limit is raised
     * for the cap this generation will actually run with rather than for a
     * compiled-in guess. */
    uint32_t max_clients = knobs.max_clients;
    host_prepare_descriptor_limit(max_clients);
    cbm_daemon_runtime_service_config_t runtime_config = {
        .endpoint = config->endpoint,
        .identity = config->identity,
        .conflict_log_path = conflict_log,
        .conflict_log_cap_bytes = HOST_CONFLICT_LOG_CAP,
        .max_clients = max_clients,
        .max_clients_source = knobs.max_clients_source,
        .lease_timeout_ms = HOST_LEASE_TIMEOUT_MS,
        .request_timeout_ms = HOST_REQUEST_TIMEOUT_MS,
        .shutdown_timeout_ms = HOST_RUNTIME_SHUTDOWN_MS,
        .application = callbacks,
        .permanent = config->permanent,
        /* B1: the ceiling/parallelism this generation runs with, reported by
         * `daemon status` (daemon.start logs the same values). */
        .budget_status = {.memory_budget_bytes = cbm_mem_budget(),
                          .index_workers = (uint32_t)knobs.index_workers,
                          .budget_source =
                              cbm_daemon_budget_source_from_name(cbm_mem_budget_source())},
    };
    cbm_daemon_runtime_service_t *service =
        cbm_daemon_runtime_service_start_reserved(&runtime_config, &lifetime_reservation);
    if (!service) {
        cbm_log_error("daemon.start_failed", "component", "runtime");
        /* Recorded while the lifetime reservation is still held: a client
         * that watches this generation vanish finds the cause already there
         * and never launches a doomed replacement. */
        host_start_failure_record(config->endpoint, "runtime");
    }
    cbm_daemon_ipc_lifetime_reservation_release(lifetime_reservation);
    lifetime_reservation = NULL;
    if (!service) {
        host_state_free(&host);
        host_log_close();
        host_participant_guard_close(&participant_guard);
        host_daemon_claim_close(&daemon_claim);
        host_cohort_close(&cohort_lease, &cohort_manager);
        return -1;
    }
    if (!host_background_start(&host)) {
        cbm_log_error("daemon.start_failed", "component", "background");
        if (!host_application_shutdown(&host)) {
            host_force_terminate("operations");
        }
        host_background_stop(&host);
        if (!host_background_join(&host)) {
            host_force_terminate("background");
        }
        if (!host_runtime_stop_free(service)) {
            host_force_terminate("runtime");
        }
        host_state_free(&host);
        host_log_close();
        host_participant_guard_close(&participant_guard);
        host_daemon_claim_close(&daemon_claim);
        host_cohort_close(&cohort_lease, &cohort_manager);
        return -1;
    }
    cbm_diag_start();

    char process_id[32];
    char memory_budget[32];
    char physical_job_limit[32];
    char worker_memory_budget[32];
    char max_clients_text[32];
    (void)snprintf(process_id, sizeof(process_id), "%llu",
                   (unsigned long long)host_current_process_id());
    (void)snprintf(memory_budget, sizeof(memory_budget), "%zu", cbm_mem_budget());
    (void)snprintf(physical_job_limit, sizeof(physical_job_limit), "%zu",
                   cbm_daemon_application_physical_job_limit(host.application));
    (void)snprintf(worker_memory_budget, sizeof(worker_memory_budget), "%zu",
                   cbm_daemon_application_worker_memory_budget_bytes(host.application));
    (void)snprintf(max_clients_text, sizeof(max_clients_text), "%u", knobs.max_clients);
    cbm_log_info("daemon.start", "version", config->identity.semantic_version, "pid", process_id,
                 "cache_fingerprint",
                 config->identity.cache_fingerprint ? config->identity.cache_fingerprint
                                                    : "unavailable",
                 "memory_budget_bytes", memory_budget, "physical_job_limit", physical_job_limit,
                 "worker_memory_budget_bytes", worker_memory_budget, "budget_source",
                 cbm_mem_budget_source(), "index_workers", knobs.index_workers_text,
                 /* C8: the cap this generation admits and which layer chose
                  * it — the same values `daemon status` reports. */
                 "max_clients", max_clients_text, "max_clients_source",
                 cbm_daemon_max_clients_source_name(knobs.max_clients_source));
    host_runtime_dir_t runtime_dir = {
        .endpoint = config->endpoint,
        .participant_guard = participant_guard,
        .daemon_claim = daemon_claim,
        .cohort_lease = cohort_lease,
        .keepalive_interval_ms = host_keepalive_interval_ms(),
        .names_check_ms = host_names_check_ms(),
    };
    host_lifetime_end_t lifetime_end = host_wait_for_lifetime(
        service, config->stop_requested, &host, config->permanent, &runtime_dir);
    if (lifetime_end == HOST_END_SHUTDOWN_TIMEOUT) {
        host_force_terminate("runtime");
    }

    /* Prevent any new UI/watcher operation, then cancel/reap every physical
     * job before those background loops and the daemon process disappear. */
    if (!host_application_shutdown(&host)) {
        host_force_terminate("operations");
    }
    host_background_stop(&host);
    if (!host_background_join(&host)) {
        host_force_terminate("background");
    }
    if (!host_runtime_stop_free(service)) {
        host_force_terminate("runtime");
    }
    host_state_free(&host);
    cbm_diag_stop();
    cbm_log_info("daemon.stop");
    host_log_close();
    /* Keep the exact-build lifetime lease through listener/lifetime teardown,
     * application destruction, diagnostics shutdown, the durable stop record,
     * participant release, and daemon-claim release. An activation transaction
     * may treat exclusive acquisition as proof that coordinated cleanup for
     * this generation has completed. */
    host_participant_guard_close(&participant_guard);
    host_daemon_claim_close(&daemon_claim);
    host_cohort_close(&cohort_lease, &cohort_manager);
    return lifetime_end == HOST_END_ORPHANED ? CBM_DAEMON_HOST_EXIT_RUNTIME_DIR_ORPHANED : 0;
}
