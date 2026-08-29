# Runtime Idle Performance Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove idle frontend and daemon polling by adding portable condition variables, predicate-driven frontend waits, an interruptible infinite IPC accept, and condition-driven runtime state waiters without changing queue, cancellation, drain, activation, or shutdown contracts.

**Architecture:** Add one portable condition-variable API beside `cbm_mutex_t`; use one mutex-bound condition per frontend state machine and one per runtime service. Make the IPC listener's accept wait interruptible through a reusable coalesced wake channel (POSIX nonblocking pipe, Windows manual-reset event), so runtime accept can wait forever and be woken only by real state changes. Keep all predicates and state transitions under their owning mutex, tolerate spurious wakes with `while` loops, and perform the exact runtime-token cancellation outside the frontend mutex.

**Tech Stack:** C11, pthreads, Win32 `CONDITION_VARIABLE`/`CRITICAL_SECTION`, POSIX `poll`/pipe, Win32 overlapped named pipes and manual-reset events, the repository C test runner, ASan/UBSan, TSan, LSan, and the daemon soak harness.

**Spec:** `docs/superpowers/specs/2026-08-28-upstream-performance-integration-design.md`

## Global Constraints

- Begin only after the integration owner has merged pinned upstream `31b611a30e01aa8f306f9fcba32492cad0702f99`, reviewed the merge, and made the targeted preservation suite green.
- Begin frontend work from merged commit `7710f86e927c5221c395ecbdab9d0f3b75edcb71`; its single independent maintenance observer and test seams are inputs, not code to recreate.
- Stop if either pinned reference differs; updating upstream requires a new audit.
- Preserve frontend queue capacity (`8`), queue byte limit (`12 * 1024 * 1024`), request timeout (24 hours), EOF no-progress window (`15000 ms`), close/join watchdog (`5000 ms`), and maintenance grace calculation.
- Preserve zero-loss backpressure, final-response/EOF drain behavior, exact request-token cancellation, activation acknowledgement ordering, and existing shutdown deadlines.
- Every condition wait must hold the predicate mutex, use a `while` predicate loop, and re-check after signal, broadcast, timeout, or spurious wake.
- Maintenance cancellation sets `stopping`, captures the active request token, and broadcasts while holding `frontend_state_t.mutex`; it calls `cbm_daemon_runtime_client_application_cancel()` only after unlocking.
- Idle frontend worker: zero periodic wakeups after it enters its condition wait.
- Idle runtime accept: `CBM_DAEMON_IPC_WAIT_FOREVER`, with zero timeout returns; only connection readiness, an explicit listener interrupt, or an error may return it.
- Keep bounded exceptional cleanup/startup retry loops unchanged, including 1 ms coordination cleanup retries and the exceptional accept-error backoff.
- No semantic index version change, cache reset, live cache mutation, live binary replacement, global configuration change, push, PR, or release action belongs to this plan.
- Build and test only in the isolated worktree, with task-specific build directories and test-created runtime/cache directories.

## Research and platform contracts

- Second Brain query: `sb query "portable condition variables mutex predicate loops spurious wakeups interruptible socket accept wake pipe Windows manual-reset event" --research biblioteca --top-k 8`. It returned no strong implementation-specific directive; the useful general signal was to make concurrent state transitions explicitly observable and avoid mutable unsynchronized state.
- POSIX `pthread_cond_wait` requires the mutex to be held, atomically releases/reacquires it, and permits spurious wakeups; therefore every caller below owns the predicate and loops while it remains false: <https://pubs.opengroup.org/onlinepubs/7908799/xsh/pthread_cond_wait.html>.
- POSIX broadcast may wake multiple waiters and still requires predicate re-checking: <https://pubs.opengroup.org/onlinepubs/9699919799/functions/pthread_cond_broadcast.html>.
- Windows condition waits also permit spurious/stolen wakes and require re-checking under the critical section: <https://learn.microsoft.com/en-us/archive/msdn-magazine/2007/june/concurrency-synchronization-primitives-new-to-windows-vista>.
- Windows overlapped named-pipe completion uses a manual-reset event and can be combined with a second event in `WaitForMultipleObjects`: <https://learn.microsoft.com/en-us/windows/win32/ipc/named-pipe-server-using-overlapped-i-o> and <https://learn.microsoft.com/en-us/windows/win32/ipc/synchronous-and-overlapped-input-and-output>.
- Repository evidence: the code graph on the original `feat/request-metrics` checkout supplied provisional symbol locations for `compat_thread`, frontend queue helpers, runtime accept/state waiters, and IPC accept. Its required coverage check failed when the MCP transport closed, so this plan relies on direct reads of the pinned `81b02389`, `31b611a3`, and `7710f86e` source objects for exact interfaces and discloses the graph limitation rather than making an exhaustive graph claim.

## File map and ownership

| Task | Exclusive files while task is active | Responsibility |
|---|---|---|
| 1 | `src/foundation/compat_thread.h`, `src/foundation/compat_thread.c`, `tests/test_daemon_frontend.c` | Portable condition API and direct signal/broadcast/timeout contract test. |
| 2 | `src/daemon/frontend.c`, `tests/test_daemon_frontend.c` | Queue, backpressure, EOF progress, worker stop, join watchdog, and maintenance cancellation predicates. Starts only after Task 1 releases `tests/test_daemon_frontend.c`. |
| 3 | `src/daemon/ipc.h`, `src/daemon/ipc_internal.h`, `src/daemon/ipc.c`, `tests/test_daemon_ipc.c` | Reusable listener interrupt and infinite accept on POSIX/Windows. May run in parallel with Task 2 after Task 1. |
| 4 | `src/daemon/runtime.c`, `src/daemon/runtime.h`, `tests/test_daemon_runtime.c` | Infinite accept loop, worker-reap wakeups, service state condition, and public count/exited waiters. Starts only after Tasks 1 and 3. |
| 5 | no source edits | Focused, sanitizer, full-suite, platform, static-contract, and soak verification. |

No task edits `host.c`, `application.c`, configuration parsing, watcher code, browse/UI files, i18n, `Makefile.cbm`, `scripts/test.sh`, or shared test registration. This keeps the runtime-idle lane disjoint from index/watcher and browse/UI plans. Tasks 1 and 2 are serialized because both deliberately extend `tests/test_daemon_frontend.c`; all other file ownership is non-overlapping.

---

### Task 1: Portable Condition Variable Foundation

**Files:**
- Modify: `src/foundation/compat_thread.h`
- Modify: `src/foundation/compat_thread.c`
- Test: `tests/test_daemon_frontend.c`

**Interfaces:**
- Consumes: existing `cbm_mutex_t`, `cbm_mutex_lock(cbm_mutex_t *)`, and `cbm_mutex_unlock(cbm_mutex_t *)`.
- Produces:

```c
typedef enum {
    CBM_CONDITION_ERROR = -1,
    CBM_CONDITION_SIGNALED = 0,
    CBM_CONDITION_TIMED_OUT = 1,
} cbm_condition_wait_result_t;

typedef struct {
#ifdef _WIN32
    CONDITION_VARIABLE condition;
#else
    pthread_cond_t condition;
#endif
} cbm_thread_condition_t;

bool cbm_condition_init(cbm_thread_condition_t *condition);
cbm_condition_wait_result_t cbm_condition_wait(cbm_thread_condition_t *condition,
                                                cbm_mutex_t *mutex);
cbm_condition_wait_result_t cbm_condition_timed_wait(cbm_thread_condition_t *condition,
                                                      cbm_mutex_t *mutex,
                                                      uint32_t timeout_ms);
void cbm_condition_signal(cbm_thread_condition_t *condition);
void cbm_condition_broadcast(cbm_thread_condition_t *condition);
void cbm_condition_destroy(cbm_thread_condition_t *condition);
```

- Contract: `cbm_condition_wait*` return with the same mutex re-acquired. `CBM_CONDITION_TIMED_OUT` is not an error. `cbm_condition_timed_wait(..., UINT32_MAX)` delegates to the infinite wait. Callers still own predicate loops.

- [ ] **Step 1: Add the failing portable condition contract test**

Add this fixture and test above `SUITE(daemon_frontend)` in `tests/test_daemon_frontend.c`, then register the test first in that suite. The workers and main thread share one condition/mutex and use explicit predicates.

```c
typedef struct {
    cbm_mutex_t mutex;
    cbm_thread_condition_t condition;
    unsigned int ready;
    unsigned int released;
    bool release;
} frontend_condition_fixture_t;

static void *frontend_condition_waiter(void *opaque) {
    frontend_condition_fixture_t *fixture = opaque;
    cbm_mutex_lock(&fixture->mutex);
    fixture->ready++;
    cbm_condition_broadcast(&fixture->condition);
    while (!fixture->release) {
        if (cbm_condition_wait(&fixture->condition, &fixture->mutex) == CBM_CONDITION_ERROR) {
            cbm_mutex_unlock(&fixture->mutex);
            return NULL;
        }
    }
    fixture->released++;
    cbm_mutex_unlock(&fixture->mutex);
    return NULL;
}

TEST(daemon_frontend_portable_condition_broadcasts_and_times_out) {
    frontend_condition_fixture_t fixture = {0};
    cbm_mutex_init(&fixture.mutex);
    bool initialized = cbm_condition_init(&fixture.condition);
    cbm_thread_t first;
    cbm_thread_t second;
    bool first_started = initialized &&
                         cbm_thread_create(&first, 0, frontend_condition_waiter, &fixture) == 0;
    bool second_started = first_started &&
                          cbm_thread_create(&second, 0, frontend_condition_waiter, &fixture) == 0;

    cbm_condition_wait_result_t ready_wait = CBM_CONDITION_SIGNALED;
    cbm_mutex_lock(&fixture.mutex);
    while (second_started && fixture.ready != 2 && ready_wait == CBM_CONDITION_SIGNALED) {
        ready_wait = cbm_condition_timed_wait(&fixture.condition, &fixture.mutex, 2000);
    }
    fixture.release = true;
    if (initialized) {
        cbm_condition_broadcast(&fixture.condition);
    }
    cbm_mutex_unlock(&fixture.mutex);

    bool first_joined = first_started && cbm_thread_join(&first) == 0;
    bool second_joined = second_started && cbm_thread_join(&second) == 0;
    cbm_condition_wait_result_t timeout = CBM_CONDITION_ERROR;
    if (initialized) {
        cbm_mutex_lock(&fixture.mutex);
        timeout = cbm_condition_timed_wait(&fixture.condition, &fixture.mutex, 20);
        cbm_mutex_unlock(&fixture.mutex);
    }
    unsigned int released = fixture.released;
    if (initialized) {
        cbm_condition_destroy(&fixture.condition);
    }
    cbm_mutex_destroy(&fixture.mutex);

    ASSERT_TRUE(initialized);
    ASSERT_TRUE(first_started);
    ASSERT_TRUE(second_started);
    ASSERT_TRUE(first_joined);
    ASSERT_TRUE(second_joined);
    ASSERT_EQ(ready_wait, CBM_CONDITION_SIGNALED);
    ASSERT_EQ(released, 2);
    ASSERT_EQ(timeout, CBM_CONDITION_TIMED_OUT);
    PASS();
}
```

- [ ] **Step 2: Run test to verify RED**

Run:

```bash
scripts/test.sh SANITIZE= BUILD_DIR=build/runtime-idle-red-condition --suites daemon_frontend
```

Expected RED: compilation fails because `cbm_thread_condition_t`, `CBM_CONDITION_*`, and `cbm_condition_*` are not defined. Record the first compiler diagnostic; if it compiles, stop for overlapping-implementation reconciliation.

- [ ] **Step 3: Declare the exact API**

In `compat_thread.h`, include `<stdbool.h>` and `<stdint.h>`, place `cbm_thread_condition_t` immediately after `cbm_mutex_t`, and add the signatures from Interfaces. Do not expose native error numbers.

- [ ] **Step 4: Implement Win32 conditions**

Add beside the mutex implementation in `compat_thread.c`:

```c
bool cbm_condition_init(cbm_thread_condition_t *condition) {
    if (!condition) return false;
    InitializeConditionVariable(&condition->condition);
    return true;
}

cbm_condition_wait_result_t cbm_condition_wait(cbm_thread_condition_t *condition,
                                                cbm_mutex_t *mutex) {
    return condition && mutex &&
                   SleepConditionVariableCS(&condition->condition, &mutex->cs, INFINITE)
               ? CBM_CONDITION_SIGNALED
               : CBM_CONDITION_ERROR;
}

cbm_condition_wait_result_t cbm_condition_timed_wait(cbm_thread_condition_t *condition,
                                                      cbm_mutex_t *mutex,
                                                      uint32_t timeout_ms) {
    if (timeout_ms == UINT32_MAX) return cbm_condition_wait(condition, mutex);
    if (!condition || !mutex) return CBM_CONDITION_ERROR;
    if (SleepConditionVariableCS(&condition->condition, &mutex->cs, (DWORD)timeout_ms)) {
        return CBM_CONDITION_SIGNALED;
    }
    return GetLastError() == ERROR_TIMEOUT ? CBM_CONDITION_TIMED_OUT : CBM_CONDITION_ERROR;
}

void cbm_condition_signal(cbm_thread_condition_t *condition) {
    if (condition) WakeConditionVariable(&condition->condition);
}

void cbm_condition_broadcast(cbm_thread_condition_t *condition) {
    if (condition) WakeAllConditionVariable(&condition->condition);
}

void cbm_condition_destroy(cbm_thread_condition_t *condition) {
    (void)condition;
}
```

- [ ] **Step 5: Implement POSIX conditions with monotonic finite waits**

Use `pthread_cond_timedwait_relative_np()` on macOS. On Linux, FreeBSD, and NetBSD, initialize the condition with `CLOCK_MONOTONIC` and use an absolute monotonic deadline.

```c
static void condition_timespec_add_ms(struct timespec *value, uint32_t timeout_ms) {
    value->tv_sec += (time_t)(timeout_ms / 1000U);
    value->tv_nsec += (long)(timeout_ms % 1000U) * 1000000L;
    if (value->tv_nsec >= 1000000000L) {
        value->tv_sec++;
        value->tv_nsec -= 1000000000L;
    }
}

bool cbm_condition_init(cbm_thread_condition_t *condition) {
    if (!condition) return false;
#ifdef __APPLE__
    return pthread_cond_init(&condition->condition, NULL) == 0;
#else
    pthread_condattr_t attributes;
    if (pthread_condattr_init(&attributes) != 0) return false;
    int clock_result = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
    int init_result = clock_result == 0
                          ? pthread_cond_init(&condition->condition, &attributes)
                          : clock_result;
    (void)pthread_condattr_destroy(&attributes);
    return init_result == 0;
#endif
}

cbm_condition_wait_result_t cbm_condition_wait(cbm_thread_condition_t *condition,
                                                cbm_mutex_t *mutex) {
    if (!condition || !mutex) return CBM_CONDITION_ERROR;
    return pthread_cond_wait(&condition->condition, &mutex->mtx) == 0
               ? CBM_CONDITION_SIGNALED
               : CBM_CONDITION_ERROR;
}

cbm_condition_wait_result_t cbm_condition_timed_wait(cbm_thread_condition_t *condition,
                                                      cbm_mutex_t *mutex,
                                                      uint32_t timeout_ms) {
    if (timeout_ms == UINT32_MAX) return cbm_condition_wait(condition, mutex);
    if (!condition || !mutex) return CBM_CONDITION_ERROR;
#ifdef __APPLE__
    struct timespec relative = {
        .tv_sec = (time_t)(timeout_ms / 1000U),
        .tv_nsec = (long)(timeout_ms % 1000U) * 1000000L,
    };
    int result = pthread_cond_timedwait_relative_np(&condition->condition, &mutex->mtx, &relative);
#else
    struct timespec deadline;
    if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) return CBM_CONDITION_ERROR;
    condition_timespec_add_ms(&deadline, timeout_ms);
    int result = pthread_cond_timedwait(&condition->condition, &mutex->mtx, &deadline);
#endif
    if (result == 0) return CBM_CONDITION_SIGNALED;
    return result == ETIMEDOUT ? CBM_CONDITION_TIMED_OUT : CBM_CONDITION_ERROR;
}

void cbm_condition_signal(cbm_thread_condition_t *condition) {
    if (condition) (void)pthread_cond_signal(&condition->condition);
}

void cbm_condition_broadcast(cbm_thread_condition_t *condition) {
    if (condition) (void)pthread_cond_broadcast(&condition->condition);
}

void cbm_condition_destroy(cbm_thread_condition_t *condition) {
    if (condition) (void)pthread_cond_destroy(&condition->condition);
}
```

Include `<errno.h>`, `<stdint.h>`, and `<time.h>`. A supported non-Apple target must fail initialization rather than silently use `CLOCK_REALTIME`.

- [ ] **Step 6: Run GREEN**

Run:

```bash
scripts/test.sh SANITIZE= BUILD_DIR=build/runtime-idle-condition --suites daemon_frontend
```

Expected GREEN: `daemon_frontend_portable_condition_broadcasts_and_times_out` and the complete frontend suite pass.

- [ ] **Step 7: Review and release the shared test file**

Run:

```bash
git diff --check -- src/foundation/compat_thread.h src/foundation/compat_thread.c tests/test_daemon_frontend.c
git diff --stat -- src/foundation/compat_thread.h src/foundation/compat_thread.c tests/test_daemon_frontend.c
```

Expected: only Task 1 paths. Release `tests/test_daemon_frontend.c` to Task 2 after GREEN.

---

### Task 2: Frontend Blocking Queue, Backpressure, EOF, Stop, and Cancellation

**Files:**
- Modify: `src/daemon/frontend.c`
- Test: `tests/test_daemon_frontend.c`

**Interfaces:**
- Consumes: Task 1 `cbm_thread_condition_t` API; merged `7710f86e` observer functions, including `cbm_daemon_frontend_test_worker_idle_cycles()`.
- Produces: no new production public API. `frontend_state_t` gains one `cbm_thread_condition_t changed`; `completed_items` becomes a mutex-protected `uint64_t`. Every predicate-changing mutation broadcasts `changed` while `state->mutex` is held.
- Preserves: `cbm_daemon_frontend_mcp_run(...)`, maintenance monitor public functions, cancellation parsing/routing, queue size/byte bounds, EOF no-progress reset, and watchdog deadline.

- [ ] **Step 1: Strengthen the idle regression so polling is RED**

In merged test `daemon_frontend_idle_uses_one_maintenance_observer`, after the worker first reports an idle wait, hold the independent maintenance observer for another 100 ms and require the worker's idle-wait entry count to remain unchanged:

```c
uint64_t idle_waits_before = cbm_daemon_frontend_test_worker_idle_cycles();
cbm_usleep(100U * 1000U);
uint64_t idle_waits_after = cbm_daemon_frontend_test_worker_idle_cycles();
/* Existing cleanup follows before assertions. */
ASSERT_TRUE(idle_waits_before > 0);
ASSERT_EQ(idle_waits_after, idle_waits_before);
```

Keep the existing assertions that the monitor made exactly one observation and the worker made zero maintenance observations.

- [ ] **Step 2: Run test to verify RED**

Run:

```bash
scripts/test.sh SANITIZE= BUILD_DIR=build/runtime-idle-red-frontend --suites daemon_frontend
```

Expected RED: `daemon_frontend_idle_uses_one_maintenance_observer` reports `idle_waits_after > idle_waits_before`, because `frontend_worker()` wakes every `FRONTEND_IDLE_WAIT_US`.

- [ ] **Step 3: Add the frontend state condition and deadline helper**

Change the relevant `frontend_state_t` members to:

```c
cbm_mutex_t mutex;
cbm_thread_condition_t changed;
/* existing client, queue, identity, and boolean fields */
uint64_t completed_items;
```

Delete `FRONTEND_WAIT_US` and `FRONTEND_IDLE_WAIT_US`. Keep `FRONTEND_MAINTENANCE_POLL_MS` for the independent cross-process maintenance observer. Add:

```c
static uint32_t frontend_deadline_remaining_ms(uint64_t deadline_ms) {
    uint64_t now_ms = cbm_now_ms();
    if (now_ms >= deadline_ms) return 0;
    uint64_t remaining = deadline_ms - now_ms;
    return remaining >= UINT32_MAX ? UINT32_MAX - 1U : (uint32_t)remaining;
}

static void frontend_changed_locked(frontend_state_t *state) {
    cbm_condition_broadcast(&state->changed);
}
```

- [ ] **Step 4: Replace worker idle polling with the queue predicate**

Refactor `frontend_pop_begin()` to hold `state->mutex` across:

```c
while (!state->stopping && !state->failed && state->count == 0 && !state->input_closed) {
#if defined(CBM_ENABLE_TEST_SEAMS) && CBM_ENABLE_TEST_SEAMS
    atomic_fetch_add_explicit(&frontend_test_worker_idle_count, 1, memory_order_release);
#endif
    if (cbm_condition_wait(&state->changed, &state->mutex) == CBM_CONDITION_ERROR) {
        state->failed = true;
        state->stopping = true;
        frontend_changed_locked(state);
        break;
    }
}
```

Pop only when `!stopping && !failed && count > 0`. Preserve token reservation and active-request publication in the same critical section. After decrementing `count`/`queued_bytes`, broadcast before unlock so a backpressured reader can re-check capacity. Remove the idle sleep branch from `frontend_worker()`; a false pop now means the stop/input predicate is terminal.

- [ ] **Step 5: Replace enqueue polling with a capacity predicate**

Keep JSON parsing and oversize rejection outside the lock. For admissible messages, lock once and wait:

```c
while (!state->stopping && !state->failed &&
       !(state->count < FRONTEND_QUEUE_CAPACITY &&
         state->queued_bytes <= FRONTEND_QUEUE_BYTES_MAX &&
         length <= FRONTEND_QUEUE_BYTES_MAX - state->queued_bytes)) {
    if (cbm_condition_wait(&state->changed, &state->mutex) == CBM_CONDITION_ERROR) {
        state->failed = true;
        state->stopping = true;
        frontend_changed_locked(state);
        break;
    }
}
```

If stopped/failed, unlock and free only `id_str`; the caller still owns `message`. If capacity is true, enqueue, update `count`/`queued_bytes`, broadcast, unlock, and return true. For an over-byte-budget frame, set `failed` and `stopping`, broadcast under the mutex, unlock, and return false.

- [ ] **Step 6: Make every frontend predicate transition observable**

Under `state->mutex`, broadcast after each exact transition:

```c
/* frontend_worker_mark_done */
state->worker_done = true;

/* frontend_input_closed */
state->input_closed = true;

/* frontend_end_request */
state->in_request = false;
state->active_has_id = false;
state->active_id = 0;
state->active_id_str = NULL;
state->active_request_token = CBM_DAEMON_RUNTIME_APPLICATION_TOKEN_INVALID;
state->failed = state->failed || failed;
state->completed_items++;

/* frontend_stop_begin */
state->stopping = true;

/* token-reservation failure */
state->failed = true;
state->stopping = true;
```

Remove the separate atomic increment of `completed_items`; completion and its wake become one critical section.

- [ ] **Step 7: Preserve exact maintenance cancellation ordering**

Implement `frontend_cancel_for_maintenance()` with this order:

```c
static bool frontend_cancel_for_maintenance(void *opaque) {
    frontend_state_t *state = opaque;
    cbm_daemon_runtime_application_token_t request_token =
        CBM_DAEMON_RUNTIME_APPLICATION_TOKEN_INVALID;
    cbm_mutex_lock(&state->mutex);
    state->stopping = true;
    if (state->in_request) request_token = state->active_request_token;
    frontend_changed_locked(state);
    cbm_mutex_unlock(&state->mutex);

    if (request_token == CBM_DAEMON_RUNTIME_APPLICATION_TOKEN_INVALID) return false;
    return cbm_daemon_runtime_client_application_cancel(state->client, request_token) ==
           CBM_DAEMON_RUNTIME_CANCEL_ACCEPTED;
}
```

The runtime call stays outside the mutex. Do not replace the exact token with a session-wide close.

- [ ] **Step 8: Replace EOF progress polling with a timed predicate loop**

Add this helper and call it after `frontend_input_closed(&state)`:

```c
static bool frontend_wait_for_eof_drain(frontend_state_t *state) {
    cbm_mutex_lock(&state->mutex);
    uint64_t completed = state->completed_items;
    uint64_t deadline = cbm_now_ms() + FRONTEND_EOF_DRAIN_MS;
    while (!state->worker_done) {
        uint32_t remaining_ms = frontend_deadline_remaining_ms(deadline);
        if (remaining_ms == 0) break;
        cbm_condition_wait_result_t waited =
            cbm_condition_timed_wait(&state->changed, &state->mutex, remaining_ms);
        if (waited == CBM_CONDITION_ERROR) {
            state->failed = true;
            state->stopping = true;
            frontend_changed_locked(state);
            cbm_mutex_unlock(&state->mutex);
            return false;
        }
        if (state->completed_items != completed) {
            completed = state->completed_items;
            deadline = cbm_now_ms() + FRONTEND_EOF_DRAIN_MS;
        }
    }
    cbm_mutex_unlock(&state->mutex);
    return true;
}
```

A timeout keeps the existing clean-EOF behavior: the subsequent stop path cancels the stuck request and returns success unless an independent failure occurred. Each completed item resets the full 15-second no-progress deadline.

- [ ] **Step 9: Replace join-watchdog polling with worker-done waiting**

Change `frontend_join_watchdog_t` to hold only `frontend_state_t *state`. The watchdog uses:

```c
static void *frontend_join_watchdog(void *opaque) {
    frontend_join_watchdog_t *watchdog = opaque;
    frontend_state_t *state = watchdog->state;
    uint64_t deadline = cbm_now_ms() + FRONTEND_JOIN_WATCHDOG_MS;
    cbm_mutex_lock(&state->mutex);
    while (!state->worker_done) {
        uint32_t remaining_ms = frontend_deadline_remaining_ms(deadline);
        if (remaining_ms == 0 ||
            cbm_condition_timed_wait(&state->changed, &state->mutex, remaining_ms) ==
                CBM_CONDITION_ERROR) {
            cbm_mutex_unlock(&state->mutex);
            _Exit(EXIT_FAILURE);
        }
    }
    cbm_mutex_unlock(&state->mutex);
    return NULL;
}
```

The main thread starts it only when `frontend_worker_is_done()` is false, joins the worker, then joins the watchdog. Remove watchdog `atomic_bool complete` and the post-join store.

- [ ] **Step 10: Initialize and destroy the condition on every path**

After `cbm_mutex_init(&state.mutex)`, require `cbm_condition_init(&state.changed)`. Failure destroys the mutex, closes the client within `FRONTEND_CLOSE_TIMEOUT_MS`, and returns `-1`. Destroy the condition after worker/watchdog/monitor joins and queued-item cleanup, immediately before mutex destruction. Every early return after successful condition initialization destroys it.

- [ ] **Step 11: Run GREEN and the no-polling proof**

Run:

```bash
scripts/test.sh SANITIZE= BUILD_DIR=build/runtime-idle-frontend --suites daemon_frontend
rg -n "FRONTEND_(WAIT_US|IDLE_WAIT_US)|cbm_usleep\(FRONTEND_(WAIT_US|IDLE_WAIT_US)" src/daemon/frontend.c
```

Expected GREEN: overflow/backpressure, EOF timeout, stdout backpressure, blocked-stdio maintenance, exact cancellation, portable condition, and strengthened idle tests pass. Expected `rg`: no matches. `FRONTEND_MAINTENANCE_POLL_MS` remains only in the independent monitor and bounded grace loop.

- [ ] **Step 12: Inspect the frontend-only diff**

Run:

```bash
git diff --check -- src/daemon/frontend.c tests/test_daemon_frontend.c
git diff -- src/daemon/frontend.c tests/test_daemon_frontend.c
```

Review each condition wait for a mutex-held `while` predicate and each predicate mutation for an under-lock broadcast. Reject any runtime client call made while `state->mutex` is held.

---

### Task 3: Interruptible Infinite IPC Accept on POSIX and Windows

**Files:**
- Modify: `src/daemon/ipc.h`
- Modify: `src/daemon/ipc_internal.h`
- Modify: `src/daemon/ipc.c`
- Test: `tests/test_daemon_ipc.c`

**Interfaces:**
- Consumes: existing `cbm_daemon_ipc_accept(listener, timeout_ms, connection_out)` tri-state API and `CBM_DAEMON_IPC_WAIT_FOREVER`.
- Produces:

```c
/* Thread-safe, reusable, and coalescing. Wakes an in-progress accept. The
 * awakened accept returns 0 with *connection_out == NULL. It does not close
 * the listener; callers may accept again after handling the state change. */
bool cbm_daemon_ipc_listener_interrupt(cbm_daemon_ipc_listener_t *listener);

#if defined(CBM_ENABLE_TEST_SEAMS) && CBM_ENABLE_TEST_SEAMS
void cbm_daemon_ipc_accept_observer_reset_for_testing(void);
uint64_t cbm_daemon_ipc_accept_waits_for_testing(void);
uint64_t cbm_daemon_ipc_accept_timeouts_for_testing(void);
#endif
```

- Lifetime contract: `listener_interrupt()` may race with `accept()`. `listener_close()` still requires all accept callers to be joined; it is not a concurrent free operation.

- [ ] **Step 1: Add RED tests for infinite blocking, explicit wake, and finite timeout**

Add this cross-platform fixture in `tests/test_daemon_ipc.c`:

```c
typedef struct {
    cbm_daemon_ipc_listener_t *listener;
    atomic_bool returned;
    int result;
    cbm_daemon_ipc_connection_t *connection;
} ipc_accept_wait_t;

static void *ipc_accept_wait_forever(void *opaque) {
    ipc_accept_wait_t *wait = opaque;
    wait->result = cbm_daemon_ipc_accept(wait->listener, CBM_DAEMON_IPC_WAIT_FOREVER,
                                         &wait->connection);
    atomic_store_explicit(&wait->returned, true, memory_order_release);
    return NULL;
}
```

Add `daemon_ipc_infinite_accept_blocks_until_listener_interrupt`: use existing secure parent/endpoint/listener helpers; reset observer; start accept thread; wait at most 2 seconds for wait count `1`; sleep 100 ms; assert `returned == false`, timeouts `0`, waits `1`; call `listener_interrupt()` twice; join; assert result `0` and connection `NULL`; close listener after join.

Add `daemon_ipc_finite_accept_timeout_remains_bounded`: on a fresh listener without a client, call `accept(listener, 20, &connection)` and assert result `0`, null connection, timeout counter `1`.

Use this exact body shape for the infinite case; the finite case repeats the same parent/endpoint/listener construction and cleanup without creating a thread:

```c
TEST(daemon_ipc_infinite_accept_blocks_until_listener_interrupt) {
    char parent[TEST_PATH_CAP];
    bool parent_ready = ipc_test_parent_new(parent, "accept-interrupt");
    cbm_daemon_ipc_endpoint_t *endpoint =
        parent_ready ? cbm_daemon_ipc_endpoint_new("0123456789abcdef", parent) : NULL;
    cbm_daemon_ipc_listener_t *listener = endpoint ? cbm_daemon_ipc_listen(endpoint) : NULL;
    ipc_accept_wait_t wait = {.listener = listener, .result = -1};
    atomic_init(&wait.returned, false);
    cbm_daemon_ipc_accept_observer_reset_for_testing();
    cbm_thread_t thread;
    bool started = listener && cbm_thread_create(&thread, 0, ipc_accept_wait_forever, &wait) == 0;
    uint64_t deadline = cbm_now_ms() + 2000U;
    while (started && cbm_daemon_ipc_accept_waits_for_testing() < 1 && cbm_now_ms() < deadline) {
        cbm_usleep(1000);
    }
    uint64_t waits_before = cbm_daemon_ipc_accept_waits_for_testing();
    cbm_usleep(100U * 1000U);
    bool blocked = !atomic_load_explicit(&wait.returned, memory_order_acquire);
    uint64_t waits_after = cbm_daemon_ipc_accept_waits_for_testing();
    uint64_t timeouts = cbm_daemon_ipc_accept_timeouts_for_testing();
    bool first_interrupt = started && cbm_daemon_ipc_listener_interrupt(listener);
    bool second_interrupt = started && cbm_daemon_ipc_listener_interrupt(listener);
    bool joined = started && cbm_thread_join(&thread) == 0;
    if (wait.connection) cbm_daemon_ipc_connection_close(wait.connection);
    cbm_daemon_ipc_listener_close(listener);
    cbm_daemon_ipc_endpoint_free(endpoint);
    bool removed = !parent_ready || th_rmtree(parent) == 0;

    ASSERT_TRUE(parent_ready);
    ASSERT_TRUE(listener != NULL);
    ASSERT_TRUE(started);
    ASSERT_EQ(waits_before, 1);
    ASSERT_TRUE(blocked);
    ASSERT_EQ(waits_after, waits_before);
    ASSERT_EQ(timeouts, 0);
    ASSERT_TRUE(first_interrupt);
    ASSERT_TRUE(second_interrupt);
    ASSERT_TRUE(joined);
    ASSERT_EQ(wait.result, 0);
    ASSERT_TRUE(wait.connection == NULL);
    ASSERT_TRUE(removed);
    PASS();
}
```

- [ ] **Step 2: Run test to verify RED**

Run:

```bash
scripts/test.sh SANITIZE= BUILD_DIR=build/runtime-idle-red-ipc --suites daemon_ipc
```

Expected RED: compile/link failure for `cbm_daemon_ipc_listener_interrupt` and observer functions. If those symbols exist, stop and reconcile their semantics first.

- [ ] **Step 3: Declare interrupt and observer contracts**

Add `cbm_daemon_ipc_listener_interrupt()` beside `listener_close()` in `ipc.h`, including the lifetime/return contract from Interfaces. Add observer declarations to `ipc_internal.h` under the test guard. In `ipc.c`, use test-only atomic counters: reset; increment before each accept kernel wait; increment timeout count only when a finite kernel wait actually times out.

- [ ] **Step 4: Add the reusable POSIX wake pipe**

Extend the POSIX listener with:

```c
int wake_read_fd;
int wake_write_fd;
cbm_mutex_t wake_mutex;
bool wake_mutex_initialized;
bool wake_pending;
```

Immediately after allocation set both wake descriptors to `-1`, initialize `wake_mutex`, create a pipe, and apply `FD_CLOEXEC` plus `O_NONBLOCK` to both ends before publishing the listener. Failure closes both ends, destroys the initialized mutex, and follows existing construction cleanup.

Implement interrupt as a coalesced one-byte nonblocking write while holding `wake_mutex`: if `wake_pending`, return true; otherwise write one byte, treat `EAGAIN`/`EWOULDBLOCK` as already readable, set `wake_pending = true`, unlock, and return true. Other write errors return false.

In `accept()`, poll wake pipe and listening socket, with wake first so a simultaneous state wake wins. On wake readiness, lock `wake_mutex`, drain until `EAGAIN`, set `wake_pending = false`, unlock, and return `0`. Preserve `EINTR` retry and finite-deadline calculation. Socket readiness follows the existing authenticated accept path unchanged.

- [ ] **Step 5: Add the reusable Windows manual-reset interrupt event**

Extend the Windows listener with:

```c
HANDLE interrupt_event;
cbm_mutex_t wake_mutex;
bool wake_mutex_initialized;
bool wake_pending;
```

Create `interrupt_event = CreateEventW(NULL, TRUE, FALSE, NULL)` during listener construction. `TRUE` is mandatory. Under `wake_mutex`, interrupt sets `wake_pending` and calls `SetEvent`; failure restores `wake_pending = false` and returns false.

When `ConnectNamedPipe` is pending, wait with:

```c
HANDLE events[2] = {listener->interrupt_event, listener->connect_event};
DWORD wait = WaitForMultipleObjects(2, events, FALSE, timeout_ms);
```

If interrupt wins, lock `wake_mutex`, clear `wake_pending`, call `ResetEvent` while still holding the mutex, unlock, and return `0`. This closes the SetEvent/ResetEvent lost-wake race. Leave overlapped connect armed; next accept observes existing `connect_pending`. If connect wins, retain `GetOverlappedResult` and identity validation.

- [ ] **Step 6: Close wake resources after accept ownership ends**

In both `listener_close()` implementations, close/destroy wake descriptors/event and wake mutex along fully and partially constructed paths. Keep `CancelIoEx` for Windows pending connect. Runtime must interrupt, join accept thread, then close/free listener.

- [ ] **Step 7: Run GREEN**

Run:

```bash
scripts/test.sh SANITIZE= BUILD_DIR=build/runtime-idle-ipc --suites daemon_ipc
```

Expected GREEN: new tests plus local-frame roundtrip, wait-forever connection interrupt, delayed listener, saturation, startup/lifetime, security, and platform-specific tests pass.

- [ ] **Step 8: Review platform hazards and release IPC ownership**

Run:

```bash
git diff --check -- src/daemon/ipc.h src/daemon/ipc_internal.h src/daemon/ipc.c tests/test_daemon_ipc.c
rg -n "WaitForSingleObject\(listener->connect_event|poll_until\(listener->fd" src/daemon/ipc.c
```

Expected `rg`: no accept-path match; unrelated IPC waits may remain. Record:

- POSIX wake descriptors are nonblocking and close-on-exec; full pipe means successful coalesced wake.
- POSIX checks wake before socket when both are readable.
- Windows connect completion and interrupt use distinct events.
- Windows `SetEvent`/`ResetEvent` share `wake_mutex`.
- Interrupt leaves pending `ConnectNamedPipe` armed; close cancels it after accept ownership ends.
- Listener memory is never freed while accept can touch it.

---

### Task 4: Runtime Infinite Accept and State Conditions

**Files:**
- Modify: `src/daemon/runtime.c`
- Modify: `src/daemon/runtime.h`
- Test: `tests/test_daemon_runtime.c`

**Interfaces:**
- Consumes: Task 1 condition API; Task 3 `cbm_daemon_ipc_listener_interrupt()`; existing public wait signatures:

```c
bool cbm_daemon_runtime_service_wait_for_clients(cbm_daemon_runtime_service_t *service,
                                                  size_t expected, uint32_t timeout_ms);
bool cbm_daemon_runtime_service_wait_for_connections(cbm_daemon_runtime_service_t *service,
                                                      size_t expected, uint32_t timeout_ms);
bool cbm_daemon_runtime_service_wait_exited(cbm_daemon_runtime_service_t *service,
                                             uint32_t timeout_ms);
```

- Produces test seams in `runtime.h`:

```c
#if defined(CBM_ENABLE_TEST_SEAMS) && CBM_ENABLE_TEST_SEAMS
void cbm_daemon_runtime_service_spurious_wake_for_testing(
    cbm_daemon_runtime_service_t *service);
uint64_t cbm_daemon_runtime_service_condition_waits_for_testing(
    cbm_daemon_runtime_service_t *service);
#endif
```

- Internal fields: `cbm_thread_condition_t state_changed`, `bool state_changed_initialized`, mutex-protected `bool accept_thread_done`, and a test-only mutex-protected wait-entry counter.

- [ ] **Step 1: Add RED predicate-loop and idle-accept tests**

Add `#include "daemon/ipc_internal.h"` to `tests/test_daemon_runtime.c` for the Task 3 observer seam, then add this waiter:

```c
typedef enum {
    RUNTIME_STATE_WAIT_CLIENTS,
    RUNTIME_STATE_WAIT_CONNECTIONS,
    RUNTIME_STATE_WAIT_EXITED,
} runtime_state_wait_kind_t;

typedef struct {
    cbm_daemon_runtime_service_t *service;
    runtime_state_wait_kind_t kind;
    size_t expected;
    uint32_t timeout_ms;
    atomic_bool returned;
    bool result;
} runtime_state_wait_t;

static void *runtime_state_waiter(void *opaque) {
    runtime_state_wait_t *wait = opaque;
    if (wait->kind == RUNTIME_STATE_WAIT_CLIENTS) {
        wait->result = cbm_daemon_runtime_service_wait_for_clients(
            wait->service, wait->expected, wait->timeout_ms);
    } else if (wait->kind == RUNTIME_STATE_WAIT_CONNECTIONS) {
        wait->result = cbm_daemon_runtime_service_wait_for_connections(
            wait->service, wait->expected, wait->timeout_ms);
    } else {
        wait->result = cbm_daemon_runtime_service_wait_exited(wait->service, wait->timeout_ms);
    }
    atomic_store_explicit(&wait->returned, true, memory_order_release);
    return NULL;
}
```

Add `daemon_runtime_idle_accept_has_no_timeout_wakes`: reset Task 3 observer, start idle service, wait until accept wait count is `1`, sleep 100 ms, assert wait count remains `1` and timeout count `0`, then stop/free within existing bounds.

```c
TEST(daemon_runtime_idle_accept_has_no_timeout_wakes) {
    cbm_daemon_build_identity_t identity =
        runtime_test_identity("2.4.0", runtime_test_self_build());
    cbm_daemon_ipc_accept_observer_reset_for_testing();
    runtime_test_fixture_t fixture;
    bool started = runtime_test_fixture_start(&fixture, "idle-accept", &identity);
    uint64_t deadline = cbm_now_ms() + RUNTIME_TEST_TIMEOUT_MS;
    while (started && cbm_daemon_ipc_accept_waits_for_testing() < 1 &&
           cbm_now_ms() < deadline) {
        cbm_usleep(1000);
    }
    uint64_t waits_before = cbm_daemon_ipc_accept_waits_for_testing();
    cbm_usleep(100U * 1000U);
    uint64_t waits_after = cbm_daemon_ipc_accept_waits_for_testing();
    uint64_t timeouts = cbm_daemon_ipc_accept_timeouts_for_testing();
    runtime_test_fixture_finish(&fixture);

    ASSERT_TRUE(started);
    ASSERT_EQ(waits_before, 1);
    ASSERT_EQ(waits_after, waits_before);
    ASSERT_EQ(timeouts, 0);
    PASS();
}
```

Add `daemon_runtime_state_waiters_recheck_after_spurious_broadcast`: start client-count waiter for `1`; wait until its condition-wait counter is at least `1`; call spurious-wake seam; wait until counter increments; assert waiter has not returned; connect one authenticated client; assert waiter returns true. Repeat spurious-wake assertion for an exited waiter, then stop service and assert true. Clean up with existing fixture helpers before assertions.

Use the existing `runtime_test_identity()`, `runtime_test_fixture_start()`, and `runtime_test_fixture_finish()` helpers. The predicate test body is:

```c
TEST(daemon_runtime_state_waiters_recheck_after_spurious_broadcast) {
    cbm_daemon_build_identity_t identity =
        runtime_test_identity("2.4.0", runtime_test_self_build());
    runtime_test_fixture_t fixture;
    bool started = runtime_test_fixture_start(&fixture, "state-condition", &identity);
    runtime_state_wait_t clients = {
        .service = fixture.service,
        .kind = RUNTIME_STATE_WAIT_CLIENTS,
        .expected = 1,
        .timeout_ms = RUNTIME_TEST_TIMEOUT_MS,
    };
    atomic_init(&clients.returned, false);
    cbm_thread_t clients_thread;
    bool clients_started =
        started && cbm_thread_create(&clients_thread, 0, runtime_state_waiter, &clients) == 0;
    uint64_t deadline = cbm_now_ms() + RUNTIME_TEST_TIMEOUT_MS;
    while (clients_started &&
           cbm_daemon_runtime_service_condition_waits_for_testing(fixture.service) < 1 &&
           cbm_now_ms() < deadline) {
        cbm_usleep(1000);
    }
    uint64_t before_clients_wake =
        cbm_daemon_runtime_service_condition_waits_for_testing(fixture.service);
    if (clients_started) cbm_daemon_runtime_service_spurious_wake_for_testing(fixture.service);
    while (clients_started &&
           cbm_daemon_runtime_service_condition_waits_for_testing(fixture.service) <=
               before_clients_wake &&
           cbm_now_ms() < deadline) {
        cbm_usleep(1000);
    }
    bool clients_still_waiting =
        !atomic_load_explicit(&clients.returned, memory_order_acquire);
    cbm_daemon_runtime_connect_result_t connect_result = {0};
    cbm_daemon_runtime_client_t *client =
        clients_still_waiting
            ? cbm_daemon_runtime_client_connect(fixture.endpoint, &identity,
                                                RUNTIME_TEST_TIMEOUT_MS, &connect_result)
            : NULL;
    bool client_connected = client != NULL;
    bool clients_joined = clients_started && cbm_thread_join(&clients_thread) == 0;

    runtime_state_wait_t exited = {
        .service = fixture.service,
        .kind = RUNTIME_STATE_WAIT_EXITED,
        .timeout_ms = RUNTIME_TEST_TIMEOUT_MS,
    };
    atomic_init(&exited.returned, false);
    cbm_thread_t exited_thread;
    bool exited_started =
        client && cbm_thread_create(&exited_thread, 0, runtime_state_waiter, &exited) == 0;
    deadline = cbm_now_ms() + RUNTIME_TEST_TIMEOUT_MS;
    uint64_t before_exited_wait =
        cbm_daemon_runtime_service_condition_waits_for_testing(fixture.service);
    while (exited_started &&
           cbm_daemon_runtime_service_condition_waits_for_testing(fixture.service) <=
               before_exited_wait &&
           cbm_now_ms() < deadline) {
        cbm_usleep(1000);
    }
    uint64_t before_exited_wake =
        cbm_daemon_runtime_service_condition_waits_for_testing(fixture.service);
    if (exited_started) cbm_daemon_runtime_service_spurious_wake_for_testing(fixture.service);
    while (exited_started &&
           cbm_daemon_runtime_service_condition_waits_for_testing(fixture.service) <=
               before_exited_wake &&
           cbm_now_ms() < deadline) {
        cbm_usleep(1000);
    }
    bool exited_still_waiting =
        !atomic_load_explicit(&exited.returned, memory_order_acquire);
    bool client_closed = client &&
                         cbm_daemon_runtime_client_close(client, RUNTIME_TEST_TIMEOUT_MS);
    client = NULL;
    bool exited_joined = exited_started && cbm_thread_join(&exited_thread) == 0;
    runtime_test_fixture_finish(&fixture);

    ASSERT_TRUE(started);
    ASSERT_TRUE(clients_started);
    ASSERT_TRUE(before_clients_wake >= 1);
    ASSERT_TRUE(clients_still_waiting);
    ASSERT_TRUE(client_connected);
    ASSERT_TRUE(clients_joined);
    ASSERT_TRUE(clients.result);
    ASSERT_EQ(connect_result.status, CBM_DAEMON_RUNTIME_CONNECT_ACCEPTED);
    ASSERT_TRUE(exited_started);
    ASSERT_TRUE(exited_still_waiting);
    ASSERT_TRUE(client_closed);
    ASSERT_TRUE(exited_joined);
    ASSERT_TRUE(exited.result);
    PASS();
}
```

- [ ] **Step 2: Run test to verify RED**

Run:

```bash
scripts/test.sh SANITIZE= BUILD_DIR=build/runtime-idle-red-runtime --suites daemon_runtime
```

Expected RED: missing test-seam symbols and idle accept observer reports repeated finite accept timeouts from `RUNTIME_ACCEPT_POLL_MS`.

- [ ] **Step 3: Add and initialize the service condition**

After `cbm_mutex_init(&service->mutex)`, initialize `state_changed`; failure routes through `runtime_service_destroy_unstarted()`. Convert `accept_thread_done` from atomic to mutex-protected bool. Destroy the condition in unstarted teardown and successful `service_free()` after all wait-owning threads end, before destroying the service mutex.

Add:

```c
static void runtime_service_changed_locked(cbm_daemon_runtime_service_t *service) {
    cbm_condition_broadcast(&service->state_changed);
}

static void runtime_service_interrupt_accept_locked(cbm_daemon_runtime_service_t *service) {
    if (service->listener) {
        (void)cbm_daemon_ipc_listener_interrupt(service->listener);
    }
}
```

`runtime_service_changed_locked()` wakes condition waiters only. `runtime_service_interrupt_accept_locked()` is the nonblocking IPC wake and owns a separate listener mutex. No path may hold listener wake mutex while acquiring `service->mutex`.

- [ ] **Step 4: Broadcast every count/state transition**

Call `runtime_service_changed_locked(service)` under `service->mutex` after:

- `state`: STARTING → RUNNING; RUNNING → STOPPING; shortened deadline/emergency escalation; STOPPING → EXITED; accept-thread creation failure → EXITED.
- `active_connections`: successful worker reservation increment; thread-create rollback decrement; `runtime_worker_finish()` decrement.
- `committed_clients`: admission commit increment; disconnect decrement.
- `accept_thread_done = true` at end of `runtime_accept_loop()`.

Also call `runtime_service_interrupt_accept_locked(service)` only for (a) a RUNNING → STOPPING transition or deadline/emergency escalation and (b) worker completion after `done = true` and `active_connections--`. Do not interrupt accept for STARTING → RUNNING, connection admission increments, committed-client increments, or `accept_thread_done`; those transitions need condition broadcasts but a self-wake would make the first idle accept return immediately. One completion wake reaps all completed workers.

- [ ] **Step 5: Replace 20 ms accept timeout with infinite accept**

Delete `RUNTIME_ACCEPT_POLL_MS`. In RUNNING call:

```c
int accepted = cbm_daemon_ipc_accept(service->listener, CBM_DAEMON_IPC_WAIT_FOREVER,
                                     &connection);
```

Handle `1` plus connection with `runtime_accept_connection()`. Handle `0` as explicit interrupt: restart loop, reap, re-check state. On `< 0`, preserve existing bounded 1 ms hostile-peer/listener-error backoff and retry only while RUNNING. An interrupt is not a timeout metric; accept thread never closes listener.

- [ ] **Step 6: Replace STOPPING polling with a condition predicate**

Split `runtime_service_can_exit()` into a `_locked` form. In STOPPING, interrupt active connections outside service mutex, then:

```c
cbm_mutex_lock(&service->mutex);
while (service->state == CBM_DAEMON_RUNTIME_SERVICE_STOPPING &&
       !runtime_service_can_exit_locked(service, cbm_now_ms())) {
    uint32_t remaining_ms = runtime_deadline_remaining_ms(service->stop_deadline_ms);
    if (remaining_ms == 0) {
        (void)runtime_service_can_exit_locked(service, cbm_now_ms());
        break;
    }
    cbm_condition_wait_result_t waited =
        cbm_condition_timed_wait(&service->state_changed, &service->mutex, remaining_ms);
    if (waited == CBM_CONDITION_ERROR) {
        service->emergency_stop = true;
        runtime_service_changed_locked(service);
    }
}
cbm_mutex_unlock(&service->mutex);
```

Keep coordinator-drained, activation-empty, deadline, and emergency predicates. When `_locked` sets EXITED, broadcast. Timeout means re-evaluate predicate, not successful wake.

- [ ] **Step 7: Replace public count and exited polling**

Add `runtime_deadline_remaining_ms()` that returns `0` after monotonic deadline and caps positive values at `UINT32_MAX - 1`. Implement `runtime_wait_for_count()` under one mutex acquisition:

```c
cbm_mutex_lock(&service->mutex);
while ((connections ? service->active_connections : service->committed_clients) != expected) {
    uint32_t remaining_ms = runtime_deadline_remaining_ms(deadline);
    if (remaining_ms == 0) {
        cbm_mutex_unlock(&service->mutex);
        return false;
    }
#if defined(CBM_ENABLE_TEST_SEAMS) && CBM_ENABLE_TEST_SEAMS
    service->condition_waits_for_testing++;
#endif
    if (cbm_condition_timed_wait(&service->state_changed, &service->mutex, remaining_ms) ==
        CBM_CONDITION_ERROR) {
        cbm_mutex_unlock(&service->mutex);
        return false;
    }
}
cbm_mutex_unlock(&service->mutex);
return true;
```

Implement `wait_exited()` with predicate `state == EXITED && accept_thread_done` in the same pattern. Preserve timeout `0` as immediate observation.

- [ ] **Step 8: Implement deterministic spurious-wake seams**

Under test guard, spurious-wake seam returns immediately for null, otherwise locks `service->mutex`, broadcasts without changing predicate, and unlocks. Wait-count getter returns `0` for null and otherwise reads the counter under the same mutex. These seams do not call listener interrupt; they test condition predicates only.

- [ ] **Step 9: Preserve activation and shutdown ordering**

Inspect and retain:

- Activation response snapshot/acknowledgement before other client interruption.
- `activation_response_inflight` prevents non-owner interruption of acknowledgement.
- Final response clears `final_response_inflight` before worker decrements `active_connections` and broadcasts.
- Emergency stop interrupts connections and reaches EXITED within caller's bound.
- `service_free()` joins accept thread before closing listener.

- [ ] **Step 10: Run GREEN for runtime and IPC**

Run:

```bash
scripts/test.sh SANITIZE= BUILD_DIR=build/runtime-idle-runtime --suites "daemon_runtime daemon_ipc"
```

Expected GREEN: idle-accept and spurious-wake tests plus final-disconnect, idle-connection, connection-cap, activation-ack, exact cancellation, close-intent, noncooperative callback, and permanent-service tests pass.

- [ ] **Step 11: Prove only exceptional polling remains**

Run:

```bash
rg -n "RUNTIME_ACCEPT_POLL_MS|RUNTIME_WAIT_POLL_NS|runtime_wait_tick|cbm_usleep\(1000\)" src/daemon/runtime.c
```

Expected: no `RUNTIME_ACCEPT_POLL_MS`; `runtime_wait_tick`/`RUNTIME_WAIT_POLL_NS` only in accepted `< 0` retry. Startup-lock/participant cleanup loops remain. Public count/exited waits do not call `runtime_wait_tick`.

- [ ] **Step 12: Inspect the runtime-only diff**

Run:

```bash
git diff --check -- src/daemon/runtime.c src/daemon/runtime.h tests/test_daemon_runtime.c
git diff -- src/daemon/runtime.c src/daemon/runtime.h tests/test_daemon_runtime.c
```

Review every write to `state`, `active_connections`, `committed_clients`, and `accept_thread_done`: it is under service mutex and broadcasts. Confirm worker completion interrupts listener so reaping does not depend on a future client.

---

### Task 5: Focused, Race, Full-Suite, Platform, and Performance Verification

**Files:**
- No source edits.
- Verification artifacts stay under task-specific build/result directories and are not committed.

**Interfaces:**
- Consumes: completed Tasks 1–4.
- Produces: one verification report containing command, exit code, failing test name when nonzero, platform, compiler, sanitizer, and elapsed time for every gate.

- [ ] **Step 1: Run changed suites with ASan/UBSan**

Run:

```bash
scripts/test.sh BUILD_DIR=build/runtime-idle-focused --suites "daemon_frontend daemon_ipc daemon_runtime"
```

Expected: exit `0`, no sanitizer report, no hang, all new and retained concurrency contracts green.

- [ ] **Step 2: Run ThreadSanitizer**

Run:

```bash
scripts/test.sh --tsan BUILD_DIR=build/runtime-idle-tsan
```

Expected: exit `0` with `TSAN_OPTIONS=halt_on_error=1:report_thread_leaks=0`; changed suites remain in `TEST_TSAN_SUITES`. Any race, lock-order warning, invalid mutex use, or condition use-after-destroy blocks completion.

- [ ] **Step 3: Run macOS arm64 LeakSanitizer**

Run on macOS arm64 with Homebrew LLVM:

```bash
make -f Makefile.cbm test-lsan LSAN_SUITES="daemon_frontend daemon_ipc daemon_runtime"
```

Expected: exit `0`, no leaked condition, listener event/pipe, or thread resource. Linux gets LSan through normal ASan; Windows relies on lifecycle/soak handle and RSS guards.

- [ ] **Step 4: Run complete canonical test gate**

Run:

```bash
scripts/test.sh BUILD_DIR=build/runtime-idle-full
```

Expected: exit `0` for static contracts, clean ASan/UBSan build, every native suite, and product-binary guards. Do not narrow or skip suites for completion.

- [ ] **Step 5: Run native Windows tests and lifecycle guards**

On Windows amd64 and arm64:

```powershell
pwsh -File scripts/test-windows.ps1
```

Expected: C tests pass; `tests\windows\test_daemon_lifecycle.py` and `tests\windows\test_daemon_stability.py` are GREEN; no named-pipe/event handle leak. Inspect the infinite accept test on both native architectures; Wine is insufficient evidence for manual-reset event behavior.

- [ ] **Step 6: Run production build and quick soak on isolated state**

Run:

```bash
scripts/build.sh CC=clang CXX=clang++ BUILD_DIR=build/runtime-idle-prod
scripts/soak-legs.sh --legs quick build/runtime-idle-prod/codebase-memory-mcp 10
```

Expected: `=== soak-test: PASSED ===`, idle daemon CPU `<=5%`, no FD/RSS failure, admission churn green. Use isolated build binary, never installed binary or live cache. If merged `soak-legs.sh --help` shows positional order different from this command, follow its help exactly while preserving `--legs quick` and duration `10`.

- [ ] **Step 7: Record performance-specific evidence**

The report contains:

- Frontend idle: worker wait-entry count unchanged across 100 ms hold.
- IPC idle: one infinite kernel wait, zero timeout returns before interrupt.
- Runtime spurious wake: waiter remains blocked until predicate changes.
- Backpressure: all over-capacity frames observed, no response loss.
- EOF timeout: cancellation occurs, clean EOF returns success.
- Blocked-stdio maintenance: cancellation/fail-stop within prior bound.
- Activation: acknowledgement ordering/client snapshot unchanged.
- Soak: numeric idle daemon CPU and `<=5%` verdict.

- [ ] **Step 8: Run final scope and placeholder checks**

Run:

```bash
git diff --check
git status --short
git diff --name-only
rg -n "T[B]D|T[O]DO|implement[[:space:]]+later|FRONTEND_(WAIT_US|IDLE_WAIT_US)|RUNTIME_ACCEPT_POLL_MS" \
  src/foundation/compat_thread.* src/daemon/frontend.* src/daemon/ipc.* \
  src/daemon/runtime.* tests/test_daemon_frontend.c tests/test_daemon_ipc.c \
  tests/test_daemon_runtime.c
```

Expected: no whitespace errors; only the eleven runtime-idle source/test paths plus approved docs changed; no placeholder text; no frontend worker/backpressure poll constants; no runtime accept poll constant. Return unrelated files to their owner without staging or reverting another worker's changes.

## Platform risk checklist

- **Spurious/stolen wakes:** every wait is a mutex-held `while`, including test helpers.
- **Clock behavior:** POSIX finite waits are monotonic; macOS uses relative pthread waits; Windows uses relative milliseconds. No deadline uses wall-clock time.
- **Timeout overflow:** remaining durations cap at `UINT32_MAX - 1`; `UINT32_MAX` is explicit infinite condition wait/IPC accept.
- **POSIX descriptor inheritance:** both wake-pipe ends are `FD_CLOEXEC` and nonblocking; partial construction cannot close stdin/stdout through zero-initialized descriptors.
- **POSIX simultaneous readiness:** interrupt wins; queued socket remains for next loop.
- **Windows lost reset:** `SetEvent`/`ResetEvent` share `wake_mutex`; connect completion and interrupt use distinct manual-reset events.
- **Windows overlapped lifetime:** interrupt does not destroy armed `OVERLAPPED`; close cancels after accept ownership ends.
- **Worker reaping:** completion wakes infinite accept; completed joinable worker cannot occupy a slot until another client.
- **Condition lifetime:** frontend destroys after worker/watchdog/monitor joins; runtime destroys after accept/worker joins.
- **Lock ordering:** frontend never cancels runtime while holding state mutex. Accept releases listener wake mutex before acquiring service mutex.
- **Exceptional loops:** coordination cleanup and accept-error retry remain bounded polling; steady-state frontend, accept, count, and exited paths do not.

## Stop conditions

Stop and return to integration owner if:

- Merge base lacks pinned upstream or `7710f86e`.
- A task needs `host.c`, `application.c`, config/watcher, browse/UI, `Makefile.cbm`, or `scripts/test.sh` edits.
- Windows cannot keep connect completion and interrupt as separate events.
- A supported POSIX target cannot provide specified monotonic/relative timeout without a new dependency.
- `service_free()` would close listener before accept thread joins.
- EOF, activation acknowledgement, cancellation, or shutdown deadline must change for tests.
- TSan finds a race/lock-order defect, or idle soak exceeds `5%` CPU.
