/*
 * host.h — Lifecycle owner for the mandatory per-account CBM daemon.
 */
#ifndef CBM_DAEMON_HOST_H
#define CBM_DAEMON_HOST_H

#include "daemon/ipc.h"
#include "daemon/service.h"

#include <stdatomic.h>

typedef struct {
    const cbm_daemon_ipc_endpoint_t *endpoint;
    cbm_daemon_build_identity_t identity;
    const char *executable_path;
    atomic_int *stop_requested;
    /* Born via `daemon start`: the generation survives its last client
     * disconnect and the no-client initial window; it stops only through the
     * stop/drain ops or an explicit process kill. */
    bool permanent;
} cbm_daemon_host_config_t;

/* Exit statuses cbm_daemon_host_run returns besides 0 (an ordinary stop) and
 * -1 (startup or teardown failure). The process passes them through and a
 * supervisor keys on them, so the values are fixed (sysexits.h). */
enum {
    /* The runtime directory stopped naming what this generation holds (a
     * cleaner removed or replaced a lock, a socket name or the identity
     * record): it stopped serving and left so a fresh generation can take
     * over. A supervisor should restart it. EX_TEMPFAIL. */
    CBM_DAEMON_HOST_EXIT_RUNTIME_DIR_ORPHANED = 75,
};

/* Blocks for the complete daemon generation. The first admitted frontend owns
 * startup; the final disconnect triggers terminal shutdown of operations,
 * watcher, UI, and runtime in that order. Returns 0, -1 or one of the exit
 * statuses above. */
int cbm_daemon_host_run(const cbm_daemon_host_config_t *config);

#endif /* CBM_DAEMON_HOST_H */
