# Upstream and Performance Integration Design

**Date:** 2026-08-28
**Status:** Approved in chat on 2026-08-28
**Repository:** `codebase-memory-mcp`

## Goal

Integrate the complete current upstream history into the locally deployed custom product without losing any local behavior, then remove the measured sources of idle CPU, redundant indexing, and stuck folder browsing. Produce a verified macOS arm64 UI candidate and a rollback procedure; do not mutate the live binary, live cache, or global configuration during implementation.

## Evidence and fixed references

- Installed custom history: `81b02389598d6215e82939f29b5b2a6917f96910` (`0.10.3-53-g81b02389`).
- Current upstream: `31b611a30e01aa8f306f9fcba32492cad0702f99` (`origin/main`, fetched 2026-08-28).
- Unmerged upstream idle-frontend fix: `7710f86e927c5221c395ecbdab9d0f3b75edcb71`.
- `f5285844` and all request-metrics commits are ancestors of `81b02389`.
- The current worktree's modified `.gitignore` belongs to the user and must remain untouched.
- The semantic index version remains `3` and the compressed artifact format
  remains `2`. The database format gate is new:
  `CBM_INDEX_FORMAT_VERSION=1`.
- The live-cache audit found 94 project databases at SQLite
  `PRAGMA user_version=0`; 88 registered roots still exist and six are missing.
  These counts describe this workstation rollout only, not a product-wide
  migration rule.
- The first index of a version-0 database under the candidate removes that
  legacy project database, performs a full rebuild, and seals the replacement
  with `user_version=1`. Installing or starting the candidate alone does not
  migrate a database.

The implementation must stop if any pinned reference changes before the isolated integration branch is created. Updating upstream after execution begins requires a new audit, not an implicit moving-target merge.

## Chosen integration topology

Create an isolated worktree and branch from `81b02389`, then merge the pinned `origin/main` with a merge commit. This is preferred over rebuilding on `f5285844` because it preserves the exact installed custom history and lets Git recognize upstream-equivalent picks. It is preferred over rebase because rebase rewrites published/custom SHAs and has more conflict stops.

Expected merge conflicts are limited to:

- `README.md`
- `docs/CONFIGURATION.md`
- `src/cli/cli.c`
- `src/cli/cli.h`
- `src/main.c`
- `tests/test_cli.c`

`tests/test_mcp.c` is expected to merge automatically and remains an explicit preservation-audit path. Any conflict outside the six listed paths is a stop condition for the merge task. After the main merge is reviewed and committed, merge `7710f86e`. Its expected changed surface is only `src/daemon/frontend.c`, `src/daemon/frontend.h`, and `tests/test_daemon_frontend.c`.

The original checkout remains on `feat/request-metrics`; the isolated branch will be `integration/main-custom-performance-20260828`. Use an isolated path under `/private/tmp` so the user's dirty `.gitignore` does not need a `.worktrees/` entry.

## Local behavior that must survive

### Request metrics

Preserve the additive `metrics_requests` table in `_config.db`, fixed-size daemon accumulator, `_other` saturation, long-key clamping, minute flush with failed-snapshot restoration, 90-day pruning, and exactly-once instrumentation for MCP, CLI TOOL frames, hooks, HTTP, and physical index attempts. Preserve `/api/metrics`, its bounded time window, the Control-tab visualizations, local-midnight semantics, 404 polling stop, row validation, and the eventless-hook null guard.

No new metrics database is allowed. Old configuration databases must open without migration loss. Instrumentation may not make an optional hook event or project pointer mandatory.

### Indexed roots and graph UI

Preserve `indexed_at` in lean `list_projects` results without restoring expensive default details. Preserve the `get_graph_schema` read-only RPC permission, `/api/indexed-roots`, orphan roots, exact/inside/contains/reindex badges, case folding, deepest-ancestor matching, and last-indexed display.

Use upstream's single-product embedded UI packaging. Do not restore obsolete `CBM_VARIANT=ui` logic or commit generated `tsconfig.tsbuildinfo`.

### Worktree policy

Preserve the opt-in `ignore_worktrees=false` policy and all four enforcement points: connect/background auto-index skip, explicit index refusal unless `index_worktree=true`, submodule exclusion, and hook guidance. Keep the default compatible with existing installations.

### Checkout freshness

Preserve `indexed_checkout_sha` at successful full and incremental publication boundaries, legacy database compatibility, bounded porcelain parsing, and current/stale/unknown `index_status` verdicts. The schema write, generation-boundary persistence, parser, and MCP verdict are one atomic feature and cannot be partially retained.

### Upstream replacements

Prefer upstream's authoritative implementations for scalable/paginated project discovery, `watcher_enabled`, hermetic test environment, runtime-dir relocation, macOS `posix_spawn`, SQLite transient-state handling, cross-LSP scaling, allocator behavior, binary replacement, and all security/release fixes. Do not preserve local merge topology or old implementations when upstream supersedes them.

## Performance subprojects

### 1. Idle frontend and daemon waits

Start from `7710f86e`, which removes duplicate worker-side maintenance observation. Then add a portable condition-variable abstraction and replace frontend queue, backpressure, EOF drain, and stop polling with predicate loops. Maintenance cancellation must set state and broadcast under the mutex, then cancel the exact runtime token outside the mutex.

Replace the runtime accept timeout loop with an interruptible infinite accept: POSIX wake pipe and Windows manual-reset event. Add a service state condition so client-count and exited waiters block on state transitions instead of 1 ms polling. Leave bounded exceptional cleanup/startup loops unchanged.

Required semantics:

- zero periodic worker wakeups in an idle frontend;
- zero timeout returns from an idle accept loop;
- blocked stdio maintenance cancellation still completes within its existing bound;
- queue/backpressure, final-response drain, activation acknowledgement, and shutdown deadlines remain unchanged;
- all waits use mutex-protected predicate loops and tolerate spurious wakeups.

### 2. Indexing and watcher control

Adopt upstream's watcher fixes and master kill switch. Preserve the compatibility default `watcher_enabled=true`, but document and validate the internal rollout with `watcher_enabled=false` for the many-project workstation profile.

Add persistent `max_index_jobs` with range `1..16` and default `2`. Reuse `cbm_daemon_application_config_t.physical_job_limit`; invalid, zero, negative, overflow, or non-decimal values fall back to `2` and cannot increase concurrency. A daemon generation captures the setting, so changes require session/daemon restart.

The migration rollout profile uses `auto_index=false`,
`watcher_enabled=false`, `max_index_jobs=1`, and `CBM_WORKERS=4`. The product
default for `max_index_jobs` remains `2`; the lower rollout value makes the
one-time destructive rebuilds serial and observable. This is deployment
evidence, not a change to graph semantics. Full-mode graph quality remains
canonical; the work must not disable cross-file LSP or silently downgrade
stored coverage.

Do not add automatic stage-file garbage collection in this batch. Existing stage names lack durable ownership identity and backups may be the only recovery artifact after a crash. Operational cleanup is allowed only after checking age, active processes, live worker ownership, and database integrity; no backup is automatically deleted.

### 3. Folder browser resilience

The React modal owns a monotonically increasing request generation and an `AbortController`. A new browse aborts its predecessor, a five-second timer aborts a hung request, unmount aborts the active request, and only the latest request may mutate state or clear loading. Supersession/unmount aborts are silent; timer abort reports a localized timeout while retaining the last good listing.

The backend keeps a 200-result cap, adds a 4096-scanned-entry cap and a best-effort 100 ms enumeration budget, and returns `truncated`. Directory classification must use the repository's non-following path metadata seam so symlink/reparse directories are neither advertised nor accepted as the final requested directory. JSON construction must remain valid at all bounds.

A single filesystem syscall on a hung remote mount is not cancellable in the current synchronous server; the client deadline prevents permanent UI loading. Moving browse into a subprocess is outside this batch unless tests prove the bounded design insufficient.

## Implementation decomposition and ownership

The work is divided into four plans with serialized dependency boundaries:

1. **Git integration and preservation** — sole owner of merge conflicts and custom-surface reconciliation. No other implementation begins until its targeted preservation suite is green and reviewed.
2. **Condition foundation and runtime idle** — sole owner of `compat_thread`, frontend, runtime, and IPC condition/interrupt interfaces.
3. **Index admission and watcher controls** — sole owner of configuration parsing and physical-job admission after the integration task releases `cli.c`, `host.c`, and `application.c`.
4. **Browse/UI resilience** — sole owner of `StatsTab`, browse HTTP handler, i18n, and their tests after the integration task releases those paths.

Plans 2, 3, and 4 may run in parallel only after file ownership is proven non-overlapping. Because Plans 2 and 3 both may touch daemon host/application surfaces, their edits must be split or serialized at those files. The main agent is the only integrator and committer of cross-task conflict resolutions.

## TDD and review contract

Every new behavior starts with a focused failing test whose failure is observed before production code is written. Upstream merge preservation is validated by pre-existing and newly added contract tests; conflict resolution itself is not treated as a new behavior.

Each task receives:

- an isolated implementer with explicit file ownership;
- a report containing RED and GREEN commands/output;
- an independent task reviewer checking both specification compliance and code quality;
- a bounded fix/re-review loop before dependent work proceeds.

A final whole-branch reviewer examines the complete diff and the preservation matrix.

## Verification gates

The candidate must pass, in order:

1. Targeted native suites for `cli`, `daemon_application`, `daemon_frontend`, `daemon_ipc`, `daemon_runtime`, `httpd`, `mcp`, `incremental`, `watcher`, `store_nodes`, `artifact`, and `activation_transaction`.
2. Graph UI `npm test` and production build.
3. No-test-skip, lint, security, UI security, and vendored-security gates.
4. Canonical macOS arm64 full suite with ASan/UBSan.
5. TSan for changed concurrency suites and LSan.
6. UI production binary, composition check, packaging, released-byte smoke, and soak.
7. Cache compatibility against a copy of the live cache, including `PRAGMA quick_check(1)`, metrics preservation, no new false `.corrupt` files, lean paginated listing, `index_status`, and `search_graph`.

Performance acceptance:

- idle daemon CPU stays within the repository's `<=5%` soak gate;
- idle frontend worker performs no periodic polling after entering its condition wait;
- idle accept loop has zero timeout wakes;
- `list_projects` default page p95 `<500 ms`, detailed page p95 `<2 s` on the copied populated cache;
- default results contain no expensive node/edge details;
- ordinary browse warm p95 `<500 ms`, timeout surfaces by `5.5 s`, stale responses never change state;
- fixed-corpus index median is no worse than 10%; over 20% regression blocks release;
- search median/p95 regressions remain under 10%/20% with stable totals.

## Rollout and rollback

Implementation and verification operate only on isolated build, cache, and runtime directories. No test may point at the live cache for mutation.

The candidate is not installed automatically. After all local gates pass, produce:

- exact candidate version and SHA-256;
- exact backup command for the installed `ec443e08...` binary;
- guarded installer plan/dry-run output;
- cache-copy compatibility evidence;
- rollback command using the prior binary's guarded installer.

Deployment and index migration are separate operations. Installing the
candidate may replace the executable and restart the daemon, but it does not
rewrite project databases. Before either operation, quiesce CBM and take a
durable backup of the installed binary, configuration database, and complete
cache. Preserve that backup until every migrated project passes integrity and
query checks and rollback is no longer required.

The authorized live migration must use this order:

1. Disable automatic indexing and the watcher, set `max_index_jobs=1`, and
   start the candidate generation with `CBM_WORKERS=4`.
2. Inventory registered roots without indexing them. Keep missing roots out of
   the migration queue; do not invent replacement paths.
3. Explicitly full-index one existing root as a canary. Its version-0 database
   is expected to be deleted and rebuilt once, then sealed at
   `user_version=1`.
4. Validate `PRAGMA quick_check(1)`, the format seal, and a representative graph
   query before continuing. Run `check_index_coverage` on representative paths
   and require `coverage_version=3` plus `hash_records_complete=true`; do not use
   `index_status` as a substitute because it does not expose those fields. The
   direct-SQL equivalent must find exactly one matching row in
   `index_coverage_meta`.
5. Migrate the remaining existing roots one at a time, applying the same
   checks after each project. For the audited workstation this means one
   canary followed by the remaining 87 of 88 existing roots; the six missing
   roots remain untouched.
6. Treat the legacy/duplicated `centroriente` registration as a separate
   reconciliation case rather than including it in an automatic batch.

Do not stamp `PRAGMA user_version` externally. Only a successful full rebuild
may create the version-1 seal. On any failure, stop the serial queue. The format
gate may already have unlinked the legacy database, so preserve the remaining
logs and stage artifacts and restore that database from the pre-migration
snapshot; do not promise that a failed database still exists.

After any project has migrated, rollback is a coordinated state restore, not a
binary-only downgrade: disable automation, quiesce every CBM process, restore
the cache and configuration snapshot together with the previous binary, verify
the restored databases, and only then restart the old generation and restore
its prior automation settings.

Actual installation, daemon/frontend quiescence, global configuration change, stale-stage deletion, push, PR, and release remain separate external-effect gates requiring explicit authorization.

## Non-goals

- No semantic-index version or artifact-format bump. The only reset is the
  required one-time full rebuild of a legacy version-0 project database when
  that project is explicitly indexed under database format version `1`.
- No automatic deletion of activation backups or stage artifacts.
- No rewrite of all polling in unrelated exceptional paths.
- No downgrade from full graph coverage to improve timings.
- No resurrection of obsolete UI variants.
- No direct mutation of the user's current branch, dirty `.gitignore`, installed binary, live daemon, or live cache during implementation.

## Research basis

The design is based on process sampling, request metrics, cache measurements, graph/source inspection, pinned upstream commits, and merge-tree/range-diff analysis. Second Brain contributed two applicable directives: profile before optimizing, and never integrate from a stale local main/reference. Primary platform contracts consulted were POSIX condition/open semantics, the Fetch abort model, and Windows condition/reparse behavior.
