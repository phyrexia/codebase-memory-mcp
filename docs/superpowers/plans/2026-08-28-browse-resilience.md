# Browse/UI Resilience Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make folder browsing cancelable and race-safe in React, bounded and non-link-following in the native HTTP server, and measurably responsive on macOS, POSIX, and Windows.

**Architecture:** The modal owns one monotonically increasing browse generation and one active `AbortController`; every request gets a five-second client deadline, and generation checks are the sole authority for state updates. The backend validates the requested path and every candidate with `cbm_path_info_utf8`, enumerates through `cbm_opendir`/`cbm_readdir` under result, scan, and monotonic-time budgets, then serializes the complete response with `yyjson` so every exit remains valid JSON. A pre-run test-only clock seam makes the best-effort 100 ms budget deterministic without adding sleeps.

**Tech Stack:** React 19, TypeScript, Vitest, Testing Library, C11, `compat_fs`, `compat`, yyjson, the repository native test runner, Python 3 standard library, PowerShell Windows guard runner.

**Spec:** `docs/superpowers/specs/2026-08-28-upstream-performance-integration-design.md`

## Global Constraints

- Execute this plan only after the pinned `origin/main` integration is committed, the preservation suite is green, and the integration owner releases the files listed below.
- Keep the semantic index version at `3`; do not reset or mutate any live index or cache.
- The React deadline is exactly `5_000 ms`; timeout must be visible by `5.5 s` and must retain the last successful listing.
- The backend limits are exactly `200` returned directories, `4_096` scanned entries, and a best-effort `100 ms` enumeration budget.
- A single blocking filesystem syscall remains non-cancellable; do not introduce subprocess browsing in this batch.
- Directory classification must not follow POSIX symlinks or Windows reparse points, either for a listed child or the final requested directory.
- Supersession and unmount aborts are silent. Only the latest generation may update listing, notice, error, or loading state.
- Preserve Windows drive-root behavior (`C:/`, `D:/`, drive root is its own parent), POSIX `/` behavior, the typed Windows-path debounce, and the `200` response-result compatibility cap.
- Use upstream's single-product embedded UI. Do not restore `CBM_VARIANT=ui` and do not commit `graph-ui/tsconfig.tsbuildinfo`.
- All implementation, tests, benchmarks, and builds use isolated cache/runtime directories; never point a mutation-capable command at the live cache.
- Workers do not stage or commit. The main integrator alone stages explicit paths, reviews the staged diff, and commits after task review.

## File Structure and Ownership

| File | Responsibility | Exclusive owner / ordering |
|---|---|---|
| `graph-ui/src/components/StatsTab.tsx` | Browse generation, abort controller, timeout, stale-response guard, truncation notice | Task 1 UI worker only |
| `graph-ui/src/components/StatsTab.test.tsx` | React RED/GREEN contracts for abort, timeout, unmount, stale state, loading, truncation | Task 1 UI worker only |
| `graph-ui/src/lib/i18n.ts` | English and Chinese timeout/fallback/truncation copy | Task 1 UI worker only |
| `graph-ui/src/lib/i18n.test.ts` | Catalog contract for new copy | Task 1 UI worker only |
| `src/ui/http_server.c` | Browse budgets, portable enumeration, non-following classification, yyjson response | Tasks 2 then 3; never concurrently |
| `src/ui/http_server.h` | Pre-run test-only browse clock type and setter | Task 2 backend worker only |
| `tests/test_httpd.c` | Deterministic budget/JSON tests and POSIX/Windows link classification | Tasks 2 then 3; never concurrently |
| `tests/windows/test_ui_drive_listing.py` | Released-binary Windows drive and junction contract | Task 3 Windows worker, after native backend worker releases files |
| `scripts/benchmark-browse.py` | Warm browse p95 acceptance on the candidate UI binary | Task 4 performance worker only |

Tasks 1 and 2 may run in parallel after integration ownership is released. Task 3 follows Task 2 because both touch `src/ui/http_server.c` and `tests/test_httpd.c`. Task 4 begins after Tasks 1–3 are green. The main integrator owns all cross-task reconciliation, staging, commit, and final verification.

---

### Task 1: Make React browsing cancelable, generation-safe, localized, and truncation-aware

**Files:**
- Modify: `graph-ui/src/components/StatsTab.tsx` (`CreateIndexModal` only)
- Modify: `graph-ui/src/components/StatsTab.test.tsx` (`StatsTab index modal` suite)
- Modify: `graph-ui/src/lib/i18n.ts` (`messages.en.index` and `messages.zh.index`)
- Modify: `graph-ui/src/lib/i18n.test.ts` (`i18n` suite)

**Interfaces:**
- Consumes: browser `AbortController`; `fetch(input, { signal })`; the existing `useUiMessages()` catalog.
- Produces: local `BrowseResponse`; `BROWSE_TIMEOUT_MS = 5_000`; `messages.{en,zh}.index.browseTimeout`, `.browseFailed`, and `.browseTruncated`; a `browse(path?, { silent? })` callback that guarantees only its generation can mutate modal state.

- [ ] **Step 1: Add failing i18n catalog assertions**

Append these assertions to `keeps UI chrome messages in the catalog` in `graph-ui/src/lib/i18n.test.ts`:

```ts
expect(messages.en.index.browseTimeout).toBe(
  "Folder listing timed out after 5 seconds. Showing the last successful listing.",
);
expect(messages.zh.index.browseTimeout).toBe(
  "文件夹列表请求在 5 秒后超时。仍显示上次成功的列表。",
);
expect(messages.en.index.browseFailed).toBe("Unable to browse this folder.");
expect(messages.zh.index.browseFailed).toBe("无法浏览此文件夹。");
expect(messages.en.index.browseTruncated).toBe(
  "Showing a partial folder list because a browse limit was reached.",
);
expect(messages.zh.index.browseTruncated).toBe(
  "由于达到浏览限制，当前仅显示部分文件夹。",
);
```

- [ ] **Step 2: Run the i18n RED test**

Run:

```bash
cd graph-ui
npm test -- src/lib/i18n.test.ts -t "keeps UI chrome messages in the catalog"
```

Expected: FAIL with missing `browseTimeout`, `browseFailed`, and `browseTruncated` properties.

- [ ] **Step 3: Add the exact English and Chinese messages**

Add these keys after `starting` in both `index` objects in `graph-ui/src/lib/i18n.ts`:

```ts
// en.index
browseTimeout: "Folder listing timed out after 5 seconds. Showing the last successful listing.",
browseFailed: "Unable to browse this folder.",
browseTruncated: "Showing a partial folder list because a browse limit was reached.",

// zh.index
browseTimeout: "文件夹列表请求在 5 秒后超时。仍显示上次成功的列表。",
browseFailed: "无法浏览此文件夹。",
browseTruncated: "由于达到浏览限制，当前仅显示部分文件夹。",
```

- [ ] **Step 4: Run the i18n GREEN test**

Run:

```bash
cd graph-ui
npm test -- src/lib/i18n.test.ts
```

Expected: PASS for the complete `i18n` suite.

- [ ] **Step 5: Add controlled-fetch helpers and failing browse lifecycle tests**

In `graph-ui/src/components/StatsTab.test.tsx`, add this helper above the suites:

```ts
type PendingBrowse = {
  url: string;
  signal: AbortSignal;
  resolve: (payload: Record<string, unknown>) => void;
  reject: (error: unknown) => void;
};

function deferredBrowseFetch() {
  const pending: PendingBrowse[] = [];
  const fetchMock = vi.fn((input: RequestInfo | URL, init?: RequestInit) => {
    const url = String(input);
    if (url === "/rpc") {
      return Promise.resolve(new Response(JSON.stringify({
        result: { content: [{ text: JSON.stringify({ projects: [] }) }] },
      }), { status: 200, headers: { "Content-Type": "application/json" } }));
    }
    if (url.startsWith("/api/ui-config")) {
      return Promise.resolve(new Response(JSON.stringify({ lang: "en" }), {
        status: 200,
        headers: { "Content-Type": "application/json" },
      }));
    }
    if (!url.startsWith("/api/browse")) {
      return Promise.resolve(new Response("{}", { status: 200 }));
    }
    return new Promise<Response>((resolve, reject) => {
      pending.push({
        url,
        signal: init?.signal as AbortSignal,
        resolve: (payload) => resolve(new Response(JSON.stringify(payload), {
          status: 200,
          headers: { "Content-Type": "application/json" },
        })),
        reject,
      });
    });
  });
  vi.stubGlobal("fetch", fetchMock);
  return { fetchMock, pending };
}
```

Add these exact tests to `describe("StatsTab index modal", ...)`:

```ts
it("aborts a superseded browse and ignores its late response and finally", async () => {
  const { pending } = deferredBrowseFetch();
  render(<StatsTab onSelectProject={() => {}} />);
  fireEvent.click(await screen.findByRole("button", { name: "Index your first repository" }));

  await waitFor(() => expect(pending).toHaveLength(1));
  pending[0].resolve({ path: "/good", parent: "/", dirs: ["old"], roots: ["/"], truncated: false });
  expect(await screen.findByText("old")).toBeInTheDocument();

  fireEvent.click(screen.getByRole("button", { name: "Browse old" }));
  await waitFor(() => expect(pending).toHaveLength(2));
  fireEvent.click(screen.getByRole("button", { name: "Browse /" }));
  await waitFor(() => expect(pending).toHaveLength(3));

  expect(pending[1].signal.aborted).toBe(true);
  pending[1].resolve({ path: "/stale", parent: "/", dirs: ["stale-only"], roots: ["/"], truncated: false });
  await act(async () => { await Promise.resolve(); });
  expect(screen.getByText("Loading...")).toBeInTheDocument();
  expect(screen.queryByText("stale-only")).not.toBeInTheDocument();

  pending[2].resolve({ path: "/latest", parent: "/", dirs: ["latest-only"], roots: ["/"], truncated: false });
  expect(await screen.findByText("latest-only")).toBeInTheDocument();
  expect(screen.queryByText("Loading...")).not.toBeInTheDocument();
  expect(screen.queryByText("stale-only")).not.toBeInTheDocument();
});

it("aborts at five seconds and retains the last good listing", async () => {
  const { pending } = deferredBrowseFetch();
  render(<StatsTab onSelectProject={() => {}} />);
  fireEvent.click(await screen.findByRole("button", { name: "Index your first repository" }));
  await waitFor(() => expect(pending).toHaveLength(1));
  pending[0].resolve({ path: "/good", parent: "/", dirs: ["kept"], roots: ["/"], truncated: false });
  expect(await screen.findByText("kept")).toBeInTheDocument();

  vi.useFakeTimers();
  fireEvent.click(screen.getByRole("button", { name: "Browse kept" }));
  await act(async () => { await Promise.resolve(); });
  expect(pending).toHaveLength(2);

  await act(async () => { await vi.advanceTimersByTimeAsync(4_999); });
  expect(pending[1].signal.aborted).toBe(false);
  expect(screen.queryByText(/timed out after 5 seconds/)).not.toBeInTheDocument();

  await act(async () => { await vi.advanceTimersByTimeAsync(1); });
  expect(pending[1].signal.aborted).toBe(true);
  await act(async () => {
    pending[1].reject(new DOMException("Aborted", "AbortError"));
    await Promise.resolve();
  });
  expect(screen.getByText(messages.en.index.browseTimeout)).toBeInTheDocument();
  expect(screen.getByText("kept")).toBeInTheDocument();
  expect(screen.queryByText("Loading...")).not.toBeInTheDocument();
  vi.useRealTimers();
});

it("aborts the active browse when the modal unmounts", async () => {
  const { pending } = deferredBrowseFetch();
  const view = render(<StatsTab onSelectProject={() => {}} />);
  fireEvent.click(await screen.findByRole("button", { name: "Index your first repository" }));
  await waitFor(() => expect(pending).toHaveLength(1));
  const active = pending[0];

  view.unmount();

  expect(active.signal.aborted).toBe(true);
});

it("shows a localized partial-list notice for a truncated response", async () => {
  mockProjectsFetch((url) => url.startsWith("/api/browse")
    ? new Response(JSON.stringify({
        path: "/home/dev",
        parent: "/home",
        dirs: ["alpha"],
        roots: ["/"],
        truncated: true,
      }), { status: 200, headers: { "Content-Type": "application/json" } })
    : undefined);

  render(<StatsTab onSelectProject={() => {}} />);
  fireEvent.click(await screen.findByRole("button", { name: "Index your first repository" }));

  expect(await screen.findByText(messages.en.index.browseTruncated)).toBeInTheDocument();
  expect(screen.getByText("alpha")).toBeInTheDocument();
});
```

Also make the suite cleanup unconditional for fake timers:

```ts
afterEach(() => {
  cleanup();
  vi.useRealTimers();
  vi.unstubAllGlobals();
  vi.restoreAllMocks();
});
```

- [ ] **Step 6: Run the React RED tests**

Run:

```bash
cd graph-ui
npm test -- src/components/StatsTab.test.tsx -t "aborts a superseded browse|aborts at five seconds|aborts the active browse|localized partial-list"
```

Expected: FAIL because browse calls do not pass a signal, do not abort, accept stale responses, have no five-second timeout, and do not render `truncated`.

- [ ] **Step 7: Implement the minimal generation/controller/timeout state machine**

In `CreateIndexModal`, add these types/constants above the component and state/refs inside it:

```ts
const BROWSE_TIMEOUT_MS = 5_000;

type BrowseResponse = {
  path?: string;
  parent?: string;
  dirs?: string[];
  roots?: string[];
  truncated?: boolean;
  error?: string;
};

// inside CreateIndexModal
const [browseNotice, setBrowseNotice] = useState<string | null>(null);
const browseGenerationRef = useRef(0);
const browseAbortRef = useRef<AbortController | null>(null);
const browseCopyRef = useRef({
  timeout: t.index.browseTimeout,
  failed: t.index.browseFailed,
  truncated: t.index.browseTruncated,
});
browseCopyRef.current = {
  timeout: t.index.browseTimeout,
  failed: t.index.browseFailed,
  truncated: t.index.browseTruncated,
};
```

Replace the existing `browse` callback with this generation-gated implementation:

```ts
const browse = useCallback(async (path?: string, opts?: { silent?: boolean }) => {
  const silent = opts?.silent ?? false;
  const generation = ++browseGenerationRef.current;
  browseAbortRef.current?.abort();
  const controller = new AbortController();
  browseAbortRef.current = controller;
  let timedOut = false;
  const timeoutId = window.setTimeout(() => {
    timedOut = true;
    controller.abort();
  }, BROWSE_TIMEOUT_MS);

  setLoading(!silent);
  setError(null);

  try {
    const q = path ? `?path=${encodeURIComponent(path)}` : "";
    const res = await fetch(`/api/browse${q}`, { signal: controller.signal });
    const data = await res.json() as BrowseResponse;
    if (!res.ok || data.error) throw new Error(data.error ?? browseCopyRef.current.failed);
    if (generation !== browseGenerationRef.current) return;

    lastBrowsedRef.current = data.path ?? "";
    setCurrentPath(data.path ?? "");
    setDirs([...(data.dirs ?? [])].sort());
    setRoots(data.roots ?? ["/"]);
    setParentPath(data.parent ?? "/");
    setBrowseNotice(data.truncated ? browseCopyRef.current.truncated : null);
  } catch (caught) {
    if (generation !== browseGenerationRef.current) return;
    if (controller.signal.aborted && !timedOut) return;
    if (timedOut) {
      setError(browseCopyRef.current.timeout);
    } else if (!silent) {
      setError(caught instanceof Error ? caught.message : browseCopyRef.current.failed);
    }
  } finally {
    window.clearTimeout(timeoutId);
    if (generation === browseGenerationRef.current) {
      if (browseAbortRef.current === controller) browseAbortRef.current = null;
      setLoading(false);
    }
  }
}, []);
```

Replace the initial browse effect with a cleanup that invalidates the generation before aborting:

```ts
useEffect(() => {
  void browse();
  return () => {
    browseGenerationRef.current += 1;
    browseAbortRef.current?.abort();
    browseAbortRef.current = null;
  };
}, [browse]);
```

Render the non-error partial-list notice immediately above the existing error block in the footer:

```tsx
{browseNotice && (
  <div className="rounded-lg bg-primary/10 border border-primary/20 px-3 py-2 mb-3">
    <p className="text-primary text-[11px]">{browseNotice}</p>
  </div>
)}
```

Finally, update existing browse-call assertions in `StatsTab.test.tsx` to account for the signal argument:

```ts
expect(fetchMock).toHaveBeenCalledWith(
  "/api/browse?path=C%3A%2F",
  expect.objectContaining({ signal: expect.any(AbortSignal) }),
);
```

Use the same two-argument form for the `C:/Users` and drive-root assertions. Keep `/rpc`, `/api/ui-config`, and `/api/index` assertions unchanged.

- [ ] **Step 8: Run the focused and complete React GREEN suites**

Run:

```bash
cd graph-ui
npm test -- src/components/StatsTab.test.tsx src/lib/i18n.test.ts
npm test
```

Expected: both commands PASS. The timeout test proves no abort at `4_999 ms`, abort and localized error at `5_000 ms`, retained listing, and cleared loading. The stale test proves an aborted request cannot replace state or clear the latest request's loader.

- [ ] **Step 9: Build the production UI**

Run:

```bash
cd graph-ui
npm run build
git status --short -- graph-ui
git diff --exit-code -- graph-ui/tsconfig.tsbuildinfo
```

Expected: build PASS. Only the four owned source/test files are modified; the build creates no diff in `graph-ui/tsconfig.tsbuildinfo`, and that generated file must not be staged.

- [ ] **Step 10: Hand off Task 1 evidence**

Return the focused RED output, focused/full GREEN output, production build output, and the four-file diff to the main integrator. Release ownership of all `graph-ui` files; do not stage or commit.

---

### Task 2: Return complete valid JSON under the 200/4,096/100 ms backend budgets

**Files:**
- Modify: `src/ui/http_server.c` (`struct cbm_http_server`, browse helpers, `handle_browse`, browse dispatch, constructor initialization)
- Modify: `src/ui/http_server.h` (test-only pre-run browse clock seam)
- Modify: `tests/test_httpd.c` (clock and JSON helpers, budget tests, suite registration)

**Interfaces:**
- Consumes: `cbm_clock_gettime(CLOCK_MONOTONIC, ...)`, `cbm_opendir`, `cbm_readdir`, `cbm_closedir`, yyjson, current drive-root rules.
- Produces: response shape `{ path: string, dirs: string[], parent: string, roots: string[], truncated: boolean }`; constants `BROWSE_RESULT_LIMIT=200`, `BROWSE_SCAN_LIMIT=4096`, `BROWSE_BUDGET_MS=100`; pre-run test seam `cbm_http_server_set_browse_clock_for_test`.

- [ ] **Step 1: Add the deterministic test clock and JSON response helpers**

Add to `tests/test_httpd.c` after `th_status`:

```c
typedef struct {
    const int64_t *values;
    size_t count;
    size_t index;
} th_browse_clock_t;

static int64_t th_browse_clock_now(void *context) {
    th_browse_clock_t *clock = context;
    if (!clock || clock->count == 0) {
        return 0;
    }
    size_t index = clock->index < clock->count ? clock->index++ : clock->count - 1;
    return clock->values[index];
}

static yyjson_doc *th_http_json(const char *response) {
    const char *body = response ? strstr(response, "\r\n\r\n") : NULL;
    return body ? yyjson_read(body + 4, strlen(body + 4), 0) : NULL;
}

static size_t th_json_dirs_count(yyjson_doc *doc) {
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *dirs = root ? yyjson_obj_get(root, "dirs") : NULL;
    return yyjson_is_arr(dirs) ? yyjson_arr_size(dirs) : 0;
}

static bool th_json_truncated(yyjson_doc *doc) {
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *value = root ? yyjson_obj_get(root, "truncated") : NULL;
    return yyjson_is_bool(value) && yyjson_get_bool(value);
}
```

Add `#include <yyjson/yyjson.h>` with the other test includes.

After the existing `th_server_start` definition, add a variant that installs the clock before creating the run thread:

```c
static int th_server_start_with_browse_clock(th_server_t *ts,
                                              cbm_http_browse_now_ms_fn now_ms,
                                              void *context) {
    ts->srv = cbm_http_server_new(0);
    if (!ts->srv)
        return -1;
    cbm_http_server_set_browse_clock_for_test(ts->srv, now_ms, context);
    if (th_server_thread_start(&ts->tid, ts->srv) != 0) {
        (void)cbm_http_server_free(ts->srv);
        return -1;
    }
    return 0;
}
```

- [ ] **Step 2: Add failing complete/result-cap/scan-cap/time-budget tests**

Add these exact tests before the suite definition:

```c
TEST(ui_server_browse_complete_listing_is_valid_json) {
    char *dir = th_mktempdir("cbm_browse_complete");
    ASSERT_NOT_NULL(dir);
    ASSERT_EQ(th_mkdir_p(TH_PATH(dir, "alpha")), 0);
    ASSERT_EQ(th_mkdir_p(TH_PATH(dir, "beta")), 0);

    int64_t times[] = {0};
    th_browse_clock_t clock = {.values = times, .count = 1};
    th_server_t ts;
    ASSERT_EQ(th_server_start_with_browse_clock(&ts, th_browse_clock_now, &clock), 0);

    char request[1024];
    char response[65536];
    snprintf(request, sizeof(request), "GET /api/browse?path=%s HTTP/1.1\r\n\r\n", dir);
    ASSERT_GT(th_http(cbm_http_server_port(ts.srv), request, response, sizeof(response)), 0);
    ASSERT_EQ(th_status(response), 200);
    yyjson_doc *doc = th_http_json(response);
    ASSERT_NOT_NULL(doc);
    ASSERT_EQ((int)th_json_dirs_count(doc), 2);
    ASSERT_FALSE(th_json_truncated(doc));
    yyjson_doc_free(doc);

    th_server_stop(&ts);
    ASSERT_EQ(th_rmtree(dir), 0);
    PASS();
}

TEST(ui_server_browse_result_cap_is_200_and_valid_json) {
    char *dir = th_mktempdir("cbm_browse_results");
    ASSERT_NOT_NULL(dir);
    for (int i = 0; i < 201; i++) {
        char child[512];
        snprintf(child, sizeof(child), "%s/dir-%03d", dir, i);
        ASSERT_EQ(th_mkdir_p(child), 0);
    }

    int64_t times[] = {0};
    th_browse_clock_t clock = {.values = times, .count = 1};
    th_server_t ts;
    ASSERT_EQ(th_server_start_with_browse_clock(&ts, th_browse_clock_now, &clock), 0);

    char request[1024];
    char response[262144];
    snprintf(request, sizeof(request), "GET /api/browse?path=%s HTTP/1.1\r\n\r\n", dir);
    ASSERT_GT(th_http(cbm_http_server_port(ts.srv), request, response, sizeof(response)), 0);
    yyjson_doc *doc = th_http_json(response);
    ASSERT_NOT_NULL(doc);
    ASSERT_EQ((int)th_json_dirs_count(doc), 200);
    ASSERT_TRUE(th_json_truncated(doc));
    yyjson_doc_free(doc);

    th_server_stop(&ts);
    ASSERT_EQ(th_rmtree(dir), 0);
    PASS();
}

TEST(ui_server_browse_scan_cap_is_4096_and_valid_json) {
    char *dir = th_mktempdir("cbm_browse_scan");
    ASSERT_NOT_NULL(dir);
    for (int i = 0; i < 4097; i++) {
        char child[512];
        snprintf(child, sizeof(child), "%s/.entry-%04d", dir, i);
        ASSERT_EQ(th_write_file(child, "x"), 0);
    }

    int64_t times[] = {0};
    th_browse_clock_t clock = {.values = times, .count = 1};
    th_server_t ts;
    ASSERT_EQ(th_server_start_with_browse_clock(&ts, th_browse_clock_now, &clock), 0);

    char request[1024];
    char response[65536];
    snprintf(request, sizeof(request), "GET /api/browse?path=%s HTTP/1.1\r\n\r\n", dir);
    ASSERT_GT(th_http(cbm_http_server_port(ts.srv), request, response, sizeof(response)), 0);
    yyjson_doc *doc = th_http_json(response);
    ASSERT_NOT_NULL(doc);
    ASSERT_EQ((int)th_json_dirs_count(doc), 0);
    ASSERT_TRUE(th_json_truncated(doc));
    yyjson_doc_free(doc);

    th_server_stop(&ts);
    ASSERT_EQ(th_rmtree(dir), 0);
    PASS();
}

TEST(ui_server_browse_time_budget_is_100ms_and_valid_json) {
    char *dir = th_mktempdir("cbm_browse_time");
    ASSERT_NOT_NULL(dir);
    ASSERT_EQ(th_mkdir_p(TH_PATH(dir, "would-be-listed")), 0);
    int64_t times[] = {1000, 1099, 1100};
    th_browse_clock_t clock = {.values = times, .count = 3};
    th_server_t ts;
    ASSERT_EQ(th_server_start_with_browse_clock(&ts, th_browse_clock_now, &clock), 0);

    char request[1024];
    char response[65536];
    snprintf(request, sizeof(request), "GET /api/browse?path=%s HTTP/1.1\r\n\r\n", dir);
    ASSERT_GT(th_http(cbm_http_server_port(ts.srv), request, response, sizeof(response)), 0);
    yyjson_doc *doc = th_http_json(response);
    ASSERT_NOT_NULL(doc);
    ASSERT_TRUE(th_json_truncated(doc));
    ASSERT_TRUE(th_json_dirs_count(doc) <= 1);
    yyjson_doc_free(doc);

    th_server_stop(&ts);
    ASSERT_EQ(th_rmtree(dir), 0);
    PASS();
}
```

Register the four names at the start of `SUITE(httpd)` so a crash in a boundary test is reported before unrelated route tests.

- [ ] **Step 3: Run the native RED suite**

Run:

```bash
make -j4 -f Makefile.cbm test-focused TEST_SUITES=httpd
```

Expected: compilation FAIL because `cbm_http_server_set_browse_clock_for_test` is undefined; after declaring the seam but before handler changes, assertions FAIL because responses omit `truncated` and the scan/time budgets do not exist.

- [ ] **Step 4: Add the pre-run browse clock seam**

Add this interface to `src/ui/http_server.h` after `cbm_http_server_set_recv_deadline_ms`:

```c
/* Pre-run deterministic monotonic clock seam for browse budget tests.
 * Set only before cbm_http_server_run; production uses CLOCK_MONOTONIC. */
typedef int64_t (*cbm_http_browse_now_ms_fn)(void *context);
void cbm_http_server_set_browse_clock_for_test(cbm_http_server_t *srv,
                                               cbm_http_browse_now_ms_fn now_ms,
                                               void *context);
```

Add `#include <stdint.h>` to the header. Add `browse_now_ms` and `browse_clock_context` fields to `struct cbm_http_server`, initialize the callback to the production monotonic function in `cbm_http_server_new`, and implement the setter as a null-safe pre-run assignment. The production function is:

```c
static int64_t browse_monotonic_ms(void *context) {
    (void)context;
    struct timespec now;
    if (cbm_clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return (int64_t)now.tv_sec * INT64_C(1000) + now.tv_nsec / INT64_C(1000000);
}
```

Use these exact struct, constructor, and setter additions:

```c
/* struct cbm_http_server fields */
cbm_http_browse_now_ms_fn browse_now_ms;
void *browse_clock_context;

/* cbm_http_server_new, before the server is returned */
srv->browse_now_ms = browse_monotonic_ms;
srv->browse_clock_context = NULL;

void cbm_http_server_set_browse_clock_for_test(cbm_http_server_t *srv,
                                               cbm_http_browse_now_ms_fn now_ms,
                                               void *context) {
    if (!srv || !now_ms) {
        return;
    }
    srv->browse_now_ms = now_ms;
    srv->browse_clock_context = context;
}
```

- [ ] **Step 5: Replace string-spliced browsing with portable enumeration and yyjson serialization**

Define the exact budgets near the existing HTTP constants:

```c
#define BROWSE_RESULT_LIMIT 200
#define BROWSE_SCAN_LIMIT 4096
#define BROWSE_BUDGET_MS INT64_C(100)
```

Change `append_roots_json` into this yyjson helper while preserving the existing readable-drive check:

```c
static bool browse_add_roots(yyjson_mut_doc *doc, yyjson_mut_val *root) {
    yyjson_mut_val *roots = yyjson_mut_arr(doc);
    if (!roots) return false;
#ifdef _WIN32
    DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; i++) {
        if (!(drives & (1u << i))) continue;
        char drive[4] = {(char)('A' + i), ':', '/', '\0'};
        if (!cbm_is_dir(drive)) continue;
        if (!yyjson_mut_arr_add_strcpy(doc, roots, drive)) return false;
    }
#else
    if (!yyjson_mut_arr_add_strcpy(doc, roots, "/")) return false;
#endif
    return yyjson_mut_obj_add_val(doc, root, "roots", roots);
}
```

Replace `handle_browse` with a server-aware handler whose loop has one authoritative `truncated` flag:

```c
static void handle_browse(cbm_http_server_t *srv, cbm_http_conn_t *c,
                          const cbm_http_req_t *req) {
    char path[1024] = {0};
    const char *home = cbm_get_home_dir();
    if (!cbm_http_query_param(req->query, "path", path, (int)sizeof(path)) || path[0] == '\0') {
        snprintf(path, sizeof(path), "%s", home ? home : "/");
    }
    cbm_normalize_path_sep(path);

    if (!cbm_is_dir(path)) {
        cbm_http_replyf(c, 400, g_cors_json, "{\"error\":\"not a directory\"}");
        return;
    }

    cbm_dir_t *dir = cbm_opendir(path);
    if (!dir) {
        cbm_http_replyf(c, 403, g_cors_json, "{\"error\":\"cannot open directory\"}");
        return;
    }

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = doc ? yyjson_mut_obj(doc) : NULL;
    yyjson_mut_val *dirs = doc ? yyjson_mut_arr(doc) : NULL;
    bool encoded = doc && root && dirs;
    if (encoded) {
        yyjson_mut_doc_set_root(doc, root);
        encoded = yyjson_mut_obj_add_strcpy(doc, root, "path", path) &&
                  yyjson_mut_obj_add_val(doc, root, "dirs", dirs);
    }

    size_t scanned = 0;
    size_t returned = 0;
    bool truncated = false;
    int64_t started_ms = srv->browse_now_ms(srv->browse_clock_context);

    while (encoded) {
        if (scanned >= BROWSE_SCAN_LIMIT || returned >= BROWSE_RESULT_LIMIT) {
            truncated = true;
            break;
        }
        int64_t now_ms = srv->browse_now_ms(srv->browse_clock_context);
        if (now_ms >= started_ms && now_ms - started_ms >= BROWSE_BUDGET_MS) {
            truncated = true;
            break;
        }

        cbm_dirent_t *entry = cbm_readdir(dir);
        if (!entry) break;
        scanned++;
        if (entry->name[0] == '.') continue;

        char full[2048];
        int full_len = snprintf(full, sizeof(full), "%s/%s", path, entry->name);
        if (full_len < 0 || (size_t)full_len >= sizeof(full)) {
            truncated = true;
            continue;
        }
        if (!cbm_is_dir(full)) continue;
        encoded = yyjson_mut_arr_add_strcpy(doc, dirs, entry->name);
        if (encoded) returned++;
    }
    cbm_closedir(dir);

    char parent[1024];
    snprintf(parent, sizeof(parent), "%s", path);
    char *last_slash = strrchr(parent, '/');
    size_t parent_len = strlen(parent);
    bool is_drive_root = parent_len == 3 && parent[1] == ':' && parent[2] == '/';
    if (!is_drive_root && last_slash && last_slash != parent) {
        *last_slash = '\0';
    } else if (!is_drive_root) {
        snprintf(parent, sizeof(parent), "/");
    }

    encoded = encoded && yyjson_mut_obj_add_strcpy(doc, root, "parent", parent) &&
              browse_add_roots(doc, root) &&
              yyjson_mut_obj_add_bool(doc, root, "truncated", truncated);
    size_t json_len = 0;
    char *json = encoded ? yyjson_mut_write(doc, YYJSON_WRITE_NOFLAG, &json_len) : NULL;
    if (doc) yyjson_mut_doc_free(doc);
    if (!json) {
        cbm_http_replyf(c, 500, g_cors_json, "{\"error\":\"cannot build browse response\"}");
        return;
    }
    cbm_http_reply_buf(c, 200, g_cors_json, json, json_len);
    free(json);
}
```

Change the route call from `handle_browse(c, req)` to `handle_browse(srv, c, req)`. Remove the local `<dirent.h>` include; the handler now uses only the portable repository seam.

If any yyjson append fails inside the loop, keep `encoded=false`, close the directory, free the document, and return the valid 500 JSON error shown above. Do not fall back to a partially written success body.

- [ ] **Step 6: Run the native GREEN suite**

Run:

```bash
make -j4 -f Makefile.cbm test-focused TEST_SUITES=httpd
```

Expected: PASS, including all four new test names and the existing `ui_server_browse_wide_dir_no_overflow` regression.

- [ ] **Step 7: Strengthen the existing overflow regression to parse the body**

In the child branch of `ui_server_browse_wide_dir_no_overflow`, replace the status-only `ok` calculation with yyjson validation:

```c
yyjson_doc *doc = resp ? th_http_json(resp) : NULL;
int ok = n > 0 && strstr(resp, "HTTP/1.1 200") != NULL && doc != NULL &&
         th_json_dirs_count(doc) <= 200 && th_json_truncated(doc);
if (doc) yyjson_doc_free(doc);
```

This turns the existing ASan overflow guard into a valid-JSON-at-the-result-bound contract.

- [ ] **Step 8: Re-run native GREEN and sanitizer-focused verification**

Run:

```bash
make -j4 -f Makefile.cbm test-focused TEST_SUITES=httpd
```

Expected: PASS with no ASan/UBSan report and a parseable capped response carrying `"truncated":true`; the dedicated constant-clock result-cap test proves the exact 200-result boundary without sanitizer timing flakiness.

- [ ] **Step 9: Hand off Task 2 evidence**

Return RED/GREEN command output, test names, and the three-file diff. Explicitly state that the clock setter is pre-run/test-only and that production defaults to `CLOCK_MONOTONIC`. Release `src/ui/http_server.c`, `src/ui/http_server.h`, and `tests/test_httpd.c`; do not stage or commit.

---

### Task 3: Reject and omit POSIX symlinks and Windows reparse directories

**Files:**
- Modify: `tests/test_httpd.c` (portable native link fixture and two endpoint tests)
- Modify: `src/ui/http_server.c` only if the Task 2 non-following checks do not satisfy the RED tests
- Modify: `tests/windows/test_ui_drive_listing.py` (released-binary junction test and `truncated` shape checks)

**Interfaces:**
- Consumes: `cbm_path_info_utf8`, which uses `lstat` on POSIX and `FILE_ATTRIBUTE_REPARSE_POINT` on Windows; `cmd.exe /d /c mklink /J` for an unprivileged Windows junction fixture.
- Produces: `/api/browse?path=<link-or-junction>` returns HTTP 400; a link/junction child never appears in `dirs`; every successful Windows browse includes boolean `truncated`.

- [ ] **Step 1: Add the portable native link fixture helper**

Add this helper to `tests/test_httpd.c` near the browse tests:

```c
static bool th_make_directory_alias(const char *target, const char *alias) {
#ifdef _WIN32
    char target_native[1024];
    char alias_native[1024];
    snprintf(target_native, sizeof(target_native), "%s", target);
    snprintf(alias_native, sizeof(alias_native), "%s", alias);
    for (char *p = target_native; *p; p++) if (*p == '/') *p = '\\';
    for (char *p = alias_native; *p; p++) if (*p == '/') *p = '\\';
    const char *argv[] = {"cmd.exe", "/d", "/c", "mklink", "/J",
                          alias_native, target_native, NULL};
    return cbm_exec_no_shell(argv) == 0;
#else
    return symlink(target, alias) == 0;
#endif
}

static void th_remove_directory_alias(const char *alias) {
#ifdef _WIN32
    (void)cbm_rmdir(alias);
#else
    (void)cbm_unlink(alias);
#endif
}
```

- [ ] **Step 2: Add failing endpoint tests for child omission and final-path rejection**

Add and register these exact native tests:

```c
TEST(ui_server_browse_omits_symlink_or_reparse_directory) {
    char *dir = th_mktempdir("cbm_browse_alias_child");
    ASSERT_NOT_NULL(dir);
    char real[512];
    char alias[512];
    snprintf(real, sizeof(real), "%s/real", dir);
    snprintf(alias, sizeof(alias), "%s/alias", dir);
    ASSERT_EQ(th_mkdir_p(real), 0);
    if (!th_make_directory_alias(real, alias)) {
        ASSERT_EQ(th_rmtree(dir), 0);
        SKIP_PLATFORM("directory alias fixture unavailable");
    }

    int64_t times[] = {0};
    th_browse_clock_t clock = {.values = times, .count = 1};
    th_server_t ts;
    ASSERT_EQ(th_server_start_with_browse_clock(&ts, th_browse_clock_now, &clock), 0);
    char request[1024];
    char response[65536];
    snprintf(request, sizeof(request), "GET /api/browse?path=%s HTTP/1.1\r\n\r\n", dir);
    ASSERT_GT(th_http(cbm_http_server_port(ts.srv), request, response, sizeof(response)), 0);
    yyjson_doc *doc = th_http_json(response);
    ASSERT_NOT_NULL(doc);
    yyjson_val *dirs = yyjson_obj_get(yyjson_doc_get_root(doc), "dirs");
    ASSERT_TRUE(yyjson_is_arr(dirs));
    bool saw_real = false;
    bool saw_alias = false;
    size_t idx, max;
    yyjson_val *value;
    yyjson_arr_foreach(dirs, idx, max, value) {
        const char *name = yyjson_get_str(value);
        saw_real = saw_real || (name && strcmp(name, "real") == 0);
        saw_alias = saw_alias || (name && strcmp(name, "alias") == 0);
    }
    ASSERT_TRUE(saw_real);
    ASSERT_FALSE(saw_alias);
    yyjson_doc_free(doc);

    th_server_stop(&ts);
    th_remove_directory_alias(alias);
    ASSERT_EQ(th_rmtree(dir), 0);
    PASS();
}

TEST(ui_server_browse_rejects_final_symlink_or_reparse_directory) {
    char *dir = th_mktempdir("cbm_browse_alias_final");
    ASSERT_NOT_NULL(dir);
    char real[512];
    char alias[512];
    snprintf(real, sizeof(real), "%s/real", dir);
    snprintf(alias, sizeof(alias), "%s/alias", dir);
    ASSERT_EQ(th_mkdir_p(real), 0);
    if (!th_make_directory_alias(real, alias)) {
        ASSERT_EQ(th_rmtree(dir), 0);
        SKIP_PLATFORM("directory alias fixture unavailable");
    }

    th_server_t ts;
    ASSERT_EQ(th_server_start(&ts), 0);
    char request[1024];
    char response[65536];
    snprintf(request, sizeof(request), "GET /api/browse?path=%s HTTP/1.1\r\n\r\n", alias);
    ASSERT_GT(th_http(cbm_http_server_port(ts.srv), request, response, sizeof(response)), 0);
    ASSERT_EQ(th_status(response), 400);
    yyjson_doc *doc = th_http_json(response);
    ASSERT_NOT_NULL(doc);
    yyjson_val *error = yyjson_obj_get(yyjson_doc_get_root(doc), "error");
    ASSERT_TRUE(yyjson_is_str(error));
    ASSERT_STR_EQ(yyjson_get_str(error), "not a directory");
    yyjson_doc_free(doc);

    th_server_stop(&ts);
    th_remove_directory_alias(alias);
    ASSERT_EQ(th_rmtree(dir), 0);
    PASS();
}
```

The alias is always removed before recursive cleanup so `th_rmtree` cannot traverse it.

- [ ] **Step 3: Prove RED against a following implementation, then restore the Task 2 non-following checks**

Run the tests before adding/restoring `cbm_path_info_utf8` checks in the handler:

```bash
make -j4 -f Makefile.cbm test-focused TEST_SUITES=httpd
```

Expected RED on the Task 2 handler: `ui_server_browse_omits_symlink_or_reparse_directory` sees `alias`, and `ui_server_browse_rejects_final_symlink_or_reparse_directory` receives 200 because Task 2 deliberately preserved the existing following classification until these tests existed.

- [ ] **Step 4: Verify the final and child checks use the non-following seam**

Replace the Task 2 `cbm_is_dir` final/child gates with both non-following forms:

```c
cbm_path_info_t requested;
if (cbm_path_info_utf8(path, &requested) != 0 ||
    !requested.is_directory || requested.is_symlink) {
    cbm_http_replyf(c, 400, g_cors_json, "{\"error\":\"not a directory\"}");
    return;
}

cbm_path_info_t info;
if (cbm_path_info_utf8(full, &info) != 0 ||
    !info.is_directory || info.is_symlink) {
    continue;
}
```

Also replace the Windows root readability gate inside `browse_add_roots` with the same metadata rule:

```c
cbm_path_info_t info;
if (cbm_path_info_utf8(drive, &info) != 0 || !info.is_directory || info.is_symlink) {
    continue;
}
```

Run:

```bash
make -j4 -f Makefile.cbm test-focused TEST_SUITES=httpd
```

Expected: both new native tests PASS on POSIX with a symlink and on Windows with a junction/reparse point. A platform without a constructible alias reports only the explicit fixture precondition skip.

- [ ] **Step 5: Extend the released-binary Windows guard**

In `tests/windows/test_ui_drive_listing.py`:

1. Import `urllib.error`.
2. After every successful control/drive browse, require `isinstance(payload.get("truncated"), bool)`; a missing or non-boolean flag returns exit 1.
3. Before the final GREEN print, add this junction contract:

```python
        fixture = os.path.join(work, "reparse-fixture")
        real = os.path.join(fixture, "real")
        alias = os.path.join(fixture, "alias")
        os.makedirs(real)
        made = subprocess.run(
            ["cmd.exe", "/d", "/c", "mklink", "/J", alias, real],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
        )
        if made.returncode != 0:
            print("PRECONDITION: could not create junction: %s" % made.stdout)
            return 2
        try:
            listing = browse(port, fixture.replace("\\", "/"))
            if "real" not in listing.get("dirs", []) or "alias" in listing.get("dirs", []):
                print("RED: browse advertised a junction/reparse directory: %s" % listing)
                return 1
            try:
                browse(port, alias.replace("\\", "/"))
            except urllib.error.HTTPError as ex:
                if ex.code != 400:
                    print("RED: direct junction browse returned HTTP %d, expected 400" % ex.code)
                    return 1
            else:
                print("RED: direct junction browse succeeded; reparse point was followed")
                return 1
        finally:
            if os.path.isdir(alias):
                os.rmdir(alias)
```

Do not call `shutil.rmtree` while the junction still exists.

- [ ] **Step 6: Run the Windows guard on native Windows**

Run from a native Windows checkout with the candidate UI binary:

```powershell
pwsh -File scripts/test-windows.ps1 -GuardsOnly -Binary build\c\codebase-memory-mcp.exe
```

Expected: `tests\windows\test_ui_drive_listing.py` reports GREEN; all fixed drives remain advertised and browsable; every success response has boolean `truncated`; the junction is omitted and direct junction browse returns 400. Exit 2 is acceptable only when the script prints the explicit junction-creation precondition and must block release until the test is rerun on a capable Windows host.

- [ ] **Step 7: Hand off Task 3 evidence**

Return POSIX native output, native-Windows guard output, fixture skip information if any, and the owned diffs. Release all backend/Windows test ownership; do not stage or commit.

---

### Task 4: Add and run the warm browse performance acceptance harness

**Files:**
- Create: `scripts/benchmark-browse.py`

**Interfaces:**
- Consumes: a built single-product UI binary, an optional ordinary local directory, `CBM_CACHE_DIR` isolated under a temporary directory.
- Produces: 5 warmups, 40 measured successful `/api/browse` samples, nearest-rank p95, JSON-shape validation, exit 0 only for p95 `<500 ms`.

- [ ] **Step 1: Write the failing benchmark contract first**

Create `scripts/benchmark-browse.py` with the complete harness below. Before the backend change is available, it must fail its response-shape check because `truncated` is absent.

```python
#!/usr/bin/env python3
"""Measure warm /api/browse latency against an isolated candidate UI binary."""
import argparse
import json
import math
import os
import socket
import subprocess
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

    with tempfile.TemporaryDirectory(prefix="cbm-browse-bench-") as work:
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
```

- [ ] **Step 2: Run the benchmark RED shape check on the pre-fix binary**

Run:

```bash
python3 scripts/benchmark-browse.py build/c/codebase-memory-mcp --path "$PWD"
```

Expected before Task 2 GREEN: non-zero with `browse response has no boolean truncated flag`. If the binary is already rebuilt from Task 2, use the Task 2 RED test output as the observed failure and proceed; do not rebuild old production code solely to reproduce this script failure.

- [ ] **Step 3: Build the embedded candidate and run the GREEN performance acceptance**

Run:

```bash
make -j4 -f Makefile.cbm cbm-with-ui
python3 scripts/benchmark-browse.py build/c/codebase-memory-mcp --path "$PWD"
```

Expected: build PASS; benchmark prints JSON with `"samples": 40`, `"pass": true`, and `"p95_ms"` strictly below `500.0` on an ordinary warm local directory. Record the raw sample summary and the tested directory/filesystem. A p95 equal to `500.0` fails because the acceptance criterion is `<500 ms`.

- [ ] **Step 4: Reconfirm the client deadline performance contract**

Run:

```bash
cd graph-ui
npm test -- src/components/StatsTab.test.tsx -t "aborts at five seconds"
```

Expected: PASS, proving the request remains active at `4_999 ms` and the timeout error is rendered at `5_000 ms`, within the `5.5 s` acceptance bound. The supersession test from Task 1 remains the stale-response acceptance proof.

- [ ] **Step 5: Hand off Task 4 evidence**

Return the benchmark JSON, filesystem/path used, candidate binary path, and focused timeout test output. Release `scripts/benchmark-browse.py`; do not stage or commit.

---

### Task 5: Main-integrator review, build, and release-gate verification

**Files:**
- Review only: every file listed in the ownership table
- Stage/commit: main integrator only, after all reviews are green

**Interfaces:**
- Consumes: all task RED/GREEN reports and diffs.
- Produces: one reviewed browse-resilience integration commit and verification evidence suitable for the whole-branch preservation matrix.

- [ ] **Step 1: Verify the exact changed-file set and no generated UI artifact**

Run:

```bash
git status --short
git diff --check
git diff --exit-code -- graph-ui/tsconfig.tsbuildinfo
```

Expected: no whitespace errors and no generated `tsconfig.tsbuildinfo` diff; browse-plan changes are limited to the nine implementation/test/benchmark files in the ownership table (plus unrelated already-owned integration changes that the main integrator can identify separately).

- [ ] **Step 2: Run targeted native and UI suites**

Run:

```bash
make -j4 -f Makefile.cbm test-focused TEST_SUITES=httpd
cd graph-ui && npm test && npm run build
```

Expected: all commands PASS; native output contains all eight browse test names:

```text
ui_server_browse_complete_listing_is_valid_json
ui_server_browse_result_cap_is_200_and_valid_json
ui_server_browse_scan_cap_is_4096_and_valid_json
ui_server_browse_time_budget_is_100ms_and_valid_json
ui_server_browse_wide_dir_no_overflow
ui_server_browse_traversal_probe
ui_server_browse_omits_symlink_or_reparse_directory
ui_server_browse_rejects_final_symlink_or_reparse_directory
```

- [ ] **Step 3: Build and composition-check the single-product binary**

Run from the repository root:

```bash
make -j4 -f Makefile.cbm cbm-with-ui
bash scripts/ci/check-binary-composition.sh build/c/codebase-memory-mcp
```

Expected: build and composition check PASS; the embedded UI is present and no obsolete UI variant is introduced.

- [ ] **Step 4: Run feature performance acceptance**

Run:

```bash
python3 scripts/benchmark-browse.py build/c/codebase-memory-mcp --path "$PWD"
cd graph-ui && npm test -- src/components/StatsTab.test.tsx -t "aborts at five seconds|aborts a superseded browse"
```

Expected: warm browse p95 `<500 ms`; timeout at exactly `5_000 ms` and therefore by `5.5 s`; stale response cannot mutate state or clear the active loader.

- [ ] **Step 5: Run platform and broader branch gates in the approved order**

On native Windows:

```powershell
pwsh -File scripts/test-windows.ps1 -GuardsOnly -Binary build\c\codebase-memory-mcp.exe
```

On the canonical macOS arm64 candidate, continue the parent integration plan's gates rather than inventing a browse-only substitute:

```bash
make -j4 -f Makefile.cbm test-focused TEST_SUITES="cli daemon_application daemon_frontend daemon_ipc daemon_runtime httpd mcp incremental watcher store_nodes artifact activation_transaction"
make -j4 -f Makefile.cbm test-tsan TEST_TSAN_SUITES=httpd
make -f Makefile.cbm test-lsan
make -f Makefile.cbm lint-ci
make -f Makefile.cbm security
bash scripts/security-ui.sh
bash scripts/security-vendored.sh
```

Expected: every gate PASS. Any Windows link-fixture precondition skip, sanitizer report, invalid JSON, p95 regression, timeout over `5.5 s`, or stale-state mutation blocks release.

- [ ] **Step 6: Review the final diff against every browse requirement**

Inspect:

```bash
git diff -- graph-ui/src/components/StatsTab.tsx \
  graph-ui/src/components/StatsTab.test.tsx \
  graph-ui/src/lib/i18n.ts graph-ui/src/lib/i18n.test.ts \
  src/ui/http_server.c src/ui/http_server.h tests/test_httpd.c \
  tests/windows/test_ui_drive_listing.py scripts/benchmark-browse.py
```

Confirm explicitly:

- new browse aborts predecessor;
- timeout is exactly five seconds and localized;
- unmount aborts and invalidates generation;
- only latest generation mutates all browse state, including loading;
- timeout retains last successful listing;
- `truncated` is localized and visible without replacing the listing;
- backend returns at most 200 directories and scans at most 4,096 entries;
- the 100 ms budget uses monotonic time and is explicitly best-effort;
- requested path and children use `cbm_path_info_utf8`, never following links/reparse points;
- all success bodies are yyjson-serialized and all failure bodies are valid JSON;
- POSIX symlink and Windows junction behavior are covered;
- ordinary warm p95 is strictly under 500 ms.

- [ ] **Step 7: Stage exact paths, review staged diff, and commit as the main integrator**

Run only after all task and whole-branch reviews are green:

```bash
git add graph-ui/src/components/StatsTab.tsx \
  graph-ui/src/components/StatsTab.test.tsx \
  graph-ui/src/lib/i18n.ts graph-ui/src/lib/i18n.test.ts \
  src/ui/http_server.c src/ui/http_server.h tests/test_httpd.c \
  tests/windows/test_ui_drive_listing.py scripts/benchmark-browse.py
git diff --cached --check
git diff --cached --stat
git commit -m "fix(ui): bound and cancel folder browsing"
```

Expected: only the nine intended files are staged; commit succeeds. The main integrator then includes this commit and its evidence in the final whole-branch preservation review. Do not install the candidate, mutate the live cache, change global configuration, push, open a PR, or publish a release without the separate explicit authorization required by the approved spec.

## Self-Review Checklist

- Spec coverage: React generation, predecessor abort, five-second timeout, unmount abort, silent supersession/unmount, localized timeout, retained last good listing, 200/4,096/100 ms backend limits, `truncated`, valid JSON, non-following POSIX/Windows classification, Windows drive behavior, warm p95, 5.5-second deadline, stale-state protection, UI build, native tests, Windows guard, and broader verification all map to concrete tasks above.
- Completeness scan: every implementation step carries concrete code, exact names, commands, and expected outcomes; no deferred work markers remain.
- Type consistency: `cbm_http_browse_now_ms_fn` returns `int64_t`; the setter, server fields, fake clock, and monotonic implementation use that exact type. `BrowseResponse.truncated` is optional on input for compatibility and becomes a boolean notice decision; backend success always emits a boolean.
- Ownership consistency: overlapping backend files are serialized Tasks 2 then 3; UI, Windows released-binary guard, and performance harness have disjoint owners; only the main integrator stages and commits.
- Safety: all candidate processes receive isolated `CBM_CACHE_DIR`/`CBM_RUNTIME_DIR`; alias cleanup removes the link/junction before recursive deletion; no command touches the live cache, installed binary, global config, remote, PR, or release.
