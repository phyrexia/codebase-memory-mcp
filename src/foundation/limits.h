/*
 * limits.h — Generous, env-configurable safety limits (Stage 2 / Track B4).
 *
 * Each knob has a generous default. Hitting a limit degrades to a *reported*
 * skip (surfaced via MCP/CLI/logfile), never a silent drop and never an
 * unbounded read (unbounded just trades a crash for an OOM/hang). Every limit
 * is env-overridable so an operator can tune it per-repo without a rebuild.
 */
#ifndef CBM_LIMITS_H
#define CBM_LIMITS_H

/* Result of an attempted per-file read, so callers can attribute a skip to the
 * right phase/reason instead of collapsing every failure into "read failed". */
typedef enum {
    CBM_READ_OK = 0,    /* file read successfully */
    CBM_READ_OPEN_FAIL, /* could not open (missing / permission) */
    CBM_READ_EMPTY,     /* zero/negative size — benign, nothing to index */
    CBM_READ_OVERSIZED, /* size exceeds cbm_max_file_bytes() */
    CBM_READ_OOM,       /* buffer allocation failed */
} cbm_read_status_t;

/* Maximum size (bytes) of a single source file the indexer will read into
 * memory. Files larger than this are skipped-and-reported (phase "oversized"),
 * never silently dropped. Override with CBM_MAX_FILE_BYTES (a positive integer
 * count of bytes). Default 512 MiB (raised from the historical 100 MB cap).
 *
 * The env var is read on each call — this is intentional: read_file() calls it
 * once per file (negligible), and reading fresh means a test / operator can
 * change the cap via setenv without a process restart or a stale memoized copy
 * leaking across runs. */
long cbm_max_file_bytes(void);

/* Maximum variable-length path depth for the Cypher engine (the `*min..max`
 * hop ceiling). BOTH the explicit (`*1..N`) and unbounded (`*`, `*..m`) forms
 * are clamped to this, so `[:CALLS*1..1000000]` degrades to a WARN-and-cap
 * rather than an unbounded (cyclic-graph DoS) traversal. Override with
 * CBM_CYPHER_MAX_DEPTH (a positive integer). Default 10. */
int cbm_cypher_max_depth(void);

/* Maximum traversal depth for client-driven MCP graph tools (trace_call_path,
 * detect_changes): the client `depth` argument is WARN-clamped to this so an
 * arbitrarily large value cannot drive an unbounded BFS over the shared store.
 * Override with CBM_MCP_MAX_DEPTH (a positive integer). Default 15. */
int cbm_mcp_max_depth(void);

enum {
    /* Default cap on concurrent daemon client connections. Sized against
     * measured load rather than a round number: a busy workstation running
     * dozens of agent sessions in parallel held ~65 live frontends, and a
     * single session of some agent CLIs opens several connections of its own
     * rather than sharing one. 128 leaves roughly 2x that headroom, so
     * ordinary growth in session count does not walk into the wall. */
    CBM_DAEMON_MAX_CLIENTS_DEFAULT = 128,
    /* Absolute ceiling CBM_MAX_CLIENTS can never raise the cap past. Each
     * admitted connection costs a reader thread parked in a blocking read
     * (plus a second thread for the duration of a tool call) and a socket
     * descriptor — so this bounds threads and descriptors, not CPU. At the
     * runtime's 256 KiB thread stacks that is ~256 MiB of virtual stack in
     * the worst case, which is why the ceiling can sit well above the
     * indexing-worker ceiling. */
    CBM_DAEMON_MAX_CLIENTS_LIMIT = 512,
};

/* Cap on how many client connections the daemon admits at once, including
 * sockets accepted but not yet through HELLO. Override with CBM_MAX_CLIENTS
 * (a positive integer).
 *
 * Unlike the knobs above, an out-of-range value is WARN-*clamped* into
 * [1, CBM_DAEMON_MAX_CLIENTS_LIMIT] rather than dropped for the default:
 * someone who writes CBM_MAX_CLIENTS=99999 is asking for "as many as you can",
 * and silently handing them the 128 default would read as the setting having
 * been honoured. Unreadable text is a different mistake and does fall back to
 * the default. Either way the daemon says in its log what it settled on.
 *
 * Read fresh on each call, like the other limits here. */
int cbm_daemon_max_clients(void);

#endif /* CBM_LIMITS_H */
