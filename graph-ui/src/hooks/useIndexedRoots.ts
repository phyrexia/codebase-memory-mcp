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

/* Fold to forward slashes, no trailing slash, lowercase. Lowercasing matches
 * filesystem semantics on the default-case-insensitive macOS and Windows
 * volumes this UI overwhelmingly runs on; on a case-sensitive volume the
 * worst case is an extra re-index confirmation — the safer failure mode
 * than silently skipping it for a differently-typed casing. */
export function normalizePath(p: string): string {
  let s = p.replace(/\\/g, "/");
  if (s.length > 1) s = s.replace(/\/+$/, "");
  return s.toLowerCase();
}

export function buildRootsMap(roots: IndexedRoot[]): Map<string, IndexedRoot> {
  const m = new Map<string, IndexedRoot>();
  for (const r of roots) {
    if (r.root_path) m.set(normalizePath(r.root_path), r);
  }
  return m;
}

/* Pure so tests can exercise the matching without mounting the hook. */
export function matchDirAgainst(byPath: Map<string, IndexedRoot>, path: string): DirMatch {
  const p = normalizePath(path);
  if (!p) return { kind: "none" };
  const exact = byPath.get(p);
  if (exact) return { kind: "exact", root: exact };
  /* Deepest ancestor wins: with nested indexed projects the badge must name
   * the closest enclosing project, not whichever root the server's cache-dir
   * scan happened to emit first. */
  let best: IndexedRoot | null = null;
  let bestLen = -1;
  for (const [rootPath, root] of byPath) {
    if (p.startsWith(rootPath + "/") && rootPath.length > bestLen) {
      best = root;
      bestLen = rootPath.length;
    }
  }
  if (best) return { kind: "ancestor", root: best };
  let count = 0;
  for (const rootPath of byPath.keys()) {
    if (rootPath.startsWith(p + "/")) count++;
  }
  if (count > 0) return { kind: "descendant", count };
  return { kind: "none" };
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

  const byPath = useMemo(() => buildRootsMap(roots), [roots]);
  const matchDir = useCallback((path: string) => matchDirAgainst(byPath, path), [byPath]);

  return { roots, matchDir, refresh };
}
