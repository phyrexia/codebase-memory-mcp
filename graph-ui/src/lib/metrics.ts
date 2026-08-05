/* Pure aggregation helpers for the request-metrics view. The server returns
 * raw hourly buckets; everything time-window shaped happens here so it can be
 * unit-tested without fetch or React. */

export interface MetricsRow {
  bucket_hour: string; // UTC "2026-08-05T14"
  source: string;
  name: string;
  project: string;
  count: number;
  errors: number;
  dur_us_sum: number;
  dur_us_max: number;
}

export interface SeriesPoint {
  label: string; // hour "14:00" or day "08-05"
  count: number;
  errors: number;
}

export interface TopEntry {
  key: string;
  count: number;
  errors: number;
  avgMs: number;
}

export interface MetricsTotals {
  count: number;
  errors: number;
  avgMs: number;
  peak: SeriesPoint | null;
}

const HOUR_MS = 3_600_000;

/* "2026-08-05T14" → epoch ms of that UTC hour; NaN-safe. */
export function bucketToMs(bucket: string): number {
  return Date.parse(bucket + ":00:00Z");
}

function hourKey(ms: number): string {
  return new Date(ms).toISOString().slice(0, 13);
}

/* Continuous per-hour series for the trailing window, zero-filled so the bar
 * chart shows quiet hours instead of collapsing them. `now` injectable for
 * tests. */
export function seriesByHour(rows: MetricsRow[], hours: number, now = Date.now()): SeriesPoint[] {
  const byBucket = new Map<string, { count: number; errors: number }>();
  for (const r of rows) {
    const cur = byBucket.get(r.bucket_hour) ?? { count: 0, errors: 0 };
    cur.count += r.count;
    cur.errors += r.errors;
    byBucket.set(r.bucket_hour, cur);
  }
  const out: SeriesPoint[] = [];
  const startMs = Math.floor(now / HOUR_MS) * HOUR_MS - (hours - 1) * HOUR_MS;
  for (let i = 0; i < hours; i++) {
    const ms = startMs + i * HOUR_MS;
    const key = hourKey(ms);
    const v = byBucket.get(key) ?? { count: 0, errors: 0 };
    out.push({ label: key.slice(11) + ":00", count: v.count, errors: v.errors });
  }
  return out;
}

/* Continuous per-day series (UTC days), zero-filled. */
export function seriesByDay(rows: MetricsRow[], days: number, now = Date.now()): SeriesPoint[] {
  const byDay = new Map<string, { count: number; errors: number }>();
  for (const r of rows) {
    const day = r.bucket_hour.slice(0, 10);
    const cur = byDay.get(day) ?? { count: 0, errors: 0 };
    cur.count += r.count;
    cur.errors += r.errors;
    byDay.set(day, cur);
  }
  const out: SeriesPoint[] = [];
  const DAY_MS = 24 * HOUR_MS;
  const startMs = Math.floor(now / DAY_MS) * DAY_MS - (days - 1) * DAY_MS;
  for (let i = 0; i < days; i++) {
    const day = new Date(startMs + i * DAY_MS).toISOString().slice(0, 10);
    const v = byDay.get(day) ?? { count: 0, errors: 0 };
    out.push({ label: day.slice(5), count: v.count, errors: v.errors });
  }
  return out;
}

/* Top-N by a dimension, with average latency; empty keys folded into "—". */
export function topBy(rows: MetricsRow[], dim: "name" | "project" | "source", n: number): TopEntry[] {
  const acc = new Map<string, { count: number; errors: number; dur: number }>();
  for (const r of rows) {
    const key = r[dim] || "—";
    const cur = acc.get(key) ?? { count: 0, errors: 0, dur: 0 };
    cur.count += r.count;
    cur.errors += r.errors;
    cur.dur += r.dur_us_sum;
    acc.set(key, cur);
  }
  return Array.from(acc, ([key, v]) => ({
    key,
    count: v.count,
    errors: v.errors,
    avgMs: v.count > 0 ? v.dur / v.count / 1000 : 0,
  }))
    .sort((a, b) => b.count - a.count)
    .slice(0, n);
}

/* Rows whose hourly bucket starts at or after the given epoch ms — used to
 * cut "today" at the viewer's LOCAL midnight, not the UTC calendar day. */
export function rowsSince(rows: MetricsRow[], sinceMs: number): MetricsRow[] {
  return rows.filter((r) => {
    const ms = bucketToMs(r.bucket_hour);
    return Number.isFinite(ms) && ms >= sinceMs;
  });
}

/* Minimal runtime validation at the fetch boundary so a drifted backend
 * shape degrades to dropped rows instead of NaN in the cards. */
export function isMetricsRow(v: unknown): v is MetricsRow {
  if (typeof v !== "object" || v === null) return false;
  const r = v as Record<string, unknown>;
  return (
    typeof r.bucket_hour === "string" &&
    typeof r.source === "string" &&
    typeof r.name === "string" &&
    typeof r.project === "string" &&
    typeof r.count === "number" &&
    typeof r.errors === "number" &&
    typeof r.dur_us_sum === "number" &&
    typeof r.dur_us_max === "number"
  );
}

export function totalsOf(rows: MetricsRow[], series: SeriesPoint[]): MetricsTotals {
  let count = 0;
  let errors = 0;
  let dur = 0;
  for (const r of rows) {
    count += r.count;
    errors += r.errors;
    dur += r.dur_us_sum;
  }
  let peak: SeriesPoint | null = null;
  for (const p of series) {
    if (p.count > 0 && (!peak || p.count > peak.count)) peak = p;
  }
  return { count, errors, avgMs: count > 0 ? dur / count / 1000 : 0, peak };
}
