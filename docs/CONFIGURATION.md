# Configuration Reference

This page documents the configuration files that `codebase-memory-mcp` reads or writes today.

## At a Glance

| Purpose | Path | Format | Notes |
|---|---|---|---|
| Global custom extension mapping | `$XDG_CONFIG_HOME/codebase-memory-mcp/config.json` | JSON | Falls back to `~/.config/codebase-memory-mcp/config.json` when `XDG_CONFIG_HOME` is unset. |
| Per-project custom extension mapping | `{repo_root}/.codebase-memory.json` | JSON | Overrides conflicting global `extra_extensions` entries. |
| CLI-managed runtime settings | `${CBM_CACHE_DIR:-~/.cache/codebase-memory-mcp}/_config.db` | SQLite | Written by `codebase-memory-mcp config set/reset`. The same database also holds additive `metrics_requests` hourly counters, flushed once a minute, retained for 90 days, served at `GET /api/metrics`, and shown in the UI **Control** tab. |
| UI settings | `${CBM_CACHE_DIR:-~/.cache/codebase-memory-mcp}/config.json` | JSON | Stores `ui_enabled` and `ui_port`. |
| Daemon operation log | `${CBM_CACHE_DIR:-~/.cache/codebase-memory-mcp}/logs/cbm-daemon.log` | Structured log | Durable daemon lifecycle, watcher/indexing, UI, resource, and error events. |
| Admission conflict log | `${CBM_CACHE_DIR:-~/.cache/codebase-memory-mcp}/logs/daemon-conflicts.ndjson` | NDJSON | Exact-build, ABI, and canonical-cache conflicts. |
| Activation log | `${CBM_CACHE_DIR:-~/.cache/codebase-memory-mcp}/logs/activation-events.ndjson` | NDJSON | Install/update/uninstall activation progress and outcomes. |

CBM resolves `CBM_CACHE_DIR` to a canonical per-account path before using any of these locations. The log directory and files are private to the account.

## 1. Custom File Extension Mapping

Two optional JSON files let you map additional file extensions to built-in languages.

### Global config

Default path:

```text
$XDG_CONFIG_HOME/codebase-memory-mcp/config.json
```

Fallback when `XDG_CONFIG_HOME` is unset:

```text
~/.config/codebase-memory-mcp/config.json
```

### Per-project config

Place this file in the repository root:

```text
.codebase-memory.json
```

### Format

```json
{
  "extra_extensions": {
    ".blade.php": "php",
    ".mjs": "javascript",
    ".twig": "html"
  }
}
```

Notes:

- Extension keys must include the leading dot.
- Language names are case-insensitive.
- Unknown language names are skipped.
- Missing files are ignored.
- If the same extension appears in both files, the per-project file wins.

## 2. CLI-Managed Runtime Settings

The `config` subcommand stores runtime settings in a small SQLite database:

```text
${CBM_CACHE_DIR:-~/.cache/codebase-memory-mcp}/_config.db
```

Inspect or change values with the CLI:

```bash
codebase-memory-mcp config list
codebase-memory-mcp config get auto_index
codebase-memory-mcp config set auto_index true
codebase-memory-mcp config set auto_index_limit 50000
codebase-memory-mcp config set watcher_enabled false
codebase-memory-mcp config set max_index_jobs 1
codebase-memory-mcp config reset auto_index
```

Current keys:

| Key | Default | Meaning |
|---|---|---|
| `auto_index` | `false` | Automatically index new projects when an MCP session starts. |
| `auto_index_limit` | `50000` | Maximum file count allowed for automatic indexing of a new project. |
| `ignore_worktrees` | `false` | Skip linked git worktrees (`git worktree add`) on automatic indexing paths. |
| `auto_watch` | `true` | Register the session's project with the background git watcher on connect. Set `false` to keep a session from registering its project (the watcher still runs for other projects). |
| `watcher_enabled` | `true` | Master switch for the background watcher subsystem. Set `false` to stop the watcher from starting at all — no poll thread and no project registration. Reindex manually with `index_repository` when disabled. |
| `max_index_jobs` | `2` | Maximum concurrent distinct physical index jobs. Strict decimal range `1..16`; missing or invalid values use `2`. Read once when the daemon starts. |
| `index_max_files` | `off` | Optional maximum number of accepted source files in one discovery run. |
| `index_max_source_mb` | `off` | Optional maximum accepted source size in MiB in one discovery run. |

> **`watcher_enabled` vs `auto_watch`.** `watcher_enabled` controls whether the
> watcher *subsystem* starts at all (the background poll thread). `auto_watch` is
> narrower: it only controls whether a connecting session registers *its own*
> project with an already-running watcher. When `watcher_enabled=false`,
> `auto_watch` has no effect — there is no watcher to register with.
>
> They also differ in **when they are read**, which matters because the watcher
> lives in the background daemon, not in your MCP client:
>
> - `auto_watch` is consulted each time a session would register its project, so
>   a change applies to sessions that connect afterwards.
> - `watcher_enabled` is read **once, when the daemon starts**, because it decides
>   whether the watcher is built at all. The daemon is long-lived and outlives
>   individual MCP sessions, so **reconnecting your client is not enough** — retire
>   the daemon so the next one picks the new value up:
>
> ```bash
> codebase-memory-mcp config set watcher_enabled false
> codebase-memory-mcp daemon stop     # next session starts a daemon without the watcher
> codebase-memory-mcp daemon status   # confirm
> ```
>
> Disabling the watcher does not disable anything else: the daemon still starts,
> `auto_index` still runs, and `index_repository` stays available for manual
> reindexing.

The two `index_max_*` settings are independent and disabled by default. They
apply to explicit indexing, automatic indexing, and watcher re-indexing, but not
to `cross-repo-intelligence`, which does not scan repository source files.
Equality is allowed; exceeding either setting fails the complete index request
and preserves any previously serving database. See
[Index resource limits](INDEX_RESOURCE_LIMITS.md) for counting, validation, and
error-response details.

### Index concurrency and the serial migration profile

`max_index_jobs` limits distinct physical index processes owned by one daemon
generation. Its product default is `2`; use `1` for a format migration so
destructive one-time rebuilds remain serial and observable. A missing value or
anything other than an ASCII decimal integer in `1..16` falls back to `2`;
invalid configuration cannot raise concurrency.
Semantically identical requests still coalesce into one physical job before the
capacity limit is applied.

`CBM_WORKERS` is separate: it limits parallel workers *inside each physical
index process*. The safe format-migration profile is:

```text
watcher_enabled=false
auto_index=false
max_index_jobs=1
CBM_WORKERS=4
```

This is an opt-in migration profile, not a change to the compatibility
default: `watcher_enabled` remains `true`. With the watcher disabled, manual
`index_repository(mode="full")` remains available.

The daemon snapshots persisted daemon settings such as `watcher_enabled` and
`max_index_jobs`, plus daemon-owned environment such as `CBM_WORKERS`, when a
generation starts. Reconnecting a client is insufficient. Retire the current
daemon and start the next generation with the intended environment:

```bash
codebase-memory-mcp daemon stop
CBM_WORKERS=4 codebase-memory-mcp daemon start
codebase-memory-mcp daemon status
```

The aggregate indexing memory budget is divided at spawn time: each physical
worker receives the aggregate divided by the jobs active then, so a lone job may
use the whole budget and concurrent jobs split it. If the budget is smaller than
the effective physical-job limit, CBM reduces effective capacity before
admitting work so every admitted worker still receives a non-zero slice and the
aggregate budget is not exceeded.

These controls govern scheduling only. They do not select `fast` or `moderate`
mode, change semantic index version `3`, disable cross-file LSP, or remove stored
coverage. The canonical full-mode quality contract remains unchanged.

#### One-time database format migration

Database format version `1` (`CBM_INDEX_FORMAT_VERSION=1`) leaves semantic index
version `3` and compressed artifact format `2` unchanged. Deployment does not
migrate an index: installing or starting the new binary leaves each project
database untouched. The first index of a legacy database with SQLite
`PRAGMA user_version=0` removes that project database, performs one full
rebuild, and seals the replacement with `user_version=1`.

Use this runbook for an existing cache:

1. Stop the daemon and make a durable backup of the installed binary,
   configuration database, and complete cache. Keep the backup until all
   projects have passed post-migration checks.
2. Inventory registered roots and divide them into existing, missing, and any
   legacy or duplicate registrations. Do not index a missing root or guess a
   replacement path.
3. Set `auto_index=false`, `watcher_enabled=false`, and `max_index_jobs=1`.
   Start a new daemon generation with `CBM_WORKERS=4` and confirm the effective
   settings.
4. Run `index_repository(mode="full")` for one existing root as a canary.
   Confirm `PRAGMA quick_check(1)`, `user_version=1`, and a representative graph
   query. Run `check_index_coverage` on representative indexed paths and require
   its metadata to report `coverage_version=3` and
   `hash_records_complete=true`; `index_status` does not expose these two gates.
   The equivalent direct-SQL assertion against the rebuilt project database
   must return `1`:

   ```sql
   SELECT COUNT(*) = 1
   FROM index_coverage_meta
   WHERE coverage_version = 3 AND hash_records_complete = 1;
   ```

5. Full-index the other existing roots one at a time, repeating those checks
   after every project. Stop on the first failure. The format gate may already
   have unlinked the legacy database, so preserve the remaining logs and stage
   artifacts and recover the database from the pre-migration snapshot.
6. Reconcile legacy or duplicate registrations separately. Restore ordinary
   concurrency or automation only after the serial migration is complete.

Never stamp `PRAGMA user_version=1` with SQLite or another external tool. The
seal is evidence that CBM completed the full rebuild; changing it independently
would skip the required migration while making the database appear current.
After any project has migrated, rollback requires a coherent state rollback:
disable automation, quiesce all CBM processes, restore the cache and
configuration snapshot together with the previous binary, verify the restored
databases, and only then restart the old generation and restore its prior
automation settings. Replacing only the executable is not a valid rollback.

#### Isolated rollout rehearsal

The following rehearsal uses a disposable cache and does not change the live
cache, daemon, installed binary, or global launch configuration:

```bash
(
  set -euo pipefail

  binary="$(command -v codebase-memory-mcp)"
  case "$(uname -s)" in
    Darwin) rollout_parent="/private/tmp" ;;
    *) rollout_parent="${TMPDIR:-/tmp}" ;;
  esac
  rollout_root=""
  rollout_cache=""
  rollout_runtime=""
  cleanup_done=0

  # Invoked indirectly by the EXIT trap.
  # shellcheck disable=SC2329
  cleanup_rollout() {
    [ "$cleanup_done" -eq 0 ] || return 0
    cleanup_done=1
    if [ -n "$rollout_cache" ] && [ -n "$rollout_runtime" ]; then
      CBM_CACHE_DIR="$rollout_cache" CBM_RUNTIME_DIR="$rollout_runtime" \
        "$binary" daemon stop >/dev/null 2>&1 || true
    fi
    case "$rollout_root" in
      "$rollout_parent"/cbm-index-control.*) rm -rf -- "$rollout_root" ;;
      "") ;;
      *) echo "refusing unsafe cleanup: $rollout_root" >&2 ;;
    esac
  }
  trap cleanup_rollout EXIT
  trap 'exit 129' HUP
  trap 'exit 130' INT
  trap 'exit 143' TERM

  # A failed mktemp exits before rollout_cache can become "/cache".
  rollout_root="$(mktemp -d "$rollout_parent/cbm-index-control.XXXXXX")"
  chmod 700 "$rollout_root"
  rollout_cache="$rollout_root/cache"
  rollout_runtime="$rollout_root/runtime"
  fixture_repo="$rollout_root/repo"
  mkdir -m 700 "$rollout_cache" "$rollout_runtime" "$fixture_repo"

  i=0
  while [ "$i" -lt 64 ]; do
    printf 'int worker_%s(void) { return %s; }\n' "$i" "$i" \
      >"$fixture_repo/worker_$i.c"
    i=$((i + 1))
  done
  git -C "$fixture_repo" init -q
  git -C "$fixture_repo" -c user.email=rehearsal.invalid \
    -c user.name=rehearsal add -A
  git -C "$fixture_repo" -c user.email=rehearsal.invalid \
    -c user.name=rehearsal commit -q -m fixture

  CBM_CACHE_DIR="$rollout_cache" CBM_RUNTIME_DIR="$rollout_runtime" \
    "$binary" config set max_index_jobs 1
  CBM_CACHE_DIR="$rollout_cache" CBM_RUNTIME_DIR="$rollout_runtime" \
    "$binary" config set auto_index false
  CBM_CACHE_DIR="$rollout_cache" CBM_RUNTIME_DIR="$rollout_runtime" \
    "$binary" config set watcher_enabled false
  [ "$(CBM_CACHE_DIR="$rollout_cache" CBM_RUNTIME_DIR="$rollout_runtime" \
    "$binary" config get max_index_jobs)" = "1" ]
  [ "$(CBM_CACHE_DIR="$rollout_cache" CBM_RUNTIME_DIR="$rollout_runtime" \
    "$binary" config get auto_index)" = "false" ]
  [ "$(CBM_CACHE_DIR="$rollout_cache" CBM_RUNTIME_DIR="$rollout_runtime" \
    "$binary" config get watcher_enabled)" = "false" ]

  CBM_CACHE_DIR="$rollout_cache" CBM_RUNTIME_DIR="$rollout_runtime" \
    CBM_PROFILE=1 CBM_WORKERS=4 "$binary" daemon start
  response="$rollout_root/index-response.jsonl"
  (
    cd "$fixture_repo"
    printf '%s\n' \
      '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"rollout-rehearsal","version":"1"}}}' \
      '{"jsonrpc":"2.0","method":"notifications/initialized"}'
    printf '%s%s%s\n' \
      '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"index_repository","arguments":{"repo_path":"' \
      "$fixture_repo" '","mode":"full"}}}'
  ) | CBM_CACHE_DIR="$rollout_cache" CBM_RUNTIME_DIR="$rollout_runtime" \
    CBM_PROFILE=1 CBM_WORKERS=4 "$binary" >"$response"
  index_response="$(grep -m 1 '"id":2' "$response")"
  case "$index_response" in
    *'"error"'*) echo "fixture full index failed" >&2; exit 1 ;;
    *nodes*) ;;
    *) echo "fixture full index returned no nodes" >&2; exit 1 ;;
  esac

  CBM_CACHE_DIR="$rollout_cache" CBM_RUNTIME_DIR="$rollout_runtime" \
    "$binary" daemon stop
  daemon_log="$rollout_cache/logs/cbm-daemon.log"
  attempts=300
  while [ "$attempts" -gt 0 ]; do
    if [ -f "$daemon_log" ] && grep -qE 'msg=daemon\.stop( |$)' "$daemon_log"; then
      break
    fi
    attempts=$((attempts - 1))
    sleep 0.1
  done
  if [ "$attempts" -eq 0 ]; then
    echo "timed out waiting for the isolated daemon lifecycle to close" >&2
    exit 1
  fi
  grep 'msg=daemon\.start' "$daemon_log" | grep -q 'physical_job_limit=1'
  grep 'msg=watcher\.disabled' "$daemon_log" | grep -q 'reason=config'
  if grep -q 'msg=watcher\.start' "$daemon_log"; then
    echo "watcher started despite watcher_enabled=false" >&2
    exit 1
  fi
  worker_log=""
  for candidate in "$rollout_cache"/logs/.worker-log-*; do
    [ -f "$candidate" ] || continue
    worker_log="$candidate"
    break
  done
  [ -n "$worker_log" ]
  grep 'msg=pipeline\.mode' "$worker_log" \
    | grep 'mode=parallel' | grep -q 'workers=4'
  printf '%s\n' 'PASS: persisted settings and effective rollout profile'
)
```

Running equivalent commands against the real cache or global launch
configuration is a separate external-effect gate and requires explicit
authorization.

> **No automatic stage or activation-backup cleanup.** CBM does not provide
> startup garbage collection, age-based deletion, or orphan deletion for stage
> files or activation backups. Manual cleanup requires checking artifact age,
> active CBM processes, live worker ownership, and `PRAGMA quick_check(1)` on the
> relevant database before deleting anything. A backup may be the only
> crash-recovery artifact.

### `ignore_worktrees`

Every indexed project is registered under its own absolute root path, so a linked
worktree would otherwise become a separate permanent index. Enable the policy to
keep short-lived worktrees out of the automatic paths:

```bash
codebase-memory-mcp config set ignore_worktrees true
```

With the policy enabled, connection-time and background automatic indexing skip
sessions rooted in linked worktrees, and hook guidance reports that the checkout
is intentionally unindexed instead of asking the agent to index it. An explicit
`index_repository` call on a linked worktree is refused unless that call passes
`index_worktree=true`.

The check applies only to linked worktrees: the repository's main checkout,
ordinary clones, and submodules are excluded. Detection uses git plumbing — the
linked checkout has a `.git` file whose referenced gitdir contains a `commondir`
entry — rather than treating every `.git` file as a worktree.

The default is `false`, preserving existing indexing behavior. Turn the policy
back off with:

```bash
codebase-memory-mcp config set ignore_worktrees false
```

## 3. UI Settings

The optional built-in graph UI stores its settings in:

```text
${CBM_CACHE_DIR:-~/.cache/codebase-memory-mcp}/config.json
```

Current format:

```json
{
  "ui_enabled": false,
  "ui_port": 9749
}
```

Notes:

- If a UI-enabled binary finds its verified external asset pack and no UI config file exists yet, the UI auto-enables on first run. Missing or invalid assets leave the MCP/daemon service available and keep the UI disabled.
- `CBM_CACHE_DIR` changes both the UI config location and the runtime settings database location.
- CBM resolves `CBM_CACHE_DIR` to one canonical per-account cache root. A process configured with a different root fails while any CBM session or command is active; close them before switching roots.

## 4. Environment Variables

These environment variables affect runtime behavior:

| Variable | Default | Description |
|---|---|---|
| `CBM_ALLOWED_ROOT` | *(unset)* | Confine `index_repository` to paths within this directory. When set, a `repo_path` that resolves (after symlink / `..` resolution) outside this root is refused, and the same check now applies to the graph UI's `POST /api/index` route rather than only to the MCP tool. Unset imposes no *containment* restriction — but see the always-on limits below, which apply whether or not this is set. Useful when the server may be driven by an untrusted caller, e.g. agentic or multi-tenant deployments. |
| `CBM_CACHE_DIR` | `~/.cache/codebase-memory-mcp` | Override the cache directory used for indexes, `_config.db`, and UI `config.json`. |
| `CBM_DIAGNOSTICS` | `false` | Enable periodic `snapshot.json` and retained `trajectory.ndjson` below a fresh owner-private directory in the system temp directory. The daemon records the randomized paths in the `diagnostics.start` discovery record (a single JSON line) in `${CBM_CACHE_DIR}/logs/cbm-daemon.log`; that one record is emitted even when `CBM_LOG_LEVEL` suppresses ordinary logging, so the paths always remain discoverable. |
| `CBM_DOWNLOAD_URL` | GitHub releases | Override the update download URL. |
| `CBM_LOG_LEVEL` | role-aware | Set the log level to `debug`, `info`, `warn`, `error`, or `none` (or `0`-`4`). Thin MCP/CLI/hook frontends default to `warn`; the detached daemon and supervised index workers default to `info`. Physical workers retain INFO liveness records because their private logs drive the supervisor's no-progress timeout. Frontend messages use that session's stderr; detached daemon events use `${CBM_CACHE_DIR}/logs/cbm-daemon.log`. |
| `CBM_MAX_CLIENTS` | `128` | How many client connections the daemon admits at once, counting sockets accepted but not yet through HELLO. Clamped to `[1, 512]`: a value outside that range is clamped and logged, not discarded, while text that is not a number falls back to the default. Read once when the daemon starts — see below. |
| `CBM_RUNTIME_DIR` | `%LOCALAPPDATA%` (Windows), `/private/tmp` (macOS), `/tmp` (other) | Parent directory for the daemon/CLI rendezvous directory, which CBM creates inside it as `cbm-daemon-<uid>` (`cbm-daemon-<key>` on Windows). Set it when the default ancestry cannot pass the private-directory check — see below. `CBM_CACHE_DIR` does **not** move the rendezvous. |
| `CBM_WORKERS` | auto-detected | Override parallel workers inside each physical index process. It is captured by the daemon generation and is separate from `max_index_jobs`. Governs the indexing pipeline only — it has no effect on how many clients the daemon accepts; use `CBM_MAX_CLIENTS` for that. |

### Connection capacity

One daemon serves every MCP session, hook and CLI call on the machine, and each
holds one connection for its whole life. Past the cap, a new client is not told
"full" in any way it can act on: its bootstrap sees a live daemon that will not
admit it and keeps retrying until the 30 s startup budget runs out, so a cap set
too low surfaces as *"CBM daemon is active or starting but could not accept this
client within 30000 ms"*. Raise `CBM_MAX_CLIENTS` if that appears while the
daemon is otherwise healthy.

Two things are worth knowing before raising it:

- **The daemon reads it once, at startup.** Changing the variable does not
  affect a running daemon; it has to be restarted, which drops every connection
  it is currently serving. On a busy machine, pick the moment.
- **Descriptors are raised to match.** At startup the daemon raises its own
  `RLIMIT_NOFILE` to cover the configured cap, because the soft limit a service
  manager hands it (256 under launchd) is exhausted well below 512 connections —
  and that shortage shows up first as the daemon failing to open its *own* log
  and index files, which looks nothing like "too many clients". If the limit
  cannot be raised far enough, the daemon still starts and logs
  `daemon.descriptor_limit.insufficient` with the numbers.

The daemon's log carries the rest: `daemon.runtime.capacity` at startup with the
cap in force, `daemon.runtime.connections_peak` whenever the high-water mark of
concurrent connections moves, and `daemon.runtime.capacity_rejected` for every
client actually turned away. The peak line is the one to read when deciding
whether the cap is right — sampling the live count almost always catches a
trough.

### Relocating the daemon rendezvous directory

Before it is used, the rendezvous directory and every ancestor of it are checked:
each ancestor must be owned by you or by root, must not be world-writable (unless
it is the standard root-owned sticky directory such as `/tmp`), and must carry no
allow-ACL — on Windows, no ACE granting mutation rights to another identity. The
rendezvous directory itself is then forced to owner-only (`0700`, no extended ACL
/ an owner-only DACL).

That ancestry is not always acceptable in the default location. A Windows profile
that has acquired a capability-SID ACE with `WRITE_DAC` / `WRITE_OWNER` / `DELETE`
on `%LOCALAPPDATA%` — something an installed packaged app can add — fails the walk,
and so can an unusual `/tmp` or home directory on POSIX. When that happens *every*
command fails, `config list` included, so the settings surface cannot be reached
either:

```text
codebase-memory-mcp: secure daemon endpoint could not be created
```

`CBM_RUNTIME_DIR` points the rendezvous at an ancestry you choose:

```bash
export CBM_RUNTIME_DIR="$HOME/cbm-runtime"   # any directory you own
```

```powershell
$env:CBM_RUNTIME_DIR = "D:\cbm-runtime"
```

The check is not relaxed for the directory you name: it goes through exactly the
same validation as the default, and a value that fails it is refused rather than
silently ignored. Because the rendezvous is how sessions find each other, every
process that should share one daemon must see the same value — set it in the
environment of your MCP client and your shell alike, or a CLI invocation without
it will coordinate through the default location instead.

Environment used by daemon-owned components—such as diagnostics, daemon logging, and process-wide indexing resource limits—is captured from the first daemon-backed session that starts the daemon. Later sessions join the existing process and cannot replace those values. To change them, close every daemon-backed session, update the relevant agent configurations consistently, and restart a session. `CBM_ALLOWED_ROOT` remains session-specific, a conflicting `CBM_CACHE_DIR` is rejected, and one-shot CLI commands use their own current environment without starting the daemon.


### Roots that are always refused

Independently of `CBM_ALLOWED_ROOT`, some directories are refused as an indexing
root because they are too broad or too sensitive to index as a unit:

- a filesystem root, a Windows drive root, or a UNC share root;
- a top-level system tree — `/etc`, `/var`, `/usr`, `/home`, `/Users`, and on
  Windows `C:\Windows`, `C:\Users`, `C:\ProgramData`, `C:\Program Files`;
- your home directory itself (directories *below* it are fine);
- a credential directory at any depth — `.ssh`, `.aws`, `.gnupg`, `.kube`,
  `.docker`, `.netrc`, `.git-credentials`, `.password-store`, macOS `Keychains`.

Two limits are worth stating plainly. This constrains *scope*, not
*sensitivity*: inside a root that is allowed, every file the process can read may
be indexed and later returned. And the credential list is a denylist, so it
raises the cost of a mistake rather than closing the class — a directory it does
not name is permitted.

## 5. Agent and Editor Integration Files

The `install` command can also write MCP entries and instruction blocks into agent/editor config files such as Claude Code, Codex, Gemini, VS Code, Cursor, Zed, and others.

Those target paths vary by tool and platform, so the easiest way to inspect the exact files for your machine is:

```bash
codebase-memory-mcp install --dry-run
```

That prints the specific config files the installer would modify without writing anything.
