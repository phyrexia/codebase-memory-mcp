/*
 * test_daemon_version.c — RED contract for one-version daemon rendezvous.
 *
 * These tests intentionally target the service boundary that will be exposed
 * by daemon/service.h.  They remain a separate suite so implementation can be
 * developed without mixing version policy into transport or job-lifecycle
 * tests.
 */
#include "test_framework.h"

#include "cli/cli.h"
#include "daemon/daemon.h"
#include "daemon/runtime.h"
#include "daemon/service.h"
#include "daemon/service_internal.h"
#include "foundation/compat.h"
#include "foundation/compat_fs.h"
#include "foundation/compat_thread.h"
#include "test_helpers.h"

#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef _WIN32
#include "foundation/win_utf8.h"
#endif

enum {
    VERSION_TEST_PATH_CAP = 1024,
    VERSION_TEST_FILE_CAP = 8192,
};

static const char BUILD_A[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static const char BUILD_B[] = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
static const char BUILD_C[] = "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";

static cbm_daemon_build_identity_t version_test_identity(const char *version, const char *build) {
    cbm_daemon_build_identity_t identity = {
        .semantic_version = version,
        .build_fingerprint = build,
        .protocol_abi = 3,
        .store_abi = 11,
        .feature_abi = 7,
    };
    return identity;
}

static bool version_test_temp_dir(char out[VERSION_TEST_PATH_CAP], const char *tag) {
    int written =
        snprintf(out, VERSION_TEST_PATH_CAP, "%s/cbm-daemon-%s-XXXXXX", cbm_tmpdir(), tag);
    return written > 0 && written < VERSION_TEST_PATH_CAP && cbm_mkdtemp(out) != NULL;
}

static bool version_test_child_path(char out[VERSION_TEST_PATH_CAP], const char *dir,
                                    const char *name) {
    int written = snprintf(out, VERSION_TEST_PATH_CAP, "%s/%s", dir, name);
    return written > 0 && written < VERSION_TEST_PATH_CAP;
}

static long version_test_file_size(const char *path) {
    FILE *file = cbm_fopen(path, "rb");
    if (!file) {
        return -1;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        (void)fclose(file);
        return -1;
    }
    long size = ftell(file);
    (void)fclose(file);
    return size;
}

static bool version_test_read_file(const char *path, char out[VERSION_TEST_FILE_CAP]) {
    FILE *file = cbm_fopen(path, "rb");
    if (!file) {
        return false;
    }
    size_t used = fread(out, 1, VERSION_TEST_FILE_CAP - 1, file);
    bool complete = !ferror(file) && feof(file);
    out[used] = '\0';
    (void)fclose(file);
    return complete;
}

static bool version_test_write_file(const char *path, const char *contents) {
    FILE *file = cbm_fopen(path, "wb");
    if (!file) {
        return false;
    }
    size_t length = strlen(contents);
    bool wrote = fwrite(contents, 1, length, file) == length;
    bool closed = fclose(file) == 0;
    return wrote && closed;
}

static uint64_t version_test_process_id(void) {
#ifdef _WIN32
    return (uint64_t)GetCurrentProcessId();
#else
    return (uint64_t)getpid();
#endif
}

static bool version_test_is_sha256(const char *value) {
    if (!value || strlen(value) != CBM_DAEMON_BUILD_FINGERPRINT_SIZE - 1) {
        return false;
    }
    for (size_t i = 0; i < CBM_DAEMON_BUILD_FINGERPRINT_SIZE - 1; i++) {
        char ch = value[i];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) {
            return false;
        }
    }
    return true;
}

/* Build-fingerprint cache fixtures. The enable helper writes through the real
 * config store; value = NULL removes the key so the strict reader must fall
 * back to the default (cache off). */
static bool version_test_write_fingerprint_cache_value(const char *cache_dir, const char *value) {
    cbm_config_t *config = cbm_config_open(cache_dir);
    if (!config) {
        return false;
    }
    int rc = value ? cbm_config_set(config, CBM_CONFIG_BUILD_FINGERPRINT_CACHE, value)
                   : cbm_config_delete(config, CBM_CONFIG_BUILD_FINGERPRINT_CACHE);
    cbm_config_close(config);
    return rc == 0;
}

/* Locate the one build-fingerprint sidecar the cache wrote. The name encodes
 * the hashed file's identity and is an implementation detail; scanning keeps
 * the tests honest about where the sidecar lives without mirroring the naming
 * format. */
static bool version_test_find_fingerprint_sidecar(const char *cache_dir, char *out, size_t cap) {
    static const char prefix[] = "build-fingerprint.";
    static const char suffix[] = ".cache";
    cbm_dir_t *dir = cbm_opendir(cache_dir);
    if (!dir || !out || cap == 0) {
        cbm_closedir(dir);
        return false;
    }
    bool found = false;
    const cbm_dirent_t *entry;
    while (!found && (entry = cbm_readdir(dir)) != NULL) {
        size_t length = strlen(entry->name);
        if (length > (sizeof(prefix) - 1) + (sizeof(suffix) - 1) &&
            strncmp(entry->name, prefix, sizeof(prefix) - 1) == 0 &&
            strcmp(entry->name + length - (sizeof(suffix) - 1), suffix) == 0) {
            int written = snprintf(out, cap, "%s/%s", cache_dir, entry->name);
            found = written > 0 && (size_t)written < cap;
        }
    }
    cbm_closedir(dir);
    return found;
}

/* Device/inode the cache names a sidecar by, read from the path itself. */
static bool version_test_file_identity(const char *path, uint64_t *device_out,
                                       uint64_t *inode_out) {
    if (!path || !device_out || !inode_out) {
        return false;
    }
#ifdef _WIN32
    wchar_t *wide = cbm_utf8_to_wide(path);
    HANDLE file = wide ? CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL,
                                     OPEN_EXISTING,
                                     FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
                                     NULL)
                       : INVALID_HANDLE_VALUE;
    free(wide);
    BY_HANDLE_FILE_INFORMATION info;
    bool ok = file != INVALID_HANDLE_VALUE && GetFileInformationByHandle(file, &info) != 0;
    if (file != INVALID_HANDLE_VALUE) {
        (void)CloseHandle(file);
    }
    if (!ok) {
        return false;
    }
    *device_out = (uint64_t)info.dwVolumeSerialNumber;
    *inode_out = ((uint64_t)info.nFileIndexHigh << 32) | (uint64_t)info.nFileIndexLow;
    return true;
#else
    struct stat status;
    if (stat(path, &status) != 0) {
        return false;
    }
    *device_out = (uint64_t)status.st_dev;
    *inode_out = (uint64_t)status.st_ino;
    return true;
#endif
}

/* The exact sidecar name the cache must use for one identity. */
static bool version_test_identity_sidecar(const char *cache_dir, uint64_t device, uint64_t inode,
                                          char out[VERSION_TEST_PATH_CAP]) {
    char name[VERSION_TEST_PATH_CAP];
    int written = snprintf(name, sizeof(name), "build-fingerprint.%016llx-%016llx.cache",
                           (unsigned long long)device, (unsigned long long)inode);
    return written > 0 && written < (int)sizeof(name) &&
           version_test_child_path(out, cache_dir, name);
}

/* Count cache-directory entries the fingerprint cache owns: finished sidecars
 * end .cache, in-flight stores end .tmp. */
static int version_test_count_fingerprint_entries(const char *cache_dir, const char *suffix) {
    static const char prefix[] = "build-fingerprint.";
    cbm_dir_t *dir = cbm_opendir(cache_dir);
    if (!dir || !suffix) {
        cbm_closedir(dir);
        return -1;
    }
    int count = 0;
    size_t suffix_length = strlen(suffix);
    const cbm_dirent_t *entry;
    while ((entry = cbm_readdir(dir)) != NULL) {
        size_t length = strlen(entry->name);
        if (length > (sizeof(prefix) - 1) + suffix_length &&
            strncmp(entry->name, prefix, sizeof(prefix) - 1) == 0 &&
            strcmp(entry->name + length - suffix_length, suffix) == 0) {
            count++;
        }
    }
    cbm_closedir(dir);
    return count;
}

typedef struct {
#ifdef _WIN32
    HANDLE handle;
#else
    int fd;
#endif
} version_test_native_t;

/* The kind of handle native_file consumes: an int fd on POSIX, a HANDLE on
 * Windows. */
static bool version_test_native_open(const char *path, version_test_native_t *out) {
    if (!path || !out) {
        return false;
    }
#ifdef _WIN32
    wchar_t *wide = cbm_utf8_to_wide(path);
    HANDLE file = wide ? CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL,
                                     OPEN_EXISTING,
                                     FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
                                     NULL)
                       : INVALID_HANDLE_VALUE;
    free(wide);
    out->handle = file;
    return file != INVALID_HANDLE_VALUE;
#else
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    out->fd = fd;
    return fd >= 0;
#endif
}

static uintptr_t version_test_native_value(const version_test_native_t *file) {
#ifdef _WIN32
    return (uintptr_t)file->handle;
#else
    return (uintptr_t)file->fd;
#endif
}

static void version_test_native_close(version_test_native_t *file) {
    if (!file) {
        return;
    }
#ifdef _WIN32
    if (file->handle != INVALID_HANDLE_VALUE) {
        (void)CloseHandle(file->handle);
        file->handle = INVALID_HANDLE_VALUE;
    }
#else
    if (file->fd >= 0) {
        (void)close(file->fd);
        file->fd = -1;
    }
#endif
}

/* Portable mtime_ns read/force for cache-invalidation fixtures. The force path
 * mirrors test_pipeline.c's pipeline_test_set_mtime: utimensat on POSIX and
 * SetFileTime on Windows (FILETIME is 100ns ticks since 1601, the same
 * representation the fingerprint identity records), so stamping a replacement
 * with the original's own stamp loses nothing the cache can observe. */
static bool version_test_file_mtime_ns(const char *path, uint64_t *out) {
    if (!out) {
        return false;
    }
#ifdef _WIN32
    wchar_t *wide = cbm_utf8_to_wide(path);
    HANDLE file = wide ? CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL)
                       : INVALID_HANDLE_VALUE;
    free(wide);
    FILETIME written;
    bool ok = file != INVALID_HANDLE_VALUE && GetFileTime(file, NULL, NULL, &written) != 0;
    if (file != INVALID_HANDLE_VALUE) {
        (void)CloseHandle(file);
    }
    if (!ok) {
        return false;
    }
    uint64_t ticks = ((uint64_t)written.dwHighDateTime << 32) | written.dwLowDateTime;
    if (ticks < UINT64_C(116444736000000000)) {
        return false;
    }
    *out = (ticks - UINT64_C(116444736000000000)) * UINT64_C(100);
    return true;
#else
    struct stat status;
    if (!out || stat(path, &status) != 0) {
        return false;
    }
#ifdef __APPLE__
    *out = (uint64_t)status.st_mtimespec.tv_sec * UINT64_C(1000000000) +
           (uint64_t)status.st_mtimespec.tv_nsec;
#else
    *out = (uint64_t)status.st_mtim.tv_sec * UINT64_C(1000000000) +
           (uint64_t)status.st_mtim.tv_nsec;
#endif
    return true;
#endif
}

static bool version_test_set_mtime_ns(const char *path, uint64_t mtime_ns) {
#ifdef _WIN32
    uint64_t ticks = mtime_ns / UINT64_C(100) + UINT64_C(116444736000000000);
    FILETIME stamp = {.dwLowDateTime = (DWORD)(ticks & UINT64_C(0xFFFFFFFF)),
                      .dwHighDateTime = (DWORD)(ticks >> 32)};
    wchar_t *wide = cbm_utf8_to_wide(path);
    HANDLE file = wide ? CreateFileW(wide, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL)
                       : INVALID_HANDLE_VALUE;
    free(wide);
    bool ok = file != INVALID_HANDLE_VALUE && SetFileTime(file, NULL, &stamp, &stamp) != 0;
    if (file != INVALID_HANDLE_VALUE) {
        (void)CloseHandle(file);
    }
    return ok;
#else
    time_t seconds = (time_t)(mtime_ns / UINT64_C(1000000000));
    long nanoseconds = (long)(mtime_ns % UINT64_C(1000000000));
    struct timespec times[2] = {{.tv_sec = seconds, .tv_nsec = nanoseconds},
                                {.tv_sec = seconds, .tv_nsec = nanoseconds}};
    return utimensat(AT_FDCWD, path, times, 0) == 0;
#endif
}

static void version_test_cleanup(const char *dir, const char *path, const char *rotated,
                                 const char *victim) {
    char lock_path[VERSION_TEST_PATH_CAP];
    if (path) {
        int written = snprintf(lock_path, sizeof(lock_path), "%s.lock", path);
        if (written > 0 && written < (int)sizeof(lock_path)) {
            (void)cbm_unlink(lock_path);
        }
    }
    if (path) {
        (void)cbm_unlink(path);
    }
    if (rotated) {
        (void)cbm_unlink(rotated);
    }
    if (victim) {
        (void)cbm_unlink(victim);
    }
    if (dir) {
        (void)cbm_rmdir(dir);
    }
}

#ifndef _WIN32
typedef struct {
    atomic_int lock_attempts;
    atomic_bool first_lock_acquired;
} version_test_rotation_race_t;

typedef struct {
    const char *path;
    const cbm_daemon_conflict_t *conflict;
    size_t cap_bytes;
    bool result;
} version_test_append_call_t;

static bool version_test_wait_atomic_bool(atomic_bool *value) {
    struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000};
    for (int i = 0; i < 2000; i++) {
        if (atomic_load_explicit(value, memory_order_acquire)) {
            return true;
        }
        (void)cbm_nanosleep(&pause, NULL);
    }
    return atomic_load_explicit(value, memory_order_acquire);
}

static void version_test_rotation_race_hook(void *opaque,
                                            cbm_daemon_conflict_log_test_stage_t stage) {
    version_test_rotation_race_t *race = opaque;
    if (stage == CBM_DAEMON_CONFLICT_LOG_BEFORE_SERIALIZATION_LOCK) {
        (void)atomic_fetch_add_explicit(&race->lock_attempts, 1, memory_order_acq_rel);
        return;
    }
    if (stage != CBM_DAEMON_CONFLICT_LOG_AFTER_SERIALIZATION_LOCK ||
        atomic_exchange_explicit(&race->first_lock_acquired, true, memory_order_acq_rel)) {
        return;
    }
    struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000};
    for (int i = 0;
         i < 2000 && atomic_load_explicit(&race->lock_attempts, memory_order_acquire) < 2; i++) {
        (void)cbm_nanosleep(&pause, NULL);
    }
}

static void *version_test_append_thread(void *opaque) {
    version_test_append_call_t *call = opaque;
    call->result = cbm_daemon_conflict_log_append(call->path, call->conflict, call->cap_bytes);
    return NULL;
}
#endif

static cbm_daemon_hello_status_t version_test_compare(const cbm_daemon_build_identity_t *active,
                                                      const cbm_daemon_build_identity_t *requested,
                                                      cbm_daemon_conflict_t *conflict) {
    memset(conflict, 0, sizeof(*conflict));
    return cbm_daemon_hello_compare(active, requested, conflict);
}

static bool version_test_conflict_identity_matches(const cbm_daemon_conflict_t *conflict,
                                                   const char *active_version,
                                                   const char *active_build,
                                                   const char *requested_version,
                                                   const char *requested_build) {
    return conflict && strcmp(conflict->active_version, active_version) == 0 &&
           strcmp(conflict->active_build_fingerprint, active_build) == 0 &&
           strcmp(conflict->requested_version, requested_version) == 0 &&
           strcmp(conflict->requested_build_fingerprint, requested_build) == 0;
}

TEST(daemon_rendezvous_key_is_stable_and_version_independent) {
    char first[CBM_DAEMON_KEY_SIZE];
    char after_upgrade[CBM_DAEMON_KEY_SIZE];

    /* Version, executable path, build fingerprint, cache directory, and ABI
     * values are deliberately absent from this API. Every build and cache
     * domain must rendezvous at the same product endpoint before HELLO decides
     * whether it may proceed. The IPC layer supplies the trusted OS-account
     * scope through its owner-only runtime directory / current-user ACL. */
    ASSERT_TRUE(cbm_daemon_rendezvous_key(first));
    ASSERT_TRUE(cbm_daemon_rendezvous_key(after_upgrade));

    ASSERT_STR_EQ(first, after_upgrade);
    PASS();
}

TEST(daemon_build_fingerprint_hashes_exact_executable_bytes) {
    char dir[VERSION_TEST_PATH_CAP] = {0};
    char first_path[VERSION_TEST_PATH_CAP] = {0};
    char second_path[VERSION_TEST_PATH_CAP] = {0};
    char first[CBM_DAEMON_BUILD_FINGERPRINT_SIZE];
    char repeated[CBM_DAEMON_BUILD_FINGERPRINT_SIZE];
    char second[CBM_DAEMON_BUILD_FINGERPRINT_SIZE];
    bool setup_ok = version_test_temp_dir(dir, "fingerprint") &&
                    version_test_child_path(first_path, dir, "build-a.bin") &&
                    version_test_child_path(second_path, dir, "build-b.bin") &&
                    version_test_write_file(first_path, "same-version-build-a") &&
                    version_test_write_file(second_path, "same-version-build-b");
    if (!setup_ok) {
        version_test_cleanup(dir, first_path, second_path, NULL);
        FAIL("could not create fingerprint fixtures");
    }

    ASSERT_TRUE(cbm_daemon_build_fingerprint_file(first_path, first));
    ASSERT_TRUE(cbm_daemon_build_fingerprint_file(first_path, repeated));
    ASSERT_TRUE(cbm_daemon_build_fingerprint_file(second_path, second));
    ASSERT_TRUE(version_test_is_sha256(first));
    ASSERT_TRUE(version_test_is_sha256(second));
    ASSERT_STR_EQ(first, repeated);
    ASSERT_STR_NEQ(first, second);

    version_test_cleanup(dir, first_path, second_path, NULL);
    PASS();
}

TEST(daemon_build_fingerprint_cache_skips_rehash_on_identical_tuple) {
    char dir[VERSION_TEST_PATH_CAP] = {0};
    char cache[VERSION_TEST_PATH_CAP] = {0};
    char image[VERSION_TEST_PATH_CAP] = {0};
    char sidecar[VERSION_TEST_PATH_CAP] = {0};
    char cold_fp[CBM_DAEMON_BUILD_FINGERPRINT_SIZE] = {0};
    char warm_fp[CBM_DAEMON_BUILD_FINGERPRINT_SIZE] = {0};
    char strict_fp[CBM_DAEMON_BUILD_FINGERPRINT_SIZE] = {0};
    char absent_fp[CBM_DAEMON_BUILD_FINGERPRINT_SIZE] = {0};
    const char *old_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache = old_cache ? cbm_strdup(old_cache) : NULL;

    bool setup_ok = (!old_cache || saved_cache) && version_test_temp_dir(dir, "fp-cache") &&
                    version_test_temp_dir(cache, "fp-cache-store") &&
                    version_test_child_path(image, dir, "build.bin") &&
                    version_test_write_file(image, "fingerprint-cache-build-bytes") &&
                    cbm_setenv("CBM_CACHE_DIR", cache, 1) == 0 &&
                    version_test_write_fingerprint_cache_value(cache, "true");
    cbm_daemon_build_fingerprint_cache_reset_for_test();
    cbm_daemon_build_fingerprint_hash_count_reset_for_test();

    /* Cold cache: exactly one real hash, and the sidecar lands in the resolved
     * cache directory (CBM_CACHE_DIR tmpdir, never the live cache). */
    bool cold_ok = setup_ok && cbm_daemon_build_fingerprint_file(image, cold_fp);
    size_t cold_count = cbm_daemon_build_fingerprint_hash_count_for_test();
    bool sidecar_found =
        cold_ok && version_test_find_fingerprint_sidecar(cache, sidecar, sizeof(sidecar));
    long sidecar_bytes = sidecar_found ? version_test_file_size(sidecar) : -1;

    /* Identical (dev, ino, size, mtime_ns): served from the sidecar, no hash. */
    bool warm_ok = cold_ok && cbm_daemon_build_fingerprint_file(image, warm_fp);
    size_t warm_count = cbm_daemon_build_fingerprint_hash_count_for_test();

    /* Only the exact stored bytes "true" enable the cache; "TRUE" must fail
     * closed to "off" and keep rehashing like today. */
    bool strict_setup = warm_ok && version_test_write_fingerprint_cache_value(cache, "TRUE");
    cbm_daemon_build_fingerprint_cache_reset_for_test();
    cbm_daemon_build_fingerprint_hash_count_reset_for_test();
    bool strict_ok = strict_setup && cbm_daemon_build_fingerprint_file(image, strict_fp) &&
                     cbm_daemon_build_fingerprint_file(image, strict_fp);
    size_t strict_count = cbm_daemon_build_fingerprint_hash_count_for_test();

    /* Key absent (the default): current behavior intact, every call rehashes. */
    bool absent_setup = strict_ok && version_test_write_fingerprint_cache_value(cache, NULL);
    cbm_daemon_build_fingerprint_cache_reset_for_test();
    cbm_daemon_build_fingerprint_hash_count_reset_for_test();
    bool absent_ok = absent_setup && cbm_daemon_build_fingerprint_file(image, absent_fp) &&
                     cbm_daemon_build_fingerprint_file(image, absent_fp);
    size_t absent_count = cbm_daemon_build_fingerprint_hash_count_for_test();

    if (saved_cache) {
        (void)cbm_setenv("CBM_CACHE_DIR", saved_cache, 1);
        free(saved_cache);
    } else {
        (void)cbm_unsetenv("CBM_CACHE_DIR");
    }
    cbm_daemon_build_fingerprint_cache_reset_for_test();
    cbm_daemon_build_fingerprint_hash_count_reset_for_test();
    (void)th_rmtree(dir);
    (void)th_rmtree(cache);

    ASSERT_TRUE(setup_ok);
    ASSERT_TRUE(cold_ok);
    ASSERT_TRUE(version_test_is_sha256(cold_fp));
    ASSERT_EQ(cold_count, 1);
    ASSERT_TRUE(sidecar_found);
    ASSERT_TRUE(sidecar_bytes > 0);
    ASSERT_TRUE(warm_ok);
    ASSERT_STR_EQ(warm_fp, cold_fp);
    ASSERT_EQ(warm_count, 1);
    ASSERT_TRUE(strict_ok);
    ASSERT_STR_EQ(strict_fp, cold_fp);
    ASSERT_EQ(strict_count, 2);
    ASSERT_TRUE(absent_ok);
    ASSERT_STR_EQ(absent_fp, cold_fp);
    ASSERT_EQ(absent_count, 2);
    PASS();
}

TEST(daemon_build_fingerprint_cache_detects_replacement_by_inode_or_size) {
    char dir[VERSION_TEST_PATH_CAP] = {0};
    char cache[VERSION_TEST_PATH_CAP] = {0};
    char image[VERSION_TEST_PATH_CAP] = {0};
    char reference_b[VERSION_TEST_PATH_CAP] = {0};
    char reference_c[VERSION_TEST_PATH_CAP] = {0};
    char replacement[VERSION_TEST_PATH_CAP] = {0};
    char fp_expected_b[CBM_DAEMON_BUILD_FINGERPRINT_SIZE] = {0};
    char fp_expected_c[CBM_DAEMON_BUILD_FINGERPRINT_SIZE] = {0};
    char fp_original[CBM_DAEMON_BUILD_FINGERPRINT_SIZE] = {0};
    char fp_after_same_size[CBM_DAEMON_BUILD_FINGERPRINT_SIZE] = {0};
    char fp_after_new_size[CBM_DAEMON_BUILD_FINGERPRINT_SIZE] = {0};
    const char *old_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache = old_cache ? cbm_strdup(old_cache) : NULL;
    /* The first two contents are equal length and differ in one byte; the
     * third is longer, so (a) pins detection on the inode and (b) on size. */
    static const char CONTENT_A[] = "cache-replaced-same-size-a";
    static const char CONTENT_B[] = "cache-replaced-same-size-b";
    static const char CONTENT_C[] = "cache-replaced-different-size-cc";

    bool setup_ok = (!old_cache || saved_cache) && version_test_temp_dir(dir, "fp-replace") &&
                    version_test_temp_dir(cache, "fp-replace-store") &&
                    version_test_child_path(image, dir, "build.bin") &&
                    version_test_child_path(reference_b, dir, "reference-b.bin") &&
                    version_test_child_path(reference_c, dir, "reference-c.bin") &&
                    version_test_child_path(replacement, dir, "incoming.bin") &&
                    version_test_write_file(image, CONTENT_A) &&
                    version_test_write_file(reference_b, CONTENT_B) &&
                    version_test_write_file(reference_c, CONTENT_C) &&
                    cbm_setenv("CBM_CACHE_DIR", cache, 1) == 0 &&
                    version_test_write_fingerprint_cache_value(cache, "true");
    cbm_daemon_build_fingerprint_cache_reset_for_test();
    cbm_daemon_build_fingerprint_hash_count_reset_for_test();

    /* Reference hashes: the exact bytes each replacement will install. */
    bool references_ok = setup_ok &&
                         cbm_daemon_build_fingerprint_file(reference_b, fp_expected_b) &&
                         cbm_daemon_build_fingerprint_file(reference_c, fp_expected_c) &&
                         strcmp(fp_expected_b, fp_expected_c) != 0;

    /* Baseline: the original bytes hash once and populate the sidecar. */
    bool baseline_ok = references_ok && cbm_daemon_build_fingerprint_file(image, fp_original);
    cbm_daemon_build_fingerprint_hash_count_reset_for_test();

    /* (a) Same size and a forced identical mtime_ns: the replacement installs
     * under a NEW inode (rename over the path), so the next call must not be
     * served the cached hash and must return the real bytes. */
    uint64_t original_mtime = 0;
    uint64_t stamped_mtime = 0;
    long original_size = baseline_ok ? version_test_file_size(image) : -1;
    bool stamped = baseline_ok && version_test_write_file(replacement, CONTENT_B) &&
                   version_test_file_mtime_ns(image, &original_mtime) &&
                   version_test_set_mtime_ns(replacement, original_mtime) &&
                   version_test_file_mtime_ns(replacement, &stamped_mtime);
    long replacement_size = stamped ? version_test_file_size(replacement) : -1;
    bool replaced_same = stamped && cbm_rename_replace(replacement, image) == 0;
    bool same_ok = replaced_same && cbm_daemon_build_fingerprint_file(image, fp_after_same_size);
    size_t same_count = cbm_daemon_build_fingerprint_hash_count_for_test();

    /* (b) A replacement of a different size is detected as well. */
    bool replaced_size = same_ok && version_test_write_file(replacement, CONTENT_C) &&
                         cbm_rename_replace(replacement, image) == 0;
    bool size_ok = replaced_size && cbm_daemon_build_fingerprint_file(image, fp_after_new_size);
    size_t size_count = cbm_daemon_build_fingerprint_hash_count_for_test();

    if (saved_cache) {
        (void)cbm_setenv("CBM_CACHE_DIR", saved_cache, 1);
        free(saved_cache);
    } else {
        (void)cbm_unsetenv("CBM_CACHE_DIR");
    }
    cbm_daemon_build_fingerprint_cache_reset_for_test();
    cbm_daemon_build_fingerprint_hash_count_reset_for_test();
    (void)th_rmtree(dir);
    (void)th_rmtree(cache);

    ASSERT_TRUE(setup_ok);
    ASSERT_TRUE(references_ok);
    ASSERT_TRUE(baseline_ok);
    ASSERT_STR_NEQ(fp_original, fp_expected_b);
    ASSERT_TRUE(stamped);
    /* The premises the claim rests on must really hold on this filesystem. */
    ASSERT_EQ(stamped_mtime, original_mtime);
    ASSERT_EQ(replacement_size, original_size);
    ASSERT_TRUE(replaced_same);
    ASSERT_TRUE(same_ok);
    ASSERT_EQ(same_count, 1);
    ASSERT_STR_EQ(fp_after_same_size, fp_expected_b);
    ASSERT_STR_NEQ(fp_after_same_size, fp_original);
    ASSERT_TRUE(replaced_size);
    ASSERT_TRUE(size_ok);
    ASSERT_EQ(size_count, 2);
    ASSERT_STR_EQ(fp_after_new_size, fp_expected_c);
    ASSERT_STR_NEQ(fp_after_new_size, fp_after_same_size);
    PASS();
}

TEST(daemon_build_fingerprint_cache_rejects_corrupt_sidecar) {
    char dir[VERSION_TEST_PATH_CAP] = {0};
    char cache[VERSION_TEST_PATH_CAP] = {0};
    char image[VERSION_TEST_PATH_CAP] = {0};
    char sidecar[VERSION_TEST_PATH_CAP] = {0};
    char expected[CBM_DAEMON_BUILD_FINGERPRINT_SIZE] = {0};
    char observed[CBM_DAEMON_BUILD_FINGERPRINT_SIZE] = {0};
    char valid[VERSION_TEST_FILE_CAP] = {0};
    char extra_field[VERSION_TEST_FILE_CAP] = {0};
    char short_hash[VERSION_TEST_FILE_CAP] = {0};
    static const char garbage[] = "not-a-fingerprint-cache-sidecar\n";
    const char *old_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache = old_cache ? cbm_strdup(old_cache) : NULL;

    bool setup_ok = (!old_cache || saved_cache) && version_test_temp_dir(dir, "fp-corrupt") &&
                    version_test_temp_dir(cache, "fp-corrupt-store") &&
                    version_test_child_path(image, dir, "build.bin") &&
                    version_test_write_file(image, "fingerprint-cache-corrupt-bytes") &&
                    cbm_setenv("CBM_CACHE_DIR", cache, 1) == 0 &&
                    version_test_write_fingerprint_cache_value(cache, "true");
    cbm_daemon_build_fingerprint_cache_reset_for_test();
    cbm_daemon_build_fingerprint_hash_count_reset_for_test();

    bool first_ok = setup_ok && cbm_daemon_build_fingerprint_file(image, expected);
    bool sidecar_found =
        first_ok && version_test_find_fingerprint_sidecar(cache, sidecar, sizeof(sidecar));
    bool valid_read = sidecar_found && version_test_read_file(sidecar, valid);
    size_t valid_length = valid_read ? strlen(valid) : 0;

    /* Four damaged shapes: plain garbage, an otherwise VALID record with an
     * extra field, a truncated hash, and an empty file. The extra-field case
     * carries the correct hash on purpose: only strict parsing (and thus a
     * real rehash) can reject it. */
    const char *payloads[4] = {NULL, NULL, NULL, NULL};
    int payload_count = 0;
    if (valid_read && valid_length > 12) {
        int extra_written = snprintf(extra_field, sizeof(extra_field), "%.*s 99\n",
                                     (int)(valid_length - 1), valid);
        int short_written =
            snprintf(short_hash, sizeof(short_hash), "%.*s\n", (int)(valid_length - 11), valid);
        if (extra_written > 0 && (size_t)extra_written < sizeof(extra_field) && short_written > 0 &&
            (size_t)short_written < sizeof(short_hash)) {
            payloads[0] = garbage;
            payloads[1] = extra_field;
            payloads[2] = short_hash;
            payloads[3] = "";
            payload_count = 4;
        }
    }

    bool all_rehashed = payload_count == 4;
    size_t counts[4] = {0, 0, 0, 0};
    for (int i = 0; all_rehashed && i < payload_count; i++) {
        all_rehashed = version_test_write_file(sidecar, payloads[i]);
        cbm_daemon_build_fingerprint_hash_count_reset_for_test();
        all_rehashed = all_rehashed && cbm_daemon_build_fingerprint_file(image, observed);
        counts[i] = cbm_daemon_build_fingerprint_hash_count_for_test();
        all_rehashed = all_rehashed && strcmp(observed, expected) == 0;
    }

    if (saved_cache) {
        (void)cbm_setenv("CBM_CACHE_DIR", saved_cache, 1);
        free(saved_cache);
    } else {
        (void)cbm_unsetenv("CBM_CACHE_DIR");
    }
    cbm_daemon_build_fingerprint_cache_reset_for_test();
    cbm_daemon_build_fingerprint_hash_count_reset_for_test();
    (void)th_rmtree(dir);
    (void)th_rmtree(cache);

    ASSERT_TRUE(setup_ok);
    ASSERT_TRUE(first_ok);
    ASSERT_TRUE(sidecar_found);
    ASSERT_TRUE(valid_read);
    ASSERT_TRUE(all_rehashed);
    ASSERT_EQ(counts[0], 1);
    ASSERT_EQ(counts[1], 1);
    ASSERT_EQ(counts[2], 1);
    ASSERT_EQ(counts[3], 1);
    PASS();
}

TEST(daemon_runtime_process_fingerprint_uses_cache) {
    char cache[VERSION_TEST_PATH_CAP] = {0};
    char cold[CBM_DAEMON_BUILD_FINGERPRINT_SIZE] = {0};
    char warm[CBM_DAEMON_BUILD_FINGERPRINT_SIZE] = {0};
    char uncached[CBM_DAEMON_BUILD_FINGERPRINT_SIZE] = {0};
    const char *old_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache = old_cache ? cbm_strdup(old_cache) : NULL;

    bool setup_ok = (!old_cache || saved_cache) && version_test_temp_dir(cache, "fp-process-store") &&
                    cbm_setenv("CBM_CACHE_DIR", cache, 1) == 0 &&
                    version_test_write_fingerprint_cache_value(cache, "true");
    cbm_daemon_build_fingerprint_cache_reset_for_test();
    cbm_daemon_build_fingerprint_hash_count_reset_for_test();

    /* The one-shot identity path hashes this process's own image twice: the
     * capture every one-shot performs before HELLO. Only one capture may hash
     * the executable; the second must be served from the sidecar. */
    bool cold_ok =
        setup_ok &&
        cbm_daemon_runtime_process_build_fingerprint(version_test_process_id(), cold);
    bool warm_ok =
        cold_ok && cbm_daemon_runtime_process_build_fingerprint(version_test_process_id(), warm);
    size_t cached_count = cbm_daemon_build_fingerprint_hash_count_for_test();

    /* Cache off: a fresh hash must reproduce the cached value, proving the
     * sidecar served the same SHA-256 hashing would compute. */
    bool off_setup = warm_ok && version_test_write_fingerprint_cache_value(cache, NULL);
    cbm_daemon_build_fingerprint_cache_reset_for_test();
    cbm_daemon_build_fingerprint_hash_count_reset_for_test();
    bool off_ok = off_setup &&
                  cbm_daemon_runtime_process_build_fingerprint(version_test_process_id(), uncached);
    size_t off_count = cbm_daemon_build_fingerprint_hash_count_for_test();

    if (saved_cache) {
        (void)cbm_setenv("CBM_CACHE_DIR", saved_cache, 1);
        free(saved_cache);
    } else {
        (void)cbm_unsetenv("CBM_CACHE_DIR");
    }
    cbm_daemon_build_fingerprint_cache_reset_for_test();
    cbm_daemon_build_fingerprint_hash_count_reset_for_test();
    (void)th_rmtree(cache);

    ASSERT_TRUE(setup_ok);
    ASSERT_TRUE(cold_ok);
    ASSERT_TRUE(warm_ok);
    ASSERT_TRUE(version_test_is_sha256(cold));
    ASSERT_STR_EQ(warm, cold);
    ASSERT_EQ(cached_count, 1);
    ASSERT_TRUE(off_ok);
    ASSERT_STR_EQ(uncached, cold);
    ASSERT_EQ(off_count, 1);
    PASS();
}

TEST(daemon_native_fingerprint_cache_serves_identical_handle) {
    char dir[VERSION_TEST_PATH_CAP] = {0};
    char cache[VERSION_TEST_PATH_CAP] = {0};
    char image[VERSION_TEST_PATH_CAP] = {0};
    char sidecar[VERSION_TEST_PATH_CAP] = {0};
    char cold[CBM_DAEMON_BUILD_FINGERPRINT_SIZE] = {0};
    char warm[CBM_DAEMON_BUILD_FINGERPRINT_SIZE] = {0};
    uint64_t device = 0;
    uint64_t inode = 0;
    const char *old_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache = old_cache ? cbm_strdup(old_cache) : NULL;

    bool setup_ok = (!old_cache || saved_cache) && version_test_temp_dir(dir, "fp-native") &&
                    version_test_temp_dir(cache, "fp-native-store") &&
                    version_test_child_path(image, dir, "build.bin") &&
                    version_test_write_file(image, "native-fingerprint-cache-bytes") &&
                    version_test_file_identity(image, &device, &inode) &&
                    version_test_identity_sidecar(cache, device, inode, sidecar) &&
                    cbm_setenv("CBM_CACHE_DIR", cache, 1) == 0 &&
                    version_test_write_fingerprint_cache_value(cache, "true");
    cbm_daemon_build_fingerprint_cache_reset_for_test();
    cbm_daemon_build_fingerprint_hash_count_reset_for_test();

    /* Two hashes of one live handle: the second must be served from the
     * sidecar named by that handle's (device, inode) without reading the
     * image again. */
    version_test_native_t file = {0};
    bool opened = setup_ok && version_test_native_open(image, &file);
    bool cold_ok = opened &&
                   cbm_daemon_build_fingerprint_native_file(version_test_native_value(&file), cold);
    bool warm_ok =
        cold_ok && cbm_daemon_build_fingerprint_native_file(version_test_native_value(&file), warm);
    size_t hash_count = cbm_daemon_build_fingerprint_hash_count_for_test();
    if (opened) {
        version_test_native_close(&file);
    }

    if (saved_cache) {
        (void)cbm_setenv("CBM_CACHE_DIR", saved_cache, 1);
        free(saved_cache);
    } else {
        (void)cbm_unsetenv("CBM_CACHE_DIR");
    }
    cbm_daemon_build_fingerprint_cache_reset_for_test();
    cbm_daemon_build_fingerprint_hash_count_reset_for_test();
    long sidecar_bytes = version_test_file_size(sidecar);
    int sidecars = version_test_count_fingerprint_entries(cache, ".cache");
    int temps = version_test_count_fingerprint_entries(cache, ".tmp");
    (void)th_rmtree(dir);
    (void)th_rmtree(cache);

    ASSERT_TRUE(setup_ok);
    ASSERT_TRUE(opened);
    ASSERT_TRUE(cold_ok);
    ASSERT_TRUE(warm_ok);
    ASSERT_TRUE(version_test_is_sha256(cold));
    ASSERT_STR_EQ(warm, cold);
    ASSERT_EQ(hash_count, 1);
    /* Exactly one sidecar, named by the hashed handle's identity, and no
     * temp file left behind. */
    ASSERT_EQ(sidecars, 1);
    ASSERT_EQ(temps, 0);
    ASSERT_GT(sidecar_bytes, 0);
    PASS();
}

TEST(daemon_native_fingerprint_cache_never_stores_without_inode) {
#ifdef _WIN32
    static const char NULL_DEVICE[] = "NUL";
#else
    static const char NULL_DEVICE[] = "/dev/null";
#endif
    char cache[VERSION_TEST_PATH_CAP] = {0};
    char observed[CBM_DAEMON_BUILD_FINGERPRINT_SIZE] = {0};
    const char *old_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache = old_cache ? cbm_strdup(old_cache) : NULL;

    bool setup_ok = (!old_cache || saved_cache) && version_test_temp_dir(cache, "fp-nodev-store") &&
                    cbm_setenv("CBM_CACHE_DIR", cache, 1) == 0 &&
                    version_test_write_fingerprint_cache_value(cache, "true");
    cbm_daemon_build_fingerprint_cache_reset_for_test();
    cbm_daemon_build_fingerprint_hash_count_reset_for_test();

    /* A device file carries no usable image identity: native_file must fail
     * closed and the cache must never be written. */
    version_test_native_t device_file = {0};
    bool opened = setup_ok && version_test_native_open(NULL_DEVICE, &device_file);
    bool rejected =
        opened && !cbm_daemon_build_fingerprint_native_file(version_test_native_value(&device_file),
                                                            observed);
    size_t hash_count = cbm_daemon_build_fingerprint_hash_count_for_test();
    if (opened) {
        version_test_native_close(&device_file);
    }

    if (saved_cache) {
        (void)cbm_setenv("CBM_CACHE_DIR", saved_cache, 1);
        free(saved_cache);
    } else {
        (void)cbm_unsetenv("CBM_CACHE_DIR");
    }
    cbm_daemon_build_fingerprint_cache_reset_for_test();
    cbm_daemon_build_fingerprint_hash_count_reset_for_test();
    int sidecars = version_test_count_fingerprint_entries(cache, ".cache");
    int temps = version_test_count_fingerprint_entries(cache, ".tmp");
    (void)th_rmtree(cache);

    ASSERT_TRUE(setup_ok);
    ASSERT_TRUE(opened);
    ASSERT_TRUE(rejected);
    ASSERT_STR_EQ(observed, "");
    ASSERT_EQ(hash_count, 0);
    ASSERT_EQ(sidecars, 0);
    ASSERT_EQ(temps, 0);
    PASS();
}

TEST(daemon_native_fingerprint_cache_rehashes_in_place_rewrite) {
    /* An in-place rewrite keeps the sidecar name (same device and inode), so
     * only the stored tuple can detect it: same-size new bytes with a fresh
     * mtime_ns must rehash. */
    static const char CONTENT_A[] = "cache-rewritten-same-size-a";
    static const char CONTENT_B[] = "cache-rewritten-same-size-b";
    char dir[VERSION_TEST_PATH_CAP] = {0};
    char cache[VERSION_TEST_PATH_CAP] = {0};
    char image[VERSION_TEST_PATH_CAP] = {0};
    char reference[VERSION_TEST_PATH_CAP] = {0};
    char fp_expected_b[CBM_DAEMON_BUILD_FINGERPRINT_SIZE] = {0};
    char fp_original[CBM_DAEMON_BUILD_FINGERPRINT_SIZE] = {0};
    char fp_after[CBM_DAEMON_BUILD_FINGERPRINT_SIZE] = {0};
    const char *old_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache = old_cache ? cbm_strdup(old_cache) : NULL;

    bool setup_ok = (!old_cache || saved_cache) && version_test_temp_dir(dir, "fp-inplace") &&
                    version_test_temp_dir(cache, "fp-inplace-store") &&
                    version_test_child_path(image, dir, "build.bin") &&
                    version_test_child_path(reference, dir, "reference-b.bin") &&
                    version_test_write_file(image, CONTENT_A) &&
                    version_test_write_file(reference, CONTENT_B) &&
                    cbm_setenv("CBM_CACHE_DIR", cache, 1) == 0 &&
                    version_test_write_fingerprint_cache_value(cache, "true");
    cbm_daemon_build_fingerprint_cache_reset_for_test();
    cbm_daemon_build_fingerprint_hash_count_reset_for_test();

    /* Reference hash: the exact bytes the in-place rewrite will install. */
    bool references_ok = setup_ok && cbm_daemon_build_fingerprint_file(reference, fp_expected_b);
    cbm_daemon_build_fingerprint_hash_count_reset_for_test();

    /* Baseline: the original bytes hash once and populate the sidecar. */
    bool baseline_ok = references_ok && cbm_daemon_build_fingerprint_file(image, fp_original);
    cbm_daemon_build_fingerprint_hash_count_reset_for_test();

    uint64_t device_before = 0;
    uint64_t device_after = 0;
    uint64_t inode_before = 0;
    uint64_t inode_after = 0;
    uint64_t original_mtime = 0;
    uint64_t forced_mtime = 0;
    uint64_t observed_mtime = 0;
    long original_size = baseline_ok ? version_test_file_size(image) : -1;
    bool stamped =
        baseline_ok && version_test_file_identity(image, &device_before, &inode_before) &&
        version_test_file_mtime_ns(image, &original_mtime);
    /* Same length, one byte differs, and a forced stamp beyond any filesystem
     * timestamp granularity: the tuple changes only in mtime_ns. */
    forced_mtime = stamped ? original_mtime + UINT64_C(2000000000) : 0;
    bool rewritten = stamped && version_test_write_file(image, CONTENT_B) &&
                     version_test_set_mtime_ns(image, forced_mtime) &&
                     version_test_file_mtime_ns(image, &observed_mtime) &&
                     version_test_file_identity(image, &device_after, &inode_after);
    long rewritten_size = rewritten ? version_test_file_size(image) : -1;
    bool rehash_ok = rewritten && cbm_daemon_build_fingerprint_file(image, fp_after);
    size_t rewrite_count = cbm_daemon_build_fingerprint_hash_count_for_test();

    if (saved_cache) {
        (void)cbm_setenv("CBM_CACHE_DIR", saved_cache, 1);
        free(saved_cache);
    } else {
        (void)cbm_unsetenv("CBM_CACHE_DIR");
    }
    cbm_daemon_build_fingerprint_cache_reset_for_test();
    cbm_daemon_build_fingerprint_hash_count_reset_for_test();
    (void)th_rmtree(dir);
    (void)th_rmtree(cache);

    ASSERT_TRUE(setup_ok);
    ASSERT_TRUE(references_ok);
    ASSERT_TRUE(baseline_ok);
    ASSERT_STR_NEQ(fp_original, fp_expected_b);
    ASSERT_TRUE(stamped);
    ASSERT_TRUE(rewritten);
    /* The premises the claim rests on must really hold on this filesystem. */
    ASSERT_EQ(device_after, device_before);
    ASSERT_EQ(inode_after, inode_before);
    ASSERT_EQ(rewritten_size, original_size);
    ASSERT_EQ(observed_mtime, forced_mtime);
    ASSERT_TRUE(rehash_ok);
    ASSERT_EQ(rewrite_count, 1);
    ASSERT_STR_EQ(fp_after, fp_expected_b);
    ASSERT_STR_NEQ(fp_after, fp_original);
    PASS();
}

TEST(daemon_hello_accepts_only_the_exact_active_build_identity) {
    cbm_daemon_build_identity_t active = version_test_identity("2.4.0", BUILD_A);
    cbm_daemon_build_identity_t exact = version_test_identity("2.4.0", BUILD_A);
    cbm_daemon_conflict_t conflict;

    ASSERT_EQ(version_test_compare(&active, &exact, &conflict), CBM_DAEMON_HELLO_COMPATIBLE);

    cbm_daemon_build_identity_t rebuilt = version_test_identity("2.4.0", BUILD_B);
    ASSERT_EQ(version_test_compare(&active, &rebuilt, &conflict), CBM_DAEMON_HELLO_BUILD_CONFLICT);
    ASSERT_TRUE(
        version_test_conflict_identity_matches(&conflict, "2.4.0", BUILD_A, "2.4.0", BUILD_B));
    PASS();
}

TEST(daemon_hello_version_conflict_exposes_active_and_requested_builds) {
    cbm_daemon_build_identity_t active = version_test_identity("2.3.1", BUILD_A);
    cbm_daemon_build_identity_t requested = version_test_identity("2.4.0", BUILD_B);
    cbm_daemon_conflict_t conflict;
    char visible[CBM_DAEMON_CONFLICT_MESSAGE_SIZE];

    ASSERT_EQ(version_test_compare(&active, &requested, &conflict),
              CBM_DAEMON_HELLO_VERSION_CONFLICT);
    ASSERT_TRUE(
        version_test_conflict_identity_matches(&conflict, "2.3.1", BUILD_A, "2.4.0", BUILD_B));
    ASSERT_TRUE(cbm_daemon_conflict_format(&conflict, visible, sizeof(visible)));
    ASSERT_NOT_NULL(strstr(visible, "could not start"));
    ASSERT_NOT_NULL(strstr(visible, "conflicting"));
    ASSERT_NOT_NULL(strstr(visible, "2.3.1"));
    ASSERT_NOT_NULL(strstr(visible, BUILD_A));
    ASSERT_NOT_NULL(strstr(visible, "2.4.0"));
    ASSERT_NOT_NULL(strstr(visible, BUILD_B));
    PASS();
}

TEST(daemon_hello_rejects_each_abi_mismatch) {
    cbm_daemon_build_identity_t active = version_test_identity("2.4.0", BUILD_A);
    cbm_daemon_build_identity_t requested = active;
    cbm_daemon_conflict_t conflict;

    requested.protocol_abi++;
    ASSERT_EQ(version_test_compare(&active, &requested, &conflict),
              CBM_DAEMON_HELLO_PROTOCOL_ABI_CONFLICT);
    ASSERT_TRUE(
        version_test_conflict_identity_matches(&conflict, "2.4.0", BUILD_A, "2.4.0", BUILD_A));

    requested = active;
    requested.store_abi++;
    ASSERT_EQ(version_test_compare(&active, &requested, &conflict),
              CBM_DAEMON_HELLO_STORE_ABI_CONFLICT);
    ASSERT_TRUE(
        version_test_conflict_identity_matches(&conflict, "2.4.0", BUILD_A, "2.4.0", BUILD_A));

    requested = active;
    requested.feature_abi++;
    ASSERT_EQ(version_test_compare(&active, &requested, &conflict),
              CBM_DAEMON_HELLO_FEATURE_ABI_CONFLICT);
    ASSERT_TRUE(
        version_test_conflict_identity_matches(&conflict, "2.4.0", BUILD_A, "2.4.0", BUILD_A));
    PASS();
}

TEST(daemon_hello_fails_closed_without_an_exact_build_fingerprint) {
    cbm_daemon_build_identity_t active = version_test_identity("2.4.0", BUILD_A);
    cbm_daemon_build_identity_t missing = version_test_identity("2.4.0", NULL);
    cbm_daemon_build_identity_t malformed = version_test_identity("2.4.0", "not-a-sha256");
    cbm_daemon_conflict_t conflict;

    ASSERT_EQ(version_test_compare(&active, &missing, &conflict), CBM_DAEMON_HELLO_INVALID);
    ASSERT_EQ(version_test_compare(&active, &malformed, &conflict), CBM_DAEMON_HELLO_INVALID);
    PASS();
}

TEST(daemon_conflict_log_is_durable_private_and_rotates) {
    char dir[VERSION_TEST_PATH_CAP] = {0};
    char path[VERSION_TEST_PATH_CAP] = {0};
    char rotated[VERSION_TEST_PATH_CAP] = {0};
    char lock_path[VERSION_TEST_PATH_CAP] = {0};
    char current_data[VERSION_TEST_FILE_CAP];
    char rotated_data[VERSION_TEST_FILE_CAP];
    cbm_daemon_build_identity_t active = version_test_identity("2.3.1", BUILD_A);
    cbm_daemon_build_identity_t first_requested = version_test_identity("2.4.0", BUILD_B);
    cbm_daemon_build_identity_t second_requested = version_test_identity("2.5.0", BUILD_C);
    cbm_daemon_conflict_t first_conflict;
    cbm_daemon_conflict_t second_conflict;
    bool setup_ok = version_test_temp_dir(dir, "log") &&
                    version_test_child_path(path, dir, "daemon.log") &&
                    version_test_child_path(rotated, dir, "daemon.log.1") &&
                    version_test_child_path(lock_path, dir, "daemon.log.lock");
    if (!setup_ok) {
        version_test_cleanup(dir, path, rotated, NULL);
        FAIL("could not create isolated daemon-log directory");
    }

    ASSERT_EQ(version_test_compare(&active, &first_requested, &first_conflict),
              CBM_DAEMON_HELLO_VERSION_CONFLICT);
    ASSERT_TRUE(cbm_daemon_conflict_log_append(path, &first_conflict, 4096));
    long first_size = version_test_file_size(path);
    ASSERT_GT(first_size, 0);

#ifndef _WIN32
    struct stat status;
    ASSERT_EQ(lstat(path, &status), 0);
    ASSERT_TRUE(S_ISREG(status.st_mode));
    ASSERT_EQ(status.st_mode & 0777, 0600);
    ASSERT_EQ(lstat(lock_path, &status), 0);
    ASSERT_TRUE(S_ISREG(status.st_mode));
    ASSERT_EQ(status.st_uid, geteuid());
    ASSERT_EQ(status.st_mode & 0777, 0600);
#endif

    /* The second complete record would cross this cap, so the old complete
     * generation moves to .1 and the new record becomes the active log. */
    ASSERT_EQ(version_test_compare(&active, &second_requested, &second_conflict),
              CBM_DAEMON_HELLO_VERSION_CONFLICT);
    ASSERT_TRUE(cbm_daemon_conflict_log_append(path, &second_conflict, (size_t)first_size + 1));
    ASSERT_TRUE(version_test_read_file(path, current_data));
    ASSERT_TRUE(version_test_read_file(rotated, rotated_data));

    ASSERT_NOT_NULL(strstr(rotated_data, "\"event\":\"daemon.version_conflict\""));
    ASSERT_NOT_NULL(strstr(rotated_data, "\"active_version\":\"2.3.1\""));
    ASSERT_NOT_NULL(strstr(rotated_data, "\"requested_version\":\"2.4.0\""));
    ASSERT_NOT_NULL(strstr(rotated_data, BUILD_A));
    ASSERT_NOT_NULL(strstr(rotated_data, BUILD_B));
    ASSERT_NOT_NULL(strstr(current_data, "\"requested_version\":\"2.5.0\""));
    ASSERT_NOT_NULL(strstr(current_data, BUILD_C));

    version_test_cleanup(dir, path, rotated, NULL);
    PASS();
}

#ifndef _WIN32
TEST(daemon_conflict_log_rotation_serializes_on_stable_sidecar) {
    enum { ROTATION_CAP = 4096, PADDED_LOG_SIZE = 4000 };
    char dir[VERSION_TEST_PATH_CAP] = {0};
    char path[VERSION_TEST_PATH_CAP] = {0};
    char rotated[VERSION_TEST_PATH_CAP] = {0};
    char padded[PADDED_LOG_SIZE + 1];
    char current_data[VERSION_TEST_FILE_CAP] = {0};
    cbm_daemon_build_identity_t active = version_test_identity("2.3.1", BUILD_A);
    cbm_daemon_build_identity_t seed_requested = version_test_identity("2.4.0", BUILD_B);
    cbm_daemon_build_identity_t first_requested = version_test_identity("2.5.0", BUILD_B);
    cbm_daemon_build_identity_t second_requested = version_test_identity("2.6.0", BUILD_C);
    cbm_daemon_conflict_t seed_conflict;
    cbm_daemon_conflict_t first_conflict;
    cbm_daemon_conflict_t second_conflict;
    version_test_rotation_race_t race = {0};
    version_test_append_call_t first_call = {0};
    version_test_append_call_t second_call = {0};
    cbm_thread_t first_thread;
    cbm_thread_t second_thread;
    bool first_started = false;
    bool second_started = false;

    bool setup_ok = version_test_temp_dir(dir, "log-rotation-race") &&
                    version_test_child_path(path, dir, "daemon.log") &&
                    version_test_child_path(rotated, dir, "daemon.log.1") &&
                    version_test_compare(&active, &seed_requested, &seed_conflict) ==
                        CBM_DAEMON_HELLO_VERSION_CONFLICT &&
                    version_test_compare(&active, &first_requested, &first_conflict) ==
                        CBM_DAEMON_HELLO_VERSION_CONFLICT &&
                    version_test_compare(&active, &second_requested, &second_conflict) ==
                        CBM_DAEMON_HELLO_VERSION_CONFLICT &&
                    cbm_daemon_conflict_log_append(path, &seed_conflict, 8192);
    memset(padded, 'x', PADDED_LOG_SIZE);
    padded[PADDED_LOG_SIZE - 1] = '\n';
    padded[PADDED_LOG_SIZE] = '\0';
    setup_ok = setup_ok && version_test_write_file(path, padded);
    if (!setup_ok) {
        version_test_cleanup(dir, path, rotated, NULL);
        FAIL("could not create rotation-race fixture");
    }

    first_call.path = path;
    first_call.conflict = &first_conflict;
    first_call.cap_bytes = ROTATION_CAP;
    second_call.path = path;
    second_call.conflict = &second_conflict;
    second_call.cap_bytes = ROTATION_CAP;

    cbm_daemon_conflict_log_set_test_hook(version_test_rotation_race_hook, &race);
    first_started =
        cbm_thread_create(&first_thread, 0, version_test_append_thread, &first_call) == 0;
    bool first_locked = first_started && version_test_wait_atomic_bool(&race.first_lock_acquired);
    if (first_locked) {
        second_started =
            cbm_thread_create(&second_thread, 0, version_test_append_thread, &second_call) == 0;
    }
    if (first_started) {
        (void)cbm_thread_join(&first_thread);
    }
    if (second_started) {
        (void)cbm_thread_join(&second_thread);
    }
    cbm_daemon_conflict_log_set_test_hook(NULL, NULL);

    bool complete = version_test_read_file(path, current_data);
    bool first_present = strstr(current_data, "\"requested_version\":\"2.5.0\"") != NULL;
    bool second_present = strstr(current_data, "\"requested_version\":\"2.6.0\"") != NULL;
    version_test_cleanup(dir, path, rotated, NULL);

    ASSERT_TRUE(first_started);
    ASSERT_TRUE(first_locked);
    ASSERT_TRUE(second_started);
    ASSERT_TRUE(first_call.result);
    ASSERT_TRUE(second_call.result);
    ASSERT_TRUE(complete);
    ASSERT_TRUE(first_present);
    ASSERT_TRUE(second_present);
    PASS();
}

TEST(daemon_conflict_log_rejects_symlink_destination) {
    char dir[VERSION_TEST_PATH_CAP] = {0};
    char path[VERSION_TEST_PATH_CAP] = {0};
    char victim[VERSION_TEST_PATH_CAP] = {0};
    char victim_data[VERSION_TEST_FILE_CAP];
    cbm_daemon_build_identity_t active = version_test_identity("2.3.1", BUILD_A);
    cbm_daemon_build_identity_t requested = version_test_identity("2.4.0", BUILD_B);
    cbm_daemon_conflict_t conflict;
    bool setup_ok = version_test_temp_dir(dir, "log-link") &&
                    version_test_child_path(path, dir, "daemon.log") &&
                    version_test_child_path(victim, dir, "victim.txt");
    if (!setup_ok) {
        version_test_cleanup(dir, path, NULL, victim);
        FAIL("could not create isolated symlink-test directory");
    }

    FILE *file = cbm_fopen(victim, "wb");
    bool victim_written = false;
    if (file) {
        victim_written = fputs("must-remain-unchanged", file) >= 0 && fclose(file) == 0;
        file = NULL;
    }
    if (!victim_written) {
        if (file) {
            (void)fclose(file);
        }
        version_test_cleanup(dir, path, NULL, victim);
        FAIL("could not create symlink victim");
    }
    if (symlink(victim, path) != 0) {
        version_test_cleanup(dir, path, NULL, victim);
        FAIL("could not create daemon-log symlink");
    }

    ASSERT_EQ(version_test_compare(&active, &requested, &conflict),
              CBM_DAEMON_HELLO_VERSION_CONFLICT);
    ASSERT_FALSE(cbm_daemon_conflict_log_append(path, &conflict, 4096));
    ASSERT_TRUE(version_test_read_file(victim, victim_data));
    ASSERT_STR_EQ(victim_data, "must-remain-unchanged");

    version_test_cleanup(dir, path, NULL, victim);
    PASS();
}

TEST(daemon_conflict_log_rejects_symlink_sidecar) {
    char dir[VERSION_TEST_PATH_CAP] = {0};
    char path[VERSION_TEST_PATH_CAP] = {0};
    char lock_path[VERSION_TEST_PATH_CAP] = {0};
    char victim[VERSION_TEST_PATH_CAP] = {0};
    char victim_data[VERSION_TEST_FILE_CAP];
    cbm_daemon_build_identity_t active = version_test_identity("2.3.1", BUILD_A);
    cbm_daemon_build_identity_t requested = version_test_identity("2.4.0", BUILD_B);
    cbm_daemon_conflict_t conflict;
    bool setup_ok = version_test_temp_dir(dir, "log-lock-link") &&
                    version_test_child_path(path, dir, "daemon.log") &&
                    version_test_child_path(lock_path, dir, "daemon.log.lock") &&
                    version_test_child_path(victim, dir, "victim.txt") &&
                    version_test_write_file(victim, "must-remain-unchanged") &&
                    symlink(victim, lock_path) == 0;
    if (!setup_ok) {
        version_test_cleanup(dir, path, NULL, victim);
        FAIL("could not create daemon-log lock symlink");
    }

    ASSERT_EQ(version_test_compare(&active, &requested, &conflict),
              CBM_DAEMON_HELLO_VERSION_CONFLICT);
    ASSERT_FALSE(cbm_daemon_conflict_log_append(path, &conflict, 4096));
    ASSERT_TRUE(version_test_read_file(victim, victim_data));
    ASSERT_STR_EQ(victim_data, "must-remain-unchanged");

    version_test_cleanup(dir, path, NULL, victim);
    PASS();
}
#endif

#ifdef _WIN32
typedef struct {
    const char *path;
    const cbm_daemon_conflict_t *conflict;
    atomic_bool *start;
    bool result;
} version_test_windows_append_t;

static void *version_test_windows_append_thread(void *opaque) {
    version_test_windows_append_t *call = opaque;
    struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000};
    while (!atomic_load_explicit(call->start, memory_order_acquire)) {
        (void)cbm_nanosleep(&pause, NULL);
    }
    call->result = cbm_daemon_conflict_log_append(call->path, call->conflict, 64U * 1024U);
    return NULL;
}

TEST(daemon_conflict_log_windows_concurrent_appends_are_not_dropped) {
    enum { APPEND_COUNT = 8 };
    char dir[VERSION_TEST_PATH_CAP] = {0};
    char path[VERSION_TEST_PATH_CAP] = {0};
    char rotated[VERSION_TEST_PATH_CAP] = {0};
    char current_data[VERSION_TEST_FILE_CAP] = {0};
    cbm_daemon_build_identity_t active = version_test_identity("2.3.1", BUILD_A);
    cbm_daemon_build_identity_t requested = version_test_identity("2.4.0", BUILD_B);
    cbm_daemon_conflict_t conflict;
    cbm_thread_t threads[APPEND_COUNT];
    version_test_windows_append_t calls[APPEND_COUNT];
    atomic_bool start = false;
    size_t started = 0;

    bool setup_ok =
        version_test_temp_dir(dir, "log-windows-concurrent") &&
        version_test_child_path(path, dir, "daemon.log") &&
        version_test_child_path(rotated, dir, "daemon.log.1") &&
        version_test_compare(&active, &requested, &conflict) == CBM_DAEMON_HELLO_VERSION_CONFLICT;
    if (!setup_ok) {
        version_test_cleanup(dir, path, rotated, NULL);
        FAIL("could not create Windows concurrent-log fixture");
    }

    memset(calls, 0, sizeof(calls));
    for (; started < APPEND_COUNT; started++) {
        calls[started].path = path;
        calls[started].conflict = &conflict;
        calls[started].start = &start;
        if (cbm_thread_create(&threads[started], 0, version_test_windows_append_thread,
                              &calls[started]) != 0) {
            break;
        }
    }
    atomic_store_explicit(&start, true, memory_order_release);
    for (size_t i = 0; i < started; i++) {
        (void)cbm_thread_join(&threads[i]);
    }

    bool complete = version_test_read_file(path, current_data);
    size_t records = 0;
    const char *cursor = current_data;
    static const char event[] = "\"event\":\"daemon.version_conflict\"";
    while ((cursor = strstr(cursor, event)) != NULL) {
        records++;
        cursor += sizeof(event) - 1;
    }
    bool all_succeeded = true;
    for (size_t i = 0; i < started; i++) {
        all_succeeded = all_succeeded && calls[i].result;
    }
    version_test_cleanup(dir, path, rotated, NULL);

    ASSERT_EQ(started, APPEND_COUNT);
    ASSERT_TRUE(all_succeeded);
    ASSERT_TRUE(complete);
    ASSERT_EQ(records, APPEND_COUNT);
    PASS();
}
#endif

SUITE(daemon_version) {
    RUN_TEST(daemon_rendezvous_key_is_stable_and_version_independent);
    RUN_TEST(daemon_build_fingerprint_hashes_exact_executable_bytes);
    RUN_TEST(daemon_build_fingerprint_cache_skips_rehash_on_identical_tuple);
    RUN_TEST(daemon_build_fingerprint_cache_detects_replacement_by_inode_or_size);
    RUN_TEST(daemon_build_fingerprint_cache_rejects_corrupt_sidecar);
    RUN_TEST(daemon_runtime_process_fingerprint_uses_cache);
    RUN_TEST(daemon_native_fingerprint_cache_serves_identical_handle);
    RUN_TEST(daemon_native_fingerprint_cache_never_stores_without_inode);
    RUN_TEST(daemon_native_fingerprint_cache_rehashes_in_place_rewrite);
    RUN_TEST(daemon_hello_accepts_only_the_exact_active_build_identity);
    RUN_TEST(daemon_hello_version_conflict_exposes_active_and_requested_builds);
    RUN_TEST(daemon_hello_rejects_each_abi_mismatch);
    RUN_TEST(daemon_hello_fails_closed_without_an_exact_build_fingerprint);
    RUN_TEST(daemon_conflict_log_is_durable_private_and_rotates);
#ifndef _WIN32
    RUN_TEST(daemon_conflict_log_rotation_serializes_on_stable_sidecar);
    RUN_TEST(daemon_conflict_log_rejects_symlink_destination);
    RUN_TEST(daemon_conflict_log_rejects_symlink_sidecar);
#endif
#ifdef _WIN32
    RUN_TEST(daemon_conflict_log_windows_concurrent_appends_are_not_dropped);
#endif
}
