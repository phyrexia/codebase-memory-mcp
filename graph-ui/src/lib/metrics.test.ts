import { describe, expect, it } from "vitest";
import {
  bucketToMs,
  isMetricsRow,
  rowsSince,
  seriesByHour,
  seriesByDay,
  topBy,
  totalsOf,
  type MetricsRow,
} from "./metrics";

const row = (partial: Partial<MetricsRow>): MetricsRow => ({
  bucket_hour: "2026-08-05T14",
  source: "mcp",
  name: "search_graph",
  project: "demo",
  count: 1,
  errors: 0,
  dur_us_sum: 1000,
  dur_us_max: 1000,
  ...partial,
});

/* Fixed "now": 2026-08-05 15:30 UTC. */
const NOW = Date.parse("2026-08-05T15:30:00Z");

describe("seriesByHour", () => {
  it("zero-fills the trailing window and sums buckets", () => {
    const rows = [
      row({ bucket_hour: "2026-08-05T14", count: 3 }),
      row({ bucket_hour: "2026-08-05T14", name: "trace_path", count: 2, errors: 1 }),
      row({ bucket_hour: "2026-08-05T15", count: 5 }),
      row({ bucket_hour: "2026-08-01T09", count: 99 }), // outside window
    ];
    const s = seriesByHour(rows, 3, NOW);
    expect(s.map((p) => p.label)).toEqual(["13:00", "14:00", "15:00"]);
    expect(s.map((p) => p.count)).toEqual([0, 5, 5]);
    expect(s[1].errors).toBe(1);
  });
});

describe("seriesByDay", () => {
  it("groups hours into UTC days and zero-fills", () => {
    const rows = [
      row({ bucket_hour: "2026-08-04T23", count: 4 }),
      row({ bucket_hour: "2026-08-05T00", count: 6 }),
    ];
    const s = seriesByDay(rows, 2, NOW);
    expect(s.map((p) => p.label)).toEqual(["08-04", "08-05"]);
    expect(s.map((p) => p.count)).toEqual([4, 6]);
  });
});

describe("topBy", () => {
  it("ranks by count with avg latency and folds empty keys", () => {
    const rows = [
      row({ name: "search_graph", count: 10, dur_us_sum: 20000 }),
      row({ name: "trace_path", count: 3, dur_us_sum: 3000 }),
      row({ name: "search_graph", count: 5, dur_us_sum: 10000 }),
      row({ name: "", count: 2 }),
    ];
    const top = topBy(rows, "name", 2);
    expect(top[0]).toMatchObject({ key: "search_graph", count: 15, avgMs: 2 });
    expect(top[1].key).toBe("trace_path");
    expect(topBy(rows, "name", 10).some((t) => t.key === "—")).toBe(true);
  });
});

describe("totalsOf", () => {
  it("sums counts and finds the peak point", () => {
    const rows = [
      row({ bucket_hour: "2026-08-05T14", count: 3, errors: 1, dur_us_sum: 3000 }),
      row({ bucket_hour: "2026-08-05T15", count: 7, dur_us_sum: 7000 }),
    ];
    const series = seriesByHour(rows, 3, NOW);
    const t = totalsOf(rows, series);
    expect(t).toMatchObject({ count: 10, errors: 1, avgMs: 1 });
    expect(t.peak?.label).toBe("15:00");
  });
  it("handles empty input", () => {
    const t = totalsOf([], seriesByHour([], 2, NOW));
    expect(t).toMatchObject({ count: 0, errors: 0, avgMs: 0, peak: null });
  });
});

describe("bucketToMs", () => {
  it("parses UTC hour buckets", () => {
    expect(bucketToMs("2026-08-05T15")).toBe(Date.parse("2026-08-05T15:00:00Z"));
  });
});

describe("rowsSince", () => {
  it("keeps buckets at or after the cutoff and drops malformed ones", () => {
    const rows = [
      row({ bucket_hour: "2026-08-05T03" }),
      row({ bucket_hour: "2026-08-05T04" }),
      row({ bucket_hour: "not-a-bucket" }),
    ];
    const since = Date.parse("2026-08-05T04:00:00Z");
    expect(rowsSince(rows, since).map((r) => r.bucket_hour)).toEqual(["2026-08-05T04"]);
  });
});

describe("isMetricsRow", () => {
  it("accepts well-formed rows and rejects drifted shapes", () => {
    expect(isMetricsRow(row({}))).toBe(true);
    expect(isMetricsRow({ ...row({}), count: "3" })).toBe(false);
    expect(isMetricsRow({ ...row({}), name: null })).toBe(false);
    expect(isMetricsRow(null)).toBe(false);
    expect(isMetricsRow("x")).toBe(false);
  });
});
