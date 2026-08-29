# Upstream Custom Integration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Merge the pinned upstream history and pinned idle-frontend fix into the exact installed custom history while preserving request metrics, indexed-root UI behavior, worktree policy, and checkout freshness.

**Architecture:** Build the integration as two reviewed merge commits on an isolated branch rooted at `81b02389598d6215e82939f29b5b2a6917f96910`. Add the missing preservation assertions first, rehearse an upstream-only conflict resolution to observe a RED failure, then resolve the six observed conflicts by taking upstream as the structural base and reapplying only the additive custom contracts. Merge `7710f86e927c5221c395ecbdab9d0f3b75edcb71` separately and require its staged surface to remain exactly three files.

**Tech Stack:** Git merge commits, C11, SQLite, yyjson, the repository C test runner (`scripts/test.sh`), Vitest, TypeScript, and Vite.

**Spec:** `docs/superpowers/specs/2026-08-28-upstream-performance-integration-design.md`

## Global Constraints

- The custom base is exactly `81b02389598d6215e82939f29b5b2a6917f96910` (`0.10.3-53-g81b02389`).
- The upstream merge target is exactly `31b611a30e01aa8f306f9fcba32492cad0702f99`; a changed `origin/main` requires a new audit and stops this plan.
- The idle-frontend merge target is exactly `7710f86e927c5221c395ecbdab9d0f3b75edcb71` with parent `909051ccee2d070f33e15fda71bfe61c9076eea6`.
- The original checkout remains on `feat/request-metrics`; its modified `.gitignore` is never staged, restored, or edited.
- The isolated branch is `integration/main-custom-performance-20260828` at `/private/tmp/cbm-main-custom-performance-20260828`.
- Preserve semantic index version `3`; do not reset or mutate the live cache.
- Preserve the existing `_config.db`; request metrics remain an additive table in that database.
- Preserve `ignore_worktrees=false` and `watcher_enabled=true` as compatibility defaults.
- Preserve `indexed_checkout_sha` as one atomic schema/publication/parser/verdict feature.
- Prefer upstream implementations for pagination, watcher fixes, hermetic tests, runtime relocation, macOS `posix_spawn`, SQLite transient-state handling, cross-LSP scaling, allocator behavior, binary replacement, and security/release fixes.
- Do not restore active `CBM_VARIANT=ui` behavior and do not stage a build-generated change to `graph-ui/tsconfig.tsbuildinfo`.
- Do not install a candidate, change global configuration, stop the live daemon, mutate the live cache, delete stage files, push, open a PR, or release from this plan.
- Only the main integrator stages and commits merge resolutions. Reviewers are read-only.

## File and Interface Map

### Preservation tests added before the merge

- Modify `tests/test_mcp.c`: make the existing issue-704 `list_projects` fixture assert that both default and paginated lean results retain `indexed_at` while omitting expensive details.
- Modify `tests/test_httpd.c`: make the UI RPC allowlist test exercise `get_graph_schema`, and make `/api/indexed-roots` require one `indexed_at` value for each returned primary project.

### Six observed conflict paths

- `README.md`: retain current upstream product counts, packaging, client surfaces, and release/security text; add only custom request-metrics and `ignore_worktrees` documentation that upstream lacks.
- `docs/CONFIGURATION.md`: retain upstream runtime-dir and `watcher_enabled` documentation; add only the custom metrics storage description and `ignore_worktrees` key/behavior.
- `src/cli/cli.c`: retain upstream config routing and CLI implementation; add the metrics DDL/writers and `ignore_worktrees` config registry entry.
- `src/cli/cli.h`: retain upstream declarations; add `cbm_metrics_entry_t`, `cbm_config_metrics_flush`, `cbm_config_metrics_prune`, and `CBM_CONFIG_IGNORE_WORKTREES`.
- `src/main.c`: take upstream's file. Upstream moved `CBM_RUNTIME_DIR` handling into `cbm_daemon_bootstrap_endpoint_new`, so the old call-site implementation must not be restored.
- `tests/test_cli.c`: retain the complete upstream suite and re-add only `cli_config_metrics_flush_and_prune`; keep exactly one upstream `cli_config_watcher_enabled_default_and_persist` definition and registration.

Automatically merged preservation path:

- `tests/test_mcp.c`: audit after the merge. Preserve upstream pagination tests plus all custom freshness/worktree tests and the new `indexed_at` assertions; never replace the automatically merged file wholesale.

### Interfaces that must remain available after resolution

```c
typedef struct {
    const char *bucket_hour;
    const char *source;
    const char *name;
    const char *project;
    int64_t count;
    int64_t errors;
    int64_t dur_us_sum;
    int64_t dur_us_max;
} cbm_metrics_entry_t;

int cbm_config_metrics_flush(cbm_config_t *cfg,
                             const cbm_metrics_entry_t *entries,
                             int n);
int cbm_config_metrics_prune(cbm_config_t *cfg,
                             const char *older_than_bucket);

#define CBM_CONFIG_IGNORE_WORKTREES "ignore_worktrees"
#define CBM_CONFIG_WATCHER_ENABLED "watcher_enabled"
bool cbm_config_watcher_enabled(cbm_config_t *cfg);
```

The upstream UI config interfaces `CBM_CONFIG_UI_ENABLED`, `CBM_CONFIG_UI_PORT`, `cbm_cli_config_key_count_for_testing`, and `cbm_cli_config_key_at_for_testing` must remain unchanged alongside these additions.

---

### Task 1: Verify Pinned Inputs and Create the Isolated Branch

**Files:**

- Inspect only: `/Users/gggp/Code/codebase-memory-mcp/.gitignore`
- Create worktree: `/private/tmp/cbm-main-custom-performance-20260828`
- Create branch: `integration/main-custom-performance-20260828`

**Interfaces:**

- Consumes: the original checkout at `81b02389598d6215e82939f29b5b2a6917f96910` and its `origin` remote.
- Produces: an isolated branch whose initial `HEAD` is exactly the custom base and whose original checkout remains untouched.

- [ ] **Step 1: Confirm the original checkout and record its dirty-file evidence**

Run from `/Users/gggp/Code/codebase-memory-mcp`:

```zsh
git status --short
test "$(git branch --show-current)" = "feat/request-metrics"
test "$(git rev-parse HEAD)" = "59fa85a948e6352986ea9d4b88338e7016f34308"
git status --short -- .gitignore
```

Expected: the original checkout remains on the pre-integration request-metrics tip `59fa85a9`, and `.gitignore` remains visible as the user's modification. The installed custom base `81b02389` is a separate branch/ref used only to seed the isolated worktree. Do not run `git add`, `git restore`, `git checkout`, or any formatter against the original checkout.

- [ ] **Step 2: Refresh the remote reference once, then pin it**

```zsh
git fetch origin main
test "$(git rev-parse refs/remotes/origin/main)" = "31b611a30e01aa8f306f9fcba32492cad0702f99"
test "$(git rev-parse 7710f86e927c5221c395ecbdab9d0f3b75edcb71^{commit})" = "7710f86e927c5221c395ecbdab9d0f3b75edcb71"
test "$(git rev-parse 7710f86e927c5221c395ecbdab9d0f3b75edcb71^)" = "909051ccee2d070f33e15fda71bfe61c9076eea6"
git merge-base --is-ancestor f5285844 81b02389598d6215e82939f29b5b2a6917f96910
git merge-base --is-ancestor 59fa85a9 81b02389598d6215e82939f29b5b2a6917f96910
```

Expected: every command exits `0`. Any mismatch is a hard stop: do not create or reuse the integration branch, and do not fetch again later in the execution.

- [ ] **Step 3: Create the isolated worktree only after the pin checks pass, or verify the already-prepared planning worktree**

```zsh
if test -e /private/tmp/cbm-main-custom-performance-20260828/.git; then
  test "$(git -C /private/tmp/cbm-main-custom-performance-20260828 branch --show-current)" = \
    "integration/main-custom-performance-20260828"
  git -C /private/tmp/cbm-main-custom-performance-20260828 merge-base --is-ancestor \
    81b02389598d6215e82939f29b5b2a6917f96910 HEAD
  test "$(git reflog show --format='%H' integration/main-custom-performance-20260828 | tail -n 1)" = \
    "81b02389598d6215e82939f29b5b2a6917f96910"
else
  test ! -e /private/tmp/cbm-main-custom-performance-20260828
  git show-ref --verify --quiet refs/heads/integration/main-custom-performance-20260828 && exit 1 || true
  git worktree add -b integration/main-custom-performance-20260828 \
    /private/tmp/cbm-main-custom-performance-20260828 \
    81b02389598d6215e82939f29b5b2a6917f96910
fi
```

Expected: Git either creates the branch/worktree from `81b02389`, or proves that the planning worktree already present was created at and still points to that exact base. A mismatched existing path, branch, `HEAD`, or oldest reflog entry is a stop condition; never delete or overwrite it to make this check pass.

- [ ] **Step 4: Verify isolation from both checkouts**

Run in the new worktree:

```zsh
cd /private/tmp/cbm-main-custom-performance-20260828
test "$(git branch --show-current)" = "integration/main-custom-performance-20260828"
git merge-base --is-ancestor 81b02389598d6215e82939f29b5b2a6917f96910 HEAD
test "$(git log --format='%s' --reverse 81b02389598d6215e82939f29b5b2a6917f96910..HEAD | head -n 1)" = \
  "docs: plan upstream performance integration"
test -z "$(git status --porcelain=v1 --untracked-files=no)"
git worktree list --porcelain
```

Expected: the integration checkout is clean, descends from `81b02389`, and contains only the approved planning commit before product work; the original checkout remains listed on `feat/request-metrics`.

Review boundary: a read-only reviewer checks the command transcript and confirms no original-checkout file changed. There is no commit for this task.

---

### Task 2: Add Missing Preservation Characterization Tests

**Files:**

- Modify: `tests/test_mcp.c`, inside `tool_resolve_store_by_internal_name_issue704`
- Modify: `tests/test_httpd.c`, inside `ui_server_rpc_allows_only_ui_read_tools` and `ui_server_indexed_roots_reports_projects_and_orphans`

**Interfaces:**

- Consumes: existing `cbm_mcp_server_handle`, `th_http`, `th_status`, and project-store fixtures.
- Produces: merge-sensitive assertions for `indexed_at`, `get_graph_schema` UI access, and indexed-root timestamps.

- [ ] **Step 1: Extend the MCP characterization at the exact pagination fixture**

In `tests/test_mcp.c`, add these assertions to the existing issue-704 test. Put the first assertion beside the default-list name assertions and the second beside the paginated lean assertions so the automatically merged result can be audited against both custom and upstream contracts:

```c
    ASSERT_NOT_NULL(strstr(list, "\\\"indexed_at\\\":"));
```

```c
    ASSERT_NOT_NULL(strstr(page, "\\\"indexed_at\\\":"));
    ASSERT_NULL(strstr(page, "\\\"nodes\\\""));
    ASSERT_NULL(strstr(page, "\\\"edges\\\""));
    ASSERT_NULL(strstr(page, "\\\"size_bytes\\\""));
```

The existing `ASSERT_NULL(...nodes...)` may be replaced by the three-field lean check above; do not leave duplicate node assertions.

- [ ] **Step 2: Exercise every allowed UI RPC tool, including graph schema**

In `tests/test_httpd.c`, replace the one-off `list_projects` request at the start of `ui_server_rpc_allows_only_ui_read_tools` with this loop; keep the existing blocked-tool, initialize, and ambiguous-name checks after it:

```c
    static const char *allowed_tools[] = {
        "list_projects",
        "get_code_snippet",
        "get_graph_schema",
    };
    char req[1024];
    char resp[8192];
    for (size_t i = 0; i < sizeof(allowed_tools) / sizeof(allowed_tools[0]); i++) {
        char body[512];
        snprintf(body, sizeof(body),
                 "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
                 "\"params\":{\"name\":\"%s\",\"arguments\":{}}}",
                 allowed_tools[i]);
        snprintf(req, sizeof(req),
                 "POST /rpc HTTP/1.1\r\n"
                 "Content-Type: application/json\r\n"
                 "Content-Length: %zu\r\n\r\n%s",
                 strlen(body), body);
        int n = th_http(cbm_http_server_port(ts.srv), req, resp, sizeof(resp));
        ASSERT_GT(n, 0);
        ASSERT_EQ(th_status(resp), 200);
        ASSERT_NOT_NULL(strstr(resp, "\"jsonrpc\""));
    }
    int n = 0;
```

The final `int n = 0;` preserves the variable used by the existing blocked-tool loop; remove the old duplicate `body`, `req`, `resp`, and `n` declarations.

- [ ] **Step 3: Require timestamps for both indexed-root rows**

After the existing root-name and `root_exists` assertions in `ui_server_indexed_roots_reports_projects_and_orphans`, add:

```c
    int indexed_at_count = 0;
    const char *indexed_at_cursor = resp;
    while ((indexed_at_cursor = strstr(indexed_at_cursor, "\"indexed_at\":\"")) != NULL) {
        indexed_at_count++;
        indexed_at_cursor += strlen("\"indexed_at\":\"");
    }
    ASSERT_EQ(indexed_at_count, 2);
```

This is exact for the fixture: `roots-live` and `roots-orphan` are the two primary project rows, and `_config.db` is excluded.

- [ ] **Step 4: Run the characterization tests on the custom base (baseline GREEN)**

```zsh
scripts/test.sh --suites "mcp httpd"
```

Expected: PASS. These tests characterize behavior that already exists on `81b02389`; their RED observation occurs in Task 3 against an upstream-only conflict resolution.

- [ ] **Step 5: Review and commit only the two test files**

```zsh
git diff --check -- tests/test_mcp.c tests/test_httpd.c
git diff -- tests/test_mcp.c tests/test_httpd.c
git add tests/test_mcp.c tests/test_httpd.c
test "$(git diff --cached --name-only | LC_ALL=C sort)" = "$(printf '%s\n' tests/test_httpd.c tests/test_mcp.c | LC_ALL=C sort)"
git diff --cached --check
git commit -m "test: pin custom merge preservation contracts"
```

Expected: one ordinary commit with exactly `tests/test_httpd.c` and `tests/test_mcp.c` staged.

Review boundary: an independent reviewer confirms that each assertion fails if its specific custom production line is absent and that no production behavior changed.

---

### Task 3: Rehearse the Upstream-Only Resolution and Observe RED

**Files:**

- Diagnostic resolution only: the six observed conflict paths
- No retained file changes; this task ends with `git merge --abort`

**Interfaces:**

- Consumes: the characterization-test commit from Task 2 and pinned upstream `31b611a3`.
- Produces: evidence that choosing upstream wholesale would break the custom contracts, plus a clean pre-merge state for the real resolution.

- [ ] **Step 1: Start the pinned merge without committing**

```zsh
git merge --no-ff --no-commit 31b611a30e01aa8f306f9fcba32492cad0702f99
```

Expected: exit non-zero because of content conflicts; Git reports no conflict outside the observed six paths.

- [ ] **Step 2: Verify the conflict set is exactly six**

```zsh
expected_conflicts='README.md
docs/CONFIGURATION.md
src/cli/cli.c
src/cli/cli.h
src/main.c
tests/test_cli.c'
actual_conflicts="$(git diff --name-only --diff-filter=U | LC_ALL=C sort)"
test "$actual_conflicts" = "$(printf '%s\n' "$expected_conflicts" | LC_ALL=C sort)"
```

Expected: the assertion succeeds. `tests/test_mcp.c` merges automatically and is audited separately in Task 4. Any extra or missing conflict path is a stop condition: abort the merge and return to design/audit.

- [ ] **Step 3: Resolve all six conflicts to upstream only for the RED rehearsal**

```zsh
git checkout --theirs -- README.md docs/CONFIGURATION.md \
  src/cli/cli.c src/cli/cli.h src/main.c tests/test_cli.c
git add README.md docs/CONFIGURATION.md \
  src/cli/cli.c src/cli/cli.h src/main.c tests/test_cli.c
test -z "$(git diff --name-only --diff-filter=U)"
```

Expected: the merge is syntactically resolved but has deliberately discarded custom declarations and tests in the conflict files. Do not commit this state.

- [ ] **Step 4: Run the RED command**

```zsh
scripts/test.sh --suites "cli mcp httpd"
```

Expected: FAIL during compilation or linking. The diagnostics must include at least one discarded preservation interface, such as `cbm_metrics_entry_t`, `cbm_config_metrics_flush`, `cbm_config_metrics_prune`, or `CBM_CONFIG_IGNORE_WORKTREES`. A passing run means the rehearsal did not actually select the upstream-only conflict state; stop and inspect before continuing.

- [ ] **Step 5: Abort and prove the characterization commit is restored**

```zsh
git merge --abort
test -z "$(git diff --name-only --diff-filter=U)"
test -z "$(git status --porcelain=v1 --untracked-files=no)"
git show --stat --oneline HEAD
scripts/test.sh --suites "mcp httpd"
```

Expected: the worktree is clean, `HEAD` is still `test: pin custom merge preservation contracts`, and the characterization suites are GREEN again.

Review boundary: preserve the failing command and relevant compiler diagnostics in the task report. There is no commit for the rehearsal.

---

### Task 4: Merge Pinned Upstream and Resolve the Six Conflicts

**Files:**

- Resolve: `README.md`
- Resolve: `docs/CONFIGURATION.md`
- Resolve: `src/cli/cli.c`
- Resolve: `src/cli/cli.h`
- Resolve: `src/main.c`
- Resolve: `tests/test_cli.c`
- Review only: automatically merged `tests/test_mcp.c`
- Review only: all automatically merged files

**Interfaces:**

- Consumes: pinned upstream history and the preservation tests.
- Produces: one merge commit whose first parent is the characterization commit and whose second parent is exactly `31b611a3`.

- [ ] **Step 1: Start the real pinned merge and repeat the exact-set guard**

```zsh
git merge --no-ff --no-commit 31b611a30e01aa8f306f9fcba32492cad0702f99
expected_conflicts='README.md
docs/CONFIGURATION.md
src/cli/cli.c
src/cli/cli.h
src/main.c
tests/test_cli.c'
actual_conflicts="$(git diff --name-only --diff-filter=U | LC_ALL=C sort)"
test "$actual_conflicts" = "$(printf '%s\n' "$expected_conflicts" | LC_ALL=C sort)"
```

Expected: the same six paths conflict. Do not fetch or substitute a newer upstream reference.

- [ ] **Step 2: Resolve `README.md` from upstream structure plus two custom additions**

Use upstream's side as the starting point:

```zsh
git checkout --theirs -- README.md
```

Then apply only these custom documentation requirements with `apply_patch`:

- Under automatic indexing, add `ignore_worktrees` behavior: default `false`, automatic-path skip, explicit `index_repository` refusal, and `index_worktree=true` override.
- In CLI/daemon documentation, add the request-metrics section: sources `mcp|cli|hook|http|index`, hourly RED buckets, minute flush, 90-day retention, `_config.db.metrics_requests`, `GET /api/metrics?hours=N`, and the Control tab.
- In the configuration example, add `config set ignore_worktrees true`.
- Keep upstream's `watcher_enabled`, current language/client counts, single embedded UI product, packaging, security, install/update, and release wording unchanged.

Verify the result:

```zsh
rg -n "ignore_worktrees|index_worktree|metrics_requests|/api/metrics|watcher_enabled" README.md
rg -n "161 languages|45 supported|graph UI is built into the binary" README.md
```

Expected: all custom terms are present once; upstream counts and single-product wording remain.

- [ ] **Step 3: Resolve `docs/CONFIGURATION.md` from upstream structure plus custom storage/policy text**

```zsh
git checkout --theirs -- docs/CONFIGURATION.md
```

Apply these exact additions with `apply_patch`:

- In the `_config.db` storage row, state that the same database also holds additive `metrics_requests` hourly counters, flushed once a minute, retained 90 days, served at `/api/metrics`, and shown in Control.
- Add `ignore_worktrees` with default `false` to the CLI-managed runtime settings table.
- Add a dedicated `ignore_worktrees` section describing the linked-worktree-only check, main-checkout and submodule exclusions, automatic-path skips, explicit refusal, `index_worktree=true`, and the command to turn the policy back off.
- Keep upstream's complete `watcher_enabled` restart semantics and `CBM_RUNTIME_DIR` ancestry/security guidance unchanged.

Verify:

```zsh
rg -n "metrics_requests|ignore_worktrees|index_worktree|watcher_enabled|CBM_RUNTIME_DIR" docs/CONFIGURATION.md
```

Expected: each subject has one coherent section; no duplicated watcher section.

- [ ] **Step 4: Resolve `src/cli/cli.h` using upstream as the structural base**

```zsh
git checkout --theirs -- src/cli/cli.h
```

With `apply_patch`, insert the exact `cbm_metrics_entry_t`, `cbm_config_metrics_flush`, and `cbm_config_metrics_prune` interface block from the File and Interface Map immediately after `cbm_config_delete`. Add only:

```c
#define CBM_CONFIG_IGNORE_WORKTREES "ignore_worktrees"
```

beside the other config keys. Retain upstream's `CBM_CONFIG_WATCHER_ENABLED`, `CBM_CONFIG_UI_ENABLED`, `CBM_CONFIG_UI_PORT`, stdin-gating declarations, Grok/OMP interfaces, hook interfaces, and installer checks.

Verify:

```zsh
rg -n "cbm_metrics_entry_t|cbm_config_metrics_flush|cbm_config_metrics_prune|CBM_CONFIG_IGNORE_WORKTREES|CBM_CONFIG_WATCHER_ENABLED|CBM_CONFIG_UI_ENABLED|CBM_CONFIG_UI_PORT" src/cli/cli.h
```

Expected: one declaration/definition of each interface.

- [ ] **Step 5: Resolve `src/cli/cli.c` using upstream as the structural base**

```zsh
git checkout --theirs -- src/cli/cli.c
```

With `apply_patch`, make these bounded additions only:

1. In `cbm_config_open`, call `sqlite3_busy_timeout(db, 2000)` after opening the database.
2. Replace the config-only DDL with the additive two-table DDL from the custom side: `config` plus `metrics_requests` keyed by `(bucket_hour, source, name, project)` and containing `count`, `errors`, `dur_us_sum`, and `dur_us_max`.
3. Insert the custom `cbm_config_metrics_flush` implementation after `cbm_config_delete`: `BEGIN IMMEDIATE`, prepared upsert-add, NULL project folded to `""`, `dur_us_max = MAX(...)`, one commit, and rollback on any error.
4. Insert `cbm_config_metrics_prune` immediately after the flush function, deleting buckets strictly older than its argument.
5. Add `{CBM_CONFIG_IGNORE_WORKTREES, "false", "Skip linked git worktrees when indexing automatically"}` to `CONFIG_KEYS`.
6. Keep the single upstream `cbm_config_watcher_enabled` implementation and all upstream UI-config routing intact.

Verify the bounded result:

```zsh
rg -n "sqlite3_busy_timeout|CREATE TABLE IF NOT EXISTS metrics_requests|BEGIN IMMEDIATE|cbm_config_metrics_flush|cbm_config_metrics_prune|CBM_CONFIG_IGNORE_WORKTREES|CBM_CONFIG_UI_ENABLED" src/cli/cli.c
test "$(rg -c '^bool cbm_config_watcher_enabled' src/cli/cli.c)" = "1"
```

Expected: metrics remain additive and transactional; watcher/UI implementations are not duplicated or replaced.

- [ ] **Step 6: Resolve `src/main.c` entirely to upstream**

```zsh
git checkout --theirs -- src/main.c
git grep -n "bootstrap_runtime_parent_override\|CBM_RUNTIME_DIR" -- src/daemon/bootstrap.c docs/CONFIGURATION.md
```

Expected: `src/main.c` contains upstream's startup-contention, stdin-gating, hook fast-path, and current client-count changes. `CBM_RUNTIME_DIR` is implemented once in `src/daemon/bootstrap.c`; do not restore the old `main_runtime_parent` call-site fan-out.

- [ ] **Step 7: Resolve `tests/test_cli.c` from upstream plus the metrics contract**

```zsh
git checkout --theirs -- tests/test_cli.c
```

With `apply_patch`, reinsert the complete existing `TEST(cli_config_metrics_flush_and_prune)` from the custom side after `cli_config_persists`, and register it once in `SUITE(cli)` immediately after `RUN_TEST(cli_config_get_set)`. Do not copy the custom watcher test because upstream already contains the same test and suite entry.

Verify:

```zsh
test "$(rg -c '^TEST\(cli_config_metrics_flush_and_prune\)' tests/test_cli.c)" = "1"
test "$(rg -c 'RUN_TEST\(cli_config_metrics_flush_and_prune\)' tests/test_cli.c)" = "1"
test "$(rg -c '^TEST\(cli_config_watcher_enabled_default_and_persist\)' tests/test_cli.c)" = "1"
test "$(rg -c 'RUN_TEST\(cli_config_watcher_enabled_default_and_persist\)' tests/test_cli.c)" = "1"
```

Expected: metrics and watcher tests each have one definition and one registration; all upstream tests remain present.

- [ ] **Step 8: Audit the automatically merged `tests/test_mcp.c`**

Do not check out either side and do not rewrite the file wholesale. The automatically merged issue-704 test must contain upstream's `offset`, `limit`, `metadata_only`, `include_details`, `total`, `returned`, and `has_more` assertions plus Task 2's `indexed_at` and lean-field assertions.

Confirm that the automatically merged custom test families remain:

```zsh
rg -n "indexed_at.*list_projects|tool_index_status_fails_closed_without_indexed_checkout_identity|tool_index_status_freshness_verdict_current_when_indexed_checkout_matches|tool_index_status_freshness_verdict_stale_when_indexed_checkout_mismatches|tool_index_status_freshness_verdict_unknown_when_untracked_only|mcp_ignore_worktrees_gates_explicit_index_repository|index_worktree" tests/test_mcp.c
rg -n "offset.*limit|metadata_only|include_details|has_more|size_bytes" tests/test_mcp.c
```

Expected: both upstream pagination and custom freshness/worktree/indexed-time contracts coexist, and `tests/test_mcp.c` has no conflict marker.

- [ ] **Step 9: Stage exactly the six manual conflict resolutions**

```zsh
git add README.md docs/CONFIGURATION.md \
  src/cli/cli.c src/cli/cli.h src/main.c tests/test_cli.c
test -z "$(git diff --name-only --diff-filter=U)"
git diff --cached --check
```

Expected: no unmerged path and no whitespace error. Do not use `git add -A`; the six explicit paths are the only manual staging list. Git's already-staged automatic merge, including `tests/test_mcp.c`, remains intact.

- [ ] **Step 10: Run the focused GREEN preservation suites before committing**

```zsh
scripts/test.sh --suites "cli daemon_application httpd mcp incremental watcher store_nodes artifact git_context pipeline"
bash tests/test_watcher_disabled.sh
```

Expected: PASS. This covers config/metrics, exactly-once application instrumentation, RPC and indexed-root endpoints, paginated projects, worktree gates, watcher startup policy, generation publication, checkout freshness, and stored metadata.

- [ ] **Step 11: Run the graph-UI preservation checks**

```zsh
cd graph-ui
npm test -- src/components/StatsTab.test.tsx
npm run build
cd ..
git diff --exit-code -- graph-ui/tsconfig.tsbuildinfo || git restore --worktree -- graph-ui/tsconfig.tsbuildinfo
git diff --quiet -- graph-ui/tsconfig.tsbuildinfo
```

Expected: the StatsTab suite and production build pass. A generated `tsconfig.tsbuildinfo` working-tree delta is discarded and never staged; an upstream version already staged by the merge is not confused with a new build-generated delta.

- [ ] **Step 12: Review the custom delta against upstream before the merge commit**

```zsh
git diff --cached --stat
git diff --cached -- README.md docs/CONFIGURATION.md src/cli/cli.c src/cli/cli.h src/main.c tests/test_cli.c tests/test_mcp.c
git grep -n "metrics_requests\|CBM_CONFIG_IGNORE_WORKTREES\|indexed_checkout_sha\|/api/indexed-roots" -- src tests graph-ui README.md docs/CONFIGURATION.md
git grep -n "CBM_VARIANT=ui" -- . ':(exclude)docs/superpowers' || true
```

Expected: the conflict delta from upstream contains only the approved additive custom behavior. Any executable/build branch on `CBM_VARIANT=ui` is a failure; historical text stating that it is obsolete is acceptable.

- [ ] **Step 13: Commit the upstream merge and verify its parents**

```zsh
git commit -m "merge: integrate origin/main at 31b611a3"
test "$(git rev-parse HEAD^2)" = "31b611a30e01aa8f306f9fcba32492cad0702f99"
git merge-base --is-ancestor 81b02389598d6215e82939f29b5b2a6917f96910 HEAD
git merge-base --is-ancestor 31b611a30e01aa8f306f9fcba32492cad0702f99 HEAD
git show --no-patch --pretty=raw HEAD
```

Expected: a two-parent merge commit; both the exact custom base and exact upstream target are ancestors.

Review boundary: an independent reviewer checks the six resolved files plus the automatically merged `tests/test_mcp.c`, the staged/committed diff against `31b611a3`, the preservation-suite transcript, and both-parent ancestry. Fixes require a new ordinary commit only if the merge commit has already been reviewed; do not amend reviewed history.

---

### Task 5: Audit Automatically Merged Preservation and Upstream-Replacement Surfaces

**Files:**

- Review only: `src/daemon/application.c`, `src/daemon/application.h`, `src/daemon/host.c`
- Review only: `src/mcp/mcp.c`, `src/mcp/mcp.h`, `src/mcp/mcp_internal.h`
- Review only: `src/cli/hook_augment.c`, `src/git/git_context.c`, `src/git/git_context.h`
- Review only: `src/store/store.c`, `src/store/store.h`
- Review only: `src/pipeline/pipeline.c`, `src/pipeline/pipeline_incremental.c`, `src/pipeline/pipeline_internal.h`
- Review only: `src/ui/http_server.c`, `graph-ui/src/hooks/useIndexedRoots.ts`, `graph-ui/src/components/StatsTab.tsx`, `graph-ui/src/lib/types.ts`

**Interfaces:**

- Consumes: the committed upstream merge.
- Produces: a signed-off preservation matrix; no code change unless the audit finds a concrete loss.

- [ ] **Step 1: Audit request metrics end to end**

```zsh
rg -n "METRICS_|_other|metrics_record|metrics_flush|metrics_requests|/api/metrics|eventless|hook_event" \
  src/cli/cli.c src/cli/cli.h src/daemon/application.c src/daemon/application.h \
  src/ui/http_server.c tests/test_cli.c tests/test_daemon_application.c tests/test_httpd.c \
  graph-ui/src/components/StatsTab.tsx graph-ui/src/components/StatsTab.test.tsx
```

Confirm: fixed-size accumulator, `_other` saturation, long-key clamping, minute flush, failed-snapshot restoration, 90-day pruning, exactly-once MCP/CLI/hook/HTTP/index recording, bounded metrics HTTP window, local-midnight UI windows, 404 polling stop, row validation, and NULL-safe eventless hooks.

- [ ] **Step 2: Audit indexed roots and graph UI**

```zsh
rg -n "indexed_at|indexed-roots|get_graph_schema|root_exists|deepest|contains|reindex|toLowerCase" \
  src/mcp/mcp.c src/ui/http_server.c tests/test_mcp.c tests/test_httpd.c \
  graph-ui/src/hooks/useIndexedRoots.ts graph-ui/src/components/StatsTab.tsx \
  graph-ui/src/components/StatsTab.test.tsx graph-ui/src/lib/types.ts
```

Confirm: lean default `list_projects` includes `indexed_at` but not nodes/edges/size, detailed mode remains opt-in, UI RPC admits `get_graph_schema`, orphan roots remain, and exact/inside/contains/reindex badges use case folding and deepest-ancestor matching.

- [ ] **Step 3: Audit all four worktree enforcement points**

```zsh
rg -n "ignore_worktrees|index_worktree|linked_worktree|submodule" \
  src/cli/cli.c src/cli/cli.h src/git/git_context.c src/git/git_context.h \
  src/daemon/application.c src/mcp/mcp.c src/mcp/mcp_internal.h src/cli/hook_augment.c \
  tests/test_cli.c tests/test_git_context.c tests/test_daemon_application.c tests/test_mcp.c
```

Confirm: connect/background auto-index skip, explicit refusal with `index_worktree=true` override, submodule exclusion, and hook guidance suppression. Default remains false.

- [ ] **Step 4: Audit checkout freshness as one atomic feature**

```zsh
rg -n "indexed_checkout_sha|worktree_status|status_available|tracked_changes|untracked_source|freshness.*verdict" \
  src/store/store.c src/store/store.h src/pipeline/pipeline.c src/pipeline/pipeline_incremental.c \
  src/pipeline/pipeline_internal.h src/git/git_context.c src/mcp/mcp.c \
  tests/test_store_nodes.c tests/test_pipeline.c tests/test_git_context.c tests/test_mcp.c
```

Confirm: legacy-column probe, full and incremental publication writes, bounded porcelain parser, and current/stale/unknown verbose verdicts all remain. A partial feature is a release blocker.

- [ ] **Step 5: Prove upstream replacement history is present**

```zsh
git merge-base --is-ancestor 31b611a30e01aa8f306f9fcba32492cad0702f99 HEAD
git log --oneline --ancestry-path 81b02389598d6215e82939f29b5b2a6917f96910..HEAD -- \
  src/daemon/bootstrap.c src/foundation/subprocess.c src/store/store.c \
  src/pipeline src/cli/cli.c src/main.c scripts tests
git diff --stat 31b611a30e01aa8f306f9fcba32492cad0702f99..HEAD
```

Confirm that custom conflict resolution did not resurrect old implementations of pagination, runtime relocation, macOS spawn, SQLite transient handling, cross-LSP scaling, allocator behavior, binary replacement, or release/security gates.

Review boundary: a read-only reviewer returns a four-row preservation matrix (`metrics`, `indexed roots/UI`, `worktree policy`, `checkout freshness`) and a separate upstream-replacement verdict. If code must change, stop this audit and make one focused fix commit with its own failing/passing test; do not mix unrelated fixes.

---

### Task 6: Merge the Pinned Idle-Frontend Fix as a Separate Commit

**Files:**

- Merge only: `src/daemon/frontend.c`
- Merge only: `src/daemon/frontend.h`
- Merge only: `tests/test_daemon_frontend.c`

**Interfaces:**

- Consumes: the reviewed upstream merge and commit `7710f86e`.
- Produces: a second merge commit introducing the upstream idle-maintenance observer fix without pulling any unrelated surface.

- [ ] **Step 1: Verify the idle fix's identity and ancestry immediately before merging**

```zsh
test "$(git rev-parse 7710f86e927c5221c395ecbdab9d0f3b75edcb71^{commit})" = "7710f86e927c5221c395ecbdab9d0f3b75edcb71"
test "$(git rev-parse 7710f86e927c5221c395ecbdab9d0f3b75edcb71^)" = "909051ccee2d070f33e15fda71bfe61c9076eea6"
git merge-base --is-ancestor 909051ccee2d070f33e15fda71bfe61c9076eea6 31b611a30e01aa8f306f9fcba32492cad0702f99
```

Expected: all assertions succeed. Do not merge a branch name or moving remote ref.

- [ ] **Step 2: Start the merge without committing and enforce the three-file surface**

```zsh
git merge --no-ff --no-commit 7710f86e927c5221c395ecbdab9d0f3b75edcb71
test -z "$(git diff --name-only --diff-filter=U)"
expected_idle_paths='src/daemon/frontend.c
src/daemon/frontend.h
tests/test_daemon_frontend.c'
actual_idle_paths="$(git diff --cached --name-only | LC_ALL=C sort)"
test "$actual_idle_paths" = "$(printf '%s\n' "$expected_idle_paths" | LC_ALL=C sort)"
git diff --cached --check
```

Expected: no conflicts and exactly the three approved files staged. Any other path is a stop condition; abort the merge and re-audit the commit relationship.

- [ ] **Step 3: Review the idle-fix semantics**

```zsh
git diff --cached -- src/daemon/frontend.c src/daemon/frontend.h tests/test_daemon_frontend.c
rg -n "frontend_test_monitor_observation_count|frontend_test_worker_observation_count|exit_when_cancel_not_needed|frontend_observe_maintenance|worker_idle" \
  src/daemon/frontend.c src/daemon/frontend.h tests/test_daemon_frontend.c
```

Confirm: the independent maintenance monitor remains; duplicate worker-side maintenance observation is removed; blocked-stdio maintenance can exit when cancellation is unnecessary; production builds do not expose test counters.

- [ ] **Step 4: Run the focused GREEN suite before committing**

```zsh
scripts/test.sh --suites "daemon_frontend daemon_application"
```

Expected: PASS, including the new observer-count regression and existing bounded maintenance cancellation tests.

- [ ] **Step 5: Commit and verify merge ancestry**

```zsh
git commit -m "merge: integrate idle frontend fix 7710f86e"
test "$(git rev-parse HEAD^2)" = "7710f86e927c5221c395ecbdab9d0f3b75edcb71"
git merge-base --is-ancestor 7710f86e927c5221c395ecbdab9d0f3b75edcb71 HEAD
git merge-base --is-ancestor 31b611a30e01aa8f306f9fcba32492cad0702f99 HEAD
git merge-base --is-ancestor 81b02389598d6215e82939f29b5b2a6917f96910 HEAD
git show --no-patch --pretty=raw HEAD
```

Expected: the idle fix is the second parent of the new merge commit, and all three pinned histories are ancestors.

Review boundary: an independent reviewer checks only the three-file merge diff and the `daemon_frontend`/`daemon_application` transcript before later performance work starts.

---

### Task 7: Run the Integration Gate and Hand Off File Ownership

**Files:**

- Verify only: complete integration branch
- Do not edit or commit unless a focused regression is found

**Interfaces:**

- Consumes: both reviewed merge commits.
- Produces: a green integration baseline that releases files to the condition/runtime, indexing/watcher, and browse/UI plans.

- [ ] **Step 1: Run the required targeted native suites in one canonical invocation**

```zsh
scripts/test.sh --suites "cli daemon_application daemon_frontend daemon_ipc daemon_runtime httpd mcp incremental watcher store_nodes artifact activation_transaction git_context pipeline"
```

Expected: every named suite passes with no unknown suite and no test failure.

- [ ] **Step 2: Run process-level watcher and UI checks omitted by focused C mode**

```zsh
bash tests/test_watcher_disabled.sh
cd graph-ui
npm test
npm run build
cd ..
git diff --exit-code -- graph-ui/tsconfig.tsbuildinfo || git restore --worktree -- graph-ui/tsconfig.tsbuildinfo
git diff --quiet -- graph-ui/tsconfig.tsbuildinfo
```

Expected: watcher-off process behavior, all Vitest tests, and the production UI build pass. No generated UI build artifact remains modified.

- [ ] **Step 3: Verify final ancestry, cleanliness, and commit boundaries**

```zsh
git merge-base --is-ancestor 81b02389598d6215e82939f29b5b2a6917f96910 HEAD
git merge-base --is-ancestor 31b611a30e01aa8f306f9fcba32492cad0702f99 HEAD
git merge-base --is-ancestor 7710f86e927c5221c395ecbdab9d0f3b75edcb71 HEAD
git log --graph --decorate --oneline --max-count=12
git status --short
git diff --check 81b02389598d6215e82939f29b5b2a6917f96910..HEAD
```

Expected: all ancestry checks pass; the log shows the characterization commit, the upstream merge commit, and the idle-fix merge commit as separate review boundaries; the worktree is clean except for plan/spec files intentionally owned by the planning session.

- [ ] **Step 4: Perform the final whole-branch review**

The independent reviewer must inspect:

```zsh
git diff --stat 81b02389598d6215e82939f29b5b2a6917f96910..HEAD
git diff 31b611a30e01aa8f306f9fcba32492cad0702f99..HEAD -- \
  README.md docs/CONFIGURATION.md src/cli/cli.c src/cli/cli.h src/main.c \
  tests/test_cli.c tests/test_mcp.c tests/test_httpd.c \
  src/daemon/frontend.c src/daemon/frontend.h tests/test_daemon_frontend.c
```

Acceptance checklist:

- Request metrics preserve storage, aggregation, restoration, pruning, all five instrumentation sources, HTTP API, and Control UI.
- Lean paginated projects preserve `indexed_at` without default node/edge details.
- `/api/indexed-roots`, orphan/root badges, graph-schema UI RPC, and last-indexed display remain.
- `ignore_worktrees=false` and all four enforcement points remain.
- `indexed_checkout_sha` schema, publication boundaries, parser, and verdict remain atomically.
- Upstream implementations and complete upstream ancestry remain.
- Idle-fix merge touches exactly its three approved files.
- No semantic index version bump, cache reset, stage deletion, active `CBM_VARIANT=ui` branch, or newly staged generated UI build output exists.

- [ ] **Step 5: Record the handoff boundary without external effects**

Report the final branch name, final `HEAD`, the two merge SHAs, exact test commands, and reviewer verdicts. State explicitly that the live binary/cache/configuration were not mutated and that Plans 2–4 may now begin only with non-overlapping ownership:

- Condition/runtime plan owns `compat_thread`, frontend, runtime, and IPC wait/interrupt interfaces.
- Index/watcher plan owns config parsing and physical-job admission after `cli.c`, `host.c`, and `application.c` are released.
- Browse/UI plan owns `StatsTab`, browse HTTP handler, i18n, and their tests after integration paths are released.

There is no push, PR, install, deploy, or release commit in this plan.
