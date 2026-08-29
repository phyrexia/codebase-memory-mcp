#!/usr/bin/env python3
"""Measure warm /api/browse latency against an isolated candidate UI binary."""
import argparse
import json
import math
import os
import socket
import subprocess
import sys
import tempfile
import time
import urllib.parse
import urllib.request


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def wait_for_server(port, deadline_s=20.0):
    deadline = time.monotonic() + deadline_s
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.25):
                return
        except OSError:
            time.sleep(0.1)
    raise RuntimeError("candidate UI server did not become ready")


def browse(port, path):
    url = "http://127.0.0.1:%d/api/browse?path=%s" % (
        port,
        urllib.parse.quote(path),
    )
    started = time.perf_counter()
    with urllib.request.urlopen(url, timeout=6.0) as response:
        payload = json.loads(response.read().decode("utf-8"))
    elapsed_ms = (time.perf_counter() - started) * 1000.0
    if not isinstance(payload.get("dirs"), list):
        raise RuntimeError("browse response has no dirs array")
    if not isinstance(payload.get("truncated"), bool):
        raise RuntimeError("browse response has no boolean truncated flag")
    return elapsed_ms


def nearest_rank_p95(samples):
    ordered = sorted(samples)
    return ordered[max(0, math.ceil(0.95 * len(ordered)) - 1)]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary")
    parser.add_argument("--path", default=os.getcwd())
    parser.add_argument("--samples", type=int, default=40)
    args = parser.parse_args()
    if args.samples < 20:
        parser.error("--samples must be at least 20")

    binary = os.path.abspath(args.binary)
    path = os.path.abspath(args.path)
    if not os.path.isfile(binary):
        raise SystemExit("binary not found: %s" % binary)
    if not os.path.isdir(path):
        raise SystemExit("browse path is not a directory: %s" % path)

    # Keep the endpoint below macOS's 104-byte sockaddr_un limit. The runtime
    # remains isolated; only its temporary parent is made explicit.
    short_temp_root = "/private/tmp" if sys.platform == "darwin" else None
    with tempfile.TemporaryDirectory(prefix="cbm_browse_", dir=short_temp_root) as work:
        port = free_port()
        env = dict(os.environ)
        env["CBM_CACHE_DIR"] = os.path.join(work, "cache")
        env["CBM_RUNTIME_DIR"] = os.path.join(work, "runtime")
        os.makedirs(env["CBM_CACHE_DIR"])
        os.makedirs(env["CBM_RUNTIME_DIR"])
        proc = subprocess.Popen(
            [binary, "--ui=true", "--port=%d" % port],
            stdin=subprocess.PIPE,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            env=env,
        )
        try:
            wait_for_server(port)
            for _ in range(5):
                browse(port, path)
            samples = [browse(port, path) for _ in range(args.samples)]
            p95 = nearest_rank_p95(samples)
            result = {
                "path": path,
                "samples": len(samples),
                "median_ms": round(sorted(samples)[len(samples) // 2], 3),
                "p95_ms": round(p95, 3),
                "limit_ms": 500.0,
                "pass": p95 < 500.0,
            }
            print(json.dumps(result, sort_keys=True))
            return 0 if result["pass"] else 1
        finally:
            if proc.stdin:
                proc.stdin.close()
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait(timeout=5)


if __name__ == "__main__":
    raise SystemExit(main())
