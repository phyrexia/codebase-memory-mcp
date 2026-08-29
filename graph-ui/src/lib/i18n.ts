import { useEffect, useState } from "react";

export type UiLanguage = "en" | "zh";

export const messages = {
  en: {
    tabs: {
      graph: "Graph",
      projects: "Projects",
      control: "Control",
    },
    common: {
      cancel: "Cancel",
      refresh: "Refresh",
      loading: "Loading...",
      save: "Save",
      saving: "Saving...",
      delete: "Delete",
      noMatches: "No matches",
      dismiss: "Dismiss",
    },
    graph: {
      selectedLabel: "Graph",
      search: "Search...",
      clearSelection: "Clear selection",
      folders: "Folders",
    },
    projects: {
      indexedProjects: "Indexed Projects",
      noIndexedProjects: "No indexed projects",
      indexFirstRepository: "Index your first repository",
      viewGraph: "View Graph",
      nodes: "nodes",
      edges: "edges",
      deleteTitle: "Delete index",
      deleteConfirm: (name: string) => `Delete index for "${name}"?`,
      healthHealthy: "Database healthy",
      healthMissing: "Database missing",
      healthCorrupt: "Database unhealthy",
      healthChecking: "Checking...",
      indexingInProgress: "Indexing in progress",
      indexingFailed: "Indexing failed",
      lastIndexed: (when: string) => `Indexed ${when}`,
      rootMissing: "Folder missing",
    },
    index: {
      newIndex: "New Index",
      selectRepositoryFolder: "Select Repository Folder",
      instructions: "Navigate to the project root and click \"Index This Folder\".",
      repositoryPath: "Repository path",
      projectName: "Project ID (optional — permanent, cannot be renamed)",
      projectNamePlaceholder: "Derived from folder name if blank",
      projectNameHelp: "Becomes the database name and query prefix. Leave blank to derive it from the path.",
      filterFolders: "Filter folders",
      noSubdirectories: "No subdirectories",
      indexThisFolder: "Index This Folder",
      starting: "Starting...",
      browseTimeout: "Folder listing timed out after 5 seconds. Showing the last successful listing.",
      browseFailed: "Unable to browse this folder.",
      browseTruncated: "Showing a partial folder list because a browse limit was reached.",
      browseRoot: (path: string) => `Browse ${path}`,
      indexDirectory: (name: string) => `Index ${name}`,
      alreadyIndexed: "Already indexed",
      reindex: "Re-index",
      reindexConfirm: (name: string) => `"${name}" is already indexed. Re-index it?`,
      insideProject: (name: string) => `Inside ${name}`,
      containsProjects: (n: number) => `Contains ${n} indexed project${n === 1 ? "" : "s"}`,
    },
    adr: {
      title: "Architecture Decision Record",
      lastUpdated: "Last updated",
    },
    control: {
      panel: "Control Panel",
      totalCpu: "Total CPU",
      totalRam: "Total RAM",
      processes: "Processes",
      selfRam: "Self RAM",
      activeProcesses: "Active Processes",
      processLogs: "Process Logs",
      noProcesses: "No processes found",
      noLogs: "No logs yet",
      thisProcess: "THIS",
      uptime: "Uptime",
      metricsTitle: "Requests",
      requestsToday: "Requests today",
      errorsToday: "Errors today",
      peakLabel: "Peak",
      avgLatency: "Avg latency",
      windowDay: "24 h",
      windowWeek: "7 d",
      topTools: "Top tools",
      topProjects: "Top projects",
      noMetrics: "No metrics yet — traffic appears here within a minute.",
      metricsBarTitle: (label: string, count: number, errors: number) =>
        `${label}: ${count} requests, ${errors} errors`,
    },
  },
  zh: {
    tabs: {
      graph: "图谱",
      projects: "项目",
      control: "控制",
    },
    common: {
      cancel: "取消",
      refresh: "刷新",
      loading: "加载中...",
      save: "保存",
      saving: "保存中...",
      delete: "删除",
      noMatches: "无匹配结果",
      dismiss: "关闭",
    },
    graph: {
      selectedLabel: "图谱",
      search: "搜索...",
      clearSelection: "清除选择",
      folders: "目录",
    },
    projects: {
      indexedProjects: "已索引项目",
      noIndexedProjects: "暂无已索引项目",
      indexFirstRepository: "索引第一个仓库",
      viewGraph: "查看图谱",
      nodes: "节点",
      edges: "边",
      deleteTitle: "删除索引",
      deleteConfirm: (name: string) => `删除 "${name}" 的索引？`,
      healthHealthy: "数据库正常",
      healthMissing: "数据库缺失",
      healthCorrupt: "数据库异常",
      healthChecking: "检查中...",
      indexingInProgress: "正在索引",
      indexingFailed: "索引失败",
      lastIndexed: (when: string) => `索引于 ${when}`,
      rootMissing: "目录已不存在",
    },
    index: {
      newIndex: "新建索引",
      selectRepositoryFolder: "选择仓库目录",
      instructions: "导航到项目根目录，然后点击“索引此目录”。",
      repositoryPath: "仓库路径",
      projectName: "项目 ID（可选，永久且不可重命名）",
      projectNamePlaceholder: "留空则从路径派生",
      projectNameHelp: "将作为数据库名称与查询前缀；留空则从路径派生。",
      filterFolders: "筛选目录",
      noSubdirectories: "没有子目录",
      indexThisFolder: "索引此目录",
      starting: "启动中...",
      browseTimeout: "文件夹列表请求在 5 秒后超时。仍显示上次成功的列表。",
      browseFailed: "无法浏览此文件夹。",
      browseTruncated: "由于达到浏览限制，当前仅显示部分文件夹。",
      browseRoot: (path: string) => `浏览 ${path}`,
      indexDirectory: (name: string) => `索引 ${name}`,
      alreadyIndexed: "已索引",
      reindex: "重新索引",
      reindexConfirm: (name: string) => `“${name}” 已被索引。重新索引？`,
      insideProject: (name: string) => `位于 ${name} 内`,
      containsProjects: (n: number) => `包含 ${n} 个已索引项目`,
    },
    adr: {
      title: "架构决策记录",
      lastUpdated: "最后更新",
    },
    control: {
      panel: "控制面板",
      totalCpu: "总 CPU",
      totalRam: "总内存",
      processes: "进程",
      selfRam: "自身内存",
      activeProcesses: "活动进程",
      processLogs: "进程日志",
      noProcesses: "未找到进程",
      noLogs: "暂无日志",
      thisProcess: "本进程",
      uptime: "运行时间",
      metricsTitle: "请求",
      requestsToday: "今日请求",
      errorsToday: "今日错误",
      peakLabel: "峰值",
      avgLatency: "平均延迟",
      windowDay: "24 小时",
      windowWeek: "7 天",
      topTools: "热门工具",
      topProjects: "热门项目",
      noMetrics: "暂无指标 — 流量将在一分钟内显示。",
      metricsBarTitle: (label: string, count: number, errors: number) =>
        `${label}: ${count} 次请求，${errors} 次错误`,
    },
  },
} as const;

export type UiMessages = (typeof messages)[UiLanguage];

export function detectLanguage(acceptLanguage?: string | null, override?: string | null): UiLanguage {
  if (override === "zh" || override === "en") return override;
  if (!acceptLanguage) return "en";

  // Ranked by q, not by whether "zh" appears anywhere. A substring test served
  // Chinese for "en-US,en;q=0.9,zh;q=0.5", where English is clearly preferred,
  // and for "zh;q=0, en", where q=0 means Chinese is unacceptable.
  const best = acceptLanguage
    .split(",")
    .map((part) => {
      const [tag, ...params] = part.trim().split(";");
      const q = params.map((p) => /^\s*q\s*=\s*([\d.]+)\s*$/i.exec(p)).find(Boolean);
      return { tag: tag.trim().toLowerCase(), q: q ? Number(q[1]) : 1 };
    })
    .filter(({ tag, q }) => tag && Number.isFinite(q) && q > 0)
    .sort((a, b) => b.q - a.q)
    .find(({ tag }) => tag.split("-")[0] === "zh" || tag.split("-")[0] === "en");

  return best?.tag.startsWith("zh") ? "zh" : "en";
}

let cachedLanguage: UiLanguage = "en";
let languageLoaded = false;
let languageRequest: Promise<UiLanguage> | null = null;
const languageListeners = new Set<(lang: UiLanguage) => void>();

function loadUiLanguage(): Promise<UiLanguage> {
  if (languageLoaded) return Promise.resolve(cachedLanguage);
  if (languageRequest) return languageRequest;

  languageRequest = fetch("/api/ui-config")
    .then((r) => r.json())
    .then((data) => detectLanguage(null, data?.lang))
    .catch(() => detectLanguage(navigator.language))
    .then((lang) => {
      cachedLanguage = lang;
      languageLoaded = true;
      for (const listener of languageListeners) listener(lang);
      return lang;
    })
    .finally(() => {
      languageRequest = null;
    });

  return languageRequest;
}

export function useUiMessages(): UiMessages {
  const [lang, setLang] = useState<UiLanguage>(cachedLanguage);

  useEffect(() => {
    let cancelled = false;
    languageListeners.add(setLang);
    void loadUiLanguage().then((nextLang) => {
      if (!cancelled) setLang(nextLang);
    });
    return () => {
      cancelled = true;
      languageListeners.delete(setLang);
    };
  }, []);

  return messages[lang];
}
