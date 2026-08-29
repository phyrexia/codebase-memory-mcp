# Index Admission and Watcher Controls Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Accept upstream's watcher behavior unchanged, add a persistent and strictly bounded `max_index_jobs` setting, route it through the daemon's existing physical-job admission seam, and prove the many-project workstation profile (`CBM_WORKERS=4`, `watcher_enabled=false`) without reducing graph quality.

**Architecture:** Keep the two concurrency controls separate: `max_index_jobs` limits distinct daemon-owned physical index processes, while `CBM_WORKERS` continues to control parallel workers inside each physical index process. Parse the persisted setting once during daemon host preparation, pass it into `cbm_daemon_application_config_t.physical_job_limit`, and retain the application's existing admission ordering so semantically identical requests coalesce before capacity is checked. The application's fixed aggregate-memory split remains `aggregate_memory_budget_bytes / effective_physical_job_limit`, including the existing clamp that prevents a zero-byte slice.

**Tech Stack:** C11, SQLite-backed CLI configuration, daemon host/application APIs, the repository C test framework, POSIX shell process tests, Makefile.cbm, ASan/UBSan, TSan.

**Spec:** `docs/superpowers/specs/2026-08-28-upstream-performance-integration-design.md`

## Global Constraints

- Execute only after the Git-integration preservation plan has merged pinned upstream `31b611a30e01aa8f306f9fcba32492cad0702f99` and the idle-runtime plan has released `src/daemon/host.c` and `src/daemon/application.c` for exclusive ownership.
- Accept upstream's watcher fixes and master kill switch; do not replace upstream watcher construction, registration, or process-test behavior with a local implementation.
- Preserve `watcher_enabled=true` as the compatibility default. The many-project workstation rollout uses `watcher_enabled=false`, but that is a profile, not a product-default change.
- Add persistent `max_index_jobs` with exact range `1..16` and default `2`.
- Invalid, empty, zero, negative, out-of-range, overflowing, signed, whitespace-padded, hexadecimal, floating-point, or otherwise non-decimal stored values resolve to `2`; they must never increase concurrency.
- Reuse `cbm_daemon_application_config_t.physical_job_limit`; do not add a second admission counter or queue.
- Read `max_index_jobs` once per daemon generation. A persisted change affects the next daemon generation only, so documentation and evidence must include daemon retirement/restart.
- Preserve coalescing: identical project/options requests share one physical job and increment subscribers even when the physical-job limit is full; conflicting options continue to return the existing conflict status; distinct work continues to queue/refuse through the existing paths.
- Preserve the aggregate-memory invariant: every admitted worker receives the same fixed slice, the sum of simultaneously assigned slices never exceeds the aggregate budget, and an aggregate budget smaller than the configured limit clamps effective capacity before division.
- The workstation profile uses `CBM_WORKERS=4`. This does not change index mode, semantic index version `3`, cross-file LSP, stored coverage, or the canonical full-mode quality contract.
- Do not change the default or accepted spellings of `watcher_enabled`; keep it distinct from per-session `auto_watch`.
- Do not mutate the installed binary, live daemon, live cache, or global configuration. Every process test and rollout rehearsal uses an isolated `CBM_CACHE_DIR`.
- **Automatic stage-file or activation-backup garbage collection is explicitly out of scope.** Do not add startup cleanup, age-based deletion, orphan scanning, or any automatic deletion of `*.stage*`/backup artifacts. Operational cleanup remains manual and requires age, active-process, live-worker-ownership, and database-integrity checks.
- Do not edit the user's original checkout or its modified `.gitignore`.

## Planned File Map

- Modify `src/cli/cli.h`: define the persisted key, bounds/default constants, and the strict `cbm_config_max_index_jobs()` reader.
- Modify `src/cli/cli.c`: implement strict bounded parsing and expose `max_index_jobs` through the existing `CONFIG_KEYS` table.
- Modify `src/daemon/application.h`: update the `physical_job_limit` contract comment from default `4` to default `2`; no new admission interface.
- Modify `src/daemon/application.c`: change only the conservative default from `4` to `2`; preserve admission, coalescing, and memory-split algorithms.
- Modify `src/daemon/host.c`: read the persisted value during host preparation and pass it through `.physical_job_limit`.
- Modify `tests/test_cli.c`: cover discoverability, persistence, all bounds, and all invalid-value families.
- Modify `tests/test_daemon_application.c`: pin the new default, upper-bound configuration, small-budget clamp, equal memory slices, admission, and coalescing.
- Modify `tests/test_watcher_disabled.sh`: retain upstream kill-switch coverage and add an isolated real-binary rollout-profile assertion for `watcher_enabled=false`, `max_index_jobs=2`, and `CBM_WORKERS=4`.
- Modify `README.md`: add the user-facing setting and separate physical jobs from per-job workers.
- Modify `docs/CONFIGURATION.md`: document defaults, bounds, fallback behavior, daemon-generation capture, memory split, the workstation profile, restart steps, and the no-stage-GC warning.
- Create `docs/superpowers/evidence/2026-08-28-index-control-workstation.md`: record reproducible isolated-cache evidence, the exact candidate SHA, commands, pass output, and scope limitations.
- Do not modify pipeline mode selection, cross-file LSP, semantic-index schema/version, stage-file naming/deletion, activation backup handling, or live workstation configuration.

---

### Task 0: Prove the serialized starting point and accept upstream watcher behavior

**Files:**
- Read: `docs/superpowers/specs/2026-08-28-upstream-performance-integration-design.md`
- Read: `src/cli/cli.c`
- Read: `src/daemon/host.c`
- Read: `tests/test_cli.c`
- Read: `tests/test_watcher_disabled.sh`
- Test: `tests/test_cli.c`
- Test: `tests/test_watcher.c`
- Test: `tests/test_watcher_disabled.sh`

**Interfaces:**
- Consumes: merged upstream `watcher_enabled` implementation and the integration plan's green preservation branch.
- Produces: a verified baseline showing that upstream's default-on master switch, disabled no-thread/no-registration behavior, enabled positive control, `auto_index` independence, and manual `index_repository` availability are already green before index-limit edits.

- [ ] **Step 1: Confirm both pinned upstream inputs are ancestors and the ownership boundary is clear**

Run:

```bash
git merge-base --is-ancestor 31b611a30e01aa8f306f9fcba32492cad0702f99 HEAD
git merge-base --is-ancestor 7710f86e927c5221c395ecbdab9d0f3b75edcb71 HEAD
git status --short
```

Expected: both ancestry commands exit `0`; no other worker is editing `src/cli/cli.c`, `src/daemon/host.c`, or `src/daemon/application.c`. Stop this plan if either pinned commit is absent or file ownership overlaps.

- [ ] **Step 2: Inspect the accepted upstream watcher contract without editing it**

Confirm all of these remain present:

```bash
rg -n "cbm_config_watcher_enabled|CBM_CONFIG_WATCHER_ENABLED|watcher_enabled" src/cli/cli.c src/cli/cli.h tests/test_cli.c
rg -n "watcher\.disabled|watcher_enabled|host_state_prepare" src/daemon/host.c tests/test_watcher_disabled.sh
```

Expected: `cbm_config_watcher_enabled(NULL)` defaults true; `CONFIG_KEYS` reports default `true`; host preparation skips watcher construction only when false; the process test has disabled/manual, disabled/auto-index, and enabled positive-control runs.

- [ ] **Step 3: Run the pre-change watcher characterization suites**

Run:

```bash
make -f Makefile.cbm test-focused TEST_SUITES="cli watcher"
make -f Makefile.cbm cbm TEST_SEAMS=1
CBM_TEST_BINARY="$PWD/build/c/codebase-memory-mcp" bash tests/test_watcher_disabled.sh
```

Expected: all commands pass before any production edit. If they fail, return the failure to the integration owner; do not reinterpret it as a `max_index_jobs` RED.

---

### Task 1: Add a strict persistent `max_index_jobs` reader

**Files:**
- Modify: `src/cli/cli.h`
- Modify: `src/cli/cli.c`
- Test: `tests/test_cli.c`

**Interfaces:**
- Consumes: `cbm_config_get(cbm_config_t *, const char *, const char *)` and the existing SQLite-backed `config` table.
- Produces: `size_t cbm_config_max_index_jobs(cbm_config_t *cfg)`; `CBM_CONFIG_MAX_INDEX_JOBS`; public constants `CBM_CONFIG_MAX_INDEX_JOBS_DEFAULT`, `CBM_CONFIG_MAX_INDEX_JOBS_MIN`, and `CBM_CONFIG_MAX_INDEX_JOBS_MAX` with values `2`, `1`, and `16` respectively.

- [ ] **Step 1: Write the failing strict-parser and persistence test**

Add this test to `tests/test_cli.c` and register it in `suite_cli()`:

```c
TEST(cli_config_max_index_jobs_is_discoverable_strict_bounded_and_persistent) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-max-index-jobs-XXXXXX");
    ASSERT_NOT_NULL(cbm_mkdtemp(tmpdir));

    ASSERT_EQ(cbm_config_max_index_jobs(NULL), 2);
    cbm_config_t *cfg = cbm_config_open(tmpdir);
    ASSERT_NOT_NULL(cfg);
    ASSERT_EQ(cbm_config_max_index_jobs(cfg), 2);

    bool listed = false;
    for (size_t i = 0; i < cbm_cli_config_key_count_for_testing(); i++) {
        const char *key = cbm_cli_config_key_at_for_testing(i);
        listed = listed || (key && strcmp(key, CBM_CONFIG_MAX_INDEX_JOBS) == 0);
    }
    ASSERT_TRUE(listed);

    const struct {
        const char *stored;
        size_t expected;
    } valid[] = {{"1", 1}, {"2", 2}, {"02", 2}, {"7", 7}, {"16", 16}};
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++) {
        ASSERT_EQ(cbm_config_set(cfg, CBM_CONFIG_MAX_INDEX_JOBS, valid[i].stored), 0);
        ASSERT_EQ(cbm_config_max_index_jobs(cfg), valid[i].expected);
    }

    const char *invalid[] = {
        "", "0", "-1", "+2", "17", " 2", "2 ", "1.5", "0x10", "2x", "abc",
        "999999999999999999999999999999999999999999999999999999999999999999"
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        ASSERT_EQ(cbm_config_set(cfg, CBM_CONFIG_MAX_INDEX_JOBS, invalid[i]), 0);
        ASSERT_EQ(cbm_config_max_index_jobs(cfg), 2);
    }

    ASSERT_EQ(cbm_config_set(cfg, CBM_CONFIG_MAX_INDEX_JOBS, "7"), 0);
    cbm_config_close(cfg);
    cfg = cbm_config_open(tmpdir);
    ASSERT_NOT_NULL(cfg);
    ASSERT_EQ(cbm_config_max_index_jobs(cfg), 7);

    cbm_config_close(cfg);
    test_rmdir_r(tmpdir);
    PASS();
}
```

- [ ] **Step 2: Run the CLI suite to observe RED**

Run:

```bash
make -f Makefile.cbm test-focused TEST_SUITES="cli"
```

Expected: compile fails because `CBM_CONFIG_MAX_INDEX_JOBS` and `cbm_config_max_index_jobs()` do not exist. This is the intended RED.

- [ ] **Step 3: Declare the exact key, bounds, default, and reader**

Add to the well-known config section in `src/cli/cli.h`:

```c
#define CBM_CONFIG_MAX_INDEX_JOBS "max_index_jobs"
#define CBM_CONFIG_MAX_INDEX_JOBS_DEFAULT 2U
#define CBM_CONFIG_MAX_INDEX_JOBS_MIN 1U
#define CBM_CONFIG_MAX_INDEX_JOBS_MAX 16U

/* Strict persisted daemon physical-index concurrency. Only ASCII decimal
 * digits whose value is in 1..16 are accepted. Missing or malformed values
 * return 2. Read by the daemon host once per generation. */
size_t cbm_config_max_index_jobs(cbm_config_t *cfg);
```

- [ ] **Step 4: Implement overflow-safe ASCII-decimal parsing without reusing the permissive integer reader**

Add beside `cbm_config_watcher_enabled()` in `src/cli/cli.c`:

```c
size_t cbm_config_max_index_jobs(cbm_config_t *cfg) {
    const char *value = cbm_config_get(cfg, CBM_CONFIG_MAX_INDEX_JOBS, NULL);
    if (!value || !value[0]) {
        return CBM_CONFIG_MAX_INDEX_JOBS_DEFAULT;
    }
    size_t parsed = 0;
    for (const unsigned char *cursor = (const unsigned char *)value; *cursor; cursor++) {
        if (*cursor < (unsigned char)'0' || *cursor > (unsigned char)'9') {
            return CBM_CONFIG_MAX_INDEX_JOBS_DEFAULT;
        }
        size_t digit = (size_t)(*cursor - (unsigned char)'0');
        if (parsed > (CBM_CONFIG_MAX_INDEX_JOBS_MAX - digit) / 10U) {
            return CBM_CONFIG_MAX_INDEX_JOBS_DEFAULT;
        }
        parsed = parsed * 10U + digit;
    }
    return parsed >= CBM_CONFIG_MAX_INDEX_JOBS_MIN &&
                   parsed <= CBM_CONFIG_MAX_INDEX_JOBS_MAX
               ? parsed
               : CBM_CONFIG_MAX_INDEX_JOBS_DEFAULT;
}
```

Do not use `cbm_config_get_int()` here: its `strtol`-then-cast contract is intentionally broader and does not provide the strict overflow/non-decimal guarantee this setting requires.

- [ ] **Step 5: Make the key discoverable through every existing config subcommand**

Add exactly one row to `CONFIG_KEYS` in `src/cli/cli.c`:

```c
{CBM_CONFIG_MAX_INDEX_JOBS, "2",
 "Maximum concurrent physical index jobs (1-16; invalid values use 2)"},
```

Because `config help`, `list`, `get`, `set`, `reset`, and key validation all consume `CONFIG_KEYS`, do not add parallel command-specific branches.

- [ ] **Step 6: Run the CLI suite to observe GREEN**

Run:

```bash
make -f Makefile.cbm test-focused TEST_SUITES="cli"
```

Expected: the new test passes, including absent/NULL default, valid endpoints, persistence, and every invalid-value family.

- [ ] **Step 7: Commit the independently reviewable config layer**

```bash
git add src/cli/cli.h src/cli/cli.c tests/test_cli.c
git commit -m "feat(config): add max index jobs setting"
```

---

### Task 2: Change the application default while preserving admission, coalescing, and memory splitting

**Files:**
- Modify: `src/daemon/application.c`
- Modify: `src/daemon/application.h`
- Test: `tests/test_daemon_application.c`

**Interfaces:**
- Consumes: the existing `cbm_daemon_application_config_t.physical_job_limit` and `aggregate_memory_budget_bytes` fields.
- Produces: default effective physical capacity `2`; unchanged explicit override behavior; unchanged `cbm_daemon_application_physical_job_limit()` and `cbm_daemon_application_worker_memory_budget_bytes()` observability.

- [ ] **Step 1: Rewrite the existing default-capacity test for two jobs and a third distinct refusal**

In `tests/test_daemon_application.c`, rename `daemon_application_default_limit_admits_four_and_rejects_fifth` to `daemon_application_default_limit_admits_two_and_rejects_third` and change its constants to:

```c
enum { DEFAULT_CAP_RUNNING = 2, DEFAULT_CAP_TOTAL = 3 };
const size_t aggregate_budget = 4099;
```

Keep the existing real concurrent admission body and retain these assertions:

```c
ASSERT_TRUE(app_wait_for_active_jobs(application, DEFAULT_CAP_RUNNING));
ASSERT_EQ(third, 1);
ASSERT_EQ(atomic_load(&fake.starts), DEFAULT_CAP_RUNNING);
ASSERT_EQ(fake.memory_budgets[0], aggregate_budget / DEFAULT_CAP_RUNNING);
ASSERT_EQ(fake.memory_budgets[1], aggregate_budget / DEFAULT_CAP_RUNNING);
ASSERT_TRUE(fake.memory_budgets[0] + fake.memory_budgets[1] <= aggregate_budget);
```

Update the `RUN_TEST` registration to the renamed symbol.

- [ ] **Step 2: Add a focused upper-bound and small-budget-clamp test**

Add and register:

```c
TEST(daemon_application_physical_limit_preserves_fixed_memory_split_and_nonzero_slice) {
    cbm_daemon_application_config_t upper = {
        .physical_job_limit = 16,
        .aggregate_memory_budget_bytes = 1600,
    };
    cbm_daemon_application_t *application = cbm_daemon_application_new(&upper);
    ASSERT_NOT_NULL(application);
    ASSERT_EQ(cbm_daemon_application_physical_job_limit(application), 16);
    ASSERT_EQ(cbm_daemon_application_worker_memory_budget_bytes(application), 100);
    ASSERT_TRUE(cbm_daemon_application_shutdown(application, APP_TEST_TIMEOUT_MS));
    cbm_daemon_application_free(application);

    cbm_daemon_application_config_t tiny = {
        .physical_job_limit = 16,
        .aggregate_memory_budget_bytes = 3,
    };
    application = cbm_daemon_application_new(&tiny);
    ASSERT_NOT_NULL(application);
    ASSERT_EQ(cbm_daemon_application_physical_job_limit(application), 3);
    ASSERT_EQ(cbm_daemon_application_worker_memory_budget_bytes(application), 1);
    ASSERT_TRUE(cbm_daemon_application_shutdown(application, APP_TEST_TIMEOUT_MS));
    cbm_daemon_application_free(application);
    PASS();
}
```

This pins both memory invariants without starting sixteen threads: equal fixed slices at the supported upper bound and effective-capacity reduction before division when the aggregate budget is smaller than the requested concurrency.

- [ ] **Step 3: Run the daemon-application suite to observe RED**

Run:

```bash
make -f Makefile.cbm test-focused TEST_SUITES="daemon_application"
```

Expected: `daemon_application_default_limit_admits_two_and_rejects_third` fails because the old application default admits four jobs. The explicit `16` and tiny-budget cases should already pass, demonstrating reuse of the existing seam.

- [ ] **Step 4: Make the minimal production change**

In `src/daemon/application.c`, change only:

```c
APPLICATION_DEFAULT_PHYSICAL_JOB_LIMIT = 2,
```

In `src/daemon/application.h`, update the field comment to say zero selects the conservative daemon default `(2)`.

Do not move the capacity check ahead of `application_find_active_job_locked()`. The required order remains:

```text
find same-project active job -> validate normalized options -> add subscriber
otherwise check active distinct jobs against physical_job_limit -> create physical job
```

- [ ] **Step 5: Run GREEN plus the named coalescing/queue invariants**

Run:

```bash
make -f Makefile.cbm test-focused TEST_SUITES="daemon_application"
```

Expected: the full suite passes, specifically including:

- `daemon_application_default_limit_admits_two_and_rejects_third`;
- `daemon_application_physical_limit_preserves_fixed_memory_split_and_nonzero_slice`;
- `daemon_application_coalesces_semantically_identical_index_requests` (two subscribers, one worker, limit `1`);
- `daemon_application_request_cancel_detaches_only_one_coalesced_subscriber`;
- `daemon_application_queues_explicit_index_behind_physical_job_limit` (session request parks; coordinator API remains non-blocking busy);
- existing options-conflict and stale-watcher admission tests.

- [ ] **Step 6: Commit the independently reviewable admission-default change**

```bash
git add src/daemon/application.c src/daemon/application.h tests/test_daemon_application.c
git commit -m "perf(daemon): default to two physical index jobs"
```

---

### Task 3: Wire the persisted setting into one daemon generation and prove the workstation profile

**Files:**
- Modify: `src/daemon/host.c`
- Modify: `tests/test_watcher_disabled.sh`
- Test: `tests/test_watcher_disabled.sh`

**Interfaces:**
- Consumes: `size_t cbm_config_max_index_jobs(cbm_config_t *cfg)` and `cbm_daemon_application_config_t.physical_job_limit`.
- Produces: each daemon generation snapshots the persisted limit during `host_state_prepare()`; the existing `daemon.start` log exposes the effective physical limit; the real-binary process test proves the disabled watcher, two-job default, four per-job workers, and manual full indexing coexist.

- [ ] **Step 1: Extend the process test fixture so `CBM_WORKERS=4` necessarily takes the parallel full-index path**

In `tests/test_watcher_disabled.sh`, keep the existing `sample.c`, add 64 deterministic C translation units before the fixture commit, and set the test-process worker profile:

```bash
for i in $(seq 0 63); do
  printf 'int worker_%s(void) { return %s; }\n' "${i}" "${i}" >"${repo}/worker_${i}.c"
done
export CBM_WORKERS=4
```

The resulting 65-file fixture is above `MIN_FILES_FOR_PARALLEL` (`50`), so the log assertion below is anti-vacuous.

- [ ] **Step 2: Add a failing non-default host snapshot run, then pin the rollout profile in the disabled/manual run**

Add a short Run 0 using its own isolated cache. A non-default value is required here: after Task 2 the application's internal default is already `2`, so configuring `2` alone would pass even if host wiring were accidentally omitted.

```bash
CURRENT_RUN="run 0: configured physical limit snapshot"
c_limit="${work}/cache-limit"
CBM_CACHE_DIR="${c_limit}" "${BINARY}" config set watcher_enabled false >/dev/null
CBM_CACHE_DIR="${c_limit}" "${BINARY}" config set max_index_jobs 7 >/dev/null
run_session "${c_limit}" "${work}/limit.out"
grep 'msg=daemon\.start' "${DAEMON_LOG}" | grep -q 'physical_job_limit=7' \
  || fail "daemon did not capture configured max_index_jobs=7"
```

Before Run 1, persist the intended physical limit in its isolated cache:

```bash
CBM_CACHE_DIR="${c_offm}" "${BINARY}" config set max_index_jobs 2 >/dev/null
```

Change Run 1's manual request from `"mode":"fast"` to `"mode":"full"`. After the lifecycle closes, assert all dimensions on completed logs:

```bash
grep 'msg=daemon\.start' "${DAEMON_LOG}" | grep -q 'physical_job_limit=2' \
  || fail "rollout profile did not use max_index_jobs=2"
grep 'msg=pipeline\.mode' "${DAEMON_LOG}" | grep 'mode=parallel' | grep -q 'workers=4' \
  || fail "CBM_WORKERS=4 did not drive the parallel full-index fixture"
log_has "${DAEMON_LOG}" 'msg=watcher\.disabled .*reason=config' \
  || fail "watcher_enabled=false was not honored in the rollout profile"
! log_has "${DAEMON_LOG}" 'msg=watcher\.start( |$)' \
  || fail "watcher thread ran in the rollout profile"
! log_has "${DAEMON_LOG}" 'msg=watcher\.watch( |$)' \
  || fail "project registered with the watcher in the rollout profile"
```

Keep the existing response assertion that manual `index_repository` returned an indexed result. Keep Runs 2 and 3 unchanged in meaning so disabled auto-index and enabled watcher positive control remain proven.

- [ ] **Step 3: Run the real-binary process test to observe RED**

Run:

```bash
make -f Makefile.cbm cbm TEST_SEAMS=1
CBM_TEST_BINARY="$PWD/build/c/codebase-memory-mcp" bash tests/test_watcher_disabled.sh
```

Expected: Run 0 fails at `physical_job_limit=7` because host preparation still omits `.physical_job_limit` and the application therefore remains on its internal default `2`. The Run 1 watcher and `CBM_WORKERS=4` assertions should otherwise pass.

- [ ] **Step 4: Snapshot the persisted value during host preparation**

In `host_state_prepare()` in `src/daemon/host.c`, read the value only after `_config.db` opens successfully:

```c
size_t max_index_jobs = cbm_config_max_index_jobs(host->runtime_config);
```

Pass it through the existing application config:

```c
cbm_daemon_application_config_t application_config = {
    .watcher = host->watcher,
    .config = host->runtime_config,
    .physical_job_limit = max_index_jobs,
    .aggregate_memory_budget_bytes = aggregate_memory_budget_bytes,
    .project_locks = host->project_locks,
    .ui_readiness_secret = host->ui_readiness_secret,
    .ui_readiness_secret_length = sizeof(host->ui_readiness_secret),
};
```

Do not read the key again in a session callback, admission loop, or worker. The application already copies the scalar at construction, which is the generation snapshot.

- [ ] **Step 5: Run the real-binary process test to observe GREEN**

Run:

```bash
make -f Makefile.cbm cbm TEST_SEAMS=1
CBM_TEST_BINARY="$PWD/build/c/codebase-memory-mcp" bash tests/test_watcher_disabled.sh
```

Expected: PASS with a non-default configured host snapshot (`7`) plus completed-lifecycle rollout evidence for `watcher.disabled`, absence of `watcher.start`/`watcher.watch`, `physical_job_limit=2`, `pipeline.mode mode=parallel workers=4`, successful manual full indexing, disabled+auto-index independence, and enabled watcher positive control.

- [ ] **Step 6: Prove a changed persisted limit is taken only by the next generation**

Extend Run 0's isolated cache with a restart check after its first lifecycle:

```bash
CBM_CACHE_DIR="${c_limit}" "${BINARY}" config set max_index_jobs 16 >/dev/null
run_session "${c_limit}" "${work}/limit-restarted.out" \
  "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"list_projects\",\"arguments\":{\"limit\":1}}}"
grep 'msg=daemon\.start' "${DAEMON_LOG}" | grep -q 'physical_job_limit=16' \
  || fail "next daemon generation did not capture max_index_jobs=16"
```

The first completed lifecycle must contain `physical_job_limit=7`; only the explicitly restarted lifecycle may contain `16`. Do not attempt live reconfiguration inside the application.

- [ ] **Step 7: Commit host wiring and process evidence together**

```bash
git add src/daemon/host.c tests/test_watcher_disabled.sh
git commit -m "feat(daemon): persist physical index concurrency"
```

---

### Task 4: Document the controls and capture isolated workstation-rollout evidence

**Files:**
- Modify: `README.md`
- Modify: `docs/CONFIGURATION.md`
- Create: `docs/superpowers/evidence/2026-08-28-index-control-workstation.md`

**Interfaces:**
- Consumes: the shipped `config` CLI, daemon restart command, `CBM_WORKERS`, the process test, and the focused native suites.
- Produces: an operator-safe rollout procedure and immutable branch evidence that does not claim or perform a live workstation change.

- [ ] **Step 1: Update the README config examples and concurrency explanation**

Add this command beside the existing runtime settings:

```bash
codebase-memory-mcp config set max_index_jobs 2        # physical index processes (1-16; default 2)
```

Add a short explanation with these exact distinctions:

```text
max_index_jobs caps distinct physical index jobs owned by one daemon generation.
CBM_WORKERS caps parallel workers inside each physical index job.
Both are captured by the daemon generation; retire/restart the daemon after changing either.
Identical requests still coalesce into one physical job.
```

- [ ] **Step 2: Add the complete setting contract to `docs/CONFIGURATION.md`**

Add a CLI-managed-settings row:

```markdown
| `max_index_jobs` | `2` | Maximum concurrent distinct physical index jobs. Strict decimal range `1..16`; missing or invalid values use `2`. Read once when the daemon starts. |
```

Document all of the following explicitly:

- `watcher_enabled` remains default `true`; `false` is the many-project workstation profile.
- `max_index_jobs=2` is the product default and the rollout value.
- `CBM_WORKERS=4` is per physical index process, not a replacement for `max_index_jobs`.
- the daemon snapshots both persisted daemon settings and daemon-owned environment at generation start;
- reconnecting a client is insufficient; use `daemon stop`, then start the next generation with the intended environment;
- fixed memory is divided by the effective physical limit, and a tiny aggregate budget reduces the effective limit before division;
- changing these controls does not select `fast`/`moderate`, does not change semantic index version `3`, and does not disable cross-file LSP or stored coverage;
- manual `index_repository(mode="full")` remains available when the watcher is disabled;
- no automatic stage/backup cleanup is provided; manual cleanup requires checking age, active processes, live worker ownership, and `PRAGMA quick_check(1)` on the relevant database, and backups may be the only crash-recovery artifact.

- [ ] **Step 3: Document an explicit, non-live rollout rehearsal**

Use an isolated cache in the documentation example:

```bash
rollout_cache="$(mktemp -d)"
CBM_CACHE_DIR="$rollout_cache" codebase-memory-mcp config set max_index_jobs 2
CBM_CACHE_DIR="$rollout_cache" codebase-memory-mcp config set watcher_enabled false
CBM_CACHE_DIR="$rollout_cache" codebase-memory-mcp config get max_index_jobs
CBM_CACHE_DIR="$rollout_cache" codebase-memory-mcp config get watcher_enabled
CBM_CACHE_DIR="$rollout_cache" CBM_WORKERS=4 codebase-memory-mcp daemon start
CBM_CACHE_DIR="$rollout_cache" codebase-memory-mcp daemon status
CBM_CACHE_DIR="$rollout_cache" codebase-memory-mcp daemon stop
```

State that equivalent commands against the real cache/global launch configuration are a separate external-effect gate requiring explicit authorization.

- [ ] **Step 4: Run and collect the exact evidence inputs**

Run:

```bash
git rev-parse HEAD
git show -s --format='%H %cI %s' HEAD
make -f Makefile.cbm test-focused TEST_SUITES="cli daemon_application pipeline"
make -f Makefile.cbm cbm TEST_SEAMS=1
CBM_TEST_BINARY="$PWD/build/c/codebase-memory-mcp" bash tests/test_watcher_disabled.sh
shasum -a 256 build/c/codebase-memory-mcp
```

Expected: all test commands pass; the process-test output contains its explicit disabled/manual, disabled/auto-index, enabled-control, and final PASS lines; the binary hash command emits one SHA-256.

- [ ] **Step 5: Create the evidence record using the actual captured values**

Using `apply_patch`, create `docs/superpowers/evidence/2026-08-28-index-control-workstation.md` with these mandatory sections and no unfilled placeholders:

1. candidate commit (`git rev-parse HEAD`) and binary SHA-256;
2. isolated cache/runtime statement and confirmation that live cache, daemon, binary, and global config were untouched;
3. exact commands from Step 4;
4. verbatim summary lines proving `watcher_enabled=false`, no watcher start/registration, `physical_job_limit=2`, `CBM_WORKERS=4`, successful manual full index, and the enabled watcher positive control;
5. focused suite and TSan results;
6. graph-quality guard: `pipeline` suite passed under `CBM_WORKERS=4`, including its full-mode/cross-file LSP/coverage contracts; semantic index version remained `3`;
7. restart semantics: the next isolated daemon generation captured the changed configured limit;
8. explicit exclusions: no live rollout, install, push, PR, stage cleanup, backup deletion, schema bump, or graph-mode downgrade was performed.

- [ ] **Step 6: Review docs for contradictory defaults or obsolete guidance**

Run:

```bash
rg -n "max_index_jobs|watcher_enabled|CBM_WORKERS|physical index|stage.*cleanup|backup" README.md docs/CONFIGURATION.md docs/superpowers/evidence/2026-08-28-index-control-workstation.md
```

Expected: every default is `max_index_jobs=2` and `watcher_enabled=true`; the rollout profile is clearly labeled `watcher_enabled=false` plus `CBM_WORKERS=4`; no text promises automatic stage deletion.

- [ ] **Step 7: Commit documentation and evidence**

```bash
git add README.md docs/CONFIGURATION.md docs/superpowers/evidence/2026-08-28-index-control-workstation.md
git commit -m "docs: record index control rollout profile"
```

---

### Task 5: Run final focused, sanitizer, quality, and scope gates

**Files:**
- Verify: `src/cli/cli.c`
- Verify: `src/daemon/application.c`
- Verify: `src/daemon/host.c`
- Verify: `tests/test_cli.c`
- Verify: `tests/test_daemon_application.c`
- Verify: `tests/test_watcher_disabled.sh`
- Verify: `README.md`
- Verify: `docs/CONFIGURATION.md`
- Verify: `docs/superpowers/evidence/2026-08-28-index-control-workstation.md`

**Interfaces:**
- Consumes: all prior task commits.
- Produces: final proof that configuration, watcher compatibility, admission/coalescing, memory accounting, full graph quality, and process rollout behavior pass without introducing stage GC.

- [ ] **Step 1: Run all task-relevant native suites under ASan/UBSan**

Run:

```bash
make -f Makefile.cbm test-focused TEST_SUITES="cli daemon_application daemon_runtime watcher pipeline incremental mcp"
```

Expected: all selected suites pass with no sanitizer report.

- [ ] **Step 2: Run the production-binary watcher/index-control process contract**

Run:

```bash
make -f Makefile.cbm cbm TEST_SEAMS=1
CBM_TEST_BINARY="$PWD/build/c/codebase-memory-mcp" bash tests/test_watcher_disabled.sh
```

Expected: PASS; absence assertions are made only after each daemon lifecycle has closed.

- [ ] **Step 3: Run TSan over every changed concurrency surface**

Run:

```bash
make -f Makefile.cbm test-tsan TEST_TSAN_SUITES="daemon_application watcher pipeline"
```

Expected: all selected suites pass with `CBM_WORKERS=4` and no race report.

- [ ] **Step 4: Run formatting/lint and repository diff hygiene**

Run:

```bash
make -f Makefile.cbm lint
git diff --check
git status --short
```

Expected: lint and diff checks pass; status contains only intentional plan/evidence changes not yet committed.

- [ ] **Step 5: Audit the exact behavioral invariants in source**

Run:

```bash
rg -n "CBM_CONFIG_MAX_INDEX_JOBS|cbm_config_max_index_jobs|physical_job_limit" src/cli src/daemon tests/test_cli.c tests/test_daemon_application.c tests/test_watcher_disabled.sh
rg -n "APPLICATION_DEFAULT_PHYSICAL_JOB_LIMIT|application_find_active_job_locked|application_active_job_count_locked|worker_memory_budget_bytes" src/daemon/application.c
rg -n "CBM_SEMANTIC_INDEX_VERSION|pass_lsp_cross|CBM_WORKERS" src tests/test_pipeline.c
```

Review must confirm:

- strict parser is the only persisted-to-limit conversion;
- host reads it exactly once during generation preparation;
- application copies it through `physical_job_limit`;
- coalescing lookup still precedes capacity rejection;
- memory division still uses effective capacity;
- full-mode and cross-file LSP code was not conditionalized on the new setting.

- [ ] **Step 6: Perform the explicit no-stage-GC scope audit**

Inspect the complete index-control change set and confirm it contains no changes to stage or backup deletion surfaces:

```bash
git log --stat --oneline --decorate -4
git show --stat --oneline HEAD~4..HEAD
git diff --check HEAD~4..HEAD
```

The expected implementation file set is limited to the files in this plan's Planned File Map. In particular, there must be no index-control-plan edits to `src/pipeline/pipeline.c`, `src/pipeline/pipeline_incremental.c`, `src/cli/activation_transaction.c`, or their stage/backup cleanup tests. If any such edit appears, remove it from this batch and rerun all gates.

- [ ] **Step 7: Hand off to the whole-branch verifier without performing external effects**

Report:

- RED and GREEN commands/results for Tasks 1-3;
- final native, process, TSan, and lint results;
- candidate commit and binary SHA-256 from the evidence record;
- confirmation that watcher compatibility default remains true;
- confirmation that the isolated rollout profile used `max_index_jobs=2`, `CBM_WORKERS=4`, and `watcher_enabled=false`;
- confirmation that graph mode/version/coverage were unchanged;
- confirmation that automatic stage GC and live rollout were not performed.

Do not install, stop the live daemon, change global configuration, delete stale stages/backups, push, open a PR, or publish a release from this plan.

## Self-Review Checklist

- [ ] Every requirement in the approved spec's “Indexing and watcher control” section maps to a task and a named test or verification gate.
- [ ] `watcher_enabled=true` compatibility is characterized before edits and preserved after them.
- [ ] `max_index_jobs` covers absent, NULL, valid `1`, valid `16`, persistence, empty, zero, negative, above-range, overflow, signs, whitespace, hex, float, suffix, and alphabetic input.
- [ ] Host wiring is process-tested; application default and explicit `16` are unit-tested.
- [ ] Coalescing-before-capacity, queue behavior, cancellation, and options conflicts remain under the existing daemon-application suite.
- [ ] Equal fixed memory slices and the tiny-budget effective-capacity clamp are asserted directly.
- [ ] `CBM_WORKERS=4` takes an anti-vacuous parallel path on a fixture with more than 50 files.
- [ ] Full mode, cross-file LSP, coverage, and semantic index version `3` are preserved by existing quality tests and documented as invariants.
- [ ] Restart/generation capture is proven against an isolated cache.
- [ ] No automatic stage/backup garbage collection appears anywhere in the implementation or documentation.
- [ ] No live binary/cache/config mutation or other external effect is included.
- [ ] The evidence record contains actual values and outputs, not placeholders.
