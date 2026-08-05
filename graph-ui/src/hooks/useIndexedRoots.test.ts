import { describe, expect, it } from "vitest";
import { buildRootsMap, matchDirAgainst, normalizePath } from "./useIndexedRoots";
import type { IndexedRoot } from "../lib/types";

const root = (name: string, root_path: string): IndexedRoot => ({
  name,
  root_path,
  nodes: 1,
  edges: 1,
  root_exists: true,
});

/* "mono" contains "pkg": deliberately nested to pin the deepest-ancestor rule. */
const byPath = buildRootsMap([
  root("mono", "/Users/dev/mono"),
  root("pkg", "/Users/dev/mono/packages/pkg"),
  root("win", "C:\\repos\\Demo"),
]);

describe("normalizePath", () => {
  it("folds separators, trailing slashes and case", () => {
    expect(normalizePath("C:\\repos\\Demo\\")).toBe("c:/repos/demo");
    expect(normalizePath("/Users/dev/Mono/")).toBe("/users/dev/mono");
    expect(normalizePath("/")).toBe("/");
  });
});

describe("matchDirAgainst", () => {
  it("matches exactly regardless of case, separators and trailing slash", () => {
    expect(matchDirAgainst(byPath, "/Users/dev/mono").kind).toBe("exact");
    expect(matchDirAgainst(byPath, "/users/DEV/Mono/").kind).toBe("exact");
    expect(matchDirAgainst(byPath, "C:/repos/demo").kind).toBe("exact");
    expect(matchDirAgainst(byPath, "C:\\repos\\demo\\").kind).toBe("exact");
  });

  it("picks the deepest enclosing project, not scan order", () => {
    const match = matchDirAgainst(byPath, "/Users/dev/mono/packages/pkg/src");
    expect(match.kind).toBe("ancestor");
    if (match.kind === "ancestor") expect(match.root.name).toBe("pkg");
  });

  it("labels folders inside a single indexed project as ancestor", () => {
    const match = matchDirAgainst(byPath, "/Users/dev/mono/src");
    expect(match.kind).toBe("ancestor");
    if (match.kind === "ancestor") expect(match.root.name).toBe("mono");
  });

  it("counts indexed projects below a folder", () => {
    expect(matchDirAgainst(byPath, "/Users/dev")).toEqual({ kind: "descendant", count: 2 });
  });

  it("returns none for unrelated, prefix-sibling and empty paths", () => {
    expect(matchDirAgainst(byPath, "/opt/elsewhere").kind).toBe("none");
    /* "/Users/dev/mono-legacy" shares a string prefix but is a sibling. */
    expect(matchDirAgainst(byPath, "/Users/dev/mono-legacy").kind).toBe("none");
    expect(matchDirAgainst(byPath, "").kind).toBe("none");
  });
});
