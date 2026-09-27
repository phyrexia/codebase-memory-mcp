/*
 * host_internal.h — Private daemon-host test seams.
 */
#ifndef CBM_DAEMON_HOST_INTERNAL_H
#define CBM_DAEMON_HOST_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "foundation/limits.h"

struct cbm_daemon_ipc_endpoint;

typedef bool (*cbm_daemon_host_cleanup_release_for_test_fn)(void *context);

/* Runs the same finite retry/fail-stop driver used by daemon-claim shutdown
 * around an injected release operation. The hook is process-local and inert
 * unless a focused subprocess test calls this seam. */
void cbm_daemon_host_cleanup_release_until_complete_for_test(
    cbm_daemon_host_cleanup_release_for_test_fn release, void *context);

/* Opens the ordinary private daemon operation log, records the same forced
 * shutdown event used by production, flushes it, and terminates the process.
 * This exists only so an isolated child can verify the hard escape path. */
_Noreturn void cbm_daemon_host_force_terminate_for_test(const char *component);

/* Focused host-state probe. It performs no listener publication and starts no
 * background thread; the production preparation and cleanup paths are used
 * verbatim so tests can make the runtime config database unavailable. */
bool cbm_daemon_host_state_prepare_for_test(const struct cbm_daemon_ipc_endpoint *endpoint);

/* ── B1/C8: durable runtime knobs read before the memory ceiling is fixed ── */

typedef struct {
    size_t configured_budget_mb; /* index_memory_budget_mb as stored; 0 = off */
    size_t index_workers;        /* effective pinned workers; 0 = auto */
    char index_workers_text[32]; /* "auto" | decimal — the daemon.start field */
    bool workers_exported;       /* this startup wrote CBM_WORKERS */
    /* C8: the connection cap this generation admits and where it came from
     * (explicit CBM_MAX_CLIENTS > max_clients in _config.db > the compiled
     * default). Resolved once here so the descriptor limit is raised for the
     * cap that will actually run, and `daemon status` reports it as settled
     * rather than re-resolving this process's config and environment. */
    uint32_t max_clients;
    cbm_daemon_max_clients_source_t max_clients_source;
} cbm_daemon_host_runtime_knobs_t;

/* Observation hook fired at the instant the startup read opens _config.db, so
 * a test can pin the startup order (SQLite already initialized on our
 * allocator before the open; the memory ceiling still unset at that point). */
typedef void (*cbm_daemon_host_config_open_probe_fn)(void *context);
void cbm_daemon_host_set_config_open_probe_for_test(cbm_daemon_host_config_open_probe_fn probe,
                                                    void *context);

/* Runs the production startup read verbatim: opens _config.db, stages
 * index_memory_budget_mb for the memory core, and exports CBM_WORKERS for the
 * workers to inherit (never overwriting an explicit value). The handle it
 * opens is closed before returning. */
bool cbm_daemon_host_runtime_knobs_load_for_test(cbm_daemon_host_runtime_knobs_t *knobs);

typedef struct {
    size_t config_loads;
    size_t server_create_attempts;
    size_t thread_start_attempts;
    size_t server_stops;
    size_t server_frees;
    size_t thread_joins;
    uint32_t largest_scheduled_retry_ms;
    uint64_t next_retry_ms;
    bool active_after_sequence;
} cbm_daemon_host_http_reconcile_test_result_t;

typedef struct {
    size_t server_create_attempts_after_refusal;
    size_t server_create_attempts;
    size_t thread_start_attempts;
    size_t server_stops;
    size_t server_free_attempts;
    size_t thread_joins;
    bool retained_after_refusal;
    bool replacement_active_after_retry;
} cbm_daemon_host_http_free_refusal_test_result_t;

/* Drive the production HTTP reconciliation state machine at explicit
 * monotonic timestamps with deterministic transient failures. */
bool cbm_daemon_host_http_reconcile_sequence_for_test(
    const uint64_t *timestamps_ms, size_t timestamp_count, size_t create_failures,
    size_t thread_start_failures, cbm_daemon_host_http_reconcile_test_result_t *result_out);

/* Change the configured UI port for one poll under an active fake server,
 * inject one server-free refusal, revert the desired port, then drive the
 * next retry. */
bool cbm_daemon_host_http_reconcile_free_refusal_for_test(
    cbm_daemon_host_http_free_refusal_test_result_t *result_out);

/* Exercise the production HTTP schedule/create/cancel adapter with an injected
 * create failure. The callback verifies free is refused while SCHEDULED; after
 * the failure, the adapter must cancel so the final free succeeds. */
bool cbm_daemon_host_http_thread_create_failure_lifecycle_for_test(void);

/* ── Descriptor budget for the connection cap ────────────────────
 *
 * Split out from the startup path because the decision is the whole point and
 * the syscall is not: raising the real RLIMIT_NOFILE inside a test process
 * changes a process-wide limit that every other test shares. These two are
 * pure, so the production warning and the tests reason about the same rule. */

typedef enum {
    /* No per-process cap to manage, or it could not be read. */
    CBM_DAEMON_HOST_FD_UNKNOWN = 0,
    /* The soft cap covers the full budget asked for. */
    CBM_DAEMON_HOST_FD_SUFFICIENT,
    /* Short of the budget, but still above what the connection cap needs to be
     * reachable — headroom is thinner than wanted, nothing is broken. */
    CBM_DAEMON_HOST_FD_PARTIAL,
    /* Below what the configured connection cap needs. The daemon will fail its
     * own open()/accept() calls with EMFILE before the cap is ever reached. */
    CBM_DAEMON_HOST_FD_INSUFFICIENT,
} cbm_daemon_host_fd_verdict_t;

/* Descriptors the given connection cap asks for: a per-connection allowance
 * over a fixed base for the daemon's own files, never below a floor that
 * clears the 256 launchd hands an unconfigured daemon. */
unsigned long cbm_daemon_host_descriptor_budget(uint32_t max_clients);

/* Classify an in-force soft cap against the budget for `max_clients`.
 * `effective` is what cbm_raise_open_file_limit reported (0 for unknown). */
cbm_daemon_host_fd_verdict_t cbm_daemon_host_classify_descriptor_limit(uint32_t max_clients,
                                                                       unsigned long desired,
                                                                       unsigned long effective);

#endif /* CBM_DAEMON_HOST_INTERNAL_H */
