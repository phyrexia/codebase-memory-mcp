import { useCallback, useEffect, useMemo, useState } from "react";
import type { IndexedRoot } from "../lib/types";

/* Classification of a browsed folder against the set of indexed roots:
 * - exact:      the folder itself is an indexed project root
 * - ancestor:   the folder lives inside an indexed project
 * - descendant: the folder contains indexed projects further down
 * - none:       unrelated to anything indexed */
export type DirMatch =
  | { kind: "exact"; root: IndexedRoot }
  | { kind: "ancestor"; root: IndexedRoot }
  | { kind: "descendant"; count: number }
  | { kind: "none" };

/* Match on forward slashes without a trailing slash, so POSIX and Windows
 * spellings of the same folder compare equal (mirrors joinPath in StatsTab). */
function normalizePath(p: string): string {
  let s = p.replace(/\\/g, "/");
  if (s.length > 1) s = s.replace(/\/+$/, "");
  return s;
}

export function useIndexedRoots() {
  const [roots, setRoots] = useState<IndexedRoot[]>([]);

  const refresh = useCallback(async () => {
    try {
      const res = await fetch("/api/indexed-roots");
      if (!res.ok) return; /* older backend — modal degrades to no badges */
      const data = await res.json();
      if (Array.isArray(data)) setRoots(data as IndexedRoot[]);
    } catch {
      /* degrade silently: badges are an enhancement, not a gate */
    }
  }, []);

  useEffect(() => {
    void refresh();
  }, [refresh]);

  const byPath = useMemo(() => {
    const m = new Map<string, IndexedRoot>();
    for (const r of roots) {
      if (r.root_path) m.set(normalizePath(r.root_path), r);
    }
    return m;
  }, [roots]);

  const matchDir = useCallback(
    (path: string): DirMatch => {
      const p = normalizePath(path);
      if (!p) return { kind: "none" };
      const exact = byPath.get(p);
      if (exact) return { kind: "exact", root: exact };
      for (const [rootPath, root] of byPath) {
        if (p.startsWith(rootPath + "/")) return { kind: "ancestor", root };
      }
      let count = 0;
      for (const rootPath of byPath.keys()) {
        if (rootPath.startsWith(p + "/")) count++;
      }
      if (count > 0) return { kind: "descendant", count };
      return { kind: "none" };
    },
    [byPath],
  );

  return { roots, matchDir, refresh };
}
