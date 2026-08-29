# Index-control workstation evidence

## Candidate and binary

- Candidate commit: `09062b2f52bf64330ab19bcee11b0d7d321a06c5`
- Candidate metadata: `09062b2f52bf64330ab19bcee11b0d7d321a06c5 2026-08-29T01:23:07-04:00 test(daemon): isolate watcher process gate`
- Candidate binary SHA-256: `c6cfc3eef3a371c06a2fa8afeb416bdca610b24458319a9d3fa7ce19d8bed38b`

## Isolation boundary

All runtime checks used fresh owner-private directories below `/private/tmp`
for `CBM_RUNTIME_DIR` and `CBM_CACHE_DIR`. The process regression created its
own disposable project/cache fixtures and closed each daemon generation before
evaluating absence assertions. The installed binary, live daemon, live cache,
and global configuration were untouched.

## Commands

```bash
git rev-parse HEAD
git show -s --format='%H %cI %s' HEAD

task4_run_root="$(mktemp -d /private/tmp/cbm-task4-gate.XXXXXX)"
mkdir -m 700 "$task4_run_root/runtime" "$task4_run_root/cache"
CBM_RUNTIME_DIR="$task4_run_root/runtime" \
CBM_CACHE_DIR="$task4_run_root/cache" \
CBM_WORKERS=4 \
make -f Makefile.cbm test-focused TEST_SUITES="cli daemon_application pipeline"

make -f Makefile.cbm cbm TEST_SEAMS=1

task4_process_root="$(mktemp -d /private/tmp/cbm-task4-process.XXXXXX)"
mkdir -m 700 "$task4_process_root/runtime" "$task4_process_root/cache"
CBM_RUNTIME_DIR="$task4_process_root/runtime" \
CBM_CACHE_DIR="$task4_process_root/cache" \
CBM_TEST_BINARY="$PWD/build/c/codebase-memory-mcp" \
bash tests/test_watcher_disabled.sh

task4_tsan_root="$(mktemp -d /private/tmp/cbm-task4-tsan.XXXXXX)"
mkdir -m 700 "$task4_tsan_root/runtime" "$task4_tsan_root/cache"
CBM_RUNTIME_DIR="$task4_tsan_root/runtime" \
CBM_CACHE_DIR="$task4_tsan_root/cache" \
make -f Makefile.cbm test-tsan \
  TEST_TSAN_SUITES="daemon_application watcher pipeline"

shasum -a 256 build/c/codebase-memory-mcp
```

## Process-contract evidence

The real-binary regression emitted these summary lines:

```text
ok: configured max_index_jobs snapshot advances only on daemon restart
evidence: level=info msg=daemon.start version=dev pid=18674 cache_fingerprint=4b9cefc2e32bdbd26a29ab95baa50999419176577224c37b3d101f0ea12295fa memory_budget_bytes=25769803776 physical_job_limit=2 worker_memory_budget_bytes=12884901888
evidence: level=info msg=pipeline.mode mode=parallel workers=4 files=65
ok: disabled — watcher.disabled emitted, no watcher.start/watch, manual index_repository served
ok: disabled+auto_index — auto-index still ran, no watcher.start, no registration
ok: enabled — watcher.start emitted and session project registered
PASS: watcher_enabled kill-switch process regression (#335)
```

The two `evidence:` lines are the exact matched log records emitted only after
the process test asserted that both captures were non-empty. The same closed
lifecycle also asserted `watcher.disabled ... reason=config` and the absence of
`watcher.start` and `watcher.watch` for the disabled/manual run. The manual
request returned a successful full index, while the enabled positive control
observed watcher start and project registration.

## Native and concurrency gates

- ASan/UBSan focused command with explicit `CBM_WORKERS=4`: PASS for `cli`,
  `daemon_application`, and `pipeline` (`604 passed`).
- TSan command: PASS for `daemon_application`, `watcher`, and `pipeline` with
  `CBM_WORKERS=4` (`382 passed`), with no race report.
- Production binary build with `TEST_SEAMS=1`: PASS.
- Real-binary watcher/index-control process contract: PASS.

## Graph-quality guard

The focused and TSan `pipeline` suites passed with the four-worker profile,
including full-mode, parallel/sequential parity, cross-file LSP, and stored
coverage contracts. The scheduling controls did not change semantic index
version `3`, select a lower graph mode, or disable coverage.

## Generation restart semantics

The process contract first observed a daemon generation with configured
`max_index_jobs=7`, changed the persisted value to `16`, retired that lifecycle,
and observed `physical_job_limit=16` only in the next isolated generation. A
client reconnect was not used as a substitute for daemon retirement.

## Explicit exclusions

No live rollout, installation, push, pull request, stage cleanup, backup
deletion, schema bump, semantic-index version change, or graph-mode downgrade was
performed. Automatic stage-file and activation-backup garbage collection remain
out of scope.
