r"""Product guard for the UI side of ``daemon start``: ``--open`` readiness and
the persisted ``ui_enabled`` switch.

The control socket becomes available before the HTTP listener does.  ``--open``
is the one synchronous user request in that flow: it must wait for the *CBM HTTP
endpoint*, not merely for the daemon process, before it reports or opens the URL.

This guard uses a real UI build and isolated daemon generations:

* a temporarily occupied port proves the command waits and then succeeds once
  the verified UI listener becomes available;
* a foreign service returning the exact formerly accepted HTML markers proves
  public page text is not treated as daemon identity;
* a second ``daemon start --open`` proves the already-active daemon path obtains
  the same generation-bound proof over a fresh authenticated IPC client;
* a bare ``daemon start`` over ``config set ui_enabled false`` proves the
  operator's switch survives it: the setting stays ``false``, no listener binds
  and ``daemon status`` names the switch that turns the UI back on;
* ``daemon start --port=N`` and ``daemon start --open`` over the same switch
  prove that asking for the UI explicitly still turns it on;
* the same three commands against a daemon that is already running with the
  UI off prove a bare start reports ``ui: disabled`` like a fresh one,
  ``--port`` says nothing about a browser, and ``--open`` fails naming
  ``config set ui_enabled true``;
* a refused UI handshake under ``--port=N`` proves the error names the port
  rather than a browser nobody asked for;
* malformed ``--port`` values (trailing text, values that wrap a C ``int``,
  out of range) prove they are refused before anything starts.

Build the fixture with test seams so browser launching is recorded rather than
performed and the two negative cases can use a short deterministic deadline:

    make -f Makefile.cbm cbm-with-ui TEST_SEAMS=1 BUILD_DIR=build/ui-open-test
    python3 tests/test_daemon_open_readiness.py build/ui-open-test/codebase-memory-mcp

``--case NAME`` (repeatable) runs only the named cases, in their usual order:
delayed-open, foreign-port, active-open, off-bare, off-port, off-open,
active-off, refused-port, bad-port.
``--keep-logs DIR`` copies every case's ``<cache>/logs`` (``cbm-daemon.log``
and the conflict/activation records) into ``DIR/<cache>/logs`` before the work
directory is removed, whether the run passed or not. The caches are, in case
order: cache-delayed, cache-occupied, cache-active, cache-off-bare,
cache-off-port, cache-off-open, cache-active-off, cache-refused, cache-bad-port.

The work directory lives under the system temp directory (``TMPDIR`` on
Linux), and the daemon's private-directory check applies to its ancestry: no
group- or world-writable ancestor other than a root-owned sticky one such as
``/tmp``.

Exit code: 0 == green, 1 == behavior regression, 2 == fixture/setup error.
"""

import argparse
import os
import re
import shutil
import socket
import stat
import subprocess
import sys
import tempfile
import threading
import time


OPEN_MARKER_ENV = "CBM_TEST_DAEMON_OPEN_MARKER"
READY_TIMEOUT_ENV = "CBM_TEST_DAEMON_UI_READY_TIMEOUT_MS"
RUNTIME_PARENT_ENV = "CBM_TEST_DAEMON_RUNTIME_PARENT"

# Every invocation of the binary hashes its whole executable image before it
# does anything else (src/main.c main_build_identity -> the SHA-256 build
# fingerprint, hundreds of MB in a UI build). On a loaded Linux runner that
# alone outlasted the 2-3 s the status probes here used to allow, so every
# `daemon status` was killed before it could answer and the first case never
# saw the daemon it had just started. One-shot commands get this budget; the
# waits that are bounded on purpose keep their own short values.
CLI_TIMEOUT_S = 60
# The longest UI readiness wait the test seam accepts
# (MAIN_DAEMON_CTL_UI_READY_TIMEOUT_MS). Success paths must outlast one status
# probe of the cost above plus the daemon's bind-retry backoff (1, 2, 4, 8 s).
READY_TIMEOUT_MS = 30000


def output_text(result):
    return ((result.stdout or b"") + (result.stderr or b"")).decode("utf-8", "replace")


def pid_from(text):
    match = re.search(r"pid[: ]+(\d+)", text)
    return int(match.group(1)) if match else 0


def force_kill(pid):
    if not pid:
        return
    if os.name == "nt":
        subprocess.run(["taskkill", "/F", "/PID", str(pid)], capture_output=True, timeout=30)
    else:
        subprocess.run(["kill", "-9", str(pid)], capture_output=True, timeout=30)


def stop_daemon(binary, env, pid):
    stopped = False
    try:
        subprocess.run([binary, "daemon", "stop"], capture_output=True, timeout=CLI_TIMEOUT_S,
                       env=env)
    except (OSError, subprocess.TimeoutExpired):
        pass
    deadline = time.monotonic() + CLI_TIMEOUT_S
    while time.monotonic() < deadline:
        try:
            status = subprocess.run([binary, "daemon", "status"], capture_output=True,
                                    timeout=CLI_TIMEOUT_S, env=env)
            if status.returncode != 0 and "not running" in output_text(status):
                stopped = True
                break
        except (OSError, subprocess.TimeoutExpired):
            pass
        time.sleep(0.1)
    if not stopped:
        force_kill(pid)


def occupied_loopback_port():
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    if os.name != "nt":
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", 0))
    listener.listen(16)
    return listener, listener.getsockname()[1]


class OldMarkerResponder:
    """Foreign HTTP service that exactly matches the former acceptance rule."""

    BODY = b"<!doctype html><html><head><title>Codebase Memory</title></head></html>"

    def __init__(self):
        self.listener, self.port = occupied_loopback_port()
        self.listener.settimeout(0.1)
        self.stop_event = threading.Event()
        self.request_count = 0
        self.thread = threading.Thread(target=self._serve, name="old-marker-responder",
                                       daemon=True)
        self.thread.start()

    def _serve(self):
        response = (
            b"HTTP/1.1 200 OK\r\n"
            b"Content-Type: text/html; charset=utf-8\r\n"
            b"Cache-Control: no-cache\r\n"
            b"Content-Length: " + str(len(self.BODY)).encode("ascii") + b"\r\n"
            b"Connection: close\r\n\r\n" + self.BODY
        )
        while not self.stop_event.is_set():
            try:
                connection, _ = self.listener.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            try:
                connection.settimeout(0.5)
                self.request_count += 1
                try:
                    connection.recv(4096)
                except (OSError, socket.timeout):
                    pass
                connection.sendall(response)
            except OSError:
                pass
            finally:
                connection.close()

    def close(self):
        self.stop_event.set()
        self.listener.close()
        self.thread.join(timeout=2)


def fixture_environment(work, cache, marker, timeout_ms):
    env = dict(os.environ)
    env["CBM_CACHE_DIR"] = cache
    env[OPEN_MARKER_ENV] = marker
    env[READY_TIMEOUT_ENV] = str(timeout_ms)
    runtime_parent = os.path.join(work, "runtime-" + os.path.basename(cache))
    os.makedirs(runtime_parent, mode=0o700, exist_ok=True)
    env[RUNTIME_PARENT_ENV] = runtime_parent
    env["CBM_TEST_FAKE_BROWSER_MARKER"] = marker

    # Before the production fix exists, POSIX builds do not know the marker
    # seam.  A PATH-local opener lets the very same behavior test demonstrate
    # the original premature-open RED instead of failing during setup.
    if os.name != "nt":
        fake_bin = os.path.join(work, "fake-bin")
        os.makedirs(fake_bin, exist_ok=True)
        opener = "open" if sys.platform == "darwin" else "xdg-open"
        opener_path = os.path.join(fake_bin, opener)
        if not os.path.exists(opener_path):
            with open(opener_path, "w", encoding="utf-8", newline="\n") as handle:
                handle.write("#!/bin/sh\nprintf '%s' \"$1\" > \"$CBM_TEST_FAKE_BROWSER_MARKER\"\n")
            os.chmod(opener_path, stat.S_IRUSR | stat.S_IWUSR | stat.S_IXUSR)
        env["PATH"] = fake_bin + os.pathsep + env.get("PATH", "")
    return env


def launch_start(binary, work, cache, marker, port, timeout_ms):
    os.makedirs(cache, exist_ok=True)
    env = fixture_environment(work, cache, marker, timeout_ms)
    process = subprocess.Popen(
        [binary, "daemon", "start", "--port=%d" % port, "--open"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=env,
    )
    return process, env


def collect(process, timeout=CLI_TIMEOUT_S):
    stdout, stderr = process.communicate(timeout=timeout)
    result = subprocess.CompletedProcess(process.args, process.returncode, stdout, stderr)
    return result, output_text(result)


def wait_daemon_active(binary, env, process, cache, timeout=2 * CLI_TIMEOUT_S):
    deadline = time.monotonic() + timeout
    last_probe = "status probe was not attempted"
    # The daemon opens <cache>/logs/cbm-daemon.log early in its startup. Until
    # it exists a status probe can only report "not running", and at a full
    # executable hash per probe that wasted probe is what pushes the release
    # of the blocked port past the daemon's next bind retry.
    daemon_log = os.path.join(cache, "logs", "cbm-daemon.log")
    while not os.path.exists(daemon_log):
        if process.poll() is not None:
            return False
        if time.monotonic() >= deadline:
            print("diagnostic: the daemon never opened %s" % daemon_log)
            return False
        time.sleep(0.05)
    while time.monotonic() < deadline:
        if process.poll() is not None:
            return False
        try:
            status = subprocess.run([binary, "daemon", "status"], capture_output=True,
                                    timeout=CLI_TIMEOUT_S, env=env)
            last_probe = "status rc=%d: %s" % (status.returncode, output_text(status)[:600])
        except subprocess.TimeoutExpired as timeout_error:
            # The starter may still own the short bootstrap handoff. A timed
            # status probe is not evidence either way; retry within this
            # function's independent bounded deadline.
            partial = (timeout_error.stdout or b"") + (timeout_error.stderr or b"")
            last_probe = "status timed out: %s" % partial.decode("utf-8", "replace")[:600]
            continue
        if status.returncode == 0 and "daemon: active" in output_text(status):
            return True
        time.sleep(0.1)
    print("diagnostic: %s" % last_probe)
    return False


def assert_delayed_success(binary, work):
    cache = os.path.join(work, "cache-delayed")
    marker = os.path.join(work, "browser-delayed.txt")
    blocker, port = occupied_loopback_port()
    process, env = launch_start(binary, work, cache, marker, port, READY_TIMEOUT_MS)
    daemon_pid = 0
    try:
        if not wait_daemon_active(binary, env, process, cache):
            result, text = collect(process)
            daemon_pid = pid_from(text)
            print("RED: daemon control service never became active during --open:\n%s"
                  % text[:600])
            return False
        # Generic daemon readiness is explicitly not UI readiness. Once status
        # is active, leave enough time for the buggy command to report/open;
        # the fixed command remains blocked on the authenticated HTTP probe.
        time.sleep(0.4)
        if process.poll() is not None or os.path.exists(marker):
            result, text = collect(process)
            daemon_pid = pid_from(text)
            print("RED: --open returned/opened while another listener still owned the UI port:\n%s"
                  % text[:600])
            return False

        # The daemon's first bind failed without delaying its control service.
        # Releasing the port lets the background retry publish the real CBM UI.
        blocker.close()
        blocker = None
        result, text = collect(process)
        daemon_pid = pid_from(text)
        expected_url = "http://127.0.0.1:%d" % port
        marker_text = ""
        if os.path.exists(marker):
            with open(marker, "r", encoding="utf-8") as handle:
                marker_text = handle.read()
        if result.returncode != 0 or expected_url not in text or marker_text != expected_url:
            print("RED: --open did not wait through the background UI retry and then open the "
                  "verified endpoint:\n%s\nmarker=%r" % (text[:700], marker_text))
            return False
        print("PASS: --open waited for the verified UI endpoint before reporting/opening it")
        return True
    finally:
        if blocker is not None:
            blocker.close()
        if process.poll() is None:
            process.kill()
            _, text = collect(process)
            daemon_pid = daemon_pid or pid_from(text)
        stop_daemon(binary, env, daemon_pid)


def assert_bounded_foreign_port_failure(binary, work):
    cache = os.path.join(work, "cache-occupied")
    marker = os.path.join(work, "browser-occupied.txt")
    blocker = OldMarkerResponder()
    port = blocker.port
    process, env = launch_start(binary, work, cache, marker, port, 900)
    daemon_pid = 0
    try:
        result, text = collect(process)
        daemon_pid = pid_from(text)
        url = "http://127.0.0.1:%d" % port
        if (result.returncode == 0 or os.path.exists(marker) or url in text or
                "UI endpoint did not become ready" not in text or "--port" not in text):
            print("RED: --open must reject a bounded wait on a foreign/occupied endpoint "
                  "without reporting or opening its URL:\n%s" % text[:700])
            return False
        request_count = blocker.request_count
        status = subprocess.run([binary, "daemon", "status"], capture_output=True,
                                timeout=CLI_TIMEOUT_S, env=env)
        status_text = output_text(status)
        if (status.returncode != 0 or "daemon: active" not in status_text or
                blocker.request_count != request_count):
            print("RED: daemon status must remain a bounded control-plane probe and must not "
                  "contact a configured foreign HTTP listener:\n%s\nrequests=%d->%d" %
                  (status_text[:700], request_count, blocker.request_count))
            return False
        print("PASS: exact legacy HTML markers cannot spoof daemon-generation readiness")
        return True
    finally:
        blocker.close()
        if process.poll() is None:
            process.kill()
            _, text = collect(process)
            daemon_pid = daemon_pid or pid_from(text)
        stop_daemon(binary, env, daemon_pid)


def assert_active_daemon_open(binary, work):
    cache = os.path.join(work, "cache-active")
    marker = os.path.join(work, "browser-active.txt")
    probe, port = occupied_loopback_port()
    probe.close()
    os.makedirs(cache, exist_ok=True)
    env = fixture_environment(work, cache, marker, READY_TIMEOUT_MS)
    daemon_pid = 0
    try:
        first = subprocess.run([binary, "daemon", "start", "--port=%d" % port],
                               capture_output=True, timeout=CLI_TIMEOUT_S, env=env)
        first_text = output_text(first)
        daemon_pid = pid_from(first_text)
        if first.returncode != 0 or "daemon: started" not in first_text:
            print("RED: initial asynchronous daemon start failed:\n%s" % first_text[:700])
            return False

        second = subprocess.run([binary, "daemon", "start", "--open"], capture_output=True,
                                timeout=CLI_TIMEOUT_S, env=env)
        second_text = output_text(second)
        expected_url = "http://127.0.0.1:%d" % port
        marker_text = ""
        if os.path.exists(marker):
            with open(marker, "r", encoding="utf-8") as handle:
                marker_text = handle.read()
        if (second.returncode != 0 or "daemon: already active" not in second_text or
                expected_url not in second_text or marker_text != expected_url):
            print("RED: active-daemon --open did not authenticate and open its exact "
                  "generation:\n%s\nmarker=%r" % (second_text[:700], marker_text))
            return False
        print("PASS: an already-active daemon is opened only after generation-bound proof")
        return True
    finally:
        stop_daemon(binary, env, daemon_pid)


class FixtureError(Exception):
    """The fixture could not be prepared: exit code 2, not a behavior RED."""


def run_cli(binary, env, args, timeout=CLI_TIMEOUT_S):
    return subprocess.run([binary] + list(args), capture_output=True, timeout=timeout, env=env)


def free_loopback_port():
    probe, port = occupied_loopback_port()
    probe.close()
    return port


def loopback_port_answers_within(port, seconds):
    deadline = time.monotonic() + seconds
    while True:
        probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        probe.settimeout(0.5)
        try:
            probe.connect(("127.0.0.1", port))
            return True
        except OSError:
            pass
        finally:
            probe.close()
        if time.monotonic() >= deadline:
            return False
        time.sleep(0.1)


def persisted_ui_setting(binary, env, key):
    result = run_cli(binary, env, ["config", "get", key])
    if result.returncode != 0:
        raise FixtureError("`config get %s` failed:\n%s" % (key, output_text(result)[:400]))
    return (result.stdout or b"").decode("utf-8", "replace").strip()


def disabled_ui_environment(binary, work, label, port):
    """An isolated generation whose operator switched the UI off on `port`."""
    cache = os.path.join(work, "cache-" + label)
    marker = os.path.join(work, "browser-%s.txt" % label)
    os.makedirs(cache, exist_ok=True)
    env = fixture_environment(work, cache, marker, READY_TIMEOUT_MS)
    # The port goes first: on a fresh cache a UI build auto-enables, so that
    # write alone persists ui_enabled=true. The operator's switch comes last,
    # through the supported surface (#1558), not a hand-written config.json.
    for key, value in (("ui_port", str(port)), ("ui_enabled", "false")):
        result = run_cli(binary, env, ["config", "set", key, value])
        if result.returncode != 0:
            raise FixtureError("`config set %s %s` failed:\n%s"
                               % (key, value, output_text(result)[:400]))
    if (persisted_ui_setting(binary, env, "ui_enabled") != "false" or
            persisted_ui_setting(binary, env, "ui_port") != str(port)):
        raise FixtureError("the disabled UI setting did not persist before the daemon started")
    return env, marker


def assert_bare_start_keeps_disabled_ui(binary, work):
    port = free_loopback_port()
    env, _ = disabled_ui_environment(binary, work, "off-bare", port)
    daemon_pid = 0
    try:
        start = run_cli(binary, env, ["daemon", "start"])
        text = output_text(start)
        daemon_pid = pid_from(text)
        if start.returncode != 0 or "daemon: started" not in text:
            print("RED: a bare daemon start over a disabled UI did not start the daemon:\n%s"
                  % text[:700])
            return False
        enabled = persisted_ui_setting(binary, env, "ui_enabled")
        persisted_port = persisted_ui_setting(binary, env, "ui_port")
        if enabled != "false" or persisted_port != str(port):
            print("RED: a bare `daemon start` rewrote the persisted UI setting to "
                  "ui_enabled=%s ui_port=%s (expected false/%d); `config set ui_enabled false` "
                  "must survive it:\n%s" % (enabled, persisted_port, port, text[:700]))
            return False
        if "ui: disabled" not in text or "warming" in text or "did not accept" in text:
            print("RED: a bare `daemon start` must report the UI as disabled instead of "
                  "configuring it:\n%s" % text[:700])
            return False
        # The daemon re-reads config.json about once a second; three seconds of
        # silence on the port spans several of those polls.
        if loopback_port_answers_within(port, 3.0):
            print("RED: a UI listener bound port %d although ui_enabled is false" % port)
            return False
        status = run_cli(binary, env, ["daemon", "status"])
        status_text = output_text(status)
        if (status.returncode != 0 or "ui: disabled" not in status_text or
                "config set ui_enabled true" not in status_text):
            print("RED: daemon status must report the UI as disabled and name the switch "
                  "that turns it back on:\n%s" % status_text[:700])
            return False
        print("PASS: a bare daemon start leaves a UI the operator switched off, off")
        return True
    finally:
        stop_daemon(binary, env, daemon_pid)


def assert_explicit_request_enables_disabled_ui(binary, work, label, use_open):
    persisted_port = free_loopback_port()
    if use_open:
        flags, expected_port = ["--open"], persisted_port
    else:
        requested_port = free_loopback_port()
        while requested_port == persisted_port:
            requested_port = free_loopback_port()
        flags, expected_port = ["--port=%d" % requested_port], requested_port
    env, marker = disabled_ui_environment(binary, work, label, persisted_port)
    daemon_pid = 0
    try:
        start = run_cli(binary, env, ["daemon", "start"] + flags)
        text = output_text(start)
        daemon_pid = pid_from(text)
        command = "daemon start " + " ".join(flags)
        enabled = persisted_ui_setting(binary, env, "ui_enabled")
        port_now = persisted_ui_setting(binary, env, "ui_port")
        if start.returncode != 0 or enabled != "true" or port_now != str(expected_port):
            print("RED: `%s` asks for the UI, so it must turn it on over "
                  "ui_enabled=false (got rc=%d ui_enabled=%s ui_port=%s, expected "
                  "true/%d):\n%s" % (command, start.returncode, enabled, port_now,
                                      expected_port, text[:700]))
            return False
        if not loopback_port_answers_within(expected_port, READY_TIMEOUT_MS / 1000.0):
            print("RED: `%s` turned the UI on but nothing listens on port %d:\n%s"
                  % (command, expected_port, text[:700]))
            return False
        if use_open:
            expected_url = "http://127.0.0.1:%d" % expected_port
            marker_text = ""
            if os.path.exists(marker):
                with open(marker, "r", encoding="utf-8") as handle:
                    marker_text = handle.read()
            if expected_url not in text or marker_text != expected_url:
                print("RED: `%s` over a disabled UI did not open the verified "
                      "endpoint:\n%s\nmarker=%r" % (command, text[:700], marker_text))
                return False
        print("PASS: `%s` still turns on a UI the operator had switched off" % command)
        return True
    finally:
        stop_daemon(binary, env, daemon_pid)


def assert_port_request_enables_disabled_ui(binary, work):
    return assert_explicit_request_enables_disabled_ui(binary, work, "off-port", use_open=False)


def assert_open_request_enables_disabled_ui(binary, work):
    return assert_explicit_request_enables_disabled_ui(binary, work, "off-open", use_open=True)


def assert_active_daemon_keeps_disabled_ui(binary, work):
    port = free_loopback_port()
    other_port = free_loopback_port()
    while other_port == port:
        other_port = free_loopback_port()
    env, marker = disabled_ui_environment(binary, work, "active-off", port)
    daemon_pid = 0
    try:
        first = run_cli(binary, env, ["daemon", "start"])
        first_text = output_text(first)
        daemon_pid = pid_from(first_text)
        if first.returncode != 0 or "daemon: started" not in first_text:
            print("RED: the bare start that brings the daemon up failed:\n%s" % first_text[:700])
            return False
        bare = run_cli(binary, env, ["daemon", "start"])
        bare_text = output_text(bare)
        if (bare.returncode != 0 or "daemon: already active" not in bare_text or
                "ui: disabled" not in bare_text or
                "config set ui_enabled true" not in bare_text or "error:" in bare_text):
            print("RED: a bare `daemon start` against an active daemon whose UI is off must "
                  "report `ui: disabled` like a fresh start, not an error:\n%s" % bare_text[:700])
            return False
        moved = run_cli(binary, env, ["daemon", "start", "--port=%d" % other_port])
        moved_text = output_text(moved)
        if (moved.returncode != 0 or "ui: disabled" not in moved_text or
                "browser" in moved_text):
            print("RED: `daemon start --port=N` against an active daemon whose UI is off must "
                  "report it disabled, not a browser it was never asked to open:\n%s"
                  % moved_text[:700])
            return False
        opened = run_cli(binary, env, ["daemon", "start", "--open"])
        opened_text = output_text(opened)
        if (opened.returncode == 0 or os.path.exists(marker) or
                "config set ui_enabled true" not in opened_text):
            print("RED: `daemon start --open` against an active daemon whose UI is off must "
                  "fail and name `config set ui_enabled true`:\n%s" % opened_text[:700])
            return False
        enabled = persisted_ui_setting(binary, env, "ui_enabled")
        port_now = persisted_ui_setting(binary, env, "ui_port")
        if enabled != "false" or port_now != str(port):
            print("RED: starts against an active daemon rewrote the persisted UI setting to "
                  "ui_enabled=%s ui_port=%s (expected false/%d)" % (enabled, port_now, port))
            return False
        print("PASS: an active daemon with its UI off is reported, not argued with")
        return True
    finally:
        stop_daemon(binary, env, daemon_pid)


def assert_refused_port_names_the_port(binary, work):
    port = free_loopback_port()
    cache = os.path.join(work, "cache-refused")
    marker = os.path.join(work, "browser-refused.txt")
    os.makedirs(cache, exist_ok=True)
    env = fixture_environment(work, cache, marker, READY_TIMEOUT_MS)
    # Test seam (TEST_SEAMS=1): the daemon comes up, the UI handshake is refused.
    env["CBM_TEST_DAEMON_UI_CONFIG_REFUSED"] = "1"
    try:
        start = run_cli(binary, env, ["daemon", "start", "--port=%d" % port])
        text = output_text(start)
        if (start.returncode == 0 or "did not accept the UI configuration" not in text or
                "browser" in text or ("--port=%d" % port) not in text):
            print("RED: a refused `daemon start --port=N` must fail naming the port it could "
                  "not configure, not a browser it was never asked to open:\n%s" % text[:700])
            return False
        print("PASS: a refused --port names the port, not a browser")
        return True
    finally:
        stop_daemon(binary, env, 0)


def assert_malformed_port_is_refused(binary, work):
    port = free_loopback_port()
    cache = os.path.join(work, "cache-bad-port")
    marker = os.path.join(work, "browser-bad-port.txt")
    os.makedirs(cache, exist_ok=True)
    env = fixture_environment(work, cache, marker, READY_TIMEOUT_MS)
    # atoi() read "<port>x" as <port> and wrapped 2**32 + <port> into <port>;
    # the rest are the plain range limits.
    values = ("%dx" % port, str((1 << 32) + port), "-1", "0", "65536", "")
    daemon_pid = 0
    try:
        for value in values:
            result = run_cli(binary, env, ["daemon", "start", "--port=" + value])
            text = output_text(result)
            daemon_pid = daemon_pid or pid_from(text)
            if (result.returncode == 0 or
                    "--port requires a value between 1 and 65535" not in text):
                print("RED: `daemon start --port=%s` must be refused before anything "
                      "starts:\n%s" % (value, text[:700]))
                return False
        status = run_cli(binary, env, ["daemon", "status"])
        if "not running" not in output_text(status):
            print("RED: a refused --port value still left a daemon running:\n%s"
                  % output_text(status)[:700])
            return False
        print("PASS: malformed --port values are refused before anything starts")
        return True
    finally:
        stop_daemon(binary, env, daemon_pid)


# Run order and the names --case accepts.
CASES = (
    ("delayed-open", assert_delayed_success),
    ("foreign-port", assert_bounded_foreign_port_failure),
    ("active-open", assert_active_daemon_open),
    ("off-bare", assert_bare_start_keeps_disabled_ui),
    ("off-port", assert_port_request_enables_disabled_ui),
    ("off-open", assert_open_request_enables_disabled_ui),
    ("active-off", assert_active_daemon_keeps_disabled_ui),
    ("refused-port", assert_refused_port_names_the_port),
    ("bad-port", assert_malformed_port_is_refused),
)


def run_cases(binary, work, cases):
    for name, case in cases:
        print("== case %s" % name)
        try:
            if not case(binary, work):
                return 1
        except FixtureError as error:
            print("SETUP FAIL (%s): %s" % (name, error))
            return 2
        except subprocess.TimeoutExpired as error:
            print("RED (%s): `%s` did not finish within %s s"
                  % (name, " ".join(str(arg) for arg in error.cmd[1:]), error.timeout))
            return 1
    print("\nGREEN: %s" % ", ".join(name for name, _ in cases))
    return 0


def keep_daemon_logs(work, destination):
    """Copy each case's <cache>/logs out of the work directory before it goes."""
    os.makedirs(destination, exist_ok=True)
    kept = 0
    for entry in sorted(os.listdir(work)):
        logs = os.path.join(work, entry, "logs")
        if entry.startswith("cache-") and os.path.isdir(logs):
            shutil.copytree(logs, os.path.join(destination, entry, "logs"), dirs_exist_ok=True)
            kept += 1
    print("daemon logs of %d case(s) kept in %s" % (kept, destination))


def main():
    parser = argparse.ArgumentParser(
        description="Product guard for the UI side of `daemon start` (needs a UI build "
                    "with TEST_SEAMS=1).")
    parser.add_argument("binary", help="path to the UI build of codebase-memory-mcp")
    parser.add_argument("--case", action="append", choices=[name for name, _ in CASES],
                        help="run only this case; repeatable; default: every case, in order")
    parser.add_argument("--keep-logs", metavar="DIR",
                        help="copy each case's daemon logs into DIR/<cache>/logs")
    args = parser.parse_args()
    binary = os.path.abspath(args.binary)
    if not os.path.isfile(binary):
        print("SETUP FAIL: binary not found: %s" % binary)
        return 2
    if os.name == "nt":
        with open(binary, "rb") as handle:
            if OPEN_MARKER_ENV.encode("ascii") not in handle.read():
                print("SETUP FAIL: Windows fixture must be built with TEST_SEAMS=1")
                return 2
    selected = [case for case in CASES if not args.case or case[0] in args.case]

    # macOS sockaddr_un paths are capped at 104 bytes. Anchor the isolated
    # runtime directly under /private/tmp rather than the much longer per-user
    # Darwin temporary root so the product endpoint itself remains valid.
    short_temp_root = "/private/tmp" if sys.platform == "darwin" else tempfile.gettempdir()
    with tempfile.TemporaryDirectory(prefix="cbm_uiopen_", dir=short_temp_root) as work:
        try:
            return run_cases(binary, work, selected)
        finally:
            if args.keep_logs:
                keep_daemon_logs(work, os.path.abspath(args.keep_logs))


if __name__ == "__main__":
    sys.exit(main())
