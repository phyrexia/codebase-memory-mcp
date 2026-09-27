/*
 * service_internal.h — Internal daemon service helpers and test hooks.
 *
 * Production callers outside the daemon implementation must use
 * daemon/service.h.  Runtime identity verification hashes an already-open,
 * kernel-bound file handle through this internal boundary; the test hook lets
 * concurrency tests establish an exact interleaving without scheduler luck.
 */
#ifndef CBM_DAEMON_SERVICE_INTERNAL_H
#define CBM_DAEMON_SERVICE_INTERNAL_H

#include "daemon/service.h"

#include <stdbool.h>
#include <stdint.h>

/* Hash an already-open regular file without closing it. native_file is an int
 * file descriptor on POSIX and a HANDLE cast through uintptr_t on Windows.
 * Callers use this after binding the handle to kernel process-image metadata,
 * avoiding a second lookup through a replaceable pathname. Deliberately NOT
 * cached by the build-fingerprint cache: it has no stable pathname to name in a
 * sidecar and its callers already hash through a live, kernel-bound handle. */
bool cbm_daemon_build_fingerprint_native_file(uintptr_t native_file,
                                              char out[CBM_DAEMON_BUILD_FINGERPRINT_SIZE]);

#if defined(CBM_CLI_ENABLE_TEST_API)
/* Test seams for the build-fingerprint cache (never compiled into
 * production). cache_reset_for_test clears the once-per-process enablement
 * memo so a test can change build_fingerprint_cache between cases; the hash
 * counter counts completed executable hash computations so a test can prove
 * an identical (device, inode, size, mtime_ns) tuple was served without
 * rehashing. */
void cbm_daemon_build_fingerprint_cache_reset_for_test(void);
size_t cbm_daemon_build_fingerprint_hash_count_for_test(void);
void cbm_daemon_build_fingerprint_hash_count_reset_for_test(void);
#endif

typedef enum {
    CBM_DAEMON_CONFLICT_LOG_BEFORE_SERIALIZATION_LOCK = 1,
    CBM_DAEMON_CONFLICT_LOG_AFTER_SERIALIZATION_LOCK,
} cbm_daemon_conflict_log_test_stage_t;

typedef void (*cbm_daemon_conflict_log_test_hook_fn)(void *context,
                                                     cbm_daemon_conflict_log_test_stage_t stage);

void cbm_daemon_conflict_log_set_test_hook(cbm_daemon_conflict_log_test_hook_fn hook,
                                           void *context);

#endif /* CBM_DAEMON_SERVICE_INTERNAL_H */
