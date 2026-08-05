import { useState, useEffect, useCallback, useMemo } from "react";
import { ScrollArea } from "@/components/ui/scroll-area";
import type { ProcessInfo } from "../lib/types";
import { useUiMessages } from "../lib/i18n";
import {
  seriesByHour,
  seriesByDay,
  topBy,
  totalsOf,
  type MetricsRow,
  type SeriesPoint,
  type TopEntry,
} from "../lib/metrics";

/* ── Gauge component ────────────────────────────────────── */

function Gauge({ label, value, max, unit, color }: {
  label: string; value: number; max: number; unit: string; color: string;
}) {
  const pct = Math.min(100, (value / max) * 100);
  return (
    <div className="flex-1 rounded-xl border border-border/30 bg-white/[0.02] p-4">
      <p className="text-[10px] text-foreground/25 uppercase tracking-widest mb-2">{label}</p>
      <p className={`text-[20px] font-semibold tabular-nums ${color}`}>
        {value.toFixed(1)}<span className="text-[11px] text-foreground/30 ml-1">{unit}</span>
      </p>
      <div className="mt-2 h-1.5 rounded-full bg-white/[0.05] overflow-hidden">
        <div
          className="h-full rounded-full transition-all duration-500"
          style={{ width: `${pct}%`, backgroundColor: pct > 80 ? "#e05252" : pct > 50 ? "#eab308" : "#1DA27E" }}
        />
      </div>
    </div>
  );
}

/* ── Process card ───────────────────────────────────────── */

function ProcessCard({ proc, selected, onSelect }: {
  proc: ProcessInfo; selected: boolean;
  onSelect: () => void;
}) {
  const t = useUiMessages();
  return (
    <button
      onClick={onSelect}
      className={`w-full text-left rounded-xl border p-4 transition-all ${
        selected
          ? "border-primary/40 bg-primary/5"
          : "border-border/30 bg-white/[0.02] hover:bg-white/[0.04]"
      }`}
    >
      <div className="flex items-start justify-between mb-2">
        <div className="flex items-center gap-2">
          <span className={`w-2 h-2 rounded-full ${proc.is_self ? "bg-primary animate-pulse" : "bg-emerald-400"}`} />
          <span className="text-[12px] font-semibold text-foreground/80">
            PID {proc.pid}
          </span>
          {proc.is_self && (
            <span className="text-[9px] px-1.5 py-0.5 rounded bg-primary/15 text-primary font-medium">{t.control.thisProcess}</span>
          )}
        </div>
      </div>

      <div className="grid grid-cols-3 gap-3 mb-2">
        <div>
          <p className="text-[9px] text-foreground/20 uppercase">CPU</p>
          <p className="text-[13px] font-semibold tabular-nums text-foreground/70">{proc.cpu.toFixed(1)}%</p>
        </div>
        <div>
          <p className="text-[9px] text-foreground/20 uppercase">RAM</p>
          <p className="text-[13px] font-semibold tabular-nums text-foreground/70">{proc.rss_mb.toFixed(0)} MB</p>
        </div>
        <div>
          <p className="text-[9px] text-foreground/20 uppercase">{t.control.uptime}</p>
          <p className="text-[13px] font-semibold tabular-nums text-foreground/70">{proc.elapsed}</p>
        </div>
      </div>

      <p className="text-[10px] text-foreground/15 font-mono truncate">{proc.command}</p>
    </button>
  );
}

/* ── Request metrics ────────────────────────────────────── */

function MetricsBars({ series, titleOf }: {
  series: SeriesPoint[];
  titleOf: (p: SeriesPoint) => string;
}) {
  const max = Math.max(1, ...series.map((p) => p.count));
  return (
    <div className="flex items-end gap-[3px] h-24">
      {series.map((p, i) => (
        <div key={i} className="flex-1 flex flex-col items-center gap-1 min-w-0" title={titleOf(p)}>
          <div className="w-full flex flex-col justify-end h-20 rounded-sm overflow-hidden bg-white/[0.03]">
            <div
              className="w-full transition-all duration-500"
              style={{
                height: `${(p.count / max) * 100}%`,
                backgroundColor: p.errors > 0 ? "#e05252aa" : "#1DA27E88",
              }}
            />
          </div>
          {/* Label every few bars to avoid clutter */}
          {(series.length <= 7 || i % 4 === 0) && (
            <span className="text-[8px] text-foreground/20 truncate">{p.label}</span>
          )}
        </div>
      ))}
    </div>
  );
}

function TopTable({ title, entries }: { title: string; entries: TopEntry[] }) {
  return (
    <div className="flex-1 rounded-xl border border-border/30 bg-white/[0.02] p-4 min-w-0">
      <p className="text-[10px] text-foreground/25 uppercase tracking-widest mb-2">{title}</p>
      {entries.map((e) => (
        <div key={e.key} className="flex items-center justify-between gap-2 py-1 text-[11px]">
          <span className="text-foreground/60 font-mono truncate">{e.key}</span>
          <span className="shrink-0 tabular-nums text-foreground/40">
            {e.count.toLocaleString()}
            {e.errors > 0 && <span className="text-red-400/70"> · {e.errors}✕</span>}
            <span className="text-foreground/20"> · {e.avgMs.toFixed(0)}ms</span>
          </span>
        </div>
      ))}
    </div>
  );
}

function MetricsSection() {
  const t = useUiMessages();
  const [rows, setRows] = useState<MetricsRow[]>([]);
  const [available, setAvailable] = useState(true);
  const [view, setView] = useState<"24h" | "7d">("24h");

  const fetchMetrics = useCallback(async () => {
    try {
      const res = await fetch("/api/metrics?hours=168");
      if (!res.ok) {
        setAvailable(false); /* older backend — hide the section */
        return;
      }
      const data = await res.json();
      if (Array.isArray(data.buckets)) setRows(data.buckets as MetricsRow[]);
    } catch {
      /* keep last data */
    }
  }, []);

  useEffect(() => {
    fetchMetrics();
    const interval = setInterval(fetchMetrics, 10000);
    return () => clearInterval(interval);
  }, [fetchMetrics]);

  const today = new Date().toISOString().slice(0, 10);
  const todayRows = useMemo(() => rows.filter((r) => r.bucket_hour.startsWith(today)), [rows, today]);
  const totals = useMemo(() => totalsOf(todayRows, seriesByHour(todayRows, 24)), [todayRows]);
  const series = useMemo(
    () => (view === "24h" ? seriesByHour(rows, 24) : seriesByDay(rows, 7)),
    [rows, view],
  );
  const topTools = useMemo(() => topBy(rows, "name", 5), [rows]);
  const topProjects = useMemo(() => topBy(rows.filter((r) => r.project), "project", 5), [rows]);

  if (!available) return null;

  return (
    <div className="mb-8">
      <div className="flex items-center justify-between mb-4">
        <h3 className="text-[13px] font-medium text-foreground/50">{t.control.metricsTitle}</h3>
        <div className="flex gap-1">
          {(["24h", "7d"] as const).map((w) => (
            <button
              key={w}
              onClick={() => setView(w)}
              className={`px-2 py-1 rounded-md text-[10px] font-medium transition-all ${
                view === w ? "bg-primary/20 text-primary" : "bg-white/[0.03] text-foreground/30 hover:text-foreground/50"
              }`}
            >
              {w === "24h" ? t.control.windowDay : t.control.windowWeek}
            </button>
          ))}
        </div>
      </div>

      <div className="flex gap-4 mb-4">
        {[
          { label: t.control.requestsToday, value: totals.count.toLocaleString(), color: "text-primary" },
          { label: t.control.errorsToday, value: totals.errors.toLocaleString(), color: totals.errors > 0 ? "text-red-400/80" : "text-foreground/80" },
          { label: t.control.peakLabel, value: totals.peak ? `${totals.peak.label} · ${totals.peak.count.toLocaleString()}` : "—", color: "text-foreground/80" },
          { label: t.control.avgLatency, value: `${totals.avgMs.toFixed(0)} ms`, color: "text-foreground/80" },
        ].map((s) => (
          <div key={s.label} className="flex-1 rounded-xl border border-border/30 bg-white/[0.02] p-4">
            <p className="text-[10px] text-foreground/25 uppercase tracking-widest mb-1">{s.label}</p>
            <p className={`text-[18px] font-semibold tabular-nums ${s.color}`}>{s.value}</p>
          </div>
        ))}
      </div>

      {rows.length === 0 ? (
        <p className="text-foreground/20 text-[12px] text-center py-6">{t.control.noMetrics}</p>
      ) : (
        <>
          <div className="rounded-xl border border-border/30 bg-white/[0.02] p-4 mb-4">
            <MetricsBars
              series={series}
              titleOf={(p) => t.control.metricsBarTitle(p.label, p.count, p.errors)}
            />
          </div>
          <div className="flex gap-4">
            <TopTable title={t.control.topTools} entries={topTools} />
            <TopTable title={t.control.topProjects} entries={topProjects} />
          </div>
        </>
      )}
    </div>
  );
}

/* ── Log viewer ─────────────────────────────────────────── */

function LogViewer() {
  const t = useUiMessages();
  const [lines, setLines] = useState<string[]>([]);

  useEffect(() => {
    const poll = setInterval(async () => {
      try {
        const res = await fetch("/api/logs?lines=200");
        const data = await res.json();
        setLines(data.lines ?? []);
      } catch { /* ignore */ }
    }, 2000);
    /* Initial fetch */
    fetch("/api/logs?lines=200").then(r => r.json()).then(d => setLines(d.lines ?? [])).catch(() => {});
    return () => clearInterval(poll);
  }, []);

  return (
    <div className="rounded-xl border border-border/30 bg-black/30 overflow-hidden">
      <div className="px-4 py-2 border-b border-border/20">
        <span className="text-[11px] font-medium text-foreground/40">{t.control.processLogs}</span>
        <span className="text-[10px] text-foreground/15 ml-2">{lines.length} lines</span>
      </div>
      <ScrollArea className="h-[400px]">
        <div className="p-3 font-mono text-[10px] leading-relaxed">
          {lines.length === 0 ? (
            <p className="text-foreground/15 text-center py-8">{t.control.noLogs}</p>
          ) : (
            lines.map((line, i) => {
              const isErr = line.includes("level=error");
              const isWarn = line.includes("level=warn");
              return (
                <div
                  key={i}
                  className={`py-[1px] ${
                    isErr ? "text-red-400/70" : isWarn ? "text-yellow-400/60" : "text-foreground/30"
                  }`}
                >
                  {line}
                </div>
              );
            })
          )}
        </div>
      </ScrollArea>
    </div>
  );
}

/* ── Main Control Tab ───────────────────────────────────── */

export function ControlTab() {
  const t = useUiMessages();
  const [processes, setProcesses] = useState<ProcessInfo[]>([]);
  const [selfMetrics, setSelfMetrics] = useState({ rss_mb: 0, user_cpu: 0, sys_cpu: 0 });
  const [selectedPid, setSelectedPid] = useState<number | null>(null);

  const fetchProcesses = useCallback(async () => {
    try {
      const res = await fetch("/api/processes");
      const data = await res.json();
      setProcesses(data.processes ?? []);
      setSelfMetrics({
        rss_mb: data.self_rss_mb ?? 0,
        user_cpu: data.self_user_cpu_s ?? 0,
        sys_cpu: data.self_sys_cpu_s ?? 0,
      });
    } catch { /* ignore */ }
  }, []);

  useEffect(() => {
    fetchProcesses();
    const interval = setInterval(fetchProcesses, 3000);
    return () => clearInterval(interval);
  }, [fetchProcesses]);

  /* Aggregates */
  const totalCpu = processes.reduce((s, p) => s + p.cpu, 0);
  const totalRam = processes.reduce((s, p) => s + p.rss_mb, 0);

  return (
    <ScrollArea className="h-full">
      <div className="p-8 max-w-4xl mx-auto">
        <h2 className="text-[15px] font-semibold text-foreground/80 mb-6">{t.control.panel}</h2>

        {/* Aggregate gauges */}
        <div className="flex gap-4 mb-8">
          <Gauge label={t.control.totalCpu} value={totalCpu} max={100 * processes.length || 100} unit="%" color="text-foreground/80" />
          <Gauge label={t.control.totalRam} value={totalRam} max={4096} unit="MB" color="text-foreground/80" />
          <Gauge label={t.control.processes} value={processes.length} max={10} unit="" color="text-primary" />
          <Gauge label={t.control.selfRam} value={selfMetrics.rss_mb} max={2048} unit="MB" color="text-primary" />
        </div>

        {/* Request metrics */}
        <MetricsSection />

        {/* Process grid */}
        <div className="mb-8">
          <div className="flex items-center justify-between mb-4">
            <h3 className="text-[13px] font-medium text-foreground/50">
              {t.control.activeProcesses}
            </h3>
            <button
              onClick={fetchProcesses}
              className="text-[11px] text-primary/60 hover:text-primary transition-colors"
            >
              {t.common.refresh}
            </button>
          </div>

          {processes.length === 0 ? (
            <p className="text-foreground/20 text-[12px] text-center py-8">{t.control.noProcesses}</p>
          ) : (
            <div className="grid grid-cols-2 gap-3">
              {processes.map((p) => (
                <ProcessCard
                  key={p.pid}
                  proc={p}
                  selected={selectedPid === p.pid}
                  onSelect={() => setSelectedPid(selectedPid === p.pid ? null : p.pid)}
                />
              ))}
            </div>
          )}
        </div>

        {/* Log viewer */}
        <LogViewer />
      </div>
    </ScrollArea>
  );
}
