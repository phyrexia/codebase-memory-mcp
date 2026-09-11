# Configuration Reference

This page documents the configuration files that `codebase-memory-mcp` reads or writes today.

## At a Glance

| Purpose | Path | Format | Notes |
|---|---|---|---|
| Global custom extension mapping | `$XDG_CONFIG_HOME/codebase-memory-mcp/config.json` | JSON | Falls back to `~/.config/codebase-memory-mcp/config.json` when `XDG_CONFIG_HOME` is unset. |
| Per-project custom extension mapping | `{repo_root}/.codebase-memory.json` | JSON | Overrides conflicting global `extra_extensions` entries. |
| CLI-managed runtime settings | `${CBM_CACHE_DIR:-~/.cache/codebase-memory-mcp}/_config.db` | SQLite | Written by `codebase-memory-mcp config set/reset`. Also holds the `metrics_requests` table: hourly request counters (source, tool/path, project, count, errors, duration) flushed by the daemon once a minute, retained 90 days, served at `GET /api/metrics` and shown in the UI Control tab. |
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
codebase-memory-mcp config reset auto_index
```

Current keys:

| Key | Default | Meaning |
|---|---|---|
| `auto_index` | `false` | Automatically index new projects when an MCP session starts. |
| `auto_index_limit` | `50000` | Maximum file count allowed for automatic indexing of a new project. |

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

- If the UI-enabled binary has embedded assets and no UI config file exists yet, the UI auto-enables on first run.
- `CBM_CACHE_DIR` changes both the UI config location and the runtime settings database location.
- CBM resolves `CBM_CACHE_DIR` to one canonical per-account cache root. A process configured with a different root fails while any CBM session or command is active; close them before switching roots.

## 4. Environment Variables

These environment variables affect runtime behavior:

| Variable | Default | Description |
|---|---|---|
| `CBM_ALLOWED_ROOT` | *(unset)* | Restrict `index_repository` to paths within this directory. When set, a `repo_path` that resolves (after symlink / `..` resolution) outside this root is refused; unset imposes no restriction. Useful when the server may be driven by an untrusted caller (agentic or multi-tenant deployments). |
| `CBM_CACHE_DIR` | `~/.cache/codebase-memory-mcp` | Override the cache directory used for indexes, `_config.db`, and UI `config.json`. |
| `CBM_DIAGNOSTICS` | `false` | Enable periodic `snapshot.json` and retained `trajectory.ndjson` below a fresh owner-private directory in the system temp directory. The daemon records the randomized paths in the `diagnostics.start` discovery record (a single JSON line) in `${CBM_CACHE_DIR}/logs/cbm-daemon.log`; that one record is emitted even when `CBM_LOG_LEVEL` suppresses ordinary logging, so the paths always remain discoverable. |
| `CBM_DOWNLOAD_URL` | GitHub releases | Override the update download URL. |
| `CBM_LOG_LEVEL` | `info` | Set the log level to `debug`, `info`, `warn`, `error`, or `none` (or `0`-`4`). Thin-frontend messages use that session's stderr; detached daemon events use `${CBM_CACHE_DIR}/logs/cbm-daemon.log`. |
| `CBM_MAX_CLIENTS` | `128` | How many client connections the daemon admits at once, counting sockets accepted but not yet through HELLO. Clamped to `[1, 512]`: a value outside that range is clamped and logged, not discarded, while text that is not a number falls back to the default. Read once when the daemon starts — see below. |
| `CBM_WORKERS` | auto-detected | Override the indexing worker count. Governs the indexing pipeline only — it has no effect on how many clients the daemon accepts. Use `CBM_MAX_CLIENTS` for that. |

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

Environment used by daemon-owned components—such as diagnostics, daemon logging, and process-wide indexing resource limits—is captured from the first daemon-backed session that starts the daemon. Later sessions join the existing process and cannot replace those values. To change them, close every daemon-backed session, update the relevant agent configurations consistently, and restart a session. `CBM_ALLOWED_ROOT` remains session-specific and a conflicting `CBM_CACHE_DIR` is rejected. One-shot CLI commands connect to the existing daemon — or start a temporary one when none is running, in which case that temporary daemon captures its environment from the command that spawned it. To pin daemon-owned settings deterministically, export them before `codebase-memory-mcp daemon start`.

## 5. Agent and Editor Integration Files

The `install` command can also write MCP entries and instruction blocks into agent/editor config files such as Claude Code, Codex, Gemini, VS Code, Cursor, Zed, and others.

Those target paths vary by tool and platform, so the easiest way to inspect the exact files for your machine is:

```bash
codebase-memory-mcp install --dry-run
```

That prints the specific config files the installer would modify without writing anything.
